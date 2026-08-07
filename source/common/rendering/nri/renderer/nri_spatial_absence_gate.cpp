#include "nri_spatial_absence_gate.h"

#include "build.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace
{
	constexpr float kBoundsEpsilon = 1.0e-3f;
	constexpr float kAreaEpsilon = 1.0e-3f;
	constexpr float kFootprintPredicateEpsilon = 1.0e-4f;
	constexpr uint32_t kFootprintGridMaxDimension = 32u;
	constexpr uint32_t kFootprintGridMaxCells =
		kFootprintGridMaxDimension * kFootprintGridMaxDimension;
	constexpr uint32_t kMaxFloatExactInteger = 1u << 24u;
	constexpr uint64_t kHashOffset = 1469598103934665603ull;
	constexpr uint64_t kHashPrime = 1099511628211ull;

	struct PairClassification
	{
		NRISpatialAbsenceConflictDecision decision = NRISpatialAbsenceConflictDecision::Disjoint;
		float overlapMin[3] = {};
		float overlapMax[3] = {};
		float distanceToCenter = 0.0f;
	};

	struct Triangle2D
	{
		float point[3][2] = {};
	};

	struct FootprintGrid
	{
		float boundsMin[2] = {};
		float boundsMax[2] = {};
		float cellSize[2] = {};
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t referenceCount = 0;
		uint32_t referenceRecordCount = 0;
		std::vector<std::vector<uint32_t>> cells;
	};

	struct CertifiedPair
	{
		NRISpatialAbsenceContinuityKey continuityKey;
		uint32_t positiveSector = UINT32_MAX;
		uint32_t negativeSector = UINT32_MAX;
		float overlapMin[3] = {};
		float overlapMax[3] = {};
		float distanceToCenter = 0.0f;
		uint32_t exactWitnessCount = 0;
		bool authorized = false;
	};

	bool IsFinite3(const float value[3])
	{
		return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
	}

	bool ContainsSorted(const std::vector<uint32_t>& values, int32_t value)
	{
		return value >= 0 && std::binary_search(values.begin(), values.end(), (uint32_t)value);
	}

	bool AreLinkedAdjacent(int32_t firstSector, int32_t secondSector)
	{
		if (firstSector < 0 || secondSector < 0 ||
			(unsigned)firstSector >= sector.Size() || (unsigned)secondSector >= sector.Size())
		{
			return true;
		}

		for (const walltype& wall : sector[(unsigned)firstSector].walls)
		{
			if (wall.nextsector == secondSector)
			{
				return true;
			}
		}
		return false;
	}

	bool ArePortalRelated(const nri_scene::PTMapWorld& mapWorld, uint32_t firstChunk, uint32_t secondChunk)
	{
		for (const nri_scene::PTMapPortal& portal : mapWorld.portals)
		{
			if (portal.sourceChunkIndex != firstChunk && portal.sourceChunkIndex != secondChunk)
			{
				continue;
			}
			const uint32_t otherChunk = portal.sourceChunkIndex == firstChunk ? secondChunk : firstChunk;
			const uint64_t targetEnd = std::min<uint64_t>(
				(uint64_t)portal.firstTarget + portal.targetCount,
				mapWorld.portalTargets.size());
			for (uint64_t targetIndex = portal.firstTarget; targetIndex < targetEnd; ++targetIndex)
			{
				if (mapWorld.portalTargets[(size_t)targetIndex].chunkIndex == otherChunk)
				{
					return true;
				}
			}
		}
		return false;
	}

	float DistanceToBounds(const float point[3], const float minimum[3], const float maximum[3])
	{
		float distanceSquared = 0.0f;
		for (uint32_t axis = 0; axis < 3; ++axis)
		{
			const float delta = point[axis] < minimum[axis] ? minimum[axis] - point[axis] :
				(point[axis] > maximum[axis] ? point[axis] - maximum[axis] : 0.0f);
			distanceSquared += delta * delta;
		}
		return std::sqrt(distanceSquared);
	}

	float Cross2(const float a[2], const float b[2], const float c[2])
	{
		return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
	}

	float PolygonSignedArea(const std::vector<std::array<float, 2>>& polygon)
	{
		float twiceArea = 0.0f;
		for (size_t index = 0; index < polygon.size(); ++index)
		{
			const std::array<float, 2>& first = polygon[index];
			const std::array<float, 2>& second = polygon[(index + 1u) % polygon.size()];
			twiceArea += first[0] * second[1] - first[1] * second[0];
		}
		return twiceArea * 0.5f;
	}

	std::array<float, 2> IntersectLines(
		const std::array<float, 2>& start,
		const std::array<float, 2>& end,
		const float clipStart[2],
		const float clipEnd[2])
	{
		const float segmentX = end[0] - start[0];
		const float segmentY = end[1] - start[1];
		const float clipX = clipEnd[0] - clipStart[0];
		const float clipY = clipEnd[1] - clipStart[1];
		const float denominator = segmentX * clipY - segmentY * clipX;
		if (std::fabs(denominator) <= std::numeric_limits<float>::epsilon())
		{
			return end;
		}
		const float offsetX = clipStart[0] - start[0];
		const float offsetY = clipStart[1] - start[1];
		const float t = (offsetX * clipY - offsetY * clipX) / denominator;
		return { start[0] + segmentX * t, start[1] + segmentY * t };
	}

	float TriangleIntersectionArea(const Triangle2D& first, const Triangle2D& second)
	{
		std::vector<std::array<float, 2>> polygon = {
			{ first.point[0][0], first.point[0][1] },
			{ first.point[1][0], first.point[1][1] },
			{ first.point[2][0], first.point[2][1] }
		};
		const float secondArea = Cross2(second.point[0], second.point[1], second.point[2]);
		if (std::fabs(secondArea) <= kAreaEpsilon)
		{
			return 0.0f;
		}
		const float orientation = secondArea >= 0.0f ? 1.0f : -1.0f;

		for (uint32_t edge = 0; edge < 3u && !polygon.empty(); ++edge)
		{
			const float* clipStart = second.point[edge];
			const float* clipEnd = second.point[(edge + 1u) % 3u];
			std::vector<std::array<float, 2>> input = std::move(polygon);
			polygon.clear();
			std::array<float, 2> previous = input.back();
			bool previousInside = orientation * Cross2(clipStart, clipEnd, previous.data()) >= -kBoundsEpsilon;
			for (const std::array<float, 2>& current : input)
			{
				const bool currentInside = orientation * Cross2(clipStart, clipEnd, current.data()) >= -kBoundsEpsilon;
				if (currentInside != previousInside)
				{
					polygon.push_back(IntersectLines(previous, current, clipStart, clipEnd));
				}
				if (currentInside)
				{
					polygon.push_back(current);
				}
				previous = current;
				previousInside = currentInside;
			}
		}
		return polygon.size() >= 3u ? std::fabs(PolygonSignedArea(polygon)) : 0.0f;
	}

	void AppendSurfaceTriangles(const nri_scene::SurfaceRef& surface, std::vector<Triangle2D>& triangles)
	{
		auto append = [&](uint32_t first, uint32_t second, uint32_t third)
		{
			if (first >= surface.vertices.size() || second >= surface.vertices.size() || third >= surface.vertices.size())
			{
				return;
			}
			Triangle2D triangle;
			const uint32_t indices[3] = { first, second, third };
			for (uint32_t corner = 0; corner < 3u; ++corner)
			{
				triangle.point[corner][0] = surface.vertices[indices[corner]].position[0];
				// NRI world space is [map X, vertical -BuildZ, -map Y].
				triangle.point[corner][1] = surface.vertices[indices[corner]].position[2];
			}
			if (std::fabs(Cross2(triangle.point[0], triangle.point[1], triangle.point[2])) > kAreaEpsilon)
			{
				triangles.push_back(triangle);
			}
		};

		if (!surface.indices.empty())
		{
			for (size_t index = 0; index + 2u < surface.indices.size(); index += 3u)
			{
				append(surface.indices[index], surface.indices[index + 1u], surface.indices[index + 2u]);
			}
		}
		else
		{
			for (uint32_t index = 1u; index + 1u < surface.vertices.size(); ++index)
			{
				append(0u, index, index + 1u);
			}
		}
	}

	std::vector<Triangle2D> CollectChunkFootprintTriangles(
		const nri_scene::PTMapWorld& mapWorld,
		const nri_scene::PTMapChunk& chunk)
	{
		std::vector<Triangle2D> triangles;
		const uint64_t surfaceEnd = std::min<uint64_t>(
			(uint64_t)chunk.firstSurface + chunk.surfaceCount,
			mapWorld.surfaces.size());
		for (uint64_t surfaceIndex = chunk.firstSurface; surfaceIndex < surfaceEnd; ++surfaceIndex)
		{
			const nri_scene::PTMapSurface& surface = mapWorld.surfaces[(size_t)surfaceIndex];
			if (surface.kind == nri_scene::PTMapSurfaceKind::Floor)
			{
				AppendSurfaceTriangles(surface.surface, triangles);
			}
		}
		return triangles;
	}

	bool HasFlatClosedVolume(const nri_scene::PTMapWorld& mapWorld, const nri_scene::PTMapChunk& chunk)
	{
		bool sawFloor = false;
		bool sawCeiling = false;
		const uint64_t surfaceEnd = std::min<uint64_t>(
			(uint64_t)chunk.firstSurface + chunk.surfaceCount,
			mapWorld.surfaces.size());
		for (uint64_t surfaceIndex = chunk.firstSurface; surfaceIndex < surfaceEnd; ++surfaceIndex)
		{
			const nri_scene::PTMapSurface& surface = mapWorld.surfaces[(size_t)surfaceIndex];
			if (surface.kind != nri_scene::PTMapSurfaceKind::Floor &&
				surface.kind != nri_scene::PTMapSurfaceKind::Ceiling)
			{
				continue;
			}
			if (surface.surface.vertices.empty())
			{
				return false;
			}
			const float planeY = surface.surface.vertices.front().position[1];
			for (const nri_scene::CapturedVertex& vertex : surface.surface.vertices)
			{
				if (!std::isfinite(vertex.position[1]) || std::fabs(vertex.position[1] - planeY) > kBoundsEpsilon)
				{
					return false;
				}
			}
			sawFloor |= surface.kind == nri_scene::PTMapSurfaceKind::Floor;
			sawCeiling |= surface.kind == nri_scene::PTMapSurfaceKind::Ceiling;
		}
		return sawFloor && sawCeiling;
	}

	uint32_t CountExactOverlapWitnesses(
		const nri_scene::PTMapWorld& mapWorld,
		const nri_scene::PTMapChunk& positive,
		const nri_scene::PTMapChunk& negative)
	{
		// Chunk-wide vertical bounds are exact only for flat floor/ceiling
		// volumes. Slopes require a per-XY plane interval and remain fail-open.
		if (!HasFlatClosedVolume(mapWorld, positive) || !HasFlatClosedVolume(mapWorld, negative))
		{
			return 0;
		}
		const float minZ = std::max(positive.bounds.min[1], negative.bounds.min[1]);
		const float maxZ = std::min(positive.bounds.max[1], negative.bounds.max[1]);
		if (maxZ - minZ <= kBoundsEpsilon)
		{
			return 0;
		}

		const std::vector<Triangle2D> positiveTriangles = CollectChunkFootprintTriangles(mapWorld, positive);
		const std::vector<Triangle2D> negativeTriangles = CollectChunkFootprintTriangles(mapWorld, negative);
		uint32_t witnessCount = 0;
		for (const Triangle2D& first : positiveTriangles)
		{
			for (const Triangle2D& second : negativeTriangles)
			{
				witnessCount += TriangleIntersectionArea(first, second) > kAreaEpsilon ? 1u : 0u;
			}
		}
		return witnessCount;
	}

	PairClassification ClassifyBounds(
		const nri_scene::PTMapChunkBounds& first,
		const nri_scene::PTMapChunkBounds& second,
		const float center[3],
		float guardRadius)
	{
		PairClassification result;
		bool boundaryContact = false;
		for (uint32_t axis = 0; axis < 3; ++axis)
		{
			result.overlapMin[axis] = std::max(first.min[axis], second.min[axis]);
			result.overlapMax[axis] = std::min(first.max[axis], second.max[axis]);
			const float extent = result.overlapMax[axis] - result.overlapMin[axis];
			if (extent < -kBoundsEpsilon)
			{
				result.decision = NRISpatialAbsenceConflictDecision::Disjoint;
				return result;
			}
			boundaryContact |= extent <= kBoundsEpsilon;
		}

		if (boundaryContact)
		{
			result.decision = NRISpatialAbsenceConflictDecision::BoundaryContact;
			return result;
		}

		result.distanceToCenter = DistanceToBounds(center, result.overlapMin, result.overlapMax);
		if (result.distanceToCenter > guardRadius)
		{
			result.decision = NRISpatialAbsenceConflictDecision::OutsideGuard;
			return result;
		}

		result.decision = NRISpatialAbsenceConflictDecision::Certified;
		return result;
	}

	void MarkChunk(std::vector<uint32_t>& words, uint32_t chunkIndex)
	{
		const uint32_t wordIndex = chunkIndex >> 5u;
		if (wordIndex < words.size())
		{
			words[wordIndex] |= 1u << (chunkIndex & 31u);
		}
	}

	uint64_t HashBytes(uint64_t hash, const void* data, size_t size)
	{
		const uint8_t* bytes = static_cast<const uint8_t*>(data);
		for (size_t i = 0; i < size; ++i)
		{
			hash = (hash ^ bytes[i]) * kHashPrime;
		}
		return hash;
	}

	template <typename T>
	uint64_t HashValue(uint64_t hash, const T& value)
	{
		return HashBytes(hash, &value, sizeof(value));
	}

	uint64_t HashPositiveOwners(std::vector<uint32_t> owners)
	{
		std::sort(owners.begin(), owners.end());
		owners.erase(std::unique(owners.begin(), owners.end()), owners.end());
		return owners.empty() ? 0 : HashBytes(kHashOffset, owners.data(), owners.size() * sizeof(owners[0]));
	}

	uint64_t HashSnapshotSelections(const NRISpatialAbsenceSnapshot& snapshot)
	{
		uint64_t hash = kHashOffset;
		for (const NRISpatialAbsenceSelectionRecord& selection : snapshot.selections)
		{
			hash = HashValue(hash, selection.negativeChunk);
			hash = HashValue(hash, selection.firstPositiveChunk);
			hash = HashValue(hash, selection.selectedWitnessCount);
			hash = HashValue(hash, selection.selectedPositiveOwnerCount);
			hash = HashValue(hash, selection.selectedPositiveOwnerHash);
			hash = HashValue(hash, selection.footprintTriangleCount);
			hash = HashValue(hash, selection.selectionHash);
			hash = HashBytes(hash, selection.boundsMin, sizeof(selection.boundsMin));
			hash = HashBytes(hash, selection.boundsMax, sizeof(selection.boundsMax));
		}
		return snapshot.selections.empty() ? 0 : hash;
	}

	uint64_t HashSnapshotSemantics(const NRISpatialAbsenceSnapshot& snapshot)
	{
		uint64_t hash = kHashOffset;
		hash = HashValue(hash, snapshot.valid);
		hash = HashValue(hash, snapshot.failOpenFlags);
		hash = HashValue(hash, snapshot.worldGeneration);
		hash = HashValue(hash, snapshot.candidateCount);
		hash = HashValue(hash, snapshot.certifiedCount);
		hash = HashValue(hash, snapshot.sourceWitnessCount);
		hash = HashValue(hash, snapshot.selectedWitnessCount);
		hash = HashValue(hash, snapshot.authorizedPairCount);
		hash = HashValue(hash, snapshot.pendingPairCount);
		hash = HashValue(hash, snapshot.footprintTriangleCount);
		hash = HashValue(hash, snapshot.footprintGridCellCount);
		hash = HashValue(hash, snapshot.footprintGridReferenceCount);
		hash = HashValue(hash, snapshot.rootLocalSpaceIndex);
		for (const NRISpatialAbsenceConflictRecord& conflict : snapshot.conflicts)
		{
			hash = HashValue(hash, conflict.decision);
			hash = HashValue(hash, conflict.positiveChunk);
			hash = HashValue(hash, conflict.negativeChunk);
			hash = HashValue(hash, conflict.positiveSector);
			hash = HashValue(hash, conflict.negativeSector);
			hash = HashBytes(hash, conflict.overlapMin, sizeof(conflict.overlapMin));
			hash = HashBytes(hash, conflict.overlapMax, sizeof(conflict.overlapMax));
			hash = HashValue(hash, conflict.exactWitnessCount);
		}
		for (const NRISpatialAbsenceSelectionRecord& selection : snapshot.selections)
		{
			hash = HashValue(hash, selection.negativeChunk);
			hash = HashValue(hash, selection.sourceWitnessCount);
			hash = HashValue(hash, selection.selectedWitnessCount);
			hash = HashValue(hash, selection.sourcePositiveOwnerCount);
			hash = HashValue(hash, selection.sourcePositiveOwnerHash);
			hash = HashValue(hash, selection.footprintTriangleCount);
		}
		return hash;
	}

	NRISpatialAbsenceConflictRecord MakeDebugRecord(
		const nri_scene::PTMapChunk& positive,
		const nri_scene::PTMapChunk& negative,
		const PairClassification& classification)
	{
		NRISpatialAbsenceConflictRecord record;
		record.decision = classification.decision;
		record.positiveChunk = positive.chunkIndex;
		record.negativeChunk = negative.chunkIndex;
		record.positiveSector = positive.sectorIndex;
		record.negativeSector = negative.sectorIndex;
		std::memcpy(record.overlapMin, classification.overlapMin, sizeof(record.overlapMin));
		std::memcpy(record.overlapMax, classification.overlapMax, sizeof(record.overlapMax));
		record.distanceToGuardCenter = classification.distanceToCenter;
		return record;
	}

	bool ResolveRootLocalSpace(
		const nri_scene::PTMapWorld& mapWorld,
		const NRISpatialAbsenceCensusInput& input,
		int32_t& outLocalSpace)
	{
		outLocalSpace = -1;
		if (input.authoritativeRootSector < 0 || input.rootSectorIndices.empty() ||
			std::find(input.rootSectorIndices.begin(), input.rootSectorIndices.end(),
				(uint32_t)input.authoritativeRootSector) == input.rootSectorIndices.end())
		{
			return false;
		}

		for (uint32_t rootSector : input.rootSectorIndices)
		{
			int32_t sectorLocalSpace = -1;
			for (const nri_scene::PTMapChunk& chunk : mapWorld.chunks)
			{
				if (chunk.sectorIndex != (int32_t)rootSector)
					continue;
				if (chunk.localSpaceIndex == UINT32_MAX ||
					chunk.localSpaceIndex > (uint32_t)std::numeric_limits<int32_t>::max())
					return false;
				if (sectorLocalSpace < 0)
					sectorLocalSpace = (int32_t)chunk.localSpaceIndex;
				else if (sectorLocalSpace != (int32_t)chunk.localSpaceIndex)
					return false;
			}
			if (sectorLocalSpace < 0)
				return false;
			if (outLocalSpace < 0)
				outLocalSpace = sectorLocalSpace;
			else if (outLocalSpace != sectorLocalSpace)
				return false;
		}
		return outLocalSpace >= 0;
	}

	std::vector<NRISpatialAbsenceContinuityRecord> AdvanceConflictContinuity(
		const std::vector<NRISpatialAbsenceContinuityKey>& currentKeys,
		const std::vector<NRISpatialAbsenceContinuityRecord>& previous,
		bool contextContinuous)
	{
		std::vector<NRISpatialAbsenceContinuityRecord> result;
		result.reserve(currentKeys.size());
		size_t previousIndex = 0;
		for (const NRISpatialAbsenceContinuityKey& key : currentKeys)
		{
			while (previousIndex < previous.size() && previous[previousIndex].key < key)
				++previousIndex;
			uint32_t count = 1u;
			if (contextContinuous && previousIndex < previous.size() && previous[previousIndex].key == key)
				count = std::min(previous[previousIndex].consecutiveCaptureCount + 1u, 0xffffu);
			NRISpatialAbsenceContinuityRecord record;
			record.key = key;
			record.consecutiveCaptureCount = count;
			record.authorized = count >= 2u;
			result.push_back(record);
		}
		return result;
	}

	bool PointInTriangleForTest(const float point[2], const Triangle2D& triangle)
	{
		const float area = Cross2(triangle.point[0], triangle.point[1], triangle.point[2]);
		if (std::fabs(area) <= kAreaEpsilon) return false;
		const float orientation = area >= 0.0f ? 1.0f : -1.0f;
		return orientation * Cross2(triangle.point[0], triangle.point[1], point) >= -kFootprintPredicateEpsilon &&
			orientation * Cross2(triangle.point[1], triangle.point[2], point) >= -kFootprintPredicateEpsilon &&
			orientation * Cross2(triangle.point[2], triangle.point[0], point) >= -kFootprintPredicateEpsilon;
	}

	bool PointInFootprintForTest(const float point[2], const std::vector<Triangle2D>& triangles)
	{
		for (const Triangle2D& triangle : triangles)
			if (PointInTriangleForTest(point, triangle)) return true;
		return false;
	}

	uint32_t FootprintGridCoordinate(float value, float minimum, float cellSize, uint32_t dimension)
	{
		const int32_t coordinate = (int32_t)std::floor((value - minimum) / cellSize);
		return (uint32_t)std::clamp(coordinate, 0, (int32_t)dimension - 1);
	}

	void FootprintGridTriangleRange(
		const FootprintGrid& grid,
		const Triangle2D& triangle,
		uint32_t minimum[2],
		uint32_t maximum[2])
	{
		float triangleMin[2] = { triangle.point[0][0], triangle.point[0][1] };
		float triangleMax[2] = { triangleMin[0], triangleMin[1] };
		for (uint32_t corner = 1; corner < 3u; ++corner)
		{
			for (uint32_t axis = 0; axis < 2u; ++axis)
			{
				triangleMin[axis] = std::min(triangleMin[axis], triangle.point[corner][axis]);
				triangleMax[axis] = std::max(triangleMax[axis], triangle.point[corner][axis]);
			}
		}
		minimum[0] = FootprintGridCoordinate(
			triangleMin[0] - kFootprintPredicateEpsilon, grid.boundsMin[0], grid.cellSize[0], grid.width);
		minimum[1] = FootprintGridCoordinate(
			triangleMin[1] - kFootprintPredicateEpsilon, grid.boundsMin[1], grid.cellSize[1], grid.height);
		maximum[0] = FootprintGridCoordinate(
			triangleMax[0] + kFootprintPredicateEpsilon, grid.boundsMin[0], grid.cellSize[0], grid.width);
		maximum[1] = FootprintGridCoordinate(
			triangleMax[1] + kFootprintPredicateEpsilon, grid.boundsMin[1], grid.cellSize[1], grid.height);
	}

	bool FootprintTriangleOverlapsGridCell(
		const FootprintGrid& grid,
		const Triangle2D& triangle,
		uint32_t cellX,
		uint32_t cellY)
	{
		const float cellMin[2] = {
			grid.boundsMin[0] + (float)cellX * grid.cellSize[0],
			grid.boundsMin[1] + (float)cellY * grid.cellSize[1]
		};
		const float cellMax[2] = {
			cellX + 1u == grid.width ? grid.boundsMax[0] : cellMin[0] + grid.cellSize[0],
			cellY + 1u == grid.height ? grid.boundsMax[1] : cellMin[1] + grid.cellSize[1]
		};
		float triangleMin[2] = { triangle.point[0][0], triangle.point[0][1] };
		float triangleMax[2] = { triangleMin[0], triangleMin[1] };
		for (uint32_t corner = 1; corner < 3u; ++corner)
		{
			for (uint32_t axis = 0; axis < 2u; ++axis)
			{
				triangleMin[axis] = std::min(triangleMin[axis], triangle.point[corner][axis]);
				triangleMax[axis] = std::max(triangleMax[axis], triangle.point[corner][axis]);
			}
		}
		for (uint32_t axis = 0; axis < 2u; ++axis)
			if (triangleMax[axis] < cellMin[axis] - kBoundsEpsilon ||
				triangleMin[axis] > cellMax[axis] + kBoundsEpsilon) return false;

		const float cellCenter[2] = {
			0.5f * (cellMin[0] + cellMax[0]), 0.5f * (cellMin[1] + cellMax[1])
		};
		const float cellHalfExtent[2] = {
			0.5f * (cellMax[0] - cellMin[0]), 0.5f * (cellMax[1] - cellMin[1])
		};
		for (uint32_t edge = 0; edge < 3u; ++edge)
		{
			const uint32_t next = (edge + 1u) % 3u;
			const float axis[2] = {
				-(triangle.point[next][1] - triangle.point[edge][1]),
				triangle.point[next][0] - triangle.point[edge][0]
			};
			float triangleProjectionMin = axis[0] * (triangle.point[0][0] - cellCenter[0]) +
				axis[1] * (triangle.point[0][1] - cellCenter[1]);
			float triangleProjectionMax = triangleProjectionMin;
			for (uint32_t corner = 1; corner < 3u; ++corner)
			{
				const float projection = axis[0] * (triangle.point[corner][0] - cellCenter[0]) +
					axis[1] * (triangle.point[corner][1] - cellCenter[1]);
				triangleProjectionMin = std::min(triangleProjectionMin, projection);
				triangleProjectionMax = std::max(triangleProjectionMax, projection);
			}
			const float cellProjectionRadius = std::fabs(axis[0]) * cellHalfExtent[0] +
				std::fabs(axis[1]) * cellHalfExtent[1];
			if (triangleProjectionMax < -cellProjectionRadius - kFootprintPredicateEpsilon ||
				triangleProjectionMin > cellProjectionRadius + kFootprintPredicateEpsilon)
				return false;
		}
		return true;
	}

	bool BuildFootprintGrid(const std::vector<Triangle2D>& triangles, FootprintGrid& grid)
	{
		grid = {};
		if (triangles.empty()) return false;
		grid.boundsMin[0] = grid.boundsMin[1] = std::numeric_limits<float>::max();
		grid.boundsMax[0] = grid.boundsMax[1] = -std::numeric_limits<float>::max();
		for (const Triangle2D& triangle : triangles)
		{
			for (uint32_t corner = 0; corner < 3u; ++corner)
			{
				for (uint32_t axis = 0; axis < 2u; ++axis)
				{
					const float value = triangle.point[corner][axis];
					if (!std::isfinite(value)) return false;
					grid.boundsMin[axis] = std::min(grid.boundsMin[axis], value);
					grid.boundsMax[axis] = std::max(grid.boundsMax[axis], value);
				}
			}
		}
		const float extentX = grid.boundsMax[0] - grid.boundsMin[0];
		const float extentY = grid.boundsMax[1] - grid.boundsMin[1];
		if (!(extentX > 0.0f) || !(extentY > 0.0f) ||
			!std::isfinite(extentX) || !std::isfinite(extentY)) return false;
		const uint64_t desired64 = std::min<uint64_t>(
			std::max<uint64_t>((uint64_t)triangles.size() * 4u, 1u), kFootprintGridMaxCells);
		const float aspect = std::clamp(extentX / extentY,
			1.0f / (float)kFootprintGridMaxDimension, (float)kFootprintGridMaxDimension);
		grid.width = std::clamp(
			(uint32_t)std::ceil(std::sqrt((float)desired64 * aspect)), 1u, kFootprintGridMaxDimension);
		grid.height = std::clamp(
			(uint32_t)std::ceil((float)desired64 / (float)grid.width), 1u, kFootprintGridMaxDimension);
		grid.cellSize[0] = extentX / (float)grid.width;
		grid.cellSize[1] = extentY / (float)grid.height;
		if (!(grid.cellSize[0] > 0.0f) || !(grid.cellSize[1] > 0.0f) ||
			!std::isfinite(grid.cellSize[0]) || !std::isfinite(grid.cellSize[1]))
			return false;
		grid.cells.resize((size_t)grid.width * grid.height);
		uint64_t referenceCount = 0;
		for (uint32_t triangleIndex = 0; triangleIndex < triangles.size(); ++triangleIndex)
		{
			uint32_t minimum[2] = {};
			uint32_t maximum[2] = {};
			FootprintGridTriangleRange(grid, triangles[triangleIndex], minimum, maximum);
			for (uint32_t y = minimum[1]; y <= maximum[1]; ++y)
			{
				for (uint32_t x = minimum[0]; x <= maximum[0]; ++x)
				{
					if (!FootprintTriangleOverlapsGridCell(grid, triangles[triangleIndex], x, y)) continue;
					grid.cells[(size_t)y * grid.width + x].push_back(triangleIndex);
					if (++referenceCount >= kMaxFloatExactInteger) return false;
				}
			}
		}
		grid.referenceCount = (uint32_t)referenceCount;
		uint64_t referenceRecordCount = 0;
		for (const std::vector<uint32_t>& references : grid.cells)
			referenceRecordCount += (references.size() + 2u) / 3u;
		if (referenceRecordCount >= kMaxFloatExactInteger) return false;
		grid.referenceRecordCount = (uint32_t)referenceRecordCount;
		return true;
	}

	bool ValidateFootprintGrid(const std::vector<Triangle2D>& triangles, const FootprintGrid& grid)
	{
		if (triangles.empty() || grid.width == 0u || grid.height == 0u ||
			grid.width > kFootprintGridMaxDimension || grid.height > kFootprintGridMaxDimension ||
			grid.cells.size() != (size_t)grid.width * grid.height ||
			!std::isfinite(grid.boundsMin[0]) || !std::isfinite(grid.boundsMin[1]) ||
			!std::isfinite(grid.boundsMax[0]) || !std::isfinite(grid.boundsMax[1]) ||
			!std::isfinite(grid.cellSize[0]) || !std::isfinite(grid.cellSize[1]) ||
			!(grid.boundsMax[0] > grid.boundsMin[0]) || !(grid.boundsMax[1] > grid.boundsMin[1]) ||
			!(grid.cellSize[0] > 0.0f) || !(grid.cellSize[1] > 0.0f))
			return false;
		uint64_t referenceCount = 0;
		for (uint32_t cellIndex = 0; cellIndex < grid.cells.size(); ++cellIndex)
		{
			const std::vector<uint32_t>& references = grid.cells[cellIndex];
			if (!std::is_sorted(references.begin(), references.end()) ||
				std::adjacent_find(references.begin(), references.end()) != references.end())
				return false;
			for (uint32_t triangleIndex : references)
			{
				if (triangleIndex >= triangles.size()) return false;
				uint32_t minimum[2] = {};
				uint32_t maximum[2] = {};
				FootprintGridTriangleRange(grid, triangles[triangleIndex], minimum, maximum);
				const uint32_t x = cellIndex % grid.width;
				const uint32_t y = cellIndex / grid.width;
				if (x < minimum[0] || x > maximum[0] || y < minimum[1] || y > maximum[1] ||
					!FootprintTriangleOverlapsGridCell(grid, triangles[triangleIndex], x, y)) return false;
			}
			referenceCount += references.size();
		}
		uint64_t referenceRecordCount = 0;
		for (const std::vector<uint32_t>& references : grid.cells)
			referenceRecordCount += (references.size() + 2u) / 3u;
		if (referenceCount != grid.referenceCount || referenceRecordCount != grid.referenceRecordCount) return false;
		for (uint32_t triangleIndex = 0; triangleIndex < triangles.size(); ++triangleIndex)
		{
			uint32_t minimum[2] = {};
			uint32_t maximum[2] = {};
			FootprintGridTriangleRange(grid, triangles[triangleIndex], minimum, maximum);
			for (uint32_t y = minimum[1]; y <= maximum[1]; ++y)
			{
				for (uint32_t x = minimum[0]; x <= maximum[0]; ++x)
				{
					if (!FootprintTriangleOverlapsGridCell(grid, triangles[triangleIndex], x, y)) continue;
					const std::vector<uint32_t>& references = grid.cells[(size_t)y * grid.width + x];
					if (!std::binary_search(references.begin(), references.end(), triangleIndex)) return false;
				}
			}
		}
		return true;
	}

	bool PointInFootprintGridForTest(
		const float point[2],
		const std::vector<Triangle2D>& triangles,
		const FootprintGrid& grid,
		bool& valid)
	{
		valid = ValidateFootprintGrid(triangles, grid);
		if (!valid) return false;
		if (point[0] < grid.boundsMin[0] - kFootprintPredicateEpsilon ||
			point[0] > grid.boundsMax[0] + kFootprintPredicateEpsilon ||
			point[1] < grid.boundsMin[1] - kFootprintPredicateEpsilon ||
			point[1] > grid.boundsMax[1] + kFootprintPredicateEpsilon)
			return false;
		const uint32_t x = FootprintGridCoordinate(point[0], grid.boundsMin[0], grid.cellSize[0], grid.width);
		const uint32_t y = FootprintGridCoordinate(point[1], grid.boundsMin[1], grid.cellSize[1], grid.height);
		for (uint32_t triangleIndex : grid.cells[(size_t)y * grid.width + x])
			if (PointInTriangleForTest(point, triangles[triangleIndex])) return true;
		return false;
	}
}

