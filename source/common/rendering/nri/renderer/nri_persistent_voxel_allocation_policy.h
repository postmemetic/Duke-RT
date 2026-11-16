#pragma once

#include <cstdint>

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
