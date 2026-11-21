#include "nri_persistent_voxels.h"
#include "nri_cvars.h"
#include "nri_voxel_compute_allocation.h"
#include "perf_audit.h"
#include "../scene/nri_hash.h"
#include <algorithm>
#include <cstdio>

CVAR(Bool, nri_ptvoxelactoradmission, true, 0)
CVAR(Bool, nri_ptvoxelcoalesce, true, 0)
CVAR(Bool, nri_ptvoxelwarmretention, true, 0)
CVAR(Bool, nri_ptvoxelparksupersededgrowth, false, 0)
CVAR(Bool, nri_ptvoxelarenaposepriority, false, 0)
EXTERN_CVAR(Bool, nri_ptactorspritefallback)
EXTERN_CVAR(Int, nri_ptactorholdframes)

NRIVoxelAdmissionPolicySettings GetNRIVoxelAdmissionPolicySettings()
{
    return { (bool)nri_ptvoxelactoradmission, (bool)nri_ptvoxelcoalesce, (bool)nri_ptvoxelwarmretention,
        (bool)nri_ptactorspritefallback ? uint32_t(std::clamp((int)nri_ptactorholdframes, 1, 600)) : 0u };
}

namespace
{
bool Unstarted(const PersistentVoxelAdmissionEntry& entry)
{
    const auto& mesh = entry.uploadMeshResource;
    const auto& material = entry.uploadMaterialResource;
    const auto emptyRange = [](const NRIPersistentVoxelMaterialRangeHandle& range)
    { return range.offset == 0 && range.capacity == 0 && range.generation == 0; };
    const auto emptyBuffer = [](const NRIBufferResource& buffer)
    { return !buffer.buffer && !buffer.shaderView && !buffer.storageView && !buffer.size && !buffer.memorySize && !buffer.usedSize; };
    const auto emptyAs = [](const NRIAccelerationStructureResource& as)
    {
        return !as.accelerationStructure && !as.descriptor && !as.memorySize && !as.buildVertexBuffer && !as.buildIndexBuffer &&
            !as.buildScratchSize && !as.updateScratchSize;
    };
    return (entry.state == PersistentVoxelAdmissionState::Pending || entry.state == PersistentVoxelAdmissionState::Deferred) &&
        !entry.schedulerTokenId && !entry.directComputeRequested && !entry.directComputeGeneration &&
        !entry.directComputeJobId && !entry.computeGeometryJobId && !entry.directBlasFenceValue &&
        entry.directBlasRecordedFrame == UINT32_MAX && !entry.directBlasExclusive &&
        !entry.uploadPrepared && !entry.uploadSubmittedBeforeBlas && !entry.uploadGeometryFromCompute &&
        !entry.shaderVertexOffset && !entry.shaderIndexOffset && !entry.shaderPrimitiveOffset &&
        !entry.bytesUploaded && !entry.vertexBytesUploaded && !entry.vertexArenaBytesUploaded &&
        !entry.indexBytesUploaded && !entry.indexArenaBytesUploaded && !entry.primitiveBytesUploaded &&
        emptyBuffer(entry.directBlasScratchBuffer) && emptyBuffer(mesh.vertexBuffer) && emptyBuffer(mesh.indexBuffer) &&
        emptyAs(mesh.accelerationStructure) && emptyBuffer(mesh.shadowProxy.vertexBuffer) && emptyBuffer(mesh.shadowProxy.indexBuffer) &&
        emptyAs(mesh.shadowProxy.accelerationStructure) && !mesh.computeProviderReservation.Total() && !mesh.geometryRange.transaction &&
        emptyRange(mesh.geometryRange.vertices) && emptyRange(mesh.geometryRange.indices) && emptyRange(mesh.geometryRange.primitives) &&
        !mesh.vertexCapacity && !mesh.indexCapacity && !mesh.primitiveCapacity &&
        !mesh.vertexOffset && !mesh.indexOffset && !mesh.primitiveOffset &&
        !mesh.directComputeGeneration && !mesh.directComputeJobId &&
        !material.materialSlotGeneration && !material.materialCapacity && !material.materialOffset && !material.materialCount;
}
}