bool NRISpatialAbsenceContinuityKey::operator<(const NRISpatialAbsenceContinuityKey& other) const
{
	if (worldGeneration != other.worldGeneration) return worldGeneration < other.worldGeneration;
	if (localSpaceIndex != other.localSpaceIndex) return localSpaceIndex < other.localSpaceIndex;
	if (negativeChunk != other.negativeChunk) return negativeChunk < other.negativeChunk;
	return positiveChunk < other.positiveChunk;
}

bool NRISpatialAbsenceContinuityKey::operator==(const NRISpatialAbsenceContinuityKey& other) const
{
	return worldGeneration == other.worldGeneration &&
		positiveChunk == other.positiveChunk && negativeChunk == other.negativeChunk &&
		localSpaceIndex == other.localSpaceIndex;
}

void NRISpatialAbsenceGate::Reset(uint32_t frameIndex, bool resetStability)
{
	mSnapshot = {};
	mSnapshot.frameIndex = frameIndex;
	mSnapshot.gpuRecords.resize(1);
	if (resetStability)
	{
		mStableWorldGeneration = 0;
		mStableObservationHash = 0;
		mStableCaptureCount = 0;
		mHasPreviousCensusObservationHash = false;
		mPreviousCensusObservationHash = 0;
		mHasPreviousAuthority = false;
		mPreviousAuthority = false;
		mPreviousContinuityCaptureSerial = 0;
		mContinuityWorldGeneration = 0;
		mContinuityRootLocalSpaceIndex = -1;
		mConflictContinuity.clear();
	}
}

