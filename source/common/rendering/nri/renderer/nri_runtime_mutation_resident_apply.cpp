#include "nri_renderer.h"
#include "nri_cvars.h"

#include "../scene/nri_hash.h"
#include "../scene/nri_map_builder.h"
#include "../../hwrenderer/data/hw_clock.h"
#include "nri_render_geometry_helpers.h"
#include "nri_runtime_mutation_shared.h"
#include "nri_runtime_mutation_trace.h"
#include "nri_static_scene_geometry.h"
#include "c_cvars.h"
#include "gamecontrol.h"
#include "gamestate.h"
#include "hw_sections.h"
#include "mapinfo.h"
#include "printf.h"
#include "texinfo.h"
#include "texturemanager.h"

#include <array>
#include <chrono>
#include <cstring>
#include <unordered_map>


namespace
{
	using namespace nri_runtime_mutation;
}

namespace
{
	static bool ShouldTraceResidentGeometryOrderHash()
	{
		return (int)perf_looptraceframes > 0;
	}

	static FTextureID ResolveAuthoredTextureIdForStaticMapSurface(const nri_scene::PTMapSurface& surface)
	{
		switch (surface.kind)
		{
		case nri_scene::PTMapSurfaceKind::Floor:
		{
			const int32_t sectorIndex = surface.surface.provenance.sectorIndex;
			return sectorIndex >= 0 && (unsigned)sectorIndex < sector.Size() ? sector[(unsigned)sectorIndex].floortexture : FNullTextureID();
		}
		case nri_scene::PTMapSurfaceKind::Ceiling:
		{
			const int32_t sectorIndex = surface.surface.provenance.sectorIndex;
			return sectorIndex >= 0 && (unsigned)sectorIndex < sector.Size() ? sector[(unsigned)sectorIndex].ceilingtexture : FNullTextureID();
		}
		case nri_scene::PTMapSurfaceKind::WallOneSided:
		{
			const int32_t wallIndex = surface.surface.provenance.wallIndex;
			if (wallIndex < 0 || (unsigned)wallIndex >= wall.Size())
			{
				return FNullTextureID();
			}
			const walltype& wal = wall[(unsigned)wallIndex];
			return ((wal.cstat & CSTAT_WALL_1WAY) != 0 && wal.nextwall != -1) ? wal.overtexture : wal.walltexture;
		}
		case nri_scene::PTMapSurfaceKind::WallUpper:
		case nri_scene::PTMapSurfaceKind::WallMiddle:
		{
			const int32_t wallIndex = surface.surface.provenance.wallIndex;
			return wallIndex >= 0 && (unsigned)wallIndex < wall.Size() ? wall[(unsigned)wallIndex].overtexture : FNullTextureID();
		}
		case nri_scene::PTMapSurfaceKind::WallLower:
		{
			const int32_t wallIndex = surface.surface.provenance.wallIndex;
			if (wallIndex < 0 || (unsigned)wallIndex >= wall.Size())
			{
				return FNullTextureID();
			}
			const walltype& wal = wall[(unsigned)wallIndex];
			if ((wal.cstat & CSTAT_WALL_BOTTOM_SWAP) != 0 && wal.nextwall >= 0 && (unsigned)wal.nextwall < wall.Size())
			{
				return wall[(unsigned)wal.nextwall].walltexture;
			}
			return wal.walltexture;
		}
		default:
			return FNullTextureID();
		}
	}

	static bool IsAnimatedStaticMapSurfaceCandidate(const nri_scene::PTMapSurface& surface)
	{
		const FTextureID textureId = ResolveAuthoredTextureIdForStaticMapSurface(surface);
		return textureId.isValid() && GetExtInfo(textureId).picanm.type() != 0;
	}

	static bool ChunkHasAnimatedStaticMapSurfaceCandidates(const nri_scene::PTMapWorld& mapWorld, const nri_scene::PTMapChunk& chunk)
	{
		const uint32_t endSurface = std::min<uint32_t>(chunk.firstSurface + chunk.surfaceCount, (uint32_t)mapWorld.surfaces.size());
		for (uint32_t surfaceIndex = chunk.firstSurface; surfaceIndex < endSurface; ++surfaceIndex)
		{
			if (IsAnimatedStaticMapSurfaceCandidate(mapWorld.surfaces[surfaceIndex]))
			{
				return true;
			}
		}
		return false;
	}

	enum RuntimeResidentBlasRefitRejectBits : uint32_t
	{
		RuntimeResidentBlasRefitReject_NoPreviousAs = 1u << 0,
		RuntimeResidentBlasRefitReject_IndexCountMismatch = 1u << 1,
		RuntimeResidentBlasRefitReject_PrimitiveCountMismatch = 1u << 2,
		RuntimeResidentBlasRefitReject_ZeroIndexCount = 1u << 3,
		RuntimeResidentBlasRefitReject_ZeroPrimitiveCount = 1u << 4,
	};

	static uint32_t ScoreRuntimeResidentBlasRefitRejectTraceEntry(const NRIRenderer::RuntimeResidentBlasRefitRejectTraceEntry& entry)
	{
		const uint32_t indexDelta =
			entry.previousIndexCount > entry.liveIndexCount ?
			entry.previousIndexCount - entry.liveIndexCount :
			entry.liveIndexCount - entry.previousIndexCount;
		const uint32_t primitiveDelta =
			entry.previousPrimitiveCount > entry.livePrimitiveCount ?
			entry.previousPrimitiveCount - entry.livePrimitiveCount :
			entry.livePrimitiveCount - entry.previousPrimitiveCount;
		uint32_t score = indexDelta * 16u;
		score += primitiveDelta * 16u;
		score += entry.livePrimitiveCount * 4u;
		score += entry.liveIndexCount * 2u;
		if ((entry.rejectMask & RuntimeResidentBlasRefitReject_IndexCountMismatch) != 0) score += 1u << 20;
		if ((entry.rejectMask & RuntimeResidentBlasRefitReject_PrimitiveCountMismatch) != 0) score += 1u << 19;
		if ((entry.rejectMask & RuntimeResidentBlasRefitReject_NoPreviousAs) != 0) score += 1u << 18;
		if ((entry.rejectMask & RuntimeResidentBlasRefitReject_ZeroIndexCount) != 0) score += 1u << 17;
		if ((entry.rejectMask & RuntimeResidentBlasRefitReject_ZeroPrimitiveCount) != 0) score += 1u << 16;
		return score;
	}

	class ScopedStaticDiagnosticsChunkMutation
	{
	public:
		ScopedStaticDiagnosticsChunkMutation(
			NRIStaticSceneDiagnosticsCache& cache,
			const NRIStaticSceneDiagnosticsInput& input,
			uint32_t chunkListIndex,
			bool enabled)
			: mCache(cache),
			  mInput(input),
			  mChunkListIndex(chunkListIndex),
			  mEnabled(enabled)
		{
			if (mEnabled)
			{
				mBefore = NRIStaticSceneDiagnosticsCache::CaptureChunkState(mInput, mChunkListIndex);
			}
		}

		~ScopedStaticDiagnosticsChunkMutation()
		{
			if (mEnabled)
			{
				mCache.NotifyChunkMutation(mInput, mChunkListIndex, mBefore);
			}
		}

	private:
		NRIStaticSceneDiagnosticsCache& mCache;
		NRIStaticSceneDiagnosticsInput mInput;
		uint32_t mChunkListIndex = UINT32_MAX;
		bool mEnabled = false;
		NRIStaticSceneDiagnosticsChunkState mBefore = {};
	};
}

