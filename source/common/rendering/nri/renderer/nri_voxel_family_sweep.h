#pragma once

#include "nri_persistent_voxel_retention_policy.h"
#include <unordered_set>
#include <vector>

struct NRIVoxelSweepCandidate
{
    uint64_t key, bytes, physicalBytes;
    uint32_t since, warmRank;
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
    std::vector<uint64_t> additionalKeys;
    bool Fits(uint64_t hardDeficit, uint64_t remainingCharge, uint32_t remainingMeshes) const
    {
        return !additionalKeys.empty() && additionalKeys.size() <= remainingMeshes &&
            additionalBytes <= remainingCharge && otherRetiringBytes >= hardDeficit;
    }
};

// Receives only candidates that ordinary maintenance could evict, in its real eviction order.
// Held keys have already been removed from the inactive and warm byte totals by an earlier proof.
NRIVoxelFamilySweepProof NRIPriceFamilySweep(const std::vector<NRIVoxelSweepCandidate>& candidates,
    const std::unordered_set<uint64_t>& held, const std::vector<uint64_t>& familyKeys,
    uint64_t inactive, uint64_t warm, int32_t meshLimit, const NRIVoxelRetentionPressure& policy);
