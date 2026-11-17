#include "nri_persistent_voxels.h"
#include "nri_persistent_voxel_publication_services.h"
#include "../scene/nri_enemy_material_key.h"
#include "../scene/nri_voxel_material_slots.h"
#include "actor_presentation_snapshot.h"
#include "perf_audit.h"

#include <algorithm>
#include <cstdio>
#include <limits>

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
    bool complete = true;
    for (int32_t texture : family->textureIds)
    {
        const uint64_t key = nri_scene::BuildEnemyStableMaterialKey(authority.owner.ownerWorldEpoch,
            uint64_t(authority.owner.ownerLifetimeGeneration), texture);
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
            "\"physical_capacity_rows\":%u,\"cursor_rows\":%llu",
            (unsigned long long)authority.owner.ownerWorldEpoch,
            (unsigned long long)authority.owner.ownerLifetimeGeneration, family->id, complete ? "true" : "false",
            uint32_t(family->textureIds.size()), reserved, complete ? uint32_t(created.size()) : 0,
            maximumRows, (unsigned long long)materialRangeAllocator.Stats().cursorRows);
        PerfAuditWriteEvent("enemy_material_reservation", PerfAuditCurrentToken(), fields);
    }
    return complete;
}

void NRIPersistentVoxelResidency::ReleaseEnemyMaterialCommits(const std::vector<uint64_t>& keys,
    const NRIPersistentVoxelResetServices& services, bool authoritativeEmptyAfterWait)
{
    enemyMaterialReleasePending.insert(keys.begin(), keys.end());
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
