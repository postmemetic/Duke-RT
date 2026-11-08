#pragma once
#include <cstdint>

struct NRIPersistentVoxelBudgetSnapshot
{
    uint64_t trackedBytes = 0;
    uint64_t liveUsageBytes = 0;
    uint64_t liveBudgetBytes = 0;
    uint64_t usageBytes = 0;
    uint64_t budgetBytes = 0;
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
