#pragma once
#include <algorithm>
#include <cstdint>

inline uint64_t NRIVoxelComputeSaturatingAdd(uint64_t a, uint64_t b)
{
    return b > UINT64_MAX - a ? UINT64_MAX : a + b;
}

enum class NRIVoxelComputeProviderStage : uint8_t { RawSource, Dispatch };

struct NRIVoxelComputeProviderReservation
{
    uint64_t rawSourceBytes = 0;
    uint64_t dispatchBytes = 0;
    uint64_t Total() const { return NRIVoxelComputeSaturatingAdd(rawSourceBytes, dispatchBytes); }
    uint64_t Bytes(NRIVoxelComputeProviderStage stage) const
    { return stage == NRIVoxelComputeProviderStage::RawSource ? rawSourceBytes : dispatchBytes; }
    void Materialized(NRIVoxelComputeProviderStage stage)
    { (stage == NRIVoxelComputeProviderStage::RawSource ? rawSourceBytes : dispatchBytes) = 0; }
};

struct NRIVoxelComputeBudgetRetry
{
    // Presentation generations age during loading; renderer frames and
    // completion fences intentionally remain separate clocks.
    uint64_t nextTick = 0;
    uint64_t firstDeniedTick = UINT64_MAX;
    uint64_t ownerKey = 0;
    bool Waiting(uint64_t tick) const { return tick < nextTick; }
    // A repeatedly denied unsubmitted owner yields its admission resources.
    // A new batch/page owner starts its own bounded observation window.
    bool Denied(uint64_t tick, uint64_t key)
    {
        if (firstDeniedTick == UINT64_MAX || ownerKey != key || tick < firstDeniedTick)
        { firstDeniedTick = tick; ownerKey = key; }
        nextTick = NRIVoxelComputeSaturatingAdd(tick, 30);
        if (tick - firstDeniedTick < 60) return false;
        firstDeniedTick = tick;
        return true;
    }
    void Accepted() { *this = {}; }
};

// usage includes actual resources plus all unspent/conservatively held promises.
// A provider may spend only promises belonging to its page or dispatched meshes.
inline uint64_t NRIVoxelComputeUsageAfterOwnerCredit(uint64_t usage,
    uint64_t allReservations, uint64_t ownerReservations)
{
    // Saturation lost information: never manufacture headroom by subtracting
    // an owner credit from an unknown larger usage value.
    if (usage == UINT64_MAX) return usage;
    return usage - std::min(usage, std::min(allReservations, ownerReservations));
}
