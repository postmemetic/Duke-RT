#include "nri_map_motion_correspondence.h"

#include "nri_hash.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>

namespace nri_scene
{
namespace
{
	uint64_t Mix(uint64_t hash, uint64_t value)
	{
		return HashCombine64(hash, value);
	}

	bool Finite3(const float value[3])
	{
		return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
	}

	bool SamePosition(const float a[3], const float b[3])
	{
		constexpr float epsilon = 1.0e-4f;
		return std::fabs(a[0] - b[0]) <= epsilon &&
			std::fabs(a[1] - b[1]) <= epsilon &&
			std::fabs(a[2] - b[2]) <= epsilon;
	}

	bool ValidMaterialUvAnchor(const float anchor[2], const float texelScale[2])
	{
		return std::isfinite(anchor[0]) && std::isfinite(anchor[1]) &&
			std::isfinite(texelScale[0]) && std::isfinite(texelScale[1]) &&
			texelScale[0] > 0.0f && texelScale[1] > 0.0f;
	}

	float EvaluateMaterialUvReactive(const PTMapTemporalSurfacePayload& previous,
		const TemporalSurfaceMetadata& current)
	{
		if (!previous.materialUvAnchorValid && !current.materialUvAnchorValid) return 0.0f;
		if (!previous.materialUvAnchorValid || !current.materialUvAnchorValid ||
			!ValidMaterialUvAnchor(previous.materialUvAnchor, previous.materialUvTexelScale) ||
			!ValidMaterialUvAnchor(current.materialUvAnchor, current.materialUvTexelScale)) return 1.0f;
		float texelDistanceSquared = 0.0f;
		for (uint32_t axis = 0; axis < 2u; ++axis)
		{
			if (previous.materialUvTexelScale[axis] != current.materialUvTexelScale[axis]) return 1.0f;
			float delta = current.materialUvAnchor[axis] - previous.materialUvAnchor[axis];
			if (!std::isfinite(delta)) return 1.0f;
			// Build pans wrap at 256, which is exactly one normalized UV repeat.
			// Do not turn a small wrap crossing into a full-texture discontinuity.
			delta -= std::round(delta);
			const float texelDelta = delta * current.materialUvTexelScale[axis];
			texelDistanceSquared += texelDelta * texelDelta;
		}
		// One source texel of untracked motion fully rejects material history;
		// subtexel pan reduces confidence continuously and paused pan is zero.
		return std::isfinite(texelDistanceSquared) ?
			std::clamp(std::sqrt(texelDistanceSquared), 0.0f, 1.0f) : 1.0f;
	}

	uint64_t BuildOccurrenceId(const PTMapSurface& surface, uint64_t mapEpoch)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = Mix(hash, mapEpoch);
		hash = Mix(hash, (uint32_t)surface.kind);
		hash = Mix(hash, surface.key.primary);
		hash = Mix(hash, surface.key.secondary);
		hash = Mix(hash, (uint32_t)surface.surface.provenance.sourceType);
		hash = Mix(hash, (uint32_t)surface.surface.provenance.sectorIndex);
		hash = Mix(hash, (uint32_t)surface.surface.provenance.wallIndex);
		hash = Mix(hash, (uint32_t)surface.surface.provenance.sectionIndex);
		return hash != 0 ? hash : 1;
	}

	uint64_t BuildTopologyKey(const SurfaceRef& surface)
	{
		using Triangle = std::array<uint64_t, 3>;
		std::vector<Triangle> triangles;
		const auto appendTriangle = [&](uint32_t i0, uint32_t i1, uint32_t i2)
		{
			if (i0 >= surface.vertices.size() || i1 >= surface.vertices.size() || i2 >= surface.vertices.size())
			{
				return;
			}
			Triangle triangle = {
				surface.vertices[i0].temporalCornerKey,
				surface.vertices[i1].temporalCornerKey,
				surface.vertices[i2].temporalCornerKey
			};
			std::sort(triangle.begin(), triangle.end());
			triangles.push_back(triangle);
		};

		if (!surface.indices.empty())
		{
			for (size_t i = 0; i + 2 < surface.indices.size(); i += 3)
			{
				appendTriangle(surface.indices[i], surface.indices[i + 1], surface.indices[i + 2]);
			}
		}
		else if (surface.provenance.sourceType == SurfaceSourceType::MapWallBand ||
			surface.provenance.sourceType == SurfaceSourceType::MapPortalSurface)
		{
			for (uint32_t i = 1; i + 1 < surface.vertices.size(); ++i)
			{
				appendTriangle(0, i, i + 1);
			}
		}
		else
		{
			for (uint32_t i = 0; i + 2 < surface.vertices.size(); i += 3)
			{
				appendTriangle(i, i + 1, i + 2);
			}
		}

		std::sort(triangles.begin(), triangles.end());
		uint64_t hash = Mix(1469598103934665603ull, triangles.size());
		for (const Triangle& triangle : triangles)
		{
			hash = Mix(hash, triangle[0]);
			hash = Mix(hash, triangle[1]);
			hash = Mix(hash, triangle[2]);
		}
		return hash != 0 ? hash : 1;
	}

