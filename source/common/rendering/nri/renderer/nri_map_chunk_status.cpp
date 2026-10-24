#include "nri_debug_reporters.h"

#include "nri_renderer.h"
#include "../scene/nri_map_world.h"
#include "nri_cvars.h"
#include "nri_diagnostic_names.h"
#include "nri_map_chunk_diagnostics.h"
#include "nri_runtime_mutation.h"
#include "nri_runtime_mutation_shared.h"
#include "nri_static_scene.h"
#include "printf.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace
{
	namespace chunk_diag = nri_map_chunk_diag;

	static const char* YesNo(bool value)
	{
		return value ? "yes" : "no";
	}
}
NRIMapChunkDumpSnapshot NRIRenderer::BuildMapChunkDumpSnapshot(int32_t chunkIndex) const
{
	NRIMapChunkDumpSnapshot snapshot = {};
	snapshot.requestedChunkIndex = chunkIndex;
	if (!mMapWorld.valid)
	{
		return snapshot;
	}

	snapshot.mapWorldValid = true;
	snapshot.chunkRange = (uint32_t)mMapWorld.chunks.size();
	if (chunkIndex < 0)
	{
		if (mSurfaceProbe.Last().valid && mSurfaceProbe.Last().hit && mSurfaceProbe.Last().provenance.mapChunkIndex >= 0)
		{
			chunkIndex = mSurfaceProbe.Last().provenance.mapChunkIndex;
			snapshot.usedProbeFallback = true;
		}
		else
		{
			return snapshot;
		}
	}

	snapshot.chunkResolved = true;
	snapshot.chunkIndex = chunkIndex;
	if (chunkIndex < 0 || (unsigned)chunkIndex >= mMapWorld.chunks.size())
	{
		return snapshot;
	}

	snapshot.chunkInRange = true;
	const auto& chunk = mMapWorld.chunks[(unsigned)chunkIndex];
	const NRIStaticSceneResidency::ChunkDiagnosticFacts staticChunkFacts =
		NRIStaticSceneResidency::BuildChunkDiagnosticFacts(mStaticMapScene, mStaticMapChunkAtlas, (uint32_t)chunkIndex);
	const NRIRuntimeMutationSystem::ChunkDiagnosticFacts replacementFacts =
		mRuntimeMutation.BuildChunkDiagnosticFacts((uint32_t)chunkIndex);

	snapshot.sectorIndex = chunk.sectorIndex;
	snapshot.localSpaceIndex = chunk.localSpaceIndex;
	snapshot.surfaceCount = chunk.surfaceCount;
	snapshot.gateVisible = YesNo(nri_runtime_mutation::IsChunkMarkedVisible(mCurrentVisibleChunkWords, (uint32_t)chunkIndex));
	snapshot.residentStatic = YesNo(staticChunkFacts.residentStatic);
	snapshot.staticTlasInstanced = YesNo(staticChunkFacts.staticTlasInstanced);
	snapshot.staticProbeIncluded = YesNo(staticChunkFacts.staticProbeIncluded);
	snapshot.runtimeReplaced = YesNo(replacementFacts.active);
	snapshot.replacementReasons = replacementFacts.reasonSummary;
	snapshot.sectionDirtyCount = replacementFacts.sectionDirtyCount;
	snapshot.sectorDirty = YesNo(replacementFacts.sectorDirty);
	snapshot.dragged = YesNo(replacementFacts.dragged);
	snapshot.blindSpot = YesNo(replacementFacts.blindSpot);
	snapshot.replacementSurfaceCount = replacementFacts.surfaceCount;
	snapshot.replacementTriangleCount = replacementFacts.triangleCount;
	snapshot.duplicateChunkSlotCount = staticChunkFacts.duplicateChunkSlotCount;
	snapshot.preferredChunkListIndex = staticChunkFacts.preferredChunkListIndex;

	if (staticChunkFacts.hasStaticChunk)
	{
		snapshot.hasStaticChunk = true;
		snapshot.staticPrimitiveOffset = staticChunkFacts.staticPrimitiveOffset;
		snapshot.staticPrimitiveCount = staticChunkFacts.staticPrimitiveCount;
		snapshot.staticMaterialOffset = staticChunkFacts.staticMaterialOffset;
		snapshot.staticMaterialCount = staticChunkFacts.staticMaterialCount;
		snapshot.staticAsReady = YesNo(staticChunkFacts.staticAsReady);
	}

	for (uint32_t localSurfaceIndex = 0; localSurfaceIndex < chunk.surfaceCount; ++localSurfaceIndex)
	{
		const uint32_t surfaceIndex = chunk.firstSurface + localSurfaceIndex;
		if (surfaceIndex >= mMapWorld.surfaces.size())
		{
			break;
		}

		const auto& surface = mMapWorld.surfaces[surfaceIndex];
		const uint32_t flags = surface.surface.material.flags;
		snapshot.triangleCount += chunk_diag::CountSurfaceTriangles(surface.surface);
		if ((flags & (nri_scene::MaterialFlag_Portal | nri_scene::MaterialFlag_Mirror)) != 0)
		{
			snapshot.portalSurfaceCount++;
		}
		if ((flags & nri_scene::MaterialFlag_Sky) != 0)
		{
			snapshot.skySurfaceCount++;
		}

		NRIMapChunkSurfaceDumpRow row = {};
		row.surfaceIndex = surfaceIndex;
		row.kindName = nri_diag::GetMapSurfaceKindName(surface.kind);
		row.sourceName = nri_diag::GetSurfaceSourceTypeName(surface.surface.provenance.sourceType);
		row.sectionIndex = surface.surface.provenance.sectionIndex;
		row.sectorIndex = surface.surface.provenance.sectorIndex;
		row.wallIndex = surface.surface.provenance.wallIndex;
		row.nextSectorIndex = surface.surface.provenance.nextSectorIndex;
		row.actorIndex = surface.surface.provenance.actorIndex;
		row.cstat = surface.surface.provenance.cstat;
		row.flags = flags;
		row.flat = YesNo((flags & nri_scene::MaterialFlag_Flat) != 0);
		row.sprite = YesNo((flags & nri_scene::MaterialFlag_Sprite) != 0);
		row.mirror = YesNo((flags & nri_scene::MaterialFlag_Mirror) != 0);
		row.sky = YesNo((flags & nri_scene::MaterialFlag_Sky) != 0);
		row.portal = YesNo((flags & nri_scene::MaterialFlag_Portal) != 0);
		row.oneWay = YesNo((flags & nri_scene::MaterialFlag_OneWay) != 0);
		row.facingBillboard = YesNo((flags & nri_scene::MaterialFlag_FacingBillboard) != 0);
		row.pointSampled = YesNo((flags & nri_scene::MaterialFlag_PointSampled) != 0);
		row.textureId = chunk_diag::GetSurfaceTextureId(surface);
		row.palette = surface.surface.material.palette;
		row.shade = surface.surface.material.shade;
		row.alpha = surface.surface.material.alpha;
		row.vertexCount = (uint32_t)surface.surface.vertices.size();
		row.triangleCount = chunk_diag::CountSurfaceTriangles(surface.surface);
		snapshot.surfaces.push_back(row);
	}

	for (const auto& portal : mMapWorld.portals)
	{
		if (portal.sourceChunkIndex != (uint32_t)chunkIndex)
		{
			continue;
		}

		snapshot.sourcePortalCount++;
		NRIMapChunkPortalDumpRow row = {};
		row.portalIndex = portal.portalIndex;
		row.sourceSurfaceIndex = portal.sourceSurfaceIndex;
		row.sourceSectorIndex = portal.sourceSectorIndex;
		row.sourceWallIndex = portal.sourceWallIndex;
		row.sourcePlane = portal.sourcePlane;
		row.targetCount = portal.targetCount;
		row.runtimeBound = YesNo(portal.runtimeBoundTarget);
		row.delta[0] = (float)portal.delta[0];
		row.delta[1] = (float)portal.delta[1];
		row.delta[2] = (float)portal.delta[2];
		snapshot.portals.push_back(row);
	}

	return snapshot;
}

