#include "nri_runtime_mutation.h"

#include "../scene/nri_hash.h"

#include "printf.h"

#include <algorithm>
#include <cstddef>
#include <unordered_map>

namespace
{
	struct RuntimeMutationLightIdentitySurfaceKey
	{
		uint32_t kind = UINT32_MAX;
		uint32_t sourceType = (uint32_t)nri_scene::SurfaceSourceType::Unknown;
		int32_t sectorIndex = -1;
		int32_t wallIndex = -1;
		int32_t sectionIndex = -1;
		int32_t nextSectorIndex = -1;
		int32_t actorIndex = -1;
		uint32_t cstat = 0;
		uint32_t materialFlags = 0;
		uint32_t primaryKey = UINT32_MAX;
		uint32_t secondaryKey = UINT32_MAX;

		bool operator==(const RuntimeMutationLightIdentitySurfaceKey& other) const
		{
			return kind == other.kind &&
				sourceType == other.sourceType &&
				sectorIndex == other.sectorIndex &&
				wallIndex == other.wallIndex &&
				sectionIndex == other.sectionIndex &&
				nextSectorIndex == other.nextSectorIndex &&
				actorIndex == other.actorIndex &&
				cstat == other.cstat &&
				materialFlags == other.materialFlags &&
				primaryKey == other.primaryKey &&
				secondaryKey == other.secondaryKey;
		}
	};

	struct RuntimeMutationLightIdentitySurfaceKeyHash
	{
		size_t operator()(const RuntimeMutationLightIdentitySurfaceKey& key) const
		{
			size_t h = 1469598103934665603ull;
			const auto mix = [&h](uint64_t value)
			{
				h ^= (size_t)value;
				h *= 1099511628211ull;
			};
			mix(key.kind);
			mix(key.sourceType);
			mix((uint32_t)key.sectorIndex);
			mix((uint32_t)key.wallIndex);
			mix((uint32_t)key.sectionIndex);
			mix((uint32_t)key.nextSectorIndex);
			mix((uint32_t)key.actorIndex);
			mix(key.cstat);
			mix(key.materialFlags);
			mix(key.primaryKey);
			mix(key.secondaryKey);
			return h;
		}
	};

	static void AppendMutationReasonToken(std::string& text, const char* token)
	{
		if (!text.empty())
		{
			text += "|";
		}
		text += token;
	}

	static RuntimeMutationLightIdentitySurfaceKey BuildRuntimeMutationLightIdentitySurfaceKey(const nri_scene::PTMapSurface& surface)
	{
		RuntimeMutationLightIdentitySurfaceKey key = {};
		key.kind = (uint32_t)surface.kind;
		key.sourceType = (uint32_t)surface.surface.provenance.sourceType;
		key.sectorIndex = surface.surface.provenance.sectorIndex;
		key.wallIndex = surface.surface.provenance.wallIndex;
		key.sectionIndex = surface.surface.provenance.sectionIndex;
		key.nextSectorIndex = surface.surface.provenance.nextSectorIndex;
		key.actorIndex = surface.surface.provenance.actorIndex;
		key.cstat = surface.surface.provenance.cstat;
		key.materialFlags = surface.surface.provenance.materialFlags;
		key.primaryKey = surface.key.primary;
		key.secondaryKey = surface.key.secondary;
		return key;
	}

	static void ComputeRuntimeMutationSurfaceCenter(const nri_scene::SurfaceRef& surface, float outCenter[3])
	{
		outCenter[0] = 0.0f;
		outCenter[1] = 0.0f;
		outCenter[2] = 0.0f;
		if (surface.vertices.empty())
		{
			return;
		}

		for (const nri_scene::CapturedVertex& vertex : surface.vertices)
		{
			outCenter[0] += vertex.position[0];
			outCenter[1] += vertex.position[1];
			outCenter[2] += vertex.position[2];
		}

		const float invCount = 1.0f / (float)surface.vertices.size();
		outCenter[0] *= invCount;
		outCenter[1] *= invCount;
		outCenter[2] *= invCount;
	}
}

bool NRIRuntimeMutationResidentUploadServices::StageGeometryRanges(const std::vector<RuntimeMutationResidentUploadRange>& ranges) const
{
	return stageGeometryRanges != nullptr && stageGeometryRanges(user, ranges);
}

void NRIRuntimeMutationResidentUploadServices::NoteUploadRange(int uploadKind, uint64_t size) const
{
	if (noteUploadRange != nullptr)
	{
		noteUploadRange(user, uploadKind, size);
	}
}

void NRIRuntimeMutationResidentUploadServices::NoteCoalescedRange(const RuntimeMutationResidentUploadRange& range) const
{
	if (noteCoalescedRange != nullptr)
	{
		noteCoalescedRange(user, range);
	}
}

void NRIRuntimeMutationResidentUploadServices::NoteCoalescedReject() const
{
	if (noteCoalescedReject != nullptr)
	{
		noteCoalescedReject(user);
	}
}

bool NRIRuntimeMutationResidentApplyServices::TryApplyChunk(
	const nri_scene::PTMapChunk& mapChunk,
	RuntimeMapMutationCache::ChunkReplacement& replacement,
	RuntimeMutationResidentApplyResult& outResult) const
{
	outResult = {};
	return tryApplyChunk != nullptr && tryApplyChunk(user, mapChunk, replacement, outResult);
}

bool NRIRuntimeMutationOverlayServices::BuildOverlay(
	nri_scene::GeometryData& outGeometry,
	nri_scene::MaterialBridgeData& outMaterials,
	bool* outResidentStaticSceneChanged) const
{
	return buildOverlay != nullptr && buildOverlay(user, outGeometry, outMaterials, outResidentStaticSceneChanged);
}

bool NRIRuntimeMutationResidentSceneRefreshServices::RefreshMaterialSlices(
	const std::vector<uint32_t>& chunkListIndices,
	const std::vector<uint32_t>& animatedChunkListIndices) const
{
	return refreshMaterialSlices != nullptr && refreshMaterialSlices(user, chunkListIndices, animatedChunkListIndices);
}

void NRIRuntimeMutationResidentSceneRefreshServices::NoteMaterialFallback(uint32_t chunkCount) const
{
	if (noteMaterialFallback != nullptr)
	{
		noteMaterialFallback(user, chunkCount);
	}
}

bool NRIRuntimeMutationResidentSceneRefreshServices::RebuildMaterialState(const char* reason) const
{
	return rebuildMaterialState != nullptr && rebuildMaterialState(user, reason);
}

bool NRIRuntimeMutationResidentSceneRefreshServices::RebuildChunkBlases(const std::vector<uint32_t>& chunkListIndices) const
{
	return rebuildChunkBlases != nullptr && rebuildChunkBlases(user, chunkListIndices);
}

void NRIRuntimeMutationResidentSceneRefreshServices::NoteBlasRebuild(uint32_t chunkCount) const
{
	if (noteBlasRebuild != nullptr)
	{
		noteBlasRebuild(user, chunkCount);
	}
}

