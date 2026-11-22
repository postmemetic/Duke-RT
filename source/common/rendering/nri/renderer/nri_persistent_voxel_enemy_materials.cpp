#include "nri_persistent_voxels.h"
#include "nri_persistent_voxel_publication_services.h"
#include "../scene/nri_enemy_material_key.h"
#include "../scene/nri_voxel_material_slots.h"
#include "actor_presentation_snapshot.h"
#include "c_cvars.h"
#include "perf_audit.h"

#include <algorithm>
#include <cstdio>
#include <limits>

EXTERN_CVAR(Bool, nri_ptenemyrepresentationtrace)

bool NRIPersistentVoxelResidency::PreviewEnemyFamilyMaterials(uint64_t signature, uint32_t owners,
    uint32_t& rows, uint32_t& reused) const
{
    const auto& snapshot = GetActorPresentationSnapshot();
    if (!owners || !materialBuffer.buffer || !materialBuffer.shaderView || !snapshot.complete) return false;
    auto preview = materialRangeAllocator;
    const auto maximum = uint32_t(std::min<uint64_t>(materialBuffer.size / sizeof(nri_scene::MaterialData), UINT32_MAX));
    constexpr auto slots = nri_scene::VoxelPaletteMaterialSlotCount;
    uint32_t matched = 0;
    for (const auto& actor : snapshot.actors)
    {
        const auto& family = actor.spriteAnimation.enemyVoxelFamily;
        const NRIEnemyRepresentationOwner owner{actor.owner.ownerWorldEpoch, uint64_t(actor.owner.ownerLifetimeGeneration)};
        const auto* choice = enemyPolicy.Find(owner);
        if (!family || family->signature != signature || !actor.owner.IsValid() || !actor.authorityCurrent ||
            owner.world != snapshot.worldEpoch || !actor.spriteAnimation.enemy || (choice && !choice->MayPromote())) continue;
        if (++matched > owners || !family->complete || family->textureIds.empty() ||
            family->textureIds.size() > NRIEnemyFamilyStaging::MaximumFamilyPoses || actor.actorIndex < 0) return false;
        std::unordered_set<uint64_t> seen;
        for (auto texture : family->textureIds)
        {
            const auto key = nri_scene::BuildEnemyStableMaterialKey(owner.world, owner.lifetime, texture);
            if (!seen.insert(key).second) continue;
            rows += slots;
            const auto resource = materialVariantResources.find(key);
            if (!key || (resource != materialVariantResources.end() && !resource->second.enemyStableMaterialBinding)) return false;
            if (materialPayloads.FindBinding(key))
            {
                if (!materialPayloads.CanReusePrivateBinding(key, actor.actorIndex, slots, maximum, materialRangeAllocator)) return false;
                reused += slots;
            }
            else if (!preview.AllocateWithin(slots, maximum)) return false;
        }
    }
    return matched == owners;
}

