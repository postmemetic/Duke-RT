#pragma once

#include <array>
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
// Driver usage may already include some or all unpublished physical resources.
// Both usage inputs include unrelated provider reservations and exclude the owner's old promise.
NRIPersistentVoxelAllocationBudget ForecastNRIPartiallyCommittedVoxelBudget(
    uint64_t startingUsage, uint64_t observedUsage, uint64_t committed, uint64_t remaining,
    uint64_t provider, uint64_t reserve, uint64_t budget);
uint64_t GetNRIPersistentVoxelGrowthCapacity(uint64_t currentBytes,
    uint64_t requiredBytes, uint32_t stride, uint32_t growthPercent);
uint64_t GetNRIPersistentVoxelStartupTarget(uint64_t plannedBytes,
    uint32_t percent, uint64_t maximumBytes);

// A resource-free budget wait need not stop unrelated runtime admissions.
// The caller proves absence of concrete/promise/submitted ownership first.
bool NRIPersistentVoxelAllocationWaitCanContinue(const char* reason, bool hasOwnedWork);

// A driver's fresh pre-allocation budget can repeatedly rebound above the
// lower budget observed after committing memory. Share that measured ceiling
// across geometry/providers briefly; usage and raw budget are always refreshed.
// Queries/preflight denials never prolong this interval. After expiry the next
// fitting allocation can probe the current budget and re-arm on a new denial.
// Optional bounded backoff applies only to repeated observed contractions;
// smaller work that fits the remembered ceiling remains eligible immediately.
struct NRIPersistentVoxelAllocationBudgetCeiling
{
    static constexpr uint64_t HoldTicks = 120;
    static constexpr uint64_t MaximumHoldTicks = 960;
    static constexpr uint64_t HistoryTicks = MaximumHoldTicks * 4;
    uint64_t mapEpoch = 0;
    uint64_t lastTick = UINT64_MAX;
    uint64_t deniedTick = 0;
    uint64_t ceilingBytes = 0;
    uint64_t holdTicks = HoldTicks;
    uint64_t lastDeniedBudgetBytes = 0;
    uint32_t contractionStreak = 0;
    bool backoffEnabled = false;
    bool recoveryConfirmed = false;
    void SetBackoffEnabled(bool enabled);
    void BeginTick(uint64_t epoch, uint64_t tick);
    uint64_t Apply(uint64_t epoch, uint64_t tick, uint64_t freshBudget);
    uint64_t Remaining(uint64_t epoch, uint64_t tick);
    void ObservePostDenial(uint64_t epoch, uint64_t tick, uint64_t beforeBudget, uint64_t afterBudget);
    void ObserveSuccess(uint64_t epoch, uint64_t tick, uint64_t actualAllocationBytes,
        uint64_t observedPhysicalUsageBytes, uint64_t freshBudgetBytes);
    const char* Reason() const;
};

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

// A concrete, expensive replacement may reveal a lower live budget than its
// preflight. Compare later requests on the same backing and stream mask before
// creating another set of buffers. The seven nonempty masks bound storage.
struct NRIVoxelArenaReplacement
{
    uint32_t streamMask = 0;
    uint32_t growthPercent = 0;
    uint64_t geometryBytes = 0;
    uint64_t arenaBytes = 0;
    uint64_t rawHeadroomBytes = 0; // After charging other owners, the provider and the safety reserve.
    uint64_t projectedBytes = 0;
    uint64_t rawBudgetBytes = 0;
};

struct NRIVoxelArenaDenial
{
    NRIVoxelArenaReplacement request;
    uint64_t tick = 0;
    uint64_t firstTick = 0;
    uint64_t mesh = 0;
    uint64_t contractedBudget = 0;
    uint64_t shortfallBytes = 0;
};

class NRIVoxelArenaRetry
{
public:
    static constexpr uint64_t ProbeTicks = 1920;
    void BeginTick(uint64_t mapEpoch, uint64_t tick, bool enabled);
    void BackingChanged();
    uint64_t Generation() const { return generation; }
    const NRIVoxelArenaDenial* Remember(const NRIVoxelArenaReplacement& request, uint64_t mesh,
        uint64_t postBudget, uint64_t postPeak, double transactionMs);
    const NRIVoxelArenaDenial* Blocking(const NRIVoxelArenaReplacement& request, uint64_t mesh) const;

private:
    std::array<NRIVoxelArenaDenial, 8> denials = {};
    uint64_t epoch = 0;
    uint64_t currentTick = 0;
    uint64_t generation = 0;
    bool active = false;
};

// Different material admission pairs can request the same mesh allocation.
// Only failed concrete allocation attempts populate this short-lived ledger;
// smaller requirements and allocation-free reuse remain eligible immediately.
// Ticks are outer presentation generations, including while loading blocks
// renderer-frame advancement. They are never resource-completion fences.
class NRIPersistentVoxelAllocationRetry
{
public:
    void BeginTick(uint64_t mapEpoch, uint64_t tick);
    uint32_t Remaining(uint64_t mapEpoch, uint64_t tick, uint64_t meshKey, uint64_t allocationBytes);
    void ObserveDenial(uint64_t mapEpoch, uint64_t tick, uint64_t meshKey, uint64_t allocationBytes);
    void ObserveSuccess(uint64_t mapEpoch, uint64_t tick, uint64_t meshKey, uint64_t allocationBytes);
    std::size_t Size() const { return entries.size(); }

private:
    struct Entry { uint64_t tick; uint64_t allocationBytes; };
    std::unordered_map<uint64_t, Entry> entries;
    uint64_t epoch = 0;
    uint64_t lastTick = UINT64_MAX;
};
