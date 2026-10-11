#include "nri_scene_lights.h"
#include "nri_static_scene.h"
#include "printf.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace
{
	template<class T> bool EqualLightField(const T& a, const T& b)
	{
		return std::memcmp(&a, &b, sizeof(T)) == 0;
	}

	bool EqualLightMaterial(const nri_scene::MaterialLightingMetadata& a, const nri_scene::MaterialLightingMetadata& b)
	{
		return
		EqualLightField(a.texture, b.texture) &&
		EqualLightField(a.materialKey, b.materialKey) &&
		EqualLightField(a.textureContentKey, b.textureContentKey) &&
		EqualLightField(a.glowmapContentKey, b.glowmapContentKey) &&
		EqualLightField(a.normalContentKey, b.normalContentKey) &&
		EqualLightField(a.metallicContentKey, b.metallicContentKey) &&
		EqualLightField(a.roughnessContentKey, b.roughnessContentKey) &&
		EqualLightField(a.textureId, b.textureId) &&
		EqualLightField(a.baseTextureId, b.baseTextureId) &&
		EqualLightField(a.textureIndex, b.textureIndex) &&
		EqualLightField(a.glowmapTextureIndex, b.glowmapTextureIndex) &&
		EqualLightField(a.normalTextureIndex, b.normalTextureIndex) &&
		EqualLightField(a.metallicTextureIndex, b.metallicTextureIndex) &&
		EqualLightField(a.roughnessTextureIndex, b.roughnessTextureIndex) &&
		EqualLightField(a.emissiveTextureIndex, b.emissiveTextureIndex) &&
		EqualLightField(a.paletteIndex, b.paletteIndex) &&
		EqualLightField(a.materialFlags, b.materialFlags) &&
		EqualLightField(a.lightingFlags, b.lightingFlags) &&
		EqualLightField(a.materialClass, b.materialClass) &&
		EqualLightField(a.emissiveMode, b.emissiveMode) &&
		EqualLightField(a.emissiveStableFrames, b.emissiveStableFrames) &&
		EqualLightField(a.voxelPaletteIndex, b.voxelPaletteIndex) &&
		EqualLightField(a.voxelPalettePolicyFlags, b.voxelPalettePolicyFlags) &&
		EqualLightField(a.voxelPalettePolicyApplied, b.voxelPalettePolicyApplied) &&
		EqualLightField(a.sourceType, b.sourceType) &&
		EqualLightField(a.sectorIndex, b.sectorIndex) &&
		EqualLightField(a.actorIndex, b.actorIndex) &&
		EqualLightField(a.actorOverlayRuleCount, b.actorOverlayRuleCount) &&
		EqualLightField(a.actorOverlayRuleIds, b.actorOverlayRuleIds) &&
		EqualLightField(a.shade, b.shade) &&
		EqualLightField(a.alpha, b.alpha) &&
		EqualLightField(a.lightLevel, b.lightLevel) &&
		EqualLightField(a.averageColor, b.averageColor) &&
		EqualLightField(a.glowColor, b.glowColor) &&
		EqualLightField(a.emissiveColor, b.emissiveColor) &&
		EqualLightField(a.emissiveIntensity, b.emissiveIntensity) &&
		EqualLightField(a.emissiveMaskScale, b.emissiveMaskScale) &&
		EqualLightField(a.visibleFullbrightBoost, b.visibleFullbrightBoost);
	}

	bool EqualLightProvenance(const nri_scene::SurfaceProvenance& a, const nri_scene::SurfaceProvenance& b)
	{
		return
		EqualLightField(a.sourceType, b.sourceType) &&
		EqualLightField(a.sectorIndex, b.sectorIndex) &&
		EqualLightField(a.wallIndex, b.wallIndex) &&
		EqualLightField(a.sectionIndex, b.sectionIndex) &&
		EqualLightField(a.mapChunkIndex, b.mapChunkIndex) &&
		EqualLightField(a.nextSectorIndex, b.nextSectorIndex) &&
		EqualLightField(a.actorIndex, b.actorIndex) &&
		EqualLightField(a.drawListType, b.drawListType) &&
		EqualLightField(a.cstat, b.cstat) &&
		EqualLightField(a.materialFlags, b.materialFlags) &&
		EqualLightField(a.actorOverlayRuleCount, b.actorOverlayRuleCount) &&
		EqualLightField(a.actorOverlayRuleIds, b.actorOverlayRuleIds);
	}

	bool EqualLightRecord(const SceneLightSystem::SurfaceRecord& a, const SceneLightSystem::SurfaceRecord& b)
	{
		return
		EqualLightField(a.identityKey, b.identityKey) &&
		EqualLightField(a.source, b.source) &&
		EqualLightField(a.materialIndex, b.materialIndex) &&
		EqualLightField(a.center, b.center) &&
		EqualLightField(a.boundsRadius, b.boundsRadius) &&
		EqualLightField(a.surfaceArea, b.surfaceArea) &&
		EqualLightField(a.placedPrimitiveBase, b.placedPrimitiveBase) &&
		EqualLightField(a.placedPrimitiveCount, b.placedPrimitiveCount) &&
		EqualLightField(a.sceneInstanceIndex, b.sceneInstanceIndex) &&
		EqualLightField(a.occurrenceKeyLo, b.occurrenceKeyLo) &&
		EqualLightField(a.occurrenceKeyHi, b.occurrenceKeyHi) &&
		EqualLightField(a.occurrenceGeneration, b.occurrenceGeneration) &&
		EqualLightMaterial(a.material, b.material) &&
		EqualLightProvenance(a.provenance, b.provenance);
	}

	template<class F> void MeasureLightAssembly(double& target, F&& work)
	{
		const auto start = std::chrono::steady_clock::now();
		work();
		target += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	}

}