const NRISpatialAbsenceSnapshot& NRISpatialAbsenceGate::Build(
	const nri_scene::PTMapWorld& mapWorld,
	const NRISpatialAbsenceCensusInput& input)
{
	Reset(input.frameIndex, false);
	auto finalizeTelemetry = [&]() -> const NRISpatialAbsenceSnapshot&
	{
		mSnapshot.stableCaptureCount = mStableCaptureCount;
		mSnapshot.previousAuthority = mHasPreviousAuthority && mPreviousAuthority;
		const bool authority = mSnapshot.HasNegativeAuthority();
		mSnapshot.authorityTransition = mHasPreviousAuthority && authority != mPreviousAuthority;
		mSnapshot.selectionHash = HashSnapshotSelections(mSnapshot);
		mSnapshot.semanticHash = HashSnapshotSemantics(mSnapshot);
		mHasPreviousCensusObservationHash = true;
		mPreviousCensusObservationHash = mSnapshot.censusObservationHash;
		mHasPreviousAuthority = true;
		mPreviousAuthority = authority;
		return mSnapshot;
	};
	mSnapshot.captureSerial = input.captureSerial;
	mSnapshot.worldGeneration = input.worldGeneration;
	mSnapshot.censusObservationHash = input.observationHash;
	mSnapshot.authoritativeRootSector = input.authoritativeRootSector;
	mSnapshot.rootSectorIndices = input.rootSectorIndices;
	std::sort(mSnapshot.rootSectorIndices.begin(), mSnapshot.rootSectorIndices.end());
	mSnapshot.rootSectorIndices.erase(
		std::unique(mSnapshot.rootSectorIndices.begin(), mSnapshot.rootSectorIndices.end()),
		mSnapshot.rootSectorIndices.end());
	mSnapshot.previousCensusObservationHash = mHasPreviousCensusObservationHash
		? mPreviousCensusObservationHash
		: 0;
	mSnapshot.guardRadius = input.guardRadius;
	std::memcpy(mSnapshot.center, input.center, sizeof(mSnapshot.center));
	if (input.worldGeneration == mStableWorldGeneration && input.observationHash != 0 &&
		input.observationHash == mStableObservationHash)
	{
		mStableCaptureCount = std::min(mStableCaptureCount + 1u, 0xffffu);
	}
	else
	{
		mStableWorldGeneration = input.worldGeneration;
		mStableObservationHash = input.observationHash;
		mStableCaptureCount = input.observationHash != 0 ? 1u : 0u;
	}

	if (!mapWorld.valid || mapWorld.chunks.empty())
	{
		mSnapshot.failOpenFlags |= NRI_SPATIAL_ABSENCE_FAIL_INVALID_WORLD;
		mStableWorldGeneration = 0;
		mStableObservationHash = 0;
		mStableCaptureCount = 0;
		mConflictContinuity.clear();
		mPreviousContinuityCaptureSerial = 0;
		return finalizeTelemetry();
	}
	if (!input.complete)
	{
		mSnapshot.failOpenFlags |= NRI_SPATIAL_ABSENCE_FAIL_INCOMPLETE_CENSUS;
	}
	int32_t rootLocalSpace = -1;
	if (!input.rootStable || !ResolveRootLocalSpace(mapWorld, input, rootLocalSpace))
	{
		mSnapshot.failOpenFlags |= NRI_SPATIAL_ABSENCE_FAIL_UNSTABLE_ROOT;
	}
	mSnapshot.rootLocalSpaceIndex = rootLocalSpace;
	if (!input.generationMatches)
	{
		mSnapshot.failOpenFlags |= NRI_SPATIAL_ABSENCE_FAIL_GENERATION_MISMATCH;
	}
	if (!IsFinite3(input.center) || !std::isfinite(input.guardRadius) || input.guardRadius <= 0.0f)
	{
		mSnapshot.failOpenFlags |= NRI_SPATIAL_ABSENCE_FAIL_INVALID_GUARD;
	}
	if (mSnapshot.failOpenFlags != NRI_SPATIAL_ABSENCE_FAIL_NONE)
	{
		mStableWorldGeneration = 0;
		mStableObservationHash = 0;
		mStableCaptureCount = 0;
		mConflictContinuity.clear();
		mPreviousContinuityCaptureSerial = 0;
		mContinuityWorldGeneration = 0;
		mContinuityRootLocalSpaceIndex = -1;
		return finalizeTelemetry();
	}

	std::vector<uint32_t> reached = input.reachedSectorIndices;
	std::vector<uint32_t> uncertain = input.uncertainSectorIndices;
	std::vector<uint32_t> uncertainChunks = input.uncertainChunkIndices;
	std::sort(reached.begin(), reached.end());
	reached.erase(std::unique(reached.begin(), reached.end()), reached.end());
	std::sort(uncertain.begin(), uncertain.end());
	uncertain.erase(std::unique(uncertain.begin(), uncertain.end()), uncertain.end());
	std::sort(uncertainChunks.begin(), uncertainChunks.end());
	uncertainChunks.erase(std::unique(uncertainChunks.begin(), uncertainChunks.end()), uncertainChunks.end());

	std::vector<CertifiedPair> certifiedPairs;

	for (size_t firstIndex = 0; firstIndex < mapWorld.chunks.size(); ++firstIndex)
	{
		const nri_scene::PTMapChunk& first = mapWorld.chunks[firstIndex];
		if (!first.bounds.valid || !IsFinite3(first.bounds.min) || !IsFinite3(first.bounds.max))
		{
			continue;
		}

		for (size_t secondIndex = firstIndex + 1u; secondIndex < mapWorld.chunks.size(); ++secondIndex)
		{
			const nri_scene::PTMapChunk& second = mapWorld.chunks[secondIndex];
			if (!second.bounds.valid || !IsFinite3(second.bounds.min) || !IsFinite3(second.bounds.max))
			{
				continue;
			}

			PairClassification classification = ClassifyBounds(first.bounds, second.bounds, input.center, input.guardRadius);
			if (classification.decision == NRISpatialAbsenceConflictDecision::Disjoint)
			{
				continue;
			}
			mSnapshot.candidateCount++;

			const bool firstVisible = ContainsSorted(reached, first.sectorIndex);
			const bool secondVisible = ContainsSorted(reached, second.sectorIndex);
			const bool firstUnknown = ContainsSorted(uncertain, first.sectorIndex);
			const bool secondUnknown = ContainsSorted(uncertain, second.sectorIndex);
			const bool firstRuntimeUnknown = std::binary_search(uncertainChunks.begin(), uncertainChunks.end(), first.chunkIndex);
			const bool secondRuntimeUnknown = std::binary_search(uncertainChunks.begin(), uncertainChunks.end(), second.chunkIndex);
			const nri_scene::PTMapChunk* positive = &first;
			const nri_scene::PTMapChunk* negative = &second;

			if (firstUnknown || secondUnknown || first.sectorIndex < 0 || second.sectorIndex < 0)
			{
				classification.decision = NRISpatialAbsenceConflictDecision::UnknownSector;
			}
			else if (firstVisible == secondVisible)
			{
				classification.decision = NRISpatialAbsenceConflictDecision::SameVisibility;
			}
			else
			{
				if (!firstVisible)
				{
					positive = &second;
					negative = &first;
				}
				if (firstRuntimeUnknown || secondRuntimeUnknown)
				{
					classification.decision = NRISpatialAbsenceConflictDecision::RuntimeAuthorityUnknown;
				}
				else if (first.localSpaceIndex != second.localSpaceIndex)
				{
					classification.decision = NRISpatialAbsenceConflictDecision::DifferentLocalSpace;
				}
				else if (first.localSpaceIndex != (uint32_t)rootLocalSpace)
				{
					classification.decision = NRISpatialAbsenceConflictDecision::DifferentLocalSpace;
				}
				else if (AreLinkedAdjacent(first.sectorIndex, second.sectorIndex) ||
					AreLinkedAdjacent(second.sectorIndex, first.sectorIndex))
				{
					classification.decision = NRISpatialAbsenceConflictDecision::LinkedAdjacent;
				}
				else if (ArePortalRelated(mapWorld, first.chunkIndex, second.chunkIndex))
				{
					classification.decision = NRISpatialAbsenceConflictDecision::PortalRelated;
				}
			}
			uint32_t exactWitnessCount = 0;
			if (classification.decision == NRISpatialAbsenceConflictDecision::Certified)
			{
				exactWitnessCount = CountExactOverlapWitnesses(mapWorld, *positive, *negative);
				if (exactWitnessCount == 0)
				{
					classification.decision = NRISpatialAbsenceConflictDecision::ExactOverlapMissing;
				}
			}

			mSnapshot.conflicts.push_back(MakeDebugRecord(*positive, *negative, classification));
			mSnapshot.conflicts.back().exactWitnessCount = exactWitnessCount;
			if (classification.decision != NRISpatialAbsenceConflictDecision::Certified)
			{
				continue;
			}

			CertifiedPair pair;
			pair.continuityKey.worldGeneration = input.worldGeneration;
			pair.continuityKey.positiveChunk = positive->chunkIndex;
			pair.continuityKey.negativeChunk = negative->chunkIndex;
			pair.continuityKey.localSpaceIndex = positive->localSpaceIndex;
			pair.positiveSector = (uint32_t)positive->sectorIndex;
			pair.negativeSector = (uint32_t)negative->sectorIndex;
			std::memcpy(pair.overlapMin, classification.overlapMin, sizeof(pair.overlapMin));
			std::memcpy(pair.overlapMax, classification.overlapMax, sizeof(pair.overlapMax));
			pair.distanceToCenter = classification.distanceToCenter;
			pair.exactWitnessCount = exactWitnessCount;
			certifiedPairs.push_back(pair);
		}
	}

	std::sort(certifiedPairs.begin(), certifiedPairs.end(), [](const CertifiedPair& first, const CertifiedPair& second)
	{
		return first.continuityKey < second.continuityKey;
	});
	std::vector<NRISpatialAbsenceContinuityKey> currentKeys;
	currentKeys.reserve(certifiedPairs.size());
	for (const CertifiedPair& pair : certifiedPairs)
		currentKeys.push_back(pair.continuityKey);
	const bool contextContinuous = input.captureSerial != 0 &&
		mPreviousContinuityCaptureSerial + 1u == input.captureSerial &&
		mContinuityWorldGeneration == input.worldGeneration &&
		mContinuityRootLocalSpaceIndex == rootLocalSpace;
	mConflictContinuity = AdvanceConflictContinuity(currentKeys, mConflictContinuity, contextContinuous);
	for (size_t index = 0; index < certifiedPairs.size(); ++index)
	{
		certifiedPairs[index].authorized = mConflictContinuity[index].authorized;
		mSnapshot.authorizedPairCount += certifiedPairs[index].authorized ? 1u : 0u;
		mSnapshot.pendingPairCount += certifiedPairs[index].authorized ? 0u : 1u;
	}
	mPreviousContinuityCaptureSerial = input.captureSerial;
	mContinuityWorldGeneration = input.worldGeneration;
	mContinuityRootLocalSpaceIndex = rootLocalSpace;

	mSnapshot.gpuRecords.resize(mapWorld.chunks.size() + 1u);
	mSnapshot.negativeChunkWords.resize(std::max<size_t>((mapWorld.chunks.size() + 31u) / 32u, 1u), 0u);
	std::vector<std::vector<Triangle2D>> footprints(mapWorld.chunks.size());
	std::vector<uint32_t> involvedChunks;
	for (const CertifiedPair& pair : certifiedPairs)
	{
		if (!pair.authorized) continue;
		involvedChunks.push_back(pair.continuityKey.positiveChunk);
		involvedChunks.push_back(pair.continuityKey.negativeChunk);
	}
	std::sort(involvedChunks.begin(), involvedChunks.end());
	involvedChunks.erase(std::unique(involvedChunks.begin(), involvedChunks.end()), involvedChunks.end());
	for (uint32_t chunkIndex : involvedChunks)
	{
		if (chunkIndex < mapWorld.chunks.size())
			footprints[chunkIndex] = CollectChunkFootprintTriangles(mapWorld, mapWorld.chunks[chunkIndex]);
	}
	std::vector<FootprintGrid> footprintGrids(mapWorld.chunks.size());
	bool gridBuildFailed = mapWorld.chunks.size() >= kMaxFloatExactInteger;
	size_t estimatedRecordCount = mapWorld.chunks.size() + 1u;
	auto addEstimatedRecords = [&](size_t count)
	{
		if (estimatedRecordCount > kMaxFloatExactInteger ||
			count > kMaxFloatExactInteger - estimatedRecordCount)
			gridBuildFailed = true;
		else
			estimatedRecordCount += count;
	};
	addEstimatedRecords(mSnapshot.authorizedPairCount);
	for (uint32_t chunkIndex : involvedChunks)
	{
		if (chunkIndex >= footprints.size() || footprints[chunkIndex].empty()) continue;
		FootprintGrid& grid = footprintGrids[chunkIndex];
		if (!BuildFootprintGrid(footprints[chunkIndex], grid) ||
			!ValidateFootprintGrid(footprints[chunkIndex], grid))
		{
			gridBuildFailed = true;
			break;
		}
		addEstimatedRecords(footprints[chunkIndex].size());
		addEstimatedRecords(1u + grid.cells.size() + grid.referenceRecordCount);
	}
	if (gridBuildFailed)
	{
		mSnapshot.failOpenFlags |= NRI_SPATIAL_ABSENCE_FAIL_GPU_INDEX_OVERFLOW;
		mSnapshot.gpuRecords.assign(1u, {});
		mSnapshot.negativeChunkWords.clear();
		mConflictContinuity.clear();
		mPreviousContinuityCaptureSerial = 0;
		mContinuityWorldGeneration = 0;
		mContinuityRootLocalSpaceIndex = -1;
		return finalizeTelemetry();
	}
	mSnapshot.gpuRecords.reserve(estimatedRecordCount);

	for (size_t firstPair = 0; firstPair < certifiedPairs.size();)
	{
		const uint32_t negativeChunk = certifiedPairs[firstPair].continuityKey.negativeChunk;
		size_t pairEnd = firstPair;
		while (pairEnd < certifiedPairs.size() &&
			certifiedPairs[pairEnd].continuityKey.negativeChunk == negativeChunk)
			++pairEnd;
		std::vector<size_t> authorizedIndices;
		for (size_t index = firstPair; index < pairEnd; ++index)
			if (certifiedPairs[index].authorized) authorizedIndices.push_back(index);
		firstPair = pairEnd;
		if (authorizedIndices.empty() || negativeChunk >= mapWorld.chunks.size() || footprints[negativeChunk].empty())
			continue;

		NRISpatialAbsenceSelectionRecord selection;
		selection.negativeChunk = negativeChunk;
		selection.boundsMin[0] = selection.boundsMin[1] = selection.boundsMin[2] = std::numeric_limits<float>::max();
		selection.boundsMax[0] = selection.boundsMax[1] = selection.boundsMax[2] = -std::numeric_limits<float>::max();
		std::vector<uint32_t> owners;
		uint64_t representationHash = HashValue(kHashOffset, negativeChunk);
		const uint32_t pairRecordStart = (uint32_t)mSnapshot.gpuRecords.size();
		for (size_t pairIndex : authorizedIndices)
		{
			const CertifiedPair& pair = certifiedPairs[pairIndex];
			const uint32_t positiveChunk = pair.continuityKey.positiveChunk;
			if (positiveChunk >= mapWorld.chunks.size() || footprints[positiveChunk].empty())
				continue;
			NRISpatialAbsenceGpuRecord pairRecord;
			pairRecord.flags = NRI_SPATIAL_ABSENCE_GPU_VALID | NRI_SPATIAL_ABSENCE_GPU_COMPLETE |
				NRI_SPATIAL_ABSENCE_GPU_CERTIFIED | NRI_SPATIAL_ABSENCE_GPU_PAIR;
			pairRecord.data0 = negativeChunk;
			pairRecord.data1 = positiveChunk;
			std::memcpy(pairRecord.payload, pair.overlapMin, sizeof(pair.overlapMin));
			std::memcpy(pairRecord.payload + 4u, pair.overlapMax, sizeof(pair.overlapMax));
			pairRecord.payload[8] = (float)pair.exactWitnessCount;
			mSnapshot.gpuRecords.push_back(pairRecord);
			owners.push_back(positiveChunk);
			selection.sourceWitnessCount += pair.exactWitnessCount;
			selection.footprintTriangleCount += (uint32_t)footprints[positiveChunk].size();
			for (uint32_t axis = 0; axis < 3u; ++axis)
			{
				selection.boundsMin[axis] = std::min(selection.boundsMin[axis], pair.overlapMin[axis]);
				selection.boundsMax[axis] = std::max(selection.boundsMax[axis], pair.overlapMax[axis]);
			}
			representationHash = HashValue(representationHash, positiveChunk);
			representationHash = HashBytes(representationHash, pair.overlapMin, sizeof(pair.overlapMin));
			representationHash = HashBytes(representationHash, pair.overlapMax, sizeof(pair.overlapMax));
		}
		const uint32_t pairRecordCount = (uint32_t)mSnapshot.gpuRecords.size() - pairRecordStart;
		if (pairRecordCount == 0u)
			continue;
		std::sort(owners.begin(), owners.end());
		owners.erase(std::unique(owners.begin(), owners.end()), owners.end());
		selection.firstPositiveChunk = owners.front();
		selection.sourcePositiveOwnerCount = selection.selectedPositiveOwnerCount = (uint32_t)owners.size();
		selection.sourcePositiveOwnerHash = selection.selectedPositiveOwnerHash = HashPositiveOwners(owners);
		selection.selectedWitnessCount = selection.sourceWitnessCount;
		selection.footprintTriangleCount += (uint32_t)footprints[negativeChunk].size();
		for (const Triangle2D& triangle : footprints[negativeChunk])
			representationHash = HashBytes(representationHash, &triangle, sizeof(triangle));
		for (uint32_t owner : owners)
			for (const Triangle2D& triangle : footprints[owner])
				representationHash = HashBytes(representationHash, &triangle, sizeof(triangle));
		selection.selectionHash = representationHash;
		mSnapshot.sourceWitnessCount += selection.sourceWitnessCount;
		mSnapshot.selectedWitnessCount += selection.selectedWitnessCount;
		mSnapshot.selections.push_back(selection);
		NRISpatialAbsenceGpuRecord& lookup = mSnapshot.gpuRecords[(size_t)negativeChunk + 1u];
		lookup.flags = NRI_SPATIAL_ABSENCE_GPU_VALID | NRI_SPATIAL_ABSENCE_GPU_COMPLETE |
			NRI_SPATIAL_ABSENCE_GPU_CERTIFIED;
		lookup.data2 = pairRecordStart;
		lookup.payload[8] = (float)pairRecordCount;
		std::memcpy(lookup.payload, selection.boundsMin, sizeof(selection.boundsMin));
		std::memcpy(lookup.payload + 4u, selection.boundsMax, sizeof(selection.boundsMax));
		MarkChunk(mSnapshot.negativeChunkWords, negativeChunk);
		mSnapshot.certifiedCount++;
	}

	for (uint32_t chunkIndex : involvedChunks)
	{
		if (chunkIndex >= mapWorld.chunks.size() || footprints[chunkIndex].empty()) continue;
		const uint32_t triangleStart = (uint32_t)mSnapshot.gpuRecords.size();
		for (const Triangle2D& triangle : footprints[chunkIndex])
		{
			NRISpatialAbsenceGpuRecord triangleRecord;
			triangleRecord.flags = NRI_SPATIAL_ABSENCE_GPU_VALID | NRI_SPATIAL_ABSENCE_GPU_COMPLETE |
				NRI_SPATIAL_ABSENCE_GPU_FOOTPRINT;
			triangleRecord.data0 = chunkIndex;
			std::memcpy(triangleRecord.payload, triangle.point, sizeof(triangle.point));
			mSnapshot.gpuRecords.push_back(triangleRecord);
		}
		const FootprintGrid& grid = footprintGrids[chunkIndex];
		const uint32_t gridHeaderIndex = (uint32_t)mSnapshot.gpuRecords.size();
		const uint32_t cellRecordStart = gridHeaderIndex + 1u;
		const uint32_t referenceRecordStart = cellRecordStart + (uint32_t)grid.cells.size();
		NRISpatialAbsenceGpuRecord gridHeader;
		gridHeader.flags = NRI_SPATIAL_ABSENCE_GPU_VALID | NRI_SPATIAL_ABSENCE_GPU_COMPLETE |
			NRI_SPATIAL_ABSENCE_GPU_GRID;
		gridHeader.data0 = chunkIndex;
		gridHeader.data1 = cellRecordStart;
		gridHeader.data2 = referenceRecordStart;
		gridHeader.payload[0] = grid.boundsMin[0];
		gridHeader.payload[1] = grid.boundsMin[1];
		gridHeader.payload[2] = grid.boundsMax[0];
		gridHeader.payload[3] = grid.boundsMax[1];
		gridHeader.payload[4] = (float)grid.width;
		gridHeader.payload[5] = (float)grid.height;
		gridHeader.payload[6] = (float)grid.cells.size();
		gridHeader.payload[7] = (float)grid.referenceCount;
		gridHeader.payload[8] = (float)grid.referenceRecordCount;
		mSnapshot.gpuRecords.push_back(gridHeader);
		uint32_t nextReferenceRecord = referenceRecordStart;
		for (uint32_t cellIndex = 0; cellIndex < grid.cells.size(); ++cellIndex)
		{
			const std::vector<uint32_t>& references = grid.cells[cellIndex];
			NRISpatialAbsenceGpuRecord cellRecord;
			cellRecord.flags = NRI_SPATIAL_ABSENCE_GPU_VALID | NRI_SPATIAL_ABSENCE_GPU_COMPLETE |
				NRI_SPATIAL_ABSENCE_GPU_GRID | NRI_SPATIAL_ABSENCE_GPU_GRID_CELL;
			cellRecord.data0 = chunkIndex;
			cellRecord.data1 = nextReferenceRecord;
			cellRecord.data2 = (uint32_t)references.size();
			cellRecord.payload[0] = (float)cellIndex;
			cellRecord.payload[1] = (float)((references.size() + 2u) / 3u);
			mSnapshot.gpuRecords.push_back(cellRecord);
			nextReferenceRecord += (cellRecord.data2 + 2u) / 3u;
		}
		for (const std::vector<uint32_t>& references : grid.cells)
		{
			for (uint32_t referenceOffset = 0; referenceOffset < references.size(); referenceOffset += 3u)
			{
				NRISpatialAbsenceGpuRecord referenceRecord;
				referenceRecord.flags = NRI_SPATIAL_ABSENCE_GPU_VALID | NRI_SPATIAL_ABSENCE_GPU_COMPLETE |
					NRI_SPATIAL_ABSENCE_GPU_GRID | NRI_SPATIAL_ABSENCE_GPU_GRID_REFERENCE;
				referenceRecord.data0 = references[referenceOffset];
				referenceRecord.data1 = referenceOffset + 1u < references.size()
					? references[referenceOffset + 1u] : UINT32_MAX;
				referenceRecord.data2 = referenceOffset + 2u < references.size()
					? references[referenceOffset + 2u] : UINT32_MAX;
				referenceRecord.payload[0] = (float)chunkIndex;
				referenceRecord.payload[1] = (float)(referenceOffset / 3u);
				mSnapshot.gpuRecords.push_back(referenceRecord);
			}
		}
		NRISpatialAbsenceGpuRecord& lookup = mSnapshot.gpuRecords[(size_t)chunkIndex + 1u];
		lookup.flags |= NRI_SPATIAL_ABSENCE_GPU_VALID | NRI_SPATIAL_ABSENCE_GPU_COMPLETE |
			NRI_SPATIAL_ABSENCE_GPU_FOOTPRINT | NRI_SPATIAL_ABSENCE_GPU_GRID;
		lookup.data0 = triangleStart;
		lookup.data1 = (uint32_t)footprints[chunkIndex].size();
		lookup.payload[9] = (float)gridHeaderIndex;
		mSnapshot.footprintTriangleCount += lookup.data1;
		mSnapshot.footprintGridCellCount += (uint32_t)grid.cells.size();
		mSnapshot.footprintGridReferenceCount += grid.referenceCount;
	}

	NRISpatialAbsenceGpuRecord& header = mSnapshot.gpuRecords[0];
	header.flags = NRI_SPATIAL_ABSENCE_GPU_VALID | NRI_SPATIAL_ABSENCE_GPU_COMPLETE;
	header.data0 = input.frameIndex;
	header.data1 = (uint32_t)input.worldGeneration;
	header.data2 = (uint32_t)(input.worldGeneration >> 32u);
	std::memcpy(header.payload, input.center, sizeof(input.center));
	header.payload[3] = input.guardRadius;
	header.payload[4] = (float)mapWorld.chunks.size();
	header.payload[5] = (float)mSnapshot.certifiedCount;
	header.payload[6] = (float)mSnapshot.authorizedPairCount;
	header.payload[7] = (float)mSnapshot.footprintTriangleCount;
	header.payload[8] = (float)mSnapshot.footprintGridCellCount;
	header.payload[9] = (float)mSnapshot.footprintGridReferenceCount;
	mSnapshot.valid = true;
	mSnapshot.payloadHash = HashBytes(kHashOffset, mSnapshot.gpuRecords.data(),
		mSnapshot.gpuRecords.size() * sizeof(NRISpatialAbsenceGpuRecord));
	return finalizeTelemetry();
}