void NRIPersistentVoxelResidency::RefreshActorDemand(uint32_t frameIndex,
    const NRIPersistentVoxelSettings& settings, const NRIPersistentVoxelResetServices& services)
{
    const auto policy = GetNRIVoxelAdmissionPolicySettings();
    const bool allowOverflowPage = EffectiveOverflowPageMode() != 0;
    const bool capturePoseRequests = (bool)nri_ptvoxelfamilyposegrace && IsEnemyFamilyRecoveryEnabled();
    const bool captureUpgrades = (bool)nri_ptvoxeladmissionreclaim || (bool)nri_ptvoxelparksupersededgrowth ||
        allowOverflowPage || (arenaUsesRequiredLayout && (bool)nri_ptvoxelarenaposepriority);
    if (!policy.actorPriority && !policy.coalesce && !policy.warmRetention && !policy.heldMaximumFrames &&
        !captureUpgrades && !capturePoseRequests)
    {
        actorDemand.Reset();
        return;
    }
    std::vector<nri_scene::PersistentVoxelCacheEntryView> source;
    if (!nri_scene::BuildPersistentVoxelCacheEntries(source)) { actorDemand.InvalidateAuthority(); return; }
    std::vector<NRIVoxelActorDemandInput> inputs;
    inputs.reserve(source.size());
    for (const auto& item : source)
    {
        if (IsEnemySpriteCommitted(item.ownerWorldEpoch, item.ownerLifetimeGeneration)) continue;
        // Every source-owned desired pair protects coalescing, including
        // retained/offscreen and not-yet-publication-eligible demand.
        if (!item.identityKey || !item.ownerWorldEpoch)
        { actorDemand.InvalidateAuthority(); return; }
        NRIVoxelActorDemandInput input;
        input.identity = item.identityKey; input.world = item.ownerWorldEpoch; input.lifetime = item.ownerLifetimeGeneration;
        input.mesh = BuildPersistentVoxelMeshResourceKey(item, settings); input.material = item.materialKeyHash;
        input.pair = nri_scene::HashCombine64(input.mesh, input.material);
        input.visible = item.capturedThisFrame && item.publicationEligible && !item.indirectOnly && !item.pendingRemoval;
        input.rayEligible = item.authorityCurrent && item.publicationEligible && !item.pendingRemoval;
        const auto slot = batch.actorIndex.Find(item.identityKey);
        if (slot < batch.actors.size())
        {
            const auto& actor = batch.actors[slot];
            if (actor.active && actor.ownerWorldEpoch == input.world && actor.ownerLifetimeGeneration == input.lifetime)
            {
                input.selectedMesh = actor.meshResourceKey; input.selectedMaterial = actor.materialKeyHash;
                input.representation = input.selectedMesh == input.mesh && input.selectedMaterial == input.material ?
                    NRIVoxelActorRepresentation::Exact : NRIVoxelActorRepresentation::HeldVoxel;
            }
        }
        if (frameIndex && UsesSpriteFallback(input.identity, frameIndex - 1))
        {
            input.representation = NRIVoxelActorRepresentation::SpriteFallback;
            input.selectedMesh = input.selectedMaterial = 0;
        }
        inputs.push_back(input);
    }
    actorDemand.Refresh(residencyMapGeneration, frameIndex, inputs, captureUpgrades, capturePoseRequests);
    uint32_t coalesced = 0;
    if (policy.coalesce)
    {
        for (auto it = admissionQueue.begin(); it != admissionQueue.end();)
        {
            const auto& entry = it->second;
            if (entry.mapGeneration == residencyMapGeneration && entry.runtimeRequested && !entry.sourceBits &&
                !entry.gpuForce && !entry.gpuPrefer && !IsRequiredAdmission(entry) &&
                !actorDemand.Pair(entry.pairKey).consumers && Unstarted(entry))
            {
                admissionIndex.Remove(it->first, GetAdmissionBucket(entry));
                DiscardAdmissionEntry(it->second, services);
                it = admissionQueue.erase(it);
                coalesced++;
            }
            else ++it;
        }
        if (coalesced) MarkMaintenanceMutation();
    }
    if (PerfAuditActive())
    {
        const auto& snapshot = actorDemand.Snapshot();
        char fields[640];
        snprintf(fields, sizeof(fields), "\"renderer_frame\":%u,\"map_epoch\":%u,\"actors\":%u,\"pairs\":%u,"
            "\"missing\":%u,\"sprite\":%u,\"held\":%u,\"exact\":%u,\"oldest_exact_wait_frames\":%u,"
            "\"warm_mesh_records\":%u,\"warm_material_records\":%u,\"coalesced_unstarted\":%u,"
            "\"actor_priority\":%s,\"coalesce\":%s,\"warm_retention\":%s",
            frameIndex, residencyMapGeneration, snapshot.actors, snapshot.pairs, snapshot.missing, snapshot.sprite,
            snapshot.held, snapshot.exact, snapshot.oldestExactWait, snapshot.warmMeshes, snapshot.warmMaterials,
            coalesced, policy.actorPriority ? "true" : "false", policy.coalesce ? "true" : "false", policy.warmRetention ? "true" : "false");
        PerfAuditWriteEvent("voxel_actor_demand", PerfAuditCurrentToken(), fields);
    }
}