void NRIRuntimeMutationResidentSceneRefreshServices::CommitSuccess(
	const std::vector<uint32_t>& geometryChunkListIndices,
	bool materialDirty) const
{
	if (commitSuccess != nullptr)
	{
		commitSuccess(user, geometryChunkListIndices, materialDirty);
	}
}

bool NRIRuntimeMutationResidentSceneRefreshServices::RecoverFailure(const char* reason) const
{
	return recoverFailure != nullptr && recoverFailure(user, reason);
}

const char* GetRuntimeMutationTraceActionName(RuntimeMutationTraceAction action)
{
	switch (action)
	{
	case RuntimeMutationTraceAction::StructuralRebuild: return "rebuild";
	case RuntimeMutationTraceAction::MaterialRefresh: return "material-refresh";
	case RuntimeMutationTraceAction::ResidentApply: return "resident-apply";
	case RuntimeMutationTraceAction::ResidentNoopSkip: return "resident-noop-skip";
	case RuntimeMutationTraceAction::ResidentFallback: return "fallback";
	case RuntimeMutationTraceAction::Held: return "held";
	case RuntimeMutationTraceAction::SyncSkip: return "sync-skip";
	case RuntimeMutationTraceAction::DeferredMaterialRefresh: return "deferred-material-refresh";
	case RuntimeMutationTraceAction::DeferredStructuralRebuild: return "deferred-structural-rebuild";
	case RuntimeMutationTraceAction::Failed: return "failed";
	default: return "none";
	}
}

uint32_t ScoreRuntimeMutationTopTraceEntry(const RuntimeMutationTopTraceEntry& entry)
{
	uint32_t score = entry.triangleCount * 16u + entry.materialCount * 32u + entry.surfaceCount * 8u;
	if (entry.forceTopology)
	{
		score += 1u << 20;
	}
	if (entry.recoveredEmpty)
	{
		score += 1u << 19;
	}
	if (entry.residentGeometryDirty)
	{
		score += 1u << 18;
	}
	if (entry.residentMaterialDirty)
	{
		score += 1u << 17;
	}
	switch (entry.action)
	{
	case RuntimeMutationTraceAction::ResidentFallback:
		score += 1u << 16;
		break;
	case RuntimeMutationTraceAction::ResidentNoopSkip:
		score += 1u << 15;
		break;
	case RuntimeMutationTraceAction::ResidentApply:
		score += 1u << 14;
		break;
	case RuntimeMutationTraceAction::StructuralRebuild:
		score += 1u << 13;
		break;
	case RuntimeMutationTraceAction::MaterialRefresh:
		score += 1u << 12;
		break;
	case RuntimeMutationTraceAction::SyncSkip:
		score += 1u << 11;
		break;
	case RuntimeMutationTraceAction::DeferredMaterialRefresh:
		score += 1u << 10;
		break;
	default:
		break;
	}
	return score;
}

std::string GetRuntimeMapMutationReasonSummary(uint32_t reasonMask)
{
	std::string text;
	if ((reasonMask & nri_scene::PTMapChunkMutationReason_SectorGeometry) != 0)
	{
		AppendMutationReasonToken(text, "sector_geom");
	}
	if ((reasonMask & nri_scene::PTMapChunkMutationReason_SectorMaterial) != 0)
	{
		AppendMutationReasonToken(text, "sector_mat");
	}
	if ((reasonMask & nri_scene::PTMapChunkMutationReason_WallGeometry) != 0)
	{
		AppendMutationReasonToken(text, "wall_geom");
	}
	if ((reasonMask & nri_scene::PTMapChunkMutationReason_WallMaterial) != 0)
	{
		AppendMutationReasonToken(text, "wall_mat");
	}
	if ((reasonMask & nri_scene::PTMapChunkMutationReason_SectorDirty) != 0)
	{
		AppendMutationReasonToken(text, "sector_dirty");
	}
	if ((reasonMask & nri_scene::PTMapChunkMutationReason_SectionDirty) != 0)
	{
		AppendMutationReasonToken(text, "section_dirty");
	}
	if ((reasonMask & nri_scene::PTMapChunkMutationReason_Dragged) != 0)
	{
		AppendMutationReasonToken(text, "dragged");
	}
	if (text.empty())
	{
		text = "none";
	}
	return text;
}

std::string GetRuntimeMutationWorklistCandidateSourceSummary(uint32_t sourceMask)
{
	std::string text;
	if ((sourceMask & RuntimeMutationWorklistCandidateSource_ActiveReplacement) != 0)
	{
		AppendMutationReasonToken(text, "active_replacement");
	}
	if ((sourceMask & RuntimeMutationWorklistCandidateSource_VisibleResidentValidation) != 0)
	{
		AppendMutationReasonToken(text, "visible_resident_validation");
	}
	if ((sourceMask & RuntimeMutationWorklistCandidateSource_StartupVisibleValidation) != 0)
	{
		AppendMutationReasonToken(text, "startup_visible_validation");
	}
	if ((sourceMask & RuntimeMutationWorklistCandidateSource_UnresolvedAuthoredTextures) != 0)
	{
		AppendMutationReasonToken(text, "unresolved_authored_textures");
	}
	if ((sourceMask & RuntimeMutationWorklistCandidateSource_StaticAnimatedSuppressed) != 0)
	{
		AppendMutationReasonToken(text, "static_animated_suppressed");
	}
	if ((sourceMask & RuntimeMutationWorklistCandidateSource_SectorDirty) != 0)
	{
		AppendMutationReasonToken(text, "sector_dirty");
	}
	if ((sourceMask & RuntimeMutationWorklistCandidateSource_SectionDirty) != 0)
	{
		AppendMutationReasonToken(text, "section_dirty");
	}
	if ((sourceMask & RuntimeMutationWorklistCandidateSource_Dragged) != 0)
	{
		AppendMutationReasonToken(text, "dragged");
	}
	if ((sourceMask & RuntimeMutationWorklistCandidateSource_SignatureWatchlist) != 0)
	{
		AppendMutationReasonToken(text, "signature_watchlist");
	}
	if ((sourceMask & RuntimeMutationWorklistCandidateSource_BackgroundSweep) != 0)
	{
		AppendMutationReasonToken(text, "background_sweep");
	}
	if ((sourceMask & RuntimeMutationWorklistCandidateSource_DeferredMaterialRefresh) != 0)
	{
		AppendMutationReasonToken(text, "deferred_material_refresh");
	}
	if ((sourceMask & RuntimeMutationWorklistCandidateSource_DeferredStructuralRebuild) != 0)
	{
		AppendMutationReasonToken(text, "deferred_structural_rebuild");
	}
	if ((sourceMask & RuntimeMutationWorklistCandidateSource_MotionSettle) != 0)
	{
		AppendMutationReasonToken(text, "motion_settle");
	}
	if ((sourceMask & RuntimeMutationWorklistCandidateSource_ValidationRecovery) != 0)
	{
		AppendMutationReasonToken(text, "validation_recovery");
	}
	if (text.empty())
	{
		text = "none";
	}
	return text;
}

