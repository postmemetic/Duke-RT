#include "nri_static_scene_diagnostics.h"

#include "nri_static_scene.h"
#include "../scene/nri_map_world.h"
#include "printf.h"

#include <algorithm>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>

void TraceStaticSceneReleaseResources(const nri::CoreInterface& core, const nri::RayTracingInterface& rt,
	const NRIStaticSceneLiveCacheDestroyInput& input, uint64_t recordingFence)
{
	const auto trace = [&](nri::Buffer* buffer, const char* owner, uint32_t index)
	{
		if (buffer == nullptr)
			return;
		char name[96];
		std::snprintf(name, sizeof(name), "Raze.StaticScene.%llu.%s[%u]",
			(unsigned long long)input.staticScene->contentBuildSerial, owner, index);
		core.SetDebugName(reinterpret_cast<nri::Object*>(buffer), name);
		Printf("NRI PT static scene release resource: name=%s object=0x%llX va=0x%llX bytes=%llu recording_fence=%llu\n",
			name, (unsigned long long)core.GetBufferNativeObject(buffer), (unsigned long long)core.GetBufferDeviceAddress(*buffer),
			(unsigned long long)core.GetBufferDesc(*buffer).size, (unsigned long long)recordingFence);
	};
	trace(input.staticVertexBuffer->buffer, "vertices", 0);
	trace(input.staticIndexBuffer->buffer, "indices", 0);
	trace(input.staticPrimitiveBuffer->buffer, "primitives", 0);
	trace(input.staticMaterialBuffer->buffer, "materials", 0);
	for (const auto& chunk : input.staticScene->chunks)
	{
		if (auto* blas = chunk.accelerationStructure.accelerationStructure)
			trace(rt.GetAccelerationStructureBuffer(*blas), "chunkBLAS", chunk.chunkIndex);
	}
	for (const auto& segment : input.staticScene->segmentBlasCache.entries)
	{
		if (auto* blas = segment.accelerationStructure.accelerationStructure)
			trace(rt.GetAccelerationStructureBuffer(*blas), "segmentBLAS", segment.firstChunkListIndex);
	}
}

