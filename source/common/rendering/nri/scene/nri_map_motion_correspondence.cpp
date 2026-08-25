#include "nri_map_motion_correspondence.h"

#include "nri_hash.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
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
	}
	outReason = MotionValidityReason::Valid;
	return true;
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
	InitializeMapTemporalSurface(mapSurface, 9u, mapSurface.surface);
	PTMapTemporalSurfacePayload payload;
	MotionValidityReason reason;
	if (!BuildMapTemporalSurfacePayload(mapSurface.surface, payload, reason)) return fail("payload build failed");
	SurfaceRef moved = mapSurface.surface;
	for (CapturedVertex& vertex : moved.vertices) vertex.position[1] += 4.0f;
	if (!ApplyMapTemporalSurfacePayload(payload, moved, reason)) return fail("stable wall correspondence failed");
	if (moved.vertices[0].prevPosition[1] != mapSurface.surface.vertices[0].position[1]) return fail("previous position mismatch");
	moved.temporal.topologyKey++;
	if (ApplyMapTemporalSurfacePayload(payload, moved, reason) || reason != MotionValidityReason::TopologyMismatch) return fail("topology mismatch accepted");
	if (failureReason != nullptr) failureReason->clear();
	return true;
}
}
