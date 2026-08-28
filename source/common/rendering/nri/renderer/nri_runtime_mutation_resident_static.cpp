#include "nri_renderer.h"
#include "nri_cvars.h"

#include "../scene/nri_hash.h"
#include "nri_scene_upload.h"
#include "../system/nri_renderdevice.h"
#include "c_cvars.h"
#include "perf_capture.h"
#include "printf.h"

#include <chrono>
#include <unordered_map>


namespace
{
    static bool ShouldTraceResidentStaticPerf()
    {
        const bool perfLoopTraceActive = (int)perf_looptraceframes > 0;
        const bool temporalTraceActive = !!nri_pttemporaltrace && (int)nri_pttraceframes > 0;
        return perfLoopTraceActive || temporalTraceActive;
    }

    static bool ShouldCollectResidentStaticPerfTiming()
    {
		return ShouldTraceResidentStaticPerf() || (bool)nri_ptslowdowntrace || (bool)nri_ptscenestats || PerfCompactCaptureTimingActive();
    }

    static double ResidentStaticDurationMs(const std::chrono::steady_clock::time_point& start, const std::chrono::steady_clock::time_point& end)
    {
        return std::chrono::duration<double, std::milli>(end - start).count();
    }

    class ScopedPtPerfTimer
    {
    public:
        explicit ScopedPtPerfTimer(double& targetMs)
            : mTarget(ShouldCollectResidentStaticPerfTiming() ? &targetMs : nullptr)
        {
            if (mTarget != nullptr)
            {
                mStart = std::chrono::steady_clock::now();
            }
        }

        ~ScopedPtPerfTimer()
        {
            if (mTarget != nullptr)
            {
                *mTarget += ResidentStaticDurationMs(mStart, std::chrono::steady_clock::now());
            }
        }

    private:
        double* mTarget = nullptr;
        std::chrono::steady_clock::time_point mStart = {};
    };

    static const char* YesNo(bool value)
    {
        return value ? "yes" : "no";
    }

    static uint64_t ResidentStaticHashCombine64(uint64_t hash, uint64_t value)
    {
        return nri_scene::HashCombine64(hash, value);
    }

