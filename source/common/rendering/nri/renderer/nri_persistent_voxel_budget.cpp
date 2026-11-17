#include "nri_renderer.h"
#include "nri_cvars.h"
#include "../system/nri_renderdevice.h"
#include "perf_audit.h"
#include <algorithm>
#include <cstdio>
#include <limits>

NRIPersistentVoxelBudgetSnapshot NRIRenderer::GetPersistentVoxelBudgetSnapshot(bool forceRefresh)
{
    // DXGI budget queries are bounded maintenance, not another per-frame walk.
    // Renderer tracked commitment remains current between OS-budget samples.
    const int testBudget = std::max(0, (int)nri_ptvoxelbudgettestbytes);
    const bool refresh = forceRefresh || mVoxelBudgetSnapshotFrame == UINT32_MAX ||
        mVoxelBudgetSnapshotMap != mMapWorld.buildSerial || mVoxelBudgetSnapshotOverride != testBudget ||
        mFrameIndex < mVoxelBudgetSnapshotFrame || mFrameIndex - mVoxelBudgetSnapshotFrame >= 60;
    if (refresh)
    {
        const auto live = mFrameBuffer ? mFrameBuffer->GetAdapterMemoryTelemetry() : NRIAdapterMemoryTelemetry{};
        mVoxelBudgetSnapshot.liveAvailable = live.liveUsageAvailable && live.localBudgetBytes != 0;
        mVoxelBudgetSnapshot.liveUsageBytes = live.localUsageBytes;
        mVoxelBudgetSnapshot.liveBudgetBytes = mVoxelBudgetSnapshot.liveAvailable ? live.localBudgetBytes : 0;
        mVoxelBudgetSnapshot.budgetBytes = mVoxelBudgetSnapshot.liveBudgetBytes;
        if (testBudget != 0)
            mVoxelBudgetSnapshot.budgetBytes = mVoxelBudgetSnapshot.budgetBytes != 0 ?
                std::min(mVoxelBudgetSnapshot.budgetBytes, uint64_t(testBudget)) : uint64_t(testBudget);
        mVoxelBudgetSnapshotFrame = mFrameIndex;
        mVoxelBudgetSnapshotMap = mMapWorld.buildSerial;
        mVoxelBudgetSnapshotOverride = testBudget;
    }
    if (forceRefresh) mPersistentVoxels.MarkMaintenanceMutation();
    mVoxelBudgetSnapshot.trackedBytes = GetMemoryTelemetry().totalTrackedBytes;
    mVoxelBudgetSnapshot.computeReservedBytes = 0;
    if (forceRefresh)
    {
        // General renderer telemetry excludes pending voxel jobs and retired
        // backing. They still own physical allocations until fence completion.
        const auto add = [&](uint64_t bytes)
        {
            auto& tracked = mVoxelBudgetSnapshot.trackedBytes;
            tracked = bytes > std::numeric_limits<uint64_t>::max() - tracked ?
                std::numeric_limits<uint64_t>::max() : tracked + bytes;
        };
        const auto voxels = mPersistentVoxels.GetMemoryUsage();
        add(voxels.admissionTransientBufferBytes);
        add(voxels.admissionTransientAsBytes);
        mVoxelBudgetSnapshot.computeReservedBytes = voxels.admissionProviderReservedBytes;
        for (const auto& frame : mResidentUploadScratchFrames)
        {
            for (const auto& buffer : frame.retiredBuffers)
                if (buffer.memoryLocation == nri::MemoryLocation::DEVICE) add(buffer.memorySize);
            for (const auto& structure : frame.retiredAccelerationStructures) add(structure.memorySize);
        }
        add(mResidentBlasScratchRetirement.RetainedBytes());
        const auto compute = GetNRIVoxelComputeMemoryUsage();
        add(compute.rawDeviceBytes);
        add(compute.transientInputDeviceBytes);
        add(compute.transientGeneratedBytes);
        add(compute.diagnosticAsBytes);
        // Conservatively budget upload/readback backing too; providers may use
        // local host-visible heaps, and their exact placement is backend-owned.
        add(compute.rawUploadBytes);
        add(compute.transientInputUploadBytes);
        add(compute.statusReadbackBytes);
        add(compute.geometryReadbackBufferBytes);
    }
    mVoxelBudgetSnapshot.usageBytes = std::max(mVoxelBudgetSnapshot.trackedBytes,
        mVoxelBudgetSnapshot.liveAvailable ? mVoxelBudgetSnapshot.liveUsageBytes : 0);
    mVoxelBudgetSnapshot.usageBytes = mVoxelBudgetSnapshot.computeReservedBytes > UINT64_MAX - mVoxelBudgetSnapshot.usageBytes ?
        UINT64_MAX : mVoxelBudgetSnapshot.usageBytes + mVoxelBudgetSnapshot.computeReservedBytes;
    // Do not feed allocation hysteresis back into the cached maintenance budget.
    // Both geometry and compute providers receive the same bounded fresh copy.
    auto snapshot = mVoxelBudgetSnapshot;
    snapshot.rawBudgetBytes = snapshot.budgetBytes;
    const uint64_t retryTick = GetNRIVoxelAdmissionRetryClock();
    auto& ceiling = mPersistentVoxels.allocationBudgetCeiling;
    if (forceRefresh && (bool)nri_ptvoxelallocationbudget)
    {
        ceiling.SetBackoffEnabled((bool)nri_ptvoxelallocationretrybackoff);
        snapshot.budgetBytes = ceiling.Apply(mMapWorld.buildSerial, retryTick, snapshot.rawBudgetBytes);
        snapshot.allocationCeilingBytes = ceiling.ceilingBytes;
        snapshot.allocationCeilingRemainingTicks = ceiling.Remaining(mMapWorld.buildSerial, retryTick);
    }
    if (refresh && PerfAuditActive())
    {
        char fields[1280];
        snprintf(fields, sizeof(fields), "\"renderer_frame\":%u,\"map_epoch\":%llu,\"live_available\":%s,"
            "\"tracked_bytes\":%llu,\"live_usage_bytes\":%llu,\"live_budget_bytes\":%llu,"
            "\"effective_usage_bytes\":%llu,\"effective_budget_bytes\":%llu,\"test_budget_bytes\":%d,\"allocation_refresh\":%s,\"reserved_provider_bytes\":%llu,"
            "\"raw_budget_bytes\":%llu,\"allocation_ceiling_bytes\":%llu,\"allocation_ceiling_remaining_ticks\":%llu,\"retry_tick\":%llu,\"retry_clock_unit\":\"presentation\","
            "\"allocation_backoff_enabled\":%s,\"allocation_hold_ticks\":%llu,\"allocation_contraction_streak\":%u,\"allocation_probe_reason\":\"%s\"",
            mFrameIndex, (unsigned long long)mMapWorld.buildSerial, snapshot.liveAvailable ? "true" : "false",
            (unsigned long long)snapshot.trackedBytes, (unsigned long long)snapshot.liveUsageBytes,
            (unsigned long long)snapshot.liveBudgetBytes, (unsigned long long)snapshot.usageBytes,
            (unsigned long long)snapshot.budgetBytes, testBudget, forceRefresh ? "true" : "false",
            (unsigned long long)snapshot.computeReservedBytes, (unsigned long long)snapshot.rawBudgetBytes,
            (unsigned long long)snapshot.allocationCeilingBytes, (unsigned long long)snapshot.allocationCeilingRemainingTicks,
            (unsigned long long)retryTick, ceiling.backoffEnabled ? "true" : "false",
            (unsigned long long)ceiling.holdTicks, ceiling.contractionStreak,
            forceRefresh ? ceiling.Reason() : "not-allocation-refresh");
        PerfAuditWriteEvent("voxel_residency_budget", PerfAuditCurrentToken(), fields);
    }
    return snapshot;
}