namespace
{
void OverlayLiveSegmentCache(
	const NRIStaticSceneDiagnosticsInput& input,
	NRIStaticSceneDiagnosticsSnapshot& snapshot)
{
	snapshot.asStaticSegmentRegistryMappedChunks =
		input.registry != nullptr && input.registry->valid ? input.registry->mappedChunkCount : 0;
	if (input.registry != nullptr && input.registry->valid)
	{
		snapshot.asBlasStatic = input.registry->accelerationResidentChunkCount;
		snapshot.asStaticChunkOwnedBlas = input.registry->accelerationResidentChunkCount;
		snapshot.asStaticUniqueGeometrySignatures = input.registry->accelerationResidentChunkCount;
	}
	snapshot.asStaticSegmentBlas = 0;
	snapshot.asStaticSegmentCacheCandidates = 0;
	snapshot.asStaticSegmentCacheEntries = 0;
	snapshot.asStaticSegmentCacheHits = 0;
	snapshot.asStaticSegmentCacheMisses = 0;
	snapshot.asStaticSegmentCacheDuplicateRefs = 0;
	snapshot.asStaticSegmentCacheResidentBlas = 0;
	snapshot.asStaticSegmentCacheBuildsThisFrame = 0;
	snapshot.asStaticSegmentCacheBuildsLastRebuild = 0;
	snapshot.asStaticSegmentCacheInvalidations = 0;
	snapshot.asStaticSegmentCacheResidentBytes = 0;
	snapshot.asStaticSegmentCacheBlasBuildEnabled = false;
	snapshot.asStaticSegmentRouteRouted = 0;
	snapshot.asStaticSegmentRouteChunkFallback = 0;
	snapshot.asStaticSegmentRouteRejectDisabled = 0;
	snapshot.asStaticSegmentRouteRejectMissingCache = 0;
	snapshot.asStaticSegmentRouteRejectMissingBlas = 0;
	snapshot.asStaticSegmentRouteSegmentBlasRefs = 0;
	snapshot.asStaticSegmentRouteChunkBlasRefs = 0;
	if (input.staticScene == nullptr)
	{
		return;
	}

	const StaticMapSegmentBlasCache& segmentCache = input.staticScene->segmentBlasCache;
	if (!segmentCache.valid || segmentCache.buildSerial != input.staticScene->buildSerial)
	{
		return;
	}

	snapshot.asStaticSegmentCacheCandidates = segmentCache.candidateCount;
	snapshot.asStaticSegmentCacheEntries = segmentCache.entryCount;
	snapshot.asStaticSegmentCacheHits = segmentCache.cacheHits;
	snapshot.asStaticSegmentCacheMisses = segmentCache.cacheMisses;
	snapshot.asStaticSegmentCacheDuplicateRefs = segmentCache.duplicateRefs;
	snapshot.asStaticSegmentCacheResidentBlas = segmentCache.residentBlasCount;
	snapshot.asStaticSegmentCacheBuildsThisFrame =
		input.builtStaticMapSceneASLastFrame ? segmentCache.buildsThisFrame : 0;
	snapshot.asStaticSegmentCacheBuildsLastRebuild = segmentCache.buildsThisFrame;
	snapshot.asStaticSegmentCacheInvalidations = segmentCache.invalidations;
	snapshot.asStaticSegmentCacheResidentBytes = segmentCache.residentMemoryBytes;
	snapshot.asStaticSegmentCacheBlasBuildEnabled = segmentCache.blasBuildEnabled;
	snapshot.asStaticSegmentRouteRouted = segmentCache.routeStats.routedSegment;
	snapshot.asStaticSegmentRouteChunkFallback = segmentCache.routeStats.routedChunkFallback;
	snapshot.asStaticSegmentRouteRejectDisabled = segmentCache.routeStats.rejectDisabled;
	snapshot.asStaticSegmentRouteRejectMissingCache = segmentCache.routeStats.rejectMissingCache;
	snapshot.asStaticSegmentRouteRejectMissingBlas = segmentCache.routeStats.rejectMissingBlas;
	snapshot.asStaticSegmentRouteSegmentBlasRefs = segmentCache.routeStats.segmentBlasRefs;
	snapshot.asStaticSegmentRouteChunkBlasRefs = segmentCache.routeStats.chunkBlasRefs;
	snapshot.asStaticSegmentBlas = segmentCache.residentBlasCount;
}

NRIStaticSceneDiagnosticsSnapshot BuildStructuralSnapshot(
	const NRIStaticSceneDiagnosticsInput& input,
	uint64_t generation,
	std::unordered_map<uint64_t, uint32_t>& staticSegmentSignatureRefs)
{
	NRIStaticSceneDiagnosticsSnapshot snapshot = {};
	snapshot.diagnosticsGeneration = generation;
	staticSegmentSignatureRefs.clear();
	if (input.mapWorld == nullptr || input.staticScene == nullptr || input.atlas == nullptr || input.registry == nullptr)
	{
		return snapshot;
	}

	snapshot.valid = true;
	snapshot.mapBuildSerial = input.mapWorld->buildSerial;
	snapshot.staticSceneBuildSerial = input.staticScene->buildSerial;
	staticSegmentSignatureRefs.reserve(input.staticScene->chunks.size());
	std::unordered_set<uint32_t> portalChunks;
	portalChunks.reserve(input.mapWorld->portals.size() * 2u);
	snapshot.diagnosticContainersBuilt = 2;
	if (input.mapWorld->valid)
	{
		for (const nri_scene::PTMapPortal& portal : input.mapWorld->portals)
		{
			if (portal.sourceChunkIndex != UINT32_MAX)
			{
				portalChunks.insert(portal.sourceChunkIndex);
			}
			const uint64_t targetEnd64 = std::min<uint64_t>(
				(uint64_t)portal.firstTarget + portal.targetCount,
				input.mapWorld->portalTargets.size());
			for (uint32_t targetIndex = portal.firstTarget; targetIndex < targetEnd64; ++targetIndex)
			{
				const nri_scene::PTMapPortalTarget& target = input.mapWorld->portalTargets[targetIndex];
				if (target.chunkIndex != UINT32_MAX)
				{
					portalChunks.insert(target.chunkIndex);
				}
			}
		}
	}

	for (uint32_t chunkListIndex = 0; chunkListIndex < input.staticScene->chunks.size(); ++chunkListIndex)
	{
		const StaticMapSceneCache::ChunkCache& chunk = input.staticScene->chunks[chunkListIndex];
		snapshot.diagnosticChunkRowsScanned++;
		if (chunk.active && chunk.accelerationStructure.accelerationStructure != nullptr)
		{
			snapshot.asBlasStatic++;
			snapshot.asStaticChunkOwnedBlas++;
		}
		if (!chunk.active || chunk.primitiveCount == 0)
		{
			continue;
		}

		snapshot.asStaticSegmentCandidateChunks++;
		const uint64_t segmentSignature =
			chunk.exactGeometrySignature != 0 ? chunk.exactGeometrySignature :
			(chunk.geometryPayloadHash != 0 ? chunk.geometryPayloadHash : chunk.geometryTopologySignature);
		if (segmentSignature != 0)
		{
			staticSegmentSignatureRefs[segmentSignature]++;
		}
		if (chunk.hasAnimatedTextureCandidates || chunk.animatedRefreshSuppressed)
		{
			snapshot.asStaticSegmentAnimatedChunks++;
		}
		if (portalChunks.find(chunk.chunkIndex) != portalChunks.end())
		{
			snapshot.asStaticSegmentPortalChunks++;
		}
		const bool chunkInLocalSpace =
			input.mapWorld->valid &&
			chunk.chunkIndex < input.mapWorld->chunks.size() &&
			input.mapWorld->chunks[chunk.chunkIndex].localSpaceIndex != UINT32_MAX;
		if (chunkInLocalSpace)
		{
			snapshot.asStaticSegmentLocalSpaceChunks++;
		}
		const bool chunkAnimated = chunk.hasAnimatedTextureCandidates || chunk.animatedRefreshSuppressed;
		const bool chunkAtlasContiguous =
			input.atlas->valid &&
			chunkListIndex < input.atlas->chunks.size() &&
			input.atlas->chunks[chunkListIndex].valid &&
			input.atlas->chunks[chunkListIndex].primitiveCount == chunk.primitiveCount &&
			input.atlas->chunks[chunkListIndex].indexCount == chunk.indexCount &&
			input.atlas->chunks[chunkListIndex].vertexCount == chunk.vertexCount;
		if (chunkAtlasContiguous)
		{
			snapshot.asStaticSegmentAtlasEligibleChunks++;
		}

		if (!input.mapWorld->valid || chunk.chunkIndex >= input.mapWorld->chunks.size())
		{
			continue;
		}
		const nri_scene::PTMapChunk& mapChunk = input.mapWorld->chunks[chunk.chunkIndex];
		const uint64_t surfaceEnd64 = std::min<uint64_t>(
			(uint64_t)mapChunk.firstSurface + mapChunk.surfaceCount,
			input.mapWorld->surfaces.size());
		for (uint32_t surfaceIndex = mapChunk.firstSurface; surfaceIndex < surfaceEnd64; ++surfaceIndex)
		{
			const nri_scene::PTMapSurface& surface = input.mapWorld->surfaces[surfaceIndex];
			snapshot.diagnosticSurfaceRowsScanned++;
			snapshot.asStaticSegmentCandidateSurfaces++;
			switch (surface.surface.provenance.sourceType)
			{
			case nri_scene::SurfaceSourceType::MapWallBand:
				snapshot.asStaticSegmentWallCandidates++;
				break;
			case nri_scene::SurfaceSourceType::MapFloorSection:
				snapshot.asStaticSegmentFloorCandidates++;
				break;
			case nri_scene::SurfaceSourceType::MapCeilingSection:
				snapshot.asStaticSegmentCeilingCandidates++;
				break;
			case nri_scene::SurfaceSourceType::MapPortalSurface:
				snapshot.asStaticSegmentPortalCandidates++;
				break;
			default:
				break;
			}
			if (chunkInLocalSpace)
			{
				snapshot.asStaticSegmentLocalSpaceSurfaces++;
			}
			if (chunkAnimated)
			{
				snapshot.asStaticSegmentAnimatedSurfaces++;
			}
			if ((surface.surface.provenance.materialFlags &
				(nri_scene::MaterialFlag_Portal |
					nri_scene::MaterialFlag_Mirror |
					nri_scene::MaterialFlag_Sky |
					nri_scene::MaterialFlag_PlainMirror |
					nri_scene::MaterialFlag_TintEmission)) != 0)
			{
				snapshot.asStaticSegmentMaterialRiskSurfaces++;
			}
			if (chunkAtlasContiguous)
			{
				snapshot.asStaticSegmentContiguousChunkSurfaces++;
			}
		}
	}
	snapshot.asStaticUniqueGeometrySignatures = snapshot.asStaticChunkOwnedBlas;

	for (const auto& pair : staticSegmentSignatureRefs)
	{
		snapshot.asStaticSegmentUniqueGeometrySignatures++;
		if (pair.second > 1)
		{
			snapshot.asStaticSegmentDuplicateKeys++;
			snapshot.asStaticSegmentDuplicateRefs += pair.second - 1u;
		}
	}
	return snapshot;
}

bool ApplyContributionDelta(uint32_t& target, uint32_t before, uint32_t after)
{
	if (target < before)
	{
		return false;
	}
	target = target - before + after;
	return true;
}

bool RemoveSignatureRef(
	std::unordered_map<uint64_t, uint32_t>& refs,
	NRIStaticSceneDiagnosticsSnapshot& snapshot,
	uint64_t signature)
{
	if (signature == 0)
	{
		return true;
	}
	const auto it = refs.find(signature);
	if (it == refs.end() || it->second == 0)
	{
		return false;
	}
	if (it->second > 1)
	{
		snapshot.asStaticSegmentDuplicateRefs--;
		if (it->second == 2)
		{
			snapshot.asStaticSegmentDuplicateKeys--;
		}
		it->second--;
	}
	else
	{
		refs.erase(it);
		snapshot.asStaticSegmentUniqueGeometrySignatures--;
	}
	return true;
}

void AddSignatureRef(
	std::unordered_map<uint64_t, uint32_t>& refs,
	NRIStaticSceneDiagnosticsSnapshot& snapshot,
	uint64_t signature)
{
	if (signature == 0)
	{
		return;
	}
	auto [it, inserted] = refs.emplace(signature, 1u);
	if (inserted)
	{
		snapshot.asStaticSegmentUniqueGeometrySignatures++;
		return;
	}
	if (it->second == 1)
	{
		snapshot.asStaticSegmentDuplicateKeys++;
	}
	it->second++;
	snapshot.asStaticSegmentDuplicateRefs++;
}
}