bool IsRuntimeMutationMaterialOnlyReasonMask(uint32_t reasonMask)
{
	const uint32_t materialOnlyReasonMask =
		nri_scene::PTMapChunkMutationReason_SectorMaterial |
		nri_scene::PTMapChunkMutationReason_WallMaterial;
	return
		(reasonMask & materialOnlyReasonMask) != 0 &&
		(reasonMask & ~materialOnlyReasonMask) == 0;
}

bool RequiresExclusiveRuntimeMutationMaterialOnlyReasonMask(uint32_t reasonMask)
{
	// Material-only wall mutations leave the stale static wall traceable if
	// we only overlay the changed wall subset. Replacing the whole rebuilt
	// live chunk avoids that without dropping unrelated geometry.
	return (reasonMask & nri_scene::PTMapChunkMutationReason_WallMaterial) != 0;
}

bool IsPureSectorRuntimeMutationMaterialOnlyReasonMask(uint32_t reasonMask)
{
	return reasonMask == nri_scene::PTMapChunkMutationReason_SectorMaterial;
}

RuntimeMutationCacheStats NRIRuntimeMutationSystem::GatherCacheStats() const
{
	RuntimeMutationCacheStats stats = {};
	for (const auto& replacement : cache.chunks)
	{
		stats.cachedMaterialStateCount += (uint32_t)replacement.materialStateCache.size();
		if (!replacement.active)
		{
			continue;
		}

		stats.activeChunkCount++;
		if (!replacement.valid)
		{
			continue;
		}

		stats.validChunkCount++;
		if (replacement.excludeStaticChunk)
		{
			stats.excludedStaticChunkCount++;
		}

		stats.cachedSurfaceCount += replacement.surfaceCount;
		stats.cachedTriangleCount += replacement.triangleCount;
		stats.cachedMaterialCount += (uint32_t)replacement.materialBridge.materials.size();
	}

	return stats;
}

bool NRIRuntimeMutationSystem::IsCacheEmpty() const
{
	return cache.chunks.empty();
}

uint32_t NRIRuntimeMutationSystem::GetCacheChunkCount() const
{
	return (uint32_t)cache.chunks.size();
}

uint64_t NRIRuntimeMutationSystem::BuildFrameGenerationHash(bool hasRuntimeMutationOverlay) const
{
	if (!hasRuntimeMutationOverlay && !lastFrame.active)
	{
		return 0ull;
	}

	uint64_t hash = 1469598103934665603ull;
	hash = nri_scene::HashCombine64(hash, (uint64_t)lastFrame.dirtyChunkCount);
	hash = nri_scene::HashCombine64(hash, (uint64_t)lastFrame.residentAppliedChunkCount);
	hash = nri_scene::HashCombine64(hash, (uint64_t)lastFrame.residentGeometryChunkCount);
	hash = nri_scene::HashCombine64(hash, (uint64_t)lastFrame.residentMaterialChunkCount);
	hash = nri_scene::HashCombine64(hash, (uint64_t)lastFrame.rebuiltChunkCount);
	hash = nri_scene::HashCombine64(hash, (uint64_t)lastFrame.heldChunkCount);
	hash = nri_scene::HashCombine64(hash, (uint64_t)lastFrame.replacementTriangleCount);
	hash = nri_scene::HashCombine64(hash, (uint64_t)lastFrame.materialCount);
	return hash;
}

void NRIRuntimeMutationSystem::CollectResidentReplacementInfo(
	uint32_t chunkCount,
	std::vector<RuntimeMutationResidentReplacementInfo>& outReplacements) const
{
	outReplacements.clear();
	outReplacements.reserve(chunkCount);
	for (uint32_t chunkListIndex = 0; chunkListIndex < chunkCount; ++chunkListIndex)
	{
		const auto* replacement = FindReplacement(chunkListIndex);
		if (replacement == nullptr)
		{
			continue;
		}

		RuntimeMutationResidentReplacementInfo info = {};
		info.chunkListIndex = chunkListIndex;
		info.baseline = replacement->baseline;
		info.baselineSignature = replacement->baselineSignature;
		info.liveSignature = replacement->liveSignature;
		outReplacements.push_back(info);
	}
}

const RuntimeMapMutationCache::ChunkReplacement* NRIRuntimeMutationSystem::FindReplacement(uint32_t chunkIndex) const
{
	return chunkIndex < cache.chunks.size() ? &cache.chunks[chunkIndex] : nullptr;
}

RuntimeMapMutationCache::ChunkReplacement* NRIRuntimeMutationSystem::FindReplacementForUpdate(uint32_t chunkIndex)
{
	// Explicit update accesses include startup baseline correction. Ordinary
	// readers cannot enlist the full cache into per-frame reconciliation.
	discovery.NoteTouchedChunk(chunkIndex);
	return chunkIndex < cache.chunks.size() ? &cache.chunks[chunkIndex] : nullptr;
}

bool NRIRuntimeMutationSystem::IsReplacementActive(uint32_t chunkIndex) const
{
	const auto* replacement = FindReplacement(chunkIndex);
	return replacement != nullptr && replacement->active;
}

bool NRIRuntimeMutationSystem::IsReplacementActiveAndValid(uint32_t chunkIndex) const
{
	const auto* replacement = FindReplacement(chunkIndex);
	return replacement != nullptr && replacement->active && replacement->valid;
}

uint32_t NRIRuntimeMutationSystem::AppendSceneLightRecords(SceneLightSystem& sceneLights) const
{
	uint32_t runtimeMutationMaterialOffset = 0;
	for (const auto& replacement : cache.chunks)
	{
		if (!replacement.active || !replacement.valid)
		{
			continue;
		}

		sceneLights.AppendSceneView(
			replacement.sceneView,
			replacement.materialBridge,
			SceneLightRecordSource::RuntimeMutationScene,
			runtimeMutationMaterialOffset,
			0u,
			&replacement.lightIdentityOverrides);
		runtimeMutationMaterialOffset += (uint32_t)replacement.materialBridge.materials.size();
	}
	return runtimeMutationMaterialOffset;
}