	uint32_t FoldGeneration(uint64_t topologyKey)
	{
		uint32_t generation = (uint32_t)(topologyKey ^ (topologyKey >> 32u));
		return generation != 0 ? generation : 1u;
	}
}

void InitializeMapTemporalSurface(const PTMapSurface& mapSurface, uint64_t mapEpoch, SurfaceRef& surface)
{
	surface.temporal.occurrenceId = BuildOccurrenceId(mapSurface, mapEpoch);
	surface.temporal.topologyKey = BuildTopologyKey(surface);
	surface.temporal.generation = FoldGeneration(surface.temporal.topologyKey);
	surface.temporal.historyAge = 0;
	surface.temporal.materialUvReactive = 0.0f;
	surface.temporal.reason = MotionValidityReason::NoHistory;
	surface.temporal.identityValid = true;
	surface.temporal.correspondenceValid = false;
}

bool BuildMapTemporalSurfacePayload(
	const SurfaceRef& surface,
	PTMapTemporalSurfacePayload& outPayload,
	MotionValidityReason& outReason)
{
	outPayload = {};
	outReason = MotionValidityReason::AmbiguousCorrespondence;
	if (!surface.temporal.identityValid || surface.temporal.occurrenceId == 0 || surface.temporal.topologyKey == 0)
	{
		outReason = MotionValidityReason::UnsupportedSource;
		return false;
	}

	std::map<uint64_t, std::array<float, 3>> uniqueCorners;
	for (const CapturedVertex& vertex : surface.vertices)
	{
		if (vertex.temporalCornerKey == UINT64_MAX || !Finite3(vertex.position))
		{
			outReason = MotionValidityReason::CurrentNonFinite;
			return false;
		}
		auto inserted = uniqueCorners.emplace(vertex.temporalCornerKey,
			std::array<float, 3>{ vertex.position[0], vertex.position[1], vertex.position[2] });
		if (!inserted.second && !SamePosition(inserted.first->second.data(), vertex.position))
		{
			return false;
		}
	}
	if (uniqueCorners.empty())
	{
		return false;
	}

	outPayload.occurrenceId = surface.temporal.occurrenceId;
	outPayload.topologyKey = surface.temporal.topologyKey;
	outPayload.generation = surface.temporal.generation;
	outPayload.chunkIndex = surface.provenance.mapChunkIndex >= 0 ?
		(uint32_t)surface.provenance.mapChunkIndex : UINT32_MAX;
	if (surface.temporal.materialVerticalReferenceValid &&
		!std::isfinite(surface.temporal.materialVerticalReference))
	{
		outReason = MotionValidityReason::CurrentNonFinite;
		return false;
	}
	outPayload.materialVerticalReference = surface.temporal.materialVerticalReference;
	outPayload.materialVerticalReferenceValid = surface.temporal.materialVerticalReferenceValid;
	if (surface.temporal.materialUvAnchorValid)
	{
		if (!ValidMaterialUvAnchor(surface.temporal.materialUvAnchor, surface.temporal.materialUvTexelScale))
		{
			outReason = MotionValidityReason::CurrentNonFinite;
			return false;
		}
		std::memcpy(outPayload.materialUvAnchor, surface.temporal.materialUvAnchor, sizeof(outPayload.materialUvAnchor));
		std::memcpy(outPayload.materialUvTexelScale, surface.temporal.materialUvTexelScale, sizeof(outPayload.materialUvTexelScale));
		outPayload.materialUvAnchorValid = true;
	}
	outPayload.provenance = surface.provenance;
	outPayload.corners.reserve(uniqueCorners.size());
	for (const auto& entry : uniqueCorners)
	{
		PTMapTemporalCorner corner;
		corner.key = entry.first;
		std::memcpy(corner.position, entry.second.data(), sizeof(corner.position));
		outPayload.corners.push_back(corner);
	}
	outReason = MotionValidityReason::Valid;
	return true;
}

