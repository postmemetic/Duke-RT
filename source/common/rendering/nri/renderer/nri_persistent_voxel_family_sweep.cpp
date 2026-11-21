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
    const auto interval = uint32_t(std::max(1, (int)nri_ptvoxelcacheintervalframes));
    const bool adjacentSweep = !lastProtectedFamilySweep.signatures.empty() &&
        lastProtectedFamilySweep.build == residencyLastBuildSerial && frameIndex > lastProtectedFamilySweep.frame &&
        frameIndex - lastProtectedFamilySweep.frame <= interval;
    const auto previousWorld = lastProtectedFamilySweep.world;
    lastProtectedFamilySweep.signatures.clear();
    if (!IsEnemyFamilyRecoveryEnabled() || enemyFamilyRecovery.signature || enemyFamilyFrame != frameIndex) return;
    const auto& snapshot = GetActorPresentationSnapshot();
    const auto& stage = enemyFamilyStaging.Stats();
    if (!snapshot.complete || !snapshot.worldEpoch || stage.world != snapshot.worldEpoch) return;
    lastProtectedFamilySweep.frame = frameIndex;
    lastProtectedFamilySweep.world = stage.world;
    lastProtectedFamilySweep.build = residencyLastBuildSerial;

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
    bool familyRefused = false;

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
        if (!keep) { familyRefused = true; continue; }
        lastProtectedFamilySweep.signatures.push_back(choice.signature);
        ++familyCount;
        meshCount += uint32_t(proof.additionalKeys.size());
        remainingCharge -= proof.additionalBytes;
        inactiveBytes -= proof.additionalBytes;
        warmBytes -= proof.additionalWarmBytes;
        held.insert(proof.additionalKeys.begin(), proof.additionalKeys.end());
    }
    const bool audit = PerfAuditActive();
    const bool allowGrace = (bool)nri_ptvoxelfamilyposegrace && !familyRefused && meshLimit < 0 &&
        meshCount < NRIEnemyFamilyStaging::MaximumMeshes &&
        (!held.empty() || (adjacentSweep && stage.world == previousWorld));
    if (held.empty() && !allowGrace) return;
    if (!ordinaryMarked)
        NRIMarkOrdinarySweepVictims(candidates, inactiveBytes, warmBytes, meshLimit, warmRetentionPressure);
    TraceOrdinarySweepVictims(frameIndex, candidates, held);
    // A grace on the next pressure sweep cannot renew itself. Both sweeps can retain
    // only from the actual victim list, and the other victims must pay the hard reserve.
    NRIVoxelSweepProtection protection;
    if (!held.empty())
        protection = NRIRestrictFamilySweep(candidates, held, inactiveBytes, warmBytes, meshLimit, warmRetentionPressure, audit);
    if (allowGrace)
    {
        const auto requests = actorDemand.RecentRecurringPoseRequests(frameIndex, interval);
        for (auto& mesh : candidates)
        {
            const auto found = requests.find(mesh.key);
            if (found != requests.end()) mesh.lastRequestedFrame = found->second;
        }
        const auto grace = NRIKeepRecurringPoseForSweep(candidates, inactiveBytes, warmBytes, warmRetentionPressure,
            hardDeficit, remainingCharge, frameIndex, interval);
        if (grace.mesh && audit)
        {
            char row[384];
            std::snprintf(row, sizeof(row), "\"renderer_frame\":%u,\"world\":%llu,\"mesh_key\":%llu,\"last_requested_frame\":%u,\"adjacent_sweep\":%s,"
                "\"logical_bytes\":%llu,\"hard_deficit\":%llu,\"other_victim_bytes\":%llu", frameIndex,
                (unsigned long long)stage.world, (unsigned long long)grace.mesh, grace.requestedFrame, held.empty() ? "true" : "false",
                (unsigned long long)grace.bytes, (unsigned long long)hardDeficit, (unsigned long long)grace.otherRetiring);
            PerfAuditWriteEvent("enemy_family_sweep_pose_grace", PerfAuditCurrentToken(), row);
        }
    }
    if (!audit || held.empty()) return;
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

uint64_t NRIPersistentVoxelResidency::FindHeldUpgradeFamilyVictim(const NRIVoxelReclaimRequest& request,
    const NRIVoxelReclaimPlan& plan, uint32_t frame) const
{
    if (!request.freshActorPair || request.beforeGrowth || lastProtectedFamilySweep.signatures.empty() ||
        !IsEnemyFamilyRecoveryEnabled() || lastProtectedFamilySweep.build != residencyLastBuildSerial ||
        enemyFamilyFrame != frame || frame < lastProtectedFamilySweep.frame ||
        frame - lastProtectedFamilySweep.frame >= uint32_t(std::max(1, (int)nri_ptvoxelcacheintervalframes))) return 0;
    const auto& snapshot = GetActorPresentationSnapshot();
    if (!snapshot.complete || snapshot.worldEpoch != lastProtectedFamilySweep.world ||
        !actorDemand.HasCurrentMeshAuthority(frame)) return 0;
    const auto demand = actorDemand.Upgrade(request.mesh, frame);
    if (demand.consumers != 1 || demand.requestedConsumers != 1 || demand.oldestWait > 1) return 0;
    const auto body = actorDemand.GetActor(demand.identity, demand.world, demand.lifetime, frame);
    if (!body.found || body.desiredMesh != request.mesh || body.representation != NRIVoxelActorRepresentation::HeldVoxel ||
        !body.selectedMesh || !body.selectedMaterial) return 0;

    for (const auto signature : lastProtectedFamilySweep.signatures)
    {
        const auto family = enemyFamilyResidency.find(signature);
        if (family == enemyFamilyResidency.end() || !family->second.complete || !enemyFamilyStaging.WasPriced(signature) ||
            !enemyFamilyStaging.NeedsFamily(signature)) continue;
        for (const auto& victim : plan.victims)
        {
            if (!NRIEnemyFamilyContainsCurrentMesh(family->second.meshes, victim.mesh)) continue;
            // A later upgrade or preload may have adopted the same geometry
            // between the notice and the live victim plan.
            if (admissionIndex.ActiveCount() > NRIVoxelAdmissionReclaim::MaxOwnerRows) return 0;
            for (const auto key : admissionIndex.ActiveKeys())
            {
                const auto found = admissionQueue.find(key);
                if (found == admissionQueue.end() || found->second.mapGeneration != residencyMapGeneration) continue;
                const auto& entry = found->second;
                if (BuildPersistentVoxelVariantMeshResourceKey(entry.variant) != request.mesh) continue;
                if (key != request.freshActorPair || entry.admissionOrigins != uint8_t(NRIVoxelAdmissionOrigin::RuntimeActor) ||
                    entry.sourceBits || entry.gpuForce || entry.gpuPrefer || entry.requiredPreload || entry.preloadInputHints.bits ||
                    !CanDiscardUnsubmittedVoxelAdmission(entry, admissionScheduler)) return 0;
            }
            return victim.mesh;
        }
    }
    return 0;
}
