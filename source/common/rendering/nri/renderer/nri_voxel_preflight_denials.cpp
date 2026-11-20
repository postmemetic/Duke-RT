#include "nri_renderer.h"
#include "nri_cvars.h"
#include "nri_voxel_compute_allocation.h"
#include "../system/nri_renderdevice.h"
#include "perf_audit.h"
#include "perf_audit_frame_report.h"
#include <algorithm>
#include <cstdio>

namespace
{
    struct DenialAuditFrame
    {
        uint32_t tracked = 0, live = 0, rechecked = 0, queries = 0;
        uint32_t trackedCensuses = 0, skippedCensuses = 0;
        uint64_t cpuNs = 0;
    };
    DenialAuditFrame frame;

    void FlushDenialAudit()
    {
        char fields[256];
        std::snprintf(fields, sizeof(fields), "\"tracked_denials\":%u,\"live_denials\":%u,\"full_rechecks\":%u,"
            "\"local_queries\":%u,\"tracked_censuses\":%u,\"skipped_tracked_censuses\":%u,\"proof_cpu_ns\":%llu",
            frame.tracked, frame.live, frame.rechecked, frame.queries, frame.trackedCensuses, frame.skippedCensuses,
            (unsigned long long)frame.cpuNs);
        PerfAuditWriteEvent("voxel_preflight_denial_frame", PerfAuditCurrentToken(), fields);
        frame = {};
    }

    bool ReportDenialProof(PerfAuditMetric result, bool queried, uint64_t start)
    {
        if (start)
        {
            if (!frame.tracked && !frame.live && !frame.rechecked) PerfAuditDeferFrameReport(FlushDenialAudit);
            PerfAuditAdd(result);
            if (result == PerfAuditMetric::VoxelPreflightDenialsTracked) ++frame.tracked;
            else if (result == PerfAuditMetric::VoxelPreflightDenialsLive) ++frame.live;
            else ++frame.rechecked;
            frame.queries += queried;
            frame.cpuNs += PerfAuditClockNs() - start;
        }
        return result != PerfAuditMetric::VoxelPreflightDenialsRechecked;
    }
}

bool NRIRenderer::CanReuseVoxelPreflightDenial(const PersistentVoxelMeshVariantResource& mesh,
    const NRIBufferResource& scratch, const std::array<uint32_t, 3>& rows, nri::AccelerationStructureBits flags,
    uint64_t reserve, bool growsArena, bool reclaimEnabled, NRIVoxelPreflightDenials::Request& request)
{
    auto& arena = mPersistentVoxels;
    if (!mesh.resourceKey || !mesh.sourceModel || mesh.accelerationStructure.accelerationStructure ||
        mesh.computeProviderReservation.Total() || arena.loadingWarmupActive || arena.preloadPending)
        return false;
    // Requests that can notify or change the reclaim planner need its full
    // usage, signature and retry result. PermitPreflight already ran.
    if (growsArena && reclaimEnabled && (arena.admissionReclaim.Records().count(mesh.resourceKey) ||
        arena.actorDemand.Upgrade(mesh.resourceKey, mFrameIndex).consumers)) return false;

    NRIVoxelComputeProviderSource source;
    if (!ReadNRIVoxelComputeProviderSource(mesh.sourceModel, source, false)) return false;
    request.device = mFrameBuffer->mDevice;
    request.model = mesh.sourceModel;
    request.rows = rows;
    request.cursors = {arena.arenaVertexCursor, arena.arenaIndexCursor, arena.arenaPrimitiveCursor};
    request.offsets = {mesh.vertexOffset, mesh.indexOffset, mesh.primitiveOffset};
    request.scratch = scratch.buffer;
    request.scratchBytes = scratch.size;
    request.provider = {source.pageIndex, source.slabCount, source.vertexCount, source.indexCount, source.primitiveCount,
        source.slabCapacity, source.colorRunCapacity, uint32_t(source.slabAllocated) | (uint32_t(source.colorRunsAllocated) << 1) |
        (uint32_t(source.uploaded) << 2) | (uint32_t(source.fullReadback) << 3)};
    request.backing = arena.allocationArenaRetry.Generation();
    request.retirement = arena.reclaimGeometryRevision;
    request.flags = uint32_t(flags);
    request.reserve = reserve;
    return true;
}

bool NRIRenderer::VoxelPreflightStillDenied(const NRIVoxelPreflightDenials::Request& request, NRIVoxelPreflightDenials::Proof& proof)
{
    const uint64_t start = PerfAuditActive() ? PerfAuditClockNs() : 0;
    auto& arena = mPersistentVoxels;
    // The new BLAS and the full size of each undersized replacement are
    // unavoidable. Ignore all scratch, provider, private and pending bytes.
    uint64_t minimum = proof.blasBytes;
    const auto stream = [&](uint64_t bytes, const NRIBufferResource& backing)
    {
        if (bytes > backing.size) minimum = NRIVoxelComputeSaturatingAdd(minimum, bytes);
    };
    stream(uint64_t(request.cursors[0]) * sizeof(nri_scene::SceneVertex), arena.vertexBuffer);
    stream(uint64_t(request.cursors[1]) * sizeof(uint32_t), arena.indexBuffer);
    stream(uint64_t(request.cursors[2]) * sizeof(nri_scene::PrimitiveData), arena.primitiveBuffer);

    auto& ceiling = arena.allocationBudgetCeiling;
    ceiling.SetBackoffEnabled((bool)nri_ptvoxelallocationretrybackoff);
    ceiling.BeginTick(mMapWorld.buildSerial, GetNRIVoxelAdmissionRetryClock());
    const uint64_t testBudget = uint64_t(std::max(0, (int)nri_ptvoxelbudgettestbytes));
    uint64_t cap = ceiling.ceilingBytes;
    if (testBudget) cap = cap ? std::min(cap, testBudget) : testBudget;
    const auto denied = [&](uint64_t usage, uint64_t budget)
    {
        return !BuildNRIPersistentVoxelAllocationBudget(usage, budget, minimum, request.reserve).fits;
    };
    if (cap && (testBudget || cap != proof.insufficientTrackedCeiling))
    {
        // A fresh tracked floor exceeding a held policy ceiling is sufficient to
        // decide some cohorts even if OS usage falls or its budget rebounds.
        // Refresh the same voxel census the full transaction would consume.
        arena.MarkMaintenanceMutation();
        if (start) ++frame.trackedCensuses;
        const uint64_t tracked = GetMemoryTelemetry().totalTrackedBytes;
        if (denied(tracked, cap))
        {
            return ReportDenialProof(PerfAuditMetric::VoxelPreflightDenialsTracked, false, start);
        }
        // An equivalent alias can try fresh local usage first under the same
        // ceiling. If usage fell, the full path reevaluates every owner.
        if (!testBudget) proof.insufficientTrackedCeiling = cap;
    }
    else if (cap && start) ++frame.skippedCensuses;
    uint64_t usage = 0, budget = 0;
    if (!testBudget)
    {
        PerfAuditAdd(PerfAuditMetric::VoxelPreflightLocalQueries);
        if (mFrameBuffer->GetLocalMemoryUsage(usage, budget) && denied(usage, cap ? std::min(cap, budget) : budget))
        {
            return ReportDenialProof(PerfAuditMetric::VoxelPreflightDenialsLive, true, start);
        }
    }
    // Any chance of fitting (including unavailable live telemetry) returns
    // to the full native requirements and all current budget inputs.
    return ReportDenialProof(PerfAuditMetric::VoxelPreflightDenialsRechecked, !testBudget, start);
}
