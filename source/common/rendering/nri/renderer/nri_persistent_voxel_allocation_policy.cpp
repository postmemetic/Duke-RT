#include "nri_persistent_voxel_allocation_policy.h"
#include <cstring>
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

uint32_t NRIPersistentVoxelGrowthRetry::SelectPercent(uint32_t configuredPercent) const
{
    return minimumAfterPostDenial ? 100u : std::clamp(configuredPercent, 100u, 200u);
}

void NRIPersistentVoxelGrowthRetry::ObservePostDenial(uint32_t attemptedPercent,
    uint64_t oldArenaBytes, uint64_t newArenaBytes)
{
    if (attemptedPercent > 100 && oldArenaBytes != 0 && newArenaBytes > oldArenaBytes)
        minimumAfterPostDenial = true;
}

bool NRIPersistentVoxelAllocationWaitCanContinue(const char* reason, bool hasOwnedWork)
{
    return !hasOwnedWork && reason &&
        (std::strcmp(reason, "allocation-budget-deferred") == 0 ||
         std::strcmp(reason, "allocation-budget-cooldown") == 0 ||
         std::strcmp(reason, "allocation-mesh-cooldown") == 0 ||
         std::strcmp(reason, "compute-provider-budget-retry") == 0);
}

void NRIPersistentVoxelAllocationBudgetCeiling::SetBackoffEnabled(bool enabled)
{
    if (backoffEnabled == enabled) return;
    backoffEnabled = enabled;
    // Changing the optional policy cannot leave an extended stale deadline.
    // Preserve the most recent actual ceiling under the original short hold.
    holdTicks = HoldTicks;
    contractionStreak = 0;
    lastDeniedBudgetBytes = 0;
    recoveryConfirmed = false;
}

void NRIPersistentVoxelAllocationBudgetCeiling::BeginTick(uint64_t epoch, uint64_t tick)
{
    if (mapEpoch != epoch || tick < lastTick)
    {
        const bool enabled = backoffEnabled;
        *this = {};
        mapEpoch = epoch;
        backoffEnabled = enabled;
    }
    lastTick = tick;
    if (ceilingBytes && tick - deniedTick >= holdTicks) ceilingBytes = 0;
    if (lastDeniedBudgetBytes && tick - deniedTick >= HistoryTicks)
    {
        lastDeniedBudgetBytes = 0;
        contractionStreak = 0;
        holdTicks = HoldTicks;
    }
}

uint64_t NRIPersistentVoxelAllocationBudgetCeiling::Apply(uint64_t epoch, uint64_t tick, uint64_t freshBudget)
{
    BeginTick(epoch, tick);
    // Unknown/zero budget never becomes positive through remembered history.
    return ceilingBytes ? std::min(freshBudget, ceilingBytes) : freshBudget;
}

uint64_t NRIPersistentVoxelAllocationBudgetCeiling::Remaining(uint64_t epoch, uint64_t tick)
{
    BeginTick(epoch, tick);
    return ceilingBytes ? holdTicks - (tick - deniedTick) : 0;
}

void NRIPersistentVoxelAllocationBudgetCeiling::ObservePostDenial(
    uint64_t epoch, uint64_t tick, uint64_t beforeBudget, uint64_t afterBudget)
{
    BeginTick(epoch, tick);
    if (!afterBudget || afterBudget >= beforeBudget) return;
    if (backoffEnabled)
    {
        // Small normal OS-budget variation is still the same contraction
        // episode. A materially higher admitted budget starts a fresh episode.
        const uint64_t margin = std::max<uint64_t>(16ull * 1024 * 1024, lastDeniedBudgetBytes / 64);
        const uint64_t comparableBudget = lastDeniedBudgetBytes > UINT64_MAX - margin ? UINT64_MAX : lastDeniedBudgetBytes + margin;
        const bool repeated = lastDeniedBudgetBytes && afterBudget <= comparableBudget;
        if (!repeated) contractionStreak = 1;
        else if (tick != deniedTick) contractionStreak = std::min(contractionStreak + 1u, 4u);
        holdTicks = HoldTicks << (std::max(contractionStreak, 1u) - 1u);
    }
    else holdTicks = HoldTicks;
    ceilingBytes = ceilingBytes ? std::min(ceilingBytes, afterBudget) : afterBudget;
    lastDeniedBudgetBytes = afterBudget;
    deniedTick = tick;
    recoveryConfirmed = false;
}

void NRIPersistentVoxelAllocationBudgetCeiling::ObserveSuccess(uint64_t epoch, uint64_t tick,
    uint64_t actualAllocationBytes, uint64_t observedPhysicalUsageBytes, uint64_t freshBudgetBytes)
{
    BeginTick(epoch, tick);
    // A small successful AS/texture/provider allocation says nothing about
    // whether the failed larger footprint is sustainable. Only fresh physical
    // usage above the remembered ceiling demonstrates recovery; unspent
    // provider promises and a projected reserve cannot manufacture that proof.
    if (!backoffEnabled || !lastDeniedBudgetBytes || !actualAllocationBytes ||
        observedPhysicalUsageBytes <= lastDeniedBudgetBytes || observedPhysicalUsageBytes > freshBudgetBytes) return;
    ceilingBytes = lastDeniedBudgetBytes = 0;
    holdTicks = HoldTicks;
    contractionStreak = 0;
    recoveryConfirmed = true;
}

const char* NRIPersistentVoxelAllocationBudgetCeiling::Reason() const
{
    if (!backoffEnabled) return ceilingBytes ? "fixed-hold" : "fixed-probe";
    if (ceilingBytes) return "bounded-hold";
    if (lastDeniedBudgetBytes) return "periodic-probe";
    return recoveryConfirmed ? "recovery-confirmed" : "unobserved";
}

void NRIPersistentVoxelAllocationRetry::BeginTick(uint64_t mapEpoch, uint64_t tick)
{
    if (epoch != mapEpoch || tick < lastTick)
    {
        entries.clear();
        epoch = mapEpoch;
    }
    if (tick == lastTick) return;
    lastTick = tick;
    for (auto it = entries.begin(); it != entries.end();)
    {
        if (tick - it->second.tick >= 30) it = entries.erase(it);
        else ++it;
    }
}

uint32_t NRIPersistentVoxelAllocationRetry::Remaining(
    uint64_t mapEpoch, uint64_t tick, uint64_t meshKey, uint64_t allocationBytes)
{
    BeginTick(mapEpoch, tick);
    if (!allocationBytes) return 0;
    const auto it = entries.find(meshKey);
    if (it == entries.end() || allocationBytes < it->second.allocationBytes) return 0;
    return uint32_t(30 - (tick - it->second.tick));
}

void NRIPersistentVoxelAllocationRetry::ObserveDenial(
    uint64_t mapEpoch, uint64_t tick, uint64_t meshKey, uint64_t allocationBytes)
{
    BeginTick(mapEpoch, tick);
    if (allocationBytes) entries[meshKey] = { tick, allocationBytes };
}

void NRIPersistentVoxelAllocationRetry::ObserveSuccess(
    uint64_t mapEpoch, uint64_t tick, uint64_t meshKey, uint64_t allocationBytes)
{
    BeginTick(mapEpoch, tick);
    const auto it = entries.find(meshKey);
    // An AS-only success can fit an existing range without proving that the
    // previously rejected whole-arena replacement now fits. Keep its deadline.
    if (it != entries.end() && allocationBytes >= it->second.allocationBytes) entries.erase(it);
}
