#pragma once
#include <cstdint>

struct NRIPersistentVoxelBudgetSnapshot
{
    uint64_t trackedBytes = 0;
    uint64_t computeReservedBytes = 0;
    uint64_t liveUsageBytes = 0;
    uint64_t liveBudgetBytes = 0;
    uint64_t usageBytes = 0;
    uint64_t budgetBytes = 0;
    uint64_t rawBudgetBytes = 0; // Fresh policy limit before allocation-only hysteresis.
    uint64_t allocationCeilingBytes = 0;
    uint64_t allocationCeilingRemainingTicks = 0;
    bool liveAvailable = false;
};

// Current/pending/in-flight ownership is checked by the residency owner, not
// guessed from map membership or camera capture age by this policy.
inline bool NRIVoxelInactiveAgeEligible(uint32_t frame, uint32_t inactiveSince, uint32_t grace)
{
    return frame >= inactiveSince && frame - inactiveSince >= grace;
}

inline uint64_t NRIVoxelCacheLowTarget(uint64_t limit)
{
    return limit - limit / 4;
}

struct NRIVoxelInactiveMeshTrim
{
    bool trim = false;
    uint64_t targetBytes = 0;
};

inline NRIVoxelInactiveMeshTrim ResolveNRIVoxelInactiveMeshTrim(
    int32_t configuredLimit, uint64_t inactiveBytes, bool pressure, bool quiescentTransition)
{
    if (pressure) return { true, 0 };
    // Automatic mode preserves runtime reuse opportunities. A synchronized
    // transition has no live authority and may discard all inactive history.
    if (configuredLimit < 0) return { quiescentTransition, 0 };
    const uint64_t limit = uint64_t(configuredLimit);
    return { inactiveBytes > limit, NRIVoxelCacheLowTarget(limit) };
}

inline uint64_t NRIVoxelBudgetPressure(uint64_t usage, uint64_t budget, uint64_t reserve)
{
    if (!budget) return 0;
    const uint64_t available = budget > reserve ? budget - reserve : 0;
    return usage > available ? usage - available : 0;
}

struct NRIVoxelRetentionPressure
{
    bool active = false;
    uint32_t healthySince = UINT32_MAX;
    uint64_t reclaimBytes = 0, warmLogicalTarget = 0;
    uint64_t generation = 0;

    void Update(uint64_t map, uint32_t frame, uint64_t usage, uint64_t budget, uint64_t reserve)
    {
        if (generation != map) { *this = {}; generation = map; }
        if (!budget) { reclaimBytes = 0; return; } // Unknown budget cannot justify new eviction.
        const uint64_t margin = budget / 64 > 16ull * 1024 * 1024 ? budget / 64 : 16ull * 1024 * 1024;
        if (NRIVoxelBudgetPressure(usage, budget, reserve)) { active = true; healthySince = UINT32_MAX; }
        else if (active && !NRIVoxelBudgetPressure(usage, budget, reserve + margin))
        {
            if (healthySince == UINT32_MAX) healthySince = frame;
            if (frame >= healthySince && frame - healthySince >= 120) active = false;
        }
        else healthySince = UINT32_MAX;
        reclaimBytes = active ? NRIVoxelBudgetPressure(usage, budget, reserve + margin) : 0;
        // This bounds preferential retention under pressure, not all caching.
        // Emergency over-budget operation has no warm reservation.
        warmLogicalTarget = usage > budget ? 0 : budget / 16;
    }
};

inline bool NRIVoxelPressureVictim(uint64_t physicalBytes, uint32_t warmRank,
    uint64_t warmLogicalBytes, uint64_t warmTarget, uint64_t retiredPhysicalBytes, uint64_t reclaimTarget)
{
    return physicalBytes != 0 && retiredPhysicalBytes < reclaimTarget &&
        (warmRank == 0 || warmLogicalBytes > warmTarget);
}
