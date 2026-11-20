#include "nri_persistent_voxels.h"
#include "nri_cvars.h"
#include "nri_voxel_compute_preload.h"
#include "../scene/nri_voxel_material_slots.h"
#include "actor_presentation_snapshot.h"
#include "perf_audit.h"
#include <algorithm>
#include <cstdio>

EXTERN_CVAR(Bool, nri_ptenemystablerepresentation)

namespace
{
constexpr uint32_t ScanInterval = 60, ProgressTimeout = 120;
uint64_t Add(uint64_t a, uint64_t b) { return a > UINT64_MAX - b ? UINT64_MAX : a + b; }

struct RecoveryPrice
{
    uint64_t extraCharge = 0, futureBytes = 0, available = 0, blockedMesh = 0;
    uint32_t additionalMeshes = 0;
};

void Trace(uint32_t frame, const NRIEnemyFamilyRecovery& group, const RecoveryPrice& price, const char* reason)
{
    if (!PerfAuditActive()) return;
    char fields[640];
    std::snprintf(fields, sizeof(fields), "\"renderer_frame\":%u,\"world\":%llu,\"signature\":%llu,\"family\":%u,"
        "\"reason\":\"%s\",\"poses\":%zu,\"resident\":%zu,\"pair\":%llu,\"future_bytes\":%llu,"
        "\"available_bytes\":%llu,\"additional_charge\":%llu,\"blocked_mesh\":%llu",
        frame, (unsigned long long)group.world, (unsigned long long)group.signature, group.family, reason,
        group.poses.size(), group.pins.size(), (unsigned long long)group.permittedPair,
        (unsigned long long)price.futureBytes, (unsigned long long)price.available,
        (unsigned long long)price.extraCharge, (unsigned long long)price.blockedMesh);
    PerfAuditWriteEvent("enemy_family_recovery", PerfAuditCurrentToken(), fields);
}

bool BeginRecovery(NRIPersistentVoxelResidency& voxels, const ActorPresentationState& actor, uint32_t frame,
    const NRIPersistentVoxelResetServices& services, NRIEnemyFamilyRecovery& group)
{
    const auto& certificate = *actor.spriteAnimation.enemyVoxelFamily;
    std::vector<nri_scene::PrecachedVoxelRawManifestView> views;
    if (!services.forecastFamilyProvider || !nri_scene::BuildEnemyVoxelFamilyViews(actor, views) ||
        views.empty() || views.size() > NRIEnemyFamilyStaging::MaximumFamilyPoses) return false;
    group.signature = certificate.signature;
    group.family = certificate.id;
    group.poseCount = uint32_t(views.size());
    group.world = actor.owner.ownerWorldEpoch;
    group.build = voxels.residencyLastBuildSerial;
    group.lastProgressFrame = frame;
    std::unordered_set<uint64_t> seen;
    for (const auto& view : views)
    {
        nri_scene::PrecachedVoxelVariantView id;
        id.meshKeyHash = view.meshKeyHash;
        id.renderPrimitiveHash = view.renderPrimitiveHash;
        id.geometryContentHash = view.geometryContentHash;
        const auto key = BuildPersistentVoxelVariantMeshResourceKey(id);
        if (!seen.insert(key).second) continue;
        const auto* planned = NRIFindPlannedEnemyGeometry(group.build, key);
        if (!key || !voxels.startupFamilyBudget.meshes.count(key) || !planned || !planned->model ||
            !planned->directOnlyAdmission || !planned->primitiveCount || !planned->materialKeyHash) return false;
        uint64_t source = 0, workspace = 0;
        if (!services.forecastFamilyProvider(services.user, planned->model, source, workspace)) return false;
        group.poses.push_back({key, nri_scene::HashCombine64(key, planned->materialKeyHash), source, workspace, planned->primitiveCount});
    }
    // One prospective job at a time. Starting with the largest contiguous
    // request also makes the order in PreviewFamily the actual request order.
    std::sort(group.poses.begin(), group.poses.end(), [](const auto& a, const auto& b)
    {
        return a.primitives != b.primitives ? a.primitives > b.primitives : a.pair < b.pair;
    });
    return !group.poses.empty();
}

const char* PriceRecovery(NRIPersistentVoxelResidency& voxels, NRIEnemyFamilyRecovery& group, uint32_t owners,
    uint64_t usage, uint64_t budget, RecoveryPrice& price)
{
    const auto& stage = voxels.enemyFamilyStaging;
    const uint64_t reserve = uint64_t(std::max(0, (int)nri_ptvoxelallocationreservemb)) * 1024 * 1024 +
        std::max<uint64_t>(budget / 64, 16ull * 1024 * 1024);
    price.available = budget - std::min(budget, Add(usage, reserve));
    uint64_t serialWorkspace = 0;
    std::vector<NRIVoxelFamilyRangeRequest> ranges;
    group.pins.clear();
    group.permittedPair = 0;
    for (const auto& pose : group.poses)
    {
        const auto& native = voxels.startupFamilyBudget.meshes.at(pose.mesh);
        if (!stage.MeshPins().count(pose.mesh))
        {
            ++price.additionalMeshes;
            price.extraCharge = Add(price.extraCharge, Add(native.geometry, native.blas));
        }
        const auto found = voxels.meshVariantResources.find(pose.mesh);
        if (found != voxels.meshVariantResources.end())
        {
            const auto& mesh = found->second;
            if (mesh.meshBakeSpace != nri_scene::VoxelMeshBakeSpace::LocalSpace || !mesh.directComputePublished ||
                !voxels.OwnsGeometryRange(mesh.geometryRange) || !mesh.accelerationStructure.accelerationStructure)
                return "unsupported-resident";
            group.pins.insert(pose.mesh);
            continue;
        }
        NRIPersistentVoxelGeometryRangeHandle owned;
        const auto entry = voxels.admissionQueue.find(pose.pair);
        if (entry != voxels.admissionQueue.end())
        {
            if (entry->second.mapGeneration != voxels.residencyMapGeneration ||
                entry->second.state == PersistentVoxelAdmissionState::Failed) return "queue-unavailable";
            owned = entry->second.uploadMeshResource.geometryRange;
        }
        ranges.push_back({pose.mesh, pose.primitives, owned});
        // Already charged work is conservatively included until publication.
        price.futureBytes = Add(price.futureBytes, Add(native.blas, pose.sourceBytes));
        serialWorkspace = std::max(serialWorkspace, Add(native.scratch, pose.workspaceBytes));
        if (!group.permittedPair) group.permittedPair = pose.pair;
    }
    if (ranges.empty()) return "complete";
    const auto& stats = stage.Stats();
    const auto limit = std::min({stats.limitBytes, voxels.startupFamilyBudget.StageCharge(), budget / 2});
    if (stats.families >= NRIEnemyFamilyStaging::MaximumFamilies ||
        stats.meshes + price.additionalMeshes > NRIEnemyFamilyStaging::MaximumMeshes ||
        price.extraCharge > limit - std::min(limit, stats.chargedBytes)) return "stage-cap";
    const uint64_t materials = uint64_t(owners) * group.poseCount *
        nri_scene::VoxelPaletteMaterialSlotCount * sizeof(nri_scene::MaterialData);
    price.futureBytes = Add(price.futureBytes, Add(materials, serialWorkspace));
    if (!price.available || price.futureBytes > price.available) return "physical-reserve";
    const auto fit = voxels.geometryRanges.PreviewFamily(ranges, {
        voxels.vertexBuffer.size / sizeof(nri_scene::SceneVertex), voxels.indexBuffer.size / sizeof(uint32_t),
        voxels.primitiveBuffer.size / sizeof(nri_scene::PrimitiveData)});
    price.blockedMesh = fit.blockedMesh;
    return fit.fits ? nullptr : "range-layout";
}
}