void NRIRuntimeMutationSystem::BuildLightIdentityOverrides(
	const nri_scene::PTMapWorld& staticWorld,
	const nri_scene::PTMapChunk& staticChunk,
	const nri_scene::PTMapWorld& liveWorld,
	const nri_scene::PTMapChunk& liveChunk,
	SceneLightSystem::SurfaceIdentityOverrides& outOverrides) const
{
	outOverrides.Clear();
	if (!staticWorld.valid || !liveWorld.valid)
	{
		return;
	}

	std::vector<uint32_t> staticSurfaceIndices;
	std::vector<uint32_t> liveSurfaceIndices;
	staticSurfaceIndices.reserve(staticChunk.surfaceCount);
	liveSurfaceIndices.reserve(liveChunk.surfaceCount);

	for (uint32_t localSurfaceIndex = 0; localSurfaceIndex < staticChunk.surfaceCount; ++localSurfaceIndex)
	{
		const uint32_t surfaceIndex = staticChunk.firstSurface + localSurfaceIndex;
		if (surfaceIndex >= staticWorld.surfaces.size())
		{
			break;
		}
		staticSurfaceIndices.push_back(surfaceIndex);
	}

	for (uint32_t localSurfaceIndex = 0; localSurfaceIndex < liveChunk.surfaceCount; ++localSurfaceIndex)
	{
		const uint32_t surfaceIndex = liveChunk.firstSurface + localSurfaceIndex;
		if (surfaceIndex >= liveWorld.surfaces.size())
		{
			break;
		}
		liveSurfaceIndices.push_back(surfaceIndex);
	}

	std::unordered_map<RuntimeMutationLightIdentitySurfaceKey, std::vector<uint32_t>, RuntimeMutationLightIdentitySurfaceKeyHash> liveSurfaceLookup;
	liveSurfaceLookup.reserve(liveSurfaceIndices.size());
	for (uint32_t liveLocalIndex = 0; liveLocalIndex < (uint32_t)liveSurfaceIndices.size(); ++liveLocalIndex)
	{
		const auto& liveSurface = liveWorld.surfaces[liveSurfaceIndices[liveLocalIndex]];
		liveSurfaceLookup[BuildRuntimeMutationLightIdentitySurfaceKey(liveSurface)].push_back(liveLocalIndex);
	}

	std::vector<uint8_t> liveSurfaceUsed(liveSurfaceIndices.size(), 0u);
	std::vector<uint64_t> inheritedIdentityKeys(liveSurfaceIndices.size(), 0ull);
	float staticSurfaceCenter[3] = {};
	for (uint32_t staticSurfaceIndex : staticSurfaceIndices)
	{
		const auto& staticSurface = staticWorld.surfaces[staticSurfaceIndex];
		const RuntimeMutationLightIdentitySurfaceKey key = BuildRuntimeMutationLightIdentitySurfaceKey(staticSurface);
		auto liveSurfaceIt = liveSurfaceLookup.find(key);
		if (liveSurfaceIt == liveSurfaceLookup.end())
		{
			continue;
		}

		uint32_t matchedLiveLocalIndex = UINT32_MAX;
		for (uint32_t candidate : liveSurfaceIt->second)
		{
			if (candidate < liveSurfaceUsed.size() && liveSurfaceUsed[candidate] == 0u)
			{
				matchedLiveLocalIndex = candidate;
				break;
			}
		}
		if (matchedLiveLocalIndex == UINT32_MAX)
		{
			continue;
		}

		liveSurfaceUsed[matchedLiveLocalIndex] = 1u;
		ComputeRuntimeMutationSurfaceCenter(staticSurface.surface, staticSurfaceCenter);
		inheritedIdentityKeys[matchedLiveLocalIndex] = SceneLightSystem::ComputeSurfaceIdentityKey(
			SceneLightRecordSource::StaticMapScene,
			staticSurface.surface.provenance,
			staticSurfaceCenter);
	}

	outOverrides.opaqueWalls.reserve(liveWorld.stats.wallSurfaceCount);
	outOverrides.opaqueFlats.reserve(liveWorld.stats.flatSurfaceCount);
	for (uint32_t liveLocalIndex = 0; liveLocalIndex < (uint32_t)liveSurfaceIndices.size(); ++liveLocalIndex)
	{
		const auto& liveSurface = liveWorld.surfaces[liveSurfaceIndices[liveLocalIndex]];
		if ((liveSurface.surface.material.flags & nri_scene::MaterialFlag_Sky) != 0 && liveSurface.surface.material.texture != nullptr)
		{
			continue;
		}

		switch (liveSurface.kind)
		{
		case nri_scene::PTMapSurfaceKind::Floor:
		case nri_scene::PTMapSurfaceKind::Ceiling:
			outOverrides.opaqueFlats.push_back(inheritedIdentityKeys[liveLocalIndex]);
			break;
		default:
			outOverrides.opaqueWalls.push_back(inheritedIdentityKeys[liveLocalIndex]);
			break;
		}
	}
}

void NRIRuntimeMutationSystem::PrintStatus() const
{
	const RuntimeMutationCacheStats cacheStats = GatherCacheStats();

	Printf("NRI PT runtime map: active=%s dirty_chunks=%u resident_applied=%u resident_geom=%u resident_mat=%u resident_atlas_grows=%u resident_fallback=%u rebuilt_chunks=%u held_chunks=%u animated_refreshes=%u blind_spots=%u sector_geom=%u sector_mat=%u wall_geom=%u wall_mat=%u sector_dirty=%u section_dirty=%u dragged=%u surfaces=%u tris=%u materials=%u\n",
		lastFrame.active ? "yes" : "no",
		lastFrame.dirtyChunkCount,
		lastFrame.residentAppliedChunkCount,
		lastFrame.residentGeometryChunkCount,
		lastFrame.residentMaterialChunkCount,
		lastFrame.residentAtlasGrowCount,
		lastFrame.residentFallbackChunkCount,
		lastFrame.rebuiltChunkCount,
		lastFrame.heldChunkCount,
		lastFrame.animatedRefreshChunkCount,
		lastFrame.blindSpotChunkCount,
		lastFrame.sectorGeometryChunkCount,
		lastFrame.sectorMaterialChunkCount,
		lastFrame.wallGeometryChunkCount,
		lastFrame.wallMaterialChunkCount,
		lastFrame.sectorDirtyChunkCount,
		lastFrame.sectionDirtyChunkCount,
		lastFrame.draggedChunkCount,
		lastFrame.replacementSurfaceCount,
		lastFrame.replacementTriangleCount,
		lastFrame.materialCount);
	Printf("NRI PT runtime map cache: active_chunks=%u valid_chunks=%u exclude_static=%u cached_surfaces=%u cached_tris=%u cached_materials=%u cached_states=%u highwater=active:%u valid:%u exclude_static:%u surfaces:%u tris:%u mats:%u states:%u\n",
		cacheStats.activeChunkCount,
		cacheStats.validChunkCount,
		cacheStats.excludedStaticChunkCount,
		cacheStats.cachedSurfaceCount,
		cacheStats.cachedTriangleCount,
		cacheStats.cachedMaterialCount,
		cacheStats.cachedMaterialStateCount,
		cacheHighWaterStats.activeChunkCount,
		cacheHighWaterStats.validChunkCount,
		cacheHighWaterStats.excludedStaticChunkCount,
		cacheHighWaterStats.cachedSurfaceCount,
		cacheHighWaterStats.cachedTriangleCount,
		cacheHighWaterStats.cachedMaterialCount,
		cacheHighWaterStats.cachedMaterialStateCount);
}