// Runtime mutation resident-apply renderer-service implementation.
bool NRIRenderer::TryApplyRuntimeMutationChunkToResidentScene(
	const nri_scene::PTMapChunk& mapChunk,
	RuntimeMapMutationCache::ChunkReplacement& replacement,
	RuntimeMutationResidentApplyResult& outResult)
{
	outResult = {};
	ScopedPtPerfTimer residentApplyPerfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyMs);
	mLastPerfShellTraceStats.runtimeMutationResidentApplyCount++;
	if (!mStaticMapScene.valid ||
		!mStaticMapScene.buffersResident ||
		!mStaticMapScene.accelerationResident ||
		!mStaticMapChunkAtlas.valid ||
		mapChunk.chunkIndex >= mStaticSceneResidency.Registry().entries.size())
	{
		return false;
	}

	auto& entry = mStaticSceneResidency.Registry().entries[mapChunk.chunkIndex];
	if (!entry.valid)
	{
		return false;
	}
	const uint32_t resolvedChunkListIndex =
		entry.staticSceneChunkListIndex < mStaticMapScene.chunks.size() &&
		mStaticMapScene.chunks[entry.staticSceneChunkListIndex].chunkIndex == mapChunk.chunkIndex ?
		entry.staticSceneChunkListIndex :
		NRIStaticSceneResidency::FindPreferredStaticSceneChunkListIndex(
			mStaticMapScene,
			mStaticMapChunkAtlas,
			mapChunk.chunkIndex);
	const bool hasResidentChunk =
		entry.active &&
		entry.mappedInStaticScene &&
		resolvedChunkListIndex < mStaticMapScene.chunks.size() &&
		resolvedChunkListIndex < mStaticMapChunkAtlas.chunks.size() &&
		mStaticMapScene.chunks[resolvedChunkListIndex].active;
	const bool hasChunkSlot =
		resolvedChunkListIndex < mStaticMapScene.chunks.size() &&
		resolvedChunkListIndex < mStaticMapChunkAtlas.chunks.size();
	NRIStaticSceneDiagnosticsInput staticDiagnosticsInput = {};
	staticDiagnosticsInput.mapWorld = &mMapWorld;
	staticDiagnosticsInput.staticScene = &mStaticMapScene;
	staticDiagnosticsInput.atlas = &mStaticMapChunkAtlas;
	staticDiagnosticsInput.registry = &mStaticSceneResidency.Registry();
	ScopedStaticDiagnosticsChunkMutation staticDiagnosticsMutation(
		mStaticSceneDiagnostics,
		staticDiagnosticsInput,
		resolvedChunkListIndex,
		ShouldTracePtPerf() && nri_ptscenestats && mStaticSceneDiagnostics.HasCachedStructuralSnapshot());
	if (hasChunkSlot)
	{
		entry.staticSceneChunkListIndex = resolvedChunkListIndex;
	}

	const bool hasResolvedAtlasChunk = resolvedChunkListIndex < mStaticMapChunkAtlas.chunks.size();
	const uint32_t resolvedAtlasMaterialCount =
		hasResolvedAtlasChunk ? mStaticMapChunkAtlas.chunks[resolvedChunkListIndex].materialCount : 0u;
	if (replacement.certifiedResidentMaterialOnly)
	{
		const auto& certifiedAtlas = hasResolvedAtlasChunk ?
			mStaticMapChunkAtlas.chunks[resolvedChunkListIndex] : StaticMapChunkAtlas::ChunkEntry{};
		const StaticMapSceneCache::ChunkCache* certifiedChunk =
			resolvedChunkListIndex < mStaticMapScene.chunks.size() ?
			&mStaticMapScene.chunks[resolvedChunkListIndex] : nullptr;
		const bool certificateMatchesResident =
			hasResidentChunk &&
			hasResolvedAtlasChunk &&
			certifiedAtlas.valid &&
			certifiedAtlas.chunkIndex == mapChunk.chunkIndex &&
			certifiedAtlas.staticSceneChunkListIndex == resolvedChunkListIndex &&
			certifiedChunk != nullptr &&
			certifiedChunk->chunkIndex == mapChunk.chunkIndex &&
			mStaticMapScene.buildSerial == mMapWorld.buildSerial &&
			mStaticMapChunkAtlas.buildSerial == mMapWorld.buildSerial &&
			mStaticSceneResidency.Registry().buildSerial == mMapWorld.buildSerial &&
			replacement.certifiedMaterialBuildSerial == mMapWorld.buildSerial &&
			replacement.certifiedMaterialBuildSerial == mMapMovers.GetBuildSerial() &&
			replacement.certifiedMaterialMapEpoch != 0 &&
			replacement.certifiedMaterialMapEpoch == mMapMovers.GetMapEpoch() &&
			replacement.certifiedMaterialOwnerStableId != UINT64_MAX &&
			replacement.certifiedMaterialLayoutKey != 0 &&
			replacement.certifiedMaterialStateKey != 0 &&
			replacement.certifiedExactGeometrySignature != 0 &&
			replacement.certifiedExactGeometrySignature == certifiedChunk->exactGeometrySignature &&
			replacement.certifiedGeometryTopologySignature == certifiedChunk->geometryTopologySignature &&
			replacement.certifiedPrimitiveLayoutSignature == certifiedChunk->primitiveLayoutSignature &&
			certifiedChunk->canonicalMaterialLayout.valid &&
			certifiedChunk->canonicalMaterialLayout.layoutKey == replacement.certifiedMaterialLayoutKey &&
			replacement.certifiedChunkListIndex == resolvedChunkListIndex &&
			replacement.certifiedVertexOffset == certifiedAtlas.vertexOffset &&
			replacement.certifiedVertexCount == certifiedAtlas.vertexCount &&
			replacement.certifiedIndexOffset == certifiedAtlas.indexOffset &&
			replacement.certifiedIndexCount == certifiedAtlas.indexCount &&
			replacement.certifiedPrimitiveOffset == certifiedAtlas.primitiveOffset &&
			replacement.certifiedPrimitiveCount == certifiedAtlas.primitiveCount &&
			replacement.certifiedMaterialOffset == certifiedAtlas.materialOffset &&
			replacement.certifiedMaterialCount == certifiedAtlas.materialCount &&
			replacement.materialBridge.materials.size() == certifiedAtlas.materialCount &&
			replacement.materialBridge.lightMetadata.size() == certifiedAtlas.materialCount &&
			entry.staticSceneChunkListIndex == resolvedChunkListIndex &&
			entry.vertexOffset == certifiedAtlas.vertexOffset &&
			entry.vertexCount == certifiedAtlas.vertexCount &&
			entry.indexOffset == certifiedAtlas.indexOffset &&
			entry.indexCount == certifiedAtlas.indexCount &&
			entry.primitiveOffset == certifiedAtlas.primitiveOffset &&
			entry.primitiveCount == certifiedAtlas.primitiveCount &&
			entry.materialOffset == certifiedAtlas.materialOffset &&
			entry.materialCount == certifiedAtlas.materialCount &&
			certifiedChunk->accelerationStructure.accelerationStructure != nullptr;
		if (!certificateMatchesResident)
		{
			replacement.certifiedResidentMaterialOnly = false;
		}
	}
	const RuntimeMutationResidentApplyMode applyMode =
		mRuntimeMutation.ClassifyResidentApplyMode(replacement, hasResidentChunk, hasResolvedAtlasChunk, resolvedAtlasMaterialCount);
	const RuntimeMutationResidentApplyModeStats applyModeStats =
		mRuntimeMutation.BuildResidentApplyModeStats(applyMode, replacement, hasResidentChunk, hasResolvedAtlasChunk, resolvedAtlasMaterialCount);
	mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialOnlyCount += applyModeStats.materialOnlyCount;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyFastMaterialOnlyCount += applyModeStats.fastMaterialOnlyCount;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyCertifiedMaterialOnlyCount += applyModeStats.certifiedMaterialOnlyCount;
	mLastPerfShellTraceStats.runtimeMutationResidentApplySlowMaterialOnlyCount += applyModeStats.slowMaterialOnlyCount;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialOnlyExclusiveCount += applyModeStats.materialOnlyExclusiveCount;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialOnlyNoResidentChunkCount += applyModeStats.materialOnlyNoResidentChunkCount;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialOnlyInvalidReplacementCount += applyModeStats.materialOnlyInvalidReplacementCount;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialOnlyMaterialCountMismatchCount += applyModeStats.materialOnlyMaterialCountMismatchCount;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyStructuralCount += applyModeStats.structuralCount;
	const bool materialOnlyReplacement = applyMode.materialOnlyReplacement;
	const bool exclusiveMaterialOnlyReplacement = applyMode.exclusiveMaterialOnlyReplacement;
	const bool fastResidentMaterialOnlyUpdate = applyMode.fastResidentMaterialOnlyUpdate;
	const bool appliedCertifiedResidentMaterialOnly = applyMode.certifiedResidentMaterialOnlyUpdate;

	nri_scene::SceneView residentSceneView;
	nri_scene::GeometryData residentGeometry;
	nri_scene::MaterialBridgeData residentMaterials;
	nri_scene::PTMapChunkMutationBaseline appliedBaseline;

	if (fastResidentMaterialOnlyUpdate)
	{
		const bool mergedSectorMaterialOnlyFastPath =
			IsPureSectorRuntimeMutationMaterialOnlyReasonMask(replacement.reasonMask) &&
			hasResidentChunk &&
			resolvedChunkListIndex < mStaticMapScene.lightChunkViews.size() &&
			TryBuildMergedSectorMaterialOnlySceneView(
				mStaticMapScene.lightChunkViews[resolvedChunkListIndex],
				replacement.sceneView,
				residentSceneView);
		if (!mergedSectorMaterialOnlyFastPath)
		{
			residentSceneView = replacement.sceneView;
		}
		residentMaterials = replacement.materialBridge;
		appliedBaseline = replacement.replacementBaseline;
	}
	else if (materialOnlyReplacement && !exclusiveMaterialOnlyReplacement)
	{
		nri_scene::PTMapWorld liveWorld = {};
		nri_scene::PTMapWorldStats ignoredStats = {};
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyLiveBuildMs);
			nri_scene::PTMapBuildOptions mapBuildOptions = {};
			if (!nri_scene::BuildLiveMapChunkWorld(mapChunk, liveWorld, &ignoredStats, mapBuildOptions))
			{
				return false;
			}

			nri_scene::BuildMapChunkSceneView(liveWorld, liveWorld.chunks[0], residentSceneView);
			const bool blindSpotReplacementNudged = replacement.blindSpot && replacement.dragged;
			if (blindSpotReplacementNudged)
			{
				NudgeBlindSpotReplacementFlats(residentSceneView);
			}
			else if (nri_ptceilingnudge)
			{
				NudgeMapCeilingSections(residentSceneView, (float)nri_ptceilingnudgedistance);
			}
			ApplyCommittedMapMotion(residentSceneView);
		}
		{
			Clocker clock(NriPTGeometryBuild);
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryBuildMs);
			ScopedPtPerfTimer siteTimer(mLastPerfShellTraceStats.geometryBuildResidentApplyMs);
			nri_scene::BuildGeometry(residentSceneView, residentGeometry);
			AssignGeometryPortalIndices(mMapWorld, residentGeometry);
		}
		mLastPerfShellTraceStats.geometryBuildResidentApplyCalls++;
		mLastPerfShellTraceStats.geometryBuildResidentPrimitives += (uint32_t)residentGeometry.primitives.size();
		{
			Clocker clock(NriPTMaterialBuild);
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialBuildMs);
			BuildMaterialsWithActorOverrides(residentSceneView, residentMaterials, "resident_runtime_mutation_chunk");
		}
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyBaselineCaptureMs);
			if (!nri_scene::CaptureMapChunkMutationBaseline(mapChunk, appliedBaseline))
			{
				return false;
			}
		}
	}
	else
	{
		residentSceneView = replacement.sceneView;
		residentGeometry = replacement.geometry;
		residentMaterials = replacement.materialBridge;
		appliedBaseline = replacement.replacementBaseline;
	}
	outResult.surfaceCount = CountSceneViewSurfaces(residentSceneView);
	outResult.triangleCount = fastResidentMaterialOnlyUpdate && hasResolvedAtlasChunk ?
		mStaticMapChunkAtlas.chunks[resolvedChunkListIndex].primitiveCount :
		(uint32_t)residentGeometry.primitives.size();
	outResult.materialCount = (uint32_t)residentMaterials.materials.size();

	const uint32_t appliedReasonMask = replacement.reasonMask;
	const bool appliedStaticAnimatedReplacement = replacement.staticAnimatedReplacement;
	const bool appliedAnimationOnlyRefreshed = replacement.animationOnlyRefreshed;
	uint64_t residentGeometryTopologySignature =
		fastResidentMaterialOnlyUpdate && hasResidentChunk ?
			mStaticMapScene.chunks[resolvedChunkListIndex].geometryTopologySignature :
			nri_static_scene_geometry::ComputeGeometryTopologySignature(residentGeometry);
	uint64_t residentPrimitiveLayoutSignature =
		fastResidentMaterialOnlyUpdate && hasResidentChunk ?
			mStaticMapScene.chunks[resolvedChunkListIndex].primitiveLayoutSignature :
			nri_static_scene_geometry::ComputePrimitiveLayoutSignature(residentGeometry);
	const uint32_t materialReasonMask =
		nri_scene::PTMapChunkMutationReason_SectorMaterial |
		nri_scene::PTMapChunkMutationReason_WallMaterial;
	const bool appliedForceTopology =
		(appliedReasonMask & (
			nri_scene::PTMapChunkMutationReason_SectorGeometry |
			nri_scene::PTMapChunkMutationReason_WallGeometry |
			nri_scene::PTMapChunkMutationReason_SectorDirty |
			nri_scene::PTMapChunkMutationReason_SectionDirty |
			nri_scene::PTMapChunkMutationReason_Dragged)) != 0;
	const bool hasResidentGeometry =
		hasResidentChunk &&
		mStaticMapScene.chunks[resolvedChunkListIndex].active &&
		mStaticMapChunkAtlas.chunks[resolvedChunkListIndex].valid;
	const uint32_t chunkListIndex = hasChunkSlot ?
		resolvedChunkListIndex :
		(uint32_t)mStaticMapScene.chunks.size();
	uint32_t residentVertexCount = 0;
	uint32_t residentIndexCount = 0;
	uint32_t residentPrimitiveCount = 0;
	uint32_t residentMaterialCount = 0;
	bool preserveResidentGeometryForMaterialOnlyUpdate = false;
	uint32_t effectiveResidentVertexCount = 0;
	uint32_t effectiveResidentIndexCount = 0;
	uint32_t effectiveResidentPrimitiveCount = 0;
	bool chunkBecomesEmpty = false;
	bool recoveredFullLiveResidentChunk = false;
	const auto recomputeResidentCounts = [&]()
	{
		residentVertexCount =
			fastResidentMaterialOnlyUpdate && !recoveredFullLiveResidentChunk && hasResidentChunk ?
			mStaticMapChunkAtlas.chunks[resolvedChunkListIndex].vertexCount :
			(uint32_t)residentGeometry.vertices.size();
		residentIndexCount =
			fastResidentMaterialOnlyUpdate && !recoveredFullLiveResidentChunk && hasResidentChunk ?
			mStaticMapChunkAtlas.chunks[resolvedChunkListIndex].indexCount :
			(uint32_t)residentGeometry.indices.size();
		residentPrimitiveCount =
			fastResidentMaterialOnlyUpdate && !recoveredFullLiveResidentChunk && hasResidentChunk ?
			mStaticMapChunkAtlas.chunks[resolvedChunkListIndex].primitiveCount :
			(uint32_t)residentGeometry.primitives.size();
		residentMaterialCount = (uint32_t)residentMaterials.materials.size();
		preserveResidentGeometryForMaterialOnlyUpdate =
			materialOnlyReplacement &&
			!exclusiveMaterialOnlyReplacement &&
			hasResidentChunk &&
			residentPrimitiveCount == 0 &&
			residentMaterialCount != 0 &&
			resolvedChunkListIndex < mStaticMapChunkAtlas.chunks.size() &&
			residentMaterialCount == mStaticMapChunkAtlas.chunks[resolvedChunkListIndex].materialCount;
		effectiveResidentVertexCount =
			preserveResidentGeometryForMaterialOnlyUpdate ?
			mStaticMapChunkAtlas.chunks[resolvedChunkListIndex].vertexCount :
			residentVertexCount;
		effectiveResidentIndexCount =
			preserveResidentGeometryForMaterialOnlyUpdate ?
			mStaticMapChunkAtlas.chunks[resolvedChunkListIndex].indexCount :
			residentIndexCount;
		effectiveResidentPrimitiveCount =
			preserveResidentGeometryForMaterialOnlyUpdate ?
			mStaticMapChunkAtlas.chunks[resolvedChunkListIndex].primitiveCount :
			residentPrimitiveCount;
		chunkBecomesEmpty = effectiveResidentPrimitiveCount == 0 || residentMaterialCount == 0;
	};
	recomputeResidentCounts();
	if (preserveResidentGeometryForMaterialOnlyUpdate)
	{
		mLastPerfShellTraceStats.runtimeMutationResidentApplyPreserveGeometryCount++;
	}
	const uint32_t residentChunkRemovalReasonMask =
		nri_scene::PTMapChunkMutationReason_SectorGeometry |
		nri_scene::PTMapChunkMutationReason_WallGeometry |
		nri_scene::PTMapChunkMutationReason_SectorDirty |
		nri_scene::PTMapChunkMutationReason_SectionDirty |
		nri_scene::PTMapChunkMutationReason_Dragged;
	const auto rebuildFullLiveResidentChunk = [&]() -> bool
	{
		nri_scene::PTMapWorld liveWorld = {};
		nri_scene::PTMapWorldStats ignoredStats = {};
		nri_scene::SceneView fullLiveSceneView;
		nri_scene::GeometryData fullLiveGeometry;
		nri_scene::MaterialBridgeData fullLiveMaterials;
		nri_scene::PTMapChunkMutationBaseline fullLiveBaseline;
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyLiveBuildMs);
			nri_scene::PTMapBuildOptions mapBuildOptions = {};
			if (!nri_scene::BuildLiveMapChunkWorld(mapChunk, liveWorld, &ignoredStats, mapBuildOptions))
			{
				return false;
			}

			nri_scene::BuildMapChunkSceneView(liveWorld, liveWorld.chunks[0], fullLiveSceneView);
			if (nri_ptceilingnudge)
			{
				NudgeMapCeilingSections(fullLiveSceneView, (float)nri_ptceilingnudgedistance);
			}
			ApplyCommittedMapMotion(fullLiveSceneView);
		}
		{
			Clocker clock(NriPTGeometryBuild);
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryBuildMs);
			ScopedPtPerfTimer siteTimer(mLastPerfShellTraceStats.geometryBuildResidentRecoverMs);
			nri_scene::BuildGeometry(fullLiveSceneView, fullLiveGeometry);
			AssignGeometryPortalIndices(mMapWorld, fullLiveGeometry);
		}
		mLastPerfShellTraceStats.geometryBuildResidentRecoverCalls++;
		mLastPerfShellTraceStats.geometryBuildResidentPrimitives += (uint32_t)fullLiveGeometry.primitives.size();
		{
			Clocker clock(NriPTMaterialBuild);
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialBuildMs);
			BuildMaterialsWithActorOverrides(fullLiveSceneView, fullLiveMaterials, "resident_runtime_mutation_chunk_recover");
		}
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyBaselineCaptureMs);
			if (!nri_scene::CaptureMapChunkMutationBaseline(mapChunk, fullLiveBaseline))
			{
				return false;
			}
		}

		if (fullLiveGeometry.primitives.empty() || fullLiveMaterials.materials.empty())
		{
			return false;
		}

		if (ShouldTracePtPerf())
		{
			Printf("NRI PT resident chunk trace: event=recover-empty chunk=%u reason_mask=0x%x recovered_prims=%u recovered_mats=%u\n",
				mapChunk.chunkIndex,
				appliedReasonMask,
				(uint32_t)fullLiveGeometry.primitives.size(),
				(uint32_t)fullLiveMaterials.materials.size());
		}

		residentSceneView = std::move(fullLiveSceneView);
		residentGeometry = std::move(fullLiveGeometry);
		residentMaterials = std::move(fullLiveMaterials);
		appliedBaseline = std::move(fullLiveBaseline);
		recoveredFullLiveResidentChunk = true;
		outResult.recoveredEmpty = true;
		mLastPerfShellTraceStats.runtimeMutationResidentApplyRecoverSuccessCount++;
		return true;
	};
	if (chunkBecomesEmpty)
	{
		mLastPerfShellTraceStats.runtimeMutationResidentApplyRecoverAttemptCount++;
		if (rebuildFullLiveResidentChunk())
		{
			residentGeometryTopologySignature = nri_static_scene_geometry::ComputeGeometryTopologySignature(residentGeometry);
			residentPrimitiveLayoutSignature = nri_static_scene_geometry::ComputePrimitiveLayoutSignature(residentGeometry);
			recomputeResidentCounts();
			if (preserveResidentGeometryForMaterialOnlyUpdate)
			{
				mLastPerfShellTraceStats.runtimeMutationResidentApplyPreserveGeometryCount++;
			}
		}
	}
	const bool suspiciousNonStructuralChunkRemoval =
		chunkBecomesEmpty &&
		hasResidentChunk &&
		(appliedReasonMask & residentChunkRemovalReasonMask) == 0;
	if (suspiciousNonStructuralChunkRemoval)
	{
		if (ShouldTracePtPerf())
		{
			Printf("NRI PT resident chunk trace: event=reject-empty-nonstructural chunk=%u reason_mask=0x%x resident_prims=%u resident_mats=%u\n",
				mapChunk.chunkIndex,
				appliedReasonMask,
				residentPrimitiveCount,
				residentMaterialCount);
		}
		return false;
	}
	const uint64_t residentMaterialPayloadHash =
		!chunkBecomesEmpty ? nri_runtime_mutation::HashResidentMaterialPayload(residentMaterials) : 0;
	uint64_t residentGeometryPayloadHash = 0;
	bool residentMaterialPayloadHashSkip = false;
	bool residentGeometryPayloadHashSkip = false;
	bool appliedPreservedResidentMaterialSlice = false;
	bool appliedPreservedResidentGeometryPayload = false;

	if (chunkBecomesEmpty)
	{
		mLastPerfShellTraceStats.runtimeMutationResidentApplyEmptyRemovalCount++;
		if (hasResidentChunk)
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyAtlasMs);
			auto& mutableChunk = mStaticMapScene.chunks[chunkListIndex];
			const auto& oldAtlasChunk = mStaticMapChunkAtlas.chunks[chunkListIndex];
			const uint32_t oldMaterialOffset = oldAtlasChunk.materialOffset;
			const uint32_t oldMaterialCount = oldAtlasChunk.materialCount;
			RetireResidentAccelerationStructure(mutableChunk.accelerationStructure);
			nri_static_scene_geometry::ReleaseChunkAtlasRange(mStaticMapChunkAtlas.freeVertexRanges, oldAtlasChunk.vertexOffset, oldAtlasChunk.vertexCount);
			nri_static_scene_geometry::ReleaseChunkAtlasRange(mStaticMapChunkAtlas.freeIndexRanges, oldAtlasChunk.indexOffset, oldAtlasChunk.indexCount);
			nri_static_scene_geometry::ReleaseChunkAtlasRange(mStaticMapChunkAtlas.freePrimitiveRanges, oldAtlasChunk.primitiveOffset, oldAtlasChunk.primitiveCount);
			nri_static_scene_geometry::ReleaseChunkAtlasRange(mStaticMapChunkAtlas.freeMaterialRanges, oldAtlasChunk.materialOffset, oldAtlasChunk.materialCount);
			mStaticMapChunkAtlas.chunks[chunkListIndex] = {};
			mStaticMapChunkAtlas.chunks[chunkListIndex].chunkIndex = mapChunk.chunkIndex;
			mStaticMapChunkAtlas.chunks[chunkListIndex].staticSceneChunkListIndex = chunkListIndex;
			mutableChunk.active = false;
			mutableChunk.blasUpdateEligible = false;
			mutableChunk.lastResidentBlasReasonMask = 0;
			mutableChunk.lastResidentBlasSurfaceCount = 0;
			mutableChunk.lastResidentBlasTriangleCount = 0;
			mutableChunk.lastResidentBlasMaterialCount = 0;
			mutableChunk.lastResidentBlasForceTopology = false;
			mutableChunk.lastResidentBlasRecoveredEmpty = false;
			mutableChunk.lastResidentBlasKeptGeometrySlice = false;
			mutableChunk.lastResidentBlasTopologyChanged = false;
			mutableChunk.residentBlasScratchSizeCacheKey = nullptr;
			mutableChunk.residentBlasBuildScratchSize = 0;
			mutableChunk.residentBlasUpdateScratchSize = 0;
			mutableChunk.residentBlasVertexBuffer = nullptr;
			mutableChunk.residentBlasIndexBuffer = nullptr;
			mutableChunk.residentBlasVertexNum = 0;
			mutableChunk.residentBlasIndexOffset = 0;
			mutableChunk.residentBlasIndexNum = 0;
			mutableChunk.fixedLayoutDeformerKey = 0;
			mutableChunk.fixedLayoutVertexSpanCount = 0;
			mutableChunk.fixedLayoutPrimitiveSpanCount = 0;
			mutableChunk.fixedLayoutVertexBytes = 0;
			mutableChunk.fixedLayoutPrimitiveBytes = 0;
			mutableChunk.vertexCount = 0;
			mutableChunk.indexCount = 0;
			mutableChunk.primitiveCount = 0;
			mutableChunk.materialCount = 0;
			mutableChunk.materialBridge = {};
			mutableChunk.geometryTopologySignature = 0;
			mutableChunk.primitiveLayoutSignature = 0;
			mutableChunk.exactGeometrySignature = 0;
			mutableChunk.geometryPayloadHash = 0;
			mutableChunk.animatedMaterialSignature = 0;
			mutableChunk.animatedGeometrySignature = 0;
			mutableChunk.hasAnimatedTextureCandidates = false;
			mutableChunk.animatedRefreshSuppressed = false;
			if (chunkListIndex < mStaticMapScene.lightChunkViews.size())
			{
				mStaticMapScene.lightChunkViews[chunkListIndex] = {};
			}
			if (oldMaterialOffset + oldMaterialCount <= mStaticMapScene.materialBridge.materials.size())
			{
				std::fill_n(
					mStaticMapScene.materialBridge.materials.begin() + oldMaterialOffset,
					oldMaterialCount,
					nri_scene::MaterialData{});
			}
			if (oldMaterialOffset + oldMaterialCount <= mStaticMapScene.materialBridge.lightMetadata.size())
			{
				std::fill_n(
					mStaticMapScene.materialBridge.lightMetadata.begin() + oldMaterialOffset,
					oldMaterialCount,
					nri_scene::MaterialLightingMetadata{});
			}
			if (oldMaterialOffset + oldMaterialCount <= mStaticMapScene.gpuMaterials.size())
			{
				std::fill_n(
					mStaticMapScene.gpuMaterials.begin() + oldMaterialOffset,
					oldMaterialCount,
					nri_scene::MaterialData{});
			}
			if (oldMaterialCount > 0)
			{
				std::vector<nri_scene::MaterialData> clearedMaterials(oldMaterialCount);
				if (!StageResidentBufferCopyRange(
						mStaticMaterialBuffer,
						(uint64_t)oldMaterialOffset * sizeof(nri_scene::MaterialData),
						clearedMaterials.data(),
						(uint64_t)oldMaterialCount * sizeof(nri_scene::MaterialData),
						NRIResourceComputeShaderResourceAccess(),
						ResidentUploadKind_Material))
				{
					return false;
				}
				mLastPerfResourceTraceStats.residentChunkBatchMaterialBytes +=
					(uint64_t)oldMaterialCount * sizeof(nri_scene::MaterialData);
			}
			outResult.geometryDirty = true;
			outResult.materialDirty = true;
			outResult.staticSceneChunkListIndex = chunkListIndex;
		}
	}
	else
	{
		ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyAtlasMs);
		StaticMapSceneCache::ChunkCache sourceChunk = {};
		StaticMapChunkAtlas nextAtlasState = {};
		StaticMapChunkAtlas::ChunkEntry nextAtlasChunk = {};
		bool keptGeometrySlices = false;
		bool keptMaterialSlice = false;
		const uint64_t previousAnimatedMaterialSignature =
			hasResidentChunk ?
			mStaticMapScene.chunks[chunkListIndex].animatedMaterialSignature :
			0;
		const bool previousAnimatedRefreshSuppressed =
			hasResidentChunk &&
			mStaticMapScene.chunks[chunkListIndex].animatedRefreshSuppressed;
		const uint64_t previousAnimatedGeometrySignature =
			hasResidentChunk ?
				mStaticMapScene.chunks[chunkListIndex].animatedGeometrySignature :
				0;
		const uint64_t previousGeometryTopologySignature =
			hasResidentChunk ?
				mStaticMapScene.chunks[chunkListIndex].geometryTopologySignature :
				0;
		const uint64_t previousPrimitiveLayoutSignature =
			hasResidentChunk ?
				mStaticMapScene.chunks[chunkListIndex].primitiveLayoutSignature :
				0;
		bool preserveResidentMaterialSlice = false;
		bool preserveResidentIndexSlice = false;
		bool preserveResidentPrimitiveSlice = false;
		bool preserveResidentBlasRefitOnly = false;
		bool useFixedLayoutVertexSpans = false;
		bool useFixedLayoutPrimitiveSpans = false;
		const auto fixedLayoutSpansValid = [](
			const std::vector<RuntimeMutationResidentElementSpan>& spans,
			uint32_t elementCapacity)
		{
			for (const RuntimeMutationResidentElementSpan& span : spans)
			{
				if (span.elementCount == 0 || span.firstElement >= elementCapacity ||
					span.elementCount > elementCapacity - span.firstElement)
				{
					return false;
				}
			}
			return true;
		};
		const bool fixedLayoutVertexSpansValid =
			!replacement.fixedLayoutDeformer ||
			fixedLayoutSpansValid(replacement.fixedLayoutVertexSpans, effectiveResidentVertexCount);
		const bool fixedLayoutPrimitiveSpansValid =
			!replacement.fixedLayoutDeformer ||
			fixedLayoutSpansValid(replacement.fixedLayoutPrimitiveSpans, effectiveResidentPrimitiveCount);
		{
			ScopedPtPerfTimer detailPerfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyAtlasBookkeepingMs);
			sourceChunk.vertexOffset = 0;
			sourceChunk.vertexCount = effectiveResidentVertexCount;
			sourceChunk.indexOffset = 0;
			sourceChunk.indexCount = effectiveResidentIndexCount;
			sourceChunk.primitiveOffset = 0;
			sourceChunk.primitiveCount = effectiveResidentPrimitiveCount;
			sourceChunk.materialOffset = 0;
			sourceChunk.materialCount = (uint32_t)residentMaterials.materials.size();

			for (;;)
			{
				nextAtlasState = mStaticMapChunkAtlas;
				if (chunkListIndex >= nextAtlasState.chunks.size())
				{
					nextAtlasState.chunks.resize(chunkListIndex + 1);
				}
				nextAtlasState.chunkCount = (uint32_t)nextAtlasState.chunks.size();
				const auto oldAtlasChunk = hasResidentChunk ? nextAtlasState.chunks[chunkListIndex] : StaticMapChunkAtlas::ChunkEntry{};
				const bool keepGeometrySlices =
					hasResidentGeometry &&
					oldAtlasChunk.vertexCount == effectiveResidentVertexCount &&
					oldAtlasChunk.indexCount == effectiveResidentIndexCount &&
					oldAtlasChunk.primitiveCount == effectiveResidentPrimitiveCount;
				const bool keepMaterialSlice =
					hasResidentGeometry &&
					oldAtlasChunk.materialCount == residentMaterialCount;

				if (hasResidentGeometry && !keepGeometrySlices)
				{
					nri_static_scene_geometry::ReleaseChunkAtlasRange(nextAtlasState.freeVertexRanges, oldAtlasChunk.vertexOffset, oldAtlasChunk.vertexCount);
					nri_static_scene_geometry::ReleaseChunkAtlasRange(nextAtlasState.freeIndexRanges, oldAtlasChunk.indexOffset, oldAtlasChunk.indexCount);
					nri_static_scene_geometry::ReleaseChunkAtlasRange(nextAtlasState.freePrimitiveRanges, oldAtlasChunk.primitiveOffset, oldAtlasChunk.primitiveCount);
				}
				if (hasResidentGeometry && !keepMaterialSlice)
				{
					nri_static_scene_geometry::ReleaseChunkAtlasRange(nextAtlasState.freeMaterialRanges, oldAtlasChunk.materialOffset, oldAtlasChunk.materialCount);
				}

				nextAtlasChunk = {};
				nextAtlasChunk.valid = true;
				nextAtlasChunk.chunkIndex = mapChunk.chunkIndex;
				nextAtlasChunk.staticSceneChunkListIndex = chunkListIndex;
				nextAtlasChunk.vertexCount = effectiveResidentVertexCount;
				nextAtlasChunk.indexCount = effectiveResidentIndexCount;
				nextAtlasChunk.primitiveCount = effectiveResidentPrimitiveCount;
				nextAtlasChunk.materialCount = residentMaterialCount;
				nextAtlasChunk.vertexOffset = keepGeometrySlices ?
					oldAtlasChunk.vertexOffset :
					nri_static_scene_geometry::AllocateChunkAtlasRange(effectiveResidentVertexCount, nextAtlasState.vertexCapacity, nextAtlasState.freeVertexRanges, nextAtlasState.vertexCount);
				nextAtlasChunk.indexOffset = keepGeometrySlices ?
					oldAtlasChunk.indexOffset :
					nri_static_scene_geometry::AllocateChunkAtlasRange(effectiveResidentIndexCount, nextAtlasState.indexCapacity, nextAtlasState.freeIndexRanges, nextAtlasState.indexCount);
				nextAtlasChunk.primitiveOffset = keepGeometrySlices ?
					oldAtlasChunk.primitiveOffset :
					nri_static_scene_geometry::AllocateChunkAtlasRange(effectiveResidentPrimitiveCount, nextAtlasState.primitiveCapacity, nextAtlasState.freePrimitiveRanges, nextAtlasState.primitiveCount);
				nextAtlasChunk.materialOffset = keepMaterialSlice ?
					oldAtlasChunk.materialOffset :
					nri_static_scene_geometry::AllocateChunkAtlasRange(residentMaterialCount, nextAtlasState.materialCapacity, nextAtlasState.freeMaterialRanges, nextAtlasState.materialCount);
				if (nextAtlasChunk.vertexOffset != UINT32_MAX &&
					nextAtlasChunk.indexOffset != UINT32_MAX &&
					nextAtlasChunk.primitiveOffset != UINT32_MAX &&
					nextAtlasChunk.materialOffset != UINT32_MAX)
				{
					keptGeometrySlices = keepGeometrySlices;
					keptMaterialSlice = keepMaterialSlice;
					break;
				}

				if (nextAtlasChunk.vertexOffset == UINT32_MAX)
				{
					nextAtlasState.vertexCapacity = nri_static_scene_geometry::GetChunkAtlasCapacity(nextAtlasState.vertexCount + effectiveResidentVertexCount);
				}
				if (nextAtlasChunk.indexOffset == UINT32_MAX)
				{
					nextAtlasState.indexCapacity = nri_static_scene_geometry::GetChunkAtlasCapacity(nextAtlasState.indexCount + effectiveResidentIndexCount);
				}
				if (nextAtlasChunk.primitiveOffset == UINT32_MAX)
				{
					nextAtlasState.primitiveCapacity = nri_static_scene_geometry::GetChunkAtlasCapacity(nextAtlasState.primitiveCount + effectiveResidentPrimitiveCount);
				}
				if (nextAtlasChunk.materialOffset == UINT32_MAX)
				{
					nextAtlasState.materialCapacity = nri_static_scene_geometry::GetChunkAtlasCapacity(nextAtlasState.materialCount + residentMaterialCount);
				}
				mLastPerfShellTraceStats.runtimeMutationResidentApplyAtlasGrowCount++;
				if (!EnsureResidentStaticMapChunkAtlasBufferCapacity(nextAtlasState))
				{
					return false;
				}
			}
			if (keptGeometrySlices)
			{
				mLastPerfShellTraceStats.runtimeMutationResidentApplyKeepGeometrySliceCount++;
			}
			if (keptMaterialSlice)
			{
				mLastPerfShellTraceStats.runtimeMutationResidentApplyKeepMaterialSliceCount++;
			}
			if (hasResidentChunk && residentMaterialCount != 0)
			{
				mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialPayloadHashCheckCount++;
				if (keptMaterialSlice && entry.materialPayloadHash != 0)
				{
					if (entry.materialPayloadHash == residentMaterialPayloadHash)
					{
						residentMaterialPayloadHashSkip = true;
						mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialPayloadHashSkipCount++;
					}
					else
					{
						mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialPayloadHashMissCount++;
					}
				}
				else
				{
					mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialPayloadHashRejectCount++;
				}
			}
			if (!fastResidentMaterialOnlyUpdate &&
				!preserveResidentGeometryForMaterialOnlyUpdate &&
				hasResidentChunk &&
				effectiveResidentVertexCount != 0 &&
				effectiveResidentIndexCount != 0 &&
				effectiveResidentPrimitiveCount != 0)
			{
				mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryPayloadHashCheckCount++;
				if (keptGeometrySlices && keptMaterialSlice && entry.geometryPayloadHash != 0)
				{
					residentGeometryPayloadHash = nri_static_scene_geometry::HashResidentGeometryPayload(
						mMapWorld,
						residentGeometry,
						sourceChunk.vertexOffset,
						sourceChunk.vertexCount,
						sourceChunk.indexOffset,
						sourceChunk.indexCount,
						sourceChunk.primitiveOffset,
						sourceChunk.primitiveCount,
						sourceChunk.materialOffset,
						sourceChunk.materialCount);
					if (residentGeometryPayloadHash != 0 &&
						entry.geometryPayloadHash == residentGeometryPayloadHash)
					{
						residentGeometryPayloadHashSkip = true;
						mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryPayloadHashSkipCount++;
						mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryPayloadHashBlasSkipCount++;
					}
					else
					{
						mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryPayloadHashMissCount++;
						if (ShouldTraceResidentGeometryOrderHash())
						{
							ScopedPtPerfTimer orderHashTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryOrderHashMs);
							mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryPayloadOrderCheckCount++;
							const uint64_t residentGeometryOrderHash = nri_static_scene_geometry::HashResidentGeometryPayloadOrderIndependent(
								mMapWorld,
								residentGeometry,
								sourceChunk.vertexOffset,
								sourceChunk.vertexCount,
								sourceChunk.primitiveOffset,
								sourceChunk.primitiveCount,
								sourceChunk.materialOffset,
								sourceChunk.materialCount);
							const uint64_t currentGeometryOrderHash = nri_static_scene_geometry::HashResidentGeometryPayloadOrderIndependent(
								mMapWorld,
								mStaticMapScene.geometry,
								nextAtlasChunk.vertexOffset,
								nextAtlasChunk.vertexCount,
								nextAtlasChunk.primitiveOffset,
								nextAtlasChunk.primitiveCount,
								nextAtlasChunk.materialOffset,
								nextAtlasChunk.materialCount);
							if (residentGeometryOrderHash != 0 && currentGeometryOrderHash != 0)
							{
								if (residentGeometryOrderHash == currentGeometryOrderHash)
								{
									mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryPayloadOrderEquivalentCount++;
								}
								else
								{
									mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryPayloadOrderMissCount++;
								}
							}
							else
							{
								mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryPayloadOrderRejectCount++;
							}
						}
					}
				}
				else
				{
					mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryPayloadHashRejectCount++;
				}
			}

			if (chunkListIndex >= mStaticMapScene.chunks.size())
			{
				mStaticMapScene.chunks.resize(chunkListIndex + 1);
			}
			if (chunkListIndex >= mStaticMapScene.lightChunkViews.size())
			{
				mStaticMapScene.lightChunkViews.resize(chunkListIndex + 1);
			}
			preserveResidentMaterialSlice =
				!materialOnlyReplacement &&
				(appliedReasonMask & materialReasonMask) == 0 &&
				!appliedStaticAnimatedReplacement &&
				!appliedAnimationOnlyRefreshed &&
				hasResidentChunk &&
				keptMaterialSlice &&
				previousAnimatedMaterialSignature == ComputeAnimatedMaterialSignature(residentSceneView);
			preserveResidentMaterialSlice = preserveResidentMaterialSlice || residentMaterialPayloadHashSkip;
			preserveResidentIndexSlice =
				!materialOnlyReplacement &&
				hasResidentChunk &&
				keptGeometrySlices &&
				previousGeometryTopologySignature == residentGeometryTopologySignature;
			preserveResidentPrimitiveSlice =
				!materialOnlyReplacement &&
				hasResidentChunk &&
				preserveResidentIndexSlice &&
				keptMaterialSlice &&
				previousPrimitiveLayoutSignature == residentPrimitiveLayoutSignature;
			const bool probeResidentBlasRefitOnly =
				!preserveResidentIndexSlice &&
				!materialOnlyReplacement &&
				hasResidentChunk;
			if (probeResidentBlasRefitOnly)
			{
				uint32_t rejectMask = 0;
				const bool hadAccelerationStructure =
					mStaticMapScene.chunks[chunkListIndex].accelerationStructure.accelerationStructure != nullptr;
				mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRefitProbeCount++;
				if (!hadAccelerationStructure)
				{
					rejectMask |= RuntimeResidentBlasRefitReject_NoPreviousAs;
					mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRefitRejectNoPreviousAsCount++;
				}
				if (entry.indexCount != effectiveResidentIndexCount)
				{
					rejectMask |= RuntimeResidentBlasRefitReject_IndexCountMismatch;
					mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRefitRejectIndexCountMismatchCount++;
				}
				if (entry.primitiveCount != effectiveResidentPrimitiveCount)
				{
					rejectMask |= RuntimeResidentBlasRefitReject_PrimitiveCountMismatch;
					mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRefitRejectPrimitiveCountMismatchCount++;
				}
				if (entry.indexCount == 0 || effectiveResidentIndexCount == 0)
				{
					rejectMask |= RuntimeResidentBlasRefitReject_ZeroIndexCount;
					mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRefitRejectZeroIndexCount++;
				}
				if (entry.primitiveCount == 0 || effectiveResidentPrimitiveCount == 0)
				{
					rejectMask |= RuntimeResidentBlasRefitReject_ZeroPrimitiveCount;
					mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRefitRejectZeroPrimitiveCount++;
				}
				preserveResidentBlasRefitOnly = rejectMask == 0;
				if (rejectMask != 0 && ShouldTracePtPerf())
				{
					RuntimeResidentBlasRefitRejectTraceEntry traceEntry = {};
					traceEntry.valid = true;
					traceEntry.chunkIndex = mapChunk.chunkIndex;
					traceEntry.sectorIndex = mapChunk.sectorIndex;
					traceEntry.reasonMask = appliedReasonMask;
					traceEntry.rejectMask = rejectMask;
					traceEntry.previousIndexCount = entry.indexCount;
					traceEntry.liveIndexCount = effectiveResidentIndexCount;
					traceEntry.previousPrimitiveCount = entry.primitiveCount;
					traceEntry.livePrimitiveCount = effectiveResidentPrimitiveCount;
					traceEntry.hadAccelerationStructure = hadAccelerationStructure;
					InsertRankedTraceEntry(
						mLastPerfShellTraceStats.runtimeResidentBlasRefitRejectEntries,
						traceEntry,
						ScoreRuntimeResidentBlasRefitRejectTraceEntry);
				}
			}
			if (preserveResidentIndexSlice)
			{
				mLastPerfShellTraceStats.runtimeMutationResidentApplyPreserveIndexCount++;
			}
			if (preserveResidentPrimitiveSlice)
			{
				mLastPerfShellTraceStats.runtimeMutationResidentApplyPreservePrimitiveCount++;
			}
			if (preserveResidentBlasRefitOnly)
			{
				mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRefitOnlyCount++;
			}
			useFixedLayoutVertexSpans =
				replacement.fixedLayoutDeformer &&
				fixedLayoutVertexSpansValid &&
				keptGeometrySlices &&
				preserveResidentIndexSlice;
			useFixedLayoutPrimitiveSpans =
				replacement.fixedLayoutDeformer &&
				fixedLayoutPrimitiveSpansValid &&
				keptGeometrySlices &&
				preserveResidentIndexSlice &&
				keptMaterialSlice &&
				!preserveResidentPrimitiveSlice;

			mStaticMapChunkAtlas = std::move(nextAtlasState);
			mStaticMapChunkAtlas.chunks[chunkListIndex] = nextAtlasChunk;
			mStaticMapScene.geometry.vertices.resize(std::max<size_t>(mStaticMapScene.geometry.vertices.size(), mStaticMapChunkAtlas.vertexCount));
			mStaticMapScene.geometry.indices.resize(std::max<size_t>(mStaticMapScene.geometry.indices.size(), mStaticMapChunkAtlas.indexCount));
			mStaticMapScene.geometry.primitives.resize(std::max<size_t>(mStaticMapScene.geometry.primitives.size(), mStaticMapChunkAtlas.primitiveCount));
			mStaticMapScene.geometry.primitiveProvenance.resize(std::max<size_t>(mStaticMapScene.geometry.primitiveProvenance.size(), mStaticMapChunkAtlas.primitiveCount));
			mStaticMapScene.materialBridge.materials.resize(std::max<size_t>(mStaticMapScene.materialBridge.materials.size(), mStaticMapChunkAtlas.materialCount));
			mStaticMapScene.materialBridge.lightMetadata.resize(std::max<size_t>(mStaticMapScene.materialBridge.lightMetadata.size(), mStaticMapChunkAtlas.materialCount));
			mStaticMapScene.gpuMaterials.resize(std::max<size_t>(mStaticMapScene.gpuMaterials.size(), mStaticMapChunkAtlas.materialCount));
		}

		if (!fastResidentMaterialOnlyUpdate &&
			!preserveResidentGeometryForMaterialOnlyUpdate &&
			!residentGeometryPayloadHashSkip)
		{
			{
				ScopedPtPerfTimer detailPerfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyVertexIndexCopyMs);
				if (preserveResidentIndexSlice)
				{
					{
						ScopedPtPerfTimer vertexCpuTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyVertexCpuCopyMs);
						nri_static_scene_geometry::CopyChunkVertexDataToAtlas(
							residentGeometry,
							sourceChunk,
							nextAtlasChunk,
							mStaticMapScene.geometry.vertices);
					}
					if (useFixedLayoutVertexSpans)
					{
						for (const RuntimeMutationResidentElementSpan& span : replacement.fixedLayoutVertexSpans)
						{
							if (span.elementCount == 0 || span.firstElement >= nextAtlasChunk.vertexCount ||
								span.elementCount > nextAtlasChunk.vertexCount - span.firstElement ||
								!mRuntimeMutation.QueueResidentGeometryUploadRange(
									ResidentUploadKind_Vertex,
									(uint64_t)(nextAtlasChunk.vertexOffset + span.firstElement) * sizeof(nri_scene::SceneVertex),
									(uint64_t)span.elementCount * sizeof(nri_scene::SceneVertex),
									NRIRuntimeMutationSystem::BuildResidentUploadServices(*this)))
							{
								return false;
							}
						}
					}
					else
					{
						const uint64_t vertexBytes = (uint64_t)nextAtlasChunk.vertexCount * sizeof(nri_scene::SceneVertex);
						if (!mRuntimeMutation.QueueResidentGeometryUploadRange(
								ResidentUploadKind_Vertex,
								(uint64_t)nextAtlasChunk.vertexOffset * sizeof(nri_scene::SceneVertex),
								vertexBytes,
								NRIRuntimeMutationSystem::BuildResidentUploadServices(*this)))
						{
							return false;
						}
					}
				}
				else
				{
					{
						ScopedPtPerfTimer vertexCpuTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyVertexCpuCopyMs);
						nri_static_scene_geometry::CopyChunkVertexDataToAtlas(
							residentGeometry,
							sourceChunk,
							nextAtlasChunk,
							mStaticMapScene.geometry.vertices);
					}
					{
						ScopedPtPerfTimer indexCpuTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyIndexCpuCopyMs);
						nri_static_scene_geometry::CopyChunkIndexDataToAtlas(
							residentGeometry,
							sourceChunk,
							nextAtlasChunk,
							mStaticMapScene.geometry.indices);
					}

					const uint64_t vertexBytes = (uint64_t)nextAtlasChunk.vertexCount * sizeof(nri_scene::SceneVertex);
					const uint64_t indexBytes = (uint64_t)nextAtlasChunk.indexCount * sizeof(uint32_t);
					if (!mRuntimeMutation.QueueResidentGeometryUploadRange(
							ResidentUploadKind_Vertex,
							(uint64_t)nextAtlasChunk.vertexOffset * sizeof(nri_scene::SceneVertex),
							vertexBytes,
							NRIRuntimeMutationSystem::BuildResidentUploadServices(*this)))
					{
						return false;
					}

					if (!mRuntimeMutation.QueueResidentGeometryUploadRange(
							ResidentUploadKind_Index,
							(uint64_t)nextAtlasChunk.indexOffset * sizeof(uint32_t),
							indexBytes,
							NRIRuntimeMutationSystem::BuildResidentUploadServices(*this)))
					{
						return false;
					}
				}
			}
			{
				ScopedPtPerfTimer detailPerfTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyPrimitiveRewriteMs);
				if (!preserveResidentPrimitiveSlice)
				{
					{
						ScopedPtPerfTimer primitiveCpuTimer(mLastPerfShellTraceStats.runtimeMutationResidentApplyPrimitiveCpuRewriteMs);
						nri_static_scene_geometry::CopyChunkPrimitiveDataToAtlas(
							mMapWorld,
							residentGeometry,
							sourceChunk,
							nextAtlasChunk,
							mStaticMapScene.geometry.primitives);
						if (residentGeometry.primitiveProvenance.size() >= nextAtlasChunk.primitiveCount &&
							nextAtlasChunk.primitiveOffset + nextAtlasChunk.primitiveCount <= mStaticMapScene.geometry.primitiveProvenance.size())
						{
							std::copy_n(
								residentGeometry.primitiveProvenance.data(),
								nextAtlasChunk.primitiveCount,
								mStaticMapScene.geometry.primitiveProvenance.data() + nextAtlasChunk.primitiveOffset);
						}
					}
					if (useFixedLayoutPrimitiveSpans)
					{
						for (const RuntimeMutationResidentElementSpan& span : replacement.fixedLayoutPrimitiveSpans)
						{
							if (span.elementCount == 0 || span.firstElement >= nextAtlasChunk.primitiveCount ||
								span.elementCount > nextAtlasChunk.primitiveCount - span.firstElement ||
								!mRuntimeMutation.QueueResidentGeometryUploadRange(
									ResidentUploadKind_Primitive,
									(uint64_t)(nextAtlasChunk.primitiveOffset + span.firstElement) * sizeof(nri_scene::PrimitiveData),
									(uint64_t)span.elementCount * sizeof(nri_scene::PrimitiveData),
									NRIRuntimeMutationSystem::BuildResidentUploadServices(*this)))
							{
								return false;
							}
						}
					}
					else
					{
						const uint64_t primitiveBytes = (uint64_t)nextAtlasChunk.primitiveCount * sizeof(nri_scene::PrimitiveData);
						if (!mRuntimeMutation.QueueResidentGeometryUploadRange(
								ResidentUploadKind_Primitive,
								(uint64_t)nextAtlasChunk.primitiveOffset * sizeof(nri_scene::PrimitiveData),
								primitiveBytes,
								NRIRuntimeMutationSystem::BuildResidentUploadServices(*this)))
						{
							return false;
						}
					}
				}
			}
			if (useFixedLayoutVertexSpans)
			{
				for (const RuntimeMutationResidentElementSpan& span : replacement.fixedLayoutVertexSpans)
				{
					mLastPerfResourceTraceStats.residentChunkBatchVertexBytes +=
						(uint64_t)span.elementCount * sizeof(nri_scene::SceneVertex);
				}
			}
			else
			{
				mLastPerfResourceTraceStats.residentChunkBatchVertexBytes +=
					(uint64_t)nextAtlasChunk.vertexCount * sizeof(nri_scene::SceneVertex);
			}
			if (!preserveResidentIndexSlice)
			{
				mLastPerfResourceTraceStats.residentChunkBatchIndexBytes +=
					(uint64_t)nextAtlasChunk.indexCount * sizeof(uint32_t);
			}
			if (useFixedLayoutPrimitiveSpans)
			{
				for (const RuntimeMutationResidentElementSpan& span : replacement.fixedLayoutPrimitiveSpans)
				{
					mLastPerfResourceTraceStats.residentChunkBatchPrimitiveBytes +=
						(uint64_t)span.elementCount * sizeof(nri_scene::PrimitiveData);
				}
			}
			else if (!preserveResidentPrimitiveSlice)
			{
				mLastPerfResourceTraceStats.residentChunkBatchPrimitiveBytes +=
					(uint64_t)nextAtlasChunk.primitiveCount * sizeof(nri_scene::PrimitiveData);
			}
		}

		auto& mutableChunk = mStaticMapScene.chunks[chunkListIndex];
		mutableChunk.chunkIndex = mapChunk.chunkIndex;
		mutableChunk.vertexOffset = nextAtlasChunk.vertexOffset;
		mutableChunk.vertexCount = nextAtlasChunk.vertexCount;
		mutableChunk.indexOffset = nextAtlasChunk.indexOffset;
		mutableChunk.indexCount = nextAtlasChunk.indexCount;
		mutableChunk.primitiveOffset = nextAtlasChunk.primitiveOffset;
		mutableChunk.primitiveCount = nextAtlasChunk.primitiveCount;
		mutableChunk.materialOffset = nextAtlasChunk.materialOffset;
		mutableChunk.materialCount = nextAtlasChunk.materialCount;
		mutableChunk.active = true;
		if (!fastResidentMaterialOnlyUpdate)
		{
			mutableChunk.canonicalMaterialLayout = {};
			mutableChunk.blasUpdateEligible = preserveResidentIndexSlice || preserveResidentBlasRefitOnly;
			mutableChunk.lastResidentBlasReasonMask = appliedReasonMask;
			mutableChunk.lastResidentBlasSurfaceCount = outResult.surfaceCount;
			mutableChunk.lastResidentBlasTriangleCount = outResult.triangleCount;
			mutableChunk.lastResidentBlasMaterialCount = outResult.materialCount;
			mutableChunk.lastResidentBlasForceTopology = appliedForceTopology;
			mutableChunk.lastResidentBlasRecoveredEmpty = outResult.recoveredEmpty;
			mutableChunk.lastResidentBlasKeptGeometrySlice = keptGeometrySlices;
			mutableChunk.lastResidentBlasTopologyChanged =
				hasResidentChunk &&
				previousGeometryTopologySignature != residentGeometryTopologySignature;
			mutableChunk.fixedLayoutDeformerKey =
				replacement.fixedLayoutDeformer ? replacement.fixedLayoutDeformerKey : 0;
			mutableChunk.fixedLayoutVertexSpanCount =
				useFixedLayoutVertexSpans ? (uint32_t)replacement.fixedLayoutVertexSpans.size() : 0;
			mutableChunk.fixedLayoutPrimitiveSpanCount =
				useFixedLayoutPrimitiveSpans ? (uint32_t)replacement.fixedLayoutPrimitiveSpans.size() : 0;
			mutableChunk.fixedLayoutVertexBytes = 0;
			mutableChunk.fixedLayoutPrimitiveBytes = 0;
			if (useFixedLayoutVertexSpans)
			{
				for (const RuntimeMutationResidentElementSpan& span : replacement.fixedLayoutVertexSpans)
				{
					mutableChunk.fixedLayoutVertexBytes +=
						(uint64_t)span.elementCount * sizeof(nri_scene::SceneVertex);
				}
			}
			if (useFixedLayoutPrimitiveSpans)
			{
				for (const RuntimeMutationResidentElementSpan& span : replacement.fixedLayoutPrimitiveSpans)
				{
					mutableChunk.fixedLayoutPrimitiveBytes +=
						(uint64_t)span.elementCount * sizeof(nri_scene::PrimitiveData);
				}
			}
		}
		if (!preserveResidentMaterialSlice)
		{
			mutableChunk.materialBridge = residentMaterials;
		}
		mutableChunk.geometryTopologySignature = residentGeometryTopologySignature;
		mutableChunk.primitiveLayoutSignature = residentPrimitiveLayoutSignature;
		if (!fastResidentMaterialOnlyUpdate)
		{
			mutableChunk.exactGeometrySignature = ComputeExactGeometrySignature(residentSceneView);
		}
		if (!residentGeometryPayloadHashSkip &&
			!fastResidentMaterialOnlyUpdate &&
			!preserveResidentGeometryForMaterialOnlyUpdate)
		{
			if (residentGeometryPayloadHash == 0)
			{
				residentGeometryPayloadHash = nri_static_scene_geometry::HashResidentGeometryPayload(
					mMapWorld,
					residentGeometry,
					sourceChunk.vertexOffset,
					sourceChunk.vertexCount,
					sourceChunk.indexOffset,
					sourceChunk.indexCount,
					sourceChunk.primitiveOffset,
					sourceChunk.primitiveCount,
					sourceChunk.materialOffset,
					sourceChunk.materialCount);
			}
			mutableChunk.geometryPayloadHash = residentGeometryPayloadHash;
		}
		mutableChunk.animatedMaterialSignature = ComputeAnimatedMaterialSignature(residentSceneView);
		mutableChunk.animatedGeometrySignature = ComputeAnimatedGeometrySignature(residentSceneView);
		mutableChunk.hasAnimatedTextureCandidates = ChunkHasAnimatedStaticMapSurfaceCandidates(mMapWorld, mapChunk);
		mutableChunk.animatedRefreshSuppressed =
			previousAnimatedRefreshSuppressed &&
			(appliedStaticAnimatedReplacement ||
			 appliedAnimationOnlyRefreshed ||
			 mutableChunk.animatedGeometrySignature == previousAnimatedGeometrySignature);
		mStaticMapScene.lightChunkViews[chunkListIndex] = residentSceneView;
		outResult.staticSceneChunkListIndex = chunkListIndex;
		outResult.materialDirty = !preserveResidentMaterialSlice;
		appliedPreservedResidentMaterialSlice = preserveResidentMaterialSlice;
		appliedPreservedResidentGeometryPayload =
			residentGeometryPayloadHashSkip ||
			fastResidentMaterialOnlyUpdate ||
			preserveResidentGeometryForMaterialOnlyUpdate;
		outResult.geometryDirty =
			!residentGeometryPayloadHashSkip &&
			((!preserveResidentGeometryForMaterialOnlyUpdate && !keptGeometrySlices) ||
			(appliedReasonMask & (
				nri_scene::PTMapChunkMutationReason_SectorGeometry |
				nri_scene::PTMapChunkMutationReason_WallGeometry |
				nri_scene::PTMapChunkMutationReason_SectorDirty |
				nri_scene::PTMapChunkMutationReason_SectionDirty)) != 0);
	}

	replacement.baseline = appliedBaseline;
	replacement.replacementBaseline = appliedBaseline;
	replacement.baselineSignature = appliedBaseline.signature;
	replacement.liveSignature = appliedBaseline.signature;
	replacement.reasonMask = nri_scene::PTMapChunkMutationReason_None;
	replacement.sectionDirtyCount = 0;
	replacement.stableMutationFrameCount = 0;
	replacement.sectorDirty = false;
	replacement.dragged = false;
	replacement.blindSpot = false;
	replacement.excludeStaticChunk = false;
	replacement.staticAnimatedReplacement = false;
	replacement.active = false;
	replacement.valid = false;
	replacement.residentAuthoritative = true;
	replacement.animationOnlyRefreshed = false;
	replacement.animatedMaterialSignature = ComputeAnimatedMaterialSignature(residentSceneView);
	replacement.surfaceCount = 0;
	replacement.triangleCount = 0;
	mRuntimeMutation.ClearReplacementPayload(replacement, !appliedCertifiedResidentMaterialOnly);

	mStaticSceneResidency.Registry().entries[mapChunk.chunkIndex].appliedBaseline = appliedBaseline;
	mStaticSceneResidency.Registry().entries[mapChunk.chunkIndex].baselineSignature = appliedBaseline.signature;
	mStaticSceneResidency.Registry().entries[mapChunk.chunkIndex].liveSignature = appliedBaseline.signature;
	mStaticSceneResidency.Registry().entries[mapChunk.chunkIndex].staticSceneChunkListIndex = chunkListIndex;
	mStaticSceneResidency.Registry().entries[mapChunk.chunkIndex].active = !chunkBecomesEmpty;
	mStaticSceneResidency.Registry().entries[mapChunk.chunkIndex].mappedInStaticScene = !chunkBecomesEmpty;
	mStaticSceneResidency.Registry().entries[mapChunk.chunkIndex].exactGeometrySignature =
		!chunkBecomesEmpty ? mStaticMapScene.chunks[chunkListIndex].exactGeometrySignature : 0;
	mStaticSceneResidency.Registry().entries[mapChunk.chunkIndex].geometryTopologySignature =
		!chunkBecomesEmpty ? mStaticMapScene.chunks[chunkListIndex].geometryTopologySignature : 0;
	mStaticSceneResidency.Registry().entries[mapChunk.chunkIndex].animatedGeometrySignature =
		!chunkBecomesEmpty ? mStaticMapScene.chunks[chunkListIndex].animatedGeometrySignature : 0;
	mStaticSceneResidency.Registry().entries[mapChunk.chunkIndex].animatedMaterialSignature =
		!chunkBecomesEmpty ? mStaticMapScene.chunks[chunkListIndex].animatedMaterialSignature : 0;
	mStaticSceneResidency.Registry().entries[mapChunk.chunkIndex].hasAnimatedTextureCandidates =
		!chunkBecomesEmpty && mStaticMapScene.chunks[chunkListIndex].hasAnimatedTextureCandidates;
	mStaticSceneResidency.Registry().entries[mapChunk.chunkIndex].animatedRefreshSuppressed =
		!chunkBecomesEmpty && mStaticMapScene.chunks[chunkListIndex].animatedRefreshSuppressed;
	if (!chunkBecomesEmpty)
	{
		mStaticSceneResidency.Registry().entries[mapChunk.chunkIndex].vertexOffset = mStaticMapChunkAtlas.chunks[chunkListIndex].vertexOffset;
		mStaticSceneResidency.Registry().entries[mapChunk.chunkIndex].vertexCount = mStaticMapChunkAtlas.chunks[chunkListIndex].vertexCount;
		mStaticSceneResidency.Registry().entries[mapChunk.chunkIndex].indexOffset = mStaticMapChunkAtlas.chunks[chunkListIndex].indexOffset;
		mStaticSceneResidency.Registry().entries[mapChunk.chunkIndex].indexCount = mStaticMapChunkAtlas.chunks[chunkListIndex].indexCount;
		mStaticSceneResidency.Registry().entries[mapChunk.chunkIndex].primitiveOffset = mStaticMapChunkAtlas.chunks[chunkListIndex].primitiveOffset;
		mStaticSceneResidency.Registry().entries[mapChunk.chunkIndex].primitiveCount = mStaticMapChunkAtlas.chunks[chunkListIndex].primitiveCount;
		mStaticSceneResidency.Registry().entries[mapChunk.chunkIndex].materialOffset = mStaticMapChunkAtlas.chunks[chunkListIndex].materialOffset;
		mStaticSceneResidency.Registry().entries[mapChunk.chunkIndex].materialCount = mStaticMapChunkAtlas.chunks[chunkListIndex].materialCount;
		if (!appliedPreservedResidentMaterialSlice)
		{
			mStaticSceneResidency.Registry().entries[mapChunk.chunkIndex].materialPayloadHash = residentMaterialPayloadHash;
		}
		if (!appliedPreservedResidentGeometryPayload)
		{
			mStaticSceneResidency.Registry().entries[mapChunk.chunkIndex].geometryPayloadHash = residentGeometryPayloadHash;
		}
		mStaticSceneResidency.Registry().entries[mapChunk.chunkIndex].accelerationResident =
			mStaticMapScene.chunks[chunkListIndex].accelerationStructure.accelerationStructure != nullptr;
	}
	else
	{
		mStaticSceneResidency.Registry().entries[mapChunk.chunkIndex].vertexOffset = 0;
		mStaticSceneResidency.Registry().entries[mapChunk.chunkIndex].vertexCount = 0;
		mStaticSceneResidency.Registry().entries[mapChunk.chunkIndex].indexOffset = 0;
		mStaticSceneResidency.Registry().entries[mapChunk.chunkIndex].indexCount = 0;
		mStaticSceneResidency.Registry().entries[mapChunk.chunkIndex].primitiveOffset = 0;
		mStaticSceneResidency.Registry().entries[mapChunk.chunkIndex].primitiveCount = 0;
		mStaticSceneResidency.Registry().entries[mapChunk.chunkIndex].materialOffset = 0;
		mStaticSceneResidency.Registry().entries[mapChunk.chunkIndex].materialCount = 0;
		mStaticSceneResidency.Registry().entries[mapChunk.chunkIndex].materialPayloadHash = 0;
		mStaticSceneResidency.Registry().entries[mapChunk.chunkIndex].geometryPayloadHash = 0;
		mStaticSceneResidency.Registry().entries[mapChunk.chunkIndex].accelerationResident = false;
	}
	if (!outResult.materialDirty && !residentMaterialPayloadHashSkip)
	{
		outResult.materialDirty =
			(appliedReasonMask & (nri_scene::PTMapChunkMutationReason_SectorMaterial | nri_scene::PTMapChunkMutationReason_WallMaterial)) != 0 ||
			appliedStaticAnimatedReplacement ||
			appliedAnimationOnlyRefreshed ||
			materialOnlyReplacement;
	}
	if (!outResult.geometryDirty && !residentGeometryPayloadHashSkip)
	{
		outResult.geometryDirty =
			(appliedReasonMask & (
				nri_scene::PTMapChunkMutationReason_SectorGeometry |
				nri_scene::PTMapChunkMutationReason_WallGeometry |
				nri_scene::PTMapChunkMutationReason_SectorDirty |
				nri_scene::PTMapChunkMutationReason_SectionDirty)) != 0;
	}
	return true;
}