bool RunNRISpatialAbsenceGateSelfTests(std::string* failureReason)
{
	auto fail = [&](const char* reason)
	{
		if (failureReason != nullptr)
		{
			*failureReason = reason;
		}
		return false;
	};
	auto bounds = [](float minX, float minY, float minZ, float maxX, float maxY, float maxZ)
	{
		nri_scene::PTMapChunkBounds value;
		value.valid = true;
		value.min[0] = minX; value.min[1] = minY; value.min[2] = minZ;
		value.max[0] = maxX; value.max[1] = maxY; value.max[2] = maxZ;
		return value;
	};

	const float center[3] = { 0.0f, 0.0f, 0.0f };
	if (ClassifyBounds(bounds(-2, -2, -2, -1, -1, -1), bounds(1, 1, 1, 2, 2, 2), center, 8.0f).decision !=
		NRISpatialAbsenceConflictDecision::Disjoint)
	{
		return fail("disjoint bounds were not rejected");
	}
	if (ClassifyBounds(bounds(-1, -1, -1, 0, 0, 0), bounds(0, -1, -1, 1, 0, 0), center, 8.0f).decision !=
		NRISpatialAbsenceConflictDecision::BoundaryContact)
	{
		return fail("boundary contact was treated as positive-volume overlap");
	}
	if (ClassifyBounds(bounds(-2, -2, -2, 2, 2, 2), bounds(-1, -1, -1, 1, 1, 1), center, 8.0f).decision !=
		NRISpatialAbsenceConflictDecision::Certified)
	{
		return fail("positive-volume overlap inside the guard was not certified");
	}
	if (ClassifyBounds(bounds(10, 10, 10, 14, 14, 14), bounds(11, 11, 11, 13, 13, 13), center, 4.0f).decision !=
		NRISpatialAbsenceConflictDecision::OutsideGuard)
	{
		return fail("overlap outside the finite guard was not rejected");
	}
	Triangle2D firstTriangle = { { { 0.0f, 0.0f }, { 4.0f, 0.0f }, { 0.0f, 4.0f } } };
	Triangle2D overlappingTriangle = { { { 1.0f, 1.0f }, { 3.0f, 1.0f }, { 1.0f, 3.0f } } };
	Triangle2D disjointTriangle = { { { 8.0f, 8.0f }, { 9.0f, 8.0f }, { 8.0f, 9.0f } } };
	if (TriangleIntersectionArea(firstTriangle, overlappingTriangle) <= kAreaEpsilon ||
		TriangleIntersectionArea(firstTriangle, disjointTriangle) > kAreaEpsilon)
	{
		return fail("exact footprint triangle overlap classification failed");
	}

	std::vector<Triangle2D> manyPositive;
	std::vector<Triangle2D> manyNegative;
	for (uint32_t index = 0; index < 17u; ++index)
	{
		const float base = (float)index * 4.0f;
		Triangle2D triangle = { { { base, 0.0f }, { base + 2.0f, 0.0f }, { base, 2.0f } } };
		manyPositive.push_back(triangle);
		manyNegative.push_back(triangle);
	}
	uint32_t exhaustiveWitnessCount = 0;
	for (const Triangle2D& positive : manyPositive)
		for (const Triangle2D& negative : manyNegative)
			exhaustiveWitnessCount += TriangleIntersectionArea(positive, negative) > kAreaEpsilon ? 1u : 0u;
	const float beyondOldCap[2] = { 64.25f, 0.25f };
	bool exhaustiveContains = false;
	for (const Triangle2D& positive : manyPositive)
	{
		for (const Triangle2D& negative : manyNegative)
		{
			if (TriangleIntersectionArea(positive, negative) > kAreaEpsilon &&
				PointInTriangleForTest(beyondOldCap, positive) && PointInTriangleForTest(beyondOldCap, negative))
			{
				exhaustiveContains = true;
			}
		}
	}
	const bool factorizedContains = PointInFootprintForTest(beyondOldCap, manyPositive) &&
		PointInFootprintForTest(beyondOldCap, manyNegative);
	if (exhaustiveWitnessCount <= 16u || !exhaustiveContains || factorizedContains != exhaustiveContains)
	{
		return fail("factorized footprints did not preserve exact coverage beyond the former 16-witness cap");
	}
	FootprintGrid manyGrid;
	if (!BuildFootprintGrid(manyPositive, manyGrid) || !ValidateFootprintGrid(manyPositive, manyGrid))
		return fail("complete footprint grid construction failed");
	for (int32_t xStep = -4; xStep <= 276; ++xStep)
	{
		for (int32_t yStep = -4; yStep <= 12; ++yStep)
		{
			const float sample[2] = { (float)xStep * 0.25f, (float)yStep * 0.25f };
			bool gridValid = false;
			const bool gridContains = PointInFootprintGridForTest(sample, manyPositive, manyGrid, gridValid);
			if (!gridValid || gridContains != PointInFootprintForTest(sample, manyPositive))
				return fail("footprint grid did not match exhaustive exact containment");
		}
	}
	FootprintGrid malformedGrid = manyGrid;
	auto nonemptyCell = std::find_if(malformedGrid.cells.begin(), malformedGrid.cells.end(),
		[](const std::vector<uint32_t>& references) { return !references.empty(); });
	if (nonemptyCell == malformedGrid.cells.end())
		return fail("footprint grid unexpectedly had no references");
	(*nonemptyCell)[0] = (uint32_t)manyPositive.size();
	if (ValidateFootprintGrid(manyPositive, malformedGrid))
		return fail("out-of-range footprint-grid reference did not fail open");
	malformedGrid = manyGrid;
	nonemptyCell = std::find_if(malformedGrid.cells.begin(), malformedGrid.cells.end(),
		[](const std::vector<uint32_t>& references) { return !references.empty(); });
	nonemptyCell->pop_back();
	malformedGrid.referenceCount--;
	malformedGrid.referenceRecordCount = 0u;
	for (const std::vector<uint32_t>& references : malformedGrid.cells)
		malformedGrid.referenceRecordCount += (uint32_t)((references.size() + 2u) / 3u);
	if (ValidateFootprintGrid(manyPositive, malformedGrid))
		return fail("truncated footprint-grid cell did not fail completeness validation");

	NRISpatialAbsenceContinuityKey keyA = { 7u, 3u, 9u, 0u };
	NRISpatialAbsenceContinuityKey keyB = { 7u, 4u, 10u, 0u };
	std::vector<NRISpatialAbsenceContinuityRecord> continuity;
	continuity = AdvanceConflictContinuity({ keyA }, continuity, false);
	if (continuity.size() != 1u || continuity[0].authorized || continuity[0].consecutiveCaptureCount != 1u)
		return fail("new conflict did not remain pending on its first complete observation");
	continuity = AdvanceConflictContinuity({ keyA }, continuity, true);
	if (!continuity[0].authorized || continuity[0].consecutiveCaptureCount != 2u)
		return fail("conflict did not authorize after two consecutive current certifications");
	continuity = AdvanceConflictContinuity({ keyA, keyB }, continuity, true);
	if (!continuity[0].authorized || continuity[1].authorized)
		return fail("a new unrelated conflict revoked established authority or authorized too early");
	continuity = AdvanceConflictContinuity({ keyB }, continuity, true);
	if (continuity.size() != 1u || continuity[0].key == keyA || !continuity[0].authorized)
		return fail("a conflict absent from the current census was carried forward");
	continuity = AdvanceConflictContinuity({ keyB }, continuity, false);
	if (continuity[0].authorized || continuity[0].consecutiveCaptureCount != 1u)
		return fail("root/world context transition did not revoke continuity immediately");

	nri_scene::PTMapWorld rootWorld;
	for (uint32_t index = 0; index < 2u; ++index)
	{
		nri_scene::PTMapChunk chunk;
		chunk.chunkIndex = index;
		chunk.sectorIndex = (int32_t)index + 3;
		chunk.localSpaceIndex = 2;
		rootWorld.chunks.push_back(chunk);
	}
	NRISpatialAbsenceCensusInput rootInput;
	rootInput.authoritativeRootSector = 3;
	rootInput.rootSectorIndices = { 3u, 4u };
	int32_t resolvedRootSpace = -1;
	if (!ResolveRootLocalSpace(rootWorld, rootInput, resolvedRootSpace) || resolvedRootSpace != 2)
		return fail("ordinary exact root-sector changes did not resolve to one continuity space");
	rootWorld.chunks[1].localSpaceIndex = 5;
	if (ResolveRootLocalSpace(rootWorld, rootInput, resolvedRootSpace))
		return fail("mixed root local spaces were accepted as one continuity context");

	NRISpatialAbsenceGate gate;
	nri_scene::PTMapWorld invalidWorld;
	NRISpatialAbsenceCensusInput incomplete;
	incomplete.frameIndex = 7;
	gate.Build(invalidWorld, incomplete);
	if (gate.GetSnapshot().HasNegativeAuthority() || gate.GetSnapshot().gpuRecords.empty())
	{
		return fail("invalid input did not produce a nonempty fail-open payload");
	}

	NRISpatialAbsenceSnapshot semanticProbe;
	semanticProbe.valid = true;
	semanticProbe.worldGeneration = 11;
	semanticProbe.candidateCount = 1;
	semanticProbe.certifiedCount = 1;
	semanticProbe.sourceWitnessCount = 24;
	semanticProbe.selectedWitnessCount = 16;
	NRISpatialAbsenceConflictRecord semanticConflict;
	semanticConflict.decision = NRISpatialAbsenceConflictDecision::Certified;
	semanticConflict.positiveChunk = 3;
	semanticConflict.negativeChunk = 7;
	semanticConflict.positiveSector = 3;
	semanticConflict.negativeSector = 7;
	semanticConflict.overlapMin[0] = -2.0f;
	semanticConflict.overlapMax[0] = 2.0f;
	semanticConflict.exactWitnessCount = 24;
	semanticProbe.conflicts.push_back(semanticConflict);
	NRISpatialAbsenceSelectionRecord semanticSelection;
	semanticSelection.negativeChunk = 7;
	semanticSelection.firstPositiveChunk = 3;
	semanticSelection.sourceWitnessCount = 24;
	semanticSelection.selectedWitnessCount = 16;
	semanticSelection.sourcePositiveOwnerCount = 1;
	semanticSelection.selectedPositiveOwnerCount = 1;
	semanticSelection.sourcePositiveOwnerHash = 17;
	semanticSelection.selectedPositiveOwnerHash = 17;
	semanticSelection.selectionHash = 23;
	semanticSelection.boundsMin[0] = -1.0f;
	semanticSelection.boundsMax[0] = 1.0f;
	semanticProbe.selections.push_back(semanticSelection);
	const uint64_t semanticHash = HashSnapshotSemantics(semanticProbe);
	const uint64_t selectionHash = HashSnapshotSelections(semanticProbe);
	semanticProbe.frameIndex = 99;
	semanticProbe.captureSerial = 123;
	semanticProbe.payloadHash = 456;
	semanticProbe.center[0] = 1024.0f;
	semanticProbe.center[1] = -32.0f;
	semanticProbe.center[2] = 2048.0f;
	semanticProbe.guardRadius = 64.0f;
	if (HashSnapshotSemantics(semanticProbe) != semanticHash ||
		HashSnapshotSelections(semanticProbe) != selectionHash)
	{
		return fail("semantic telemetry hash included camera or frame-only state");
	}
	semanticProbe.selections.front().selectionHash++;
	if (HashSnapshotSelections(semanticProbe) == selectionHash)
	{
		return fail("selection telemetry hash ignored retained witness identity");
	}
	semanticProbe.conflicts.front().decision = NRISpatialAbsenceConflictDecision::SameVisibility;
	if (HashSnapshotSemantics(semanticProbe) == semanticHash)
	{
		return fail("semantic telemetry hash ignored classifier decision");
	}

	NRISpatialAbsenceGate stableGate;
	nri_scene::PTMapWorld stableWorld;
	stableWorld.valid = true;
	stableWorld.buildSerial = 5;
	nri_scene::PTMapChunk stableChunk;
	stableChunk.chunkIndex = 0;
	stableChunk.sectorIndex = 0;
	stableChunk.localSpaceIndex = 0;
	stableChunk.bounds.valid = true;
	stableChunk.bounds.min[0] = stableChunk.bounds.min[1] = stableChunk.bounds.min[2] = -1.0f;
	stableChunk.bounds.max[0] = stableChunk.bounds.max[1] = stableChunk.bounds.max[2] = 1.0f;
	stableWorld.chunks.push_back(stableChunk);
	NRISpatialAbsenceCensusInput stableInput;
	stableInput.complete = true;
	stableInput.rootStable = true;
	stableInput.generationMatches = true;
	stableInput.observationHash = 0x1234;
	stableInput.worldGeneration = stableWorld.buildSerial;
	stableInput.captureSerial = 1;
	stableInput.frameIndex = 1;
	stableInput.authoritativeRootSector = 0;
	stableInput.rootSectorIndices.push_back(0);
	stableInput.guardRadius = 8.0f;
	stableInput.reachedSectorIndices.push_back(0);
	const NRISpatialAbsenceSnapshot& firstStable = stableGate.Build(stableWorld, stableInput);
	if (firstStable.stableCaptureCount != 1u ||
		firstStable.censusObservationHash != 0x1234 ||
		firstStable.previousCensusObservationHash != 0 ||
		firstStable.failOpenFlags != NRI_SPATIAL_ABSENCE_FAIL_NONE ||
		firstStable.HasNegativeAuthority())
	{
		return fail("first complete capture did not remain pair-level fail-open");
	}
	stableInput.captureSerial++;
	stableInput.frameIndex++;
	const NRISpatialAbsenceSnapshot& secondStable = stableGate.Build(stableWorld, stableInput);
	if (secondStable.stableCaptureCount != 2u || !secondStable.valid ||
		secondStable.censusObservationHash != 0x1234 ||
		secondStable.previousCensusObservationHash != 0x1234 ||
		(secondStable.failOpenFlags & NRI_SPATIAL_ABSENCE_FAIL_UNSTABLE_ROOT) != 0)
	{
		return fail("second equivalent capture did not publish stable telemetry");
	}
	const uint64_t stableSemanticHash = secondStable.semanticHash;
	stableInput.captureSerial++;
	stableInput.frameIndex++;
	stableInput.center[0] = 2.0f;
	const NRISpatialAbsenceSnapshot& translatedStable = stableGate.Build(stableWorld, stableInput);
	if (translatedStable.stableCaptureCount != 3u || translatedStable.semanticHash != stableSemanticHash)
	{
		return fail("camera-only translation changed stable semantic telemetry");
	}
	stableInput.captureSerial++;
	stableInput.frameIndex++;
	stableInput.observationHash = 0x5678;
	const NRISpatialAbsenceSnapshot& changedObservation = stableGate.Build(stableWorld, stableInput);
	if (changedObservation.censusObservationHash != 0x5678 ||
		changedObservation.previousCensusObservationHash != 0x1234)
	{
		return fail("census observation telemetry did not publish the current and previous hashes");
	}

	if (failureReason != nullptr)
	{
		failureReason->clear();
	}
	return true;
}