void NRIRenderer::AuditPersistentVoxelArenaGrowthBudget(uint64_t oldBytes, uint64_t newCapacityBytes, uint32_t stride)
{
    if (!PerfAuditActive()) return;
    const auto budget = GetPersistentVoxelBudgetSnapshot();
    const uint64_t projected = newCapacityBytes > std::numeric_limits<uint64_t>::max() - budget.usageBytes ?
        std::numeric_limits<uint64_t>::max() : budget.usageBytes + newCapacityBytes;
    char fields[640];
    snprintf(fields, sizeof(fields), "\"renderer_frame\":%u,\"stride\":%u,\"old_committed_bytes\":%llu,"
        "\"new_logical_capacity_bytes\":%llu,\"current_usage_bytes\":%llu,\"budget_bytes\":%llu,"
        "\"projected_overlap_usage_lower_bound_bytes\":%llu,\"live_available\":%s,\"budget_age_frames\":%u,"
        "\"required_admission_preserved\":true", mFrameIndex, stride, (unsigned long long)oldBytes,
        (unsigned long long)newCapacityBytes, (unsigned long long)budget.usageBytes,
        (unsigned long long)budget.budgetBytes, (unsigned long long)projected,
        budget.liveAvailable ? "true" : "false", mFrameIndex - mVoxelBudgetSnapshotFrame);
    PerfAuditWriteEvent("voxel_arena_growth_budget", PerfAuditCurrentToken(), fields);
}