NRIStaticSceneDiagnosticsCache::BuildIdentity NRIStaticSceneDiagnosticsCache::MakeBuildIdentity(
	const NRIStaticSceneDiagnosticsInput& input)
{
	BuildIdentity identity = {};
	identity.mapWorld = input.mapWorld;
	identity.staticScene = input.staticScene;
	identity.atlas = input.atlas;
	identity.registry = input.registry;
	if (input.mapWorld != nullptr)
	{
		identity.mapBuildSerial = input.mapWorld->buildSerial;
		identity.mapChunkCount = input.mapWorld->chunks.size();
		identity.mapSurfaceCount = input.mapWorld->surfaces.size();
		identity.mapPortalCount = input.mapWorld->portals.size();
		identity.mapPortalTargetCount = input.mapWorld->portalTargets.size();
		identity.mapValid = input.mapWorld->valid;
	}
	if (input.staticScene != nullptr)
	{
		identity.staticSceneBuildSerial = input.staticScene->buildSerial;
		identity.staticChunkCount = input.staticScene->chunks.size();
		identity.staticSceneValid = input.staticScene->valid;
	}
	if (input.atlas != nullptr)
	{
		identity.atlasBuildSerial = input.atlas->buildSerial;
		identity.atlasChunkCount = input.atlas->chunks.size();
		identity.atlasValid = input.atlas->valid;
	}
	if (input.registry != nullptr)
	{
		identity.registryBuildSerial = input.registry->buildSerial;
		identity.registryEntryCount = input.registry->entries.size();
		identity.registryValid = input.registry->valid;
	}
	return identity;
}

