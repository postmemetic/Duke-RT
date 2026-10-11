#include "nri_static_scene.h"
#include "nri_runtime_mutation_shared.h"
#include "../scene/nri_scene_math.h"
#include "../../hwrenderer/data/hw_clock.h"

#include <algorithm>

bool nri_static_scene::BuildResidentStaticMaterialBridgeFromChunks(
	const StaticMapSceneCache& staticScene,
	const StaticMapChunkAtlas& atlas,
	nri_scene::MaterialBridgeData& outBridge,
	bool traceFailures)
{
	if (!atlas.valid || atlas.chunks.size() != staticScene.chunks.size())
	{
		return false;
	}

	nri_scene::MaterialBridgeData bridge = {};
	std::vector<uint32_t> chunkListIndices;
	chunkListIndices.reserve(staticScene.chunks.size());
	for (uint32_t chunkListIndex = 0; chunkListIndex < staticScene.chunks.size(); ++chunkListIndex)
	{
		const auto& chunkCache = staticScene.chunks[chunkListIndex];
		const auto& atlasChunk = atlas.chunks[chunkListIndex];
		if (!chunkCache.active || !atlasChunk.valid || atlasChunk.materialCount == 0)
		{
			continue;
		}

		chunkListIndices.push_back(chunkListIndex);
	}

	std::sort(
		chunkListIndices.begin(),
		chunkListIndices.end(),
		[&atlas](uint32_t lhs, uint32_t rhs)
		{
			const auto& lhsChunk = atlas.chunks[lhs];
			const auto& rhsChunk = atlas.chunks[rhs];
			if (lhsChunk.materialOffset != rhsChunk.materialOffset)
			{
				return lhsChunk.materialOffset < rhsChunk.materialOffset;
			}

			return lhs < rhs;
		});

	for (uint32_t chunkListIndex : chunkListIndices)
	{
		const auto& chunkCache = staticScene.chunks[chunkListIndex];
		const auto& atlasChunk = atlas.chunks[chunkListIndex];

		if (bridge.materials.size() < atlasChunk.materialOffset)
		{
			bridge.materials.resize(atlasChunk.materialOffset);
			bridge.lightMetadata.resize(atlasChunk.materialOffset);
		}

		const uint32_t nextMaterialOffset = (uint32_t)bridge.materials.size();
		if (nextMaterialOffset != atlasChunk.materialOffset ||
			(uint32_t)chunkCache.materialBridge.materials.size() != atlasChunk.materialCount)
		{
			if (traceFailures)
			{
				Printf("NRI PT static scene trace: event=resident_material_bridge_failed chunk=%u atlas_offset=%u next_offset=%u atlas_count=%u bridge_count=%u\n",
					chunkCache.chunkIndex,
					atlasChunk.materialOffset,
					nextMaterialOffset,
					atlasChunk.materialCount,
					(uint32_t)chunkCache.materialBridge.materials.size());
			}
			return false;
		}

		nri_scene::AppendMaterialBridge(chunkCache.materialBridge, bridge);
	}

	if (bridge.materials.size() < atlas.materialCount)
	{
		bridge.materials.resize(atlas.materialCount);
		bridge.lightMetadata.resize(atlas.materialCount);
	}

	outBridge = std::move(bridge);
	return true;
}

bool nri_static_scene::RebuildResidentStaticMaterialBridgeFromChunks(
	StaticMapSceneCache& staticScene,
	const StaticMapChunkAtlas& atlas,
	bool traceFailures)
{
	if (!BuildResidentStaticMaterialBridgeFromChunks(staticScene, atlas, staticScene.materialBridge, traceFailures)) return false;
	++staticScene.lightBindingGeneration;
	++staticScene.materialGeneration;
	if (staticScene.materialGeneration == 0)
	{
		staticScene.materialGeneration = 1;
	}
	auto& state = staticScene.animatedMaterials;
	state.textureSlots.clear();
	state.textureSlots.reserve(staticScene.materialBridge.textures.size());
	for (uint32_t index = 0; index < staticScene.materialBridge.textures.size(); ++index)
	{
		state.textureSlots.emplace(staticScene.materialBridge.textures[index].key, index);
	}
	state.canonicalMaterialGeneration = staticScene.materialGeneration;
	state.canonicalLayoutValid = true;
	return true;
}