void PrintNRIMapChunkDumpSnapshot(const NRIMapChunkDumpSnapshot& snapshot)
{
	if (!snapshot.mapWorldValid)
	{
		Printf("NRI PT chunk dump: no authoritative map world has been built yet.\n");
		return;
	}

	if (!snapshot.chunkResolved)
	{
		Printf("NRI PT chunk dump: no chunk was specified and the last surface probe hit did not resolve to a map chunk.\n");
		return;
	}

	if (!snapshot.chunkInRange)
	{
		Printf("NRI PT chunk dump: chunk %d is out of range [0,%u).\n", snapshot.chunkIndex, snapshot.chunkRange);
		return;
	}

	Printf("NRI PT chunk dump: chunk=%d sector=%d local_space=%u surfaces=%u tris=%u portal_surfaces=%u sky_surfaces=%u source_portals=%u gate_visible=%s resident_static=%s static_tlas_instanced=%s static_probe_included=%s runtime_replaced=%s replacement_reasons=%s section_dirty=%u sector_dirty=%s dragged=%s blind_spot=%s replacement_surfaces=%u replacement_tris=%u\n",
		snapshot.chunkIndex,
		snapshot.sectorIndex,
		snapshot.localSpaceIndex,
		snapshot.surfaceCount,
		snapshot.triangleCount,
		snapshot.portalSurfaceCount,
		snapshot.skySurfaceCount,
		snapshot.sourcePortalCount,
		snapshot.gateVisible,
		snapshot.residentStatic,
		snapshot.staticTlasInstanced,
		snapshot.staticProbeIncluded,
		snapshot.runtimeReplaced,
		snapshot.replacementReasons.c_str(),
		snapshot.sectionDirtyCount,
		snapshot.sectorDirty,
		snapshot.dragged,
		snapshot.blindSpot,
		snapshot.replacementSurfaceCount,
		snapshot.replacementTriangleCount);

	if (snapshot.duplicateChunkSlotCount > 1)
	{
		Printf("NRI PT chunk dump slots: chunk=%d duplicate_slots=%u preferred_slot=%u\n",
			snapshot.chunkIndex,
			snapshot.duplicateChunkSlotCount,
			snapshot.preferredChunkListIndex);
	}

	if (snapshot.hasStaticChunk)
	{
		Printf("NRI PT chunk dump static: primitive_offset=%u primitive_count=%u material_offset=%u material_count=%u as_ready=%s\n",
			snapshot.staticPrimitiveOffset,
			snapshot.staticPrimitiveCount,
			snapshot.staticMaterialOffset,
			snapshot.staticMaterialCount,
			snapshot.staticAsReady);
	}

	for (const auto& portal : snapshot.portals)
	{
		Printf("NRI PT chunk portal: portal=%u source_surface=%u source_sector=%d source_wall=%d source_plane=%d target_count=%u runtime_bound=%s delta=(%.2f, %.2f, %.2f)\n",
			portal.portalIndex,
			portal.sourceSurfaceIndex,
			portal.sourceSectorIndex,
			portal.sourceWallIndex,
			portal.sourcePlane,
			portal.targetCount,
			portal.runtimeBound,
			portal.delta[0],
			portal.delta[1],
			portal.delta[2]);
	}

	for (const auto& surface : snapshot.surfaces)
	{
		Printf("NRI PT chunk surface %u: kind=%s source=%s section=%d sector=%d wall=%d nextsector=%d actor=%d cstat=0x%x flags=0x%x flat=%s sprite=%s mirror=%s sky=%s portal=%s one_way=%s facing_billboard=%s point_sampled=%s tile=%u pal=%d shade=%d alpha=%.3f verts=%u tris=%u\n",
			surface.surfaceIndex,
			surface.kindName,
			surface.sourceName,
			surface.sectionIndex,
			surface.sectorIndex,
			surface.wallIndex,
			surface.nextSectorIndex,
			surface.actorIndex,
			surface.cstat,
			surface.flags,
			surface.flat,
			surface.sprite,
			surface.mirror,
			surface.sky,
			surface.portal,
			surface.oneWay,
			surface.facingBillboard,
			surface.pointSampled,
			surface.textureId,
			surface.palette,
			surface.shade,
			surface.alpha,
			surface.vertexCount,
			surface.triangleCount);
	}
}