void NRIRuntimeMutationSystem::ClearReplacementPayload(RuntimeMapMutationCache::ChunkReplacement& replacement, bool clearMaterialStateCache)
{
	replacement.lightIdentityOverrides.Clear();
	replacement.sceneView = {};
	replacement.geometry = {};
	replacement.materialBridge = {};
	replacement.exactGeometrySignature = 0;
	replacement.deferredMaterialRefresh = false;
	replacement.deferredMaterialFrame = 0;
	replacement.deferredStructuralRebuild = false;
	replacement.deferredStructuralFrame = 0;
	replacement.fixedLayoutDeformer = false;
	replacement.fixedLayoutDeformerKey = 0;
	replacement.fixedLayoutVertexSpans.clear();
	replacement.fixedLayoutPrimitiveSpans.clear();
	replacement.certifiedResidentMaterialOnly = false;
	replacement.certifiedMaterialBuildSerial = 0;
	replacement.certifiedMaterialMapEpoch = 0;
	replacement.certifiedMaterialOwnerStableId = UINT64_MAX;
	replacement.certifiedMaterialLayoutKey = 0;
	replacement.certifiedMaterialStateKey = 0;
	replacement.certifiedExactGeometrySignature = 0;
	replacement.certifiedGeometryTopologySignature = 0;
	replacement.certifiedPrimitiveLayoutSignature = 0;
	replacement.certifiedChunkListIndex = UINT32_MAX;
	replacement.certifiedVertexOffset = 0;
	replacement.certifiedVertexCount = 0;
	replacement.certifiedIndexOffset = 0;
	replacement.certifiedIndexCount = 0;
	replacement.certifiedPrimitiveOffset = 0;
	replacement.certifiedPrimitiveCount = 0;
	replacement.certifiedMaterialOffset = 0;
	replacement.certifiedMaterialCount = 0;
	if (clearMaterialStateCache)
	{
		replacement.materialStateCache.clear();
	}
}

void NRIRuntimeMutationSystem::TraceChunk(
	const nri_scene::PTMapChunk& mapChunk,
	RuntimeMapMutationCache::ChunkReplacement& replacement,
	bool traceEnabled,
	int traceChunkIndex,
	int traceSectorIndex)
{
	if (!traceEnabled)
	{
		return;
	}

	const bool filterByChunk = traceChunkIndex >= 0;
	const bool filterBySector = traceSectorIndex >= 0;
	if (!filterByChunk && !filterBySector)
	{
		return;
	}

	if (filterByChunk && mapChunk.chunkIndex != (uint32_t)traceChunkIndex)
	{
		return;
	}

	if (filterBySector && mapChunk.sectorIndex != traceSectorIndex)
	{
		return;
	}

	const bool changed =
		replacement.traceCount == 0 ||
		replacement.lastTraceSignature != replacement.liveSignature ||
		replacement.lastTraceAnimatedMaterialSignature != replacement.animatedMaterialSignature ||
		replacement.lastTraceReasonMask != replacement.reasonMask ||
		replacement.lastTraceActive != replacement.active ||
		replacement.lastTraceBlindSpot != replacement.blindSpot ||
		replacement.lastTraceAnimationOnlyRefreshed != replacement.animationOnlyRefreshed ||
		replacement.lastTraceStaticAnimatedReplacement != replacement.staticAnimatedReplacement;
	if (!changed)
	{
		return;
	}

	const std::string reasons = GetRuntimeMapMutationReasonSummary(replacement.reasonMask);
	Printf("NRI PT runtime map trace: chunk=%u sector=%d active=%s blind_spot=%s static_anim=%s signature_changed=%s anim_refresh=%s baseline_sig=0x%llx live_sig=0x%llx anim_sig=0x%llx reasons=%s section_dirty=%u sector_dirty=%s dragged=%s surfaces=%u tris=%u materials=%u\n",
		mapChunk.chunkIndex,
		mapChunk.sectorIndex,
		replacement.active ? "yes" : "no",
		replacement.blindSpot ? "yes" : "no",
		replacement.staticAnimatedReplacement ? "yes" : "no",
		replacement.liveSignature != replacement.baselineSignature ? "yes" : "no",
		replacement.animationOnlyRefreshed ? "yes" : "no",
		(unsigned long long)replacement.baselineSignature,
		(unsigned long long)replacement.liveSignature,
		(unsigned long long)replacement.animatedMaterialSignature,
		reasons.c_str(),
		replacement.sectionDirtyCount,
		replacement.sectorDirty ? "yes" : "no",
		replacement.dragged ? "yes" : "no",
		replacement.surfaceCount,
		replacement.triangleCount,
		(uint32_t)replacement.materialBridge.materials.size());

	replacement.lastTraceSignature = replacement.liveSignature;
	replacement.lastTraceAnimatedMaterialSignature = replacement.animatedMaterialSignature;
	replacement.lastTraceReasonMask = replacement.reasonMask;
	replacement.lastTraceActive = replacement.active;
	replacement.lastTraceBlindSpot = replacement.blindSpot;
	replacement.lastTraceAnimationOnlyRefreshed = replacement.animationOnlyRefreshed;
	replacement.lastTraceStaticAnimatedReplacement = replacement.staticAnimatedReplacement;
	replacement.traceCount++;
}

void NRIRuntimeMutationSystem::ClearResidentGeometryUploadRanges()
{
	residentGeometryUploadRanges.clear();
}

bool NRIRuntimeMutationSystem::QueueResidentGeometryUploadRange(
	int uploadKind,
	uint64_t byteOffset,
	uint64_t size,
	const NRIRuntimeMutationResidentUploadServices& services)
{
	if (size == 0)
	{
		return true;
	}

	switch (uploadKind)
	{
	case 0:
	case 1:
	case 2:
		services.NoteUploadRange(uploadKind, size);
		break;
	default:
		return false;
	}

	residentGeometryUploadRanges.push_back({ uploadKind, byteOffset, size, size });
	return true;
}

bool NRIRuntimeMutationSystem::FlushResidentGeometryUploadRanges(const NRIRuntimeMutationResidentUploadServices& services)
{
	if (residentGeometryUploadRanges.empty())
	{
		return true;
	}

	constexpr uint64_t kResidentUploadCoalesceMaxGapBytes = 4ull * 1024ull;
	constexpr uint64_t kResidentUploadCoalesceMaxByteExpansion = 2;
	auto clearAndFail = [&]()
	{
		residentGeometryUploadRanges.clear();
		return false;
	};

	std::sort(
		residentGeometryUploadRanges.begin(),
		residentGeometryUploadRanges.end(),
		[](const RuntimeMutationResidentUploadRange& a, const RuntimeMutationResidentUploadRange& b)
		{
			if (a.uploadKind != b.uploadKind)
			{
				return a.uploadKind < b.uploadKind;
			}
			return a.byteOffset < b.byteOffset;
		});

	std::vector<RuntimeMutationResidentUploadRange> coalescedRanges;
	coalescedRanges.reserve(residentGeometryUploadRanges.size());
	for (const RuntimeMutationResidentUploadRange& range : residentGeometryUploadRanges)
	{
		if (coalescedRanges.empty() ||
			coalescedRanges.back().uploadKind != range.uploadKind)
		{
			coalescedRanges.push_back(range);
			continue;
		}

		RuntimeMutationResidentUploadRange& tail = coalescedRanges.back();
		const uint64_t tailEnd = tail.byteOffset + tail.size;
		const uint64_t rangeEnd = range.byteOffset + range.size;
		const uint64_t gapBytes = range.byteOffset > tailEnd ? range.byteOffset - tailEnd : 0;
		const uint64_t candidateSize = rangeEnd > tailEnd ? rangeEnd - tail.byteOffset : tail.size;
		const uint64_t candidateDirtySize = tail.dirtySize + range.size;
		const bool acceptableByteExpansion =
			candidateDirtySize > UINT64_MAX / kResidentUploadCoalesceMaxByteExpansion ||
			candidateSize <= candidateDirtySize * kResidentUploadCoalesceMaxByteExpansion;
		if (gapBytes <= kResidentUploadCoalesceMaxGapBytes && acceptableByteExpansion)
		{
			if (rangeEnd > tailEnd)
			{
				tail.size = rangeEnd - tail.byteOffset;
			}
			tail.dirtySize += range.size;
			continue;
		}

		services.NoteCoalescedReject();
		coalescedRanges.push_back(range);
	}

	if (!services.StageGeometryRanges(coalescedRanges))
	{
		return clearAndFail();
	}

	for (const RuntimeMutationResidentUploadRange& range : coalescedRanges)
	{
		services.NoteCoalescedRange(range);
	}

	residentGeometryUploadRanges.clear();
	return true;
}