    static uint32_t ResidentStaticFloatBits(float value)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
    }

    static uint64_t HashMaterialBridgeSummary(const nri_scene::MaterialBridgeData& materials)
    {
        uint64_t hash = 1469598103934665603ull;
        hash = ResidentStaticHashCombine64(hash, (uint64_t)materials.materials.size());
        hash = ResidentStaticHashCombine64(hash, (uint64_t)materials.lightMetadata.size());
        hash = ResidentStaticHashCombine64(hash, (uint64_t)materials.textures.size());
        for (size_t i = 0; i < materials.materials.size(); ++i)
        {
            const auto& material = materials.materials[i];
            hash = ResidentStaticHashCombine64(hash, (uint64_t)material.textureIndex);
            hash = ResidentStaticHashCombine64(hash, (uint64_t)material.paletteIndex);
            hash = ResidentStaticHashCombine64(hash, (uint64_t)material.flags);
            hash = ResidentStaticHashCombine64(hash, (uint64_t)material.lightingFlags);
            hash = ResidentStaticHashCombine64(hash, (uint64_t)material.emissiveMode);
            hash = ResidentStaticHashCombine64(hash, (uint64_t)material.emissiveTextureIndex);
            hash = ResidentStaticHashCombine64(hash, (uint64_t)ResidentStaticFloatBits(material.alpha));
            hash = ResidentStaticHashCombine64(hash, (uint64_t)material.fluidBindingIndex);
        }

        for (const auto& metadata : materials.lightMetadata)
        {
            hash = ResidentStaticHashCombine64(hash, metadata.materialKey);
            hash = ResidentStaticHashCombine64(hash, (uint64_t)metadata.textureId);
            hash = ResidentStaticHashCombine64(hash, (uint64_t)metadata.actorIndex);
            hash = ResidentStaticHashCombine64(hash, (uint64_t)metadata.textureIndex);
            hash = ResidentStaticHashCombine64(hash, (uint64_t)metadata.paletteIndex);
            hash = ResidentStaticHashCombine64(hash, (uint64_t)metadata.emissiveMode);
            hash = ResidentStaticHashCombine64(hash, (uint64_t)metadata.emissiveTextureIndex);
        }

        for (const auto& texture : materials.textures)
        {
            hash = ResidentStaticHashCombine64(hash, texture.key);
            hash = ResidentStaticHashCombine64(hash, (uint64_t)texture.width);
            hash = ResidentStaticHashCombine64(hash, (uint64_t)texture.height);
            hash = ResidentStaticHashCombine64(hash, texture.indexed ? 1ull : 0ull);
        }
        return hash;
    }

    static void RemapMaterialBridgeAgainstTextureTable(
        const nri_scene::MaterialBridgeData& source,
        nri_scene::MaterialBridgeData& inOutTextureTable,
        nri_scene::MaterialBridgeData& outRemapped,
        bool* outTextureTableGrew = nullptr)
    {
        if (outTextureTableGrew != nullptr)
        {
            *outTextureTableGrew = false;
        }

        std::unordered_map<uint64_t, uint32_t> textureLookup;
        textureLookup.reserve(inOutTextureTable.textures.size() + source.textures.size());
        for (uint32_t i = 0; i < (uint32_t)inOutTextureTable.textures.size(); ++i)
        {
            textureLookup.emplace(inOutTextureTable.textures[i].key, i);
        }

        auto remapTextureIndex = [&source, &inOutTextureTable, &textureLookup, outTextureTableGrew](uint32_t textureIndex) -> uint32_t
        {
            if (textureIndex == UINT32_MAX)
            {
                return UINT32_MAX;
            }
            if (textureIndex >= source.textures.size())
            {
                return textureIndex;
            }

            const auto& texture = source.textures[textureIndex];
            auto it = textureLookup.find(texture.key);
            if (it != textureLookup.end())
            {
                return it->second;
            }

            const uint32_t newIndex = (uint32_t)inOutTextureTable.textures.size();
            textureLookup.emplace(texture.key, newIndex);
            inOutTextureTable.textures.push_back(texture);
            if (outTextureTableGrew != nullptr)
            {
                *outTextureTableGrew = true;
            }
            return newIndex;
        };

        outRemapped = {};
        outRemapped.materials.reserve(source.materials.size());
        outRemapped.lightMetadata.reserve(source.lightMetadata.size());

        for (size_t materialIndex = 0; materialIndex < source.materials.size(); ++materialIndex)
        {
            const auto& material = source.materials[materialIndex];
            nri_scene::MaterialData copy = material;
            copy.textureIndex = remapTextureIndex(material.textureIndex);
            copy.normalTextureIndex = remapTextureIndex(material.normalTextureIndex);
            copy.metallicTextureIndex = remapTextureIndex(material.metallicTextureIndex);
            copy.roughnessTextureIndex = remapTextureIndex(material.roughnessTextureIndex);
            copy.emissiveTextureIndex = remapTextureIndex(material.emissiveTextureIndex);
            outRemapped.materials.push_back(copy);

            if (materialIndex < source.lightMetadata.size())
            {
                nri_scene::MaterialLightingMetadata metadata = source.lightMetadata[materialIndex];
                metadata.textureIndex = remapTextureIndex(metadata.textureIndex);
                metadata.glowmapTextureIndex = remapTextureIndex(metadata.glowmapTextureIndex);
                metadata.normalTextureIndex = remapTextureIndex(metadata.normalTextureIndex);
                metadata.metallicTextureIndex = remapTextureIndex(metadata.metallicTextureIndex);
                metadata.roughnessTextureIndex = remapTextureIndex(metadata.roughnessTextureIndex);
                metadata.emissiveTextureIndex = remapTextureIndex(metadata.emissiveTextureIndex);
                outRemapped.lightMetadata.push_back(metadata);
            }
        }

        if (!source.paletteLookup.empty())
        {
            outRemapped.paletteLookup = source.paletteLookup;
            outRemapped.paletteWidth = source.paletteWidth;
            outRemapped.paletteHeight = source.paletteHeight;
            if (inOutTextureTable.paletteLookup.empty())
            {
                inOutTextureTable.paletteLookup = source.paletteLookup;
                inOutTextureTable.paletteWidth = source.paletteWidth;
                inOutTextureTable.paletteHeight = source.paletteHeight;
            }
        }
    }

    enum RuntimeResidentBlasRecreateFallbackBits : uint32_t
    {
        RuntimeResidentBlasRecreateFallback_NoPreviousAs = 1u << 0,
        RuntimeResidentBlasRecreateFallback_RecoveredEmpty = 1u << 1,
        RuntimeResidentBlasRecreateFallback_SliceMoved = 1u << 2,
        RuntimeResidentBlasRecreateFallback_TopologyChanged = 1u << 3,
        RuntimeResidentBlasRecreateFallback_ForceTopology = 1u << 4,
    };

    static uint32_t ScoreRuntimeResidentBlasRecreateTraceEntry(const NRIRenderer::RuntimeResidentBlasRecreateTraceEntry& entry)
    {
        uint32_t score = entry.triangleCount + entry.surfaceCount * 4u + entry.materialCount * 2u;
        if ((entry.fallbackMask & RuntimeResidentBlasRecreateFallback_TopologyChanged) != 0)
        {
            score += 4000u;
        }
        if ((entry.fallbackMask & RuntimeResidentBlasRecreateFallback_ForceTopology) != 0)
        {
            score += 3000u;
        }
        if ((entry.fallbackMask & RuntimeResidentBlasRecreateFallback_SliceMoved) != 0)
        {
            score += 2000u;
        }
        if ((entry.fallbackMask & RuntimeResidentBlasRecreateFallback_NoPreviousAs) != 0)
        {
            score += 1000u;
        }
        if ((entry.fallbackMask & RuntimeResidentBlasRecreateFallback_RecoveredEmpty) != 0)
        {
            score += 500u;
        }
        return score;
    }

    template <typename Entry, size_t N, typename ScoreFn>
    static void InsertRankedTraceEntry(std::array<Entry, N>& entries, Entry entry, ScoreFn scoreFn)
    {
        entry.score = scoreFn(entry);
        for (Entry& existing : entries)
        {
            if (!existing.valid || entry.score > existing.score)
            {
                std::swap(entry, existing);
            }
        }
    }

    static nri::AccelerationStructureBits GetStaticMapChunkBlasBuildBits()
    {
        return
            nri::AccelerationStructureBits::PREFER_FAST_TRACE |
            nri::AccelerationStructureBits::ALLOW_UPDATE;
    }

    static bool IsStaticMapChunkBlasUpdateCompatible(const NRIAccelerationStructureResource& resource)
    {
        return
            resource.buildTypeValid &&
            resource.buildType == nri::AccelerationStructureType::BOTTOM_LEVEL &&
            ((uint32_t)resource.buildFlags & (uint32_t)nri::AccelerationStructureBits::ALLOW_UPDATE) != 0 &&
            !resource.compacted;
    }

}


void NRIRenderer::NoteResidentStaticAtlasGrow()
{
	mRuntimeMutation.NoteResidentAtlasGrow();
}

