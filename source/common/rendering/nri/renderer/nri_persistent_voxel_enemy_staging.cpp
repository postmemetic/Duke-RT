#include "nri_persistent_voxels.h"
#include "actor_presentation_snapshot.h"
#include "c_cvars.h"
#include "perf_audit.h"
#include <algorithm>
#include <cstdio>
#include <unordered_set>

EXTERN_CVAR(Bool, nri_ptenemystablerepresentation)
EXTERN_CVAR(Bool, nri_ptenemyrepresentationtrace)

namespace
{
uint64_t Add(uint64_t a, uint64_t b) { return b > UINT64_MAX - a ? UINT64_MAX : a + b; }
}

void NRIPersistentVoxelResidency::ReconcileEnemyFamilyStaging()
{
    if (!(bool)nri_ptenemystablerepresentation)
    { enemyFamilyStaging.Reset(); enemyStagingFrame = UINT32_MAX; return; }
    const auto& snapshot = GetActorPresentationSnapshot();
    if (!snapshot.complete) return;
    std::vector<NRIEnemyStageOwner> live;
    live.reserve(NRIEnemyFamilyStaging::MaximumOwners + 1);
    for (const auto& actor : snapshot.actors)
    {
        const auto& family = actor.spriteAnimation.enemyVoxelFamily;
        const NRIEnemyRepresentationOwner owner{actor.owner.ownerWorldEpoch, uint64_t(actor.owner.ownerLifetimeGeneration)};
        if (actor.owner.IsValid() && actor.authorityCurrent && actor.spriteAnimation.enemy && family && family->complete &&
            !enemyPolicy.Find(owner)) live.push_back({owner, family->signature});
        if (live.size() > NRIEnemyFamilyStaging::MaximumOwners) break;
    }
    const uint64_t oldWorld = enemyFamilyStaging.Stats().world;
    enemyFamilyStaging.Reconcile(snapshot.worldEpoch, live);
    if (oldWorld != snapshot.worldEpoch) enemyStagingFrame = UINT32_MAX;
}

