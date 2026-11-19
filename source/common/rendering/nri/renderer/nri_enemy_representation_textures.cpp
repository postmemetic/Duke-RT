#include "nri_enemy_representation_textures.h"
#include "nri_persistent_voxels.h"
#include "nri_scene_textures.h"
#include "../scene/nri_actor_sprite_fallback.h"
#include "../scene/nri_material_bridge.h"
#include "actor_presentation_snapshot.h"
#include "c_cvars.h"
#include "perf_audit.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <numeric>
#include <unordered_set>
#include <vector>

EXTERN_CVAR(Bool, nri_ptactorspritefallback)
EXTERN_CVAR(Bool, nri_ptenemystablerepresentation)

namespace
{
constexpr size_t MaximumNewSets = 4;
constexpr size_t MaximumPreparedPoses = 256;

bool PaletteIndependentVoxelMaterial(const nri_scene::MaterialRef& material)
{
    constexpr uint32_t required = nri_scene::MaterialFlag_PointSampled | nri_scene::MaterialFlag_Indexed;
    return (material.flags & required) == required;
}

bool SameDependencyMaterial(const nri_scene::MaterialRef& a, const nri_scene::MaterialRef& b)
{
    return a.texture == b.texture && a.emissiveSourceTexture == b.emissiveSourceTexture &&
        a.authoredTextureId == b.authoredTextureId && a.palette == b.palette && a.shade == b.shade &&
        a.alpha == b.alpha && a.flags == b.flags && a.voxelPalettePolicy == b.voxelPalettePolicy &&
        a.voxelPalettePolicyContentKey == b.voxelPalettePolicyContentKey;
}

struct DependencySet
{
    uint64_t signature = 0;
    std::vector<nri_scene::MaterialRef> source;
    const char* reason = "not-prepared";
    bool ready = false;
    uint64_t unavailableKey = 0;
};

bool SameSet(const DependencySet& set, uint64_t signature,
    const std::vector<nri_scene::PrecachedVoxelRawManifestView>& views)
{
    if (set.signature != signature || set.source.size() != views.size()) return false;
    for (size_t i = 0; i < views.size(); ++i)
        if (!SameDependencyMaterial(set.source[i], views[i].material)) return false;
    return true;
}
}