bool NRIRuntimeMutationSystem::TryApplyResidentChunk(
	const NRIRuntimeMutationResidentApplyServices& services,
	const nri_scene::PTMapChunk& mapChunk,
	RuntimeMapMutationCache::ChunkReplacement& replacement,
	RuntimeMutationResidentApplyResult& outResult)
{
	return services.TryApplyChunk(mapChunk, replacement, outResult);
}

bool NRIRuntimeMutationSystem::BuildOverlay(
	const NRIRuntimeMutationOverlayServices& services,
	nri_scene::GeometryData& outGeometry,
	nri_scene::MaterialBridgeData& outMaterials,
	bool* outResidentStaticSceneChanged)
{
	return services.BuildOverlay(outGeometry, outMaterials, outResidentStaticSceneChanged);
}

bool NRIRuntimeMutationSystem::BuildFrameOverlay(
	const NRIRuntimeMutationOverlayServices& services,
	NRIRuntimeMutationFrameOutput& outFrame)
{
	nri_scene::ClearGeometryRetainingCapacity(outFrame.geometry);
	nri_scene::ClearMaterialBridgeRetainingCapacity(outFrame.materialBridge);
	outFrame.hasOverlay = false;
	outFrame.residentStaticSceneChanged = false;
	outFrame.hasOverlay = BuildOverlay(
		services,
		outFrame.geometry,
		outFrame.materialBridge,
		&outFrame.residentStaticSceneChanged);
	return outFrame.hasOverlay;
}

bool NRIRuntimeMutationSystem::CommitResidentSceneRefresh(
	const NRIRuntimeMutationResidentUploadServices& uploadServices,
	const NRIRuntimeMutationResidentSceneRefreshServices& refreshServices,
	const RuntimeMutationResidentSceneRefreshRequest& request,
	RuntimeMutationResidentSceneRefreshResult& outResult)
{
	outResult = {};
	if (!request.sceneChanged)
	{
		return true;
	}

	bool residentRefreshOkay = true;
	const char* residentRefreshFailureReason = nullptr;
	if (!FlushResidentGeometryUploadRanges(uploadServices))
	{
		residentRefreshOkay = false;
		residentRefreshFailureReason = "runtime-mutation-resident-geometry-upload-failed";
	}
	if (residentRefreshOkay && request.materialDirty)
	{
		if (request.materialChunkListIndices == nullptr || request.animatedMaterialChunkListIndices == nullptr)
		{
			residentRefreshOkay = false;
			residentRefreshFailureReason = "runtime-mutation-resident-material-refresh-failed";
		}
		else
		{
			auto& materialChunkListIndices = *request.materialChunkListIndices;
			auto& animatedMaterialChunkListIndices = *request.animatedMaterialChunkListIndices;
			std::sort(materialChunkListIndices.begin(), materialChunkListIndices.end());
			materialChunkListIndices.erase(
				std::unique(materialChunkListIndices.begin(), materialChunkListIndices.end()),
				materialChunkListIndices.end());
			std::sort(animatedMaterialChunkListIndices.begin(), animatedMaterialChunkListIndices.end());
			animatedMaterialChunkListIndices.erase(
				std::unique(animatedMaterialChunkListIndices.begin(), animatedMaterialChunkListIndices.end()),
				animatedMaterialChunkListIndices.end());
			residentRefreshOkay = refreshServices.RefreshMaterialSlices(materialChunkListIndices, animatedMaterialChunkListIndices);
			if (!residentRefreshOkay)
			{
				refreshServices.NoteMaterialFallback((uint32_t)materialChunkListIndices.size());
				residentRefreshOkay = refreshServices.RebuildMaterialState("resident_runtime_mutation_static_recover");
				if (!residentRefreshOkay)
				{
					residentRefreshFailureReason = "runtime-mutation-resident-material-refresh-failed";
				}
			}
		}
	}
	if (residentRefreshOkay && request.geometryDirty)
	{
		if (request.geometryChunkListIndices == nullptr)
		{
			residentRefreshOkay = false;
			residentRefreshFailureReason = "runtime-mutation-resident-blas-refresh-failed";
		}
		else
		{
			auto& geometryChunkListIndices = *request.geometryChunkListIndices;
			std::sort(geometryChunkListIndices.begin(), geometryChunkListIndices.end());
			geometryChunkListIndices.erase(
				std::unique(geometryChunkListIndices.begin(), geometryChunkListIndices.end()),
				geometryChunkListIndices.end());
			residentRefreshOkay = refreshServices.RebuildChunkBlases(geometryChunkListIndices);
			refreshServices.NoteBlasRebuild((uint32_t)geometryChunkListIndices.size());
			if (!residentRefreshOkay)
			{
				residentRefreshFailureReason = "runtime-mutation-resident-blas-refresh-failed";
			}
		}
	}

	if (residentRefreshOkay)
	{
		const std::vector<uint32_t> emptyGeometryList;
		const std::vector<uint32_t>& geometryChunkListIndices =
			request.geometryChunkListIndices != nullptr ? *request.geometryChunkListIndices : emptyGeometryList;
		refreshServices.CommitSuccess(geometryChunkListIndices, request.materialDirty);
		outResult.residentStaticSceneGeometryChanged = request.geometryDirty;
		return true;
	}

	outResult.failureReason =
		residentRefreshFailureReason != nullptr ?
			residentRefreshFailureReason :
			"runtime-mutation-resident-refresh-failed";
	MarkFrameInactive();
	(void)refreshServices.RecoverFailure(outResult.failureReason);
	return false;
}

void NRIRuntimeMutationSystem::NoteResidentAtlasGrow()
{
	lastFrame.residentAtlasGrowCount++;
}

void NRIRuntimeMutationSystem::ResetCacheAndFrame()
{
	discovery.Reset();
	cache.chunks.clear();
	lastFrame = {};
}

