#pragma once

#include <cstdint>

namespace nri_scene
{
static constexpr uint32_t PrecachedVoxelSourceBit_MountedVoxelPreload = 1u << 3;
static constexpr uint32_t PrecachedVoxelSourceBit_MountedPreloadMap = 1u << 6;
static constexpr uint32_t PrecachedVoxelSourceBit_EnemyFamily = 1u << 7;
static constexpr uint32_t PrecachedVoxelSourceBit_SpawnableEnemyFamily = 1u << 8;
static constexpr uint32_t PrecachedVoxelSourceBits_EnemyFamilies =
    PrecachedVoxelSourceBit_EnemyFamily | PrecachedVoxelSourceBit_SpawnableEnemyFamily;

// One classification authority for raw planning, CPU planning and the queue.
// Required service preserves all admission/allocation budgets; it is not force.
inline bool IsRequiredVoxelPreload(uint32_t sourceBits, int32_t priority, bool gpuForce, bool gpuPrefer)
{
    if ((sourceBits & PrecachedVoxelSourceBit_EnemyFamily) != 0) return true;
    return priority <= 0 && (sourceBits & PrecachedVoxelSourceBit_MountedVoxelPreload) != 0 &&
        (gpuForce || (gpuPrefer && (sourceBits & PrecachedVoxelSourceBit_MountedPreloadMap) != 0));
}

inline bool IsOptionalPreferredVoxelPreload(uint32_t sourceBits, int32_t priority, bool gpuForce, bool gpuPrefer)
{
    return (sourceBits & (PrecachedVoxelSourceBit_MountedVoxelPreload | PrecachedVoxelSourceBit_SpawnableEnemyFamily)) != 0 && gpuPrefer && !gpuForce &&
        !IsRequiredVoxelPreload(sourceBits, priority, gpuForce, gpuPrefer);
}

// Retain what each input requested before the graph merges priority and
// source bits across declarations and material contexts for the same mesh.
struct VoxelPreloadInputHints
{
    enum : uint8_t { Seen = 1, Preferred = 2, Required = 4, Forced = 8, EnemyFamily = 16, Unknown = 32 };
    uint8_t bits = 0;

    void Observe(uint32_t sources, int32_t priority, bool force, bool prefer)
    {
        bits |= Seen;
        if ((sources & (PrecachedVoxelSourceBit_MountedVoxelPreload | PrecachedVoxelSourceBit_SpawnableEnemyFamily)) && prefer) bits |= Preferred;
        if (IsRequiredVoxelPreload(sources, priority, force, prefer)) bits |= Required;
        if (force) bits |= Forced;
        if (sources & PrecachedVoxelSourceBit_EnemyFamily) bits |= EnemyFamily;
    }
    bool HasObligation() const { return (bits & (Required | Forced | EnemyFamily)) != 0; }
    bool OnlyOptionalPreference() const
    { return (bits & (Seen | Preferred | Unknown)) == (Seen | Preferred) && !HasObligation(); }
};
}