void NRIRenderer::PrintMapChunkDump(int32_t chunkIndex) const
{
	PrintNRIMapChunkDumpSnapshot(BuildMapChunkDumpSnapshot(chunkIndex));
}
static NRIMapChunkCompareUnmatchedRow BuildMapChunkCompareUnmatchedRow(uint32_t surfaceIndex, const nri_scene::PTMapSurface& surface)
{
	NRIMapChunkCompareUnmatchedRow row = {};
	row.surfaceIndex = surfaceIndex;
	row.kindName = nri_diag::GetMapSurfaceKindName(surface.kind);
	row.sourceName = nri_diag::GetSurfaceSourceTypeName(surface.surface.provenance.sourceType);
	row.sectorIndex = surface.surface.provenance.sectorIndex;
	row.wallIndex = surface.surface.provenance.wallIndex;
	row.sectionIndex = surface.surface.provenance.sectionIndex;
	row.nextSectorIndex = surface.surface.provenance.nextSectorIndex;
	row.cstat = surface.surface.provenance.cstat;
	row.textureId = chunk_diag::GetSurfaceTextureId(surface);
	row.flags = surface.surface.material.flags;
	row.vertexCount = (uint32_t)surface.surface.vertices.size();
	row.triangleCount = chunk_diag::CountSurfaceTriangles(surface.surface);
	return row;
}

