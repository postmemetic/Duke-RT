#include "nri_renderer.h"
#include "nri_cvars.h"
#include "../system/nri_renderdevice.h"
#include "perf_audit.h"
#include <algorithm>
#include <cstdio>
#include <limits>

NRIPersistentVoxelBudgetSnapshot NRIRenderer::GetPersistentVoxelBudgetSnapshot()
{
    // DXGI budget queries are bounded maintenance, not another per-frame walk.
    // Renderer tracked commitment remains current between OS-budget samples.
    const int testBudget = std::max(0, (int)nri_ptvoxelbudgettestbytes);
    const bool refresh = mVoxelBudgetSnapshotFrame == UINT32_MAX ||
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
    mVoxelBudgetSnapshot.trackedBytes = GetMemoryTelemetry().totalTrackedBytes;
    mVoxelBudgetSnapshot.usageBytes = std::max(mVoxelBudgetSnapshot.trackedBytes,
        mVoxelBudgetSnapshot.liveAvailable ? mVoxelBudgetSnapshot.liveUsageBytes : 0);
    if (refresh && PerfAuditActive())
    {
        char fields[512];
        snprintf(fields, sizeof(fields), "\"renderer_frame\":%u,\"map_epoch\":%llu,\"live_available\":%s,"
            "\"tracked_bytes\":%llu,\"live_usage_bytes\":%llu,\"live_budget_bytes\":%llu,"
            "\"effective_usage_bytes\":%llu,\"effective_budget_bytes\":%llu,\"test_budget_bytes\":%d",
            mFrameIndex, (unsigned long long)mMapWorld.buildSerial, mVoxelBudgetSnapshot.liveAvailable ? "true" : "false",
            (unsigned long long)mVoxelBudgetSnapshot.trackedBytes, (unsigned long long)mVoxelBudgetSnapshot.liveUsageBytes,
            (unsigned long long)mVoxelBudgetSnapshot.liveBudgetBytes, (unsigned long long)mVoxelBudgetSnapshot.usageBytes,
            (unsigned long long)mVoxelBudgetSnapshot.budgetBytes, testBudget);
        PerfAuditWriteEvent("voxel_residency_budget", PerfAuditCurrentToken(), fields);
    }
    return mVoxelBudgetSnapshot;
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