void nri_static_scene::UpdateAnimatedMaterialCandidate(StaticMapSceneCache& staticScene, uint32_t chunkListIndex)
{
	auto& state = staticScene.animatedMaterials;
	state.canonicalLayoutValid = false;
	const bool candidate = chunkListIndex < staticScene.chunks.size() &&
		staticScene.chunks[chunkListIndex].active && staticScene.chunks[chunkListIndex].hasAnimatedTextureCandidates;
	nri_static_material_slices::UpdateCandidate(state.candidates, chunkListIndex, candidate);
	staticScene.animatedCandidateChunkCount = (uint32_t)state.candidates.size();
}

bool nri_static_scene::RefreshStaticMapAnimatedMaterials(
	const NRIStaticSceneAnimatedMaterialRefreshInput& input,
	const NRIStaticSceneAnimatedMaterialRefreshServices& services)
{
	if (input.mapWorld == nullptr || input.staticScene == nullptr || input.atlas == nullptr) return true;
	const nri_scene::PTMapWorld& mapWorld = *input.mapWorld;
	StaticMapSceneCache& staticScene = *input.staticScene;
	const StaticMapChunkAtlas& atlas = *input.atlas;
	if (!staticScene.valid || !staticScene.texturesResident || !staticScene.buffersResident ||
		!staticScene.accelerationResident || staticScene.buildSerial != mapWorld.buildSerial) return true;
	auto& state = staticScene.animatedMaterials;
	state.lastFrame = {};
	state.changedChunks.clear();
	auto& stats = state.lastFrame;
	bool optimized = input.patchMaterials && !state.quarantined;
	const auto recover = [&](const char* reason)
	{
		return services.recoverStaticScene != nullptr && services.recoverStaticScene(services.user, reason);
	};
	const auto mismatch = [&](const char* reason)
	{
		const bool firstFailure = !state.quarantined;
		++stats.mismatches;
		state.quarantined = true;
		state.canonicalLayoutValid = false;
		optimized = false;
		if (firstFailure)
			Printf("NRI PT static material validation: mismatch=1 reason=%s quarantine=1 build=%llu\n",
				reason, (unsigned long long)staticScene.contentBuildSerial);
	};
	const auto trace = [&]()
	{
		// Compact capture alone never enables synchronous diagnostic output.
		if ((input.traceStats || input.validateMaterialPatches || stats.mismatches != 0) &&
			state.traceRows < 4096)
		{
			++state.traceRows;
			Printf("PERF pt static material cache NRI: candidates=%u visited=%u eligible=%u signatures=%u unchanged=%u clones=%u patched_chunks=%u patched_rows=%u full_rebuilds=%u candidate_checks=%u binding_checks=%u bridge_checks=%u mismatches=%u quarantined=%u\n",
				(uint32_t)state.candidates.size(), stats.candidatesVisited, stats.eligibleChunks,
				stats.signaturesChecked, stats.unchangedChunks, stats.chunksCloned, stats.chunksPatched,
				stats.rowsPatched, stats.fullRebuilds, stats.candidateChecks, stats.bindingChecks,
				stats.bridgeChecks, stats.mismatches, state.quarantined ? 1u : 0u);
		}
	};
	if (staticScene.lightChunkViews.size() != staticScene.chunks.size() || atlas.chunks.size() != staticScene.chunks.size())
		return recover("animated-refresh-layout-mismatch");
	if (input.validateMaterialPatches)
	{
		state.referenceCandidates.clear();
		for (uint32_t index = 0; index < staticScene.chunks.size(); ++index)
		{
			const auto& chunk = staticScene.chunks[index];
			if (chunk.active && chunk.hasAnimatedTextureCandidates) state.referenceCandidates.push_back(index);
		}
		++stats.candidateChecks;
		if (state.referenceCandidates != state.candidates) mismatch("candidate-membership");
	}
	bool patchedAll = optimized && state.canonicalLayoutValid &&
		state.canonicalMaterialGeneration == staticScene.materialGeneration &&
		state.canonicalTopologyRevision == mapWorld.topologyRevision;
	const auto suppress = [&](StaticMapSceneCache::ChunkCache& chunk, const char* reason)
	{
		if (chunk.animatedRefreshSuppressed) return;
		chunk.animatedRefreshSuppressed = true;
		++staticScene.animatedRefreshSuppressedChunkCount;
		if (input.registry != nullptr && chunk.chunkIndex < input.registry->entries.size() && input.registry->entries[chunk.chunkIndex].valid)
		{
			auto& entry = input.registry->entries[chunk.chunkIndex];
			entry.animatedRefreshSuppressed = true;
			++entry.animatedSuppressionEmitCount;
		}
		if (input.runtimeAnimatedSuppressionEmitCount != nullptr) ++*input.runtimeAnimatedSuppressionEmitCount;
		if (input.traceStats) Printf("NRI PT static scene anim: suppressing chunk=%u resident animated refresh (%s).\n", chunk.chunkIndex, reason);
	};
	// Choose iteration mode once: quarantining a later chunk must not reinterpret
	// the current candidate cursor as a full-map cursor or process chunks twice.
	const bool useCandidateList = optimized;
	const size_t visitCount = useCandidateList ? state.candidates.size() : staticScene.chunks.size();
	for (size_t cursor = 0; cursor < visitCount; ++cursor)
	{
		const uint32_t chunkListIndex = useCandidateList ? state.candidates[cursor] : (uint32_t)cursor;
		++stats.candidatesVisited;
		if (chunkListIndex >= staticScene.chunks.size()) return recover("animated-refresh-candidate-index-mismatch");
		auto& chunk = staticScene.chunks[chunkListIndex];
		if (chunk.chunkIndex >= mapWorld.chunks.size()) return recover("animated-refresh-layout-mismatch");
		if (!chunk.active || !chunk.hasAnimatedTextureCandidates || chunk.animatedRefreshSuppressed ||
			input.visibleChunkWords == nullptr || !nri_runtime_mutation::IsChunkMarkedVisible(*input.visibleChunkWords, chunk.chunkIndex)) continue;
		++stats.eligibleChunks;
		const auto& retainedView = staticScene.lightChunkViews[chunkListIndex];
		uint64_t cheapSignature = 0;
		bool cheapValid = false;
		if (optimized && services.resolveAnimatedBindingsForStaticMapChunk != nullptr)
		{
			cheapValid = services.resolveAnimatedBindingsForStaticMapChunk(services.user, mapWorld,
				mapWorld.chunks[chunk.chunkIndex], retainedView, state.resolvedBindings) &&
				nri_runtime_mutation::ComputeAnimatedMaterialSignatureWithBindings(retainedView, state.resolvedBindings, cheapSignature);
			++stats.signaturesChecked;
			if (cheapValid && cheapSignature == chunk.animatedMaterialSignature && !input.validateMaterialPatches)
			{
				++stats.unchangedChunks;
				continue;
			}
		}
		nri_scene::SceneView liveView = retainedView;
		++stats.chunksCloned;
		uint64_t liveSignature = 0;
		if (cheapValid && optimized && !input.validateMaterialPatches)
		{
			size_t bindingIndex = 0;
			for (auto& surface : liveView.opaqueWalls) surface.material.texture = state.resolvedBindings[bindingIndex++];
			for (auto& surface : liveView.opaqueFlats) surface.material.texture = state.resolvedBindings[bindingIndex++];
			for (auto& surface : liveView.opaqueSprites) surface.material.texture = state.resolvedBindings[bindingIndex++];
			liveSignature = cheapSignature;
		}
		else
		{
			if (services.refreshAnimatedBindingsForStaticMapChunk == nullptr ||
				!services.refreshAnimatedBindingsForStaticMapChunk(services.user, mapWorld, mapWorld.chunks[chunk.chunkIndex], liveView))
			{
				if (input.validateMaterialPatches && cheapValid) mismatch("binding-mapping");
				suppress(chunk, "surface-mapping-mismatch");
				continue;
			}
			liveSignature = nri_runtime_mutation::ComputeAnimatedMaterialSignature(liveView);
			if (input.validateMaterialPatches)
			{
				++stats.bindingChecks;
				if (cheapValid && cheapSignature != liveSignature) mismatch("binding-signature");
			}
		}
		if (liveSignature == chunk.animatedMaterialSignature)
		{
			++stats.unchangedChunks;
			continue;
		}
		const uint64_t geometrySignature = nri_runtime_mutation::ComputeAnimatedGeometrySignature(liveView);
		if (geometrySignature != chunk.animatedGeometrySignature)
		{
			++staticScene.animatedGeometryFallbackCount;
			suppress(chunk, "display-metric-mismatch");
			continue;
		}
		nri_scene::MaterialBridgeData liveMaterials;
		{
			Clocker clock(NriPTMaterialBuild);
			if (services.buildMaterialsWithActorOverrides != nullptr)
				services.buildMaterialsWithActorOverrides(services.user, liveView, liveMaterials, "static_map_anim_chunk");
		}
		if (liveMaterials.materials.size() != chunk.materialCount)
		{
			++staticScene.animatedGeometryFallbackCount;
			suppress(chunk, "material-slice-mismatch");
			continue;
		}
		const auto& slice = atlas.chunks[chunkListIndex];
		if (patchedAll && optimized && slice.valid && nri_static_material_slices::Patch(chunk.materialBridge, liveMaterials,
			slice.materialOffset, slice.materialCount, staticScene.materialBridge, state.textureSlots, state.remappedScratch))
		{
			++stats.chunksPatched;
			stats.rowsPatched += slice.materialCount;
		}
		else patchedAll = false;
		staticScene.lightChunkViews[chunkListIndex] = std::move(liveView);
		++chunk.lightGeneration;
		chunk.materialBridge = std::move(liveMaterials);
		chunk.animatedMaterialSignature = liveSignature;
		state.changedChunks.push_back(chunkListIndex);
	}
	if (state.changedChunks.empty())
	{
		trace();
		return true;
	}
	patchedAll = patchedAll && optimized;
	if (patchedAll && input.validateMaterialPatches)
	{
		nri_scene::MaterialBridgeData reference;
		++stats.bridgeChecks;
		if (!BuildResidentStaticMaterialBridgeFromChunks(staticScene, atlas, reference, input.traceMaterialBridgeFailures) ||
			!nri_static_material_slices::BridgeEqual(staticScene.materialBridge, reference))
		{
			mismatch("material-bytes-or-slots");
			patchedAll = false;
		}
	}
	if (!patchedAll)
	{
		++stats.fullRebuilds;
		nri_scene::BuildMapSceneView(mapWorld, staticScene.sceneView, input.preservedSkyView);
		state.aggregateSkyPreserved = input.preservedSkyView != nullptr;
		if (!RebuildResidentStaticMaterialBridgeFromChunks(staticScene, atlas, input.traceMaterialBridgeFailures))
		{
			++staticScene.animatedGeometryFallbackCount;
			trace();
			return recover("animated-refresh-material-bridge-failed");
		}
		state.canonicalTopologyRevision = mapWorld.topologyRevision;
	}
	else
	{
		// Map-world geometry is immutable within this topology publication. Runtime
		// authority lives in lightChunkViews; do not copy it into the baseline view.
		if (input.preservedSkyView != nullptr)
		{
			staticScene.sceneView.sky = input.preservedSkyView->sky;
			nri_scene::Copy3(input.preservedSkyView->skyColor, staticScene.sceneView.skyColor);
			nri_scene::Copy3(input.preservedSkyView->groundColor, staticScene.sceneView.groundColor);
		}
		else if (state.aggregateSkyPreserved) nri_scene::BuildMapSceneView(mapWorld, staticScene.sceneView, nullptr);
		state.aggregateSkyPreserved = input.preservedSkyView != nullptr;
		++staticScene.lightBindingGeneration;
		if (++staticScene.materialGeneration == 0) staticScene.materialGeneration = 1;
		state.canonicalMaterialGeneration = staticScene.materialGeneration;
	}
	// Keep the established complete texture/GPU product refresh: concurrent global
	// light/actor policy changes are not yet certified for partial static uploads.
	const bool uploaded = services.ensurePaletteTexture != nullptr && services.ensurePaletteTexture(services.user, staticScene.materialBridge) &&
		services.ensureSceneTextures != nullptr && services.ensureSceneTextures(services.user, staticScene.sceneView,
			staticScene.materialBridge, staticScene.gpuMaterials, false, "static_map_scene_anim") &&
		services.uploadStaticMaterialAtlas != nullptr && services.uploadStaticMaterialAtlas(services.user);
	if (!uploaded)
	{
		trace();
		return recover("animated-refresh-upload-failed");
	}
	staticScene.texturesResident = true;
	staticScene.buffersResident = true;
	++staticScene.gpuUploadCount;
	staticScene.animatedRefreshCount += (uint32_t)state.changedChunks.size();
	++staticScene.animatedRefreshUploadCount;
	if (services.syncResidentRegistry != nullptr) services.syncResidentRegistry(services.user);
	if (services.markUploadedStaticMapSceneLastFrame != nullptr) services.markUploadedStaticMapSceneLastFrame(services.user);
	trace();
	return true;
}
