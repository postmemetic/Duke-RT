#include "nri_renderer.h"
#include "nri_cvars.h"
#include "nri_voxel_compute_allocation.h"
#include "../system/nri_renderdevice.h"
#include "perf_audit.h"
#include <algorithm>

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
    if (!PrepareNRIVoxelComputeProviderSource(mesh.sourceModel, source)) return false;
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

bool NRIRenderer::VoxelPreflightStillDenied(const NRIVoxelPreflightDenials::Request& request, uint64_t blasBytes)
{
    auto& arena = mPersistentVoxels;
    // The new BLAS and the full size of each undersized replacement are
    // unavoidable. Ignore all scratch, provider, private and pending bytes.
    uint64_t minimum = blasBytes;
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
    if (cap)
    {
        // A fresh tracked floor below a held policy ceiling is sufficient to
        // decide some cohorts even if OS usage falls or its budget rebounds.
        // Refresh the same voxel census the full transaction would consume.
        arena.MarkMaintenanceMutation();
        const uint64_t tracked = GetMemoryTelemetry().totalTrackedBytes;
        if (denied(tracked, cap))
        {
            PerfAuditAdd(PerfAuditMetric::VoxelPreflightDenialsTracked);
            return true;
        }
    }
    uint64_t usage = 0, budget = 0;
    if (!testBudget)
    {
        PerfAuditAdd(PerfAuditMetric::VoxelPreflightLocalQueries);
        if (mFrameBuffer->GetLocalMemoryUsage(usage, budget) && denied(usage, cap ? std::min(cap, budget) : budget))
        {
            PerfAuditAdd(PerfAuditMetric::VoxelPreflightDenialsLive);
            return true;
        }
    }
    // Any chance of fitting (including unavailable live telemetry) returns
    // to the full native requirements and all current budget inputs.
    PerfAuditAdd(PerfAuditMetric::VoxelPreflightDenialsRechecked);
    return false;
}
