#include "nri_scene_textures.h"
#include "nri_actor_representation.h"
#include "nri_enemy_representation_textures.h"
#include "nri_persistent_voxels.h"
#include "../scene/nri_actor_sprite_fallback.h"
#include "../scene/nri_material_bridge.h"
#include "../system/nri_renderdevice.h"
#include "c_cvars.h"
#include "perf_audit.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

EXTERN_CVAR(Bool, nri_ptactorspritefallback)
CVAR(Bool, nri_ptactororderedtextures, true, 0)

void NRIActorRepresentations::PrepareTextures(uint32_t frameIndex, NRIPersistentVoxelResidency& voxels,
    const NRIPersistentVoxelSettings& settings, NRIRenderDevice& device, NRISceneTextureResidency& textures, void* user,
    void (*buildMaterials)(void*, nri_scene::SceneView&, nri_scene::MaterialBridgeData&))
{
    voxels.spriteFallbackReadyActors.clear();
    voxels.spriteFallbackReadyFrame = frameIndex;
	NRIPrepareEnemyFamilyTextures(frameIndex, voxels, settings, device, textures);
    if (!(bool)nri_ptactorspritefallback || !buildMaterials) return;
    const bool trace = PerfAuditActive();
    const auto begin = trace ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    mScene = {};
    std::vector<uint64_t> identities;
    uint32_t materialCount = 0;
    const auto report = [&]()
    {
        if (!trace) return;
        char fields[256];
        std::snprintf(fields, sizeof(fields), "\"renderer_frame\":%u,\"descriptors\":%u,\"materials\":%u,\"ready_owners\":%u,\"cpu_ms\":%.6f",
            frameIndex, uint32_t(identities.size()), materialCount, uint32_t(voxels.spriteFallbackReadyActors.size()),
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count());
        PerfAuditWriteEvent("actor_representation_preflight", PerfAuditCurrentToken(), fields);
    };
    for (const auto& candidate : nri_scene::GetActorSpriteFallbackCandidates())
    {
        if (!candidate.descriptorCurrent || candidate.presentationHidden || !candidate.ownerWorldEpoch ||
            candidate.surface.vertices.size() < 3 || candidate.transientGeometryCaptured) continue;
        mScene.opaqueSprites.push_back(candidate.surface);
        identities.push_back(candidate.identityKey);
    }
    if (identities.empty()) { report(); return; }
    buildMaterials(user, mScene, mMaterials);
    materialCount = uint32_t(mMaterials.materials.size());
    // A sprite-only scene has one material row per surface, in source order.
    // Fail closed if that bridge contract changes.
    if (mMaterials.materials.size() != identities.size()) { report(); return; }
    std::vector<uint32_t> rows(identities.size());
    for (uint32_t i = 0; i < rows.size(); ++i) rows[i] = i;
    NRIActorRepresentationTextureResult result;
    textures.EnsurePriorityMaterialTextures(device, mMaterials, rows, result);
    for (uint32_t i = 0; i < identities.size(); ++i)
        if (std::find(result.unavailableMaterials.begin(), result.unavailableMaterials.end(), i) == result.unavailableMaterials.end())
            voxels.spriteFallbackReadyActors.insert(identities[i]);
    report();
}

