#include "nri_spatial_absence_gate.h"

#include "build.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>

namespace
{
	constexpr float kBoundsEpsilon = 1.0e-3f;
	constexpr float kAreaEpsilon = 1.0e-3f;
	// Large floor triangulations can create many redundant triangle-pair
	// witnesses. Retain a bounded hybrid of the largest witnesses in the
	// nearest conflict pair and the exact-nearest witnesses. Omission remains
	// a conservative false negative while the shader loop stays bounded.
	constexpr uint32_t kMaxExactWitnessesPerChunk = 16u;
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

	struct ExactOverlapWitness
	{
		Triangle2D first;
		Triangle2D second;
		float minZ = 0.0f;
		float maxZ = 0.0f;
		float area = 0.0f;
		float distanceToCenter = 0.0f;
		float pairDistanceToCenter = 0.0f;
		uint32_t positiveChunk = UINT32_MAX;
		uint32_t negativeChunk = UINT32_MAX;
	};

	struct CertifiedChunkWitnesses
	{
		uint32_t firstDebugRecord = UINT32_MAX;
		std::vector<ExactOverlapWitness> witnesses;
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

	std::vector<ExactOverlapWitness> BuildExactOverlapWitnesses(
		const nri_scene::PTMapWorld& mapWorld,
		const nri_scene::PTMapChunk& positive,
		const nri_scene::PTMapChunk& negative,
		const float center[3])
	{
		std::vector<ExactOverlapWitness> witnesses;
		// Chunk-wide vertical bounds are exact only for flat floor/ceiling
		// volumes. Slopes require a per-XY plane interval and remain fail-open.
		if (!HasFlatClosedVolume(mapWorld, positive) || !HasFlatClosedVolume(mapWorld, negative))
		{
			return witnesses;
		}
		const float minZ = std::max(positive.bounds.min[1], negative.bounds.min[1]);
		const float maxZ = std::min(positive.bounds.max[1], negative.bounds.max[1]);
		if (maxZ - minZ <= kBoundsEpsilon)
		{
			return witnesses;
		}

		const std::vector<Triangle2D> positiveTriangles = CollectChunkFootprintTriangles(mapWorld, positive);
		const std::vector<Triangle2D> negativeTriangles = CollectChunkFootprintTriangles(mapWorld, negative);
		for (const Triangle2D& first : positiveTriangles)
		{
			for (const Triangle2D& second : negativeTriangles)
			{
				const float area = TriangleIntersectionArea(first, second);
				if (area <= kAreaEpsilon)
				{
					continue;
				}
				ExactOverlapWitness witness;
				witness.first = first;
				witness.second = second;
				witness.minZ = minZ;
				witness.maxZ = maxZ;
				witness.area = area;
				float firstMin[2] = { first.point[0][0], first.point[0][1] };
				float firstMax[2] = { firstMin[0], firstMin[1] };
				float secondMin[2] = { second.point[0][0], second.point[0][1] };
				float secondMax[2] = { secondMin[0], secondMin[1] };
				for (uint32_t corner = 1; corner < 3u; ++corner)
				{
					for (uint32_t axis = 0; axis < 2u; ++axis)
					{
						firstMin[axis] = std::min(firstMin[axis], first.point[corner][axis]);
						firstMax[axis] = std::max(firstMax[axis], first.point[corner][axis]);
						secondMin[axis] = std::min(secondMin[axis], second.point[corner][axis]);
						secondMax[axis] = std::max(secondMax[axis], second.point[corner][axis]);
					}
				}
				const float exactBoundsMin[3] = {
					std::max(firstMin[0], secondMin[0]), minZ,
					std::max(firstMin[1], secondMin[1])
				};
				const float exactBoundsMax[3] = {
					std::min(firstMax[0], secondMax[0]), maxZ,
					std::min(firstMax[1], secondMax[1])
				};
				witness.distanceToCenter = DistanceToBounds(center, exactBoundsMin, exactBoundsMax);
				witness.positiveChunk = positive.chunkIndex;
				witness.negativeChunk = negative.chunkIndex;
				witnesses.push_back(witness);
			}
		}
		return witnesses;
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

	uint64_t HashSelectedWitnesses(uint32_t negativeChunk, const std::vector<ExactOverlapWitness>& witnesses)
	{
		uint64_t hash = HashValue(kHashOffset, negativeChunk);
		for (const ExactOverlapWitness& witness : witnesses)
		{
			hash = HashValue(hash, witness.positiveChunk);
			hash = HashValue(hash, witness.negativeChunk);
			hash = HashBytes(hash, &witness.first, sizeof(witness.first));
			hash = HashBytes(hash, &witness.second, sizeof(witness.second));
			hash = HashValue(hash, witness.minZ);
			hash = HashValue(hash, witness.maxZ);
			hash = HashValue(hash, witness.area);
		}
		return hash;
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
		return finalizeTelemetry();
	}
	if (!input.complete)
	{
		mSnapshot.failOpenFlags |= NRI_SPATIAL_ABSENCE_FAIL_INCOMPLETE_CENSUS;
	}
	if (!input.rootStable || mStableCaptureCount < 2u)
	{
		mSnapshot.failOpenFlags |= NRI_SPATIAL_ABSENCE_FAIL_UNSTABLE_ROOT;
	}
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
		const uint32_t continuityBreakingFlags = mSnapshot.failOpenFlags &
			~NRI_SPATIAL_ABSENCE_FAIL_UNSTABLE_ROOT;
		if (continuityBreakingFlags != 0 || !input.rootStable)
		{
			mStableWorldGeneration = 0;
			mStableObservationHash = 0;
			mStableCaptureCount = 0;
		}
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

	mSnapshot.gpuRecords.resize(mapWorld.chunks.size() + 1u);
	mSnapshot.negativeChunkWords.resize(std::max<size_t>((mapWorld.chunks.size() + 31u) / 32u, 1u), 0u);
	std::unordered_map<uint32_t, CertifiedChunkWitnesses> certifiedByNegativeChunk;

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
			std::vector<ExactOverlapWitness> exactWitnesses;
			if (classification.decision == NRISpatialAbsenceConflictDecision::Certified)
			{
				exactWitnesses = BuildExactOverlapWitnesses(mapWorld, *positive, *negative, input.center);
				for (ExactOverlapWitness& witness : exactWitnesses)
				{
					witness.pairDistanceToCenter = classification.distanceToCenter;
				}
				if (exactWitnesses.empty())
				{
					classification.decision = NRISpatialAbsenceConflictDecision::ExactOverlapMissing;
				}
			}

			const size_t debugIndex = mSnapshot.conflicts.size();
			mSnapshot.conflicts.push_back(MakeDebugRecord(*positive, *negative, classification));
			mSnapshot.conflicts.back().exactWitnessCount = (uint32_t)exactWitnesses.size();
			if (classification.decision != NRISpatialAbsenceConflictDecision::Certified)
			{
				continue;
			}

			CertifiedChunkWitnesses& certified = certifiedByNegativeChunk[negative->chunkIndex];
			if (certified.firstDebugRecord == UINT32_MAX)
			{
				certified.firstDebugRecord = (uint32_t)debugIndex;
			}
			certified.witnesses.insert(certified.witnesses.end(), exactWitnesses.begin(), exactWitnesses.end());
		}
	}

