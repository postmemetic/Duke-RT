#include "nri_persistent_voxels.h"
#include "nri_persistent_voxel_publication_services.h"
#include "nri_cvars.h"
#include "perf_audit.h"
#include "../scene/nri_cpu_perf_audit.h"
#include "printf.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_set>
#include <set>

void NRIPersistentVoxelResidency::MaintainInactiveCache(const char* phase, uint32_t frameIndex,
    const NRIPersistentVoxelSettings& settings, uint64_t usageBytes, uint64_t budgetBytes,
    const NRIPersistentVoxelResetServices& services, bool authoritativeEmptyAfterWait)
{
    if (!(bool)nri_ptvoxelcachebounded || retentionQuarantined || !phase ||
        (!authoritativeEmptyAfterWait && std::strcmp(phase, "runtime") != 0) ||
        preloadPending || loadingWarmupActive || !services.cacheRetirementAllowed ||
        !services.cacheRetirementAllowed(services.user)) return;
    const uint32_t interval = uint32_t(std::max(1, (int)nri_ptvoxelcacheintervalframes));
    if (!authoritativeEmptyAfterWait && retentionLastFrame != UINT32_MAX && retentionMapGeneration == residencyMapGeneration &&
        frameIndex >= retentionLastFrame && frameIndex - retentionLastFrame < interval) return;
    retentionLastFrame = frameIndex;
    retentionMapGeneration = residencyMapGeneration;
    PerfAuditScope elapsed(PerfAuditMetric::VoxelCacheMaintenanceNs);
    PerfAuditAdd(PerfAuditMetric::VoxelCacheMaintenanceCalls);
    const auto begin = PerfAuditClockNs();
    const auto validate = [&]()
    {
        if (!(bool)nri_ptvoxelcachevalidate) return true;
        PerfAuditAdd(PerfAuditMetric::VoxelCacheInvariantChecks);
        bool valid = geometryRanges.ValidateInvariants() && materialRangeAllocator.ValidateInvariants();
        for (const auto& pair : meshVariantResources)
        {
            const auto& resource = pair.second;
            const auto& lease = resource.geometryRange;
            const auto emptySlice = [](const NRIPersistentVoxelMaterialRangeHandle& h)
            { return h.offset == 0 && h.capacity == 0 && h.generation == 0; };
            const bool unallocated = lease.transaction == 0 && emptySlice(lease.vertices) &&
                emptySlice(lease.indices) && emptySlice(lease.primitives) &&
                resource.vertexOffset == 0 && resource.indexOffset == 0 && resource.primitiveOffset == 0 &&
                resource.vertexCount == 0 && resource.indexCount == 0 && resource.primitiveCount == 0 &&
                resource.vertexCapacity == 0 && resource.indexCapacity == 0 && resource.primitiveCapacity == 0 &&
                !resource.vertexBuffer.buffer && !resource.indexBuffer.buffer && !resource.accelerationStructure.accelerationStructure &&
                !resource.shadowProxy.vertexBuffer.buffer && !resource.shadowProxy.indexBuffer.buffer &&
                !resource.shadowProxy.accelerationStructure.accelerationStructure;
            if (unallocated) continue;
            valid = valid && geometryRanges.Owns(resource.geometryRange) &&
                resource.vertexOffset == resource.geometryRange.vertices.offset &&
                resource.indexOffset == resource.geometryRange.indices.offset &&
                resource.primitiveOffset == resource.geometryRange.primitives.offset &&
                resource.vertexCapacity == resource.geometryRange.vertices.capacity &&
                resource.indexCapacity == resource.geometryRange.indices.capacity &&
                resource.primitiveCapacity == resource.geometryRange.primitives.capacity;
        }
        for (const auto& pair : materialVariantResources)
        {
            const auto& resource = pair.second;
            // try_emplace precedes deferred material closure. A completely
            // unallocated placeholder is valid; partial handles are not.
            if (resource.materialOffset == 0 && resource.materialCapacity == 0 &&
                resource.materialCount == 0 && resource.materialSlotGeneration == 0) continue;
            const auto binding = materialPayloads.FindBinding(pair.first);
            valid = valid && materialRangeAllocator.Owns(binding) &&
                binding.offset == resource.materialOffset && binding.capacity == resource.materialCapacity &&
                binding.generation == resource.materialSlotGeneration && resource.materialCount <= binding.capacity;
        }
        if (!valid)
        {
            PerfAuditAdd(PerfAuditMetric::VoxelCacheInvariantMismatches);
            Printf("NRI PT voxel cache invariant mismatch: frame=%u map=%u\n", frameIndex, residencyMapGeneration);
            retentionQuarantined = true;
        }
        return valid;
    };
    if (!validate()) return;
    const uint64_t meshLimit = uint64_t(std::max(0, (int)nri_ptvoxelcachemeshbytes));
    const uint64_t materialLimit = uint64_t(std::max(0, (int)nri_ptvoxelcachematerialbytes));
    const uint32_t materialCountLimit = uint32_t(std::max(0, (int)nri_ptvoxelcachematerialrecords));
    const uint32_t grace = authoritativeEmptyAfterWait ? 0 : uint32_t(std::max(0, (int)nri_ptvoxelcachegraceframes));
    const bool pressure = NRIVoxelBudgetPressure(usageBytes, budgetBytes, settings.residentMinHeadroomBytes) != 0;

    // Include the committed frame and current source authority. Source entries
    // include retained offscreen actors and desiredPending bindings. Admission
    // terminal Ready records are cache history, not an ownership pin.
    std::unordered_set<uint64_t> pinnedMeshes, pinnedMaterials, pendingMeshes;
    std::set<std::pair<uint64_t, uint64_t>> pinnedPairs;
    for (const auto& actor : batch.actors)
        if (actor.active) pinnedPairs.emplace(actor.meshResourceKey, actor.materialKeyHash);
    for (const auto& pair : activeMeshReferences) if (pair.second) pinnedMeshes.insert(pair.first);
    for (const auto& pair : activeMaterialReferences) if (pair.second) pinnedMaterials.insert(pair.first);
    std::vector<nri_scene::PersistentVoxelCacheEntryView> current;
    if (authoritativeEmptyAfterWait && (!batch.actors.empty() || !instances.empty() || !admissionQueue.empty())) return;
    if (!authoritativeEmptyAfterWait && !nri_scene::BuildPersistentVoxelCacheEntries(current))
    {
        PerfAuditAdd(PerfAuditMetric::VoxelCacheAuthorityUnavailable);
        return;
    }
    for (const auto& actor : current)
    {
        const uint64_t meshKey = BuildPersistentVoxelMeshResourceKey(actor, settings);
        pinnedMeshes.insert(meshKey);
        pinnedMaterials.insert(actor.materialKeyHash);
        pinnedPairs.emplace(meshKey, actor.materialKeyHash);
    }
    for (const auto& pair : admissionQueue)
    {
        const auto& admission = pair.second;
        if (admission.state == PersistentVoxelAdmissionState::Ready ||
            admission.state == PersistentVoxelAdmissionState::Failed) continue;
        const uint64_t meshKey = BuildPersistentVoxelVariantMeshResourceKey(admission.variant);
        pendingMeshes.insert(meshKey);
        pinnedMeshes.insert(meshKey);
        pinnedMaterials.insert(admission.variant.materialKeyHash);
        pinnedPairs.emplace(meshKey, admission.variant.materialKeyHash);
    }
    struct Candidate { uint64_t key, bytes; uint32_t since; };
    std::vector<Candidate> meshes, materials;
    uint64_t inactiveMeshBytes = 0, inactiveMaterialBytes = 0;
    uint32_t inactiveMaterialCount = 0;
    for (const auto& pair : meshVariantResources)
    {
        if (pinnedMeshes.count(pair.first)) { inactiveMeshSince.erase(pair.first); continue; }
        const auto& mesh = pair.second;
        const auto* shared = sharedBlasCache.Find(pair.first);
        const uint64_t bytes = mesh.vertexBuffer.memorySize + mesh.indexBuffer.memorySize + mesh.accelerationStructure.memorySize +
            mesh.shadowProxy.vertexBuffer.memorySize + mesh.shadowProxy.indexBuffer.memorySize +
            mesh.shadowProxy.accelerationStructure.memorySize + (shared ? shared->accelerationStructure.memorySize : 0) +
            uint64_t(mesh.vertexCapacity) * sizeof(nri_scene::SceneVertex) +
            uint64_t(mesh.indexCapacity) * sizeof(uint32_t) + uint64_t(mesh.primitiveCapacity) * sizeof(nri_scene::PrimitiveData);
        inactiveMeshBytes += bytes;
        const uint32_t since = inactiveMeshSince.emplace(pair.first, frameIndex).first->second;
        if (pressure || NRIVoxelInactiveAgeEligible(frameIndex, since, grace)) meshes.push_back({ pair.first, bytes, since });
    }
    for (const auto& pair : materialVariantResources)
    {
        if (pinnedMaterials.count(pair.first)) { inactiveMaterialSince.erase(pair.first); continue; }
        const uint64_t bytes = NRIAuditMaterialBridgeBytes(pair.second.materialBridge, true);
        inactiveMaterialBytes += bytes;
        inactiveMaterialCount++;
        const uint32_t since = inactiveMaterialSince.emplace(pair.first, frameIndex).first->second;
        if (pressure || NRIVoxelInactiveAgeEligible(frameIndex, since, grace)) materials.push_back({ pair.first, bytes, since });
    }
    const auto oldest = [](const Candidate& a, const Candidate& b)
    { return a.since != b.since ? a.since < b.since : a.key < b.key; };
    std::sort(meshes.begin(), meshes.end(), oldest);
    std::sort(materials.begin(), materials.end(), oldest);
    const bool trimMeshes = pressure || inactiveMeshBytes > meshLimit;
    const bool trimMaterials = pressure || inactiveMaterialBytes > materialLimit || inactiveMaterialCount > materialCountLimit;
    const uint64_t meshTarget = pressure ? 0 : NRIVoxelCacheLowTarget(meshLimit);
    const uint64_t materialTarget = pressure ? 0 : NRIVoxelCacheLowTarget(materialLimit);
    const uint64_t materialCountTarget = pressure ? 0 : NRIVoxelCacheLowTarget(materialCountLimit);
    uint32_t evictedMeshes = 0, evictedMaterials = 0, terminalPruned = 0;
    uint64_t reusableBytes = 0, cpuBytes = 0;
    std::unordered_set<uint64_t> evictedMeshKeys, evictedMaterialKeys;
    for (const auto& candidate : meshes)
    {
        // A deferred/failed all-zero placeholder has no byte weight. Remove
        // it once unpinned and aged even when the byte target is already met.
        if ((!trimMeshes || inactiveMeshBytes <= meshTarget) && candidate.bytes != 0) continue;
        auto found = meshVariantResources.find(candidate.key);
        if (found == meshVariantResources.end()) continue;
        auto& resource = found->second;
        // A compute-cache result contains absolute offsets. Remove it before
        // the reservation can retire and those offsets can belong to a new key.
        if (resource.directComputeGeneration)
            CancelNRIVoxelComputeDirectPublication(candidate.key, resource.directComputeGeneration);
        services.RetireGeometryRange(resource.geometryRange);
        services.RetireBuffer(resource.vertexBuffer);
        services.RetireBuffer(resource.indexBuffer);
        services.RetireAccelerationStructure(resource.accelerationStructure);
        services.RetireBuffer(resource.shadowProxy.vertexBuffer);
        services.RetireBuffer(resource.shadowProxy.indexBuffer);
        services.RetireAccelerationStructure(resource.shadowProxy.accelerationStructure);
        sharedBlasCache.RetireKey(candidate.key, [&](NRIAccelerationStructureResource& as) { services.RetireAccelerationStructure(as); });
        reusableBytes += uint64_t(resource.vertexCapacity) * sizeof(nri_scene::SceneVertex) +
            uint64_t(resource.indexCapacity) * sizeof(uint32_t) + uint64_t(resource.primitiveCapacity) * sizeof(nri_scene::PrimitiveData);
        inactiveMeshBytes -= candidate.bytes;
        inactiveMeshSince.erase(candidate.key);
        publishedMeshKeys.erase(candidate.key);
        evictedMeshKeys.insert(candidate.key);
        meshVariantResources.erase(found);
        evictedMeshes++;
    }
    for (const auto& candidate : materials)
    {
        if (!trimMaterials || (inactiveMaterialBytes <= materialTarget && inactiveMaterialCount <= materialCountTarget)) break;
        auto found = materialVariantResources.find(candidate.key);
        if (found == materialVariantResources.end()) continue;
        const auto& resource = found->second;
        const auto range = PersistentVoxelMaterialRangeHandle(resource);
        const bool unallocated = resource.materialOffset == 0 && resource.materialCapacity == 0 &&
            resource.materialCount == 0 && resource.materialSlotGeneration == 0;
        const auto binding = materialPayloads.FindBinding(candidate.key);
        if (!unallocated && (!materialRangeAllocator.Owns(binding) || binding.offset != range.offset ||
            binding.capacity != range.capacity || binding.generation != range.generation)) continue;
        // Remove only this occurrence alias. A live sibling may retain the
        // same physical payload; only the last alias produces a retire ticket.
        const auto retired = materialPayloads.ReleaseBinding(candidate.key);
        if (retired) services.RetireMaterialRange(retired);
        inactiveMaterialBytes -= candidate.bytes;
        inactiveMaterialCount--;
        cpuBytes += candidate.bytes;
        inactiveMaterialSince.erase(candidate.key);
        publishedMaterialKeys.erase(candidate.key);
        dirtyMaterialResourceKeys.erase(candidate.key);
        evictedMaterialKeys.insert(candidate.key);
        materialVariantResources.erase(found);
        evictedMaterials++;
    }
    // Prune orphan terminal history even if an earlier pressure pass removed
    // its backing resource. Exact live pairs protect authority; sharing a mesh
    // or material alone does not permanently pin an unrelated terminal pair.
    for (auto it = admissionQueue.begin(); it != admissionQueue.end(); )
    {
        const auto& entry = it->second;
        const uint64_t meshKey = BuildPersistentVoxelVariantMeshResourceKey(entry.variant);
        const uint64_t materialKey = entry.variant.materialKeyHash;
        const bool terminal = entry.state == PersistentVoxelAdmissionState::Ready || entry.state == PersistentVoxelAdmissionState::Failed;
        const bool backingMissing = meshVariantResources.find(meshKey) == meshVariantResources.end() ||
            materialVariantResources.find(materialKey) == materialVariantResources.end();
        if (terminal && backingMissing && !pinnedPairs.count({meshKey, materialKey}) &&
            (!entry.directComputeRequested || !pendingMeshes.count(meshKey)))
        {
            DiscardAdmissionEntry(it->second, services);
            it = admissionQueue.erase(it);
            terminalPruned++;
        }
        else ++it;
    }
    if (evictedMeshes || evictedMaterials || terminalPruned)
    {
        PerfAuditAdd(PerfAuditMetric::VoxelCacheMeshEvictions, evictedMeshes);
        PerfAuditAdd(PerfAuditMetric::VoxelCacheMaterialEvictions, evictedMaterials);
        for (auto it = instances.begin(); it != instances.end(); )
            if (evictedMeshKeys.count(it->second.meshResourceKey) || evictedMaterialKeys.count(it->second.materialKeyHash))
                it = instances.erase(it); else ++it;
        materialResourceGeneration += evictedMaterials != 0;
        PerfAuditAdd(PerfAuditMetric::VoxelCacheTerminalPruned, terminalPruned);
        ResetActorPublicationConsumer(); // It caches raw resource pointers.
        MarkMaintenanceMutation();
        RebuildAdmissionIndex(true);
        services.InvalidateSceneDataDescriptors();
    }
    if (PerfAuditActive())
    {
        char fields[1024];
        snprintf(fields, sizeof(fields), "\"renderer_frame\":%u,\"map_epoch\":%u,\"transition\":%s,\"pressure\":%s,"
            "\"pinned_meshes\":%llu,\"pinned_materials\":%llu,\"inactive_mesh_logical_bytes\":%llu,"
            "\"inactive_material_vector_bytes\":%llu,\"inactive_material_records\":%u,\"evicted_meshes\":%u,"
            "\"evicted_materials\":%u,\"terminal_pruned\":%u,\"geometry_bytes_retiring\":%llu,"
            "\"cpu_material_vector_bytes_released\":%llu,\"cpu_ms\":%.6f", frameIndex, residencyMapGeneration,
            authoritativeEmptyAfterWait ? "true" : "false",
            pressure ? "true" : "false", (unsigned long long)pinnedMeshes.size(), (unsigned long long)pinnedMaterials.size(),
            (unsigned long long)inactiveMeshBytes, (unsigned long long)inactiveMaterialBytes, inactiveMaterialCount,
            evictedMeshes, evictedMaterials, terminalPruned, (unsigned long long)reusableBytes,
            (unsigned long long)cpuBytes, double(PerfAuditClockNs() - begin) / 1e6);
        PerfAuditWriteEvent("voxel_cache_retention", PerfAuditCurrentToken(), fields);
    }
    validate();
}