bool NRISceneTextureResidency::EnsurePriorityMaterialTextures(NRIRenderDevice& device,
    const nri_scene::MaterialBridgeData& materials, const std::vector<uint32_t>& selectedMaterialIndices,
    NRIActorRepresentationTextureResult& result)
{
    result = {};
    const auto begin = std::chrono::steady_clock::now();
	const bool ordered = (bool)nri_ptactororderedtextures;
    std::vector<uint32_t> rows = selectedMaterialIndices;
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    std::vector<uint32_t> textures;
    textures.reserve(rows.size() * 6);
    const auto dependency = [&](uint32_t index, bool required)
    {
        if (index == UINT32_MAX && !required) return;
        if (index >= materials.textures.size()) { ++result.failures; return; }
        textures.push_back(index);
    };
    for (uint32_t row : rows)
    {
        if (row >= materials.materials.size()) { ++result.failures; continue; }
        ++result.materials;
        const auto& material = materials.materials[row];
        dependency(material.textureIndex, true);
        dependency(material.normalTextureIndex, false);
        dependency(material.metallicTextureIndex, false);
        dependency(material.roughnessTextureIndex, false);
        dependency(material.emissiveTextureIndex, false);
        if (row < materials.lightMetadata.size()) dependency(materials.lightMetadata[row].glowmapTextureIndex, false);
    }
    std::sort(textures.begin(), textures.end());
    textures.erase(std::unique(textures.begin(), textures.end()), textures.end());
    result.textures = uint32_t(textures.size());
    std::vector<bool> readyTextures(materials.textures.size(), false);
    for (uint32_t index : textures)
    {
        const auto& upload = materials.textures[index];
		uint32_t beforeIndex = FindCacheIndex(upload.key);
        NRISceneTextureClosureResult previous;
        QueryRuntimeClosure(device, upload.key, previous);
        // The established synchronous resolver retires a pending async upload
        // before replacing it. Never wait directly on an unsubmitted frame's
        // future fence here. Keep the overlapping retirement cost explicit.
		bool pending = previous.state == NRISceneTextureClosureState::Pending && beforeIndex < CachedTextures().size();
		uint64_t pendingBytes = pending ? CachedTextures()[beforeIndex].resource.memorySize : 0;
		auto pendingResource = pending ? CachedTextures()[beforeIndex].resource.texture : nullptr;
        SceneTextureResolveResult resolved;
		bool resolvedReady = ordered && ResolveActorTextureOrdered(device, upload, true, resolved);
		if (!resolvedReady)
		{
			if (ordered)
			{
				++result.emergencySync;
				// A failed order proof may leave a newly recorded pending entry.
				// Track its actual replacement rather than only the original lookup.
				beforeIndex = FindCacheIndex(upload.key);
				QueryRuntimeClosure(device, upload.key, previous);
				pending = previous.state == NRISceneTextureClosureState::Pending && beforeIndex < CachedTextures().size();
				pendingBytes = pending ? CachedTextures()[beforeIndex].resource.memorySize : 0;
				pendingResource = pending ? CachedTextures()[beforeIndex].resource.texture : nullptr;
			}
			const auto syncBegin = std::chrono::steady_clock::now();
			resolvedReady = ResolveTextureDescriptor(device, upload, true, NRISceneTextureMissPolicy::Synchronous, resolved);
			const double syncMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - syncBegin).count();
			++result.syncCalls;
			result.syncInserts += resolved.inserted;
			result.syncMs += syncMs;
			if (ordered) result.emergencySyncMs += syncMs;
		}
        const uint32_t afterIndex = FindCacheIndex(upload.key);
        const auto afterResource = afterIndex < CachedTextures().size() ? CachedTextures()[afterIndex].resource.texture : nullptr;
        if (pending && pendingResource != afterResource)
        {
            ++result.pendingReplacements;
            result.pendingRetirementBytes += pendingBytes;
        }
        if (resolved.inserted)
        {
            ++result.inserted;
            if (afterIndex < CachedTextures().size()) result.insertedDeviceBytes += CachedTextures()[afterIndex].resource.memorySize;
        }
		if (resolved.graphicsOrderedReady)
		{
			++result.orderedReady;
			result.orderedInserts += resolved.inserted;
			if (resolved.inserted) result.orderedUploadBytes += resolved.orderedUploadBytes;
			if (afterIndex < CachedTextures().size()) result.orderedPendingBytes += CachedTextures()[afterIndex].resource.memorySize;
		}
        if (!resolvedReady || resolved.pending || resolved.activeCanvasSelfReference || resolved.descriptor == nullptr)
        {
            ++result.failures;
            continue;
        }
        ++result.ready;
        readyTextures[index] = true;
    }
    for (uint32_t row : rows)
    {
        bool available = row < materials.materials.size();
        const auto ready = [&](uint32_t index, bool required)
        { return (!required && index == UINT32_MAX) || (index < readyTextures.size() && readyTextures[index]); };
        if (available)
        {
            const auto& material = materials.materials[row];
            available = ready(material.textureIndex, true) && ready(material.normalTextureIndex, false) &&
                ready(material.metallicTextureIndex, false) && ready(material.roughnessTextureIndex, false) &&
                ready(material.emissiveTextureIndex, false) &&
                (row >= materials.lightMetadata.size() || ready(materials.lightMetadata[row].glowmapTextureIndex, false));
        }
        if (!available) result.unavailableMaterials.push_back(row);
    }
    result.cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    if (PerfAuditActive())
    {
		char fields[1024];
        std::snprintf(fields, sizeof(fields), "\"materials\":%u,\"textures\":%u,\"ready\":%u,\"inserted\":%u,"
            "\"pending_replacements\":%u,\"failures\":%u,\"inserted_device_bytes\":%llu,"
			"\"pending_retirement_bytes\":%llu,\"cpu_ms\":%.6f,\"resource_views_ready\":%s,"
			"\"ordered_enabled\":%s,\"ordered_ready\":%u,\"ordered_inserts\":%u,\"ordered_upload_bytes\":%llu,"
			"\"ordered_pending_bytes\":%llu,\"sync_calls\":%u,\"sync_inserts\":%u,\"sync_ms\":%.6f,"
			"\"emergency_sync\":%u,\"emergency_sync_ms\":%.6f",
            result.materials, result.textures, result.ready, result.inserted, result.pendingReplacements, result.failures,
            (unsigned long long)result.insertedDeviceBytes, (unsigned long long)result.pendingRetirementBytes,
			result.cpuMs, result.failures == 0 ? "true" : "false", ordered ? "true" : "false",
			result.orderedReady, result.orderedInserts, (unsigned long long)result.orderedUploadBytes,
			(unsigned long long)result.orderedPendingBytes, result.syncCalls, result.syncInserts, result.syncMs,
			result.emergencySync, result.emergencySyncMs);
        PerfAuditWriteEvent("actor_representation_textures", PerfAuditCurrentToken(), fields);
    }
    return result.failures == 0;
}