bool NRIRenderer::RestoreStaticTopLevelScene()
{
	ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.restoreStaticSceneMs);
	std::vector<nri::TopLevelInstance> instances;
	std::vector<SceneInstanceData> sceneInstances;
	BuildStaticMapInstances(instances, sceneInstances);
	return
		BuildTopLevelAccelerationStructure(instances, SceneDataBufferMask_Static) &&
		NRISceneUploadManager::UpdateSceneDataSet(*this,
			mStaticVertexBuffer,
			mStaticIndexBuffer,
			mStaticPrimitiveBuffer,
			mStaticMaterialBuffer,
			// Static restoration must be independently bindable before any current
			// dynamic ring slot has been initialized. The dynamic counts are zero;
			// duplicate the static views as valid descriptor fallbacks.
			mStaticVertexBuffer,
			mStaticIndexBuffer,
			mStaticPrimitiveBuffer,
			mStaticMaterialBuffer,
			sceneInstances,
			(uint32_t)mStaticMapScene.geometry.primitives.size(),
			0u,
			(uint32_t)mStaticMapScene.gpuMaterials.size(),
			0u,
			"restore_static_scene");
}

bool NRIRenderer::RefreshResidentStaticSceneDataSet()
{
	ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.residentLightRefreshMs);
	std::vector<SceneInstanceData> sceneInstances;
	std::vector<nri::TopLevelInstance> ignoredInstances;
	BuildStaticMapInstances(ignoredInstances, sceneInstances);
	return NRISceneUploadManager::UpdateSceneDataSet(*this,
		mStaticVertexBuffer,
		mStaticIndexBuffer,
		mStaticPrimitiveBuffer,
		mStaticMaterialBuffer,
		mStaticVertexBuffer,
		mStaticIndexBuffer,
		mStaticPrimitiveBuffer,
		mStaticMaterialBuffer,
		sceneInstances,
		(uint32_t)mStaticMapScene.geometry.primitives.size(),
		0u,
		(uint32_t)mStaticMapScene.gpuMaterials.size(),
			0u,
			"refresh_resident_static_scene");
}

