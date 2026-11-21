#pragma once

#include "nri_persistent_voxel_retention_policy.h"
#include <unordered_set>
#include <vector>

struct NRIVoxelSweepCandidate
{
    uint64_t key, bytes, physicalBytes;
    uint32_t since, warmRank;
    bool ordinaryVictim = false;
    uint32_t lastRequestedFrame = UINT32_MAX;
};

inline bool NRIVoxelSweepVictim(const NRIVoxelSweepCandidate& mesh, NRIVoxelInactiveMeshTrim trim,
    uint64_t inactive, bool pressure, uint64_t warm, const NRIVoxelRetentionPressure& policy, uint64_t retiring)
{
    return !mesh.bytes || (trim.trim && inactive > trim.targetBytes) ||
        (pressure && NRIVoxelPressureVictim(mesh.physicalBytes, mesh.warmRank, warm,
            policy.warmLogicalTarget, retiring, policy.reclaimBytes));
}

struct NRIVoxelFamilySweepProof
{
    uint64_t additionalBytes = 0, additionalWarmBytes = 0, otherRetiringBytes = 0;
    uint32_t ordinaryFamilyVictims = 0;
    std::vector<uint64_t> additionalKeys;
    bool Fits(uint64_t hardDeficit, uint64_t remainingCharge, uint32_t remainingMeshes) const
    {
        return ordinaryFamilyVictims != 0 && additionalKeys.size() <= remainingMeshes &&
            additionalBytes <= remainingCharge && otherRetiringBytes >= hardDeficit;
    }
};

// Freeze the ordinary sweep's selections on the untouched snapshot, in the real eviction order.
void NRIMarkOrdinarySweepVictims(std::vector<NRIVoxelSweepCandidate>& candidates,
    uint64_t inactive, uint64_t warm, int32_t meshLimit, const NRIVoxelRetentionPressure& policy);

// The full family is charged, but only marked ordinary victims may fund the hard reserve.
// Held keys have already been removed from the inactive and warm byte totals by an earlier proof.
NRIVoxelFamilySweepProof NRIPriceFamilySweep(const std::vector<NRIVoxelSweepCandidate>& candidates,
    const std::unordered_set<uint64_t>& held, const std::vector<uint64_t>& familyKeys,
    uint64_t inactive, uint64_t warm, int32_t meshLimit, const NRIVoxelRetentionPressure& policy);

struct NRIVoxelSweepProtection
{
    uint64_t logicalBytes = 0, physicalBytes = 0;
    uint32_t meshCount = 0;
    std::vector<NRIVoxelSweepCandidate> samples;
};

// Apply the same bound to actual eviction. With diagnostics enabled, count the substitutions
// that an unrestricted sweep would have made and collect a bounded sample of their keys.
NRIVoxelSweepProtection NRIRestrictFamilySweep(std::vector<NRIVoxelSweepCandidate>& candidates,
    const std::unordered_set<uint64_t>& held, uint64_t inactive, uint64_t warm, int32_t meshLimit,
    const NRIVoxelRetentionPressure& policy, bool diagnoseSubstitutions);

struct NRIVoxelRecurringPoseGrace
{
    uint64_t mesh = 0, bytes = 0, otherRetiring = 0;
    uint32_t requestedFrame = UINT32_MAX;
};

NRIVoxelRecurringPoseGrace NRIKeepRecurringPoseForSweep(std::vector<NRIVoxelSweepCandidate>& candidates,
    uint64_t inactive, uint64_t warm, const NRIVoxelRetentionPressure& policy, uint64_t hardDeficit,
    uint64_t availableCharge, uint32_t frame, uint32_t interval);
