#include "nri_voxel_family_sweep.h"

NRIVoxelFamilySweepProof NRIPriceFamilySweep(const std::vector<NRIVoxelSweepCandidate>& candidates,
    const std::unordered_set<uint64_t>& held, const std::vector<uint64_t>& familyKeys,
    uint64_t inactive, uint64_t warm, int32_t meshLimit, const NRIVoxelRetentionPressure& policy)
{
    NRIVoxelFamilySweepProof proof;
    const std::unordered_set<uint64_t> family(familyKeys.begin(), familyKeys.end());
    for (const auto& mesh : candidates)
    {
        if (!family.count(mesh.key) || held.count(mesh.key)) continue;
        proof.additionalKeys.push_back(mesh.key);
        proof.additionalBytes += mesh.bytes;
        if (mesh.warmRank) proof.additionalWarmBytes += mesh.bytes;
    }
    inactive -= proof.additionalBytes;
    warm -= proof.additionalWarmBytes;
    const auto trim = ResolveNRIVoxelInactiveMeshTrim(meshLimit, inactive, false, false);
    for (const auto& mesh : candidates)
    {
        if (family.count(mesh.key) || held.count(mesh.key)) continue;
        if (!NRIVoxelSweepVictim(mesh, trim, inactive, true, warm, policy, proof.otherRetiringBytes)) continue;
        inactive -= mesh.bytes;
        if (mesh.warmRank) warm -= mesh.bytes;
        proof.otherRetiringBytes += mesh.physicalBytes;
    }
    return proof;
}
