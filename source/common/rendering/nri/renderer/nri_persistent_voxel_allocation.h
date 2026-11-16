#pragma once

#include "nri_resources.h"

enum class NRIPersistentVoxelAllocationResult : uint8_t
{
    Disabled,
    Ready,
    DeferredBudget,
    DeferredFence,
    RequirementsUnavailable,
    AllocationFailed,
};

inline bool NRIPersistentVoxelAllocationDeferred(NRIPersistentVoxelAllocationResult result)
{
    return result == NRIPersistentVoxelAllocationResult::DeferredBudget ||
        result == NRIPersistentVoxelAllocationResult::DeferredFence ||
        result == NRIPersistentVoxelAllocationResult::RequirementsUnavailable;
}

// Shared by metadata reservation and the later voxel build so their required
// memory and build flags cannot silently diverge.
nri::AccelerationStructureBits GetNRIPersistentVoxelAllocationBlasFlags();