nri_scene::MaterialLightingMetadata SceneLightSystem::ResolveSurfaceMaterial(const nri_scene::MaterialBridgeData& materials, uint32_t materialIndex)
{
	nri_scene::MaterialLightingMetadata result = {};
	if (materialIndex < materials.lightMetadata.size())
	{
		result = materials.lightMetadata[materialIndex];
	}
	else if (materialIndex < materials.materials.size())
	{
		result.sectorIndex = materials.materials[materialIndex].sectorIndex != UINT32_MAX ? (int32_t)materials.materials[materialIndex].sectorIndex : -1;
		result.paletteIndex = materials.materials[materialIndex].paletteIndex;
		result.materialFlags = materials.materials[materialIndex].flags;
		result.alpha = materials.materials[materialIndex].alpha;
		result.lightLevel = materials.materials[materialIndex].lightLevel;
	}

	return result;
}

void SceneLightSystem::IndexSurfaceRecord(uint32_t recordIndex)
{
	const SurfaceRecord& record = mSurfaceRecords[recordIndex];
	if ((record.material.materialFlags & nri_scene::MaterialFlag_Sprite) != 0)
	{
		mSurfaceRecordIndex.spriteRecordsByTextureId[record.material.textureId].push_back(recordIndex);
		if (record.provenance.actorIndex >= 0)
		{
			mSurfaceRecordIndex.spriteRecordsByActorIndex[record.provenance.actorIndex].push_back(recordIndex);
			mSurfaceRecordIndex.spriteRecordsByActorTexture[((uint64_t)(uint32_t)record.provenance.actorIndex << 32u) | (uint64_t)record.material.textureId].push_back(recordIndex);
		}
	}
}