bool NRIRenderer::RefreshResidentStaticMaterialSlices(
	const std::vector<uint32_t>& chunkListIndices,
	const char* reason,
	const std::vector<uint32_t>* animatedApplyChunkListIndices)
{
	static constexpr size_t ResidentAnimatedMaterialSliceCacheLimit = 4;

	if (chunkListIndices.empty())
	{
		return true;
	}
	if (!mStaticMapScene.valid ||
		!mStaticMapScene.buffersResident ||
		!mStaticMapChunkAtlas.valid)
	{
		if (nri_ptscenestats)
		{
			Printf("NRI PT static scene trace: event=resident_material_refresh_failed reason=precondition scene_valid=%s buffers=%s atlas=%s chunks=%u\n",
				YesNo(mStaticMapScene.valid),
				YesNo(mStaticMapScene.buffersResident),
				YesNo(mStaticMapChunkAtlas.valid),
				(uint32_t)chunkListIndices.size());
		}
		return false;
	}

	bool textureTableGrew = false;
	const bool staticMaterialsUseStableTextureSlots =
		mStaticMapScene.gpuMaterialsUseStableTextureSlots;
	for (uint32_t chunkListIndex : chunkListIndices)
	{
		if (chunkListIndex >= mStaticMapScene.chunks.size() ||
			chunkListIndex >= mStaticMapChunkAtlas.chunks.size())
		{
			if (nri_ptscenestats)
			{
				Printf("NRI PT static scene trace: event=resident_material_refresh_failed reason=chunk-index-oob chunk_list_index=%u scene_chunks=%u atlas_chunks=%u\n",
					chunkListIndex,
					(uint32_t)mStaticMapScene.chunks.size(),
					(uint32_t)mStaticMapChunkAtlas.chunks.size());
			}
			return false;
		}

		auto& chunkCache = mStaticMapScene.chunks[chunkListIndex];
		const auto& atlasChunk = mStaticMapChunkAtlas.chunks[chunkListIndex];
		if (!chunkCache.active || !atlasChunk.valid || atlasChunk.materialCount == 0)
		{
			continue;
		}
		if (atlasChunk.materialOffset + atlasChunk.materialCount > mStaticMapScene.materialBridge.materials.size() ||
			atlasChunk.materialOffset + atlasChunk.materialCount > mStaticMapScene.materialBridge.lightMetadata.size() ||
			atlasChunk.materialOffset + atlasChunk.materialCount > mStaticMapScene.gpuMaterials.size())
		{
			if (nri_ptscenestats)
			{
				Printf("NRI PT static scene trace: event=resident_material_refresh_failed reason=atlas-range-oob chunk=%u chunk_list_index=%u material_offset=%u material_count=%u bridge=%u light_meta=%u gpu=%u\n",
					chunkCache.chunkIndex,
					chunkListIndex,
					atlasChunk.materialOffset,
					atlasChunk.materialCount,
					(uint32_t)mStaticMapScene.materialBridge.materials.size(),
					(uint32_t)mStaticMapScene.materialBridge.lightMetadata.size(),
					(uint32_t)mStaticMapScene.gpuMaterials.size());
			}
			return false;
		}

		nri_scene::MaterialBridgeData remappedChunkBridge;
		std::vector<nri_scene::MaterialData> remappedGpuMaterials;
		const uint64_t materialBridgeHash = HashMaterialBridgeSummary(chunkCache.materialBridge);
		uint64_t actorOverrideHash = 0;
		uint64_t emissiveOverrideHash = 0;
		bool chunkTextureTableGrew = false;
		bool reusedCachedRemap = false;
		bool reusedCachedGpuPayload = false;
		const StaticMapSceneCache::ChunkCache::ResidentMaterialSliceCacheEntry* remapCacheEntry = nullptr;
		size_t gpuPayloadCacheEntryIndex = chunkCache.residentMaterialSliceCache.size();
		for (const auto& cacheEntry : chunkCache.residentMaterialSliceCache)
		{
			if (cacheEntry.animatedGeometrySignature != chunkCache.animatedGeometrySignature ||
				cacheEntry.animatedMaterialSignature != chunkCache.animatedMaterialSignature ||
				cacheEntry.materialBridgeHash != materialBridgeHash ||
				cacheEntry.materialCount != atlasChunk.materialCount)
			{
				continue;
			}

			if (remapCacheEntry == nullptr)
			{
				remapCacheEntry = &cacheEntry;
			}
		}
		if (remapCacheEntry != nullptr)
		{
			remappedChunkBridge = remapCacheEntry->remappedMaterialBridge;
			reusedCachedRemap = true;
		}
		const bool animatedApplyChunk =
			animatedApplyChunkListIndices != nullptr &&
			std::binary_search(
				animatedApplyChunkListIndices->begin(),
				animatedApplyChunkListIndices->end(),
				chunkListIndex);
		if (reusedCachedRemap)
		{
			mLastPerfShellTraceStats.staticAnimatedResidentSliceCacheHitCount++;
			if (animatedApplyChunk)
			{
				mLastPerfShellTraceStats.staticAnimatedResidentSliceApplyHitCount++;
			}
		}
		else
		{
			mLastPerfShellTraceStats.staticAnimatedResidentSliceCacheMissCount++;
			if (animatedApplyChunk)
			{
				mLastPerfShellTraceStats.staticAnimatedResidentSliceApplyMissCount++;
			}
			RemapMaterialBridgeAgainstTextureTable(
				chunkCache.materialBridge,
				mStaticMapScene.materialBridge,
				remappedChunkBridge,
				&chunkTextureTableGrew);
		}
		actorOverrideHash = ComputeChunkActorOverrideHash(remappedChunkBridge);
		emissiveOverrideHash = ComputeChunkEmissiveOverrideHash(remappedChunkBridge);
		for (size_t cacheIndex = 0; cacheIndex < chunkCache.residentMaterialSliceCache.size(); ++cacheIndex)
		{
			const auto& cacheEntry = chunkCache.residentMaterialSliceCache[cacheIndex];
			if (cacheEntry.animatedGeometrySignature != chunkCache.animatedGeometrySignature ||
				cacheEntry.animatedMaterialSignature != chunkCache.animatedMaterialSignature ||
				cacheEntry.materialBridgeHash != materialBridgeHash ||
				cacheEntry.materialCount != atlasChunk.materialCount ||
				cacheEntry.actorOverrideHash != actorOverrideHash ||
				cacheEntry.emissiveOverrideHash != emissiveOverrideHash ||
				cacheEntry.stableTextureSlots != staticMaterialsUseStableTextureSlots ||
				cacheEntry.textureSlotRevision != (staticMaterialsUseStableTextureSlots ? mSceneTextures.SlotTable().MappingRevision() : 0) ||
				cacheEntry.gpuMaterials.size() != atlasChunk.materialCount)
			{
				continue;
			}

			gpuPayloadCacheEntryIndex = cacheIndex;
			break;
		}
		if (gpuPayloadCacheEntryIndex < chunkCache.residentMaterialSliceCache.size())
		{
			remappedGpuMaterials = chunkCache.residentMaterialSliceCache[gpuPayloadCacheEntryIndex].gpuMaterials;
			reusedCachedGpuPayload = true;
		}
		if (reusedCachedGpuPayload)
		{
			mLastPerfShellTraceStats.staticAnimatedResidentGpuPayloadCacheHitCount++;
		}
		else
		{
			mLastPerfShellTraceStats.staticAnimatedResidentGpuPayloadCacheMissCount++;
		}
		textureTableGrew = textureTableGrew || chunkTextureTableGrew;
		if ((uint32_t)remappedChunkBridge.materials.size() != atlasChunk.materialCount ||
			(uint32_t)remappedChunkBridge.lightMetadata.size() != atlasChunk.materialCount)
		{
			if (nri_ptscenestats)
			{
				Printf("NRI PT static scene trace: event=resident_material_refresh_failed reason=material-count-mismatch chunk=%u chunk_list_index=%u remapped_materials=%u remapped_light=%u atlas_materials=%u\n",
					chunkCache.chunkIndex,
					chunkListIndex,
					(uint32_t)remappedChunkBridge.materials.size(),
					(uint32_t)remappedChunkBridge.lightMetadata.size(),
					atlasChunk.materialCount);
			}
			return false;
		}
		std::copy_n(
			remappedChunkBridge.materials.data(),
			atlasChunk.materialCount,
			mStaticMapScene.materialBridge.materials.data() + atlasChunk.materialOffset);
		std::copy_n(
			remappedChunkBridge.lightMetadata.data(),
			atlasChunk.materialCount,
			mStaticMapScene.materialBridge.lightMetadata.data() + atlasChunk.materialOffset);

		if (!reusedCachedGpuPayload)
		{
			remappedGpuMaterials = remappedChunkBridge.materials;
			ApplyEmissiveMaterialOverrides(remappedChunkBridge, remappedGpuMaterials);
			ApplyActorShadowMaterialOverrides(remappedChunkBridge, remappedGpuMaterials);
			if (staticMaterialsUseStableTextureSlots && !textureTableGrew)
			{
				NRISceneMaterialTextureSlotResolveStats resolveStats = {};
				NRIResolveSceneMaterialTextureSlots(
					mSceneTextures.SlotTable(),
					mStaticMapScene.materialBridge,
					remappedGpuMaterials,
					&resolveStats);
				if (resolveStats.requiredFallbacks != 0 || resolveStats.optionalFallbacks != 0)
				{
					if (nri_ptscenestats)
					{
						Printf("NRI PT static scene trace: event=resident_material_refresh_failed reason=stable-slot-resolution chunk=%u chunk_list_index=%u required_fallbacks=%u optional_fallbacks=%u\n",
							chunkCache.chunkIndex,
							chunkListIndex,
							resolveStats.requiredFallbacks,
							resolveStats.optionalFallbacks);
					}
					return false;
				}
			}
		}
		std::copy_n(
			remappedGpuMaterials.data(),
			atlasChunk.materialCount,
			mStaticMapScene.gpuMaterials.data() + atlasChunk.materialOffset);
		if (!textureTableGrew && (!reusedCachedRemap || !reusedCachedGpuPayload))
		{
			StaticMapSceneCache::ChunkCache::ResidentMaterialSliceCacheEntry cacheEntry = {};
			cacheEntry.animatedGeometrySignature = chunkCache.animatedGeometrySignature;
			cacheEntry.animatedMaterialSignature = chunkCache.animatedMaterialSignature;
			cacheEntry.materialBridgeHash = materialBridgeHash;
			cacheEntry.actorOverrideHash = actorOverrideHash;
			cacheEntry.emissiveOverrideHash = emissiveOverrideHash;
			cacheEntry.textureSlotRevision = staticMaterialsUseStableTextureSlots ?
				mSceneTextures.SlotTable().MappingRevision() : 0;
			cacheEntry.stableTextureSlots = staticMaterialsUseStableTextureSlots;
			cacheEntry.materialCount = atlasChunk.materialCount;
			cacheEntry.remappedMaterialBridge = remappedChunkBridge;
			cacheEntry.gpuMaterials = remappedGpuMaterials;
			if (chunkCache.residentMaterialSliceCache.size() >= ResidentAnimatedMaterialSliceCacheLimit)
			{
				chunkCache.residentMaterialSliceCache.erase(chunkCache.residentMaterialSliceCache.begin());
			}
			chunkCache.residentMaterialSliceCache.push_back(std::move(cacheEntry));
			if (!reusedCachedRemap)
			{
				mLastPerfShellTraceStats.staticAnimatedResidentSliceCacheStoreCount++;
			}
			if (!reusedCachedGpuPayload)
			{
				mLastPerfShellTraceStats.staticAnimatedResidentGpuPayloadCacheStoreCount++;
			}
		}

		if (!textureTableGrew &&
			!StageResidentBufferCopyRange(
				mStaticMaterialBuffer,
				(uint64_t)atlasChunk.materialOffset * sizeof(nri_scene::MaterialData),
				remappedGpuMaterials.data(),
				(uint64_t)atlasChunk.materialCount * sizeof(nri_scene::MaterialData),
				NRIResourceComputeShaderResourceAccess(),
				ResidentUploadKind_Material))
		{
			if (nri_ptscenestats)
			{
				Printf("NRI PT static scene trace: event=resident_material_refresh_failed reason=buffer-update chunk=%u chunk_list_index=%u material_offset=%u material_count=%u\n",
					chunkCache.chunkIndex,
					chunkListIndex,
					atlasChunk.materialOffset,
					atlasChunk.materialCount);
			}
			return false;
		}
		if (!textureTableGrew)
		{
			mLastPerfResourceTraceStats.residentChunkBatchMaterialBytes +=
				(uint64_t)atlasChunk.materialCount * sizeof(nri_scene::MaterialData);
		}
	}
	++mStaticMapScene.materialGeneration;
	if (mStaticMapScene.materialGeneration == 0)
	{
		mStaticMapScene.materialGeneration = 1;
	}

	if (!textureTableGrew)
	{
		return true;
	}

	const bool rebuiltOkay =
		EnsurePaletteTexture(mStaticMapScene.materialBridge) &&
		EnsureSceneTextures(
			mStaticMapScene.sceneView,
			mStaticMapScene.materialBridge,
			mStaticMapScene.gpuMaterials,
			false,
			reason != nullptr ? reason : "resident_runtime_mutation_static") &&
		nri_static_scene_geometry_upload::UploadStaticMapChunkMaterialAtlas(
			BuildStaticSceneGeometryUploadServices(),
			mStaticMaterialBuffer,
			mMaterialBufferStats,
			mStaticMapChunkAtlas,
			mStaticMapScene,
			mStaticMapScene.gpuMaterials);
	if (!rebuiltOkay)
	{
		if (nri_ptscenestats)
		{
			Printf("NRI PT static scene trace: event=resident_material_refresh_failed reason=texture-table-rebuild\n");
		}
	}
	return rebuiltOkay;
}