NRIVoxelOptionalGrowth::Demand NRIPersistentVoxelResidency::GeometryGrowthDemand(uint64_t mesh, uint32_t frame, bool includeRequiredJobs) const
{
    NRIVoxelOptionalGrowth::Demand demand;
    demand.authoritative = actorDemand.HasCurrentMeshAuthority(frame);
    // requestedConsumers includes retained and indirect-only sources, even
    // when the mesh is already exact or no direct-view upgrade is needed.
    demand.consumers = actorDemand.Upgrade(mesh, frame).requestedConsumers;
    auto active = activeMeshReferences.find(mesh);
    demand.active = active == activeMeshReferences.end() ? 0 : active->second;
    const auto& pins = enemyPolicy.MeshPins();
    auto family = pins.find(mesh);
    demand.family = family != pins.end() && family->second != 0;
    demand.staged = enemyFamilyStaging.MeshPins().count(mesh) != 0 || enemyFamilyRecovery.pins.count(mesh) != 0;
    if (includeRequiredJobs)
    {
        demand.authoritative &= requiredGrowthFrame == frame && requiredGrowthMap == residencyMapGeneration;
        demand.requiredJob = std::binary_search(requiredGrowthMeshes.begin(), requiredGrowthMeshes.end(), mesh);
    }
    return demand;
}

NRIVoxelOptionalGrowth::Replacement NRIPersistentVoxelResidency::GeometryGrowthReplacement() const
{
    NRIVoxelOptionalGrowth::Replacement replacement;
    const auto stream = [&](const NRIBufferResource& backing, uint64_t required)
    {
        if (required <= backing.size) return;
        replacement.oldBytes = NRIVoxelComputeSaturatingAdd(replacement.oldBytes, backing.memorySize);
        replacement.minimumBytes = NRIVoxelComputeSaturatingAdd(replacement.minimumBytes, required);
    };
    stream(vertexBuffer, uint64_t(arenaVertexCursor) * sizeof(nri_scene::SceneVertex));
    stream(indexBuffer, uint64_t(arenaIndexCursor) * sizeof(uint32_t));
    stream(primitiveBuffer, uint64_t(arenaPrimitiveCursor) * sizeof(nri_scene::PrimitiveData));
    return replacement;
}

