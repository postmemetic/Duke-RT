#include "nri_persistent_voxels.h"
#include "actor_presentation_snapshot.h"
#include "c_cvars.h"
#include "nri_voxel_compute_preload.h"
#include "nri_voxel_preload_budget.h"
#include "../scene/nri_voxel_material_slots.h"
#include "perf_audit.h"
#include <algorithm>
#include <cstdio>
#include <unordered_set>

EXTERN_CVAR(Bool, nri_ptenemystablerepresentation)
EXTERN_CVAR(Bool, nri_ptenemyrepresentationtrace)
EXTERN_CVAR(Int, nri_ptvoxelallocationreservemb)
EXTERN_CVAR(Bool, nri_ptvoxelstartupfamilybudget)
EXTERN_CVAR(Bool, nri_ptvoxelallocationbudget)
EXTERN_CVAR(Int, nri_ptvoxelcomputepreloadminreservemb)

namespace
{
uint64_t Add(uint64_t a, uint64_t b) { return b > UINT64_MAX - a ? UINT64_MAX : a + b; }

bool GetStageOwner(const ActorPresentationState& actor, uint64_t world, const NRIEnemyRepresentationPolicy& policy,
    NRIEnemyStageOwner& result)
{
    const NRIEnemyRepresentationOwner owner{actor.owner.ownerWorldEpoch, uint64_t(actor.owner.ownerLifetimeGeneration)};
    const auto& family = actor.spriteAnimation.enemyVoxelFamily;
    if (!actor.owner.IsValid() || owner.world != world || !actor.authorityCurrent || !actor.spriteAnimation.enemy ||
        !family || !family->complete) return false;
    const auto* choice = policy.Find(owner);
    if (choice && (!choice->MayPromote() || !family->CoveredBy(choice->familySignature, choice->successorSignature))) return false;
    result = {owner, family->signature, choice != nullptr};
    return true;
}

bool IsSpawnOnlyPose(const nri_scene::PrecachedVoxelVariantView& pose)
{
    return (pose.sourceBits & nri_scene::PrecachedVoxelSourceBits_EnemyFamilies) ==
        nri_scene::PrecachedVoxelSourceBit_SpawnableEnemyFamily;
}

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
        NRIEnemyStageOwner item;
        if (GetStageOwner(actor, snapshot.worldEpoch, enemyPolicy, item)) live.push_back(item);
        if (live.size() > NRIEnemyFamilyStaging::MaximumOwners) break;
    }
    const uint64_t oldWorld = enemyFamilyStaging.Stats().world;
    enemyFamilyStaging.Reconcile(snapshot.worldEpoch, live);
    if (oldWorld != snapshot.worldEpoch)
    {
        enemyStagingFrame = enemyRestageAttemptFrame = enemyPromotionScanFrame = UINT32_MAX;
        enemyPromotionHeadroom = 0;
    }
}