bool ApplyMapTemporalSurfacePayload(
	const PTMapTemporalSurfacePayload& previous,
	SurfaceRef& current,
	MotionValidityReason& outReason)
{
	outReason = MotionValidityReason::NoHistory;
	current.temporal.materialUvReactive = 0.0f;
	if (previous.occurrenceId != current.temporal.occurrenceId)
	{
		return false;
	}
	if (previous.topologyKey != current.temporal.topologyKey ||
		previous.generation != current.temporal.generation)
	{
		outReason = MotionValidityReason::TopologyMismatch;
		return false;
	}
	if (previous.corners.empty())
	{
		return false;
	}
	if (previous.materialVerticalReferenceValid != current.temporal.materialVerticalReferenceValid)
	{
		outReason = MotionValidityReason::TopologyMismatch;
		return false;
	}
	float materialVerticalDelta = 0.0f;
	if (previous.materialVerticalReferenceValid)
	{
		if (!std::isfinite(previous.materialVerticalReference))
		{
			outReason = MotionValidityReason::PreviousNonFinite;
			return false;
		}
		if (!std::isfinite(current.temporal.materialVerticalReference))
		{
			outReason = MotionValidityReason::CurrentNonFinite;
			return false;
		}
		materialVerticalDelta =
			current.temporal.materialVerticalReference - previous.materialVerticalReference;
	}

	for (CapturedVertex& vertex : current.vertices)
	{
		const auto found = std::lower_bound(
			previous.corners.begin(), previous.corners.end(), vertex.temporalCornerKey,
			[](const PTMapTemporalCorner& corner, uint64_t key) { return corner.key < key; });
		if (found == previous.corners.end() || found->key != vertex.temporalCornerKey)
		{
			outReason = MotionValidityReason::TopologyMismatch;
			return false;
		}
		std::memcpy(vertex.prevPosition, found->position, sizeof(vertex.prevPosition));
		if (previous.materialVerticalReferenceValid)
		{
			// At a fixed wall UV, referenceHeight + renderer-space Y is
			// invariant. Reconstruct the previous material point from that
			// invariant instead of following a top/bottom clip corner.
			vertex.prevPosition[1] = vertex.position[1] + materialVerticalDelta;
			if (!std::isfinite(vertex.prevPosition[1]))
			{
				outReason = MotionValidityReason::PreviousNonFinite;
				return false;
			}
		}
	}
	current.temporal.materialUvReactive = EvaluateMaterialUvReactive(previous, current.temporal);
	outReason = MotionValidityReason::Valid;
	return true;
}

bool RequiresMapMaterialUvUpload(const SceneView& resident, const SceneView& current)
{
	for (size_t index = 0; index < current.opaqueFlats.size(); ++index)
	{
		const auto& next = current.opaqueFlats[index].temporal;
		if (index >= resident.opaqueFlats.size()) return true;
		const auto& prior = resident.opaqueFlats[index].temporal;
		if (!next.materialUvAnchorValid && !prior.materialUvAnchorValid &&
			next.materialUvReactive == 0.0f && prior.materialUvReactive == 0.0f) continue;
		if (next.occurrenceId != prior.occurrenceId ||
			next.materialUvAnchorValid != prior.materialUvAnchorValid ||
			next.materialUvReactive != prior.materialUvReactive) return true;
		if (next.materialUvAnchorValid &&
			(std::memcmp(next.materialUvAnchor, prior.materialUvAnchor, sizeof(next.materialUvAnchor)) != 0 ||
				std::memcmp(next.materialUvTexelScale, prior.materialUvTexelScale, sizeof(next.materialUvTexelScale)) != 0)) return true;
	}
	return false;
}

