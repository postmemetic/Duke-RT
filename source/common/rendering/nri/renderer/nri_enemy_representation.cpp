#include "nri_persistent_voxels.h"
#include "../scene/nri_actor_sprite_fallback.h"
#include "../scene/nri_enemy_material_key.h"
#include "../scene/nri_enemy_voxel_retained.h"
#include "actor_presentation_snapshot.h"
#include "c_cvars.h"
#include "perf_audit.h"
#include "printf.h"
#include <algorithm>
#include <cstdio>

CVAR(Bool, nri_ptenemystablerepresentation, true, 0)
CVAR(Bool, nri_ptenemyrepresentationtrace, false, 0)

namespace
{
void TraceChoice(uint32_t frame, const nri_scene::ActorSpriteFallbackCandidate& candidate,
    const NRIEnemyRepresentationCommitment& choice)
{
    if (!(bool)nri_ptenemyrepresentationtrace) return;
    char fields[640];
    std::snprintf(fields, sizeof(fields), "\"renderer_frame\":%u,\"actor\":%d,\"identity\":%llu,\"world\":%llu,\"lifetime\":%llu,\"family\":%u,\"selection\":\"%s\",\"reason\":\"%s\",\"emergency\":%s,\"awaiting_sprite\":%s,\"mesh_pins\":%zu,\"material_pins\":%zu",
        frame, candidate.actorIndex, (unsigned long long)candidate.identityKey,
        (unsigned long long)candidate.ownerWorldEpoch, (unsigned long long)candidate.ownerLifetimeGeneration,
        choice.family, choice.mode == NRIEnemyRepresentationMode::Voxel ? "voxel" : "sprite",
        choice.reason, choice.emergency ? "true" : "false", choice.awaitingSprite ? "true" : "false", choice.meshes.size(), choice.materials.size());
    PerfAuditWriteEvent("enemy_representation", PerfAuditCurrentToken(), fields);
}
}

void NRIPersistentVoxelResidency::RefreshEnemyRepresentations(uint32_t frame,
    const NRIPersistentVoxelResetServices& services)
{
    const auto& snapshot = GetActorPresentationSnapshot();
    if (!(bool)nri_ptenemystablerepresentation) { enemyPolicy.Reset(); nri_scene::ResetEnemyVoxelSources(); }
    else if (snapshot.complete)
    {
        std::vector<NRIEnemyRepresentationOwner> live;
        live.reserve(snapshot.actors.size());
        // Include corpses and hidden/offscreen actors. Visibility and enemy flags
        // are not removal authority; a corpse retains its lifetime's choice.
        for (const auto& actor : snapshot.actors)
            if (actor.owner.IsValid()) live.push_back({actor.owner.ownerWorldEpoch, uint64_t(actor.owner.ownerLifetimeGeneration)});
        enemyPolicy.Reconcile(snapshot.worldEpoch, live);
    }
    ReleaseEnemyMaterialCommits(enemyPolicy.TakeReleasedMaterialKeys(), services);
    ReconcileEnemyFamilyStaging();
    // Staging observes the resources at the start of this pump. A later mesh
    // publish, replacement, or eviction invalidates this observation before
    // first-visible selection and texture preparation can consume it.
    enemyFamilyResidency.clear(); enemyFamilyFrame = frame;
    if ((bool)nri_ptenemyrepresentationtrace && snapshot.complete)
    {
        const auto settings = BuildNRIPersistentVoxelSettingsFromCVars();
        for (const auto& actor : snapshot.actors)
            if (actor.spriteAnimation.enemy) InspectEnemyFamily(actor, frame, settings);
    }
}