void NRIRuntimeMutationSystem::ResetCacheForStaticSceneBuild(uint32_t chunkCount)
{
	discovery.Reset();
	cache.chunks.clear();
	cache.chunks.resize(chunkCount);
}

void NRIRuntimeMutationSystem::InitializeStaticChunkReplacement(const nri_scene::PTMapChunk& chunk)
{
	if (chunk.chunkIndex >= cache.chunks.size())
	{
		return;
	}

	auto& replacement = cache.chunks[chunk.chunkIndex];
	nri_scene::CaptureMapChunkMutationBaseline(chunk, replacement.baseline);
	replacement.replacementBaseline = replacement.baseline;
	replacement.baselineSignature = replacement.baseline.signature;
	replacement.liveSignature = replacement.baselineSignature;
	replacement.animatedMaterialSignature = 0;
	replacement.reasonMask = 0;
	replacement.sectionDirtyCount = 0;
	replacement.stableMutationFrameCount = 0;
	replacement.sectorDirty = false;
	replacement.dragged = false;
	replacement.blindSpot = false;
	replacement.excludeStaticChunk = false;
	replacement.staticAnimatedReplacement = false;
	replacement.lastTraceSignature = UINT64_MAX;
	replacement.lastTraceAnimatedMaterialSignature = UINT64_MAX;
	replacement.lastTraceReasonMask = UINT32_MAX;
	replacement.lastTraceActive = false;
	replacement.lastTraceBlindSpot = false;
	replacement.animationOnlyRefreshed = false;
	replacement.lastTraceAnimationOnlyRefreshed = false;
	replacement.lastTraceStaticAnimatedReplacement = false;
	replacement.traceCount = 0;
	replacement.surfaceCount = 0;
	replacement.triangleCount = 0;
	replacement.residentAuthoritative = true;
	ClearReplacementPayload(replacement, true);
	discovery.NoteTouchedChunk(chunk.chunkIndex);
}

void NRIRuntimeMutationSystem::ResetWorklist()
{
	worklist.Reset();
	discovery.Reset();
	signatureWatchlist.clear();
	signatureWatchlistChunks.clear();
	signatureWatchlistBuildSerial = 0;
	worklistSweepCursor = 0;
}

void NRIRuntimeMutationSystem::BeginWorklistFrame(uint32_t chunkCount)
{
	worklist.BeginFrame(chunkCount);
}

bool NRIRuntimeMutationSystem::MarkWorklistCandidate(uint32_t chunkIndex, uint32_t sourceMask)
{
	return worklist.MarkCandidate(chunkIndex, sourceMask);
}

uint32_t NRIRuntimeMutationSystem::GetWorklistCandidateSourceMask(uint32_t chunkIndex) const
{
	return worklist.GetSourceMask(chunkIndex);
}

const std::vector<uint32_t>& NRIRuntimeMutationSystem::GetWorklistCandidateSourceMasks() const
{
	return worklist.GetSourceMasks();
}

const std::vector<uint32_t>& NRIRuntimeMutationSystem::GetWorklistCandidates() const
{
	return worklist.GetCandidates();
}

void NRIRuntimeMutationSystem::ResetFrameState()
{
	lastFrame = {};
}

void NRIRuntimeMutationSystem::ResetHighWaterStats()
{
	cacheHighWaterStats = {};
}

void NRIRuntimeMutationSystem::ResetLevelLifecycleState()
{
	ResetWorklist();
	ResetFrameState();
	ResetHighWaterStats();
}

void NRIRuntimeMutationSystem::ResetForMapWorldBuildFailure()
{
	ResetWorklist();
}

void NRIRuntimeMutationSystem::ResetLevelHighWaterStats()
{
	ResetHighWaterStats();
}

void NRIRuntimeMutationSystem::BeginFrameState()
{
	ResetFrameState();
}

void NRIRuntimeMutationSystem::PrepareStartupBaseline(uint64_t buildSerial, uint32_t chunkCount)
{
	PrepareSignatureWatchlist(buildSerial, chunkCount);
}

bool NRIRuntimeMutationSystem::CanApplyStartupCorrection(uint32_t chunkCount) const
{
	return HasCacheChunkCount(chunkCount);
}

void NRIRuntimeMutationSystem::PrepareSignatureWatchlist(uint64_t buildSerial, uint32_t chunkCount)
{
	discovery.Reset();
	signatureWatchlistChunks.clear();
	signatureWatchlistChunks.reserve(chunkCount);
	signatureWatchlist.clear();
	signatureWatchlist.resize(chunkCount, 0u);
	signatureWatchlistBuildSerial = buildSerial;
	worklistSweepCursor = 0;
}

bool NRIRuntimeMutationSystem::HasCacheChunkCount(uint32_t chunkCount) const
{
	return cache.chunks.size() == chunkCount;
}

void NRIRuntimeMutationSystem::EnsureSignatureWatchlist(uint64_t buildSerial, uint32_t chunkCount)
{
	if (signatureWatchlistBuildSerial == buildSerial && signatureWatchlist.size() == chunkCount)
	{
		return;
	}

	PrepareSignatureWatchlist(buildSerial, chunkCount);
}

bool NRIRuntimeMutationSystem::SeedSignatureWatchlist(uint32_t chunkIndex)
{
	if (chunkIndex >= signatureWatchlist.size() || signatureWatchlist[chunkIndex] != 0u)
	{
		return false;
	}

	signatureWatchlist[chunkIndex] = 1u;
	signatureWatchlistChunks.push_back(chunkIndex);
	return true;
}

bool NRIRuntimeMutationSystem::IsSignatureWatchlistSeeded(uint32_t chunkIndex) const
{
	return chunkIndex < signatureWatchlist.size() && signatureWatchlist[chunkIndex] != 0u;
}

uint32_t NRIRuntimeMutationSystem::GetSignatureWatchlistSeedCount() const
{
	return (uint32_t)signatureWatchlistChunks.size();
}

uint32_t NRIRuntimeMutationSystem::GetWorklistSweepChunkIndex(uint32_t sweepOffset, uint32_t chunkCount) const
{
	if (chunkCount == 0)
	{
		return 0;
	}
	return (worklistSweepCursor + sweepOffset) % chunkCount;
}

void NRIRuntimeMutationSystem::AdvanceWorklistSweepCursor(uint32_t sweepCount, uint32_t chunkCount)
{
	if (chunkCount == 0)
	{
		worklistSweepCursor = 0;
		return;
	}
	worklistSweepCursor = (worklistSweepCursor + sweepCount) % chunkCount;
}

void NRIRuntimeMutationSystem::FinalizeFrameActive()
{
	lastFrame.active =
		lastFrame.dirtyChunkCount > 0 ||
		lastFrame.residentAppliedChunkCount > 0 ||
		lastFrame.residentFallbackChunkCount > 0;
}

bool NRIRuntimeMutationSystem::HasStartupMaterialOnlyMutation() const
{
	return lastFrame.sectorMaterialChunkCount + lastFrame.wallMaterialChunkCount > 0;
}

