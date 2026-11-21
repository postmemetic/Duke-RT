#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace nri { struct Buffer; }

// Physical descriptor capacities reserved above the ordinary startup layout.
// Only the bytes used by a successful, camera-qualified admission become
// available to the normal range allocator. Leases keep their normal fences.
struct NRIVoxelRuntimeExtent
{
    std::array<nri::Buffer*, 3> buffers{};
    std::array<uint64_t, 3> physicalBytes{};
    uint64_t extraPrimitives = 0;

    bool Funded() const { return extraPrimitives != 0; }

    bool Covers(const std::array<nri::Buffer*, 3>& currentBuffers,
        const std::array<uint32_t, 3>& requestedRows, const std::array<uint64_t, 3>& endBytes) const
    {
        if (!Funded() || buffers != currentBuffers) return false;
        // Leave room for at least two comparable poses of the observed actor.
        const uint64_t perPose = extraPrimitives / 2;
        if (requestedRows[0] > 2 * perPose || requestedRows[1] > 3 * perPose || requestedRows[2] > perPose)
            return false;
        for (std::size_t i = 0; i < endBytes.size(); ++i)
            if (endBytes[i] > physicalBytes[i]) return false;
        return true;
    }
};
