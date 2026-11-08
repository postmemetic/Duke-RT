#include "nri_renderer.h"
#include "nri_persistent_voxel_geometry_arena_policy.h"
#include "nri_persistent_voxel_services.h"
#include "nri_voxel_compute_preload.h"
#include "nri_cvars.h"
#include "../system/nri_renderdevice.h"
#include "perf_audit.h"
#include "printf.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <limits>

void NRIRenderer::PreSizePersistentVoxelGeometryArenas(
    const NRIVoxelComputePreloadStats& stats, const NRIVoxelComputePreloadSettings& settings)
{
    auto& arena = mPersistentVoxels;
    const uint64_t buildSerial = mMapWorld.buildSerial;
    if (buildSerial == 0 || arena.arenaPresizeBuildSerial == buildSerial ||
        (stats.rawSelectedUniqueGeometryBytes == 0 && stats.rawRuntimeWithheldUniqueGeometryBytes == 0))
        return;

    // This is optional capacity planning, never a migration of published data.
    // One attempt per map prevents a failed reservation from stalling every load tick.
    arena.arenaPresizeBuildSerial = buildSerial;
    const auto start = std::chrono::steady_clock::now();
    const auto plan = BuildNRIPersistentVoxelGeometryArenaPlan(stats.rawSelectedUniqueGeometryBytes,
        stats.rawRuntimeWithheldUniqueGeometryBytes, stats.rawSelectedLargestUniqueGeometryBytes);
    NRIAdapterMemoryTelemetry adapter = {};
    NRIAdapterMemoryTelemetry postAdapter = {};
    NRIPersistentVoxelGeometryArenaBudget budget = {};
    uint64_t currentUsage = 0, predictedBytes = 0, committedBytes = 0, selectedBytes = 0;
    uint32_t testedCandidates = 0;
    const auto report = [&](const char* decision)
    {
        const double elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (PerfAuditActive())
        {
            char fields[1536];
            snprintf(fields, sizeof(fields),
                "\"renderer_frame\":%u,\"map_epoch\":%llu,\"phase\":\"loading\",\"decision\":\"%s\","
                "\"selected_geometry_bytes\":%llu,\"withheld_geometry_bytes\":%llu,\"alias_reserve_bytes\":%llu,"
                "\"full_target_geometry_bytes\":%llu,\"selected_target_geometry_bytes\":%llu,\"plan_kind\":\"%s\","
                "\"candidate_count\":%u,\"budget_candidate_valid\":%s,\"predicted_allocation_bytes\":%llu,\"committed_bytes\":%llu,"
                "\"live_budget_available\":%s,\"current_usage_bytes\":%llu,\"local_budget_bytes\":%llu,"
                "\"minimum_reserve_bytes\":%llu,\"future_bytes\":%llu,\"required_headroom_bytes\":%llu,"
                "\"available_headroom_bytes\":%llu,\"post_live_budget_available\":%s,"
                "\"post_local_usage_bytes\":%llu,\"post_local_budget_bytes\":%llu,\"cpu_ms\":%.6f",
                mFrameIndex, (unsigned long long)buildSerial, decision,
                (unsigned long long)plan.plannedGeometryBytes, (unsigned long long)plan.plannedRuntimeTailBytes,
                (unsigned long long)plan.lateAliasReserveBytes, (unsigned long long)plan.targetGeometryBytes,
                (unsigned long long)selectedBytes, selectedBytes == 0 ? "none" :
                    (selectedBytes >= plan.targetGeometryBytes ? "full" : "partial"), testedCandidates,
                selectedBytes != 0 ? "true" : "false",
                (unsigned long long)predictedBytes, (unsigned long long)committedBytes,
                adapter.liveUsageAvailable ? "true" : "false", (unsigned long long)currentUsage,
                (unsigned long long)adapter.localBudgetBytes, (unsigned long long)settings.minimumLocalMemoryReserveBytes,
                (unsigned long long)budget.futureBytes, (unsigned long long)budget.requiredBytes,
                (unsigned long long)budget.availableBytes, postAdapter.liveUsageAvailable ? "true" : "false",
                (unsigned long long)postAdapter.localUsageBytes, (unsigned long long)postAdapter.localBudgetBytes, elapsedMs);
            PerfAuditWriteEvent("resident_arena_plan", PerfAuditCurrentToken(), fields);
        }
        if ((int)nri_ptloadingtrace >= 1 || (int)nri_ptvoxelcomputetrace > 0)
            Printf("NRI PT voxel arena plan: build_serial=%llu decision=%s full_target=%llu selected_target=%llu candidates=%u predicted=%llu committed=%llu usage=%llu budget=%llu required_headroom=%llu available_headroom=%llu ms=%.3f\n",
                (unsigned long long)buildSerial, decision, (unsigned long long)plan.targetGeometryBytes,
                (unsigned long long)selectedBytes, testedCandidates,
                (unsigned long long)predictedBytes, (unsigned long long)committedBytes,
                (unsigned long long)currentUsage, (unsigned long long)adapter.localBudgetBytes,
                (unsigned long long)budget.requiredBytes, (unsigned long long)budget.availableBytes, elapsedMs);
    };
    if (arena.arenaVertexCursor || arena.arenaIndexCursor || arena.arenaPrimitiveCursor ||
        arena.vertexBuffer.buffer || arena.indexBuffer.buffer || arena.primitiveBuffer.buffer)
    { report("retained-arena"); return; }
    constexpr uint64_t bytesPerPrimitive = 2ull * sizeof(nri_scene::SceneVertex) +
        3ull * sizeof(uint32_t) + sizeof(nri_scene::PrimitiveData);
    if (plan.overflow || plan.targetGeometryBytes == 0 ||
        plan.targetGeometryBytes > std::numeric_limits<uint64_t>::max() - (bytesPerPrimitive - 1))
    { report("estimate-overflow"); return; }
    const uint64_t requestedPrimitives = (plan.targetGeometryBytes + bytesPerPrimitive - 1) / bytesPerPrimitive;
    if (!mFrameBuffer || !mFrameBuffer->mDevice || !mFrameBuffer->mCore.GetBufferMemoryDesc2 ||
        !mFrameBuffer->mCore.GetDeviceDesc(*mFrameBuffer->mDevice).features.getMemoryDesc2)
    { report("memory-requirements-unavailable"); return; }
    adapter = mFrameBuffer->GetAdapterMemoryTelemetry();
    if (!adapter.liveUsageAvailable || adapter.localBudgetBytes == 0)
    { report("live-budget-unavailable"); return; }
    currentUsage = std::max(GetMemoryTelemetry().totalTrackedBytes, adapter.localUsageBytes);
    budget.availableBytes = adapter.localBudgetBytes > currentUsage ? adapter.localBudgetBytes - currentUsage : 0;

    const auto geometryUsage = NRIResourceFlags(NRIResourceFlags(nri::BufferUsageBits::SHADER_RESOURCE,
        nri::BufferUsageBits::SHADER_RESOURCE_STORAGE), nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT);
    const auto primitiveUsage = NRIResourceFlags(nri::BufferUsageBits::SHADER_RESOURCE,
        nri::BufferUsageBits::SHADER_RESOURCE_STORAGE);
    const auto& limits = mFrameBuffer->mCore.GetDeviceDesc(*mFrameBuffer->mDevice).memory;
    uint64_t maximumPrimitives = std::min<uint64_t>(requestedPrimitives, std::numeric_limits<uint32_t>::max() / 3ull);
    if (limits.bufferMaxSize != 0)
    {
        maximumPrimitives = std::min<uint64_t>(maximumPrimitives, limits.bufferMaxSize / (2ull * sizeof(nri_scene::SceneVertex)));
        maximumPrimitives = std::min<uint64_t>(maximumPrimitives, limits.bufferMaxSize / (3ull * sizeof(uint32_t)));
        maximumPrimitives = std::min<uint64_t>(maximumPrimitives, limits.bufferMaxSize / sizeof(nri_scene::PrimitiveData));
    }
    const uint64_t minimumPrimitives = std::max<uint64_t>(1,
        (stats.rawSelectedLargestUniqueGeometryBytes + bytesPerPrimitive - 1) / bytesPerPrimitive);
    std::array<nri::BufferDesc, 3> descriptors = {};
    std::array<uint64_t, 3> predicted = {};
    const auto selection = SelectNRIPersistentVoxelGeometryArenaCapacity(maximumPrimitives, minimumPrimitives,
        [&](uint64_t candidatePrimitives)
    {
        std::array<nri::BufferDesc, 3> candidate = {};
        candidate[0].size = candidatePrimitives * 2ull * sizeof(nri_scene::SceneVertex);
        candidate[0].structureStride = sizeof(nri_scene::SceneVertex);
        candidate[0].usage = geometryUsage;
        candidate[1].size = candidatePrimitives * 3ull * sizeof(uint32_t);
        candidate[1].structureStride = sizeof(uint32_t);
        candidate[1].usage = geometryUsage;
        candidate[2].size = candidatePrimitives * sizeof(nri_scene::PrimitiveData);
        candidate[2].structureStride = sizeof(nri_scene::PrimitiveData);
        candidate[2].usage = primitiveUsage;
        uint64_t candidateBytes = 0;
        std::array<uint64_t, 3> requirementsBytes = {};
        for (size_t i = 0; i < candidate.size(); ++i)
        {
            nri::MemoryDesc requirements = {};
            mFrameBuffer->mCore.GetBufferMemoryDesc2(*mFrameBuffer->mDevice, candidate[i], nri::MemoryLocation::DEVICE, requirements);
            if (requirements.size < candidate[i].size ||
                requirements.size > std::numeric_limits<uint64_t>::max() - candidateBytes ||
                (limits.allocationMaxSize != 0 && requirements.size > limits.allocationMaxSize)) return false;
            requirementsBytes[i] = requirements.size;
            candidateBytes += requirements.size;
        }
        // A partial plan budgets future AS/scratch for the selected geometry
        // capacity, not an assertion that the entire manifest will fit later.
        const auto candidateBudget = BuildNRIPersistentVoxelGeometryArenaBudget(currentUsage, adapter.localBudgetBytes,
            settings.minimumLocalMemoryReserveBytes, candidateBytes, candidatePrimitives * bytesPerPrimitive,
            stats.rawSelectedUniqueSourceBytes, settings.peakEstimatePercent);
        if (!candidateBudget.fits) return false;
        descriptors = candidate;
        predicted = requirementsBytes;
        predictedBytes = candidateBytes;
        budget = candidateBudget;
        return true;
    });
    testedCandidates = selection.candidates;
    if (selection.primitives == 0)
    { report("insufficient-meaningful-capacity"); return; }
    selectedBytes = selection.primitives * bytesPerPrimitive;

    std::array<NRIBufferResource, 3> reserved = {};
    const auto discard = [&]()
    {
        for (auto& resource : reserved) DestroyBufferResource(resource);
        committedBytes = 0;
    };
    const auto services = BuildNRIPersistentVoxelAdmissionServices(*this);
    for (size_t i = 0; i < reserved.size(); ++i)
    {
        if (!services.EnsureArenaBuffer(reserved[i], descriptors[i].size, descriptors[i].structureStride,
            descriptors[i].usage, NRIResourceComputeShaderResourceAccess()))
        { discard(); report("allocation-failed"); return; }
        // Reservation creates no geometry. Do not copy uninitialized reserved
        // capacity if an unmanifested runtime mesh eventually causes growth.
        reserved[i].usedSize = 0;
        if (reserved[i].memorySize != predicted[i])
        { discard(); report("memory-requirements-changed"); return; }
        committedBytes += reserved[i].memorySize;
    }
    // Budget can change while the driver allocates. A second live query and a
    // conservative initial+actual floor protect against delayed usage reporting.
    postAdapter = mFrameBuffer->GetAdapterMemoryTelemetry();
    const uint64_t usageFloor = currentUsage > std::numeric_limits<uint64_t>::max() - committedBytes ?
        std::numeric_limits<uint64_t>::max() : currentUsage + committedBytes;
    const uint64_t finalUsage = std::max(usageFloor, postAdapter.localUsageBytes);
    const uint64_t remaining = postAdapter.localBudgetBytes > finalUsage ? postAdapter.localBudgetBytes - finalUsage : 0;
    if (!postAdapter.liveUsageAvailable || postAdapter.localBudgetBytes == 0 || finalUsage > postAdapter.localBudgetBytes ||
        remaining < budget.futureBytes ||
        remaining - budget.futureBytes < settings.minimumLocalMemoryReserveBytes)
    { discard(); report("budget-changed"); return; }

    arena.vertexBuffer = reserved[0];
    arena.indexBuffer = reserved[1];
    arena.primitiveBuffer = reserved[2];
    report("reserved");
}
