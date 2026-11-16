#pragma once

#include "nri_resources.h"

// Outer presentations advance during loading even when renderer frames do not.
// Retry policy only; never use this clock as a resource-completion fence.
uint64_t GetNRIVoxelAdmissionRetryClock();
struct PersistentVoxelAdmissionEntry;
bool CanContinueNRIVoxelAllocationWait(const PersistentVoxelAdmissionEntry& entry);

enum class NRIPersistentVoxelAllocationResult : uint8_t
{
    Disabled,
    Ready,
    DeferredBudget,
    DeferredCooldown,
    DeferredSource,
    DeferredFence,
    RequirementsUnavailable,
    AllocationFailed,
};

inline bool NRIPersistentVoxelAllocationDeferred(NRIPersistentVoxelAllocationResult result)
{
    return result == NRIPersistentVoxelAllocationResult::DeferredBudget ||
        result == NRIPersistentVoxelAllocationResult::DeferredCooldown ||
        result == NRIPersistentVoxelAllocationResult::DeferredSource ||
        result == NRIPersistentVoxelAllocationResult::DeferredFence ||
        result == NRIPersistentVoxelAllocationResult::RequirementsUnavailable;
}

// Shared by metadata reservation and the later voxel build so their required
// memory and build flags cannot silently diverge.
nri::AccelerationStructureBits GetNRIPersistentVoxelAllocationBlasFlags();