// Retained prefix slices are immutable during frame consumption. Only this
// assembly owner patches them before the dynamic tail is published.
void SceneLightSystem::RefreshStaticLightRegistry(
	const FrameAssemblyInput& input,
	const FrameAssemblyServices& services)
{
	const StaticMapSceneCache& scene = *input.staticScene;
	const size_t chunkCount = std::min(scene.lightChunkViews.size(), scene.chunks.size());
	const bool reset = mStaticLightRegistry.contentBuildSerial != scene.contentBuildSerial ||
		mStaticLightRegistry.suppressedActors != mSuppressedActorIndices;
	const bool refreshMaterials = mStaticLightRegistry.materialGeneration != scene.lightBindingGeneration;
	if (reset)
	{
		mSurfaceRecords.clear();
		mStaticLightRegistry.slices.clear();
		mStaticLightRegistry.recordCount = 0;
		mStaticLightRegistry.suppressedActors = mSuppressedActorIndices;
	}
	mStaticLightRegistry.slices.resize(chunkCount);
	uint32_t cursor = 0;
	bool rebuildSuffix = reset;
	bool changed = reset;
	for (size_t index = 0; index < chunkCount; ++index)
	{
		const auto& chunk = scene.chunks[index];
		auto& slice = mStaticLightRegistry.slices[index];
		const bool included = chunk.active &&
			(services.isRuntimeMutationReplacementActive == nullptr ||
			 !services.isRuntimeMutationReplacementActive(services.runtimeMutationUser, chunk.chunkIndex));
		const bool dirty = rebuildSuffix || !slice.valid || slice.included != included ||
			slice.chunkIndex != chunk.chunkIndex || slice.lightGeneration != chunk.lightGeneration ||
			slice.materialOffset != chunk.materialOffset || slice.materialCount != chunk.materialCount;
		if (dirty)
		{
			mStaticLightPatchScratch.clear();
			if (included)
			{
				uint32_t localMaterialIndex = 0;
				auto buildList = [&](const std::vector<nri_scene::SurfaceRef>& surfaces)
				{
					for (const auto& surface : surfaces)
					{
						const uint32_t materialIndex = chunk.materialOffset + localMaterialIndex++;
						if (!IsActorSuppressedForFrame(surface.provenance.actorIndex))
						{
							mStaticLightPatchScratch.push_back(BuildSurfaceRecord(surface, scene.materialBridge,
								SceneLightRecordSource::StaticMapScene, materialIndex, materialIndex));
						}
					}
				};
				const auto& view = scene.lightChunkViews[index];
				buildList(view.opaqueWalls);
				buildList(view.opaqueFlats);
				buildList(view.opaqueSprites);
			}
			if (!slice.valid || slice.recordCount != mStaticLightPatchScratch.size() || slice.recordOffset != cursor)
			{
				rebuildSuffix = true;
			}
			if (rebuildSuffix)
			{
				mSurfaceRecords.resize(cursor);
				mSurfaceRecords.insert(mSurfaceRecords.end(), mStaticLightPatchScratch.begin(), mStaticLightPatchScratch.end());
				mLightRegistryStats.staticRecordsCopied += (uint32_t)mStaticLightPatchScratch.size();
				changed = true;
			}
			else
			{
				for (uint32_t recordIndex = 0; recordIndex < mStaticLightPatchScratch.size(); ++recordIndex)
				{
					auto& destination = mSurfaceRecords[cursor + recordIndex];
					if (!EqualLightRecord(destination, mStaticLightPatchScratch[recordIndex]))
					{
						destination = mStaticLightPatchScratch[recordIndex];
						++mLightRegistryStats.staticRecordsCopied;
						changed = true;
					}
				}
			}
			++mLightRegistryStats.staticSlicesPatched;
			slice.recordCount = (uint32_t)mStaticLightPatchScratch.size();
		}
		else
		{
			++mLightRegistryStats.staticSlicesReused;
			if (refreshMaterials && included)
			{
				for (uint32_t recordIndex = 0; recordIndex < slice.recordCount; ++recordIndex)
				{
					auto& record = mSurfaceRecords[cursor + recordIndex];
					++mLightRegistryStats.staticMaterialRowsChecked;
					// Full material bridge rebuilds can remap unchanged chunks' texture
					// bindings. Compare metadata separately without recomputing geometry.
					const auto material = ResolveSurfaceMaterial(scene.materialBridge, record.materialIndex);
					if (!EqualLightMaterial(record.material, material))
					{
						record.material = material;
						++mLightRegistryStats.staticRecordsCopied;
						changed = true;
					}
				}
			}
		}
		slice.valid = true;
		slice.included = included;
		slice.recordOffset = cursor;
		slice.chunkIndex = chunk.chunkIndex;
		slice.lightGeneration = chunk.lightGeneration;
		slice.materialOffset = chunk.materialOffset;
		slice.materialCount = chunk.materialCount;
		cursor += slice.recordCount;
	}
	changed |= cursor != mStaticLightRegistry.recordCount;
	mSurfaceRecords.resize(cursor);
	mStaticLightRegistry.recordCount = cursor;
	mStaticLightRegistry.contentBuildSerial = scene.contentBuildSerial;
	mStaticLightRegistry.materialGeneration = scene.lightBindingGeneration;
	if (changed)
	{
		++mStaticLightRegistry.generation;
		mSurfaceRecordIndex.Clear();
		for (uint32_t index = 0; index < cursor; ++index)
		{
			IndexSurfaceRecord(index);
		}
	}
	mFrameAppendStats.totalRecordCount = cursor;
	mFrameAppendStats.staticRecordCount = cursor;
}