bool NRIPersistentVoxelResidency::IsEnemyFamilyRecoveryEnabled() const
{
    return (bool)nri_ptvoxelfamilyrecovery && (bool)nri_ptvoxelstartupfamilybudget &&
        (bool)nri_ptvoxelarenastartupinventory && (bool)nri_ptvoxelallocationbudget &&
        (bool)nri_ptenemystablerepresentation && !retentionQuarantined &&
        startupFamilyBudget.build && startupFamilyBudget.build == residencyLastBuildSerial &&
        startupFamilyBudget.blasFlags == GetNRIPersistentVoxelAllocationBlasFlags();
}

bool NRIPersistentVoxelResidency::AllowsEnemyFamilyRecoveryPair(uint64_t pair) const
{
    return pair && enemyFamilyRecovery.permittedPair == pair && IsEnemyFamilyRecoveryEnabled() &&
        enemyFamilyRecovery.build == residencyLastBuildSerial && enemyFamilyStaging.NeedsFamily(enemyFamilyRecovery.signature);
}

void NRIPersistentVoxelResidency::UpdateEnemyFamilyRecovery(uint32_t frame, const NRIPersistentVoxelSettings& settings,
    uint64_t usage, uint64_t budget, const NRIPersistentVoxelResetServices& services)
{
    auto& group = enemyFamilyRecovery;
    const auto& snapshot = GetActorPresentationSnapshot();
    if (!IsEnemyFamilyRecoveryEnabled() || !services.allocationBudget || settings.transformKeyed || !snapshot.complete || !snapshot.worldEpoch ||
        !budget || (group.signature && (group.world != snapshot.worldEpoch || group.build != residencyLastBuildSerial ||
        !enemyFamilyStaging.NeedsFamily(group.signature))))
    {
        if (group.signature) { Trace(frame, group, {}, "authority-or-budget"); group = {}; }
        return;
    }
    const bool opening = !group.signature;
    if (opening)
    {
        if (enemyFamilyRecoveryScan != UINT32_MAX && frame >= enemyFamilyRecoveryScan &&
            frame - enemyFamilyRecoveryScan < ScanInterval) return;
        enemyFamilyRecoveryScan = frame;
        std::unordered_set<uint64_t> seen;
        std::vector<const ActorPresentationState*> candidates;
        for (const auto& actor : snapshot.actors)
        {
            const auto& family = actor.spriteAnimation.enemyVoxelFamily;
            if (!family || !family->complete || !actor.owner.IsValid() || !actor.authorityCurrent ||
                !actor.spriteAnimation.enemy || actor.owner.ownerWorldEpoch != snapshot.worldEpoch ||
                enemyPolicy.Find({actor.owner.ownerWorldEpoch, uint64_t(actor.owner.ownerLifetimeGeneration)}) ||
                !enemyFamilyStaging.WasPriced(family->signature) || !enemyFamilyStaging.NeedsFamily(family->signature) ||
                !seen.insert(family->signature).second || InspectEnemyFamily(actor, frame, settings).complete) continue;
            candidates.push_back(&actor);
        }
        if (candidates.empty()) return;
        // Bound native forecasts to one family per scan. Rotate so a family
        // with an impossible range layout cannot monopolize later scans.
        const auto* actor = candidates[(frame / ScanInterval) % candidates.size()];
        NRIEnemyFamilyRecovery candidate;
        if (!BeginRecovery(*this, *actor, frame, services, candidate)) return;
        group = std::move(candidate);
    }
    uint64_t liveUsage = 0, liveBudget = 0;
    services.allocationBudget(services.user, liveUsage, liveBudget);
    usage = std::max(usage, liveUsage);
    budget = std::min(budget, liveBudget);
    const auto previousPair = group.permittedPair;
    RecoveryPrice price;
    const char* stop = PriceRecovery(*this, group, enemyFamilyStaging.OwnerCount(group.signature), usage, budget, price);
    if (!stop && group.pins.size() != group.ready)
    {
        if (!opening && group.pins.size() < group.ready) stop = "lost-progress";
        group.ready = uint32_t(group.pins.size());
        group.lastProgressFrame = frame;
    }
    if (!stop && (frame < group.lastProgressFrame || frame - group.lastProgressFrame >= ProgressTimeout)) stop = "no-progress";
    if (!stop && (opening || previousPair != group.permittedPair))
    {
        const auto pose = std::find_if(group.poses.begin(), group.poses.end(),
            [&](const auto& item) { return item.pair == group.permittedPair; });
        const auto* variant = NRIFindPlannedEnemyGeometry(group.build, pose->mesh);
        const auto entry = admissionQueue.find(pose->pair);
        if (!variant) stop = "plan-unavailable";
        else if ((entry == admissionQueue.end() || entry->second.state == PersistentVoxelAdmissionState::Ready) &&
            !EnqueueAdmission(*variant, NRIVoxelAdmissionOrigin::FamilyRecovery, group.build, settings, 0, false, services))
            stop = "queue-refused";
        if (!stop) Trace(frame, group, price, "permit");
    }
    if (stop)
    {
        Trace(frame, group, price, stop);
        group = {};
        enemyFamilyRecoveryScan = frame;
    }
}
