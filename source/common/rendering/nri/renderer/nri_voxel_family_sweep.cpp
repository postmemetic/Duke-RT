#include "nri_voxel_family_sweep.h"
#include <algorithm>
#include <tuple>

namespace
{
struct SweepForecast
{
    SweepForecast(uint64_t inactive, uint64_t warm, int32_t meshLimit, const NRIVoxelRetentionPressure& policy)
        : inactive(inactive), warm(warm), trim(ResolveNRIVoxelInactiveMeshTrim(meshLimit, inactive, false, false)),
          policy(policy) {}

    bool Take(const NRIVoxelSweepCandidate& mesh)
    {
        if (!NRIVoxelSweepVictim(mesh, trim, inactive, true, warm, policy, retiring)) return false;
        inactive -= mesh.bytes;
        if (mesh.warmRank) warm -= mesh.bytes;
        retiring += mesh.physicalBytes;
        return true;
    }

    uint64_t inactive, warm, retiring = 0;
    NRIVoxelInactiveMeshTrim trim;
    const NRIVoxelRetentionPressure& policy;
};
}

void NRIMarkOrdinarySweepVictims(std::vector<NRIVoxelSweepCandidate>& candidates,
    uint64_t inactive, uint64_t warm, int32_t meshLimit, const NRIVoxelRetentionPressure& policy)
{
    SweepForecast ordinary(inactive, warm, meshLimit, policy);
    for (auto& mesh : candidates) mesh.ordinaryVictim = ordinary.Take(mesh);
}

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
        if (mesh.ordinaryVictim) ++proof.ordinaryFamilyVictims;
        proof.additionalBytes += mesh.bytes;
        if (mesh.warmRank) proof.additionalWarmBytes += mesh.bytes;
    }
    SweepForecast allowed(inactive - proof.additionalBytes, warm - proof.additionalWarmBytes, meshLimit, policy);
    for (const auto& mesh : candidates)
    {
        if (mesh.ordinaryVictim && !family.count(mesh.key) && !held.count(mesh.key)) allowed.Take(mesh);
    }
    proof.otherRetiringBytes = allowed.retiring;
    return proof;
}

NRIVoxelSweepProtection NRIRestrictFamilySweep(std::vector<NRIVoxelSweepCandidate>& candidates,
    const std::unordered_set<uint64_t>& held, uint64_t inactive, uint64_t warm, int32_t meshLimit,
    const NRIVoxelRetentionPressure& policy, bool diagnoseSubstitutions)
{
    NRIVoxelSweepProtection protection;
    if (diagnoseSubstitutions)
    {
        SweepForecast unrestricted(inactive, warm, meshLimit, policy);
        for (const auto& mesh : candidates)
        {
            if (held.count(mesh.key) || !unrestricted.Take(mesh) || mesh.ordinaryVictim) continue;
            ++protection.meshCount;
            protection.logicalBytes += mesh.bytes;
            protection.physicalBytes += mesh.physicalBytes;
            if (protection.samples.size() < 16) protection.samples.push_back(mesh);
        }
    }
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
        [&](const auto& mesh) { return !mesh.ordinaryVictim || held.count(mesh.key); }), candidates.end());
    return protection;
}

NRIVoxelRecurringPoseGrace NRIKeepRecurringPoseForSweep(std::vector<NRIVoxelSweepCandidate>& candidates,
    uint64_t inactive, uint64_t warm, const NRIVoxelRetentionPressure& policy, uint64_t hardDeficit,
    uint64_t availableCharge, uint32_t frame, uint32_t interval)
{
    NRIVoxelRecurringPoseGrace grace;
    // This is a share of the current warm allowance, inside the family staging charge.
    const uint64_t limit = std::min(availableCharge, policy.warmLogicalTarget / 8);
    if (!limit) return grace;
    SweepForecast forecast(inactive, warm, -1, policy);
    for (auto& mesh : candidates) mesh.ordinaryVictim = forecast.Take(mesh);
    for (const auto& mesh : candidates)
    {
        if (!mesh.ordinaryVictim || !mesh.physicalBytes || mesh.bytes > limit || mesh.lastRequestedFrame == UINT32_MAX || mesh.lastRequestedFrame > frame ||
            frame - mesh.lastRequestedFrame >= interval || forecast.retiring - mesh.physicalBytes < hardDeficit) continue;
        // Prefer the latest current-owner request, then the smaller retained charge and a stable key.
        if (grace.mesh && std::make_tuple(frame - mesh.lastRequestedFrame, mesh.bytes, mesh.key) >=
            std::make_tuple(frame - grace.requestedFrame, grace.bytes, grace.mesh)) continue;
        grace = {mesh.key, mesh.bytes, forecast.retiring - mesh.physicalBytes, mesh.lastRequestedFrame};
    }
    if (grace.mesh)
    {
        // Only the original retirements can proceed. An unrelated mesh cannot become a substitute.
        candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [&](const auto& mesh) {
            return !mesh.ordinaryVictim || mesh.key == grace.mesh;
        }), candidates.end());
    }
    return grace;
}
