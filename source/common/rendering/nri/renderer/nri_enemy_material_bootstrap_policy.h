#pragma once

#include <cstdint>
#include <limits>

// Conservative pre-publication backing. Existing holes are deliberately not
// credited: the allocator remains the authority when each family commits.
struct NRIEnemyMaterialBootstrapPlan
{
    static constexpr uint32_t MaximumOwners = 16;
    static constexpr uint64_t MaximumExtraBytes = 8ull * 1024 * 1024;
    uint32_t ownerLimit = MaximumOwners;
    uint64_t extraByteLimit = MaximumExtraBytes;
    uint64_t cursorRows = 0;
    uint64_t extraRows = 0;
    uint32_t owners = 0;

    bool AddFamily(uint32_t missingPoses, uint32_t rowsPerPose, uint32_t stride)
    {
        if (!stride || !rowsPerPose || owners >= ownerLimit) return false;
        const uint64_t rows = uint64_t(missingPoses) * rowsPerPose;
        const uint64_t maximumExtraRows = extraByteLimit / stride;
        if (extraRows > maximumExtraRows || rows > maximumExtraRows - extraRows ||
            cursorRows > UINT32_MAX || extraRows + rows > UINT32_MAX - cursorRows) return false;
        extraRows += rows;
        ++owners;
        return true;
    }

    uint64_t RequiredBytes(uint32_t stride) const
    {
        const uint64_t rows = cursorRows + extraRows;
        return owners && stride && rows <= std::numeric_limits<uint64_t>::max() / stride ? rows * stride : 0;
    }
};