// Reconstruct only certified poses of a previously seen enemy. Test the whole
// remaining family's footprint before granting one ordinary, fenced admission.
void NRIPersistentVoxelResidency::RequestEnemyPromotionGeometry(const ActorPresentationState& actor, uint32_t frame,
    const NRIPersistentVoxelSettings& settings, uint64_t available, const NRIVoxelComputePreloadSettings& preload,
    const NRIPersistentVoxelResetServices& services)
{
    const auto& family = *actor.spriteAnimation.enemyVoxelFamily;
    std::vector<nri_scene::PrecachedVoxelRawManifestView> poses;
    if (!nri_scene::BuildEnemyVoxelFamilyViews(actor, poses)) return;
    uint64_t geometry = 0, largest = 0, sources = 0;
    uint64_t nextPair = 0;
    uint64_t nextMesh = 0;
    int nextTile = -1;
    nri_scene::PrecachedVoxelVariantView next;
    std::unordered_set<uint64_t> unique;
    for (const auto& pose : poses)
    {
        auto variant = NRIBuildEnemyPoseAdmission(pose);
        const auto mesh = BuildPersistentVoxelVariantMeshResourceKey(variant);
        if (!unique.insert(mesh).second) continue;
        const auto found = meshVariantResources.find(mesh);
        if (found != meshVariantResources.end() && IsEnemyFamilyMeshReady(found->second, frame)) continue;
        if (pose.primitiveCount > settings.admitMaxBlasPrimitives) { enemyFamilyStaging.PermitPromotion(family.signature, 0); return; }
        const uint64_t bytes = uint64_t(pose.primitiveCount) * (2 * sizeof(nri_scene::SceneVertex) +
            3 * sizeof(uint32_t) + sizeof(nri_scene::PrimitiveData));
        geometry = Add(geometry, bytes); largest = std::max(largest, bytes); sources = Add(sources, pose.rawBytes);
        if (!nextPair)
        {
            nextPair = nri_scene::HashCombine64(mesh, variant.materialKeyHash);
            nextMesh = mesh;
            nextTile = pose.sourceBuildTile;
            next = std::move(variant);
        }
    }
    const auto materials = uint64_t(enemyFamilyStaging.OwnerCount(family.signature)) * poses.size() *
        nri_scene::VoxelPaletteMaterialSlotCount * sizeof(nri_scene::MaterialData);
    const auto peak = Add(EstimateNRIVoxelPreloadPeak(geometry, 0, sources, largest, preload.peakEstimatePercent, preload.strict), materials);
    if (!nextPair || peak > available) { enemyFamilyStaging.PermitPromotion(family.signature, 0); return; }
    const uint64_t prior = enemyFamilyStaging.PromotionPair(family.signature);
    if (prior)
    {
        const auto active = admissionQueue.find(prior);
        if (active != admissionQueue.end() && active->second.mapGeneration == residencyMapGeneration &&
            BuildPersistentVoxelVariantMeshResourceKey(active->second.variant) == nextMesh &&
            active->second.state != PersistentVoxelAdmissionState::Ready)
        {
            if (active->second.state != PersistentVoxelAdmissionState::Failed || (active->second.firstQueuedFrame != UINT32_MAX &&
                frame >= active->second.firstQueuedFrame && frame - active->second.firstQueuedFrame < 60)) return;
        }
    }
    if (!EnqueueAdmission(next, NRIVoxelAdmissionOrigin::EnemyPromotion, residencyLastBuildSerial, settings, 0, false, services)) return;
    enemyFamilyStaging.PermitPromotion(family.signature, nextPair);
    if (PerfAuditActive())
    {
        char fields[320];
        std::snprintf(fields, sizeof(fields), "\"renderer_frame\":%u,\"family\":%u,\"tile\":%d,\"pair\":%llu,"
            "\"peak_bytes\":%llu,\"available_bytes\":%llu", frame, family.id, nextTile, (unsigned long long)nextPair,
            (unsigned long long)peak, (unsigned long long)available);
        PerfAuditWriteEvent("enemy_promotion_pose", PerfAuditCurrentToken(), fields);
    }
}