bool RunNRIMapMotionCorrespondenceSelfTests(std::string* failureReason)
{
	auto fail = [&](const char* reason)
	{
		if (failureReason != nullptr) *failureReason = reason;
		return false;
	};
	PTMapSurface mapSurface;
	mapSurface.kind = PTMapSurfaceKind::WallMiddle;
	mapSurface.key = { 7u, 3u };
	mapSurface.chunkIndex = 2u;
	mapSurface.surface.provenance.sourceType = SurfaceSourceType::MapWallBand;
	mapSurface.surface.provenance.wallIndex = 7;
	mapSurface.surface.provenance.mapChunkIndex = 2;
	for (uint64_t i = 0; i < 4; ++i)
	{
		CapturedVertex vertex;
		vertex.position[0] = (float)(i & 1u);
		vertex.position[1] = (float)(i >> 1u);
		vertex.temporalCornerKey = i;
		mapSurface.surface.vertices.push_back(vertex);
	}
	mapSurface.surface.indices = { 0u, 1u, 2u, 0u, 2u, 3u };
	InitializeMapTemporalSurface(mapSurface, 9u, mapSurface.surface);
	PTMapTemporalSurfacePayload payload;
	MotionValidityReason reason;
	if (!BuildMapTemporalSurfacePayload(mapSurface.surface, payload, reason)) return fail("payload build failed");
	SurfaceRef moved = mapSurface.surface;
	for (CapturedVertex& vertex : moved.vertices) vertex.position[1] += 4.0f;
	if (!ApplyMapTemporalSurfacePayload(payload, moved, reason)) return fail("stable wall correspondence failed");
	if (moved.vertices[0].prevPosition[1] != mapSurface.surface.vertices[0].position[1]) return fail("previous position mismatch");
	SurfaceRef reordered = moved;
	const std::vector<CapturedVertex> oldVertices = reordered.vertices;
	const uint32_t permutation[4] = { 2u, 0u, 3u, 1u };
	for (uint32_t i = 0; i < 4u; ++i) reordered.vertices[i] = oldVertices[permutation[i]];
	const uint32_t inverse[4] = { 1u, 3u, 0u, 2u };
	for (uint32_t& index : reordered.indices) index = inverse[index];
	InitializeMapTemporalSurface(mapSurface, 9u, reordered);
	if (reordered.temporal.topologyKey != payload.topologyKey ||
		!ApplyMapTemporalSurfacePayload(payload, reordered, reason))
		return fail("certified vertex reordering failed");
	if (reordered.vertices[0].prevPosition[0] != mapSurface.surface.vertices[2].position[0])
		return fail("semantic corner mapping followed emitted order");
	SurfaceRef duplicate = mapSurface.surface;
	duplicate.vertices[1].temporalCornerKey = duplicate.vertices[0].temporalCornerKey;
	duplicate.vertices[1].position[0] += 8.0f;
	InitializeMapTemporalSurface(mapSurface, 9u, duplicate);
	PTMapTemporalSurfacePayload duplicatePayload;
	if (BuildMapTemporalSurfacePayload(duplicate, duplicatePayload, reason))
		return fail("ambiguous duplicate corner accepted");

	SurfaceRef clippedWall = mapSurface.surface;
	clippedWall.temporal.materialVerticalReferenceValid = true;
	clippedWall.temporal.materialVerticalReference = 10.0f;
	clippedWall.vertices[0].position[1] = -10.0f;
	clippedWall.vertices[1].position[1] = -2.0f;
	clippedWall.vertices[2].position[1] = -2.0f;
	clippedWall.vertices[3].position[1] = -10.0f;
	PTMapTemporalSurfacePayload clippedPayload;
	if (!BuildMapTemporalSurfacePayload(clippedWall, clippedPayload, reason))
		return fail("wall material-anchor payload build failed");
	SurfaceRef clippedCurrent = clippedWall;
	clippedCurrent.vertices[0].position[1] = -6.0f;
	clippedCurrent.vertices[3].position[1] = -6.0f;
	if (!ApplyMapTemporalSurfacePayload(clippedPayload, clippedCurrent, reason))
		return fail("wall clip-boundary correspondence failed");
	for (const CapturedVertex& vertex : clippedCurrent.vertices)
		if (vertex.prevPosition[1] != vertex.position[1])
			return fail("fixed wall material inherited clip-boundary motion");

	SurfaceRef liftedCurrent = clippedWall;
	liftedCurrent.temporal.materialVerticalReference = 14.0f;
	for (CapturedVertex& vertex : liftedCurrent.vertices) vertex.position[1] -= 4.0f;
	if (!ApplyMapTemporalSurfacePayload(clippedPayload, liftedCurrent, reason))
		return fail("moving wall material-anchor correspondence failed");
	for (const CapturedVertex& vertex : liftedCurrent.vertices)
		if (vertex.prevPosition[1] != vertex.position[1] + 4.0f)
			return fail("moving wall material did not receive uniform vertical motion");

	SurfaceRef pannedFlat = mapSurface.surface;
	pannedFlat.provenance.sourceType = SurfaceSourceType::MapFloorSection;
	pannedFlat.temporal.materialUvAnchorValid = true;
	pannedFlat.temporal.materialUvTexelScale[0] = 64.0f;
	pannedFlat.temporal.materialUvTexelScale[1] = 32.0f;
	pannedFlat.temporal.materialUvAnchor[0] = 255.0f / 256.0f;
	PTMapTemporalSurfacePayload panPayload;
	if (!BuildMapTemporalSurfacePayload(pannedFlat, panPayload, reason)) return fail("flat pan payload build failed");
	pannedFlat.temporal.materialUvAnchor[0] = 1.0f / 256.0f;
	if (!ApplyMapTemporalSurfacePayload(panPayload, pannedFlat, reason) ||
		std::fabs(pannedFlat.temporal.materialUvReactive - 0.5f) > 1.0e-6f)
		return fail("flat pan wrap did not retain subtexel distance");
	for (const CapturedVertex& vertex : pannedFlat.vertices)
		if (std::memcmp(vertex.position, vertex.prevPosition, sizeof(vertex.position)) != 0)
			return fail("flat texture pan changed geometric motion");
	pannedFlat.temporal.materialUvAnchor[0] = 0.5f;
	if (!ApplyMapTemporalSurfacePayload(panPayload, pannedFlat, reason) || pannedFlat.temporal.materialUvReactive != 1.0f)
		return fail("fast flat texture pan did not saturate");
	pannedFlat.temporal.materialUvAnchor[0] = panPayload.materialUvAnchor[0] - 1.0f / 256.0f;
	pannedFlat.temporal.materialUvAnchor[1] = 1.0f / 128.0f;
	if (!ApplyMapTemporalSurfacePayload(panPayload, pannedFlat, reason) ||
		std::fabs(pannedFlat.temporal.materialUvReactive - std::sqrt(0.125f)) > 1.0e-6f)
		return fail("two-axis negative pan used wrong texture scale");
	pannedFlat.temporal.materialUvAnchor[0] = panPayload.materialUvAnchor[0];
	pannedFlat.temporal.materialUvAnchor[1] = panPayload.materialUvAnchor[1];
	if (!ApplyMapTemporalSurfacePayload(panPayload, pannedFlat, reason) || pannedFlat.temporal.materialUvReactive != 0.0f)
		return fail("paused flat retained material reactivity");
	pannedFlat.temporal.materialUvTexelScale[0] = 128.0f;
	if (!ApplyMapTemporalSurfacePayload(panPayload, pannedFlat, reason) || pannedFlat.temporal.materialUvReactive != 1.0f)
		return fail("changed texture dimensions reused material history");
	pannedFlat.temporal.materialUvTexelScale[0] = 64.0f;
	pannedFlat.temporal.materialUvAnchorValid = false;
	if (!ApplyMapTemporalSurfacePayload(panPayload, pannedFlat, reason) || pannedFlat.temporal.materialUvReactive != 1.0f)
		return fail("missing flat material anchor reused history");
	pannedFlat.temporal.materialUvAnchorValid = true;
	pannedFlat.temporal.materialUvAnchor[0] = std::numeric_limits<float>::quiet_NaN();
	PTMapTemporalSurfacePayload invalidPanPayload;
	if (BuildMapTemporalSurfacePayload(pannedFlat, invalidPanPayload, reason) || reason != MotionValidityReason::CurrentNonFinite)
		return fail("nonfinite flat material anchor accepted");
	if (!ApplyMapTemporalSurfacePayload(panPayload, pannedFlat, reason) || pannedFlat.temporal.materialUvReactive != 1.0f)
		return fail("invalid pan contaminated geometric correspondence");

	SceneView residentPanView;
	residentPanView.opaqueFlats.push_back(mapSurface.surface);
	residentPanView.opaqueFlats[0].temporal.materialUvAnchorValid = true;
	residentPanView.opaqueFlats[0].temporal.materialUvTexelScale[0] = 64.0f;
	residentPanView.opaqueFlats[0].temporal.materialUvTexelScale[1] = 32.0f;
	SceneView nextPanView = residentPanView;
	if (RequiresMapMaterialUvUpload(residentPanView, nextPanView)) return fail("unchanged flat requested UV upload");
	nextPanView.opaqueFlats[0].temporal.materialUvAnchor[0] = 1.0f / 256.0f;
	if (!RequiresMapMaterialUvUpload(residentPanView, nextPanView)) return fail("flat pan entered material-only preservation");
	nextPanView = residentPanView;
	residentPanView.opaqueFlats[0].temporal.materialUvReactive = 1.0f;
	if (!RequiresMapMaterialUvUpload(residentPanView, nextPanView)) return fail("flat settle entered material-only preservation");
	nextPanView = residentPanView;
	nextPanView.opaqueFlats[0].temporal.occurrenceId++;
	if (!RequiresMapMaterialUvUpload(residentPanView, nextPanView)) return fail("unmatched pan anchor entered material-only preservation");

	moved.temporal.topologyKey++;
	if (ApplyMapTemporalSurfacePayload(payload, moved, reason) || reason != MotionValidityReason::TopologyMismatch) return fail("topology mismatch accepted");
	if (failureReason != nullptr) failureReason->clear();
	return true;
}
}