bool NRIStaticSceneDiagnosticsCache::BuildIdentity::operator==(const BuildIdentity& other) const
{
	return mapWorld == other.mapWorld &&
		staticScene == other.staticScene &&
		atlas == other.atlas &&
		registry == other.registry &&
		mapBuildSerial == other.mapBuildSerial &&
		staticSceneBuildSerial == other.staticSceneBuildSerial &&
		atlasBuildSerial == other.atlasBuildSerial &&
		registryBuildSerial == other.registryBuildSerial &&
		mapChunkCount == other.mapChunkCount &&
		mapSurfaceCount == other.mapSurfaceCount &&
		mapPortalCount == other.mapPortalCount &&
		mapPortalTargetCount == other.mapPortalTargetCount &&
		staticChunkCount == other.staticChunkCount &&
		atlasChunkCount == other.atlasChunkCount &&
		registryEntryCount == other.registryEntryCount &&
		mapValid == other.mapValid &&
		staticSceneValid == other.staticSceneValid &&
		atlasValid == other.atlasValid &&
		registryValid == other.registryValid;
}

NRIStaticSceneDiagnosticsChunkState NRIStaticSceneDiagnosticsCache::CaptureChunkState(
	const NRIStaticSceneDiagnosticsInput& input,
	uint32_t chunkListIndex)
{
	NRIStaticSceneDiagnosticsChunkState state = {};
	if (input.mapWorld == nullptr || input.staticScene == nullptr || input.atlas == nullptr ||
		chunkListIndex >= input.staticScene->chunks.size())
	{
		return state;
	}

	const StaticMapSceneCache::ChunkCache& chunk = input.staticScene->chunks[chunkListIndex];
	state.valid = true;
	state.active = chunk.active;
	state.accelerationResident = chunk.active && chunk.accelerationStructure.accelerationStructure != nullptr;
	state.animated = chunk.hasAnimatedTextureCandidates || chunk.animatedRefreshSuppressed;
	state.chunkIndex = chunk.chunkIndex;
	state.primitiveCount = chunk.primitiveCount;
	state.reuseSignature =
		chunk.active && chunk.primitiveCount != 0 ?
		(chunk.exactGeometrySignature != 0 ? chunk.exactGeometrySignature :
			(chunk.geometryPayloadHash != 0 ? chunk.geometryPayloadHash : chunk.geometryTopologySignature)) :
		0;
	state.atlasEligible =
		chunk.active &&
		chunk.primitiveCount != 0 &&
		input.atlas->valid &&
		chunkListIndex < input.atlas->chunks.size() &&
		input.atlas->chunks[chunkListIndex].valid &&
		input.atlas->chunks[chunkListIndex].primitiveCount == chunk.primitiveCount &&
		input.atlas->chunks[chunkListIndex].indexCount == chunk.indexCount &&
		input.atlas->chunks[chunkListIndex].vertexCount == chunk.vertexCount;
	if (!chunk.active || chunk.primitiveCount == 0)
	{
		return state;
	}

	state.candidateChunks = 1;
	state.animatedChunks = state.animated ? 1u : 0u;
	state.atlasEligibleChunks = state.atlasEligible ? 1u : 0u;
	bool portalChunk = false;
	if (input.mapWorld->valid)
	{
		for (const nri_scene::PTMapPortal& portal : input.mapWorld->portals)
		{
			if (portal.sourceChunkIndex == chunk.chunkIndex)
			{
				portalChunk = true;
				break;
			}
			const uint64_t targetEnd64 = std::min<uint64_t>(
				(uint64_t)portal.firstTarget + portal.targetCount,
				input.mapWorld->portalTargets.size());
			for (uint32_t targetIndex = portal.firstTarget; targetIndex < targetEnd64; ++targetIndex)
			{
				if (input.mapWorld->portalTargets[targetIndex].chunkIndex == chunk.chunkIndex)
				{
					portalChunk = true;
					break;
				}
			}
			if (portalChunk)
			{
				break;
			}
		}
	}
	state.portalChunks = portalChunk ? 1u : 0u;
	const bool chunkInLocalSpace =
		input.mapWorld->valid &&
		chunk.chunkIndex < input.mapWorld->chunks.size() &&
		input.mapWorld->chunks[chunk.chunkIndex].localSpaceIndex != UINT32_MAX;
	state.localSpaceChunks = chunkInLocalSpace ? 1u : 0u;
	if (!input.mapWorld->valid || chunk.chunkIndex >= input.mapWorld->chunks.size())
	{
		return state;
	}

	const nri_scene::PTMapChunk& mapChunk = input.mapWorld->chunks[chunk.chunkIndex];
	const uint64_t surfaceEnd64 = std::min<uint64_t>(
		(uint64_t)mapChunk.firstSurface + mapChunk.surfaceCount,
		input.mapWorld->surfaces.size());
	for (uint32_t surfaceIndex = mapChunk.firstSurface; surfaceIndex < surfaceEnd64; ++surfaceIndex)
	{
		const nri_scene::PTMapSurface& surface = input.mapWorld->surfaces[surfaceIndex];
		state.surfaceRowsInspected++;
		state.candidateSurfaces++;
		switch (surface.surface.provenance.sourceType)
		{
		case nri_scene::SurfaceSourceType::MapWallBand:
			state.wallSurfaces++;
			break;
		case nri_scene::SurfaceSourceType::MapFloorSection:
			state.floorSurfaces++;
			break;
		case nri_scene::SurfaceSourceType::MapCeilingSection:
			state.ceilingSurfaces++;
			break;
		case nri_scene::SurfaceSourceType::MapPortalSurface:
			state.portalSurfaces++;
			break;
		default:
			break;
		}
		state.localSpaceSurfaces += chunkInLocalSpace ? 1u : 0u;
		state.animatedSurfaces += state.animated ? 1u : 0u;
		state.contiguousChunkSurfaces += state.atlasEligible ? 1u : 0u;
		if ((surface.surface.provenance.materialFlags &
			(nri_scene::MaterialFlag_Portal |
				nri_scene::MaterialFlag_Mirror |
				nri_scene::MaterialFlag_Sky |
				nri_scene::MaterialFlag_PlainMirror |
				nri_scene::MaterialFlag_TintEmission)) != 0)
		{
			state.materialRiskSurfaces++;
		}
	}
	return state;
}