// Restore only map-planned geometry, using exactly the route, material and
// admission rank used by loading. A missing GPU resource never becomes a pin.
bool NRIPersistentVoxelResidency::TryRestageEnemyGeometry(const ActorPresentationState& actor, uint32_t frame,
    const NRIPersistentVoxelSettings& settings, uint64_t available, const NRIPersistentVoxelResetServices& services)
{
    std::vector<nri_scene::PrecachedVoxelRawManifestView> views;
    if (!nri_scene::BuildEnemyVoxelFamilyViews(actor, views)) return false;
    const uint64_t signature = actor.spriteAnimation.enemyVoxelFamily->signature;
    std::vector<const nri_scene::PrecachedVoxelVariantView*> missing;
    uint64_t bytes = 0, largest = 0, sources = 0;
    std::unordered_set<uint64_t> seen;
    for (const auto& view : views)
    {
        nri_scene::PrecachedVoxelVariantView identity;
        identity.meshKeyHash = view.meshKeyHash;
        identity.geometryContentHash = view.geometryContentHash;
        identity.renderPrimitiveHash = view.renderPrimitiveHash;
        const auto key = BuildPersistentVoxelVariantMeshResourceKey(identity);
        if (!seen.insert(key).second || meshVariantResources.count(key)) continue;
        const auto* planned = NRIFindPlannedEnemyGeometry(residencyLastBuildSerial, key);
        if (!planned) return false;
        const uint64_t size = uint64_t(planned->primitiveCount) * (2 * sizeof(nri_scene::SceneVertex) +
            3 * sizeof(uint32_t) + sizeof(nri_scene::PrimitiveData));
        bytes = Add(bytes, size);
        sources = Add(sources, view.rawBytes);
        largest = std::max(largest, size);
        const auto pair = nri_scene::HashCombine64(key, planned->materialKeyHash);
        const auto entry = admissionQueue.find(pair);
        if (entry != admissionQueue.end() && entry->second.mapGeneration == residencyMapGeneration &&
            entry->second.state != PersistentVoxelAdmissionState::Ready &&
            !(IsSpawnOnlyPose(*planned) && !enemyFamilyStaging.RequestedSpawnPose(signature, pair) &&
              IsUnstartedOptionalAdmission(entry->second, IsRequiredAdmission(entry->second)))) continue;
        missing.push_back(planned);
    }
    if (missing.empty()) return false;
    const auto preload = BuildNRIVoxelComputePreloadSettingsFromCVars();
    const auto peak = EstimateNRIVoxelPreloadPeak(bytes, 0, sources, largest, preload.peakEstimatePercent, preload.strict);
    if (!preload.enabled || preload.dryRun || peak > available) return false;
    uint32_t queued = 0;
    uint32_t liveSpawnPoses = 0;
    for (const auto* planned : missing)
    {
        if (!EnqueueAdmission(*planned, NRIVoxelAdmissionOrigin::Preload, residencyLastBuildSerial, settings, 0, false, services)) continue;
        ++queued;
        if (IsSpawnOnlyPose(*planned))
        {
            const auto mesh = BuildPersistentVoxelVariantMeshResourceKey(*planned);
            if (enemyFamilyStaging.RequestSpawnPose(signature, nri_scene::HashCombine64(mesh, planned->materialKeyHash)))
                ++liveSpawnPoses;
        }
    }
    if (PerfAuditActive())
    {
        char fields[320];
        std::snprintf(fields, sizeof(fields), "\"renderer_frame\":%u,\"family\":%u,\"requested_poses\":%zu,\"queued\":%u,"
            "\"live_spawn_poses\":%u,\"peak_bytes\":%llu,\"available_bytes\":%llu", frame, actor.spriteAnimation.enemyVoxelFamily->id,
            missing.size(), queued, liveSpawnPoses, (unsigned long long)peak, (unsigned long long)available);
        PerfAuditWriteEvent("enemy_family_restage", PerfAuditCurrentToken(), fields);
    }
    return queued != 0;
}

