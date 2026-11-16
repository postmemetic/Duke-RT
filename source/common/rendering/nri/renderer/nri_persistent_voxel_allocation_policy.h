#pragma once

#include <cstdint>
#include <cstddef>
#include <unordered_map>

struct NRIPersistentVoxelAllocationBudget
{
    uint64_t usageBytes = 0;
    uint64_t allocationBytes = 0;
    uint64_t reserveBytes = 0;
    uint64_t projectedBytes = 0;
    bool overflow = false;
    bool fits = false;
};

// The usage already includes old backing, pending AS and retired resources.
// All replacing allocations are added together; none of the old bytes are
// subtracted until the resource owner has actually retired them.
NRIPersistentVoxelAllocationBudget BuildNRIPersistentVoxelAllocationBudget(
    uint64_t usageBytes, uint64_t budgetBytes, uint64_t allocationBytes, uint64_t reserveBytes);
uint64_t GetNRIPersistentVoxelGrowthCapacity(uint64_t currentBytes,
    uint64_t requiredBytes, uint32_t stride, uint32_t growthPercent);
uint64_t GetNRIPersistentVoxelStartupTarget(uint64_t plannedBytes,
    uint32_t percent, uint64_t maximumBytes);

// A live budget can contract while speculative arena backing is allocated.
// Remember that result for this map's shared arenas so deferred owners ask
// for required capacity instead of repeating the same rejected spare capacity.
// Reset with the level scheduling state; no lower budget is cached here.
struct NRIPersistentVoxelGrowthRetry
{
    bool minimumAfterPostDenial = false;
    uint32_t SelectPercent(uint32_t configuredPercent) const;
    void ObservePostDenial(uint32_t attemptedPercent, uint64_t oldArenaBytes, uint64_t newArenaBytes);
};

// Different material admission pairs can request the same mesh allocation.
// Only failed concrete allocation attempts populate this short-lived ledger;
// smaller requirements and allocation-free reuse remain eligible immediately.
class NRIPersistentVoxelAllocationRetry
{
public:
    void BeginFrame(uint64_t mapEpoch, uint32_t frame);
    uint32_t Remaining(uint64_t mapEpoch, uint32_t frame, uint64_t meshKey, uint64_t allocationBytes);
    void ObserveDenial(uint64_t mapEpoch, uint32_t frame, uint64_t meshKey, uint64_t allocationBytes);
    void ObserveSuccess(uint64_t mapEpoch, uint32_t frame, uint64_t meshKey, uint64_t allocationBytes);
    std::size_t Size() const { return entries.size(); }

private:
    struct Entry { uint32_t frame; uint64_t allocationBytes; };
    std::unordered_map<uint64_t, Entry> entries;
    uint64_t epoch = 0;
    uint32_t lastFrame = UINT32_MAX;
};
