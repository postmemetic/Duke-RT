#include "nri_persistent_voxel_allocation_policy.h"
#include <algorithm>
#include <limits>

namespace
{
    uint64_t Add(uint64_t a, uint64_t b, bool& overflow)
    {
        if (b > std::numeric_limits<uint64_t>::max() - a)
        {
            overflow = true;
            return std::numeric_limits<uint64_t>::max();
        }
        return a + b;
    }
}

NRIPersistentVoxelAllocationBudget BuildNRIPersistentVoxelAllocationBudget(
    uint64_t usageBytes, uint64_t budgetBytes, uint64_t allocationBytes, uint64_t reserveBytes)
{
    NRIPersistentVoxelAllocationBudget result;
    result.usageBytes = usageBytes;
    result.allocationBytes = allocationBytes;
    result.reserveBytes = reserveBytes;
    result.projectedBytes = Add(Add(usageBytes, allocationBytes, result.overflow), reserveBytes, result.overflow);
    result.fits = budgetBytes != 0 && !result.overflow && result.projectedBytes <= budgetBytes;
    return result;
}

uint64_t GetNRIPersistentVoxelGrowthCapacity(uint64_t currentBytes,
    uint64_t requiredBytes, uint32_t stride, uint32_t growthPercent)
{
    stride = std::max(1u, stride);
    const uint64_t minimum = std::max<uint64_t>(requiredBytes, stride);
    if (currentBytes >= minimum) return currentBytes;
    // Empty arenas start at the requested capacity. Speculation belongs to the
    // separately budgeted startup planner, never multiplication from zero.
    if (currentBytes == 0 || growthPercent <= 100) return minimum;
    growthPercent = std::clamp(growthPercent, 100u, 200u);
    uint64_t capacity = currentBytes;
    while (capacity < minimum)
    {
        const uint64_t extraPercent = growthPercent - 100u;
        const uint64_t extra = capacity / 100 * extraPercent +
            (capacity % 100 * extraPercent + 99) / 100;
        bool overflow = false;
        capacity = Add(capacity, std::max<uint64_t>(extra, stride), overflow);
        if (overflow) return minimum;
        const uint64_t remainder = capacity % stride;
        if (remainder) capacity = Add(capacity, stride - remainder, overflow);
        if (overflow) return minimum;
    }
    return capacity;
}

uint64_t GetNRIPersistentVoxelStartupTarget(uint64_t plannedBytes,
    uint32_t percent, uint64_t maximumBytes)
{
    percent = std::min(percent, 100u);
    const uint64_t target = plannedBytes / 100 * percent + plannedBytes % 100 * percent / 100;
    return maximumBytes ? std::min(target, maximumBytes) : target;
}
