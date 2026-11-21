#pragma once

#include "nri_persistent_voxel_allocation_policy.h"
#include <array>
#include <cstdint>

// Grow each stream in its own row units as the real arena would. Integer
// rounding accumulates independently, even for a 2:3:1 direct mesh.
inline std::array<uint64_t, 3> NRIRequiredArenaRows(uint32_t firstPrimitives, uint64_t requiredPrimitives,
    uint32_t growthPercent)
{
    if (!firstPrimitives || requiredPrimitives < firstPrimitives || requiredPrimitives > UINT32_MAX / 3u) return {};
    std::array<uint64_t, 3> rows{};
    constexpr std::array<uint32_t, 3> ratio{2, 3, 1};
    for (size_t i = 0; i < rows.size(); ++i)
    {
        rows[i] = GetNRIPersistentVoxelGrowthCapacity(uint64_t(firstPrimitives) * ratio[i],
            requiredPrimitives * ratio[i], 1, growthPercent);
        if (rows[i] > UINT32_MAX) return {};
    }
    return rows;
}