uint32_t NRIRuntimeMutationSystem::GetDirtyChunkCount() const
{
	return lastFrame.dirtyChunkCount;
}

uint32_t NRIRuntimeMutationSystem::GetStartupMaterialOnlyDirtyChunkCount() const
{
	return lastFrame.sectorMaterialChunkCount + lastFrame.wallMaterialChunkCount;
}

void NRIRuntimeMutationSystem::MarkFrameInactive()
{
	lastFrame.active = false;
}

void NRIRuntimeMutationSystem::NoteMutationReasonMask(uint32_t reasonMask)
{
	if ((reasonMask & nri_scene::PTMapChunkMutationReason_SectorGeometry) != 0)
	{
		lastFrame.sectorGeometryChunkCount++;
	}
	if ((reasonMask & nri_scene::PTMapChunkMutationReason_SectorMaterial) != 0)
	{
		lastFrame.sectorMaterialChunkCount++;
	}
	if ((reasonMask & nri_scene::PTMapChunkMutationReason_WallGeometry) != 0)
	{
		lastFrame.wallGeometryChunkCount++;
	}
	if ((reasonMask & nri_scene::PTMapChunkMutationReason_WallMaterial) != 0)
	{
		lastFrame.wallMaterialChunkCount++;
	}
	if ((reasonMask & nri_scene::PTMapChunkMutationReason_SectorDirty) != 0)
	{
		lastFrame.sectorDirtyChunkCount++;
	}
	if ((reasonMask & nri_scene::PTMapChunkMutationReason_SectionDirty) != 0)
	{
		lastFrame.sectionDirtyChunkCount++;
	}
	if ((reasonMask & nri_scene::PTMapChunkMutationReason_Dragged) != 0)
	{
		lastFrame.draggedChunkCount++;
	}
}

void NRIRuntimeMutationSystem::NoteDirtyChunk(bool blindSpot)
{
	lastFrame.dirtyChunkCount++;
	if (blindSpot)
	{
		lastFrame.blindSpotChunkCount++;
	}
}

void NRIRuntimeMutationSystem::NoteRebuiltChunk()
{
	lastFrame.rebuiltChunkCount++;
}

void NRIRuntimeMutationSystem::NoteHeldChunk()
{
	lastFrame.heldChunkCount++;
}

void NRIRuntimeMutationSystem::NoteAnimatedRefreshChunk()
{
	lastFrame.animatedRefreshChunkCount++;
}

void NRIRuntimeMutationSystem::NoteResidentApply(bool materialDirty, bool geometryDirty)
{
	lastFrame.residentAppliedChunkCount++;
	if (materialDirty)
	{
		lastFrame.residentMaterialChunkCount++;
	}
	if (geometryDirty)
	{
		lastFrame.residentGeometryChunkCount++;
	}
}

void NRIRuntimeMutationSystem::NoteResidentFallback()
{
	lastFrame.residentFallbackChunkCount++;
}

void NRIRuntimeMutationSystem::UpdateHighWaterStats(const RuntimeMutationCacheStats& cacheStats)
{
	cacheHighWaterStats.activeChunkCount = std::max(cacheHighWaterStats.activeChunkCount, cacheStats.activeChunkCount);
	cacheHighWaterStats.validChunkCount = std::max(cacheHighWaterStats.validChunkCount, cacheStats.validChunkCount);
	cacheHighWaterStats.excludedStaticChunkCount = std::max(cacheHighWaterStats.excludedStaticChunkCount, cacheStats.excludedStaticChunkCount);
	cacheHighWaterStats.cachedSurfaceCount = std::max(cacheHighWaterStats.cachedSurfaceCount, cacheStats.cachedSurfaceCount);
	cacheHighWaterStats.cachedTriangleCount = std::max(cacheHighWaterStats.cachedTriangleCount, cacheStats.cachedTriangleCount);
	cacheHighWaterStats.cachedMaterialCount = std::max(cacheHighWaterStats.cachedMaterialCount, cacheStats.cachedMaterialCount);
	cacheHighWaterStats.cachedMaterialStateCount = std::max(cacheHighWaterStats.cachedMaterialStateCount, cacheStats.cachedMaterialStateCount);
}

RuntimeMutationResidentApplyMode NRIRuntimeMutationSystem::ClassifyResidentApplyMode(
	const RuntimeMapMutationCache::ChunkReplacement& replacement,
	bool hasResidentChunk,
	bool hasResolvedAtlasChunk,
	uint32_t resolvedAtlasMaterialCount) const
{
	RuntimeMutationResidentApplyMode mode = {};
	mode.materialOnlyReplacement = IsRuntimeMutationMaterialOnlyReasonMask(replacement.reasonMask);
	mode.exclusiveMaterialOnlyReplacement =
		mode.materialOnlyReplacement &&
		RequiresExclusiveRuntimeMutationMaterialOnlyReasonMask(replacement.reasonMask);
	mode.certifiedResidentMaterialOnlyUpdate =
		mode.materialOnlyReplacement &&
		replacement.certifiedResidentMaterialOnly;
	mode.fastResidentMaterialOnlyUpdate =
		mode.materialOnlyReplacement &&
		(!mode.exclusiveMaterialOnlyReplacement || mode.certifiedResidentMaterialOnlyUpdate) &&
		hasResidentChunk &&
		replacement.valid &&
		hasResolvedAtlasChunk &&
		(uint32_t)replacement.materialBridge.materials.size() == resolvedAtlasMaterialCount;
	return mode;
}

RuntimeMutationResidentApplyModeStats NRIRuntimeMutationSystem::BuildResidentApplyModeStats(
	const RuntimeMutationResidentApplyMode& mode,
	const RuntimeMapMutationCache::ChunkReplacement& replacement,
	bool hasResidentChunk,
	bool hasResolvedAtlasChunk,
	uint32_t resolvedAtlasMaterialCount) const
{
	RuntimeMutationResidentApplyModeStats stats = {};
	if (mode.materialOnlyReplacement)
	{
		stats.materialOnlyCount++;
		if (mode.fastResidentMaterialOnlyUpdate)
		{
			stats.fastMaterialOnlyCount++;
			if (mode.certifiedResidentMaterialOnlyUpdate)
			{
				stats.certifiedMaterialOnlyCount++;
			}
		}
		else
		{
			stats.slowMaterialOnlyCount++;
			if (mode.exclusiveMaterialOnlyReplacement)
			{
				stats.materialOnlyExclusiveCount++;
			}
			if (!hasResidentChunk)
			{
				stats.materialOnlyNoResidentChunkCount++;
			}
			if (!replacement.valid)
			{
				stats.materialOnlyInvalidReplacementCount++;
			}
			if (hasResolvedAtlasChunk &&
				(uint32_t)replacement.materialBridge.materials.size() != resolvedAtlasMaterialCount)
			{
				stats.materialOnlyMaterialCountMismatchCount++;
			}
		}
	}
	else
	{
		stats.structuralCount++;
	}
	return stats;
}
