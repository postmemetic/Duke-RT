#include "nri_persistent_voxels.h"
#include "nri_cvars.h"
#include "actor_presentation_snapshot.h"
#include "perf_audit.h"
#include <algorithm>
#include <cstdio>

void NRIPersistentVoxelResidency::RetainCompleteFamiliesForSweep(uint32_t frameIndex,
    uint64_t usageBytes, uint64_t budgetBytes, int32_t meshLimit,
    std::vector<NRIVoxelSweepCandidate>& candidates, uint64_t& inactiveBytes, uint64_t& warmBytes)
{
    if (!IsEnemyFamilyRecoveryEnabled() || enemyFamilyRecovery.signature || enemyFamilyFrame != frameIndex) return;
    const auto& snapshot = GetActorPresentationSnapshot();
    const auto& stage = enemyFamilyStaging.Stats();
    if (!snapshot.complete || !snapshot.worldEpoch || stage.world != snapshot.worldEpoch) return;

    const uint64_t reserve = uint64_t(std::max(0, (int)nri_ptvoxelallocationreservemb)) * 1024 * 1024 +
        std::max<uint64_t>(budgetBytes / 64, 16ull * 1024 * 1024);
    // An unknown or wholly consumed budget cannot support discretionary retention.
    if (budgetBytes <= reserve) return;
    const uint64_t hardDeficit = NRIVoxelBudgetPressure(usageBytes, budgetBytes, reserve);
    const uint64_t limit = std::min({stage.limitBytes, startupFamilyBudget.StageCharge(), budgetBytes / 2});
    uint64_t remainingCharge = limit - std::min(limit, stage.chargedBytes);
    uint32_t familyCount = stage.families, meshCount = stage.meshes;
    std::unordered_set<uint64_t> held;
    bool ordinaryMarked = false;

    // Decisions retain the normal staging priority. Earlier selections and all active, required
    // or pending owners were excluded from this sweep's candidate list upstream.
    for (const auto& choice : enemyFamilyStaging.Decisions())
    {
        if (!choice.reserveLimited || !enemyFamilyStaging.WasStaged(choice.signature) ||
            !enemyFamilyStaging.NeedsFamily(choice.signature)) continue;
        const auto found = enemyFamilyResidency.find(choice.signature);
        if (found == enemyFamilyResidency.end() || !found->second.complete) continue;
        const auto& family = found->second;
        if (family.poses > NRIEnemyFamilyStaging::MaximumFamilyPoses ||
            !std::all_of(family.meshes.begin(), family.meshes.end(),
                [&](uint64_t key) { return startupFamilyBudget.meshes.count(key) != 0; })) continue;

        if (!ordinaryMarked)
        {
            NRIMarkOrdinarySweepVictims(candidates, inactiveBytes, warmBytes, meshLimit, warmRetentionPressure);
            ordinaryMarked = true;
        }
        const auto proof = NRIPriceFamilySweep(candidates, held, family.meshes,
            inactiveBytes, warmBytes, meshLimit, warmRetentionPressure);
        if (proof.additionalKeys.empty()) continue;
        const uint32_t remainingMeshes = NRIEnemyFamilyStaging::MaximumMeshes -
            std::min(meshCount, NRIEnemyFamilyStaging::MaximumMeshes);
        const bool keep = familyCount < NRIEnemyFamilyStaging::MaximumFamilies &&
            proof.Fits(hardDeficit, remainingCharge, remainingMeshes);
        if (PerfAuditActive())
        {
            char fields[512];
            std::snprintf(fields, sizeof(fields), "\"renderer_frame\":%u,\"world\":%llu,\"family\":%u,"
                "\"signature\":%llu,\"poses\":%u,\"additional_meshes\":%u,\"additional_bytes\":%llu,"
                "\"ordinary_family_victims\":%u,\"remaining_charge\":%llu,\"hard_deficit\":%llu,"
                "\"other_victim_bytes\":%llu,\"keep\":%s",
                frameIndex, (unsigned long long)stage.world, family.family, (unsigned long long)choice.signature,
                family.poses, (unsigned)proof.additionalKeys.size(), (unsigned long long)proof.additionalBytes,
                proof.ordinaryFamilyVictims, (unsigned long long)remainingCharge, (unsigned long long)hardDeficit,
                (unsigned long long)proof.otherRetiringBytes, keep ? "true" : "false");
            PerfAuditWriteEvent("enemy_family_sweep", PerfAuditCurrentToken(), fields);
        }
        if (!keep) continue;
        ++familyCount;
        meshCount += uint32_t(proof.additionalKeys.size());
        remainingCharge -= proof.additionalBytes;
        inactiveBytes -= proof.additionalBytes;
        warmBytes -= proof.additionalWarmBytes;
        held.insert(proof.additionalKeys.begin(), proof.additionalKeys.end());
    }
    if (held.empty()) return;
    // The proof chooses this sweep's victims only. No retired bytes become credit for an
    // allocation until the backend's ordinary fence and live-budget checks allow it.
    const bool audit = PerfAuditActive();
    const auto protection = NRIRestrictFamilySweep(candidates, held, inactiveBytes, warmBytes, meshLimit,
        warmRetentionPressure, audit);
    if (!audit) return;
    char fields[384];
    std::snprintf(fields, sizeof(fields), "\"renderer_frame\":%u,\"world\":%llu,\"additional_protected_meshes\":%u,"
        "\"additional_protected_logical_bytes\":%llu,\"additional_protected_physical_bytes\":%llu,"
        "\"sampled_meshes\":%u", frameIndex, (unsigned long long)stage.world, protection.meshCount,
        (unsigned long long)protection.logicalBytes, (unsigned long long)protection.physicalBytes,
        (unsigned)protection.samples.size());
    PerfAuditWriteEvent("enemy_family_sweep_protection", PerfAuditCurrentToken(), fields);
    for (const auto& mesh : protection.samples)
    {
        std::snprintf(fields, sizeof(fields), "\"renderer_frame\":%u,\"world\":%llu,\"mesh_key\":\"%llu\","
            "\"warm_rank\":%u,\"inactive_since\":%u,\"logical_bytes\":%llu,\"physical_bytes\":%llu",
            frameIndex, (unsigned long long)stage.world, (unsigned long long)mesh.key, mesh.warmRank,
            mesh.since, (unsigned long long)mesh.bytes, (unsigned long long)mesh.physicalBytes);
        PerfAuditWriteEvent("enemy_family_sweep_spared", PerfAuditCurrentToken(), fields);
    }
}