void NRIPersistentVoxelResidency::UpdateEnemyFamilyStaging(uint32_t frame,
    const NRIPersistentVoxelSettings& settings, uint64_t usageBytes, uint64_t budgetBytes,
    const NRIPersistentVoxelResetServices& services)
{
    enemyPreparesProspectiveMaterials = false;
    enemyPreparationHeadroom = 0;
    if (!(bool)nri_ptenemystablerepresentation || settings.transformKeyed)
    { enemyFamilyStaging.Reset(); enemyStagingFrame = UINT32_MAX; return; }
    if (retentionQuarantined) return;
    const auto& snapshot = GetActorPresentationSnapshot();
    if (!snapshot.complete || !snapshot.worldEpoch || !budgetBytes) return;
    std::vector<NRIEnemyStageFamily> families;
    std::vector<uint64_t> readyKeys;
    std::unordered_set<uint64_t> signatures;
    const uint64_t margin = std::max<uint64_t>(budgetBytes / 64, 16ull * 1024 * 1024);
    const uint64_t reserve = Add(uint64_t(std::max(0, (int)nri_ptvoxelallocationreservemb)) * 1024 * 1024, margin);
    const uint64_t freeAfterReserve = budgetBytes > Add(usageBytes, reserve) ? budgetBytes - usageBytes - reserve : 0;
    enemyPreparesProspectiveMaterials = freeAfterReserve != 0 && NRICompleteEnemyPreloadFits(residencyLastBuildSerial, budgetBytes, usageBytes, reserve);
    if (enemyPreparesProspectiveMaterials) enemyPreparationHeadroom = freeAfterReserve;
    bool tryRestage = freeAfterReserve && (enemyRestageAttemptFrame == UINT32_MAX || frame < enemyRestageAttemptFrame ||
        frame - enemyRestageAttemptFrame >= 60);
    std::vector<const ActorPresentationState*> retry;
    bool hasPromotion = false;
    // Examine each distinct certified family before ranking them by measured cost.
    // A loaded level can contain more families than the constrained retention cap.
    for (const auto& actor : snapshot.actors)
    {
        const auto& certificate = actor.spriteAnimation.enemyVoxelFamily;
        NRIEnemyStageOwner item;
        if (!GetStageOwner(actor, snapshot.worldEpoch, enemyPolicy, item) || !enemyFamilyStaging.NeedsFamily(certificate->signature) ||
            !signatures.insert(certificate->signature).second) continue;
        if (signatures.size() > NRIEnemyFamilyStaging::MaximumPreparedFamilies) break;
        const auto& resident = InspectEnemyFamily(actor, frame, settings);
        const bool requested = enemyFamilyStaging.NeedsPromotion(certificate->signature);
        hasPromotion |= requested;
        if (requested && !resident.complete) retry.push_back(&actor);
        if (!requested && !resident.complete && tryRestage &&
            !resident.recoveryPricesKnown && !(IsEnemyFamilyRecoveryEnabled() && enemyFamilyStaging.WasPriced(certificate->signature)) &&
            (enemyPreparesProspectiveMaterials || enemyFamilyStaging.WasStaged(certificate->signature)) &&
            TryRestageEnemyGeometry(actor, frame, settings, freeAfterReserve, services))
        {
            enemyRestageAttemptFrame = frame;
            tryRestage = false;
        }
        NRIEnemyStageFamily family;
        family.signature = certificate->signature; family.id = certificate->id;
        family.poses = uint32_t(certificate->textureIds.size()); family.complete = resident.complete;
        family.promotionRequested = requested;
        for (uint64_t key : resident.meshes) family.meshes.push_back({key});
        families.push_back(std::move(family));
    }
    // Sample the adapter only while a real sprite commitment needs preparation.
    // The allocation owner still verifies the current budget before every GPU allocation.
    constexpr uint32_t PromotionInterval = 4;
    const auto promotionReserve = std::max<uint64_t>(reserve, uint64_t(std::max(0, (int)nri_ptvoxelcomputepreloadminreservemb)) * 1024 * 1024);
    const bool scan = enemyPromotionScanFrame == UINT32_MAX || frame < enemyPromotionScanFrame ||
        frame - enemyPromotionScanFrame >= (enemyPromotionHeadroom ? PromotionInterval : 60u);
    if (!hasPromotion || budgetBytes <= Add(usageBytes, promotionReserve) || !services.allocationBudget || !(bool)nri_ptvoxelallocationbudget)
    {
        enemyPromotionHeadroom = 0;
        enemyFamilyStaging.StopPromotions();
    }
    else if (scan)
    {
        enemyPromotionScanFrame = frame;
        const auto preload = BuildNRIVoxelComputePreloadSettingsFromCVars();
        uint64_t liveUsage = 0, liveBudget = 0;
        services.allocationBudget(services.user, liveUsage, liveBudget);
        const uint64_t limit = std::min(budgetBytes, liveBudget);
        const uint64_t occupied = Add(std::max(usageBytes, liveUsage), promotionReserve);
        enemyPromotionHeadroom = limit > occupied ? limit - occupied : 0;
        if (!enemyPromotionHeadroom) enemyFamilyStaging.StopPromotions();
        else if (!retry.empty())
        {
            const auto* actor = retry[(frame / PromotionInterval) % retry.size()];
            RequestEnemyPromotionGeometry(*actor, frame, settings, enemyPromotionHeadroom, preload, services);
        }
    }
    enemyPreparationHeadroom = std::max(enemyPreparationHeadroom, std::min(freeAfterReserve, enemyPromotionHeadroom));
    for (auto& family : families) family.promotionRequested &= enemyPromotionHeadroom != 0;
    std::sort(families.begin(), families.end(), [](const auto& a, const auto& b) { return a.signature < b.signature; });
    for (const auto& family : families) if (family.complete)
    {
        readyKeys.push_back(family.signature); readyKeys.push_back(family.meshes.size()); readyKeys.push_back(family.promotionRequested);
        for (const auto& mesh : family.meshes) readyKeys.push_back(mesh.key);
    }
    // Prospective families rank below current/committed ownership, but above
    // the ordinary cache's preferred warm-headroom target. Use the same hard
    // safety reserve as allocation admission, plus the existing pressure margin.
    // This does not change either cache maintenance or allocation admission.
    if (tryRestage) enemyRestageAttemptFrame = frame;
    const bool reserveRecovered = enemyFamilyStaging.Stats().requiredReclaimBytes != 0 &&
        budgetBytes >= reserve && usageBytes <= budgetBytes - reserve;
    constexpr uint64_t GrowthRefresh = 16ull * 1024 * 1024;
    if (enemyStagingFrame != UINT32_MAX && frame >= enemyStagingFrame && frame - enemyStagingFrame < 60 &&
        readyKeys == enemyStagingReadyKeys && budgetBytes >= enemyStagingBudget &&
        usageBytes <= Add(enemyStagingUsage, GrowthRefresh) && !reserveRecovered) return;
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
    const bool hasPricedBacking = (bool)nri_ptvoxelstartupfamilybudget && startupFamilyBudget.build &&
        startupFamilyBudget.build == residencyLastBuildSerial;
    for (auto& family : families)
    {
        family.startupPriced = hasPricedBacking && family.complete;
        for (auto& mesh : family.meshes)
        {
            family.startupPriced = family.startupPriced && startupFamilyBudget.meshes.count(mesh.key);
            const auto found = costs.find(mesh.key);
            if (found == costs.end()) family.complete = false;
            else mesh = found->second;
        }
    }
    enemyFamilyStaging.Select(families, {usageBytes, budgetBytes, reserve, coldPhysical, enemyPreparesProspectiveMaterials,
        hasPricedBacking ? startupFamilyBudget.StageCharge() : 0, hasPricedBacking ? startupFamilyBudget.largestScratchBytes : 0,
        uint64_t(nri_scene::VoxelPaletteMaterialSlotCount) * sizeof(nri_scene::MaterialData), enemyPromotionHeadroom});
    // No allocation or immediate freed-memory claim follows this selection.
    // Ordinary retirement and allocation admission retain their fence/budget gates.
    if ((bool)nri_ptenemyrepresentationtrace && PerfAuditActive())
    {
        const auto& state = enemyFamilyStaging.Stats();
        char fields[896];
        std::snprintf(fields, sizeof(fields), "\"renderer_frame\":%u,\"world\":%llu,\"families\":%u,\"owners\":%u,\"meshes\":%u,"
            "\"geometry_bytes\":%llu,\"physical_bytes\":%llu,\"charged_bytes\":%llu,\"limit_bytes\":%llu,\"usage_bytes\":%llu,"
            "\"budget_bytes\":%llu,\"reserve_bytes\":%llu,\"required_reclaim_bytes\":%llu,\"other_cold_physical_bytes\":%llu,"
            "\"cap_refusals_total\":%llu,\"pressure_drops_total\":%llu,\"complete_plan_fits\":%s,\"preparation_headroom\":%llu",
            frame, (unsigned long long)state.world, state.families, state.owners, state.meshes,
            (unsigned long long)state.geometryBytes, (unsigned long long)state.physicalBytes, (unsigned long long)state.chargedBytes,
            (unsigned long long)state.limitBytes, (unsigned long long)usageBytes, (unsigned long long)budgetBytes,
            (unsigned long long)reserve, (unsigned long long)state.requiredReclaimBytes,
            (unsigned long long)state.otherColdPhysicalBytes, (unsigned long long)state.capRefusalsTotal, (unsigned long long)state.pressureDropsTotal,
            enemyPreparesProspectiveMaterials ? "true" : "false", (unsigned long long)enemyPreparationHeadroom);
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