NRIMapChunkCompareSnapshot NRIRenderer::BuildMapChunkCompareSnapshot(int32_t chunkIndex) const
{
	NRIMapChunkCompareSnapshot snapshot = {};
	snapshot.requestedChunkIndex = chunkIndex;
	if (!mMapWorld.valid)
	{
		return snapshot;
	}

	snapshot.mapWorldValid = true;
	snapshot.chunkRange = (uint32_t)mMapWorld.chunks.size();
	if (chunkIndex < 0)
	{
		if (mSurfaceProbe.Last().valid && mSurfaceProbe.Last().hit && mSurfaceProbe.Last().provenance.mapChunkIndex >= 0)
		{
			chunkIndex = mSurfaceProbe.Last().provenance.mapChunkIndex;
		}
		else
		{
			return snapshot;
		}
	}

	snapshot.chunkResolved = true;
	snapshot.chunkIndex = chunkIndex;
	if (chunkIndex < 0 || (unsigned)chunkIndex >= mMapWorld.chunks.size())
	{
		return snapshot;
	}

	snapshot.chunkInRange = true;
	const auto& staticChunk = mMapWorld.chunks[(unsigned)chunkIndex];
	nri_scene::PTMapWorld liveWorld = {};
	nri_scene::PTMapWorldStats liveStats = {};
	nri_scene::PTMapBuildOptions mapBuildOptions = {};
	if (!nri_scene::BuildLiveMapChunkWorld(staticChunk, liveWorld, &liveStats, mapBuildOptions) ||
		liveWorld.chunks.empty())
	{
		return snapshot;
	}

	snapshot.liveBuildSucceeded = true;
	const auto& liveChunk = liveWorld.chunks[0];
	const NRIRuntimeMutationSystem::ChunkDiagnosticFacts replacementFacts =
		mRuntimeMutation.BuildChunkDiagnosticFacts((uint32_t)chunkIndex);
	snapshot.sectorIndex = staticChunk.sectorIndex;
	snapshot.replacementReasons = replacementFacts.reasonSummary;
	snapshot.dragged = YesNo(replacementFacts.dragged);
	snapshot.replacementActive = YesNo(replacementFacts.active);
	snapshot.liveTriangleCount = liveChunk.triangleCount;

	std::vector<uint32_t> staticSurfaceIndices;
	std::vector<uint32_t> liveSurfaceIndices;
	staticSurfaceIndices.reserve(staticChunk.surfaceCount);
	liveSurfaceIndices.reserve(liveChunk.surfaceCount);

	for (uint32_t localSurfaceIndex = 0; localSurfaceIndex < staticChunk.surfaceCount; ++localSurfaceIndex)
	{
		const uint32_t surfaceIndex = staticChunk.firstSurface + localSurfaceIndex;
		if (surfaceIndex >= mMapWorld.surfaces.size())
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

	snapshot.staticSurfaceCount = (uint32_t)staticSurfaceIndices.size();
	snapshot.liveSurfaceCount = (uint32_t)liveSurfaceIndices.size();
	std::unordered_map<chunk_diag::SurfaceKey, std::vector<uint32_t>, chunk_diag::SurfaceKeyHash> liveSurfaceLookup;
	liveSurfaceLookup.reserve(liveSurfaceIndices.size());
	for (uint32_t liveLocalIndex = 0; liveLocalIndex < (uint32_t)liveSurfaceIndices.size(); ++liveLocalIndex)
	{
		const auto& liveSurface = liveWorld.surfaces[liveSurfaceIndices[liveLocalIndex]];
		liveSurfaceLookup[chunk_diag::BuildSurfaceKey(liveSurface)].push_back(liveLocalIndex);
	}

	std::vector<uint8_t> liveSurfaceUsed(liveSurfaceIndices.size(), 0u);
	std::vector<chunk_diag::MatchRecord> matches;
	std::vector<uint32_t> unmatchedStaticSurfaceIndices;
	std::vector<uint32_t> unmatchedLiveSurfaceIndices;
	matches.reserve(std::min(staticSurfaceIndices.size(), liveSurfaceIndices.size()));
	unmatchedStaticSurfaceIndices.reserve(staticSurfaceIndices.size());
	unmatchedLiveSurfaceIndices.reserve(liveSurfaceIndices.size());

	for (uint32_t staticSurfaceIndex : staticSurfaceIndices)
	{
		const auto& staticSurface = mMapWorld.surfaces[staticSurfaceIndex];
		const chunk_diag::SurfaceKey key = chunk_diag::BuildSurfaceKey(staticSurface);
		auto it = liveSurfaceLookup.find(key);
		if (it == liveSurfaceLookup.end())
		{
			unmatchedStaticSurfaceIndices.push_back(staticSurfaceIndex);
			continue;
		}

		uint32_t matchedLiveLocalIndex = UINT32_MAX;
		for (uint32_t candidate : it->second)
		{
			if (candidate < liveSurfaceUsed.size() && liveSurfaceUsed[candidate] == 0u)
			{
				matchedLiveLocalIndex = candidate;
				break;
			}
		}
		if (matchedLiveLocalIndex == UINT32_MAX)
		{
			unmatchedStaticSurfaceIndices.push_back(staticSurfaceIndex);
			continue;
		}

		liveSurfaceUsed[matchedLiveLocalIndex] = 1u;
		const uint32_t liveSurfaceIndex = liveSurfaceIndices[matchedLiveLocalIndex];
		const auto& liveSurface = liveWorld.surfaces[liveSurfaceIndex];

		chunk_diag::MatchRecord match = {};
		match.staticSurfaceIndex = staticSurfaceIndex;
		match.liveSurfaceIndex = liveSurfaceIndex;
		match.key = key;
		match.staticMetrics = chunk_diag::ComputeSurfaceMetrics(staticSurface);
		match.liveMetrics = chunk_diag::ComputeSurfaceMetrics(liveSurface);
		for (int axis = 0; axis < 3; ++axis)
		{
			match.delta[axis] = match.liveMetrics.centroid[axis] - match.staticMetrics.centroid[axis];
		}
		match.deltaDistance = chunk_diag::Distance3(match.liveMetrics.centroid, match.staticMetrics.centroid);
		if (match.staticMetrics.area > 0.0001f)
		{
			match.areaRatio = match.liveMetrics.area / match.staticMetrics.area;
		}
		else
		{
			match.areaRatio = match.liveMetrics.area > 0.0001f ? 9999.0f : 1.0f;
		}

		const float staticNormalLength = std::sqrt(chunk_diag::Dot3(match.staticMetrics.normal, match.staticMetrics.normal));
		const float liveNormalLength = std::sqrt(chunk_diag::Dot3(match.liveMetrics.normal, match.liveMetrics.normal));
		if (staticNormalLength > 0.0001f && liveNormalLength > 0.0001f)
		{
			match.normalDot = std::max(-1.0f, std::min(1.0f, chunk_diag::Dot3(match.staticMetrics.normal, match.liveMetrics.normal)));
		}
		else
		{
			match.normalDot = staticNormalLength <= 0.0001f && liveNormalLength <= 0.0001f ? 1.0f : 0.0f;
		}

		match.materialScore =
			(match.staticMetrics.textureId == match.liveMetrics.textureId ? 0.0f : 1.0f) +
			(match.staticMetrics.palette == match.liveMetrics.palette ? 0.0f : 1.0f) +
			(match.staticMetrics.shade == match.liveMetrics.shade ? 0.0f : 1.0f) +
			(match.staticMetrics.materialFlags == match.liveMetrics.materialFlags ? 0.0f : 1.0f) +
			(std::fabs(match.staticMetrics.alpha - match.liveMetrics.alpha) > 0.001f ? 1.0f : 0.0f);
		matches.push_back(match);
	}

	for (uint32_t liveLocalIndex = 0; liveLocalIndex < (uint32_t)liveSurfaceIndices.size(); ++liveLocalIndex)
	{
		if (liveSurfaceUsed[liveLocalIndex] == 0u)
		{
			unmatchedLiveSurfaceIndices.push_back(liveSurfaceIndices[liveLocalIndex]);
		}
	}

	for (const auto& match : matches)
	{
		snapshot.meanDelta[0] += match.delta[0];
		snapshot.meanDelta[1] += match.delta[1];
		snapshot.meanDelta[2] += match.delta[2];
	}
	if (!matches.empty())
	{
		const float invMatchCount = 1.0f / (float)matches.size();
		snapshot.meanDelta[0] *= invMatchCount;
		snapshot.meanDelta[1] *= invMatchCount;
		snapshot.meanDelta[2] *= invMatchCount;
	}

	std::unordered_map<int32_t, uint32_t> sectorChunkLookup;
	sectorChunkLookup.reserve(mMapWorld.chunks.size());
	for (const auto& mapChunk : mMapWorld.chunks)
	{
		if (mapChunk.sectorIndex >= 0)
		{
			sectorChunkLookup.emplace(mapChunk.sectorIndex, mapChunk.chunkIndex);
		}
	}

	for (auto& match : matches)
	{
		const float meanDeltaPoint[3] = { snapshot.meanDelta[0], snapshot.meanDelta[1], snapshot.meanDelta[2] };
		match.deviationFromMean = chunk_diag::Distance3(match.delta, meanDeltaPoint);
		const float areaDelta = std::fabs(match.areaRatio - 1.0f);
		if (match.deviationFromMean <= 1.0f)
		{
			snapshot.within1++;
		}
		if (match.deviationFromMean <= 4.0f)
		{
			snapshot.within4++;
		}
		if (areaDelta > 0.05f)
		{
			snapshot.areaOutlierCount++;
		}
		if (match.normalDot < 0.98f)
		{
			snapshot.normalOutlierCount++;
		}
		if (match.materialScore > 0.0f)
		{
			snapshot.materialDiffCount++;
		}
		match.score = match.deviationFromMean + areaDelta * 10.0f + (1.0f - match.normalDot) * 10.0f + match.materialScore;

		const auto& staticSurface = mMapWorld.surfaces[match.staticSurfaceIndex];
		if (staticSurface.surface.provenance.nextSectorIndex >= 0 &&
			staticSurface.kind != nri_scene::PTMapSurfaceKind::Floor &&
			staticSurface.kind != nri_scene::PTMapSurfaceKind::Ceiling &&
			staticSurface.kind != nri_scene::PTMapSurfaceKind::Portal)
		{
			snapshot.seamSurfaceCount++;
			auto adjacentChunkIt = sectorChunkLookup.find(staticSurface.surface.provenance.nextSectorIndex);
			const bool adjacentReplaced =
				adjacentChunkIt != sectorChunkLookup.end() &&
				mRuntimeMutation.BuildChunkDiagnosticFacts(adjacentChunkIt->second).active;
			if (adjacentReplaced)
			{
				snapshot.seamAgainstReplacedCount++;
			}
			else
			{
				snapshot.seamAgainstStaticCount++;
			}

			if (match.deviationFromMean > 0.5f)
			{
				snapshot.seamOutlierCount++;
			}
		}
	}

	std::sort(matches.begin(), matches.end(), [](const chunk_diag::MatchRecord& a, const chunk_diag::MatchRecord& b)
	{
		return a.score > b.score;
	});

	const bool likelyCoherent =
		!matches.empty() &&
		unmatchedStaticSurfaceIndices.empty() &&
		unmatchedLiveSurfaceIndices.empty() &&
		snapshot.within4 + std::max<uint32_t>(1u, (uint32_t)matches.size() / 10u) >= (uint32_t)matches.size() &&
		snapshot.areaOutlierCount == 0 &&
		snapshot.normalOutlierCount == 0;
	snapshot.likelyCoherent = YesNo(likelyCoherent);
	snapshot.matchedCount = (uint32_t)matches.size();
	snapshot.unmatchedStaticCount = (uint32_t)unmatchedStaticSurfaceIndices.size();
	snapshot.unmatchedLiveCount = (uint32_t)unmatchedLiveSurfaceIndices.size();

	const size_t outlierCount = std::min<size_t>(matches.size(), 8u);
	for (size_t i = 0; i < outlierCount; ++i)
	{
		const auto& match = matches[i];
		if (match.score <= 0.01f && likelyCoherent)
		{
			break;
		}

		const auto& staticSurface = mMapWorld.surfaces[match.staticSurfaceIndex];
		const auto& liveSurface = liveWorld.surfaces[match.liveSurfaceIndex];
		NRIMapChunkCompareMatchRow row = {};
		row.staticSurfaceIndex = match.staticSurfaceIndex;
		row.liveSurfaceIndex = match.liveSurfaceIndex;
		row.kindName = nri_diag::GetMapSurfaceKindName(staticSurface.kind);
		row.sourceName = nri_diag::GetSurfaceSourceTypeName(staticSurface.surface.provenance.sourceType);
		row.sectorIndex = staticSurface.surface.provenance.sectorIndex;
		row.wallIndex = staticSurface.surface.provenance.wallIndex;
		row.sectionIndex = staticSurface.surface.provenance.sectionIndex;
		row.nextSectorIndex = staticSurface.surface.provenance.nextSectorIndex;
		row.cstat = staticSurface.surface.provenance.cstat;
		row.delta[0] = match.delta[0];
		row.delta[1] = match.delta[1];
		row.delta[2] = match.delta[2];
		row.deviationFromMean = match.deviationFromMean;
		row.areaRatio = match.areaRatio;
		row.normalDot = match.normalDot;
		row.staticTextureId = match.staticMetrics.textureId;
		row.liveTextureId = match.liveMetrics.textureId;
		row.staticFlags = staticSurface.surface.material.flags;
		row.liveFlags = liveSurface.surface.material.flags;
		snapshot.matchRows.push_back(row);
	}

	size_t seamPrinted = 0;
	for (const auto& match : matches)
	{
		if (seamPrinted >= 8u)
		{
			break;
		}

		const auto& staticSurface = mMapWorld.surfaces[match.staticSurfaceIndex];
		if (staticSurface.surface.provenance.nextSectorIndex < 0 ||
			staticSurface.kind == nri_scene::PTMapSurfaceKind::Floor ||
			staticSurface.kind == nri_scene::PTMapSurfaceKind::Ceiling ||
			staticSurface.kind == nri_scene::PTMapSurfaceKind::Portal)
		{
			continue;
		}

		auto adjacentChunkIt = sectorChunkLookup.find(staticSurface.surface.provenance.nextSectorIndex);
		const int32_t adjacentChunkIndex = adjacentChunkIt != sectorChunkLookup.end() ? (int32_t)adjacentChunkIt->second : -1;
		const bool adjacentReplaced =
			adjacentChunkIndex >= 0 &&
			mRuntimeMutation.BuildChunkDiagnosticFacts((uint32_t)adjacentChunkIndex).active;
		const bool seamOutlier = match.deviationFromMean > 0.5f;
		if (!seamOutlier && seamPrinted >= 4u)
		{
			continue;
		}

		NRIMapChunkCompareSeamRow row = {};
		row.staticSurfaceIndex = match.staticSurfaceIndex;
		row.liveSurfaceIndex = match.liveSurfaceIndex;
		row.kindName = nri_diag::GetMapSurfaceKindName(staticSurface.kind);
		row.wallIndex = staticSurface.surface.provenance.wallIndex;
		row.nextSectorIndex = staticSurface.surface.provenance.nextSectorIndex;
		row.adjacentChunkIndex = adjacentChunkIndex;
		row.adjacentReplaced = YesNo(adjacentReplaced);
		row.delta[0] = match.delta[0];
		row.delta[1] = match.delta[1];
		row.delta[2] = match.delta[2];
		row.deviationFromMean = match.deviationFromMean;
		row.areaRatio = match.areaRatio;
		row.normalDot = match.normalDot;
		row.seamOutlier = YesNo(seamOutlier);
		snapshot.seamRows.push_back(row);
		seamPrinted++;
	}

	const size_t unmatchedStaticPrintCount = std::min<size_t>(unmatchedStaticSurfaceIndices.size(), 8u);
	for (size_t i = 0; i < unmatchedStaticPrintCount; ++i)
	{
		const auto& surface = mMapWorld.surfaces[unmatchedStaticSurfaceIndices[i]];
		snapshot.unmatchedStaticRows.push_back(BuildMapChunkCompareUnmatchedRow(unmatchedStaticSurfaceIndices[i], surface));
	}

	const size_t unmatchedLivePrintCount = std::min<size_t>(unmatchedLiveSurfaceIndices.size(), 8u);
	for (size_t i = 0; i < unmatchedLivePrintCount; ++i)
	{
		const auto& surface = liveWorld.surfaces[unmatchedLiveSurfaceIndices[i]];
		snapshot.unmatchedLiveRows.push_back(BuildMapChunkCompareUnmatchedRow(unmatchedLiveSurfaceIndices[i], surface));
	}

	return snapshot;
}

void PrintNRIMapChunkCompareSnapshot(const NRIMapChunkCompareSnapshot& snapshot)
{
	if (!snapshot.mapWorldValid)
	{
		Printf("NRI PT chunk compare: no authoritative map world has been built yet.\n");
		return;
	}

	if (!snapshot.chunkResolved)
	{
		Printf("NRI PT chunk compare: no chunk was specified and the last surface probe hit did not resolve to a map chunk.\n");
		return;
	}

	if (!snapshot.chunkInRange)
	{
		Printf("NRI PT chunk compare: chunk %d is out of range [0,%u).\n", snapshot.chunkIndex, snapshot.chunkRange);
		return;
	}

	if (!snapshot.liveBuildSucceeded)
	{
		Printf("NRI PT chunk compare: failed to build live runtime chunk %d.\n", snapshot.chunkIndex);
		return;
	}

	Printf("NRI PT chunk compare: chunk=%d sector=%d static_surfaces=%u live_surfaces=%u matched=%u unmatched_static=%u unmatched_live=%u reasons=%s dragged=%s replacement_active=%s mean_delta=(%.2f, %.2f, %.2f) within_1=%u within_4=%u area_outliers=%u normal_outliers=%u material_diffs=%u likely_coherent=%s live_tris=%u\n",
		snapshot.chunkIndex,
		snapshot.sectorIndex,
		snapshot.staticSurfaceCount,
		snapshot.liveSurfaceCount,
		snapshot.matchedCount,
		snapshot.unmatchedStaticCount,
		snapshot.unmatchedLiveCount,
		snapshot.replacementReasons.c_str(),
		snapshot.dragged,
		snapshot.replacementActive,
		snapshot.meanDelta[0],
		snapshot.meanDelta[1],
		snapshot.meanDelta[2],
		snapshot.within1,
		snapshot.within4,
		snapshot.areaOutlierCount,
		snapshot.normalOutlierCount,
		snapshot.materialDiffCount,
		snapshot.likelyCoherent,
		snapshot.liveTriangleCount);
	Printf("NRI PT chunk seam compare: chunk=%d border_surfaces=%u seam_outliers=%u adjacent_static=%u adjacent_replaced=%u\n",
		snapshot.chunkIndex,
		snapshot.seamSurfaceCount,
		snapshot.seamOutlierCount,
		snapshot.seamAgainstStaticCount,
		snapshot.seamAgainstReplacedCount);

	for (const auto& match : snapshot.matchRows)
	{
		Printf("NRI PT chunk compare match: static_surface=%u live_surface=%u kind=%s source=%s sector=%d wall=%d section=%d nextsector=%d cstat=0x%x delta=(%.2f, %.2f, %.2f) dev=%.2f area_ratio=%.3f normal_dot=%.3f tile_static=%u tile_live=%u flags_static=0x%x flags_live=0x%x\n",
			match.staticSurfaceIndex,
			match.liveSurfaceIndex,
			match.kindName,
			match.sourceName,
			match.sectorIndex,
			match.wallIndex,
			match.sectionIndex,
			match.nextSectorIndex,
			match.cstat,
			match.delta[0],
			match.delta[1],
			match.delta[2],
			match.deviationFromMean,
			match.areaRatio,
			match.normalDot,
			match.staticTextureId,
			match.liveTextureId,
			match.staticFlags,
			match.liveFlags);
	}

	for (const auto& seam : snapshot.seamRows)
	{
		Printf("NRI PT chunk seam match: static_surface=%u live_surface=%u kind=%s wall=%d nextsector=%d adjacent_chunk=%d adjacent_replaced=%s delta=(%.2f, %.2f, %.2f) dev=%.2f area_ratio=%.3f normal_dot=%.3f seam_outlier=%s\n",
			seam.staticSurfaceIndex,
			seam.liveSurfaceIndex,
			seam.kindName,
			seam.wallIndex,
			seam.nextSectorIndex,
			seam.adjacentChunkIndex,
			seam.adjacentReplaced,
			seam.delta[0],
			seam.delta[1],
			seam.delta[2],
			seam.deviationFromMean,
			seam.areaRatio,
			seam.normalDot,
			seam.seamOutlier);
	}

	for (const auto& surface : snapshot.unmatchedStaticRows)
	{
		Printf("NRI PT chunk compare unmatched_static: surface=%u kind=%s source=%s sector=%d wall=%d section=%d nextsector=%d cstat=0x%x tile=%u flags=0x%x verts=%u tris=%u\n",
			surface.surfaceIndex,
			surface.kindName,
			surface.sourceName,
			surface.sectorIndex,
			surface.wallIndex,
			surface.sectionIndex,
			surface.nextSectorIndex,
			surface.cstat,
			surface.textureId,
			surface.flags,
			surface.vertexCount,
			surface.triangleCount);
	}

	for (const auto& surface : snapshot.unmatchedLiveRows)
	{
		Printf("NRI PT chunk compare unmatched_live: surface=%u kind=%s source=%s sector=%d wall=%d section=%d nextsector=%d cstat=0x%x tile=%u flags=0x%x verts=%u tris=%u\n",
			surface.surfaceIndex,
			surface.kindName,
			surface.sourceName,
			surface.sectorIndex,
			surface.wallIndex,
			surface.sectionIndex,
			surface.nextSectorIndex,
			surface.cstat,
			surface.textureId,
			surface.flags,
			surface.vertexCount,
			surface.triangleCount);
	}
}

void NRIRenderer::PrintMapChunkCompare(int32_t chunkIndex) const
{
	PrintNRIMapChunkCompareSnapshot(BuildMapChunkCompareSnapshot(chunkIndex));
}
