#pragma once

#include "../nri_adapter_memory.h"

namespace nri { struct AdapterDesc; }

// Use dedicated capacity for the tier. The driver's live budget prices each
// allocation separately and can contract even on a large adapter.
constexpr bool NRIShouldPreloadOpeningEnemyMaterials(int mode, bool discrete, uint64_t dedicatedBytes)
{
    return mode == 1 || (mode == 2 && NRIIsSmallDiscreteGpu(discrete, dedicatedBytes));
}

void PrintNRIOpeningEnemyMaterialPolicy(const nri::AdapterDesc& adapter);

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

    uint64_t RequiredBytes(uint32_t stride, uint64_t currentPoseRows = 0) const
    {
        if (!owners || !stride) return 0;
        const uint64_t maxExtraRows = extraByteLimit / stride;
        if (extraRows > maxExtraRows || currentPoseRows > maxExtraRows - extraRows ||
            cursorRows > UINT32_MAX || extraRows + currentPoseRows > UINT32_MAX - cursorRows) return 0;
        return (cursorRows + extraRows + currentPoseRows) * stride;
    }
};
