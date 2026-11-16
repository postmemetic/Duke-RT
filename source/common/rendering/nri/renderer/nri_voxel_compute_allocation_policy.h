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
    uint64_t nextFrame = 0;
    uint64_t firstDeniedFrame = UINT64_MAX;
    uint64_t ownerKey = 0;
    bool Waiting(uint64_t frame) const { return frame < nextFrame; }
    // A repeatedly denied unsubmitted owner yields its admission resources.
    // A new batch/page owner starts its own bounded observation window.
    bool Denied(uint64_t frame, uint64_t key)
    {
        if (firstDeniedFrame == UINT64_MAX || ownerKey != key || frame < firstDeniedFrame)
        { firstDeniedFrame = frame; ownerKey = key; }
        nextFrame = NRIVoxelComputeSaturatingAdd(frame, 30);
        if (frame - firstDeniedFrame < 60) return false;
        firstDeniedFrame = frame;
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