void NRIPersistentVoxelResidency::ReportGeometryReplacementConsumers(uint64_t mesh, uint32_t frame,
    const NRIVoxelGeometryAdmissionIntent& intent, uint64_t oldArenaBytes, uint64_t newArenaBytes) const
{
    const auto demand = GeometryGrowthDemand(mesh, frame, true);
    char fields[512];
    snprintf(fields, sizeof(fields), "\"map_epoch\":%u,\"renderer_frame\":%u,\"mesh_key\":%llu,\"pair_key\":%llu,"
        "\"pure_unowned_gameplay\":%s,\"current_authority\":%s,\"requested_consumers\":%u,\"active_references\":%u,"
        "\"family_pinned\":%u,\"family_staged\":%u,\"required_job\":%u,\"old_arena_bytes\":%llu,\"new_arena_bytes\":%llu",
        residencyMapGeneration, frame, (unsigned long long)mesh, (unsigned long long)intent.freshDirectPair,
        intent.IsUnownedGameplayRequest() ? "true" : "false", demand.authoritative ? "true" : "false",
        demand.consumers, demand.active, unsigned(demand.family), unsigned(demand.staged), unsigned(demand.requiredJob),
        (unsigned long long)oldArenaBytes, (unsigned long long)newArenaBytes);
    PerfAuditWriteEvent("voxel_replacement_consumers", PerfAuditCurrentToken(), fields);
}

bool NRIPersistentVoxelResidency::ParkSupersededGeometryGrowth(PersistentVoxelMeshVariantResource& mesh, uint32_t frame,
    const NRIVoxelGeometryAdmissionIntent& intent, bool growsArena, bool fundedOrdinaryFit) const
{
    NRIVoxelOptionalGrowth::Demand demand;
    NRIVoxelOptionalGrowth::Replacement replacement;
    const char* bypass = "other-work";
    const bool preserveFit = (bool)nri_ptvoxelarenaposepriority && fundedOrdinaryFit && intent.afterHandoff;
    if ((((bool)nri_ptvoxelparksupersededgrowth && growsArena) || preserveFit) && intent.IsUnownedGameplayRequest() &&
        !loadingWarmupActive && !preloadPending && !retentionQuarantined && mesh.resourceKey)
    {
        if (growsArena) replacement = GeometryGrowthReplacement();
        // A fitting lease in a funded partial arena may be the last contiguous
        // span for a visible pose. Do not spend it on an abandoned actor request.
        if (preserveFit || replacement.oldBytes >= NRIVoxelOptionalGrowth::MinimumCopyBytes)
        {
            demand = GeometryGrowthDemand(mesh.resourceKey, frame, true);
            bypass = demand.Bypass();
        }
        else bypass = "existing-or-small";
    }
    const bool park = !bypass;
    if (park) PerfAuditAdd(PerfAuditMetric::VoxelSupersededGrowthDeferrals);
    // This flag records an episode. Each new pump rechecks actual range fit
    // and current ownership, including any source or family that just arrived.
    if (mesh.supersededGrowthParked != park && PerfAuditActive())
    {
        char fields[512];
        snprintf(fields, sizeof(fields), "\"map_epoch\":%u,\"renderer_frame\":%u,\"mesh_key\":%llu,\"pair_key\":%llu,"
            "\"action\":\"%s\",\"reason\":\"%s\",\"current_authority\":%s,\"requested_consumers\":%u,\"active_references\":%u,"
            "\"family_pinned\":%u,\"family_staged\":%u,\"required_job\":%u,\"old_arena_bytes\":%llu,\"minimum_arena_bytes\":%llu",
            residencyMapGeneration, frame, (unsigned long long)mesh.resourceKey, (unsigned long long)intent.freshDirectPair,
            park ? "park" : "resume", park ? (preserveFit ? "preserve-funded-fit" : "superseded-gameplay-pose") : bypass,
            demand.authoritative ? "true" : "false",
            demand.consumers, demand.active, unsigned(demand.family), unsigned(demand.staged), unsigned(demand.requiredJob),
            (unsigned long long)replacement.oldBytes, (unsigned long long)replacement.minimumBytes);
        PerfAuditWriteEvent("voxel_superseded_growth", PerfAuditCurrentToken(), fields);
    }
    mesh.supersededGrowthParked = park;
    return park;
}