void NRIPersistentVoxelResidency::ResetEnemyRepresentationsForLevelTransition()
{
    enemyFamilyStaging.Reset(); enemyStagingFrame = enemyRestageAttemptFrame = UINT32_MAX;
    enemyPreparesProspectiveMaterials = false; enemyPreparationHeadroom = 0; enemyPreparationCursor = 0;
    prospectiveMaterialOwners.clear();
    nri_scene::ResetEnemyVoxelSources();
    enemyPolicy.Reset();
    const auto released = enemyPolicy.TakeReleasedMaterialKeys();
    enemyMaterialReleasePending.insert(released.begin(), released.end());
    // Include rows acquired before an actor's first visible commitment. Their
    // GPU storage still belongs to the existing retirement/fence owner.
    for (const auto& entry : materialVariantResources)
        if (entry.second.enemyStableMaterialBinding) enemyMaterialReleasePending.insert(entry.first);
    enemyFamilyResidency.clear();
    enemyFamilyFrame = UINT32_MAX;
    enemyPublicationEntries.clear();
    enemyMaterialTexturesReadyActors.clear();
    enemyMaterialTextureReasons.clear();
    enemyMaterialTexturesReadyFrame = UINT32_MAX;
}

bool NRIPersistentVoxelResidency::IsEnemySpriteCommitted(uint64_t world, uint64_t lifetime) const
{
    const auto* choice = enemyPolicy.Find({world, lifetime});
    return (bool)nri_ptenemystablerepresentation && choice && choice->mode == NRIEnemyRepresentationMode::Sprite && !choice->awaitingSprite;
}

void NRIPersistentVoxelResidency::FilterEnemyPublicationEntries(
    const std::vector<nri_scene::PersistentVoxelCacheEntryView>*& entries)
{
    if (!(bool)nri_ptenemystablerepresentation || !entries) return;
    const auto excluded = [&](const nri_scene::PersistentVoxelCacheEntryView& actor)
    { return IsEnemySpriteCommitted(actor.ownerWorldEpoch, actor.ownerLifetimeGeneration); };
    if (std::none_of(entries->begin(), entries->end(), excluded)) return;
    enemyPublicationEntries.clear();
    enemyPublicationEntries.reserve(entries->size());
    for (const auto& actor : *entries) if (!excluded(actor)) enemyPublicationEntries.push_back(actor);
    entries = &enemyPublicationEntries;
}

