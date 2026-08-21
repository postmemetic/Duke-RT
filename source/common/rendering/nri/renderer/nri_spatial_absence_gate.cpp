#include "nri_spatial_absence_gate.h"

#include "nri_diagnostic_names.h"
#include "nri_frame_resources.h"

#include "build.h"

#include <Extensions/NRIRayTracing.h>

#include <algorithm>
#include <array>
#include <chrono>
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
	constexpr uint32_t kOpenPlaneMaterialFlags = nri_scene::MaterialFlag_Sky |
		nri_scene::MaterialFlag_Portal | nri_scene::MaterialFlag_Mirror |
		nri_scene::MaterialFlag_PlainMirror;

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
		std::vector<uint32_t> interiorTriangles;
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
		size_t debugRecordIndex = std::numeric_limits<size_t>::max();
		bool authorized = false;
	};

	struct CachedChunkTopology
	{
		bool boundaryResolved = false;
		bool resolved = false;
		bool flatClosed = false;
		bool openBoundary = false;
		bool gridValid = false;
		std::vector<Triangle2D> footprint;
		FootprintGrid grid;
	};

	struct CachedPairTopology
	{
		size_t firstIndex = 0;
		size_t secondIndex = 0;
		PairClassification bounds;
		bool exactWitnessResolved = false;
		uint32_t exactWitnessCount = 0;
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

	bool AreOrdinarilyLinkedAdjacent(int32_t firstSector, int32_t secondSector)
	{
		if (firstSector < 0 || secondSector < 0 ||
			(unsigned)firstSector >= sector.Size() || (unsigned)secondSector >= sector.Size())
		{
			return false;
		}

		for (const walltype& wall : sector[(unsigned)firstSector].walls)
		{
			if (wall.nextsector == secondSector && wall.portalflags == 0)
			{
				return true;
			}
		}
		return false;
	}

	template<typename VisitNeighbors, typename IsAdjacent>
	int32_t FindOrdinaryTopologyIntermediateImpl(
		int32_t firstSector,
		int32_t secondSector,
		size_t sectorCount,
		VisitNeighbors&& visitNeighbors,
		IsAdjacent&& isAdjacent)
	{
		if (firstSector < 0 || secondSector < 0 ||
			(size_t)firstSector >= sectorCount || (size_t)secondSector >= sectorCount)
		{
			return -1;
		}
		if (firstSector == secondSector || isAdjacent(firstSector, secondSector) ||
			isAdjacent(secondSector, firstSector))
		{
			return -1;
		}

		auto findFrom = [&](int32_t sourceSector, int32_t targetSector)
		{
			int32_t foundIntermediate = -1;
			visitNeighbors(sourceSector, [&](int32_t intermediateSector)
			{
				if (foundIntermediate >= 0 || intermediateSector < 0 || intermediateSector == sourceSector ||
					intermediateSector == targetSector || (size_t)intermediateSector >= sectorCount)
				{
					return;
				}
				if (isAdjacent(intermediateSector, targetSector) ||
					isAdjacent(targetSector, intermediateSector))
				{
					foundIntermediate = intermediateSector;
				}
			});
			return foundIntermediate;
		};

		const int32_t forwardIntermediate = findFrom(firstSector, secondSector);
		return forwardIntermediate >= 0 ? forwardIntermediate : findFrom(secondSector, firstSector);
	}

	int32_t FindOrdinaryTopologyIntermediate(int32_t firstSector, int32_t secondSector)
	{
		return FindOrdinaryTopologyIntermediateImpl(
			firstSector,
			secondSector,
			sector.Size(),
			[](int32_t sourceSector, const auto& visit)
			{
				for (const walltype& wall : sector[(unsigned)sourceSector].walls)
				{
					if (wall.portalflags == 0)
						visit(wall.nextsector);
				}
			},
			[](int32_t sourceSector, int32_t targetSector)
			{
				return AreOrdinarilyLinkedAdjacent(sourceSector, targetSector);
			});
	}

	struct ClosureStripEdge
	{
		double start[2] = {};
		double end[2] = {};
		int32_t wallIndex = -1;
		int32_t nextSector = -1;
		bool ordinaryPortal = false;
	};

	struct ClosureStripTopology
	{
		int32_t sectorIndex = -1;
		bool collapsed = false;
		std::vector<ClosureStripEdge> edges;
	};

	struct ClosureStripAnalysis
	{
		bool valid = false;
		int32_t portalNeighbors[2] = { -1, -1 };
		int32_t portalWalls[2] = { -1, -1 };
	};

	double EdgeLength(const ClosureStripEdge& edge)
	{
		const double x = edge.end[0] - edge.start[0];
		const double y = edge.end[1] - edge.start[1];
		return std::sqrt(x * x + y * y);
	}

	bool NearlyEqualLength(double first, double second)
	{
		const double scale = std::max({ 1.0, std::abs(first), std::abs(second) });
		return std::abs(first - second) <= scale * 1.0e-6;
	}

	ClosureStripAnalysis AnalyzeClosureStrip(const ClosureStripTopology& topology)
	{
		ClosureStripAnalysis analysis;
		if (topology.sectorIndex < 0 || topology.edges.size() != 4u)
			return analysis;

		int portalIndices[2] = { -1, -1 };
		int capIndices[2] = { -1, -1 };
		uint32_t portalCount = 0;
		uint32_t capCount = 0;
		for (uint32_t edgeIndex = 0; edgeIndex < topology.edges.size(); ++edgeIndex)
		{
			const ClosureStripEdge& edge = topology.edges[edgeIndex];
			if (edge.ordinaryPortal && edge.nextSector >= 0)
			{
				if (portalCount >= 2u)
					return analysis;
				portalIndices[portalCount++] = (int)edgeIndex;
			}
			else if (edge.nextSector < 0)
			{
				if (capCount >= 2u)
					return analysis;
				capIndices[capCount++] = (int)edgeIndex;
			}
			else
			{
				// A linked nonordinary edge is not a one-sided end cap.
				return analysis;
			}
		}
		if (portalCount != 2u || capCount != 2u ||
			topology.edges[(size_t)portalIndices[0]].nextSector ==
				topology.edges[(size_t)portalIndices[1]].nextSector ||
			std::abs(portalIndices[0] - portalIndices[1]) != 2)
		{
			return analysis;
		}

		const ClosureStripEdge& firstPortal = topology.edges[(size_t)portalIndices[0]];
		const ClosureStripEdge& secondPortal = topology.edges[(size_t)portalIndices[1]];
		const double firstX = firstPortal.end[0] - firstPortal.start[0];
		const double firstY = firstPortal.end[1] - firstPortal.start[1];
		const double secondX = secondPortal.end[0] - secondPortal.start[0];
		const double secondY = secondPortal.end[1] - secondPortal.start[1];
		const double firstLength = EdgeLength(firstPortal);
		const double secondLength = EdgeLength(secondPortal);
		if (firstLength <= 0.0 || secondLength <= 0.0 ||
			!NearlyEqualLength(firstLength, secondLength))
		{
			return analysis;
		}
		const double vectorScale = firstLength * secondLength;
		const double cross = firstX * secondY - firstY * secondX;
		const double dot = firstX * secondX + firstY * secondY;
		if (std::abs(cross) > vectorScale * 1.0e-6 || dot >= 0.0)
			return analysis;

		const double firstCapLength = EdgeLength(topology.edges[(size_t)capIndices[0]]);
		const double secondCapLength = EdgeLength(topology.edges[(size_t)capIndices[1]]);
		const double maximumCapLength = std::max(firstCapLength, secondCapLength);
		if (maximumCapLength <= 0.0 || firstLength / maximumCapLength < 16.0)
			return analysis;

		analysis.valid = true;
		for (uint32_t portalIndex = 0; portalIndex < 2u; ++portalIndex)
		{
			const ClosureStripEdge& edge = topology.edges[(size_t)portalIndices[portalIndex]];
			analysis.portalNeighbors[portalIndex] = edge.nextSector;
			analysis.portalWalls[portalIndex] = edge.wallIndex;
		}
		return analysis;
	}

	bool ContainsPortalNeighbor(const ClosureStripAnalysis& analysis, int32_t sectorIndex)
	{
		return analysis.portalNeighbors[0] == sectorIndex ||
			analysis.portalNeighbors[1] == sectorIndex;
	}

	int32_t FindPortalWall(const ClosureStripAnalysis& analysis, int32_t sectorIndex)
	{
		for (uint32_t portalIndex = 0; portalIndex < 2u; ++portalIndex)
		{
			if (analysis.portalNeighbors[portalIndex] == sectorIndex)
				return analysis.portalWalls[portalIndex];
		}
		return -1;
	}

	bool IsProtectedSealingCarrierPair(
		const ClosureStripTopology& negative,
		const ClosureStripTopology& closure,
		bool authoredClosure,
		bool emittedOrdinaryWallBand)
	{
		if (negative.collapsed || !closure.collapsed || !authoredClosure ||
			!emittedOrdinaryWallBand)
		{
			return false;
		}
		const ClosureStripAnalysis negativeAnalysis = AnalyzeClosureStrip(negative);
		const ClosureStripAnalysis closureAnalysis = AnalyzeClosureStrip(closure);
		return negativeAnalysis.valid && closureAnalysis.valid &&
			ContainsPortalNeighbor(negativeAnalysis, closure.sectorIndex) &&
			ContainsPortalNeighbor(closureAnalysis, negative.sectorIndex);
	}

	ClosureStripTopology BuildClosureStripTopology(int32_t sectorIndex)
	{
		ClosureStripTopology topology;
		if (sectorIndex < 0 || (unsigned)sectorIndex >= sector.Size())
			return topology;
		const sectortype& candidate = sector[(unsigned)sectorIndex];
		topology.sectorIndex = sectorIndex;
		topology.collapsed = candidate.floorz == candidate.ceilingz &&
			candidate.floorheinum == candidate.ceilingheinum;
		topology.edges.reserve(candidate.walls.Size());
		for (const walltype& candidateWall : candidate.walls)
		{
			ClosureStripEdge edge;
			edge.start[0] = candidateWall.pos.X;
			edge.start[1] = candidateWall.pos.Y;
			const walltype* endWall = candidateWall.point2Wall();
			if (endWall == nullptr)
				return {};
			edge.end[0] = endWall->pos.X;
			edge.end[1] = endWall->pos.Y;
			edge.wallIndex = wall.IndexOf(&candidateWall);
			edge.nextSector = candidateWall.nextsector;
			edge.ordinaryPortal = candidateWall.portalflags == 0 &&
				candidateWall.nextsector >= 0;
			topology.edges.push_back(edge);
		}
		return topology;
	}

	bool HasEmittedOrdinaryWallBand(
		const nri_scene::PTMapWorld& mapWorld,
		const nri_scene::PTMapChunk& negativeChunk,
		int32_t negativeSector,
		int32_t closureSector,
		int32_t expectedWall)
	{
		if (expectedWall < 0)
			return false;
		const uint64_t surfaceEnd = std::min<uint64_t>(
			(uint64_t)negativeChunk.firstSurface + negativeChunk.surfaceCount,
			mapWorld.surfaces.size());
		for (uint64_t surfaceIndex = negativeChunk.firstSurface;
			surfaceIndex < surfaceEnd; ++surfaceIndex)
		{
			const nri_scene::SurfaceProvenance& provenance =
				mapWorld.surfaces[(size_t)surfaceIndex].surface.provenance;
			if (provenance.sourceType != nri_scene::SurfaceSourceType::MapWallBand ||
				provenance.mapChunkIndex != (int32_t)negativeChunk.chunkIndex ||
				provenance.sectorIndex != negativeSector ||
				provenance.nextSectorIndex != closureSector ||
				provenance.wallIndex != expectedWall ||
				(unsigned)provenance.wallIndex >= wall.Size())
			{
				continue;
			}
			const walltype& sourceWall = wall[(unsigned)provenance.wallIndex];
			if (sourceWall.nextsector == closureSector && sourceWall.portalflags == 0)
				return true;
		}
		return false;
	}

	int32_t FindProtectedSealingCarrier(
		const nri_scene::PTMapWorld& mapWorld,
		const nri_scene::PTMapChunk& negativeChunk,
		const std::vector<uint32_t>& authoredClosureSectors)
	{
		const ClosureStripTopology negative = BuildClosureStripTopology(negativeChunk.sectorIndex);
		const ClosureStripAnalysis negativeAnalysis = AnalyzeClosureStrip(negative);
		if (negative.collapsed || !negativeAnalysis.valid)
			return -1;
		for (int32_t closureSector : negativeAnalysis.portalNeighbors)
		{
			if (closureSector < 0 ||
				!std::binary_search(authoredClosureSectors.begin(), authoredClosureSectors.end(),
					(uint32_t)closureSector))
			{
				continue;
			}
			const ClosureStripTopology closure = BuildClosureStripTopology(closureSector);
			if (IsProtectedSealingCarrierPair(
				negative,
				closure,
				true,
				HasEmittedOrdinaryWallBand(mapWorld, negativeChunk,
					negativeChunk.sectorIndex, closureSector,
					FindPortalWall(negativeAnalysis, closureSector))))
			{
				return closureSector;
			}
		}
		return -1;
	}

	struct InsetBoundarySealEdge
	{
		int32_t nextSector = -1;
		bool ordinaryReciprocalPortal = false;
		bool oneSided = false;
	};

	struct InsetBoundarySealTopology
	{
		int32_t sectorIndex = -1;
		bool flatPlanes = false;
		bool collapsed = false;
		bool sharesNeighborCeiling = false;
		bool floorStrictlyInsideNeighbor = false;
		bool emittedFloorSection = false;
		std::vector<InsetBoundarySealEdge> edges;
	};

	int32_t FindInsetBoundarySealNeighbor(const InsetBoundarySealTopology& topology)
	{
		if (topology.sectorIndex < 0 || topology.edges.size() < 3u)
			return -1;

		int32_t neighborSector = -1;
		uint32_t portalCount = 0;
		uint32_t oneSidedCount = 0;
		for (const InsetBoundarySealEdge& edge : topology.edges)
		{
			if (edge.oneSided && edge.nextSector < 0)
			{
				oneSidedCount++;
				continue;
			}
			if (!edge.ordinaryReciprocalPortal || edge.nextSector < 0 ||
				edge.nextSector == topology.sectorIndex)
			{
				return -1;
			}
			if (neighborSector < 0)
				neighborSector = edge.nextSector;
			else if (edge.nextSector != neighborSector)
				return -1;
			portalCount++;
		}
		return oneSidedCount == 1u && portalCount + oneSidedCount == topology.edges.size()
			? neighborSector : -1;
	}

	bool IsInsetBoundarySeal(const InsetBoundarySealTopology& topology)
	{
		return topology.flatPlanes && !topology.collapsed &&
			topology.sharesNeighborCeiling && topology.floorStrictlyInsideNeighbor &&
			topology.emittedFloorSection && FindInsetBoundarySealNeighbor(topology) >= 0;
	}

	bool HasEmittedInsetPlaneSection(
		const nri_scene::PTMapWorld& mapWorld,
		const nri_scene::PTMapChunk& chunk,
		nri_scene::PTMapSurfaceKind kind,
		nri_scene::SurfaceSourceType sourceType)
	{
		const uint64_t surfaceEnd = std::min<uint64_t>(
			(uint64_t)chunk.firstSurface + chunk.surfaceCount,
			mapWorld.surfaces.size());
		for (uint64_t surfaceIndex = chunk.firstSurface;
			surfaceIndex < surfaceEnd; ++surfaceIndex)
		{
			const nri_scene::PTMapSurface& surface = mapWorld.surfaces[(size_t)surfaceIndex];
			const nri_scene::SurfaceProvenance& provenance = surface.surface.provenance;
			if (surface.kind == kind &&
				surface.chunkIndex == chunk.chunkIndex &&
				provenance.sourceType == sourceType &&
				provenance.mapChunkIndex == (int32_t)chunk.chunkIndex &&
				provenance.sectorIndex == chunk.sectorIndex &&
				(surface.surface.material.flags & kOpenPlaneMaterialFlags) == 0 &&
				(surface.surface.indices.size() >= 3u || surface.surface.vertices.size() >= 3u))
			{
				return true;
			}
		}
		return false;
	}

	bool HasEmittedInsetFloorSection(
		const nri_scene::PTMapWorld& mapWorld,
		const nri_scene::PTMapChunk& chunk)
	{
		return HasEmittedInsetPlaneSection(mapWorld, chunk,
			nri_scene::PTMapSurfaceKind::Floor,
			nri_scene::SurfaceSourceType::MapFloorSection);
	}

	bool HasEmittedInsetCeilingSection(
		const nri_scene::PTMapWorld& mapWorld,
		const nri_scene::PTMapChunk& chunk)
	{
		return HasEmittedInsetPlaneSection(mapWorld, chunk,
			nri_scene::PTMapSurfaceKind::Ceiling,
			nri_scene::SurfaceSourceType::MapCeilingSection);
	}

	InsetBoundarySealTopology BuildInsetBoundarySealTopology(
		const nri_scene::PTMapWorld& mapWorld,
		const nri_scene::PTMapChunk& insetChunk)
	{
		InsetBoundarySealTopology topology;
		if (insetChunk.sectorIndex < 0 ||
			(unsigned)insetChunk.sectorIndex >= sector.Size())
		{
			return topology;
		}

		const sectortype& candidate = sector[(unsigned)insetChunk.sectorIndex];
		topology.sectorIndex = insetChunk.sectorIndex;
		topology.collapsed = candidate.ceilingz >= candidate.floorz;
		topology.emittedFloorSection = HasEmittedInsetFloorSection(mapWorld, insetChunk);
		topology.edges.reserve(candidate.walls.Size());
		for (const walltype& candidateWall : candidate.walls)
		{
			InsetBoundarySealEdge edge;
			edge.nextSector = candidateWall.nextsector;
			edge.oneSided = candidateWall.nextsector < 0 && candidateWall.nextwall < 0 &&
				candidateWall.portalflags == 0 &&
				!(candidateWall.cstat & CSTAT_WALL_MASKED) &&
				!(candidateWall.cstat & CSTAT_WALL_1WAY);
			if (candidateWall.nextsector >= 0 &&
				(unsigned)candidateWall.nextsector < sector.Size())
			{
				const walltype* reciprocalWall = candidateWall.nextWall();
				edge.ordinaryReciprocalPortal = candidateWall.portalflags == 0 &&
					reciprocalWall != nullptr &&
					reciprocalWall->nextsector == insetChunk.sectorIndex &&
					reciprocalWall->portalflags == 0 &&
					!(candidateWall.cstat & CSTAT_WALL_MASKED) &&
					!(candidateWall.cstat & CSTAT_WALL_1WAY) &&
					!(reciprocalWall->cstat & CSTAT_WALL_MASKED) &&
					!(reciprocalWall->cstat & CSTAT_WALL_1WAY);
			}
			topology.edges.push_back(edge);
		}

		const int32_t neighborSector = FindInsetBoundarySealNeighbor(topology);
		if (neighborSector < 0 || (unsigned)neighborSector >= sector.Size())
			return topology;
		const sectortype& neighbor = sector[(unsigned)neighborSector];
		topology.flatPlanes = candidate.ceilingheinum == 0 && candidate.floorheinum == 0 &&
			neighbor.ceilingheinum == 0 && neighbor.floorheinum == 0;
		topology.sharesNeighborCeiling = candidate.ceilingz == neighbor.ceilingz;
		topology.floorStrictlyInsideNeighbor = neighbor.ceilingz < candidate.floorz &&
			candidate.floorz < neighbor.floorz;
		return topology;
	}

	const nri_scene::PTMapChunk* FindChunkForSector(
		const nri_scene::PTMapWorld& mapWorld,
		int32_t sectorIndex,
		uint32_t localSpaceIndex)
	{
		if (sectorIndex < 0 || (size_t)sectorIndex >= mapWorld.sectorChunkLookup.size())
			return nullptr;
		const uint32_t chunkIndex = mapWorld.sectorChunkLookup[(size_t)sectorIndex];
		if (chunkIndex >= mapWorld.chunks.size())
			return nullptr;
		const nri_scene::PTMapChunk& chunk = mapWorld.chunks[chunkIndex];
		return chunk.chunkIndex == chunkIndex && chunk.sectorIndex == sectorIndex &&
			chunk.localSpaceIndex == localSpaceIndex ? &chunk : nullptr;
	}

	int32_t FindProtectedInsetBoundaryEnclosure(
		const nri_scene::PTMapWorld& mapWorld,
		const nri_scene::PTMapChunk& negativeOwner)
	{
		if (negativeOwner.sectorIndex < 0 ||
			(unsigned)negativeOwner.sectorIndex >= sector.Size() ||
			!HasEmittedInsetCeilingSection(mapWorld, negativeOwner))
		{
			return -1;
		}

		// The census-negative occurrence that must remain opaque is the sector
		// owning the enclosing ceiling, not the small raised-floor inset itself.
		// Require the inset's exact topology and emitted floor before extending
		// that dependency to its unique enclosing neighbor.
		const sectortype& ownerSector = sector[(unsigned)negativeOwner.sectorIndex];
		for (const walltype& ownerWall : ownerSector.walls)
		{
			if (ownerWall.nextsector < 0 ||
				(unsigned)ownerWall.nextsector >= sector.Size())
			{
				continue;
			}
			const nri_scene::PTMapChunk* insetChunk = FindChunkForSector(
				mapWorld, ownerWall.nextsector, negativeOwner.localSpaceIndex);
			if (insetChunk == nullptr)
				continue;
			const InsetBoundarySealTopology topology =
				BuildInsetBoundarySealTopology(mapWorld, *insetChunk);
			if (IsInsetBoundarySeal(topology) &&
				FindInsetBoundarySealNeighbor(topology) == negativeOwner.sectorIndex)
			{
				return insetChunk->sectorIndex;
			}
		}
		return -1;
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
			if ((surface.surface.material.flags & kOpenPlaneMaterialFlags) != 0)
			{
				return false;
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

	bool HasOpenPlaneBoundary(const nri_scene::PTMapWorld& mapWorld, const nri_scene::PTMapChunk& chunk)
	{
		const uint64_t surfaceEnd = std::min<uint64_t>(
			(uint64_t)chunk.firstSurface + chunk.surfaceCount,
			mapWorld.surfaces.size());
		for (uint64_t surfaceIndex = chunk.firstSurface; surfaceIndex < surfaceEnd; ++surfaceIndex)
		{
			const nri_scene::PTMapSurface& surface = mapWorld.surfaces[(size_t)surfaceIndex];
			if ((surface.kind == nri_scene::PTMapSurfaceKind::Floor ||
				surface.kind == nri_scene::PTMapSurfaceKind::Ceiling) &&
				(surface.surface.material.flags & kOpenPlaneMaterialFlags) != 0)
			{
				return true;
			}
		}
		return false;
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
		hash = HashValue(hash, snapshot.openBoundaryProtectedCount);
		hash = HashValue(hash, snapshot.nearTopologyProtectedCount);
		hash = HashValue(hash, snapshot.collapsedPortalProtectedCount);
		hash = HashValue(hash, snapshot.insetBoundaryEnclosureProtectedCount);
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
			hash = HashValue(hash, conflict.protectionFlags);
			hash = HashValue(hash, conflict.topologyIntermediateSector);
			hash = HashValue(hash, conflict.collapsedPortalSector);
			hash = HashValue(hash, conflict.insetBoundaryChildSector);
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

	bool DecodeSerializedUint(float value, uint32_t& decoded)
	{
		decoded = 0;
		if (!std::isfinite(value) || value < 0.0f || value > (float)kMaxFloatExactInteger)
			return false;
		decoded = (uint32_t)std::round(value);
		return value == (float)decoded;
	}

	bool SerializedRangeFits(uint32_t first, uint32_t count, size_t recordCount)
	{
		return (size_t)first <= recordCount && (size_t)count <= recordCount - (size_t)first;
	}

	bool ValidateSerializedSpatialAbsencePayload(const NRISpatialAbsenceSnapshot& snapshot)
	{
		const std::vector<NRISpatialAbsenceGpuRecord>& records = snapshot.gpuRecords;
		if (records.size() <= 1u || records.size() > kMaxFloatExactInteger)
			return false;

		constexpr uint32_t requiredBaseFlags =
			NRI_SPATIAL_ABSENCE_GPU_VALID | NRI_SPATIAL_ABSENCE_GPU_COMPLETE;
		constexpr uint32_t requiredFootprintFlags =
			requiredBaseFlags | NRI_SPATIAL_ABSENCE_GPU_FOOTPRINT;
		constexpr uint32_t requiredGridFlags = requiredBaseFlags | NRI_SPATIAL_ABSENCE_GPU_GRID;
		constexpr uint32_t requiredChunkFlags =
			requiredBaseFlags | NRI_SPATIAL_ABSENCE_GPU_CERTIFIED |
			NRI_SPATIAL_ABSENCE_GPU_FOOTPRINT | NRI_SPATIAL_ABSENCE_GPU_GRID;
		constexpr uint32_t requiredPairFlags =
			requiredBaseFlags | NRI_SPATIAL_ABSENCE_GPU_CERTIFIED | NRI_SPATIAL_ABSENCE_GPU_PAIR;
		constexpr uint32_t requiredCellFlags =
			requiredBaseFlags | NRI_SPATIAL_ABSENCE_GPU_GRID | NRI_SPATIAL_ABSENCE_GPU_GRID_CELL;
		constexpr uint32_t requiredReferenceFlags =
			requiredBaseFlags | NRI_SPATIAL_ABSENCE_GPU_GRID | NRI_SPATIAL_ABSENCE_GPU_GRID_REFERENCE;

		const NRISpatialAbsenceGpuRecord& header = records[0];
		uint32_t chunkCount = 0;
		uint32_t certifiedCount = 0;
		uint32_t authorizedPairCount = 0;
		uint32_t footprintTriangleCount = 0;
		uint32_t footprintGridCellCount = 0;
		if ((header.flags & requiredBaseFlags) != requiredBaseFlags ||
			header.data0 != snapshot.frameIndex ||
			((uint64_t)header.data1 | ((uint64_t)header.data2 << 32u)) != snapshot.worldGeneration ||
			!std::isfinite(header.payload[0]) || !std::isfinite(header.payload[1]) ||
			!std::isfinite(header.payload[2]) || !std::isfinite(header.payload[3]) ||
			header.payload[3] <= 0.0f ||
			!DecodeSerializedUint(header.payload[4], chunkCount) ||
			!DecodeSerializedUint(header.payload[5], certifiedCount) ||
			!DecodeSerializedUint(header.payload[6], authorizedPairCount) ||
			!DecodeSerializedUint(header.payload[7], footprintTriangleCount) ||
			!DecodeSerializedUint(header.payload[8], footprintGridCellCount) ||
			chunkCount == 0u || !SerializedRangeFits(1u, chunkCount, records.size()) ||
			certifiedCount != snapshot.certifiedCount ||
			authorizedPairCount != snapshot.authorizedPairCount ||
			footprintTriangleCount != snapshot.footprintTriangleCount ||
			footprintGridCellCount != snapshot.footprintGridCellCount)
		{
			return false;
		}

		const size_t expectedNegativeWordCount = std::max<size_t>((chunkCount + 31u) / 32u, 1u);
		if (snapshot.negativeChunkWords.size() != expectedNegativeWordCount ||
			snapshot.reachedChunkWords.size() != expectedNegativeWordCount ||
			snapshot.selections.size() != certifiedCount)
		{
			return false;
		}

		std::vector<uint8_t> selectedNegativeChunks(chunkCount, 0u);
		uint32_t selectedNegativeCount = 0u;
		for (size_t wordIndex = 0; wordIndex < snapshot.negativeChunkWords.size(); ++wordIndex)
		{
			uint32_t bits = snapshot.negativeChunkWords[wordIndex];
			for (uint32_t bitIndex = 0; bitIndex < 32u; ++bitIndex)
			{
				if ((bits & (1u << bitIndex)) == 0u)
					continue;
				const uint64_t chunkIndex = (uint64_t)wordIndex * 32u + bitIndex;
				if (chunkIndex >= chunkCount)
					return false;
				selectedNegativeChunks[(size_t)chunkIndex] = 1u;
				selectedNegativeCount++;
			}
		}
		if (selectedNegativeCount != certifiedCount)
			return false;

		std::vector<uint8_t> reachedChunks(chunkCount, 0u);
		for (size_t wordIndex = 0; wordIndex < snapshot.reachedChunkWords.size(); ++wordIndex)
		{
			uint32_t bits = snapshot.reachedChunkWords[wordIndex];
			for (uint32_t bitIndex = 0; bitIndex < 32u; ++bitIndex)
			{
				if ((bits & (1u << bitIndex)) == 0u)
					continue;
				const uint64_t chunkIndex = (uint64_t)wordIndex * 32u + bitIndex;
				if (chunkIndex >= chunkCount || selectedNegativeChunks[(size_t)chunkIndex] != 0u)
					return false;
				reachedChunks[(size_t)chunkIndex] = 1u;
			}
		}

		std::vector<uint8_t> selectionSeen(chunkCount, 0u);
		for (const NRISpatialAbsenceSelectionRecord& selection : snapshot.selections)
		{
			if (selection.negativeChunk >= chunkCount ||
				selectedNegativeChunks[selection.negativeChunk] == 0u ||
				selectionSeen[selection.negativeChunk] != 0u)
			{
				return false;
			}
			selectionSeen[selection.negativeChunk] = 1u;
		}

		enum SerializedRecordRole : uint8_t
		{
			SerializedRoleNone = 0,
			SerializedRoleLookup,
			SerializedRolePair,
			SerializedRoleTriangle,
			SerializedRoleGrid,
			SerializedRoleCell,
			SerializedRoleReference,
		};
		std::vector<uint8_t> recordRoles(records.size(), SerializedRoleNone);
		for (uint32_t recordIndex = 0; recordIndex <= chunkCount; ++recordIndex)
			recordRoles[recordIndex] = SerializedRoleLookup;
		auto claimRecord = [&](uint32_t recordIndex, SerializedRecordRole role)
		{
			if (recordIndex >= records.size() || recordRoles[recordIndex] != SerializedRoleNone)
				return false;
			recordRoles[recordIndex] = role;
			return true;
		};

		uint64_t validatedTriangleCount = 0u;
		uint64_t validatedGridCellCount = 0u;
		std::vector<uint8_t> footprintValidated(chunkCount, 0u);
		auto validateFootprint = [&](uint32_t ownerChunk)
		{
			if (ownerChunk >= chunkCount)
				return false;
			if (footprintValidated[ownerChunk] != 0u)
				return true;

			const NRISpatialAbsenceGpuRecord& lookup = records[(size_t)ownerChunk + 1u];
			uint32_t gridHeaderIndex = 0u;
			if ((lookup.flags & (requiredFootprintFlags | NRI_SPATIAL_ABSENCE_GPU_GRID)) !=
					(requiredFootprintFlags | NRI_SPATIAL_ABSENCE_GPU_GRID) ||
				lookup.data1 == 0u || lookup.data0 <= chunkCount ||
				!SerializedRangeFits(lookup.data0, lookup.data1, records.size()) ||
				!DecodeSerializedUint(lookup.payload[9], gridHeaderIndex) ||
				gridHeaderIndex <= chunkCount || gridHeaderIndex >= records.size())
			{
				return false;
			}

			for (uint32_t triangleOffset = 0; triangleOffset < lookup.data1; ++triangleOffset)
			{
				const uint32_t triangleIndex = lookup.data0 + triangleOffset;
				if (!claimRecord(triangleIndex, SerializedRoleTriangle))
					return false;
				const NRISpatialAbsenceGpuRecord& triangle = records[triangleIndex];
				if ((triangle.flags & requiredFootprintFlags) != requiredFootprintFlags ||
					triangle.data0 != ownerChunk ||
					!std::isfinite(triangle.payload[0]) || !std::isfinite(triangle.payload[1]) ||
					!std::isfinite(triangle.payload[2]) || !std::isfinite(triangle.payload[3]) ||
					!std::isfinite(triangle.payload[4]) || !std::isfinite(triangle.payload[5]))
				{
					return false;
				}
			}

			if (!claimRecord(gridHeaderIndex, SerializedRoleGrid))
				return false;
			const NRISpatialAbsenceGpuRecord& grid = records[gridHeaderIndex];
			uint32_t width = 0u;
			uint32_t height = 0u;
			uint32_t cellCount = 0u;
			uint32_t referenceCount = 0u;
			uint32_t referenceRecordCount = 0u;
			if ((grid.flags & requiredGridFlags) != requiredGridFlags || grid.data0 != ownerChunk ||
				!std::isfinite(grid.payload[0]) || !std::isfinite(grid.payload[1]) ||
				!std::isfinite(grid.payload[2]) || !std::isfinite(grid.payload[3]) ||
				grid.payload[0] >= grid.payload[2] || grid.payload[1] >= grid.payload[3] ||
				!DecodeSerializedUint(grid.payload[4], width) ||
				!DecodeSerializedUint(grid.payload[5], height) ||
				!DecodeSerializedUint(grid.payload[6], cellCount) ||
				!DecodeSerializedUint(grid.payload[7], referenceCount) ||
				!DecodeSerializedUint(grid.payload[8], referenceRecordCount) ||
				width == 0u || height == 0u || width > kFootprintGridMaxDimension ||
				height > kFootprintGridMaxDimension || cellCount != width * height ||
				grid.data1 != gridHeaderIndex + 1u ||
				!SerializedRangeFits(grid.data1, cellCount, records.size()) ||
				grid.data2 != grid.data1 + cellCount ||
				!SerializedRangeFits(grid.data2, referenceRecordCount, records.size()))
			{
				return false;
			}

			uint64_t nextReferenceRecord = grid.data2;
			uint64_t accumulatedReferences = 0u;
			for (uint32_t cellOffset = 0; cellOffset < cellCount; ++cellOffset)
			{
				const uint32_t cellIndex = grid.data1 + cellOffset;
				if (!claimRecord(cellIndex, SerializedRoleCell))
					return false;
				const NRISpatialAbsenceGpuRecord& cell = records[cellIndex];
				uint32_t storedCellOffset = 0u;
				uint32_t cellReferenceRecordCount = 0u;
				const uint32_t expectedCellReferenceRecordCount = (cell.data2 + 2u) / 3u;
				if ((cell.flags & requiredCellFlags) != requiredCellFlags || cell.data0 != ownerChunk ||
					!DecodeSerializedUint(cell.payload[0], storedCellOffset) || storedCellOffset != cellOffset ||
					!DecodeSerializedUint(cell.payload[1], cellReferenceRecordCount) ||
					cellReferenceRecordCount != expectedCellReferenceRecordCount ||
					cell.data2 > referenceCount || cell.data1 != nextReferenceRecord ||
					!SerializedRangeFits(cell.data1, cellReferenceRecordCount, records.size()) ||
					(uint64_t)cell.data1 + cellReferenceRecordCount >
						(uint64_t)grid.data2 + referenceRecordCount)
				{
					return false;
				}

				if ((cell.flags & NRI_SPATIAL_ABSENCE_GPU_GRID_CELL_INTERIOR) != 0u)
				{
					uint32_t certificateTriangle = 0u;
					if (!DecodeSerializedUint(cell.payload[2], certificateTriangle) ||
						certificateTriangle >= lookup.data1)
					{
						return false;
					}
				}

				for (uint32_t referenceOffset = 0; referenceOffset < cellReferenceRecordCount; ++referenceOffset)
				{
					const uint32_t referenceIndex = cell.data1 + referenceOffset;
					if (!claimRecord(referenceIndex, SerializedRoleReference))
						return false;
					const NRISpatialAbsenceGpuRecord& reference = records[referenceIndex];
					uint32_t referenceOwner = 0u;
					uint32_t storedReferenceOffset = 0u;
					const uint32_t consumedReferences = referenceOffset * 3u;
					const uint32_t tailCount = std::min(cell.data2 - consumedReferences, 3u);
					const bool tailValid = tailCount == 3u ||
						(tailCount == 2u && reference.data2 == UINT32_MAX) ||
						(tailCount == 1u && reference.data1 == UINT32_MAX && reference.data2 == UINT32_MAX);
					if ((reference.flags & requiredReferenceFlags) != requiredReferenceFlags ||
						!DecodeSerializedUint(reference.payload[0], referenceOwner) ||
						referenceOwner != ownerChunk ||
						!DecodeSerializedUint(reference.payload[1], storedReferenceOffset) ||
						storedReferenceOffset != referenceOffset || !tailValid)
					{
						return false;
					}
					const uint32_t triangleOffsets[3] = { reference.data0, reference.data1, reference.data2 };
					for (uint32_t lane = 0; lane < tailCount; ++lane)
					{
						if (triangleOffsets[lane] >= lookup.data1)
							return false;
					}
				}

				nextReferenceRecord += cellReferenceRecordCount;
				accumulatedReferences += cell.data2;
			}
			if (nextReferenceRecord != (uint64_t)grid.data2 + referenceRecordCount ||
				accumulatedReferences != referenceCount)
			{
				return false;
			}

			validatedTriangleCount += lookup.data1;
			validatedGridCellCount += cellCount;
			footprintValidated[ownerChunk] = 1u;
			return true;
		};

		uint64_t validatedPairCount = 0u;
		for (uint32_t negativeChunk = 0; negativeChunk < chunkCount; ++negativeChunk)
		{
			const NRISpatialAbsenceGpuRecord& chunk = records[(size_t)negativeChunk + 1u];
			const bool selected = selectedNegativeChunks[negativeChunk] != 0u;
			const bool reached = reachedChunks[negativeChunk] != 0u;
			if (((chunk.flags & NRI_SPATIAL_ABSENCE_GPU_CERTIFIED) != 0u) != selected)
				return false;
			if (((chunk.flags & NRI_SPATIAL_ABSENCE_GPU_REACHED) != 0u) != reached ||
				(reached && (chunk.flags & requiredBaseFlags) != requiredBaseFlags))
				return false;
			if (!selected)
				continue;

			uint32_t pairCount = 0u;
			if ((chunk.flags & requiredChunkFlags) != requiredChunkFlags ||
				!DecodeSerializedUint(chunk.payload[8], pairCount) || pairCount == 0u ||
				chunk.data2 <= chunkCount || !SerializedRangeFits(chunk.data2, pairCount, records.size()) ||
				!std::isfinite(chunk.payload[0]) || !std::isfinite(chunk.payload[1]) ||
				!std::isfinite(chunk.payload[2]) || !std::isfinite(chunk.payload[4]) ||
				!std::isfinite(chunk.payload[5]) || !std::isfinite(chunk.payload[6]) ||
				chunk.payload[0] > chunk.payload[4] || chunk.payload[1] > chunk.payload[5] ||
				chunk.payload[2] > chunk.payload[6] || !validateFootprint(negativeChunk))
			{
				return false;
			}

			for (uint32_t pairOffset = 0; pairOffset < pairCount; ++pairOffset)
			{
				const uint32_t pairIndex = chunk.data2 + pairOffset;
				if (!claimRecord(pairIndex, SerializedRolePair))
					return false;
				const NRISpatialAbsenceGpuRecord& pair = records[pairIndex];
				if ((pair.flags & requiredPairFlags) != requiredPairFlags ||
					pair.data0 != negativeChunk || pair.data1 >= chunkCount ||
					!std::isfinite(pair.payload[0]) || !std::isfinite(pair.payload[1]) ||
					!std::isfinite(pair.payload[2]) || !std::isfinite(pair.payload[4]) ||
					!std::isfinite(pair.payload[5]) || !std::isfinite(pair.payload[6]) ||
					pair.payload[0] > pair.payload[4] || pair.payload[1] > pair.payload[5] ||
					pair.payload[2] > pair.payload[6] || !validateFootprint(pair.data1))
				{
					return false;
				}
			}
			validatedPairCount += pairCount;
		}

		if (validatedPairCount != authorizedPairCount ||
			validatedTriangleCount != footprintTriangleCount ||
			validatedGridCellCount != footprintGridCellCount)
		{
			return false;
		}
		for (size_t recordIndex = (size_t)chunkCount + 1u; recordIndex < recordRoles.size(); ++recordIndex)
		{
			if (recordRoles[recordIndex] == SerializedRoleNone)
				return false;
		}
		return true;
	}

	bool SealSerializedSpatialAbsencePayload(NRISpatialAbsenceSnapshot& snapshot)
	{
		snapshot.failOpenFlags &= ~NRI_SPATIAL_ABSENCE_FAIL_GPU_PAYLOAD_INVALID;
		if (snapshot.gpuRecords.empty())
		{
			snapshot.failOpenFlags |= NRI_SPATIAL_ABSENCE_FAIL_GPU_PAYLOAD_INVALID;
			return false;
		}
		snapshot.gpuRecords[0].flags &= ~NRI_SPATIAL_ABSENCE_GPU_RAY_QUERY_VALIDATED;
		if (!ValidateSerializedSpatialAbsencePayload(snapshot))
		{
			snapshot.failOpenFlags |= NRI_SPATIAL_ABSENCE_FAIL_GPU_PAYLOAD_INVALID;
			return false;
		}
		snapshot.gpuRecords[0].flags |= NRI_SPATIAL_ABSENCE_GPU_RAY_QUERY_VALIDATED;
		return true;
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
			const bool presentPrevious = previousIndex < previous.size() && previous[previousIndex].key == key;
			uint32_t count = 1u;
			if (contextContinuous && presentPrevious)
				count = std::min(previous[previousIndex].consecutiveCaptureCount + 1u, 0xffffu);
			NRISpatialAbsenceContinuityRecord record;
			record.key = key;
			record.consecutiveCaptureCount = count;
			record.presentPrevious = presentPrevious;
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

	bool FootprintTriangleContainsGridCell(
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
		const float area = Cross2(triangle.point[0], triangle.point[1], triangle.point[2]);
		if (std::fabs(area) <= kAreaEpsilon) return false;
		const float orientation = area >= 0.0f ? 1.0f : -1.0f;
		const float corners[4][2] = {
			{ cellMin[0], cellMin[1] }, { cellMax[0], cellMin[1] },
			{ cellMin[0], cellMax[1] }, { cellMax[0], cellMax[1] }
		};
		for (const auto& corner : corners)
		{
			if (orientation * Cross2(triangle.point[0], triangle.point[1], corner) < -kFootprintPredicateEpsilon ||
				orientation * Cross2(triangle.point[1], triangle.point[2], corner) < -kFootprintPredicateEpsilon ||
				orientation * Cross2(triangle.point[2], triangle.point[0], corner) < -kFootprintPredicateEpsilon)
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
		grid.interiorTriangles.assign(grid.cells.size(), UINT32_MAX);
		for (uint32_t cellIndex = 0; cellIndex < grid.cells.size(); ++cellIndex)
		{
			const uint32_t cellX = cellIndex % grid.width;
			const uint32_t cellY = cellIndex / grid.width;
			for (uint32_t triangleIndex : grid.cells[cellIndex])
			{
				if (FootprintTriangleContainsGridCell(grid, triangles[triangleIndex], cellX, cellY))
				{
					grid.interiorTriangles[cellIndex] = triangleIndex;
					break;
				}
			}
		}
		return true;
	}

	bool ValidateFootprintGrid(const std::vector<Triangle2D>& triangles, const FootprintGrid& grid)
	{
		if (triangles.empty() || grid.width == 0u || grid.height == 0u ||
			grid.width > kFootprintGridMaxDimension || grid.height > kFootprintGridMaxDimension ||
			grid.cells.size() != (size_t)grid.width * grid.height ||
			grid.interiorTriangles.size() != grid.cells.size() ||
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
			if (grid.interiorTriangles[cellIndex] != UINT32_MAX)
			{
				const uint32_t x = cellIndex % grid.width;
				const uint32_t y = cellIndex / grid.width;
				const uint32_t triangleIndex = grid.interiorTriangles[cellIndex];
				if (triangleIndex >= triangles.size() ||
					!std::binary_search(references.begin(), references.end(), triangleIndex) ||
					!FootprintTriangleContainsGridCell(grid, triangles[triangleIndex], x, y)) return false;
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

struct NRISpatialAbsenceGate::TopologyCache
{
	uint64_t worldGeneration = 0;
	uint64_t topologyRevision = 0;
	size_t chunkCount = 0;
	std::vector<CachedChunkTopology> chunks;
	std::vector<CachedPairTopology> pairs;
};

NRISpatialAbsenceGate::NRISpatialAbsenceGate()
	: mTopologyCache(std::make_unique<TopologyCache>())
{
}

NRISpatialAbsenceGate::~NRISpatialAbsenceGate() = default;

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
	auto rootSectorIndices = std::move(mSnapshot.rootSectorIndices);
	auto reachedSectorIndices = std::move(mSnapshot.reachedSectorIndices);
	auto reachedWallIndices = std::move(mSnapshot.reachedWallIndices);
	auto negativeChunkWords = std::move(mSnapshot.negativeChunkWords);
	auto reachedChunkWords = std::move(mSnapshot.reachedChunkWords);
	auto gpuRecords = std::move(mSnapshot.gpuRecords);
	auto conflicts = std::move(mSnapshot.conflicts);
	auto selections = std::move(mSnapshot.selections);
	mSnapshot = {};
	mSnapshot.rootSectorIndices = std::move(rootSectorIndices);
	mSnapshot.reachedSectorIndices = std::move(reachedSectorIndices);
	mSnapshot.reachedWallIndices = std::move(reachedWallIndices);
	mSnapshot.negativeChunkWords = std::move(negativeChunkWords);
	mSnapshot.reachedChunkWords = std::move(reachedChunkWords);
	mSnapshot.gpuRecords = std::move(gpuRecords);
	mSnapshot.conflicts = std::move(conflicts);
	mSnapshot.selections = std::move(selections);
	mSnapshot.rootSectorIndices.clear();
	mSnapshot.reachedSectorIndices.clear();
	mSnapshot.reachedWallIndices.clear();
	mSnapshot.negativeChunkWords.clear();
	mSnapshot.reachedChunkWords.clear();
	mSnapshot.gpuRecords.clear();
	mSnapshot.conflicts.clear();
	mSnapshot.selections.clear();
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
	const auto buildStarted = std::chrono::steady_clock::now();
	bool topologyCacheHit = false;
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
		mSnapshot.topologyCacheHit = topologyCacheHit;
		mSnapshot.buildElapsedMilliseconds = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - buildStarted).count();
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
	// All serialized record indices must remain exactly representable by the
	// float fields in the shared CPU/GPU contract. Reject pathological worlds
	// before any chunk-sized allocation or O(n^2) conflict enumeration.
	if (mapWorld.chunks.size() >= kMaxFloatExactInteger)
	{
		mSnapshot.failOpenFlags |= NRI_SPATIAL_ABSENCE_FAIL_GPU_INDEX_OVERFLOW;
		mStableWorldGeneration = 0;
		mStableObservationHash = 0;
		mStableCaptureCount = 0;
		mConflictContinuity.clear();
		mPreviousContinuityCaptureSerial = 0;
		mContinuityWorldGeneration = 0;
		mContinuityRootLocalSpaceIndex = -1;
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
	std::vector<uint32_t> authoredClosureSectors = input.authoredClosureSectorIndices;
	std::sort(reached.begin(), reached.end());
	reached.erase(std::unique(reached.begin(), reached.end()), reached.end());
	mSnapshot.reachedSectorIndices = reached;
	mSnapshot.reachedWallIndices = input.reachedWallIndices;
	std::sort(mSnapshot.reachedWallIndices.begin(), mSnapshot.reachedWallIndices.end());
	mSnapshot.reachedWallIndices.erase(
		std::unique(mSnapshot.reachedWallIndices.begin(), mSnapshot.reachedWallIndices.end()),
		mSnapshot.reachedWallIndices.end());
	std::sort(uncertain.begin(), uncertain.end());
	uncertain.erase(std::unique(uncertain.begin(), uncertain.end()), uncertain.end());
	std::sort(uncertainChunks.begin(), uncertainChunks.end());
	uncertainChunks.erase(std::unique(uncertainChunks.begin(), uncertainChunks.end()), uncertainChunks.end());
	std::sort(authoredClosureSectors.begin(), authoredClosureSectors.end());
	authoredClosureSectors.erase(
		std::unique(authoredClosureSectors.begin(), authoredClosureSectors.end()),
		authoredClosureSectors.end());

	std::vector<CertifiedPair> certifiedPairs;
	const bool rebuildTopology = mTopologyCache->worldGeneration != input.worldGeneration ||
		mTopologyCache->topologyRevision != mapWorld.topologyRevision ||
		mTopologyCache->chunkCount != mapWorld.chunks.size();
	if (rebuildTopology)
	{
		mTopologyCache->worldGeneration = input.worldGeneration;
		mTopologyCache->topologyRevision = mapWorld.topologyRevision;
		mTopologyCache->chunkCount = mapWorld.chunks.size();
		mTopologyCache->chunks.assign(mapWorld.chunks.size(), {});
		mTopologyCache->pairs.clear();
		const float topologyCenter[3] = {};
		for (size_t firstIndex = 0; firstIndex < mapWorld.chunks.size(); ++firstIndex)
		{
			const nri_scene::PTMapChunk& first = mapWorld.chunks[firstIndex];
			if (!first.bounds.valid || !IsFinite3(first.bounds.min) || !IsFinite3(first.bounds.max))
				continue;
			for (size_t secondIndex = firstIndex + 1u; secondIndex < mapWorld.chunks.size(); ++secondIndex)
			{
				const nri_scene::PTMapChunk& second = mapWorld.chunks[secondIndex];
				if (!second.bounds.valid || !IsFinite3(second.bounds.min) || !IsFinite3(second.bounds.max))
					continue;
				PairClassification bounds = ClassifyBounds(
					first.bounds, second.bounds, topologyCenter, std::numeric_limits<float>::max());
				if (bounds.decision == NRISpatialAbsenceConflictDecision::Disjoint)
					continue;
				CachedPairTopology pair;
				pair.firstIndex = firstIndex;
				pair.secondIndex = secondIndex;
				pair.bounds = bounds;
				mTopologyCache->pairs.push_back(std::move(pair));
			}
		}
	}
	topologyCacheHit = !rebuildTopology;
	mSnapshot.topologyPairCount = (uint32_t)std::min<size_t>(
		mTopologyCache->pairs.size(), std::numeric_limits<uint32_t>::max());

	auto ensureChunkBoundary = [&](size_t chunkIndex) -> CachedChunkTopology&
	{
		CachedChunkTopology& cached = mTopologyCache->chunks[chunkIndex];
		if (!cached.boundaryResolved)
		{
			topologyCacheHit = false;
			cached.openBoundary = HasOpenPlaneBoundary(mapWorld, mapWorld.chunks[chunkIndex]);
			cached.boundaryResolved = true;
		}
		return cached;
	};

	auto ensureChunkTopology = [&](size_t chunkIndex) -> CachedChunkTopology&
	{
		CachedChunkTopology& cached = mTopologyCache->chunks[chunkIndex];
		if (!cached.resolved)
		{
			topologyCacheHit = false;
			cached.flatClosed = HasFlatClosedVolume(mapWorld, mapWorld.chunks[chunkIndex]);
			cached.footprint = CollectChunkFootprintTriangles(mapWorld, mapWorld.chunks[chunkIndex]);
			cached.gridValid = !cached.footprint.empty() &&
				BuildFootprintGrid(cached.footprint, cached.grid) &&
				ValidateFootprintGrid(cached.footprint, cached.grid);
			cached.resolved = true;
		}
		ensureChunkBoundary(chunkIndex);
		return cached;
	};

	for (CachedPairTopology& cachedPair : mTopologyCache->pairs)
	{
		const nri_scene::PTMapChunk& first = mapWorld.chunks[cachedPair.firstIndex];
		const nri_scene::PTMapChunk& second = mapWorld.chunks[cachedPair.secondIndex];
		PairClassification classification = cachedPair.bounds;
		if (classification.decision != NRISpatialAbsenceConflictDecision::BoundaryContact)
		{
			classification.distanceToCenter = DistanceToBounds(
				input.center, classification.overlapMin, classification.overlapMax);
			classification.decision = classification.distanceToCenter > input.guardRadius
				? NRISpatialAbsenceConflictDecision::OutsideGuard
				: NRISpatialAbsenceConflictDecision::Certified;
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
		int32_t topologyIntermediateSector = -1;
		int32_t collapsedPortalSector = -1;
		int32_t insetBoundaryChildSector = -1;

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
			else
			{
				topologyIntermediateSector = FindOrdinaryTopologyIntermediate(
					first.sectorIndex, second.sectorIndex);
				if (topologyIntermediateSector >= 0)
				{
					classification.decision = NRISpatialAbsenceConflictDecision::NearOrdinaryTopology;
				}
			}
		}
		uint32_t protectionFlags = NRI_SPATIAL_ABSENCE_PROTECTION_NONE;
		if (classification.decision == NRISpatialAbsenceConflictDecision::NearOrdinaryTopology)
		{
			protectionFlags |= NRI_SPATIAL_ABSENCE_PROTECTION_NEAR_ORDINARY_TOPOLOGY;
			mSnapshot.nearTopologyProtectedCount++;
		}
		uint32_t exactWitnessCount = 0;
		const bool protectionCandidate =
			classification.decision == NRISpatialAbsenceConflictDecision::Certified ||
			classification.decision == NRISpatialAbsenceConflictDecision::NearOrdinaryTopology;
		if (protectionCandidate)
		{
			// Orientation is resolved above. Only a census-negative chunk whose own
			// emitted wall band seals an authored closure may fail open here; a
			// nearby closure on the positive side is not negative-volume evidence.
			collapsedPortalSector = FindProtectedSealingCarrier(
				mapWorld, *negative, authoredClosureSectors);
			if (collapsedPortalSector >= 0)
			{
				protectionFlags |= NRI_SPATIAL_ABSENCE_PROTECTION_COLLAPSED_PORTAL_ENVELOPE;
				mSnapshot.collapsedPortalProtectedCount++;
			}
			insetBoundaryChildSector = FindProtectedInsetBoundaryEnclosure(mapWorld, *negative);
			if (insetBoundaryChildSector >= 0)
			{
				protectionFlags |= NRI_SPATIAL_ABSENCE_PROTECTION_INSET_BOUNDARY_ENCLOSURE;
				mSnapshot.insetBoundaryEnclosureProtectedCount++;
			}
			const CachedChunkTopology& firstTopology = ensureChunkBoundary(cachedPair.firstIndex);
			const CachedChunkTopology& secondTopology = ensureChunkBoundary(cachedPair.secondIndex);
			if (firstTopology.openBoundary || secondTopology.openBoundary)
			{
				protectionFlags |= NRI_SPATIAL_ABSENCE_PROTECTION_OPEN_BOUNDARY;
				mSnapshot.openBoundaryProtectedCount++;
			}
			if (classification.decision == NRISpatialAbsenceConflictDecision::Certified)
			{
				if (collapsedPortalSector >= 0)
					classification.decision = NRISpatialAbsenceConflictDecision::CollapsedPortalEnvelope;
				else if (insetBoundaryChildSector >= 0)
					classification.decision = NRISpatialAbsenceConflictDecision::InsetBoundaryEnclosure;
				else if ((protectionFlags & NRI_SPATIAL_ABSENCE_PROTECTION_OPEN_BOUNDARY) != 0)
					classification.decision = NRISpatialAbsenceConflictDecision::OpenBoundary;
			}
		}
		if (classification.decision == NRISpatialAbsenceConflictDecision::Certified)
		{
			if (!cachedPair.exactWitnessResolved)
			{
				topologyCacheHit = false;
				const CachedChunkTopology& firstTopology = ensureChunkTopology(cachedPair.firstIndex);
				const CachedChunkTopology& secondTopology = ensureChunkTopology(cachedPair.secondIndex);
				if (firstTopology.flatClosed && secondTopology.flatClosed &&
					std::min(first.bounds.max[1], second.bounds.max[1]) -
					std::max(first.bounds.min[1], second.bounds.min[1]) > kBoundsEpsilon)
				{
					for (const Triangle2D& firstTriangle : firstTopology.footprint)
						for (const Triangle2D& secondTriangle : secondTopology.footprint)
							cachedPair.exactWitnessCount += TriangleIntersectionArea(
								firstTriangle, secondTriangle) > kAreaEpsilon ? 1u : 0u;
				}
				cachedPair.exactWitnessResolved = true;
			}
			exactWitnessCount = cachedPair.exactWitnessCount;
			if (exactWitnessCount == 0)
				classification.decision = NRISpatialAbsenceConflictDecision::ExactOverlapMissing;
		}

		mSnapshot.conflicts.push_back(MakeDebugRecord(*positive, *negative, classification));
		const size_t debugRecordIndex = mSnapshot.conflicts.size() - 1u;
		mSnapshot.conflicts.back().exactWitnessCount = exactWitnessCount;
		mSnapshot.conflicts.back().protectionFlags = protectionFlags;
		mSnapshot.conflicts.back().topologyIntermediateSector = topologyIntermediateSector;
		mSnapshot.conflicts.back().collapsedPortalSector = collapsedPortalSector;
		mSnapshot.conflicts.back().insetBoundaryChildSector = insetBoundaryChildSector;
		if (classification.decision != NRISpatialAbsenceConflictDecision::Certified)
			continue;

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
		pair.debugRecordIndex = debugRecordIndex;
		certifiedPairs.push_back(pair);
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
		if (certifiedPairs[index].debugRecordIndex < mSnapshot.conflicts.size())
		{
			NRISpatialAbsenceConflictRecord& debug = mSnapshot.conflicts[certifiedPairs[index].debugRecordIndex];
			debug.continuityCount = mConflictContinuity[index].consecutiveCaptureCount;
			debug.continuityPresentPrevious = mConflictContinuity[index].presentPrevious;
			debug.continuityContextContinuous = contextContinuous;
			debug.continuityAuthorized = mConflictContinuity[index].authorized;
		}
		mSnapshot.authorizedPairCount += certifiedPairs[index].authorized ? 1u : 0u;
		mSnapshot.pendingPairCount += certifiedPairs[index].authorized ? 0u : 1u;
	}
	mPreviousContinuityCaptureSerial = input.captureSerial;
	mContinuityWorldGeneration = input.worldGeneration;
	mContinuityRootLocalSpaceIndex = rootLocalSpace;

	mSnapshot.gpuRecords.resize(mapWorld.chunks.size() + 1u);
	mSnapshot.negativeChunkWords.resize(std::max<size_t>((mapWorld.chunks.size() + 31u) / 32u, 1u), 0u);
	mSnapshot.reachedChunkWords.resize(std::max<size_t>((mapWorld.chunks.size() + 31u) / 32u, 1u), 0u);
	std::vector<const std::vector<Triangle2D>*> footprints(mapWorld.chunks.size(), nullptr);
	std::vector<const FootprintGrid*> footprintGrids(mapWorld.chunks.size(), nullptr);
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
		{
			const CachedChunkTopology& cached = ensureChunkTopology(chunkIndex);
			footprints[chunkIndex] = &cached.footprint;
			footprintGrids[chunkIndex] = cached.gridValid ? &cached.grid : nullptr;
		}
	}
	bool gridBuildFailed = false;
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
		if (chunkIndex >= footprints.size() || footprints[chunkIndex] == nullptr ||
			footprints[chunkIndex]->empty()) continue;
		if (footprintGrids[chunkIndex] == nullptr)
		{
			gridBuildFailed = true;
			break;
		}
		const FootprintGrid& grid = *footprintGrids[chunkIndex];
		size_t serializedReferenceRecordCount = 0u;
		for (size_t cellIndex = 0; cellIndex < grid.cells.size(); ++cellIndex)
		{
			if (grid.interiorTriangles[cellIndex] == UINT32_MAX)
				serializedReferenceRecordCount += (grid.cells[cellIndex].size() + 2u) / 3u;
		}
		addEstimatedRecords(footprints[chunkIndex]->size());
		addEstimatedRecords(1u + grid.cells.size() + serializedReferenceRecordCount);
	}
	if (gridBuildFailed)
	{
		mSnapshot.failOpenFlags |= NRI_SPATIAL_ABSENCE_FAIL_GPU_INDEX_OVERFLOW;
		mSnapshot.gpuRecords.assign(1u, {});
		mSnapshot.negativeChunkWords.clear();
		mSnapshot.reachedChunkWords.clear();
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
		if (authorizedIndices.empty() || negativeChunk >= mapWorld.chunks.size() ||
			footprints[negativeChunk] == nullptr || footprints[negativeChunk]->empty())
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
			if (positiveChunk >= mapWorld.chunks.size() || footprints[positiveChunk] == nullptr ||
				footprints[positiveChunk]->empty())
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
			selection.footprintTriangleCount += (uint32_t)footprints[positiveChunk]->size();
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
		selection.footprintTriangleCount += (uint32_t)footprints[negativeChunk]->size();
		for (const Triangle2D& triangle : *footprints[negativeChunk])
			representationHash = HashBytes(representationHash, &triangle, sizeof(triangle));
		for (uint32_t owner : owners)
			for (const Triangle2D& triangle : *footprints[owner])
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
		if (chunkIndex >= mapWorld.chunks.size() || footprints[chunkIndex] == nullptr ||
			footprints[chunkIndex]->empty() || footprintGrids[chunkIndex] == nullptr) continue;
		const uint32_t triangleStart = (uint32_t)mSnapshot.gpuRecords.size();
		for (const Triangle2D& triangle : *footprints[chunkIndex])
		{
			NRISpatialAbsenceGpuRecord triangleRecord;
			triangleRecord.flags = NRI_SPATIAL_ABSENCE_GPU_VALID | NRI_SPATIAL_ABSENCE_GPU_COMPLETE |
				NRI_SPATIAL_ABSENCE_GPU_FOOTPRINT;
			triangleRecord.data0 = chunkIndex;
			std::memcpy(triangleRecord.payload, triangle.point, sizeof(triangle.point));
			mSnapshot.gpuRecords.push_back(triangleRecord);
		}
		const FootprintGrid& grid = *footprintGrids[chunkIndex];
		uint32_t serializedReferenceCount = 0u;
		uint32_t serializedReferenceRecordCount = 0u;
		for (size_t cellIndex = 0; cellIndex < grid.cells.size(); ++cellIndex)
		{
			if (grid.interiorTriangles[cellIndex] != UINT32_MAX) continue;
			serializedReferenceCount += (uint32_t)grid.cells[cellIndex].size();
			serializedReferenceRecordCount += (uint32_t)((grid.cells[cellIndex].size() + 2u) / 3u);
		}
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
		gridHeader.payload[7] = (float)serializedReferenceCount;
		gridHeader.payload[8] = (float)serializedReferenceRecordCount;
		mSnapshot.gpuRecords.push_back(gridHeader);
		uint32_t nextReferenceRecord = referenceRecordStart;
		for (uint32_t cellIndex = 0; cellIndex < grid.cells.size(); ++cellIndex)
		{
			const std::vector<uint32_t>& references = grid.cells[cellIndex];
			const bool interior = grid.interiorTriangles[cellIndex] != UINT32_MAX;
			NRISpatialAbsenceGpuRecord cellRecord;
			cellRecord.flags = NRI_SPATIAL_ABSENCE_GPU_VALID | NRI_SPATIAL_ABSENCE_GPU_COMPLETE |
				NRI_SPATIAL_ABSENCE_GPU_GRID | NRI_SPATIAL_ABSENCE_GPU_GRID_CELL;
			if (interior)
			{
				cellRecord.flags |= NRI_SPATIAL_ABSENCE_GPU_GRID_CELL_INTERIOR;
				cellRecord.payload[2] = (float)grid.interiorTriangles[cellIndex];
			}
			cellRecord.data0 = chunkIndex;
			cellRecord.data1 = nextReferenceRecord;
			cellRecord.data2 = interior ? 0u : (uint32_t)references.size();
			cellRecord.payload[0] = (float)cellIndex;
			cellRecord.payload[1] = (float)((cellRecord.data2 + 2u) / 3u);
			mSnapshot.gpuRecords.push_back(cellRecord);
			nextReferenceRecord += (cellRecord.data2 + 2u) / 3u;
		}
		for (size_t cellIndex = 0; cellIndex < grid.cells.size(); ++cellIndex)
		{
			if (grid.interiorTriangles[cellIndex] != UINT32_MAX) continue;
			const std::vector<uint32_t>& references = grid.cells[cellIndex];
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
		lookup.data1 = (uint32_t)footprints[chunkIndex]->size();
		lookup.payload[9] = (float)gridHeaderIndex;
		mSnapshot.footprintTriangleCount += lookup.data1;
		mSnapshot.footprintGridCellCount += (uint32_t)grid.cells.size();
		// Keep semantic coverage telemetry independent of the packed payload:
		// certified cells omit redundant references, but still represent the
		// same complete logical candidate set.
		mSnapshot.footprintGridReferenceCount += grid.referenceCount;
	}

	// Serialize positive census membership independently from certified-negative
	// overlap authority. Persistent-voxel hits use this compact lookup to form
	// the exact "ray observed AND drawlist absent" intersection without evicting
	// off-screen or merely occluded actors from the TLAS.
	for (uint32_t sectorIndex : reached)
	{
		if (sectorIndex >= mapWorld.sectorChunkLookup.size())
			continue;
		const uint32_t chunkIndex = mapWorld.sectorChunkLookup[sectorIndex];
		if (chunkIndex >= mapWorld.chunks.size())
			continue;
		mSnapshot.reachedChunkWords[chunkIndex / 32u] |= 1u << (chunkIndex % 32u);
		mSnapshot.gpuRecords[(size_t)chunkIndex + 1u].flags |=
			NRI_SPATIAL_ABSENCE_GPU_VALID |
			NRI_SPATIAL_ABSENCE_GPU_COMPLETE |
			NRI_SPATIAL_ABSENCE_GPU_REACHED;
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
	if (input.probeEnabled)
	{
		header.flags |= NRI_SPATIAL_ABSENCE_GPU_PROBE;
		header.payload[9] = input.probeOrigin[0];
		header.payload[10] = input.probeOrigin[1];
		header.payload[11] = input.probeOrigin[2];
		header.payload[12] = input.probeRadius;
		header.payload[13] = (float)input.probeExpectedChunk;
		static_assert(sizeof(float) == sizeof(uint32_t));
		std::memcpy(&header.payload[14], &input.probeTargetPixel, sizeof(input.probeTargetPixel));
		std::memcpy(&header.payload[15], &input.probeReferencePixel, sizeof(input.probeReferencePixel));
	}
	mSnapshot.valid = true;
	if (!SealSerializedSpatialAbsencePayload(mSnapshot))
	{
		mSnapshot.failOpenFlags |= NRI_SPATIAL_ABSENCE_FAIL_GPU_PAYLOAD_INVALID;
	}
	mSnapshot.payloadHash = HashBytes(kHashOffset, mSnapshot.gpuRecords.data(),
		mSnapshot.gpuRecords.size() * sizeof(NRISpatialAbsenceGpuRecord));
	return finalizeTelemetry();
}

uint32_t ApplyNRISpatialAbsenceRayQueryCandidateFlags(
	bool enabled,
	const NRISpatialAbsenceSnapshot& snapshot,
	std::vector<nri::TopLevelInstance>& tlasInstances,
	const std::vector<SceneInstanceData>& sceneInstances)
{
	if (!enabled || !snapshot.HasNegativeAuthority() ||
		tlasInstances.size() != sceneInstances.size())
	{
		return 0u;
	}

	uint32_t markedInstanceCount = 0u;
	for (size_t instanceIndex = 0; instanceIndex < sceneInstances.size(); ++instanceIndex)
	{
		const SceneInstanceData& sceneInstance = sceneInstances[instanceIndex];
		if (sceneInstance.dataSource != nri_diag::SceneDataSourceStatic)
		{
			continue;
		}

		const uint32_t chunkIndex = sceneInstance.metadata0;
		const size_t wordIndex = chunkIndex / 32u;
		if (wordIndex >= snapshot.negativeChunkWords.size() ||
			(snapshot.negativeChunkWords[wordIndex] & (1u << (chunkIndex % 32u))) == 0u)
		{
			continue;
		}

		tlasInstances[instanceIndex].flags = (nri::TopLevelInstanceBits)(
			(uint32_t)tlasInstances[instanceIndex].flags |
			(uint32_t)nri::TopLevelInstanceBits::FORCE_NON_OPAQUE);
		markedInstanceCount++;
	}

	return markedInstanceCount;
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

	auto makePlaneSurface = [](nri_scene::PTMapSurfaceKind kind, uint32_t materialFlags, float y)
	{
		nri_scene::PTMapSurface surface;
		surface.kind = kind;
		surface.surface.material.flags = materialFlags;
		nri_scene::CapturedVertex vertex = {};
		vertex.position[1] = y;
		surface.surface.vertices.push_back(vertex);
		return surface;
	};
	nri_scene::PTMapWorld volumeWorld;
	volumeWorld.surfaces.push_back(makePlaneSurface(
		nri_scene::PTMapSurfaceKind::Floor, nri_scene::MaterialFlag_Flat, -1.0f));
	volumeWorld.surfaces.push_back(makePlaneSurface(
		nri_scene::PTMapSurfaceKind::Ceiling, nri_scene::MaterialFlag_Flat, 1.0f));
	nri_scene::PTMapChunk volumeChunk;
	volumeChunk.firstSurface = 0u;
	volumeChunk.surfaceCount = 2u;
	if (!HasFlatClosedVolume(volumeWorld, volumeChunk) || HasOpenPlaneBoundary(volumeWorld, volumeChunk))
		return fail("ordinary horizontal floor and ceiling did not satisfy the closed-volume precondition");
	const uint32_t openPlaneFlags[] = {
		nri_scene::MaterialFlag_Sky,
		nri_scene::MaterialFlag_Portal,
		nri_scene::MaterialFlag_Mirror,
		nri_scene::MaterialFlag_PlainMirror,
	};
	for (size_t boundarySurfaceIndex = 0; boundarySurfaceIndex < volumeWorld.surfaces.size(); ++boundarySurfaceIndex)
	{
		for (uint32_t openPlaneFlag : openPlaneFlags)
		{
			volumeWorld.surfaces[boundarySurfaceIndex].surface.material.flags =
				nri_scene::MaterialFlag_Flat | openPlaneFlag;
			if (HasFlatClosedVolume(volumeWorld, volumeChunk) || !HasOpenPlaneBoundary(volumeWorld, volumeChunk))
				return fail("an open floor or ceiling material satisfied the closed-volume precondition");
			volumeWorld.surfaces[boundarySurfaceIndex].surface.material.flags = nri_scene::MaterialFlag_Flat;
		}
	}

	const std::vector<std::vector<int32_t>> ordinaryNeighbors = {
		{ 1 }, { 0, 2 }, { 1, 3 }, { 2 }, { 5 }, { 4 }, { 7, 8 }, { 6, 8 }, { 6, 7 }
	};
	auto visitOrdinaryNeighbors = [&](int32_t sourceSector, const auto& visit)
	{
		for (int32_t neighbor : ordinaryNeighbors[(size_t)sourceSector])
			visit(neighbor);
	};
	auto isOrdinarilyAdjacent = [&](int32_t sourceSector, int32_t targetSector)
	{
		const std::vector<int32_t>& neighbors = ordinaryNeighbors[(size_t)sourceSector];
		return std::find(neighbors.begin(), neighbors.end(), targetSector) != neighbors.end();
	};
	if (FindOrdinaryTopologyIntermediateImpl(
			0, 2, ordinaryNeighbors.size(), visitOrdinaryNeighbors, isOrdinarilyAdjacent) != 1)
	{
		return fail("a shared ordinary neighbor did not protect a two-hop sector pair");
	}
	const std::vector<std::vector<int32_t>> asymmetricNeighbors = { { 1 }, {}, { 1 } };
	auto visitAsymmetricNeighbors = [&](int32_t sourceSector, const auto& visit)
	{
		for (int32_t neighbor : asymmetricNeighbors[(size_t)sourceSector])
			visit(neighbor);
	};
	auto isAsymmetricallyAdjacent = [&](int32_t sourceSector, int32_t targetSector)
	{
		const std::vector<int32_t>& neighbors = asymmetricNeighbors[(size_t)sourceSector];
		return std::find(neighbors.begin(), neighbors.end(), targetSector) != neighbors.end();
	};
	if (FindOrdinaryTopologyIntermediateImpl(
			0, 2, asymmetricNeighbors.size(), visitAsymmetricNeighbors, isAsymmetricallyAdjacent) != 1)
	{
		return fail("an asymmetric ordinary second hop did not fail open as near topology");
	}
	if (FindOrdinaryTopologyIntermediateImpl(
			0, 1, ordinaryNeighbors.size(), visitOrdinaryNeighbors, isOrdinarilyAdjacent) >= 0 ||
		FindOrdinaryTopologyIntermediateImpl(
			6, 7, ordinaryNeighbors.size(), visitOrdinaryNeighbors, isOrdinarilyAdjacent) >= 0 ||
		FindOrdinaryTopologyIntermediateImpl(
			0, 3, ordinaryNeighbors.size(), visitOrdinaryNeighbors, isOrdinarilyAdjacent) >= 0 ||
		FindOrdinaryTopologyIntermediateImpl(
			2, 2, ordinaryNeighbors.size(), visitOrdinaryNeighbors, isOrdinarilyAdjacent) >= 0 ||
		FindOrdinaryTopologyIntermediateImpl(
			-1, 2, ordinaryNeighbors.size(), visitOrdinaryNeighbors, isOrdinarilyAdjacent) >= 0)
	{
		return fail("direct, same-sector, distant, or malformed topology was misclassified as a two-hop pair");
	}

	auto makeClosureStrip = [](
		int32_t sectorIndex,
		bool collapsed,
		int32_t firstNeighbor,
		int32_t secondNeighbor,
		double length,
		double thickness)
	{
		ClosureStripTopology topology;
		topology.sectorIndex = sectorIndex;
		topology.collapsed = collapsed;
		topology.edges = {
			{ { 0.0, 0.0 }, { length, 0.0 }, 10, firstNeighbor, true },
			{ { length, 0.0 }, { length, thickness }, 11, -1, false },
			{ { length, thickness }, { 0.0, thickness }, 12, secondNeighbor, true },
			{ { 0.0, thickness }, { 0.0, 0.0 }, 13, -1, false },
		};
		return topology;
	};
	const ClosureStripTopology sealingCarrier = makeClosureStrip(0, false, 1, 2, 128.0, 2.0);
	const ClosureStripTopology authoredClosure = makeClosureStrip(1, true, 0, 3, 128.0, 4.0);
	if (!IsProtectedSealingCarrierPair(sealingCarrier, authoredClosure, true, true))
		return fail("an authored double-strip sealing carrier was not protected");
	if (IsProtectedSealingCarrierPair(authoredClosure, sealingCarrier, true, true))
		return fail("a closure on the oriented positive side protected the negative certificate");
	if (IsProtectedSealingCarrierPair(sealingCarrier, authoredClosure, false, true) ||
		IsProtectedSealingCarrierPair(sealingCarrier, authoredClosure, true, false))
	{
		return fail("an unauthored closure or missing emitted N-to-D wall band was protected");
	}

	ClosureStripTopology malformedCarrier = sealingCarrier;
	malformedCarrier.collapsed = true;
	if (IsProtectedSealingCarrierPair(malformedCarrier, authoredClosure, true, true))
		return fail("a collapsed negative carrier was protected");
	ClosureStripTopology malformedClosure = authoredClosure;
	malformedClosure.collapsed = false;
	if (IsProtectedSealingCarrierPair(sealingCarrier, malformedClosure, true, true))
		return fail("a noncollapsed authored closure was protected");

	malformedCarrier = makeClosureStrip(0, false, 1, 2, 128.0, 9.0);
	if (IsProtectedSealingCarrierPair(malformedCarrier, authoredClosure, true, true))
		return fail("a carrier below the minimum strip aspect was protected");
	malformedClosure = makeClosureStrip(1, true, 0, 3, 128.0, 9.0);
	if (IsProtectedSealingCarrierPair(sealingCarrier, malformedClosure, true, true))
		return fail("a closure below the minimum strip aspect was protected");
	malformedCarrier = sealingCarrier;
	malformedCarrier.edges.push_back(
		{ { 0.0, 0.0 }, { 1.0, 1.0 }, 14, -1, false });
	if (IsProtectedSealingCarrierPair(malformedCarrier, authoredClosure, true, true))
		return fail("a non-quadrilateral carrier was protected");
	malformedCarrier = sealingCarrier;
	malformedCarrier.edges[2].nextSector = malformedCarrier.edges[0].nextSector;
	if (IsProtectedSealingCarrierPair(malformedCarrier, authoredClosure, true, true))
		return fail("a carrier without two distinct portal neighbors was protected");
	malformedCarrier = sealingCarrier;
	malformedCarrier.edges[1].nextSector = 4;
	malformedCarrier.edges[1].ordinaryPortal = false;
	if (IsProtectedSealingCarrierPair(malformedCarrier, authoredClosure, true, true))
		return fail("a linked nonordinary edge was accepted as an end cap");
	malformedCarrier = sealingCarrier;
	malformedCarrier.edges[2].end[1] += 1.0;
	if (IsProtectedSealingCarrierPair(malformedCarrier, authoredClosure, true, true))
		return fail("unequal nonparallel portal edges were accepted as an opposite pair");
	malformedClosure = authoredClosure;
	malformedClosure.edges[0].nextSector = 4;
	if (IsProtectedSealingCarrierPair(sealingCarrier, malformedClosure, true, true))
		return fail("a closure without reciprocal carrier topology was protected");

	auto makeInsetFloorSurface = []()
	{
		nri_scene::PTMapSurface surface;
		surface.kind = nri_scene::PTMapSurfaceKind::Floor;
		surface.chunkIndex = 4;
		surface.surface.provenance.sourceType = nri_scene::SurfaceSourceType::MapFloorSection;
		surface.surface.provenance.mapChunkIndex = 4;
		surface.surface.provenance.sectorIndex = 7;
		surface.surface.vertices.resize(3u);
		return surface;
	};
	nri_scene::PTMapWorld insetFloorWorld;
	insetFloorWorld.surfaces.push_back(makeInsetFloorSurface());
	nri_scene::PTMapChunk insetFloorChunk;
	insetFloorChunk.chunkIndex = 4;
	insetFloorChunk.sectorIndex = 7;
	insetFloorChunk.surfaceCount = 1u;
	if (!HasEmittedInsetFloorSection(insetFloorWorld, insetFloorChunk))
		return fail("an opaque owned map-floor section was not recognized as an inset seal");
	nri_scene::PTMapSurface insetCeilingSurface = makeInsetFloorSurface();
	insetCeilingSurface.kind = nri_scene::PTMapSurfaceKind::Ceiling;
	insetCeilingSurface.surface.provenance.sourceType =
		nri_scene::SurfaceSourceType::MapCeilingSection;
	insetFloorWorld.surfaces[0] = insetCeilingSurface;
	if (!HasEmittedInsetCeilingSection(insetFloorWorld, insetFloorChunk))
		return fail("an opaque owned map-ceiling section was not recognized as an inset owner");
	insetFloorWorld.surfaces[0] = makeInsetFloorSurface();
	auto rejectsInsetFloor = [&](const nri_scene::PTMapSurface& surface)
	{
		insetFloorWorld.surfaces[0] = surface;
		return !HasEmittedInsetFloorSection(insetFloorWorld, insetFloorChunk);
	};
	nri_scene::PTMapSurface malformedInsetFloor = makeInsetFloorSurface();
	malformedInsetFloor.kind = nri_scene::PTMapSurfaceKind::Ceiling;
	if (!rejectsInsetFloor(malformedInsetFloor))
		return fail("a ceiling surface was accepted as an inset floor seal");
	malformedInsetFloor = makeInsetFloorSurface();
	malformedInsetFloor.chunkIndex++;
	if (!rejectsInsetFloor(malformedInsetFloor))
		return fail("an inset floor owned by another surface chunk was protected");
	malformedInsetFloor = makeInsetFloorSurface();
	malformedInsetFloor.surface.provenance.sourceType = nri_scene::SurfaceSourceType::FloorFlat;
	if (!rejectsInsetFloor(malformedInsetFloor))
		return fail("a non-map floor source was accepted as an inset seal");
	malformedInsetFloor = makeInsetFloorSurface();
	malformedInsetFloor.surface.provenance.mapChunkIndex++;
	if (!rejectsInsetFloor(malformedInsetFloor))
		return fail("an inset floor with mismatched chunk provenance was protected");
	malformedInsetFloor = makeInsetFloorSurface();
	malformedInsetFloor.surface.provenance.sectorIndex++;
	if (!rejectsInsetFloor(malformedInsetFloor))
		return fail("an inset floor with mismatched sector provenance was protected");
	for (uint32_t openPlaneFlag : openPlaneFlags)
	{
		malformedInsetFloor = makeInsetFloorSurface();
		malformedInsetFloor.surface.material.flags = openPlaneFlag;
		if (!rejectsInsetFloor(malformedInsetFloor))
			return fail("an open floor material was accepted as an inset seal");
	}
	malformedInsetFloor = makeInsetFloorSurface();
	malformedInsetFloor.surface.vertices.resize(2u);
	if (!rejectsInsetFloor(malformedInsetFloor))
		return fail("an inset floor without emitted triangle geometry was protected");

	nri_scene::PTMapWorld insetLookupWorld;
	insetLookupWorld.chunks.resize(1u);
	insetLookupWorld.chunks[0].chunkIndex = 0u;
	insetLookupWorld.chunks[0].sectorIndex = 7;
	insetLookupWorld.chunks[0].localSpaceIndex = 3u;
	insetLookupWorld.sectorChunkLookup.assign(8u, UINT32_MAX);
	insetLookupWorld.sectorChunkLookup[7] = 0u;
	if (FindChunkForSector(insetLookupWorld, 7, 3u) != &insetLookupWorld.chunks[0] ||
		FindChunkForSector(insetLookupWorld, 7, 2u) != nullptr ||
		FindChunkForSector(insetLookupWorld, -1, 3u) != nullptr)
	{
		return fail("inset enclosure sector-to-chunk lookup validation failed");
	}
	insetLookupWorld.chunks[0].sectorIndex = 6;
	if (FindChunkForSector(insetLookupWorld, 7, 3u) != nullptr)
		return fail("an inset enclosure lookup accepted mismatched sector ownership");

	auto makeInsetBoundarySeal = [](uint32_t portalCount)
	{
		InsetBoundarySealTopology topology;
		topology.sectorIndex = 7;
		topology.flatPlanes = true;
		topology.sharesNeighborCeiling = true;
		topology.floorStrictlyInsideNeighbor = true;
		topology.emittedFloorSection = true;
		for (uint32_t portalIndex = 0; portalIndex < portalCount; ++portalIndex)
			topology.edges.push_back({ 9, true, false });
		topology.edges.push_back({ -1, false, true });
		return topology;
	};
	const InsetBoundarySealTopology fourWallInset = makeInsetBoundarySeal(3u);
	const InsetBoundarySealTopology splitSixWallInset = makeInsetBoundarySeal(5u);
	if (!IsInsetBoundarySeal(fourWallInset) ||
		FindInsetBoundarySealNeighbor(fourWallInset) != 9 ||
		!IsInsetBoundarySeal(splitSixWallInset))
	{
		return fail("four-wall or collinearly split raised-floor ceiling inset was not protected");
	}

	InsetBoundarySealTopology malformedInset = fourWallInset;
	malformedInset.sectorIndex = -1;
	if (IsInsetBoundarySeal(malformedInset))
		return fail("an inset with invalid sector identity was protected");
	malformedInset = fourWallInset;
	malformedInset.edges.resize(2u);
	if (IsInsetBoundarySeal(malformedInset))
		return fail("an inset with a malformed perimeter was protected");
	malformedInset = fourWallInset;
	malformedInset.flatPlanes = false;
	if (IsInsetBoundarySeal(malformedInset))
		return fail("a sloped inset boundary was protected");
	malformedInset = fourWallInset;
	malformedInset.collapsed = true;
	if (IsInsetBoundarySeal(malformedInset))
		return fail("a collapsed inset boundary was protected");
	malformedInset = fourWallInset;
	malformedInset.sharesNeighborCeiling = false;
	if (IsInsetBoundarySeal(malformedInset))
		return fail("an inset without a shared neighbor ceiling was protected");
	malformedInset = fourWallInset;
	malformedInset.floorStrictlyInsideNeighbor = false;
	if (IsInsetBoundarySeal(malformedInset))
		return fail("an inset floor outside the neighbor volume was protected");
	malformedInset = fourWallInset;
	malformedInset.emittedFloorSection = false;
	if (IsInsetBoundarySeal(malformedInset))
		return fail("an inset without an emitted owned floor section was protected");
	malformedInset = fourWallInset;
	malformedInset.edges[1].nextSector = 10;
	if (IsInsetBoundarySeal(malformedInset))
		return fail("an inset with multiple ordinary neighbors was protected");
	malformedInset = fourWallInset;
	malformedInset.edges.back() = { 9, true, false };
	if (IsInsetBoundarySeal(malformedInset))
		return fail("an inset without exactly one one-sided cap was protected");
	malformedInset = fourWallInset;
	malformedInset.edges[1] = { -1, false, true };
	if (IsInsetBoundarySeal(malformedInset))
		return fail("an inset with multiple one-sided caps was protected");
	malformedInset = fourWallInset;
	malformedInset.edges[1].ordinaryReciprocalPortal = false;
	if (IsInsetBoundarySeal(malformedInset))
		return fail("a linked nonordinary or nonreciprocal inset edge was protected");
	malformedInset = fourWallInset;
	malformedInset.edges[1].nextSector = malformedInset.sectorIndex;
	if (IsInsetBoundarySeal(malformedInset))
		return fail("a self-linked inset edge was protected");
	malformedInset = fourWallInset;
	malformedInset.edges.back() = { -1, false, false };
	if (IsInsetBoundarySeal(malformedInset))
		return fail("a malformed linked edge was accepted as a one-sided inset cap");
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
	std::vector<Triangle2D> certificateTriangles(9u,
		Triangle2D{ { { 0.0f, 0.0f }, { 100.0f, 0.0f }, { 0.0f, 100.0f } } });
	FootprintGrid certificateGrid;
	if (!BuildFootprintGrid(certificateTriangles, certificateGrid) ||
		!ValidateFootprintGrid(certificateTriangles, certificateGrid))
		return fail("interior-certificate footprint grid construction failed");
	FootprintGrid malformedGrid = certificateGrid;
	auto interiorCell = std::find_if(malformedGrid.interiorTriangles.begin(), malformedGrid.interiorTriangles.end(),
		[](uint32_t triangleIndex) { return triangleIndex != UINT32_MAX; });
	if (interiorCell == malformedGrid.interiorTriangles.end())
		return fail("footprint grid unexpectedly produced no exact interior certificate");
	*interiorCell = (uint32_t)certificateTriangles.size();
	if (ValidateFootprintGrid(certificateTriangles, malformedGrid))
		return fail("out-of-range footprint-grid interior certificate did not fail open");
	malformedGrid = manyGrid;
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
	if (continuity.size() != 1u || continuity[0].authorized || continuity[0].presentPrevious ||
		continuity[0].consecutiveCaptureCount != 1u)
		return fail("new conflict did not remain pending on its first complete observation");
	continuity = AdvanceConflictContinuity({ keyA }, continuity, true);
	if (!continuity[0].authorized || !continuity[0].presentPrevious ||
		continuity[0].consecutiveCaptureCount != 2u)
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
	semanticProbe.negativeChunkWords = { 1u << 7u };

	auto makeSealablePayload = []()
	{
		NRISpatialAbsenceSnapshot snapshot;
		snapshot.valid = true;
		snapshot.frameIndex = 17u;
		snapshot.worldGeneration = 29u;
		snapshot.certifiedCount = 1u;
		snapshot.authorizedPairCount = 1u;
		snapshot.footprintTriangleCount = 2u;
		snapshot.footprintGridCellCount = 2u;
		snapshot.footprintGridReferenceCount = 2u;
		snapshot.negativeChunkWords = { 1u << 1u };
		snapshot.reachedSectorIndices = { 0u };
		snapshot.reachedChunkWords = { 1u };
		NRISpatialAbsenceSelectionRecord selection;
		selection.negativeChunk = 1u;
		snapshot.selections.push_back(selection);
		snapshot.gpuRecords.resize(12u);

		NRISpatialAbsenceGpuRecord& header = snapshot.gpuRecords[0];
		header.flags = NRI_SPATIAL_ABSENCE_GPU_VALID | NRI_SPATIAL_ABSENCE_GPU_COMPLETE;
		header.data0 = snapshot.frameIndex;
		header.data1 = (uint32_t)snapshot.worldGeneration;
		header.data2 = (uint32_t)(snapshot.worldGeneration >> 32u);
		header.payload[3] = 8.0f;
		header.payload[4] = 2.0f;
		header.payload[5] = 1.0f;
		header.payload[6] = 1.0f;
		header.payload[7] = 2.0f;
		header.payload[8] = 2.0f;

		NRISpatialAbsenceGpuRecord& negative = snapshot.gpuRecords[2];
		negative.flags = NRI_SPATIAL_ABSENCE_GPU_VALID | NRI_SPATIAL_ABSENCE_GPU_COMPLETE |
			NRI_SPATIAL_ABSENCE_GPU_CERTIFIED;
		negative.data2 = 3u;
		negative.payload[4] = negative.payload[5] = negative.payload[6] = 1.0f;
		negative.payload[8] = 1.0f;
		snapshot.gpuRecords[1].flags = NRI_SPATIAL_ABSENCE_GPU_VALID |
			NRI_SPATIAL_ABSENCE_GPU_COMPLETE | NRI_SPATIAL_ABSENCE_GPU_REACHED;
		NRISpatialAbsenceGpuRecord& pair = snapshot.gpuRecords[3];
		pair.flags = NRI_SPATIAL_ABSENCE_GPU_VALID | NRI_SPATIAL_ABSENCE_GPU_COMPLETE |
			NRI_SPATIAL_ABSENCE_GPU_CERTIFIED | NRI_SPATIAL_ABSENCE_GPU_PAIR;
		pair.data0 = 1u;
		pair.data1 = 0u;
		pair.payload[4] = pair.payload[5] = pair.payload[6] = 1.0f;

		auto setFootprint = [&](uint32_t owner, uint32_t triangleIndex, uint32_t gridIndex,
			uint32_t cellIndex, uint32_t referenceIndex)
		{
			NRISpatialAbsenceGpuRecord& lookup = snapshot.gpuRecords[(size_t)owner + 1u];
			lookup.flags |= NRI_SPATIAL_ABSENCE_GPU_VALID | NRI_SPATIAL_ABSENCE_GPU_COMPLETE |
				NRI_SPATIAL_ABSENCE_GPU_FOOTPRINT | NRI_SPATIAL_ABSENCE_GPU_GRID;
			lookup.data0 = triangleIndex;
			lookup.data1 = 1u;
			lookup.payload[9] = (float)gridIndex;

			NRISpatialAbsenceGpuRecord& triangle = snapshot.gpuRecords[triangleIndex];
			triangle.flags = NRI_SPATIAL_ABSENCE_GPU_VALID | NRI_SPATIAL_ABSENCE_GPU_COMPLETE |
				NRI_SPATIAL_ABSENCE_GPU_FOOTPRINT;
			triangle.data0 = owner;
			triangle.payload[0] = 0.0f; triangle.payload[1] = 0.0f;
			triangle.payload[2] = 1.0f; triangle.payload[3] = 0.0f;
			triangle.payload[4] = 0.0f; triangle.payload[5] = 1.0f;

			NRISpatialAbsenceGpuRecord& grid = snapshot.gpuRecords[gridIndex];
			grid.flags = NRI_SPATIAL_ABSENCE_GPU_VALID | NRI_SPATIAL_ABSENCE_GPU_COMPLETE |
				NRI_SPATIAL_ABSENCE_GPU_GRID;
			grid.data0 = owner;
			grid.data1 = cellIndex;
			grid.data2 = referenceIndex;
			grid.payload[2] = grid.payload[3] = 1.0f;
			grid.payload[4] = grid.payload[5] = grid.payload[6] = 1.0f;
			grid.payload[7] = grid.payload[8] = 1.0f;

			NRISpatialAbsenceGpuRecord& cell = snapshot.gpuRecords[cellIndex];
			cell.flags = NRI_SPATIAL_ABSENCE_GPU_VALID | NRI_SPATIAL_ABSENCE_GPU_COMPLETE |
				NRI_SPATIAL_ABSENCE_GPU_GRID | NRI_SPATIAL_ABSENCE_GPU_GRID_CELL;
			cell.data0 = owner;
			cell.data1 = referenceIndex;
			cell.data2 = 1u;
			cell.payload[1] = 1.0f;

			NRISpatialAbsenceGpuRecord& reference = snapshot.gpuRecords[referenceIndex];
			reference.flags = NRI_SPATIAL_ABSENCE_GPU_VALID | NRI_SPATIAL_ABSENCE_GPU_COMPLETE |
				NRI_SPATIAL_ABSENCE_GPU_GRID | NRI_SPATIAL_ABSENCE_GPU_GRID_REFERENCE;
			reference.data0 = 0u;
			reference.data1 = UINT32_MAX;
			reference.data2 = UINT32_MAX;
			reference.payload[0] = (float)owner;
		};
		setFootprint(0u, 4u, 5u, 6u, 7u);
		setFootprint(1u, 8u, 9u, 10u, 11u);
		return snapshot;
	};

	NRISpatialAbsenceSnapshot sealablePayload = makeSealablePayload();
	if (!SealSerializedSpatialAbsencePayload(sealablePayload) ||
		!sealablePayload.HasNegativeAuthority())
	{
		return fail("complete serialized spatial-absence payload did not receive ray-query validation authority");
	}
	std::vector<nri::TopLevelInstance> candidateInstances(3u);
	for (nri::TopLevelInstance& instance : candidateInstances)
	{
		instance.flags = nri::TopLevelInstanceBits::TRIANGLE_CULL_DISABLE;
	}
	std::vector<SceneInstanceData> candidateSceneInstances(3u);
	candidateSceneInstances[0].dataSource = nri_diag::SceneDataSourceStatic;
	candidateSceneInstances[0].metadata0 = 1u;
	candidateSceneInstances[1].dataSource = nri_diag::SceneDataSourceStatic;
	candidateSceneInstances[1].metadata0 = 0u;
	candidateSceneInstances[2].dataSource = nri_diag::SceneDataSourceDynamic;
	candidateSceneInstances[2].metadata0 = 1u;
	if (ApplyNRISpatialAbsenceRayQueryCandidateFlags(
		true, sealablePayload, candidateInstances, candidateSceneInstances) != 1u ||
		((uint32_t)candidateInstances[0].flags & (uint32_t)nri::TopLevelInstanceBits::FORCE_NON_OPAQUE) == 0u ||
		((uint32_t)candidateInstances[0].flags & (uint32_t)nri::TopLevelInstanceBits::TRIANGLE_CULL_DISABLE) == 0u ||
		((uint32_t)candidateInstances[1].flags & (uint32_t)nri::TopLevelInstanceBits::FORCE_NON_OPAQUE) != 0u ||
		((uint32_t)candidateInstances[2].flags & (uint32_t)nri::TopLevelInstanceBits::FORCE_NON_OPAQUE) != 0u)
	{
		return fail("ray-query candidate flags did not select only the authorized negative static occurrence");
	}
	for (nri::TopLevelInstance& instance : candidateInstances)
	{
		instance.flags = nri::TopLevelInstanceBits::NONE;
	}
	if (ApplyNRISpatialAbsenceRayQueryCandidateFlags(
		false, sealablePayload, candidateInstances, candidateSceneInstances) != 0u ||
		((uint32_t)candidateInstances[0].flags & (uint32_t)nri::TopLevelInstanceBits::FORCE_NON_OPAQUE) != 0u)
	{
		return fail("disabled ray-query candidate flagging changed a TLAS occurrence");
	}
	NRISpatialAbsenceSnapshot candidateFailOpenSnapshot = sealablePayload;
	candidateFailOpenSnapshot.valid = false;
	if (ApplyNRISpatialAbsenceRayQueryCandidateFlags(
		true, candidateFailOpenSnapshot, candidateInstances, candidateSceneInstances) != 0u)
	{
		return fail("missing negative authority did not fail open during TLAS candidate flagging");
	}
	std::vector<SceneInstanceData> mismatchedCandidateSceneInstances(2u);
	if (ApplyNRISpatialAbsenceRayQueryCandidateFlags(
		true, sealablePayload, candidateInstances, mismatchedCandidateSceneInstances) != 0u)
	{
		return fail("mismatched TLAS and scene-record vectors did not fail open during candidate flagging");
	}
	NRISpatialAbsenceSnapshot malformedPayload = sealablePayload;
	malformedPayload.gpuRecords[0].payload[4] = 32.0f;
	if (SealSerializedSpatialAbsencePayload(malformedPayload) || malformedPayload.HasNegativeAuthority())
		return fail("out-of-range serialized chunk table did not fail open");
	malformedPayload = sealablePayload;
	malformedPayload.gpuRecords[2].data2 = (uint32_t)malformedPayload.gpuRecords.size();
	if (SealSerializedSpatialAbsencePayload(malformedPayload) || malformedPayload.HasNegativeAuthority())
		return fail("out-of-range serialized pair span did not fail open");
	malformedPayload = sealablePayload;
	malformedPayload.gpuRecords[1].data0 = (uint32_t)malformedPayload.gpuRecords.size();
	if (SealSerializedSpatialAbsencePayload(malformedPayload) || malformedPayload.HasNegativeAuthority())
		return fail("out-of-range serialized footprint span did not fail open");
	malformedPayload = sealablePayload;
	malformedPayload.gpuRecords[5].data2++;
	if (SealSerializedSpatialAbsencePayload(malformedPayload) || malformedPayload.HasNegativeAuthority())
		return fail("malformed serialized grid span did not fail open");
	malformedPayload = sealablePayload;
	malformedPayload.gpuRecords[6].data1 = (uint32_t)malformedPayload.gpuRecords.size();
	if (SealSerializedSpatialAbsencePayload(malformedPayload) || malformedPayload.HasNegativeAuthority())
		return fail("out-of-range serialized cell reference span did not fail open");
	malformedPayload = sealablePayload;
	malformedPayload.gpuRecords[7].data0 = 1u;
	if (SealSerializedSpatialAbsencePayload(malformedPayload) || malformedPayload.HasNegativeAuthority())
		return fail("out-of-range serialized triangle reference did not fail open");
	malformedPayload = sealablePayload;
	malformedPayload.gpuRecords[8].data0 = 0u;
	if (SealSerializedSpatialAbsencePayload(malformedPayload) || malformedPayload.HasNegativeAuthority())
		return fail("serialized footprint triangle owner mismatch did not fail open");
	malformedPayload = sealablePayload;
	malformedPayload.gpuRecords[3].data0 = 0u;
	if (SealSerializedSpatialAbsencePayload(malformedPayload) || malformedPayload.HasNegativeAuthority())
		return fail("serialized pair owner mismatch did not fail open");
	malformedPayload = sealablePayload;
	malformedPayload.gpuRecords[1].flags &= ~NRI_SPATIAL_ABSENCE_GPU_REACHED;
	if (SealSerializedSpatialAbsencePayload(malformedPayload) || malformedPayload.HasCensusAuthority())
		return fail("serialized reached-chunk membership mismatch did not fail open");

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
	stableWorld.sectorChunkLookup.push_back(0u);
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
		firstStable.topologyCacheHit ||
		firstStable.censusObservationHash != 0x1234 ||
		firstStable.previousCensusObservationHash != 0 ||
		firstStable.failOpenFlags != NRI_SPATIAL_ABSENCE_FAIL_NONE ||
		!firstStable.HasCensusAuthority() || firstStable.reachedChunkWords.size() != 1u ||
		(firstStable.reachedChunkWords[0] & 1u) == 0u ||
		firstStable.HasNegativeAuthority())
	{
		return fail("first complete capture did not remain pair-level fail-open");
	}
	stableInput.captureSerial++;
	stableInput.frameIndex++;
	const NRISpatialAbsenceSnapshot& secondStable = stableGate.Build(stableWorld, stableInput);
	if (secondStable.stableCaptureCount != 2u || !secondStable.valid || !secondStable.topologyCacheHit ||
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
	if (translatedStable.stableCaptureCount != 3u || !translatedStable.topologyCacheHit ||
		translatedStable.semanticHash != stableSemanticHash)
	{
		return fail("camera-only translation changed stable semantic telemetry");
	}
	stableInput.captureSerial++;
	stableInput.frameIndex++;
	stableWorld.topologyRevision++;
	const NRISpatialAbsenceSnapshot& rebuiltTopology = stableGate.Build(stableWorld, stableInput);
	if (rebuiltTopology.topologyCacheHit || rebuiltTopology.semanticHash != stableSemanticHash)
	{
		return fail("map topology replacement did not invalidate cached topology safely");
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
	case NRISpatialAbsenceConflictDecision::OpenBoundary: return "open-boundary";
	case NRISpatialAbsenceConflictDecision::NearOrdinaryTopology: return "near-ordinary-topology";
	case NRISpatialAbsenceConflictDecision::CollapsedPortalEnvelope: return "collapsed-portal-envelope";
	case NRISpatialAbsenceConflictDecision::InsetBoundaryEnclosure: return "inset-boundary-enclosure";
	default: return "unknown";
	}
}