bool NRIRenderer::RebuildResidentStaticMaterialState(const char* reason)
{
	const nri_scene::SceneView* preservedSkyView =
		(mSkyEnvironment.PreservedStaticMapSky().valid && mSkyEnvironment.PreservedStaticMapSky().buildSerial == mMapWorld.buildSerial) ?
		&mSkyEnvironment.PreservedStaticMapSky().sceneView :
		nullptr;
	nri_scene::BuildMapSceneView(mMapWorld, mStaticMapScene.sceneView, preservedSkyView);
	if (!nri_static_scene::RebuildResidentStaticMaterialBridgeFromChunks(
		mStaticMapScene,
		mStaticMapChunkAtlas,
		nri_ptscenestats && ShouldTraceResidentStaticPerf()))
	{
		return false;
	}

	return
		EnsurePaletteTexture(mStaticMapScene.materialBridge) &&
		EnsureSceneTextures(
			mStaticMapScene.sceneView,
			mStaticMapScene.materialBridge,
			mStaticMapScene.gpuMaterials,
			false,
			reason != nullptr ? reason : "resident_runtime_mutation_static") &&
		nri_static_scene_geometry_upload::UploadStaticMapChunkMaterialAtlas(
			BuildStaticSceneGeometryUploadServices(),
			mStaticMaterialBuffer,
			mMaterialBufferStats,
			mStaticMapChunkAtlas,
			mStaticMapScene,
			mStaticMapScene.gpuMaterials) &&
		RefreshResidentStaticSceneDataSet();
}