const NRIEnemyFamilyResidency& NRIPersistentVoxelResidency::InspectEnemyFamily(
    const ActorPresentationState& authority, uint32_t frame, const NRIPersistentVoxelSettings& settings)
{
    if (enemyFamilyFrame != frame) { enemyFamilyResidency.clear(); enemyFamilyFrame = frame; }
    const auto& family = authority.spriteAnimation.enemyVoxelFamily;
    const uint64_t signature = family ? family->signature : 0;
    const uint64_t cacheKey = signature ? signature : ~uint64_t(family ? family->id : 0);
    const auto prior = enemyFamilyResidency.find(cacheKey);
    if (prior != enemyFamilyResidency.end()) return prior->second;
    auto& result = enemyFamilyResidency[cacheKey];
    if (family) { result.family = family->id; result.poses = uint32_t(family->textureIds.size()); result.signature = signature; result.reason = family->reason; }
    nri_scene::EnemyVoxelPoseFailure failure;
    const auto finish = [&]() -> const NRIEnemyFamilyResidency&
    {
        if ((bool)nri_ptenemyrepresentationtrace)
        {
            char fields[512];
            std::snprintf(fields, sizeof(fields), "\"renderer_frame\":%u,\"family\":%u,\"name\":\"%s\",\"poses\":%u,\"ready\":%u,\"logical_geometry_bytes\":%llu,\"physical_bytes\":%llu,\"material_reservation_rows_per_owner\":%u,\"reason\":\"%s\",\"failed_tile\":%d,\"failed_texture\":%d,\"failed_voxel_index\":%d,\"pose_source\":\"%s\"",
                frame, result.family, family ? family->name : "unsupported", result.poses, result.ready,
                (unsigned long long)result.logicalGeometryBytes, (unsigned long long)result.physicalBytes, result.poses * 256u, result.reason,
                failure.buildTile, failure.textureId, failure.voxelIndex, failure.source);
            PerfAuditWriteEvent("enemy_family_residency", PerfAuditCurrentToken(), fields);
        }
        return result;
    };
    std::vector<nri_scene::PrecachedVoxelRawManifestView> views;
    if (!family || !family->complete) return finish();
    if (settings.transformKeyed) { result.reason = "transform-keyed"; return finish(); }
    if (!nri_scene::BuildEnemyVoxelFamilyViews(authority, views, &failure))
    {
        result.firstUnavailableTexture = failure.textureId;
        result.firstUnavailableTile = failure.buildTile;
        result.firstUnavailableAction = failure.source;
        result.reason = failure.reason;
        return finish();
    }
    std::unordered_set<uint64_t> unique;
    for (size_t pose = 0; pose < views.size(); ++pose)
    {
        const auto& view = views[pose];
        nri_scene::PrecachedVoxelVariantView variant;
        variant.meshKeyHash = view.meshKeyHash;
        variant.renderPrimitiveHash = view.renderPrimitiveHash;
        variant.geometryContentHash = view.geometryContentHash;
        const uint64_t key = BuildPersistentVoxelVariantMeshResourceKey(variant);
        const auto found = meshVariantResources.find(key);
        const auto rememberUnavailable = [&]()
        {
            if (result.firstUnavailableTexture >= 0) return;
            result.firstUnavailableMesh = key;
            result.firstUnavailableTexture = view.sourcePicnum;
            result.firstUnavailableTile = view.sourceBuildTile;
            if (pose < family->poseSources.size()) result.firstUnavailableAction = family->poseSources[pose];
        };
        if (found == meshVariantResources.end()) { rememberUnavailable(); continue; }
        const auto& mesh = found->second;
        const bool ready = mesh.meshBakeSpace == nri_scene::VoxelMeshBakeSpace::LocalSpace &&
            mesh.vertexCount && mesh.indexCount && mesh.primitiveCount && geometryRanges.Owns(mesh.geometryRange) &&
            mesh.accelerationStructure.accelerationStructure && mesh.accelerationStructure.buildVertexBuffer &&
            vertexBuffer.shaderView && indexBuffer.shaderView && primitiveBuffer.shaderView &&
            (mesh.tlasPublished || mesh.tlasReadyFrame <= frame);
        if (ready) ++result.ready; else rememberUnavailable();
        if (!unique.insert(key).second) continue;
        result.meshes.push_back(key);
        result.logicalGeometryBytes += uint64_t(mesh.vertexCapacity) * sizeof(nri_scene::SceneVertex) +
            uint64_t(mesh.indexCapacity) * sizeof(uint32_t) + uint64_t(mesh.primitiveCapacity) * sizeof(nri_scene::PrimitiveData);
        const auto* shared = sharedBlasCache.Find(key);
        result.physicalBytes += mesh.vertexBuffer.memorySize + mesh.indexBuffer.memorySize + mesh.accelerationStructure.memorySize +
            mesh.shadowProxy.vertexBuffer.memorySize + mesh.shadowProxy.indexBuffer.memorySize +
            mesh.shadowProxy.accelerationStructure.memorySize + (shared ? shared->accelerationStructure.memorySize : 0);
    }
    result.complete = result.ready == result.poses && result.poses != 0;
    result.reason = result.complete ? "geometry-ready" : "geometry-incomplete";
    return finish();
}