SceneLightSystem::FrameAssemblyTimingStats SceneLightSystem::AssembleFrameSurfaceRecordsFull(
	const FrameAssemblyInput& input,
	const FrameAssemblyServices& services)
{
	FrameAssemblyTimingStats timings = {};
	BeginFrame(input.frameSerial);
	if (input.suppressedActorIndices != nullptr)
	{
		mSuppressedActorIndices = *input.suppressedActorIndices;
	}


	if (input.usedStaticMapScene && input.staticScene != nullptr && input.staticScene->valid)
	{
		const StaticMapSceneCache& staticScene = *input.staticScene;
		const size_t chunkCount = std::min(staticScene.lightChunkViews.size(), staticScene.chunks.size());
		MeasureLightAssembly(timings.staticAppendMs, [&]()
		{
			for (size_t chunkListIndex = 0; chunkListIndex < chunkCount; ++chunkListIndex)
			{
				const auto& staticChunk = staticScene.chunks[chunkListIndex];
				if (!staticChunk.active)
				{
					continue;
				}
				const uint32_t mapChunkIndex = staticChunk.chunkIndex;
				const bool useRuntimeMutationReplacement =
					services.isRuntimeMutationReplacementActive != nullptr &&
					services.isRuntimeMutationReplacementActive(services.runtimeMutationUser, mapChunkIndex);
				if (useRuntimeMutationReplacement)
				{
					continue;
				}

				AppendSceneView(
					staticScene.lightChunkViews[chunkListIndex],
					staticScene.materialBridge,
					SceneLightRecordSource::StaticMapScene,
					staticChunk.materialOffset,
					staticChunk.materialOffset);
			}
		});

		MeasureLightAssembly(timings.runtimeMutationAppendMs, [&]()
		{
			if (services.appendRuntimeMutationSceneLightRecords != nullptr)
			{
				services.appendRuntimeMutationSceneLightRecords(services.runtimeMutationUser, *this);
			}
		});
	}
	else if (input.capturedSceneView != nullptr && input.capturedMaterials != nullptr)
	{
		MeasureLightAssembly(timings.capturedAppendMs, [&]()
		{
			AppendSceneView(*input.capturedSceneView, *input.capturedMaterials, SceneLightRecordSource::CapturedScene);
		});
	}

	AppendFrameSurfaceTail(input, services, timings);
	mSurfaceRecordIndex.PruneUnused();

	return timings;
}


void SceneLightSystem::AppendFrameSurfaceTail(const FrameAssemblyInput& input, const FrameAssemblyServices& services, FrameAssemblyTimingStats& timings)
{
	if (input.dynamicSceneView != nullptr && input.dynamicMaterials != nullptr)
	{
		MeasureLightAssembly(timings.dynamicAppendMs, [&]()
		{
			AppendSceneView(*input.dynamicSceneView, *input.dynamicMaterials, SceneLightRecordSource::DynamicScene);
		});
	}

	if (input.surfaceLightSceneView != nullptr && input.surfaceLightMaterials != nullptr)
	{
		MeasureLightAssembly(timings.surfaceLightOverlayAppendMs, [&]()
		{
			AppendSceneView(*input.surfaceLightSceneView, *input.surfaceLightMaterials, SceneLightRecordSource::SurfaceLightOverlayScene);
		});
	}

	if (input.appendPersistentVoxelSceneLights)
	{
		MeasureLightAssembly(timings.persistentVoxelAppendMs, [&]()
		{
			if (services.appendPersistentVoxelSceneLights != nullptr)
			{
				services.appendPersistentVoxelSceneLights(services.persistentVoxelUser, *this, input.frameIndex, input.voxelStats);
			}
		});
	}

}