void NRIPrepareEnemyFamilyTextures(uint32_t frameIndex, NRIPersistentVoxelResidency& voxels,
    const NRIPersistentVoxelSettings& settings, NRIRenderDevice& device, NRISceneTextureResidency& textures)
{
    voxels.enemyMaterialTexturesReadyActors.clear();
    voxels.prospectiveMaterialOwners.clear();
    voxels.enemyMaterialTextureReasons.clear();
    voxels.enemyUnavailableTextureKeys.clear();
    voxels.enemyMaterialTexturesReadyFrame = frameIndex;
    if (!(bool)nri_ptactorspritefallback || !(bool)nri_ptenemystablerepresentation) return;
    const auto begin = std::chrono::steady_clock::now();
    std::vector<DependencySet> sets;
    std::unordered_set<uint64_t> owners;
    uint32_t eligible = 0, reused = 0, capRefusals = 0, geometryPending = 0;
    uint32_t poses = 0, completeHits = 0, pendingSets = 0, failedSets = 0;
    double materialBuildMs = 0;
    const bool prospective = voxels.enemyPreparesProspectiveMaterials;
    const size_t maximumSets = prospective ? NRIEnemyFamilyStaging::MaximumPreparedFamilies : MaximumNewSets;
    const size_t maximumPoses = prospective ? maximumSets * NRIEnemyFamilyStaging::MaximumFamilyPoses : MaximumPreparedPoses;
    const auto prepare = [&](const ActorPresentationState* authority, uint64_t identity)
    {
        if (!authority || !authority->authorityCurrent || !authority->publicationEligible ||
            !authority->spriteAnimation.enemy || !authority->spriteAnimation.recipeCertified ||
            authority->spriteAnimation.recipeHidden) return;
        if (voxels.enemyPolicy.Find({authority->owner.ownerWorldEpoch, uint64_t(authority->owner.ownerLifetimeGeneration)})) return;
        ++eligible;
        const auto& family = authority->spriteAnimation.enemyVoxelFamily;
        const auto& geometry = voxels.InspectEnemyFamily(*authority, frameIndex, settings);
        if (!family || !family->complete || !geometry.complete)
        {
            ++geometryPending;
            voxels.enemyMaterialTextureReasons[identity] = "geometry-not-ready";
            return;
        }
        std::vector<nri_scene::PrecachedVoxelRawManifestView> views;
        if (!nri_scene::BuildEnemyVoxelFamilyViews(*authority, views) || views.empty())
        {
            voxels.enemyMaterialTextureReasons[identity] = "family-context-unavailable";
            return;
        }
        auto found = std::find_if(sets.begin(), sets.end(), [&](const DependencySet& set)
            { return SameSet(set, family->signature, views); });
        if (found == sets.end())
        {
            if (sets.size() >= maximumSets || views.size() > maximumPoses - poses)
            {
                ++capRefusals;
                voxels.enemyMaterialTextureReasons[identity] = "family-texture-work-cap";
                return;
            }
            DependencySet set;
            set.signature = family->signature;
            nri_scene::SceneView scene;
            scene.opaqueSprites.reserve(views.size() * 2);
            bool paletteIndependent = true;
            for (const auto& view : views)
            {
                set.source.push_back(view.material);
                paletteIndependent &= PaletteIndependentVoxelMaterial(view.material);
                nri_scene::SurfaceRef surface;
                surface.material = view.material;
                surface.provenance.sourceType = nri_scene::SurfaceSourceType::VoxelProxySprite;
                // Dependency-only union. Actor override policy can add
                // Fullbright before material construction; later overrides
                // change scalar/metadata fields, not texture resources. Cover
                // both variants without sharing actor-specific material rows.
                surface.material.flags &= ~nri_scene::MaterialFlag_Fullbright;
                scene.opaqueSprites.push_back(surface);
                surface.material.flags |= nri_scene::MaterialFlag_Fullbright;
                scene.opaqueSprites.push_back(std::move(surface));
            }
            poses += uint32_t(views.size());
            // Future actor palette/shade/alpha changes only update rows while
            // PointSampled|Indexed keeps the uploaded texture key independent
            // of the palette. Refuse certification if this bridge invariant
            // changes; a current-palette closure would no longer cover a life.
            if (!paletteIndependent)
            {
                set.reason = "palette-dependent-family-textures";
                ++failedSets;
            }
            else
            {
            nri_scene::MaterialBridgeData materials;
            const auto materialBegin = std::chrono::steady_clock::now();
            nri_scene::BuildMaterials(scene, materials);
            materialBuildMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - materialBegin).count();
            std::vector<uint32_t> rows(materials.materials.size());
            std::iota(rows.begin(), rows.end(), 0u);
            NRIActorRepresentationTextureResult result;
            textures.EnsurePriorityMaterialTextures(device, materials, rows, result);
            bool failed = rows.size() != views.size() * 2 || !result.unavailableMaterials.empty() || result.failures != 0;
            bool pending = false;
            // Voxel material closure still requires actual completion. The
            // ordered-only sprite capability cannot certify a whole family.
            for (const auto& upload : materials.textures)
            {
                NRISceneTextureClosureResult closure;
                if (!textures.QueryRuntimeClosure(device, upload.key, closure) || !closure.descriptorReady ||
                    closure.residencyIndex >= textures.CachedTextures().size() ||
                    textures.CachedTextures()[closure.residencyIndex].resource.mipCount != (std::max)(1u, upload.mipCount))
                {
                    if (!set.unavailableKey) set.unavailableKey = upload.key;
                    pending |= closure.state == NRISceneTextureClosureState::Pending;
                    failed |= closure.state != NRISceneTextureClosureState::Pending;
                }
            }
            set.ready = !failed && !pending;
            set.reason = failed ? "failed-texture-closure" : (pending ? "pending-texture-upload" : "ready");
            if (set.ready && result.inserted == 0) ++completeHits;
            if (!set.ready)
            {
                if (!failed && pending) ++pendingSets;
                else ++failedSets;
            }
            }
            sets.push_back(std::move(set));
            found = sets.end() - 1;
        }
        else ++reused;
        voxels.enemyMaterialTextureReasons[identity] = found->reason;
        if (found->unavailableKey) voxels.enemyUnavailableTextureKeys[identity] = found->unavailableKey;
        if (found->ready) voxels.enemyMaterialTexturesReadyActors.insert(identity);
    };
    // The first published body has priority over speculative work.
    for (const auto& candidate : nri_scene::GetActorSpriteFallbackCandidates())
    {
        if (!candidate.descriptorCurrent || candidate.presentationHidden || !candidate.ownerWorldEpoch ||
            candidate.transientGeometryCaptured || !owners.insert(candidate.identityKey).second) continue;
        prepare(nri_scene::LookupActorSpriteFallbackAuthority(candidate.ownerWorldEpoch, candidate.ownerLifetimeGeneration),
            candidate.identityKey);
    }
    // Visit one authoritative, as-yet unseen owner per frame. This also covers
    // later spawns, without allocating material rows or choosing a body for them.
    const auto& snapshot = GetActorPresentationSnapshot();
    if (prospective && snapshot.complete && !snapshot.actors.empty())
    {
        const size_t inspections = std::min<size_t>(64, snapshot.actors.size());
        for (size_t i = 0; i < inspections; ++i)
        {
            voxels.enemyPreparationCursor %= snapshot.actors.size();
            const auto& actor = snapshot.actors[voxels.enemyPreparationCursor++];
            const auto& family = actor.spriteAnimation.enemyVoxelFamily;
            const NRIEnemyRepresentationOwner owner{actor.owner.ownerWorldEpoch, uint64_t(actor.owner.ownerLifetimeGeneration)};
            if (!actor.owner.IsValid() || !actor.authorityCurrent || !actor.publicationEligible || !family || !family->complete ||
                actor.spriteAnimation.recipeHidden || voxels.enemyPolicy.Find(owner)) continue;
            const auto identity = nri_scene::BuildVoxelActorIdentityKey(actor.owner);
            auto& visit = voxels.enemyPreparationVisits[identity % voxels.enemyPreparationVisits.size()];
            if ((visit.first == identity && frameIndex >= visit.second && frameIndex - visit.second < 240) ||
                !owners.insert(identity).second) continue;
            prepare(&actor, identity);
            if (voxels.enemyMaterialTexturesReadyActors.count(identity))
            {
                voxels.prospectiveMaterialOwners.push_back(owner);
                visit = {identity, frameIndex};
            }
            break;
        }
    }
    if (PerfAuditActive() && eligible)
    {
        char fields[640];
        std::snprintf(fields, sizeof(fields), "\"renderer_frame\":%u,\"eligible_owners\":%u,\"new_sets\":%u,"
            "\"prepared_poses\":%u,\"matched_owners\":%u,\"ready_owners\":%u,\"already_complete_sets\":%u,"
            "\"cap_refusals\":%u,\"geometry_pending\":%u,\"pending_sets\":%u,\"failed_sets\":%u,"
            "\"material_build_ms\":%.6f,\"cpu_ms\":%.6f", frameIndex, eligible, uint32_t(sets.size()), poses, reused,
            uint32_t(voxels.enemyMaterialTexturesReadyActors.size()), completeHits, capRefusals, geometryPending,
            pendingSets, failedSets, materialBuildMs,
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count());
        PerfAuditWriteEvent("enemy_family_textures", PerfAuditCurrentToken(), fields);
    }
}