const char* GetNRISpatialAbsenceConflictDecisionName(NRISpatialAbsenceConflictDecision decision)
{
	switch (decision)
	{
	case NRISpatialAbsenceConflictDecision::Certified: return "certified";
	case NRISpatialAbsenceConflictDecision::Disjoint: return "disjoint";
	case NRISpatialAbsenceConflictDecision::BoundaryContact: return "boundary-contact";
	case NRISpatialAbsenceConflictDecision::SameVisibility: return "same-visibility";
	case NRISpatialAbsenceConflictDecision::UnknownSector: return "unknown-sector";
	case NRISpatialAbsenceConflictDecision::LinkedAdjacent: return "linked-adjacent";
	case NRISpatialAbsenceConflictDecision::PortalRelated: return "portal-related";
	case NRISpatialAbsenceConflictDecision::DifferentLocalSpace: return "different-local-space";
	case NRISpatialAbsenceConflictDecision::OutsideGuard: return "outside-guard";
	case NRISpatialAbsenceConflictDecision::ExactOverlapMissing: return "no-exact-overlap";
	case NRISpatialAbsenceConflictDecision::RuntimeAuthorityUnknown: return "runtime-authority-unknown";
	case NRISpatialAbsenceConflictDecision::AmbiguousNegativeOwner: return "ambiguous-negative-owner";
	default: return "unknown";
	}
}