SceneLightSystem::FrameAssemblyTimingStats SceneLightSystem::AssembleFrameSurfaceRecords(
	const FrameAssemblyInput& input,
	const FrameAssemblyServices& services)
{
	const uint64_t previousVectorBytes = mLightRegistryStats.retainedVectorBytes;
	const uint64_t highWater = mLightRegistryStats.highWaterVectorBytes;
	mLightRegistryStats = {};
	mLightRegistryStats.highWaterVectorBytes = highWater;
	FrameAssemblyTimingStats timings = {};
	const bool usesStatic = input.usedStaticMapScene && input.staticScene != nullptr && input.staticScene->valid;
	if (!input.useRegistry)
	{
		mStaticLightRegistry = {};
		timings = AssembleFrameSurfaceRecordsFull(input, services);
		mLightRegistryStats.staticRecordsCopied = mFrameAppendStats.staticRecordCount;
	}
	else if (usesStatic && mStaticLightRegistry.quarantined &&
		mStaticLightRegistry.contentBuildSerial == input.staticScene->contentBuildSerial)
	{
		timings = AssembleFrameSurfaceRecordsFull(input, services);
		mLightRegistryStats.staticRecordsCopied = mFrameAppendStats.staticRecordCount;
	}
	else
	{
		if (!usesStatic)
		{
			mStaticLightRegistry = {};
		}
		BeginFrame(input.frameSerial);
		if (input.suppressedActorIndices != nullptr)
		{
			mSuppressedActorIndices = *input.suppressedActorIndices;
		}
		if (usesStatic)
		{
			mStaticLightRegistry.quarantined = false;
			MeasureLightAssembly(timings.staticAppendMs, [&]() { RefreshStaticLightRegistry(input, services); });
			MeasureLightAssembly(timings.runtimeMutationAppendMs, [&]()
			{
				if (services.appendRuntimeMutationSceneLightRecords != nullptr)
				{
					services.appendRuntimeMutationSceneLightRecords(services.runtimeMutationUser, *this);
				}
			});
		}
		else if (input.capturedSceneView != nullptr && input.capturedMaterials != nullptr)
		{
			MeasureLightAssembly(timings.capturedAppendMs, [&]()
			{
				AppendSceneView(*input.capturedSceneView, *input.capturedMaterials, SceneLightRecordSource::CapturedScene);
			});
		}
		AppendFrameSurfaceTail(input, services, timings);
		mSurfaceRecordIndex.PruneUnused();
		if (input.validateRegistry)
		{
			ValidateFrameSurfaceRecords(input, services);
		}
	}
	mLightRegistryStats.retainedVectorBytes =
		mSurfaceRecords.capacity() * sizeof(SurfaceRecord) +
		mStaticLightPatchScratch.capacity() * sizeof(SurfaceRecord) +
		mStaticLightRegistry.slices.capacity() * sizeof(StaticLightSlice) +
		(mNextAnalyticLights.capacity() + mAnalyticLights.activeLights.capacity()) * sizeof(SceneAnalyticLight) +
		(mNextEmissiveSurfaces.capacity() + mEmissiveSurfaces.activeSurfaces.capacity()) * sizeof(EmissiveSurfaceRegistry::EmissiveSurfaceRecord) +
		(mNextAnalyticTopologyKeys.capacity() + mNextEmissiveTopologyKeys.capacity()) * sizeof(uint64_t) +
		mNextSectorTopologyKeys.capacity() * sizeof(uint32_t) + mSeenLightSectors.capacity();
	mLightRegistryStats.vectorGrowthEvents = mLightRegistryStats.retainedVectorBytes > previousVectorBytes ? 1u : 0u;
	mLightRegistryStats.highWaterVectorBytes = std::max(mLightRegistryStats.highWaterVectorBytes, mLightRegistryStats.retainedVectorBytes);
	return timings;
}