NRIEnemyRepresentationChoice NRIPersistentVoxelResidency::SelectEnemyRepresentation(
    const nri_scene::ActorSpriteFallbackCandidate& candidate, uint32_t frame,
    const NRIPersistentVoxelSettings& settings, bool exactReady, bool spriteReady)
{
    if (!(bool)nri_ptenemystablerepresentation) return {};
    const NRIEnemyRepresentationOwner owner{candidate.ownerWorldEpoch, candidate.ownerLifetimeGeneration};
    const auto* authority = nri_scene::LookupActorSpriteFallbackAuthority(owner.world, owner.lifetime);
    uint64_t currentMesh = 0;
    if (exactReady)
    {
        const auto slot = batch.actorIndex.Find(candidate.identityKey);
        if (slot < batch.actors.size())
        {
            const auto& actor = batch.actors[slot];
            if (actor.active && actor.ownerWorldEpoch == owner.world && actor.ownerLifetimeGeneration == owner.lifetime)
                currentMesh = actor.meshResourceKey;
        }
    }
    const auto* committed = enemyPolicy.Find(owner);
    if (committed)
    {
        if (committed->awaitingSprite && spriteReady && enemyPolicy.ResolveSpriteContinuity(owner))
        {
            committed = enemyPolicy.Find(owner); TraceChoice(frame, candidate, *committed);
            ResetActorPublicationConsumer();
        }
        if (committed->mode == NRIEnemyRepresentationMode::Voxel && authority)
        {
            const auto& family = authority->spriteAnimation.enemyVoxelFamily;
            const char* failure = !family || family->signature != committed->familySignature ? "family-authority-changed" :
                !InspectEnemyFamily(*authority, frame, settings).complete ? "committed-geometry-lost" :
                !exactReady ? "committed-current-binding-unavailable" :
                !NRIEnemyFamilyContainsCurrentMesh(committed->meshes, currentMesh) ? "committed-current-pose-outside-family" : nullptr;
            // A device/resource failure has one terminal downgrade. It never
            // enables opportunistic promotion, and requires a drawable sprite.
            if (failure && spriteReady && enemyPolicy.Downgrade(owner, failure))
            { committed = enemyPolicy.Find(owner); nri_scene::SetEnemyVoxelSourceMode(owner.world, owner.lifetime, false);
              TraceChoice(frame, candidate, *committed); ResetActorPublicationConsumer(); }
        }
        return {true, committed->mode == NRIEnemyRepresentationMode::Sprite && !committed->awaitingSprite};
    }
    if (!authority || !authority->spriteAnimation.enemy || !candidate.capturedThisFrame || candidate.primaryHidden ||
        candidate.presentationHidden || !candidate.recipeCertified) return {};
    const auto& family = InspectEnemyFamily(*authority, frame, settings);
    const bool currentCovered = NRIEnemyFamilyContainsCurrentMesh(family.meshes, currentMesh);
    bool voxel = family.complete && exactReady && currentCovered;
    const char* reason = !family.complete ? family.reason : !exactReady ? "current-pose-not-ready" :
        !currentCovered ? "current-pose-outside-family" : "complete-resident-set";
    if (voxel && !nri_scene::HasEnemyVoxelSource(*authority)) { voxel = false; reason = "retained-source-unavailable"; }
    if (voxel && (enemyMaterialTexturesReadyFrame != frame || !enemyMaterialTexturesReadyActors.count(candidate.identityKey)))
    {
        voxel = false;
        const auto pending = enemyMaterialTextureReasons.find(candidate.identityKey);
        reason = pending != enemyMaterialTextureReasons.end() ? pending->second : "family-material-textures-pending";
    }
    if (voxel && !PrepareEnemyMaterialCommit(*authority)) { voxel = false; reason = "material-capacity-unavailable"; }
    std::vector<uint64_t> materials;
    if (voxel) for (const auto texture : authority->spriteAnimation.enemyVoxelFamily->textureIds)
        materials.push_back(nri_scene::BuildEnemyStableMaterialKey(owner.world, owner.lifetime, texture));
    std::vector<uint64_t> meshes = voxel ? family.meshes : std::vector<uint64_t>{};
    const bool continuityFailure = !voxel && !spriteReady;
    if (continuityFailure)
    {
        // Texture preparation has already tried its emergency synchronous path.
        // This is an actual fallback-resource failure, outside the stable-mode
        // guarantee. Latch Sprite now, explicitly retain only the current body
        // while its descriptor recovers, and never silently promote afterward.
        reason = "initial-fallback-resource-unavailable";
        const auto slot = batch.actorIndex.Find(candidate.identityKey);
        if (slot < batch.actors.size())
        {
            const auto& actor = batch.actors[slot];
            if (actor.active && actor.ownerWorldEpoch == owner.world && actor.ownerLifetimeGeneration == owner.lifetime)
            { meshes.push_back(actor.meshResourceKey); materials.push_back(actor.materialKeyHash); }
        }
    }
    const auto& choice = enemyPolicy.Commit(owner, candidate.identityKey, family.family, family.signature, frame,
        voxel, reason, std::move(meshes), std::move(materials), continuityFailure,
        authority->spriteAnimation.enemyVoxelFamily && authority->spriteAnimation.enemyVoxelFamily->complete);
    enemyFamilyStaging.ReleaseOwner(owner);
    nri_scene::SetEnemyVoxelSourceMode(owner.world, owner.lifetime, voxel);
    TraceChoice(frame, candidate, choice);
    if (PerfAuditActive() || (bool)nri_ptenemyrepresentationtrace)
    {
        const auto texture = enemyUnavailableTextureKeys.find(candidate.identityKey);
        const uint64_t missingKey = texture == enemyUnavailableTextureKeys.end() ? 0 : texture->second;
        char fields[600];
        std::snprintf(fields, sizeof(fields), "\"renderer_frame\":%u,\"world\":%llu,\"lifetime\":%llu,\"family\":%u,"
            "\"selection\":\"%s\",\"reason\":\"%s\",\"ready_poses\":%u,\"poses\":%u,\"current_mesh\":%llu,"
            "\"missing_mesh\":%llu,\"missing_pose_texture\":%d,\"missing_tile\":%d,\"action\":\"%s\",\"missing_upload_key\":%llu,\"preparation_headroom\":%llu",
            frame, (unsigned long long)owner.world, (unsigned long long)owner.lifetime, family.family, voxel ? "voxel" : "sprite",
            reason, family.ready, family.poses, (unsigned long long)currentMesh, (unsigned long long)family.firstUnavailableMesh,
            family.firstUnavailableTexture, family.firstUnavailableTile, family.firstUnavailableAction, (unsigned long long)missingKey, (unsigned long long)enemyPreparationHeadroom);
        PerfAuditWriteEvent("enemy_first_visible", PerfAuditCurrentToken(), fields);
        if ((bool)nri_ptenemyrepresentationtrace) Printf("NRI enemy first visible: %s\n", fields);
    }
    if (!voxel)
    {
        // A current pose may have acquired its stable private row before the
        // first visible decision. It is no longer demand once Sprite commits.
        // Include a refused pose outside the certified set. Existing body pins
        // and retirement fences still control when its rows can be reused.
        const auto slot = batch.actorIndex.Find(candidate.identityKey);
        if (slot < batch.actors.size())
        {
            const auto& actor = batch.actors[slot];
            const auto material = materialVariantResources.find(actor.materialKeyHash);
            if (actor.ownerWorldEpoch == owner.world && actor.ownerLifetimeGeneration == owner.lifetime &&
                material != materialVariantResources.end() && material->second.enemyStableMaterialBinding)
                enemyMaterialReleasePending.insert(actor.materialKeyHash);
        }
        if (authority->spriteAnimation.enemyVoxelFamily)
            for (int32_t texture : authority->spriteAnimation.enemyVoxelFamily->textureIds)
                enemyMaterialReleasePending.insert(nri_scene::BuildEnemyStableMaterialKey(owner.world, owner.lifetime, texture));
        ResetActorPublicationConsumer();
    }
    return {true, !voxel && !continuityFailure};
}