bool NRIPersistentVoxelResidency::PrepareEnemyMaterialCommit(const ActorPresentationState& authority)
{
    const auto& family = authority.spriteAnimation.enemyVoxelFamily;
    if (!authority.owner.IsValid() || !authority.spriteAnimation.enemy || !family || !family->complete ||
        family->textureIds.empty() || family->textureIds.size() > 128 || retentionQuarantined ||
        !materialBuffer.buffer || !materialBuffer.shaderView) return false;

    // This is a reservation inside already committed backing, not permission
    // to grow a GPU buffer after we promise an actor its complete animation.
    const uint32_t maximumRows = uint32_t(std::min<uint64_t>(
        materialBuffer.size / sizeof(nri_scene::MaterialData), UINT32_MAX));
    std::vector<uint64_t> created;
    uint32_t reserved = 0;
    int32_t attemptedTexture = -1;
    uint64_t attemptedKey = 0;
    bool complete = true;
    for (int32_t texture : family->textureIds)
    {
        const uint64_t key = nri_scene::BuildEnemyStableMaterialKey(authority.owner.ownerWorldEpoch,
            uint64_t(authority.owner.ownerLifetimeGeneration), texture);
        attemptedTexture = texture;
        attemptedKey = key;
        const auto existing = materialVariantResources.find(key);
        if (!key || (existing != materialVariantResources.end() && !existing->second.enemyStableMaterialBinding))
        { complete = false; break; }
        NRIPersistentVoxelMaterialRangeHandle range;
        bool fresh = false;
        if (!materialPayloads.ReservePrivateBinding(key, authority.actorIndex,
            nri_scene::VoxelPaletteMaterialSlotCount, maximumRows, materialRangeAllocator, range, fresh))
        { complete = false; break; }
        if (fresh) created.push_back(key);
        auto& resource = materialVariantResources[key];
        resource.enemyStableMaterialBinding = true;
        if (fresh) resource.enemyMaterialReservationOnly = true;
        resource.materialKeyHash = key;
        resource.materialOffset = range.offset;
        resource.materialCapacity = range.capacity;
        resource.materialSlotGeneration = range.generation;
        ++reserved;
    }
    if (!complete)
    {
        // No new row has been published or submitted. Leave all pre-existing
        // bindings untouched and return only this attempt's unused slots.
        for (uint64_t key : created)
        {
            const auto range = materialPayloads.ReleaseBinding(key);
            if (!materialPayloads.ReleaseRetiredRange(range, materialRangeAllocator)) retentionQuarantined = true;
            materialVariantResources.erase(key);
        }
    }
    else if (!created.empty())
    {
        ++materialResourceGeneration;
        MarkMaintenanceMutation();
    }
    if (PerfAuditActive())
    {
        char fields[512];
        std::snprintf(fields, sizeof(fields), "\"action\":\"reserve\",\"world\":%llu,\"lifetime\":%llu,"
            "\"family\":%u,\"complete\":%s,\"poses\":%u,\"reserved_poses\":%u,\"new_bindings\":%u,"
            "\"physical_capacity_rows\":%u,\"cursor_rows\":%llu,\"failed_texture\":%d,\"failed_binding\":%llu",
            (unsigned long long)authority.owner.ownerWorldEpoch,
            (unsigned long long)authority.owner.ownerLifetimeGeneration, family->id, complete ? "true" : "false",
            uint32_t(family->textureIds.size()), reserved, complete ? uint32_t(created.size()) : 0,
            maximumRows, (unsigned long long)materialRangeAllocator.Stats().cursorRows, complete ? -1 : attemptedTexture,
            (unsigned long long)(complete ? 0 : attemptedKey));
        PerfAuditWriteEvent("enemy_material_reservation", PerfAuditCurrentToken(), fields);
        if (complete && (bool)nri_ptenemyrepresentationtrace)
        {
            for (const int32_t texture : family->textureIds)
            {
                const auto key = nri_scene::BuildEnemyStableMaterialKey(authority.owner.ownerWorldEpoch,
                    uint64_t(authority.owner.ownerLifetimeGeneration), texture);
                const auto range = materialPayloads.FindBinding(key);
                const bool acquired = std::find(created.begin(), created.end(), key) != created.end();
                std::snprintf(fields, sizeof(fields), "\"world\":%llu,\"lifetime\":%lld,\"family\":%u,"
                    "\"texture\":%d,\"binding\":%llu,\"offset\":%u,\"capacity\":%u,\"generation\":%llu,\"backed_rows\":%u,\"created_this_commit\":%s",
                    (unsigned long long)authority.owner.ownerWorldEpoch, (long long)authority.owner.ownerLifetimeGeneration,
                    family->id, texture, (unsigned long long)key, range.offset, range.capacity,
                    (unsigned long long)range.generation, maximumRows, acquired ? "true" : "false");
                PerfAuditWriteEvent("enemy_material_binding", PerfAuditCurrentToken(), fields);
            }
        }
    }
    return complete;
}

void NRIPersistentVoxelResidency::ReleaseEnemyMaterialCommits(const std::vector<uint64_t>& keys,
    const NRIPersistentVoxelResetServices& services, bool authoritativeEmptyAfterWait)
{
    for (uint64_t key : keys)
        if (!enemyPolicy.MaterialPins().count(key)) enemyMaterialReleasePending.insert(key);
    if (enemyMaterialReleasePending.empty()) return;
    NRIVoxelCachePins pins;
    if (!CollectCachePins(BuildNRIPersistentVoxelSettingsFromCVars(), pins,
        SIZE_MAX, authoritativeEmptyAfterWait)) return;
    uint32_t unused = 0, retiring = 0;
    for (auto it = enemyMaterialReleasePending.begin(); it != enemyMaterialReleasePending.end();)
    {
        const uint64_t key = *it;
        if (pins.materials.count(key)) { ++it; continue; }
        const auto found = materialVariantResources.find(key);
        if (found == materialVariantResources.end() || !found->second.enemyStableMaterialBinding)
        { it = enemyMaterialReleasePending.erase(it); continue; }
        const bool unpublished = found->second.enemyMaterialReservationOnly;
        const auto range = materialPayloads.ReleaseBinding(key);
        if (range)
        {
            if (unpublished)
            {
                if (!materialPayloads.ReleaseRetiredRange(range, materialRangeAllocator)) retentionQuarantined = true;
                ++unused;
            }
            else { pendingMaterialRangeRetirements.push_back(range); ++retiring; }
        }
        inactiveMaterialSince.erase(key);
        publishedMaterialKeys.erase(key);
        dirtyMaterialResourceKeys.erase(key);
        materialVariantResources.erase(found);
        it = enemyMaterialReleasePending.erase(it);
    }
    if (unused || retiring)
    {
        RetirePendingMaterialRanges(services);
        PruneOrphanedTerminalAdmissions(pins, services);
        ++materialResourceGeneration;
        ResetActorPublicationConsumer();
        MarkMaintenanceMutation();
        RebuildAdmissionIndex(true);
        if (PerfAuditActive())
        {
            char fields[256];
            std::snprintf(fields, sizeof(fields), "\"action\":\"release\",\"unused_bindings\":%u,"
                "\"retiring_bindings\":%u,\"waiting_for_pins\":%u", unused, retiring,
                uint32_t(enemyMaterialReleasePending.size()));
            PerfAuditWriteEvent("enemy_material_reservation", PerfAuditCurrentToken(), fields);
        }
    }
}