void SceneLightSystem::ValidateFrameSurfaceRecords(const FrameAssemblyInput& input, const FrameAssemblyServices& services)
{
	// Validation deliberately repeats every source's original assembly. The
	// callbacks only read current mutation/voxel state and write into this owner.
	SceneLightSystem reference;
	FrameAssemblyInput referenceInput = input;
	referenceInput.voxelStats = false;
	reference.AssembleFrameSurfaceRecordsFull(referenceInput, services);
	++mLightRegistryStats.validationChecks;
	uint32_t firstMismatch = UINT32_MAX;
	const uint32_t commonCount = (uint32_t)std::min(mSurfaceRecords.size(), reference.mSurfaceRecords.size());
	for (uint32_t index = 0; index < commonCount; ++index)
	{
		if (!EqualLightRecord(mSurfaceRecords[index], reference.mSurfaceRecords[index]))
		{
			firstMismatch = index;
			break;
		}
	}
	const bool equal = firstMismatch == UINT32_MAX && mSurfaceRecords.size() == reference.mSurfaceRecords.size() &&
		mSurfaceRecordIndex.spriteRecordsByTextureId == reference.mSurfaceRecordIndex.spriteRecordsByTextureId &&
		mSurfaceRecordIndex.spriteRecordsByActorIndex == reference.mSurfaceRecordIndex.spriteRecordsByActorIndex &&
		mSurfaceRecordIndex.spriteRecordsByActorTexture == reference.mSurfaceRecordIndex.spriteRecordsByActorTexture &&
		mPublishedActorOverlayIndices == reference.mPublishedActorOverlayIndices &&
		EqualLightField(mFrameAppendStats, reference.mFrameAppendStats);
	if (equal)
	{
		return;
	}
	++mLightRegistryStats.validationMismatches;
	Printf("NRI PT light registry mismatch: frame=%llu first=%u cached=%u full=%u action=full-assembly-quarantine\n",
		(unsigned long long)mFrameSerial, firstMismatch, (uint32_t)mSurfaceRecords.size(), (uint32_t)reference.mSurfaceRecords.size());
	mSurfaceRecords.swap(reference.mSurfaceRecords);
	mSurfaceRecordIndex = std::move(reference.mSurfaceRecordIndex);
	mPublishedActorOverlayIndices = std::move(reference.mPublishedActorOverlayIndices);
	mFrameAppendStats = reference.mFrameAppendStats;
	mStaticLightRegistry = {};
	if (input.staticScene != nullptr)
	{
		mStaticLightRegistry.contentBuildSerial = input.staticScene->contentBuildSerial;
		mStaticLightRegistry.quarantined = true;
	}
}

void SceneLightSystem::TraceLightRegistryStats() const
{
	const auto& stats = mLightRegistryStats;
	Printf("PERF pt light registry NRI: frame=%llu static_reused=%u static_patched=%u static_copied=%u material_checked=%u generation=%llu rule_hits=%u rule_rebuilds=%u validate_checks=%u validate_mismatches=%u quarantined=%u rule_validate_checks=%u rule_validate_mismatches=%u rules_quarantined=%u retained_vector_bytes=%llu high_water_vector_bytes=%llu vector_growth=%u\n",
		(unsigned long long)mFrameSerial, stats.staticSlicesReused, stats.staticSlicesPatched,
		stats.staticRecordsCopied, stats.staticMaterialRowsChecked, (unsigned long long)mStaticLightRegistry.generation,
		stats.ruleCacheHits, stats.ruleCacheRebuilds, stats.validationChecks, stats.validationMismatches,
		mStaticLightRegistry.quarantined ? 1u : 0u,
		stats.ruleValidationChecks, stats.ruleValidationMismatches, mCompiledOverlayRules.quarantined ? 1u : 0u,
		(unsigned long long)stats.retainedVectorBytes, (unsigned long long)stats.highWaterVectorBytes, stats.vectorGrowthEvents);
}