	std::vector<uint32_t> certifiedChunkIndices;
	certifiedChunkIndices.reserve(certifiedByNegativeChunk.size());
	mSnapshot.gpuRecords.reserve(mSnapshot.gpuRecords.size() +
		certifiedByNegativeChunk.size() * kMaxExactWitnessesPerChunk);
	for (const auto& item : certifiedByNegativeChunk)
	{
		certifiedChunkIndices.push_back(item.first);
	}
	std::sort(certifiedChunkIndices.begin(), certifiedChunkIndices.end());

	for (uint32_t negativeChunk : certifiedChunkIndices)
	{
		CertifiedChunkWitnesses& certified = certifiedByNegativeChunk[negativeChunk];
		if (negativeChunk >= mapWorld.chunks.size() || certified.witnesses.empty())
		{
			continue;
		}
		NRISpatialAbsenceSelectionRecord selection;
		selection.negativeChunk = negativeChunk;
		selection.sourceWitnessCount = (uint32_t)certified.witnesses.size();
		std::vector<uint32_t> sourcePositiveOwners;
		sourcePositiveOwners.reserve(certified.witnesses.size());
		for (const ExactOverlapWitness& witness : certified.witnesses)
		{
			sourcePositiveOwners.push_back(witness.positiveChunk);
		}
		std::sort(sourcePositiveOwners.begin(), sourcePositiveOwners.end());
		sourcePositiveOwners.erase(std::unique(sourcePositiveOwners.begin(), sourcePositiveOwners.end()), sourcePositiveOwners.end());
		selection.sourcePositiveOwnerCount = (uint32_t)sourcePositiveOwners.size();
		selection.sourcePositiveOwnerHash = HashPositiveOwners(sourcePositiveOwners);
		mSnapshot.sourceWitnessCount += selection.sourceWitnessCount;

		std::vector<ExactOverlapWitness> pairPriority = certified.witnesses;
		std::stable_sort(pairPriority.begin(), pairPriority.end(), [](const auto& first, const auto& second)
		{
			if (first.pairDistanceToCenter != second.pairDistanceToCenter)
				return first.pairDistanceToCenter < second.pairDistanceToCenter;
			if (first.area != second.area) return first.area > second.area;
			if (first.positiveChunk != second.positiveChunk) return first.positiveChunk < second.positiveChunk;
			return first.negativeChunk < second.negativeChunk;
		});
		std::vector<ExactOverlapWitness> exactPriority = certified.witnesses;
		std::stable_sort(exactPriority.begin(), exactPriority.end(), [](const auto& first, const auto& second)
		{
			if (first.distanceToCenter != second.distanceToCenter)
				return first.distanceToCenter < second.distanceToCenter;
			if (first.area != second.area) return first.area > second.area;
			if (first.positiveChunk != second.positiveChunk) return first.positiveChunk < second.positiveChunk;
			return first.negativeChunk < second.negativeChunk;
		});
		certified.witnesses.clear();
		auto appendUnique = [&](const ExactOverlapWitness& candidate)
		{
			for (const ExactOverlapWitness& selected : certified.witnesses)
			{
				if (selected.positiveChunk == candidate.positiveChunk &&
					selected.negativeChunk == candidate.negativeChunk &&
					std::memcmp(&selected.first, &candidate.first, sizeof(candidate.first)) == 0 &&
					std::memcmp(&selected.second, &candidate.second, sizeof(candidate.second)) == 0)
				{
					return;
				}
			}
			certified.witnesses.push_back(candidate);
		};
		const uint32_t areaQuota = kMaxExactWitnessesPerChunk / 2u;
		for (uint32_t index = 0; index < pairPriority.size() && index < areaQuota; ++index)
		{
			appendUnique(pairPriority[index]);
		}
		for (const ExactOverlapWitness& witness : exactPriority)
		{
			if (certified.witnesses.size() >= kMaxExactWitnessesPerChunk) break;
			appendUnique(witness);
		}
		selection.selectedWitnessCount = (uint32_t)certified.witnesses.size();
		selection.selectionHash = HashSelectedWitnesses(negativeChunk, certified.witnesses);
		std::vector<uint32_t> selectedPositiveOwners;
		selectedPositiveOwners.reserve(certified.witnesses.size());
		for (const ExactOverlapWitness& witness : certified.witnesses)
		{
			selectedPositiveOwners.push_back(witness.positiveChunk);
		}
		std::sort(selectedPositiveOwners.begin(), selectedPositiveOwners.end());
		selectedPositiveOwners.erase(std::unique(selectedPositiveOwners.begin(), selectedPositiveOwners.end()), selectedPositiveOwners.end());
		selection.firstPositiveChunk = selectedPositiveOwners.empty() ? UINT32_MAX : selectedPositiveOwners.front();
		selection.selectedPositiveOwnerCount = (uint32_t)selectedPositiveOwners.size();
		selection.selectedPositiveOwnerHash = HashPositiveOwners(selectedPositiveOwners);
		mSnapshot.selectedWitnessCount += selection.selectedWitnessCount;

		const size_t recordIndex = (size_t)negativeChunk + 1u;
		const uint32_t witnessStart = (uint32_t)mSnapshot.gpuRecords.size();
		float witnessBoundsMin[3] = {
			std::numeric_limits<float>::max(),
			std::numeric_limits<float>::max(),
			std::numeric_limits<float>::max()
		};
		float witnessBoundsMax[3] = {
			-std::numeric_limits<float>::max(),
			-std::numeric_limits<float>::max(),
			-std::numeric_limits<float>::max()
		};
		for (const ExactOverlapWitness& witness : certified.witnesses)
		{
			NRISpatialAbsenceGpuRecord gpuWitness;
			gpuWitness.flags = NRI_SPATIAL_ABSENCE_GPU_VALID | NRI_SPATIAL_ABSENCE_GPU_COMPLETE |
				NRI_SPATIAL_ABSENCE_GPU_CERTIFIED;
			gpuWitness.data0 = witness.negativeChunk;
			gpuWitness.data1 = witness.positiveChunk;
			for (uint32_t corner = 0; corner < 3u; ++corner)
			{
				gpuWitness.payload[corner * 2u] = witness.first.point[corner][0];
				gpuWitness.payload[corner * 2u + 1u] = witness.first.point[corner][1];
				gpuWitness.payload[6u + corner * 2u] = witness.second.point[corner][0];
				gpuWitness.payload[6u + corner * 2u + 1u] = witness.second.point[corner][1];
			}
			gpuWitness.payload[12] = witness.minZ;
			gpuWitness.payload[13] = witness.maxZ;
			gpuWitness.payload[14] = witness.area;
			mSnapshot.gpuRecords.push_back(gpuWitness);
			witnessBoundsMin[1] = std::min(witnessBoundsMin[1], witness.minZ);
			witnessBoundsMax[1] = std::max(witnessBoundsMax[1], witness.maxZ);
			float firstMin[2] = { witness.first.point[0][0], witness.first.point[0][1] };
			float firstMax[2] = { firstMin[0], firstMin[1] };
			float secondMin[2] = { witness.second.point[0][0], witness.second.point[0][1] };
			float secondMax[2] = { secondMin[0], secondMin[1] };
			for (uint32_t corner = 1; corner < 3u; ++corner)
			{
				for (uint32_t axis = 0; axis < 2u; ++axis)
				{
					firstMin[axis] = std::min(firstMin[axis], witness.first.point[corner][axis]);
					firstMax[axis] = std::max(firstMax[axis], witness.first.point[corner][axis]);
					secondMin[axis] = std::min(secondMin[axis], witness.second.point[corner][axis]);
					secondMax[axis] = std::max(secondMax[axis], witness.second.point[corner][axis]);
				}
			}
			witnessBoundsMin[0] = std::min(witnessBoundsMin[0], std::max(firstMin[0], secondMin[0]));
			witnessBoundsMax[0] = std::max(witnessBoundsMax[0], std::min(firstMax[0], secondMax[0]));
			witnessBoundsMin[2] = std::min(witnessBoundsMin[2], std::max(firstMin[1], secondMin[1]));
			witnessBoundsMax[2] = std::max(witnessBoundsMax[2], std::min(firstMax[1], secondMax[1]));
		}
		// Appending witness records can reallocate gpuRecords. Reacquire the
		// lookup record only after all pushes so its payload cannot be written
		// through an invalidated reference.
		NRISpatialAbsenceGpuRecord& record = mSnapshot.gpuRecords[recordIndex];
		record.flags = NRI_SPATIAL_ABSENCE_GPU_VALID | NRI_SPATIAL_ABSENCE_GPU_COMPLETE |
			NRI_SPATIAL_ABSENCE_GPU_CERTIFIED;
		record.data0 = witnessStart;
		record.data1 = (uint32_t)certified.witnesses.size();
		record.data2 = certified.witnesses.front().positiveChunk;
		record.payload[0] = witnessBoundsMin[0];
		record.payload[1] = witnessBoundsMin[1];
		record.payload[2] = witnessBoundsMin[2];
		record.payload[4] = witnessBoundsMax[0];
		record.payload[5] = witnessBoundsMax[1];
		record.payload[6] = witnessBoundsMax[2];
		std::memcpy(selection.boundsMin, witnessBoundsMin, sizeof(selection.boundsMin));
		std::memcpy(selection.boundsMax, witnessBoundsMax, sizeof(selection.boundsMax));
		mSnapshot.selections.push_back(selection);
		MarkChunk(mSnapshot.negativeChunkWords, negativeChunk);
		mSnapshot.certifiedCount++;
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
	header.payload[6] = (float)(mSnapshot.gpuRecords.size() - mapWorld.chunks.size() - 1u);
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
	stableInput.frameIndex = 1;
	stableInput.guardRadius = 8.0f;
	stableInput.reachedSectorIndices.push_back(0);
	const NRISpatialAbsenceSnapshot& firstStable = stableGate.Build(stableWorld, stableInput);
	if (firstStable.stableCaptureCount != 1u ||
		firstStable.censusObservationHash != 0x1234 ||
		firstStable.previousCensusObservationHash != 0 ||
		(firstStable.failOpenFlags & NRI_SPATIAL_ABSENCE_FAIL_UNSTABLE_ROOT) == 0)
	{
		return fail("first stable capture did not remain fail-open");
	}
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
	stableInput.frameIndex++;
	stableInput.center[0] = 2.0f;
	const NRISpatialAbsenceSnapshot& translatedStable = stableGate.Build(stableWorld, stableInput);
	if (translatedStable.stableCaptureCount != 3u || translatedStable.semanticHash != stableSemanticHash)
	{
		return fail("camera-only translation changed stable semantic telemetry");
	}
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