void NRIStaticSceneDiagnosticsCache::NotifyChunkMutation(
	const NRIStaticSceneDiagnosticsInput& input,
	uint32_t chunkListIndex,
	const NRIStaticSceneDiagnosticsChunkState& before)
{
	if (!mHasCachedStructuralSnapshot)
	{
		return;
	}
	const BuildIdentity identity = MakeBuildIdentity(input);
	if (!(identity == mCachedIdentity))
	{
		Invalidate();
		return;
	}

	const NRIStaticSceneDiagnosticsChunkState after = CaptureChunkState(input, chunkListIndex);
	mPendingChunkRowsIncrementallyUpdated++;
	mPendingSurfaceRowsIncrementallyUpdated += before.surfaceRowsInspected + after.surfaceRowsInspected;
	if (!before.valid || !after.valid)
	{
		Invalidate();
		return;
	}
	NRIStaticSceneDiagnosticsSnapshot& snapshot = mCachedStructuralSnapshot;
	const bool contributionUpdated =
		ApplyContributionDelta(snapshot.asStaticSegmentCandidateChunks, before.candidateChunks, after.candidateChunks) &&
		ApplyContributionDelta(snapshot.asStaticSegmentPortalChunks, before.portalChunks, after.portalChunks) &&
		ApplyContributionDelta(snapshot.asStaticSegmentLocalSpaceChunks, before.localSpaceChunks, after.localSpaceChunks) &&
		ApplyContributionDelta(snapshot.asStaticSegmentAnimatedChunks, before.animatedChunks, after.animatedChunks) &&
		ApplyContributionDelta(snapshot.asStaticSegmentAtlasEligibleChunks, before.atlasEligibleChunks, after.atlasEligibleChunks) &&
		ApplyContributionDelta(snapshot.asStaticSegmentCandidateSurfaces, before.candidateSurfaces, after.candidateSurfaces) &&
		ApplyContributionDelta(snapshot.asStaticSegmentWallCandidates, before.wallSurfaces, after.wallSurfaces) &&
		ApplyContributionDelta(snapshot.asStaticSegmentFloorCandidates, before.floorSurfaces, after.floorSurfaces) &&
		ApplyContributionDelta(snapshot.asStaticSegmentCeilingCandidates, before.ceilingSurfaces, after.ceilingSurfaces) &&
		ApplyContributionDelta(snapshot.asStaticSegmentPortalCandidates, before.portalSurfaces, after.portalSurfaces) &&
		ApplyContributionDelta(snapshot.asStaticSegmentLocalSpaceSurfaces, before.localSpaceSurfaces, after.localSpaceSurfaces) &&
		ApplyContributionDelta(snapshot.asStaticSegmentAnimatedSurfaces, before.animatedSurfaces, after.animatedSurfaces) &&
		ApplyContributionDelta(snapshot.asStaticSegmentMaterialRiskSurfaces, before.materialRiskSurfaces, after.materialRiskSurfaces) &&
		ApplyContributionDelta(snapshot.asStaticSegmentContiguousChunkSurfaces, before.contiguousChunkSurfaces, after.contiguousChunkSurfaces);
	if (!contributionUpdated)
	{
		Invalidate();
		return;
	}
	if (before.reuseSignature == after.reuseSignature)
	{
		return;
	}
	if (!RemoveSignatureRef(mStaticSegmentSignatureRefs, snapshot, before.reuseSignature))
	{
		Invalidate();
		return;
	}
	AddSignatureRef(mStaticSegmentSignatureRefs, snapshot, after.reuseSignature);
}

