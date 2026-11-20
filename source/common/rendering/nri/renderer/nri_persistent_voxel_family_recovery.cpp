#include "nri_persistent_voxels.h"
#include "nri_cvars.h"
#include "nri_voxel_compute_meshing.h"
#include "nri_voxel_compute_preload.h"
#include "../scene/nri_voxel_material_slots.h"
#include "actor_presentation_snapshot.h"
#include "perf_audit.h"
#include <algorithm>
#include <cstdio>

EXTERN_CVAR(Bool, nri_ptenemystablerepresentation)
EXTERN_CVAR(Bool, nri_ptenemyrepresentationtrace)

namespace
{
constexpr uint32_t ScanInterval = 60, ProgressTimeout = 120;
uint64_t Add(uint64_t a, uint64_t b) { return a > UINT64_MAX - b ? UINT64_MAX : a + b; }

struct RecoveryPrice
{
    uint64_t extraCharge = 0, futureBytes = 0, available = 0, blockedMesh = 0;
    uint64_t stageAvailable = 0, retainedGeometry = 0, retainedBlas = 0, futureBlas = 0, source = 0, transient = 0;
    uint32_t additionalMeshes = 0, materialRows = 0, reusedMaterialRows = 0;
    bool rangeFits = false, materialsFit = false;
    std::array<uint64_t, 3> backing = {}, required = {};
};

void Trace(uint32_t frame, const NRIEnemyFamilyRecovery& group, const RecoveryPrice& price, const char* reason)
{
    if (!PerfAuditActive()) return;
    char fields[1280];
    std::snprintf(fields, sizeof(fields), "\"renderer_frame\":%u,\"world\":%llu,\"signature\":%llu,\"family\":%u,"
        "\"reason\":\"%s\",\"poses\":%zu,\"resident\":%zu,\"pair\":%llu,\"future_bytes\":%llu,"
        "\"available_bytes\":%llu,\"additional_charge\":%llu,\"blocked_mesh\":%llu,"
        "\"late\":%s,\"stage_available\":%llu,\"retained_geometry\":%llu,\"retained_blas\":%llu,\"future_blas\":%llu,"
        "\"future_source\":%llu,\"serial_workspace\":%llu,\"range_fits\":%s,\"material_fits\":%s,\"material_rows_tested\":%u,"
        "\"material_rows_reused\":%u,\"backed_rows\":[%llu,%llu,%llu],\"required_rows\":[%llu,%llu,%llu]",
        frame, (unsigned long long)group.world, (unsigned long long)group.signature, group.family, reason,
        group.poses.size(), group.pins.size(), (unsigned long long)group.permittedPair,
        (unsigned long long)price.futureBytes, (unsigned long long)price.available,
        (unsigned long long)price.extraCharge, (unsigned long long)price.blockedMesh,
        group.late ? "true" : "false", (unsigned long long)price.stageAvailable,
        (unsigned long long)price.retainedGeometry, (unsigned long long)price.retainedBlas,
        (unsigned long long)price.futureBlas, (unsigned long long)price.source, (unsigned long long)price.transient,
        price.rangeFits ? "true" : "false", price.materialsFit ? "true" : "false", price.materialRows, price.reusedMaterialRows,
        (unsigned long long)price.backing[0], (unsigned long long)price.backing[1], (unsigned long long)price.backing[2],
        (unsigned long long)price.required[0], (unsigned long long)price.required[1], (unsigned long long)price.required[2]);
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
    group.late = !voxels.enemyFamilyStaging.WasPriced(group.signature);
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
        if (!key || !voxels.startupFamilyBudget.RecoveryCost(key) || !planned || !planned->model ||
            !planned->directOnlyAdmission || !planned->primitiveCount || !planned->materialKeyHash) return false;
        uint64_t source = 0, workspace = 0;
        if (!services.forecastFamilyProvider(services.user, planned->model, source, workspace, !group.late)) return false;
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
    uint32_t frame, uint64_t usage, uint64_t budget, bool opening, RecoveryPrice& price)
{
    const auto& stage = voxels.enemyFamilyStaging;
    const uint64_t reserve = uint64_t(std::max(0, (int)nri_ptvoxelallocationreservemb)) * 1024 * 1024 +
        std::max<uint64_t>(budget / 64, 16ull * 1024 * 1024);
    price.available = budget - std::min(budget, Add(usage, reserve));
    const bool detail = group.late && opening && (bool)nri_ptenemyrepresentationtrace && PerfAuditActive();
    struct NativePose { uint64_t blas = 0, source = 0, scratch = 0, workspace = 0; };
    std::vector<NativePose> nativePoses;
    if (detail) nativePoses.reserve(group.poses.size());
    std::vector<NRIVoxelFamilyRangeRequest> ranges;
    const char* invalid = nullptr;
    group.pins.clear();
    group.permittedPair = 0;
    for (const auto& pose : group.poses)
    {
        const auto& native = *voxels.startupFamilyBudget.RecoveryCost(pose.mesh);
        if (detail) nativePoses.emplace_back();
        if (!stage.MeshPins().count(pose.mesh))
        {
            ++price.additionalMeshes;
            price.retainedGeometry = Add(price.retainedGeometry, native.geometry);
            price.retainedBlas = Add(price.retainedBlas, native.blas);
            price.extraCharge = Add(price.retainedGeometry, price.retainedBlas);
        }
        const auto found = voxels.meshVariantResources.find(pose.mesh);
        if (found != voxels.meshVariantResources.end() && !IsEmptyNRIVoxelMeshPlaceholder(found->second))
        {
            const auto& mesh = found->second;
            if (mesh.directComputePublished && voxels.IsEnemyFamilyMeshReady(mesh, frame))
            {
                group.pins.insert(pose.mesh);
                continue;
            }
            if (!group.late) return "unsupported-resident";
            invalid = "unsupported-resident";

        }
        NRIPersistentVoxelGeometryRangeHandle owned;
        const auto entry = voxels.admissionQueue.find(pose.pair);
        uint64_t blas = native.blas, scratch = native.scratch;
        if (entry != voxels.admissionQueue.end())
        {
            if (entry->second.mapGeneration != voxels.residencyMapGeneration ||
                entry->second.state == PersistentVoxelAdmissionState::Failed)
            {
                if (!group.late) return "queue-unavailable";
                invalid = "queue-unavailable";
            }
            owned = entry->second.uploadMeshResource.geometryRange;
            if (group.late)
            {
                const auto& as = entry->second.uploadMeshResource.accelerationStructure;
                if (as.accelerationStructure && as.buildTypeValid && as.buildType == nri::AccelerationStructureType::BOTTOM_LEVEL &&
                    as.buildFlags == voxels.startupFamilyBudget.blasFlags && !as.buildVertexBuffer && !as.buildIndexBuffer &&
                    as.buildVertexCount >= 2ull * pose.primitives && as.buildIndexCount >= 3ull * pose.primitives)
                    blas = 0;
                const auto& buffer = entry->second.directBlasScratchBuffer;
                if (buffer.buffer && buffer.size >= scratch && buffer.stride == 16 && buffer.memoryLocation == nri::MemoryLocation::DEVICE &&
                    buffer.usage == nri::BufferUsageBits::SCRATCH_BUFFER) scratch = 0;
            }
        }
        ranges.push_back({pose.mesh, pose.primitives, owned});
        price.futureBlas = Add(price.futureBlas, blas);
        price.source = Add(price.source, pose.sourceBytes);
        price.transient = std::max(price.transient, Add(scratch, pose.workspaceBytes));
        if (detail) nativePoses.back() = {blas, pose.sourceBytes, scratch, pose.workspaceBytes};
        if (!group.permittedPair) group.permittedPair = pose.pair;
    }
    if (ranges.empty() && !group.late) return "complete";
    const auto& stats = stage.Stats();
    const auto limit = std::min({stats.limitBytes, voxels.startupFamilyBudget.StageCharge(), budget / 2});
    price.stageAvailable = limit - std::min(limit, stats.chargedBytes);
    const bool cap = stats.families >= NRIEnemyFamilyStaging::MaximumFamilies ||
        stats.meshes + price.additionalMeshes > NRIEnemyFamilyStaging::MaximumMeshes || price.extraCharge > price.stageAvailable;
    if (cap && !group.late) return "stage-cap";
    const uint64_t reservedMaterialBytes = group.late ? 0 : uint64_t(owners) * group.poseCount *
        nri_scene::VoxelPaletteMaterialSlotCount * sizeof(nri_scene::MaterialData);
    price.futureBytes = Add(Add(price.futureBlas, price.source), Add(reservedMaterialBytes, price.transient));
    const bool pressure = !price.available || price.futureBytes > price.available;
    if (pressure && !group.late) return "physical-reserve";
    price.backing = {voxels.vertexBuffer.size / sizeof(nri_scene::SceneVertex),
        voxels.indexBuffer.size / sizeof(uint32_t), voxels.primitiveBuffer.size / sizeof(nri_scene::PrimitiveData)};
    const auto fit = voxels.geometryRanges.PreviewFamily(ranges, price.backing, detail);
    price.blockedMesh = fit.blockedMesh;
    price.rangeFits = fit.fits;
    price.required = fit.requiredRows;
    price.materialsFit = !group.late || voxels.PreviewEnemyFamilyMaterials(group.signature, owners, price.materialRows, price.reusedMaterialRows);
    if (detail)
    {
        for (size_t i = 0; i < group.poses.size(); ++i)
        {
            const auto& pose = group.poses[i];
            const auto& native = nativePoses[i];
            const auto place = std::find_if(fit.placements.begin(), fit.placements.end(),
                [&](const auto& item) { return item.mesh == pose.mesh; });
            const bool resident = group.pins.count(pose.mesh) != 0;
            const auto range = resident ? voxels.meshVariantResources.at(pose.mesh).geometryRange :
                place != fit.placements.end() ? place->range : NRIPersistentVoxelGeometryRangeHandle{};
            char fields[640];
            std::snprintf(fields, sizeof(fields), "\"renderer_frame\":%u,\"family\":%u,\"signature\":%llu,\"mesh_key\":\"%llu\","
                "\"primitives\":%u,\"resident\":%s,\"backed\":%s,\"offsets\":[%u,%u,%u],\"future_blas\":%llu,"
                "\"future_source\":%llu,\"scratch\":%llu,\"provider_workspace\":%llu", frame, group.family,
                (unsigned long long)group.signature, (unsigned long long)pose.mesh, pose.primitives,
                resident ? "true" : "false", (resident || (place != fit.placements.end() && place->backed)) ? "true" : "false",
                range.vertices.offset, range.indices.offset, range.primitives.offset, (unsigned long long)native.blas,
                (unsigned long long)native.source, (unsigned long long)native.scratch, (unsigned long long)native.workspace);
            PerfAuditWriteEvent("enemy_family_recovery_pose", PerfAuditCurrentToken(), fields);
        }
    }
    if (invalid) return invalid;
    if (!fit.fits) return "range-layout";
    if (pressure) return "physical-reserve";
    if (!price.materialsFit) return "material-layout";
    if (cap) return "stage-cap";
    if (ranges.empty() && !price.additionalMeshes) return "complete";
    return nullptr;
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
        (!enemyFamilyRecovery.late || (bool)nri_ptvoxellatefamilyrecovery) &&
        enemyFamilyRecovery.build == residencyLastBuildSerial && enemyFamilyStaging.NeedsFamily(enemyFamilyRecovery.signature);
}

bool NRIPersistentVoxelResidency::CanWakeParkedEnemyFamilyPose(const PersistentVoxelAdmissionEntry& entry) const
{
    return AllowsEnemyFamilyRecoveryPair(entry.pairKey) && entry.mapGeneration == residencyMapGeneration &&
        !NRIVoxelFamilyRecoveryOnly(entry.admissionOrigins) && entry.variant.directOnlyAdmission &&
        entry.variant.model && entry.variant.primitiveCount && !entry.directComputeFailed && ShouldDirectPublishNRIVoxelComputeMeshing() &&
        !enemyFamilyStaging.NeedsSpawnPose(entry.pairKey) && IsUnstartedOptionalAdmission(entry, IsRequiredAdmission(entry)) &&
        CanDiscardUnsubmittedVoxelAdmission(entry, admissionScheduler);
}

void NRIPersistentVoxelResidency::UpdateEnemyFamilyRecovery(uint32_t frame, const NRIPersistentVoxelSettings& settings,
    uint64_t usage, uint64_t budget, const NRIPersistentVoxelResetServices& services)
{
    auto& group = enemyFamilyRecovery;
    const auto& snapshot = GetActorPresentationSnapshot();
    if (!IsEnemyFamilyRecoveryEnabled() || !services.allocationBudget || settings.transformKeyed || !snapshot.complete || !snapshot.worldEpoch ||
        !budget || (group.signature && (group.world != snapshot.worldEpoch || group.build != residencyLastBuildSerial ||
        (group.late && !(bool)nri_ptvoxellatefamilyrecovery) || !enemyFamilyStaging.NeedsFamily(group.signature))))
    {
        if (group.signature) { Trace(frame, group, {}, "authority-or-budget"); group = {}; }
        return;
    }
    // A completed late recovery can bridge to the first presentation. A priced
    // family that needs reconstruction always gets the serial worker first.
    if (group.late && frame % ScanInterval == 0)
        for (const auto& actor : snapshot.actors)
        {
            const auto& family = actor.spriteAnimation.enemyVoxelFamily;
            if (family && family->signature != group.signature && enemyFamilyStaging.WasPriced(family->signature) &&
                enemyFamilyStaging.NeedsFamily(family->signature) && !InspectEnemyFamily(actor, frame, settings).complete)
            {
                Trace(frame, group, {}, "yield-to-staged");
                group = {};
                enemyFamilyRecoveryScan = UINT32_MAX;
                break;
            }
        }
    const bool opening = !group.signature;
    if (opening)
    {
        if (enemyFamilyRecoveryScan != UINT32_MAX && frame >= enemyFamilyRecoveryScan &&
            frame - enemyFamilyRecoveryScan < ScanInterval) return;
        enemyFamilyRecoveryScan = frame;
        std::unordered_set<uint64_t> seen;
        std::vector<const ActorPresentationState*> candidates, late;
        for (const auto& actor : snapshot.actors)
        {
            const auto& family = actor.spriteAnimation.enemyVoxelFamily;
            if (!family || !family->complete || !actor.owner.IsValid() || !actor.authorityCurrent ||
                !actor.spriteAnimation.enemy || actor.owner.ownerWorldEpoch != snapshot.worldEpoch ||
                enemyPolicy.Find({actor.owner.ownerWorldEpoch, uint64_t(actor.owner.ownerLifetimeGeneration)}) ||
                !enemyFamilyStaging.NeedsFamily(family->signature) || !seen.insert(family->signature).second) continue;
            const bool selected = enemyFamilyStaging.WasPriced(family->signature);
            if (!selected && !(bool)nri_ptvoxellatefamilyrecovery) continue;
            const auto& inspected = InspectEnemyFamily(actor, frame, settings);
            if (inspected.complete) continue;
            if (selected) candidates.push_back(&actor);
            else if (inspected.recoveryPricesKnown) late.push_back(&actor);
        }
        if (candidates.empty()) candidates.swap(late);
        if (candidates.empty()) return;
        // Bound native forecasts to one family per scan. Rotate so a family
        // with an impossible range layout cannot monopolize later scans.
        const auto* actor = candidates[(frame / ScanInterval) % candidates.size()];
        NRIEnemyFamilyRecovery candidate;
        if (!BeginRecovery(*this, *actor, frame, services, candidate))
        {
            if (candidate.late) Trace(frame, candidate, {}, "native-plan-unavailable");
            return;
        }
        group = std::move(candidate);
    }
    uint64_t liveUsage = 0, liveBudget = 0;
    services.allocationBudget(services.user, liveUsage, liveBudget);
    usage = std::max(usage, liveUsage);
    budget = std::min(budget, liveBudget);
    const auto previousPair = group.permittedPair;
    RecoveryPrice price;
    const char* stop = PriceRecovery(*this, group, enemyFamilyStaging.OwnerCount(group.signature), frame, usage, budget, opening, price);
    if (!stop && group.pins.size() != group.ready)
    {
        if (!opening && group.pins.size() < group.ready) stop = "lost-progress";
        group.ready = uint32_t(group.pins.size());
        group.lastProgressFrame = frame;
    }
    if (!stop && group.permittedPair && (frame < group.lastProgressFrame || frame - group.lastProgressFrame >= ProgressTimeout)) stop = "no-progress";
    if (!stop && group.permittedPair && (opening || previousPair != group.permittedPair))
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