bool NRIRenderer::RebuildResidentStaticMapChunkBlases(const std::vector<uint32_t>& chunkListIndices)
{
	ScopedPtPerfTimer totalPerfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyDownstreamBlasMs);
	if (chunkListIndices.empty())
	{
		return true;
	}

	auto& activeChunkListIndices = mResidentStaticBlasActiveChunkListIndices;
	activeChunkListIndices.clear();
	{
		ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyDownstreamBlasSetupMs);
		{
			ScopedPtPerfTimer filterPerfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyDownstreamBlasFilterMs);
			activeChunkListIndices.reserve(chunkListIndices.size());
			for (uint32_t chunkListIndex : chunkListIndices)
			{
				if (chunkListIndex >= mStaticMapScene.chunks.size() ||
					chunkListIndex >= mStaticMapChunkAtlas.chunks.size())
				{
					return false;
				}

				const auto& chunk = mStaticMapScene.chunks[chunkListIndex];
				const auto& atlasChunk = mStaticMapChunkAtlas.chunks[chunkListIndex];
				if (!chunk.active || !atlasChunk.valid || atlasChunk.indexCount == 0 || atlasChunk.primitiveCount == 0)
				{
					continue;
				}

				activeChunkListIndices.push_back(chunkListIndex);
			}
		}

		if (activeChunkListIndices.empty())
		{
			return true;
		}

		uint64_t maxScratchSize = 0;
		for (uint32_t chunkListIndex : activeChunkListIndices)
		{
			auto& chunk = mStaticMapScene.chunks[chunkListIndex];
			const auto& atlasChunk = mStaticMapChunkAtlas.chunks[chunkListIndex];
			const bool hadAccelerationStructure = chunk.accelerationStructure.accelerationStructure != nullptr;
			const uint64_t expectedIndexOffset = (uint64_t)atlasChunk.indexOffset * sizeof(uint32_t);
			const bool geometryDescMatches =
				chunk.residentBlasVertexBuffer == mStaticVertexBuffer.buffer &&
				chunk.residentBlasIndexBuffer == mStaticIndexBuffer.buffer &&
				chunk.residentBlasVertexNum == mStaticMapChunkAtlas.vertexCount &&
				chunk.residentBlasIndexOffset == expectedIndexOffset &&
				chunk.residentBlasIndexNum == atlasChunk.indexCount;

			const bool canUpdateResidentBlas =
				chunk.blasUpdateEligible &&
				hadAccelerationStructure &&
				IsStaticMapChunkBlasUpdateCompatible(chunk.accelerationStructure) &&
				geometryDescMatches;
			chunk.blasUpdateEligible = canUpdateResidentBlas;
			auto* accelerationStructure = chunk.accelerationStructure.accelerationStructure;
			if (chunk.residentBlasScratchSizeCacheKey != accelerationStructure)
			{
				chunk.residentBlasScratchSizeCacheKey = accelerationStructure;
				chunk.residentBlasBuildScratchSize = 0;
				chunk.residentBlasUpdateScratchSize = 0;
			}
			if (canUpdateResidentBlas)
			{
				mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasReuseCount++;
				mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasUpdateCount++;
				uint64_t scratchSize = chunk.residentBlasUpdateScratchSize;
				if (scratchSize != 0)
				{
					mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasScratchCacheHitCount++;
				}
				else
				{
					ScopedPtPerfTimer scratchPerfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyDownstreamBlasScratchMs);
					mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasScratchCacheMissCount++;
					mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasScratchQueryCount++;
					scratchSize = mFrameBuffer->mRayTracing.GetAccelerationStructureUpdateScratchBufferSize(*accelerationStructure);
					chunk.residentBlasUpdateScratchSize = scratchSize;
				}
				maxScratchSize = std::max(maxScratchSize, scratchSize);
			}
			else
			{
				mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRecreateCount++;
				uint32_t fallbackMask = 0;
				if (!hadAccelerationStructure)
				{
					fallbackMask |= RuntimeResidentBlasRecreateFallback_NoPreviousAs;
					mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRecreateNoPreviousAsCount++;
				}
				if (chunk.lastResidentBlasRecoveredEmpty)
				{
					fallbackMask |= RuntimeResidentBlasRecreateFallback_RecoveredEmpty;
					mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRecreateRecoveredEmptyCount++;
				}
				if (!chunk.lastResidentBlasKeptGeometrySlice)
				{
					fallbackMask |= RuntimeResidentBlasRecreateFallback_SliceMoved;
					mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRecreateSliceMovedCount++;
				}
				if (chunk.lastResidentBlasTopologyChanged)
				{
					fallbackMask |= RuntimeResidentBlasRecreateFallback_TopologyChanged;
					mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRecreateTopologyChangedCount++;
				}
				if (chunk.lastResidentBlasForceTopology)
				{
					fallbackMask |= RuntimeResidentBlasRecreateFallback_ForceTopology;
					mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRecreateForceTopologyCount++;
				}
				if (ShouldTraceResidentStaticPerf())
				{
					RuntimeResidentBlasRecreateTraceEntry entry = {};
					entry.valid = true;
					entry.chunkIndex = chunk.chunkIndex;
					entry.sectorIndex =
						chunk.chunkIndex < mMapWorld.chunks.size() ?
						mMapWorld.chunks[chunk.chunkIndex].sectorIndex :
						-1;
					entry.reasonMask = chunk.lastResidentBlasReasonMask;
					entry.fallbackMask = fallbackMask;
					entry.surfaceCount = chunk.lastResidentBlasSurfaceCount;
					entry.triangleCount = chunk.lastResidentBlasTriangleCount;
					entry.materialCount = chunk.lastResidentBlasMaterialCount;
					entry.forceTopology = chunk.lastResidentBlasForceTopology;
					entry.recoveredEmpty = chunk.lastResidentBlasRecoveredEmpty;
					entry.keptGeometrySlice = chunk.lastResidentBlasKeptGeometrySlice;
					entry.topologyChanged = chunk.lastResidentBlasTopologyChanged;
					entry.hadAccelerationStructure = hadAccelerationStructure;
					InsertRankedTraceEntry(
						mLastPerfShellTraceStats.runtimeResidentBlasRecreateEntries,
						entry,
						ScoreRuntimeResidentBlasRecreateTraceEntry);
				}
				{
					ScopedPtPerfTimer createPerfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyDownstreamBlasCreateMs);
					RetireResidentAccelerationStructure(chunk.accelerationStructure);
					chunk.residentBlasScratchSizeCacheKey = nullptr;
					chunk.residentBlasBuildScratchSize = 0;
					chunk.residentBlasUpdateScratchSize = 0;
					chunk.residentBlasVertexBuffer = nullptr;
					chunk.residentBlasIndexBuffer = nullptr;
					chunk.residentBlasVertexNum = 0;
					chunk.residentBlasIndexOffset = 0;
					chunk.residentBlasIndexNum = 0;

					nri::BottomLevelGeometryDesc geometryDesc = {};
					geometryDesc.flags = nri::BottomLevelGeometryBits::OPAQUE_GEOMETRY;
					geometryDesc.type = nri::BottomLevelGeometryType::TRIANGLES;
					geometryDesc.triangles.vertexBuffer = mStaticVertexBuffer.buffer;
					geometryDesc.triangles.vertexOffset = 0;
					geometryDesc.triangles.vertexNum = mStaticMapChunkAtlas.vertexCount;
					geometryDesc.triangles.vertexStride = sizeof(nri_scene::SceneVertex);
					geometryDesc.triangles.vertexFormat = nri::Format::RGB32_SFLOAT;
					geometryDesc.triangles.indexBuffer = mStaticIndexBuffer.buffer;
					geometryDesc.triangles.indexOffset = (uint64_t)atlasChunk.indexOffset * sizeof(uint32_t);
					geometryDesc.triangles.indexNum = atlasChunk.indexCount;
					geometryDesc.triangles.indexType = nri::IndexType::UINT32;

					nri::AccelerationStructureDesc blasDesc = {};
					blasDesc.type = nri::AccelerationStructureType::BOTTOM_LEVEL;
					blasDesc.flags = GetStaticMapChunkBlasBuildBits();
					blasDesc.geometryOrInstanceNum = 1;
					blasDesc.geometries = &geometryDesc;
					if (mFrameBuffer->mRayTracing.CreateCommittedAccelerationStructure(*mFrameBuffer->mDevice, nri::MemoryLocation::DEVICE, 0.0f, blasDesc, chunk.accelerationStructure.accelerationStructure) != nri::Result::SUCCESS)
					{
						return false;
					}

					nri::MemoryDesc memoryDesc = {};
					mFrameBuffer->mRayTracing.GetAccelerationStructureMemoryDesc(*chunk.accelerationStructure.accelerationStructure, nri::MemoryLocation::DEVICE, memoryDesc);
					chunk.accelerationStructure.memorySize = memoryDesc.size;
					chunk.accelerationStructure.memoryLocation = nri::MemoryLocation::DEVICE;
					chunk.accelerationStructure.buildFlags = blasDesc.flags;
					chunk.accelerationStructure.buildType = blasDesc.type;
					chunk.accelerationStructure.buildTypeValid = true;
					chunk.accelerationStructure.uncompactedMemorySize = memoryDesc.size;
					chunk.accelerationStructure.compacted = false;
				}

				{
					ScopedPtPerfTimer scratchPerfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyDownstreamBlasScratchMs);
					mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasScratchCacheMissCount++;
					mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasScratchQueryCount++;
					const uint64_t scratchSize =
						mFrameBuffer->mRayTracing.GetAccelerationStructureBuildScratchBufferSize(*chunk.accelerationStructure.accelerationStructure);
					chunk.residentBlasScratchSizeCacheKey = chunk.accelerationStructure.accelerationStructure;
					chunk.residentBlasBuildScratchSize = scratchSize;
					chunk.residentBlasUpdateScratchSize = 0;
					chunk.residentBlasVertexBuffer = mStaticVertexBuffer.buffer;
					chunk.residentBlasIndexBuffer = mStaticIndexBuffer.buffer;
					chunk.residentBlasVertexNum = mStaticMapChunkAtlas.vertexCount;
					chunk.residentBlasIndexOffset = (uint64_t)atlasChunk.indexOffset * sizeof(uint32_t);
					chunk.residentBlasIndexNum = atlasChunk.indexCount;
					maxScratchSize = std::max(maxScratchSize, scratchSize);
				}
			}
		}

		{
			ScopedPtPerfTimer scratchPerfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyDownstreamBlasScratchMs);
			if (mResidentStaticBlasScratchBuffer.buffer == nullptr || mResidentStaticBlasScratchBuffer.size < maxScratchSize)
			{
				mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasScratchGrowCount++;
				if (mResidentStaticBlasScratchBuffer.buffer != nullptr)
				{
					WaitForCommandsTracked();
				}
				DestroyBufferResource(mResidentStaticBlasScratchBuffer);
				if (!CreateBufferWithoutView(mResidentStaticBlasScratchBuffer, maxScratchSize, 16, nri::BufferUsageBits::SCRATCH_BUFFER))
				{
					return false;
				}
			}
		}
	}

	auto& blasBarriers = mResidentStaticBlasBarriers;
	blasBarriers.clear();
	{
		ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyDownstreamBlasBuildMs);
		std::vector<nri::BufferBarrierDesc> beforeUpdateBarriers;
		beforeUpdateBarriers.reserve(activeChunkListIndices.size());
		for (uint32_t chunkListIndex : activeChunkListIndices)
		{
			const auto& chunk = mStaticMapScene.chunks[chunkListIndex];
			if (!chunk.blasUpdateEligible)
			{
				continue;
			}
			nri::BufferBarrierDesc barrier = {};
			barrier.buffer = mFrameBuffer->mRayTracing.GetAccelerationStructureBuffer(*chunk.accelerationStructure.accelerationStructure);
			barrier.before = NRIResourceAccelerationStructureReadAccess();
			barrier.after = NRIResourceAccelerationStructureReadWriteAccess();
			beforeUpdateBarriers.push_back(barrier);
		}
		if (!beforeUpdateBarriers.empty())
		{
			ScopedPtPerfTimer barrierPerfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyDownstreamBlasBarrierMs);
			nri::BarrierDesc barrierDesc = {};
			barrierDesc.buffers = beforeUpdateBarriers.data();
			barrierDesc.bufferNum = (uint32_t)beforeUpdateBarriers.size();
			mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, barrierDesc);
		}
		blasBarriers.reserve(activeChunkListIndices.size());
		for (size_t i = 0; i < activeChunkListIndices.size(); ++i)
		{
			const uint32_t chunkListIndex = activeChunkListIndices[i];
			auto& chunk = mStaticMapScene.chunks[chunkListIndex];
			const auto& atlasChunk = mStaticMapChunkAtlas.chunks[chunkListIndex];

			nri::BottomLevelGeometryDesc geometryDesc = {};
			geometryDesc.flags = nri::BottomLevelGeometryBits::OPAQUE_GEOMETRY;
			geometryDesc.type = nri::BottomLevelGeometryType::TRIANGLES;
			geometryDesc.triangles.vertexBuffer = mStaticVertexBuffer.buffer;
			geometryDesc.triangles.vertexOffset = 0;
			geometryDesc.triangles.vertexNum = mStaticMapChunkAtlas.vertexCount;
			geometryDesc.triangles.vertexStride = sizeof(nri_scene::SceneVertex);
			geometryDesc.triangles.vertexFormat = nri::Format::RGB32_SFLOAT;
			geometryDesc.triangles.indexBuffer = mStaticIndexBuffer.buffer;
			geometryDesc.triangles.indexOffset = (uint64_t)atlasChunk.indexOffset * sizeof(uint32_t);
			geometryDesc.triangles.indexNum = atlasChunk.indexCount;
			geometryDesc.triangles.indexType = nri::IndexType::UINT32;

			nri::BuildBottomLevelAccelerationStructureDesc build = {};
			build.dst = chunk.accelerationStructure.accelerationStructure;
			build.src = chunk.blasUpdateEligible ? chunk.accelerationStructure.accelerationStructure : nullptr;
			build.geometries = &geometryDesc;
			build.geometryNum = 1;
			build.scratchBuffer = mResidentStaticBlasScratchBuffer.buffer;
			build.scratchOffset = 0;
			mFrameBuffer->mRayTracing.CmdBuildBottomLevelAccelerationStructures(*mFrameBuffer->mCommandBuffer, &build, 1);
			NoteWorldBlasContentChanged();
			mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasBuildCommandCount++;
			if (chunk.fixedLayoutDeformerKey != 0)
			{
				mSE29FloorDeformerRoute.NotePartialUpload(
					chunk.fixedLayoutDeformerKey,
					chunk.fixedLayoutVertexSpanCount,
					chunk.fixedLayoutPrimitiveSpanCount,
					chunk.fixedLayoutVertexBytes,
					chunk.fixedLayoutPrimitiveBytes);
				if (chunk.blasUpdateEligible)
				{
					mSE29FloorDeformerRoute.NoteBlasUpdated(chunk.fixedLayoutDeformerKey);
				}
				else
				{
					mSE29FloorDeformerRoute.NoteBlasRecreated(chunk.fixedLayoutDeformerKey);
				}
				chunk.fixedLayoutDeformerKey = 0;
				chunk.fixedLayoutVertexSpanCount = 0;
				chunk.fixedLayoutPrimitiveSpanCount = 0;
				chunk.fixedLayoutVertexBytes = 0;
				chunk.fixedLayoutPrimitiveBytes = 0;
			}

			if (i + 1 < activeChunkListIndices.size())
			{
				ScopedPtPerfTimer barrierPerfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyDownstreamBlasBarrierMs);
				nri::BufferBarrierDesc scratchBarrier = {};
				scratchBarrier.buffer = mResidentStaticBlasScratchBuffer.buffer;
				scratchBarrier.before = NRIResourceAccelerationStructureScratchAccess();
				scratchBarrier.after = NRIResourceAccelerationStructureScratchAccess();

				nri::BarrierDesc scratchBarrierDesc = {};
				scratchBarrierDesc.buffers = &scratchBarrier;
				scratchBarrierDesc.bufferNum = 1;
				mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, scratchBarrierDesc);
				mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasScratchBarrierCount++;
			}

			{
				ScopedPtPerfTimer barrierPerfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyDownstreamBlasBarrierMs);
				nri::BufferBarrierDesc barrier = {};
				barrier.buffer = mFrameBuffer->mRayTracing.GetAccelerationStructureBuffer(*chunk.accelerationStructure.accelerationStructure);
				barrier.before = NRIResourceAccelerationStructureWriteAccess();
				barrier.after = NRIResourceAccelerationStructureReadAccess();
				blasBarriers.push_back(barrier);
			}
		}

		if (!blasBarriers.empty())
		{
			ScopedPtPerfTimer barrierPerfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyDownstreamBlasBarrierMs);
			nri::BarrierDesc barrierDesc = {};
			barrierDesc.buffers = blasBarriers.data();
			barrierDesc.bufferNum = (uint32_t)blasBarriers.size();
			mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, barrierDesc);
		}
	}
	return true;
}