NRIStaticSceneDiagnosticsSnapshot NRIStaticSceneDiagnosticsCache::Build(
	const NRIStaticSceneDiagnosticsInput& input)
{
	const BuildIdentity identity = MakeBuildIdentity(input);
	const bool cacheHit =
		mHasCachedStructuralSnapshot &&
		mCachedGeneration == mGeneration &&
		mCachedIdentity == identity;
	NRIStaticSceneDiagnosticsSnapshot snapshot;
	if (cacheHit)
	{
		snapshot = mCachedStructuralSnapshot;
		snapshot.cacheHit = true;
		snapshot.diagnosticChunkRowsScanned = 0;
		snapshot.diagnosticSurfaceRowsScanned = 0;
		snapshot.diagnosticRegistryRowsScanned = 0;
		snapshot.diagnosticContainersBuilt = 0;
		snapshot.diagnosticChunkRowsIncrementallyUpdated = mPendingChunkRowsIncrementallyUpdated;
		snapshot.diagnosticSurfaceRowsIncrementallyUpdated = mPendingSurfaceRowsIncrementallyUpdated;
	}
	else
	{
		snapshot = BuildStructuralSnapshot(input, mGeneration, mStaticSegmentSignatureRefs);
		if (snapshot.valid)
		{
			mCachedIdentity = identity;
			mCachedGeneration = mGeneration;
			mCachedStructuralSnapshot = snapshot;
			mHasCachedStructuralSnapshot = true;
		}
	}
	mPendingChunkRowsIncrementallyUpdated = 0;
	mPendingSurfaceRowsIncrementallyUpdated = 0;
	OverlayLiveSegmentCache(input, snapshot);
	return snapshot;
}

void NRIStaticSceneDiagnosticsCache::Invalidate()
{
	mPendingChunkRowsIncrementallyUpdated = 0;
	mPendingSurfaceRowsIncrementallyUpdated = 0;
	mGeneration++;
	if (mGeneration == 0)
	{
		mGeneration = 1;
	}
}

void NRIStaticSceneDiagnosticsCache::Discard()
{
	mHasCachedStructuralSnapshot = false;
	mCachedGeneration = 0;
	mCachedIdentity = {};
	mCachedStructuralSnapshot = {};
	mStaticSegmentSignatureRefs.clear();
	mPendingChunkRowsIncrementallyUpdated = 0;
	mPendingSurfaceRowsIncrementallyUpdated = 0;
}
