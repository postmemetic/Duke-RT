#include "nri_persistent_voxels.h"
#include "nri_cvars.h"
#include "perf_audit.h"
#include "../scene/nri_hash.h"
#include <algorithm>
#include <cstdio>

CVAR(Bool, nri_ptvoxelactoradmission, false, 0)
CVAR(Bool, nri_ptvoxelcoalesce, false, 0)
CVAR(Bool, nri_ptvoxelwarmretention, false, 0)
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
        emptyAs(mesh.shadowProxy.accelerationStructure) && !mesh.geometryRange.transaction &&
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
    if (!policy.actorPriority && !policy.coalesce && !policy.warmRetention && !policy.heldMaximumFrames)
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
        // Every source-owned desired pair protects coalescing, including
        // retained/offscreen and not-yet-publication-eligible demand.
        if (!item.identityKey || !item.ownerWorldEpoch)
        { actorDemand.InvalidateAuthority(); return; }
        NRIVoxelActorDemandInput input;
        input.identity = item.identityKey; input.world = item.ownerWorldEpoch; input.lifetime = item.ownerLifetimeGeneration;
        input.mesh = BuildPersistentVoxelMeshResourceKey(item, settings); input.material = item.materialKeyHash;
        input.pair = nri_scene::HashCombine64(input.mesh, input.material);
        input.visible = item.capturedThisFrame && item.publicationEligible && !item.indirectOnly && !item.pendingRemoval;
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
    actorDemand.Refresh(residencyMapGeneration, frameIndex, inputs);
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
