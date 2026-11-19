#pragma once

#include <cstdint>

namespace nri_scene
{
static constexpr uint32_t PrecachedVoxelSourceBit_MountedVoxelPreload = 1u << 3;
static constexpr uint32_t PrecachedVoxelSourceBit_MountedPreloadMap = 1u << 6;
static constexpr uint32_t PrecachedVoxelSourceBit_EnemyFamily = 1u << 7;

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
    return (sourceBits & PrecachedVoxelSourceBit_MountedVoxelPreload) != 0 && gpuPrefer && !gpuForce &&
        !IsRequiredVoxelPreload(sourceBits, priority, gpuForce, gpuPrefer);
}
}