void NRIPersistentVoxelResidency::UpdateEnemyFamilyStaging(uint32_t frame,
    const NRIPersistentVoxelSettings& settings, uint64_t usageBytes, uint64_t budgetBytes)
{
    if (!(bool)nri_ptenemystablerepresentation || settings.transformKeyed)
    { enemyFamilyStaging.Reset(); enemyStagingFrame = UINT32_MAX; return; }
    if (retentionQuarantined) return;
    const auto& snapshot = GetActorPresentationSnapshot();
    if (!snapshot.complete || !snapshot.worldEpoch || !budgetBytes) return;
    std::vector<NRIEnemyStageFamily> families;
    std::vector<uint64_t> readyKeys;
    std::unordered_set<uint64_t> signatures;
    // There are exactly eight certified stock families. Inspect their bounded
    // pose lists every pump so a newly complete set is protected before this
    // pump's reclaim/pressure passes, even while another family is staged.
    // InspectEnemyFamily is shared with this frame's texture/selection cache.
    for (const auto& actor : snapshot.actors)
    {
        const auto& certificate = actor.spriteAnimation.enemyVoxelFamily;
        if (!certificate || !certificate->complete || !enemyFamilyStaging.NeedsFamily(certificate->signature) ||
            !signatures.insert(certificate->signature).second) continue;
        if (signatures.size() > NRIEnemyFamilyStaging::MaximumFamilies) break;
        const auto& resident = InspectEnemyFamily(actor, frame, settings);
        NRIEnemyStageFamily family;
        family.signature = certificate->signature; family.id = certificate->id;
        family.poses = uint32_t(certificate->textureIds.size()); family.complete = resident.complete;
        for (uint64_t key : resident.meshes) family.meshes.push_back({key});
        families.push_back(std::move(family));
    }
    std::sort(families.begin(), families.end(), [](const auto& a, const auto& b) { return a.signature < b.signature; });
    for (const auto& family : families) if (family.complete)
    {
        readyKeys.push_back(family.signature); readyKeys.push_back(family.meshes.size());
        for (const auto& mesh : family.meshes) readyKeys.push_back(mesh.key);
    }
    constexpr uint64_t GrowthRefresh = 16ull * 1024 * 1024;
    if (enemyStagingFrame != UINT32_MAX && frame >= enemyStagingFrame && frame - enemyStagingFrame < 60 &&
        readyKeys == enemyStagingReadyKeys && budgetBytes >= enemyStagingBudget &&
        usageBytes <= Add(enemyStagingUsage, GrowthRefresh)) return;
    enemyStagingFrame = frame; enemyStagingUsage = usageBytes; enemyStagingBudget = budgetBytes;
    NRIVoxelCachePins basePins;
    // Current, pending and committed ownership is non-negotiable. Staging alone
    // is excluded while proving which existing physical resources are cold.
    if (!CollectCachePins(settings, basePins, SIZE_MAX, false, false)) return;
    enemyStagingReadyKeys = std::move(readyKeys);
    std::unordered_map<uint64_t, NRIEnemyStageMesh> costs;
    uint64_t coldPhysical = 0;
    for (const auto& item : meshVariantResources)
    {
        const auto& mesh = item.second;
        const auto* shared = sharedBlasCache.Find(item.first);
        const uint64_t physical = mesh.vertexBuffer.memorySize + mesh.indexBuffer.memorySize + mesh.accelerationStructure.memorySize +
            mesh.shadowProxy.vertexBuffer.memorySize + mesh.shadowProxy.indexBuffer.memorySize +
            mesh.shadowProxy.accelerationStructure.memorySize + (shared ? shared->accelerationStructure.memorySize : 0);
        const uint64_t geometry = uint64_t(mesh.vertexCapacity) * sizeof(nri_scene::SceneVertex) +
            uint64_t(mesh.indexCapacity) * sizeof(uint32_t) + uint64_t(mesh.primitiveCapacity) * sizeof(nri_scene::PrimitiveData);
        const uint64_t cold = !basePins.meshes.count(item.first) && !actorDemand.WarmMeshRank(item.first, frame) ? physical : 0;
        costs.emplace(item.first, NRIEnemyStageMesh{item.first, geometry, physical, cold});
        coldPhysical = Add(coldPhysical, cold);
    }
    for (auto& family : families)
    {
        for (auto& mesh : family.meshes)
        {
            const auto found = costs.find(mesh.key);
            if (found == costs.end()) family.complete = false;
            else mesh = found->second;
        }
    }
    const uint64_t margin = std::max<uint64_t>(budgetBytes / 64, 16ull * 1024 * 1024);
    enemyFamilyStaging.Select(families, {usageBytes, budgetBytes, Add(settings.residentMinHeadroomBytes, margin), coldPhysical});
    // No allocation or immediate freed-memory claim follows this selection.
    // Ordinary retirement and allocation admission retain their fence/budget gates.
    if ((bool)nri_ptenemyrepresentationtrace && PerfAuditActive())
    {
        const auto& state = enemyFamilyStaging.Stats();
        char fields[896];
        std::snprintf(fields, sizeof(fields), "\"renderer_frame\":%u,\"world\":%llu,\"families\":%u,\"owners\":%u,\"meshes\":%u,"
            "\"geometry_bytes\":%llu,\"physical_bytes\":%llu,\"charged_bytes\":%llu,\"limit_bytes\":%llu,\"usage_bytes\":%llu,"
            "\"budget_bytes\":%llu,\"reserve_bytes\":%llu,\"required_reclaim_bytes\":%llu,\"other_cold_physical_bytes\":%llu,"
            "\"cap_refusals_total\":%llu,\"pressure_drops_total\":%llu",
            frame, (unsigned long long)state.world, state.families, state.owners, state.meshes,
            (unsigned long long)state.geometryBytes, (unsigned long long)state.physicalBytes, (unsigned long long)state.chargedBytes,
            (unsigned long long)state.limitBytes, (unsigned long long)usageBytes, (unsigned long long)budgetBytes,
            (unsigned long long)Add(settings.residentMinHeadroomBytes, margin), (unsigned long long)state.requiredReclaimBytes,
            (unsigned long long)state.otherColdPhysicalBytes, (unsigned long long)state.capRefusalsTotal, (unsigned long long)state.pressureDropsTotal);
        PerfAuditWriteEvent("enemy_family_staging", PerfAuditCurrentToken(), fields);
        for (const auto& decision : enemyFamilyStaging.Decisions())
        {
            char detail[320];
            std::snprintf(detail, sizeof(detail), "\"renderer_frame\":%u,\"world\":%llu,\"family\":%u,\"signature\":%llu,"
                "\"owners\":%u,\"charged_bytes\":%llu,\"reason\":\"%s\"", frame, (unsigned long long)state.world,
                decision.family, (unsigned long long)decision.signature, decision.owners,
                (unsigned long long)decision.chargedBytes, decision.reason);
            PerfAuditWriteEvent("enemy_family_staging_choice", PerfAuditCurrentToken(), detail);
        }
    }
}
