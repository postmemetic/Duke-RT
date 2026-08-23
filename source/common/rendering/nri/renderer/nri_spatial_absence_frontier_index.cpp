#include "nri_spatial_absence_frontier_index.h"

#include "build.h"
#include "gamefuncs.h"

#include <algorithm>
#include <cmath>

namespace
{
	constexpr double kBoundsEpsilon = 1.0e-3;

	struct FrontierWallLink
	{
		int32_t wallIndex = -1;
		int32_t sectorIndex = -1;
		int32_t reciprocalWallIndex = -1;
		int32_t nextSectorIndex = -1;
		double start[2] = {};
		double end[2] = {};
		bool ordinaryReciprocal = false;
	};

	struct ClosureEdge
	{
		double start[2] = {};
		double end[2] = {};
		int32_t wallIndex = -1;
		int32_t nextSector = -1;
		bool ordinaryPortal = false;
	};

	struct ClosureDescriptor
	{
		int32_t sectorIndex = -1;
		bool valid = false;
		int32_t portalNeighbors[2] = { -1, -1 };
		int32_t portalWalls[2] = { -1, -1 };
	};

	struct WallLinkFacts
	{
		int32_t wallIndex = -1;
		int32_t sectorIndex = -1;
		int32_t nextWallIndex = -1;
		int32_t nextSectorIndex = -1;
		uint32_t portalFlags = 0;
		uint32_t cstat = 0;
	};

	bool IsOrdinaryReciprocal(const WallLinkFacts& edge, const WallLinkFacts& reciprocal)
	{
		return edge.wallIndex >= 0 && edge.sectorIndex >= 0 && edge.nextWallIndex >= 0 &&
			edge.nextSectorIndex >= 0 && edge.portalFlags == 0 &&
			(edge.cstat & (CSTAT_WALL_MASKED | CSTAT_WALL_1WAY)) == 0 &&
			reciprocal.wallIndex == edge.nextWallIndex &&
			reciprocal.sectorIndex == edge.nextSectorIndex &&
			reciprocal.nextSectorIndex == edge.sectorIndex &&
			reciprocal.nextWallIndex == edge.wallIndex && reciprocal.portalFlags == 0 &&
			(reciprocal.cstat & (CSTAT_WALL_MASKED | CSTAT_WALL_1WAY)) == 0;
	}

	double EdgeLength(const ClosureEdge& edge)
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

	ClosureDescriptor AnalyzeClosure(
		int32_t sectorIndex,
		bool collapsed,
		const std::vector<ClosureEdge>& edges)
	{
		ClosureDescriptor descriptor;
		descriptor.sectorIndex = sectorIndex;
		if (!collapsed || sectorIndex < 0 || edges.size() != 4u)
			return descriptor;

		int portalIndices[2] = { -1, -1 };
		int capIndices[2] = { -1, -1 };
		uint32_t portalCount = 0;
		uint32_t capCount = 0;
		for (uint32_t edgeIndex = 0; edgeIndex < edges.size(); ++edgeIndex)
		{
			const ClosureEdge& edge = edges[edgeIndex];
			if (edge.ordinaryPortal && edge.nextSector >= 0)
			{
				if (portalCount >= 2u) return descriptor;
				portalIndices[portalCount++] = (int)edgeIndex;
			}
			else if (edge.nextSector < 0)
			{
				if (capCount >= 2u) return descriptor;
				capIndices[capCount++] = (int)edgeIndex;
			}
			else
			{
				return descriptor;
			}
		}
		if (portalCount != 2u || capCount != 2u ||
			edges[(size_t)portalIndices[0]].nextSector == edges[(size_t)portalIndices[1]].nextSector ||
			std::abs(portalIndices[0] - portalIndices[1]) != 2)
		{
			return descriptor;
		}

		const ClosureEdge& firstPortal = edges[(size_t)portalIndices[0]];
		const ClosureEdge& secondPortal = edges[(size_t)portalIndices[1]];
		const double firstX = firstPortal.end[0] - firstPortal.start[0];
		const double firstY = firstPortal.end[1] - firstPortal.start[1];
		const double secondX = secondPortal.end[0] - secondPortal.start[0];
		const double secondY = secondPortal.end[1] - secondPortal.start[1];
		const double firstLength = EdgeLength(firstPortal);
		const double secondLength = EdgeLength(secondPortal);
		if (firstLength <= 0.0 || secondLength <= 0.0 || !NearlyEqualLength(firstLength, secondLength))
			return descriptor;

		const double vectorScale = firstLength * secondLength;
		const double cross = firstX * secondY - firstY * secondX;
		const double dot = firstX * secondX + firstY * secondY;
		if (std::abs(cross) > vectorScale * 1.0e-6 || dot >= 0.0)
			return descriptor;

		const double firstCapLength = EdgeLength(edges[(size_t)capIndices[0]]);
		const double secondCapLength = EdgeLength(edges[(size_t)capIndices[1]]);
		const double maximumCapLength = std::max(firstCapLength, secondCapLength);
		if (maximumCapLength <= 0.0 || firstLength / maximumCapLength < 16.0)
			return descriptor;

		descriptor.valid = true;
		for (uint32_t portalIndex = 0; portalIndex < 2u; ++portalIndex)
		{
			const ClosureEdge& edge = edges[(size_t)portalIndices[portalIndex]];
			descriptor.portalNeighbors[portalIndex] = edge.nextSector;
			descriptor.portalWalls[portalIndex] = edge.wallIndex;
		}
		return descriptor;
	}

	template<typename VisitWalls, typename GetLink, typename GetClosure, typename HasPositiveAperture>
	void BuildRoutes(
		size_t sectorCount,
		const std::vector<uint32_t>& reachedSectorIndices,
		VisitWalls&& visitWalls,
		GetLink&& getLink,
		GetClosure&& getClosure,
		HasPositiveAperture&& hasPositiveAperture,
		std::vector<NRISpatialAbsenceFrontierRoute>& directRoutes,
		std::vector<std::vector<NRISpatialAbsenceFrontierRoute>>& closureRoutes,
		NRISpatialAbsenceFrontierIndexStats& stats)
	{
		directRoutes.assign(sectorCount, {});
		closureRoutes.assign(sectorCount, {});
		for (uint32_t reachedSector : reachedSectorIndices)
		{
			if ((size_t)reachedSector >= sectorCount) continue;
			stats.reachedSectorCount++;
			visitWalls((int32_t)reachedSector, [&](int32_t wallIndex)
			{
				stats.reachedAuthoredWallCount++;
				const FrontierWallLink reachedEdge = getLink(wallIndex);
				if (!reachedEdge.ordinaryReciprocal ||
					reachedEdge.sectorIndex != (int32_t)reachedSector ||
					reachedEdge.nextSectorIndex < 0 ||
					(size_t)reachedEdge.nextSectorIndex >= sectorCount)
				{
					return;
				}

				const ClosureDescriptor closure = getClosure(reachedEdge.nextSectorIndex);
				if (closure.valid && closure.sectorIndex == reachedEdge.nextSectorIndex)
				{
					int reachedPortal = -1;
					for (int portalIndex = 0; portalIndex < 2; ++portalIndex)
					{
						if (closure.portalNeighbors[portalIndex] == reachedEdge.sectorIndex &&
							closure.portalWalls[portalIndex] == reachedEdge.reciprocalWallIndex)
						{
							reachedPortal = portalIndex;
							break;
						}
					}
					if (reachedPortal >= 0)
					{
						const int negativePortal = reachedPortal ^ 1;
						const int32_t negativeSector = closure.portalNeighbors[negativePortal];
						const FrontierWallLink closureEdge = getLink(closure.portalWalls[negativePortal]);
						if (negativeSector >= 0 && (size_t)negativeSector < sectorCount &&
							closureEdge.sectorIndex == closure.sectorIndex &&
							closureEdge.nextSectorIndex == negativeSector &&
							closureEdge.ordinaryReciprocal)
						{
							closureRoutes[(size_t)negativeSector].push_back({
								reachedEdge.sectorIndex, reachedEdge.wallIndex,
								closure.sectorIndex, closureEdge.reciprocalWallIndex });
							stats.closureRouteCount++;
						}
					}
				}

				// A valid closure is live-collapsed, so its direct aperture cannot be positive.
				// Its separate closure route deliberately performs no aperture query.
				if (closure.valid ||
					std::binary_search(reachedSectorIndices.begin(), reachedSectorIndices.end(),
						(uint32_t)reachedEdge.nextSectorIndex))
				{
					return;
				}
				NRISpatialAbsenceFrontierRoute& direct = directRoutes[(size_t)reachedEdge.nextSectorIndex];
				if (direct.IsValid()) return;
				stats.apertureTestCount++;
				if (hasPositiveAperture(reachedEdge))
				{
					direct.reachedSector = reachedEdge.sectorIndex;
					direct.reachedWall = reachedEdge.wallIndex;
					stats.directRouteCount++;
				}
			});
		}
	}

	FrontierWallLink BuildLiveWallLink(int32_t wallIndex)
	{
		FrontierWallLink link;
		if (wallIndex < 0 || (unsigned)wallIndex >= wall.Size()) return link;
		const walltype& source = wall[(unsigned)wallIndex];
		const walltype* reciprocal = source.nextWall();
		const walltype* endWall = source.point2Wall();
		if (reciprocal == nullptr || endWall == nullptr || source.sector < 0 || source.nextsector < 0 ||
			(unsigned)source.sector >= sector.Size() || (unsigned)source.nextsector >= sector.Size())
		{
			return link;
		}

		const WallLinkFacts sourceFacts = {
			wallIndex, source.sector, source.nextwall, source.nextsector,
			(uint32_t)source.portalflags, (uint32_t)source.cstat };
		const WallLinkFacts reciprocalFacts = {
			source.nextwall, reciprocal->sector, reciprocal->nextwall, reciprocal->nextsector,
			(uint32_t)reciprocal->portalflags, (uint32_t)reciprocal->cstat };
		link.wallIndex = wallIndex;
		link.sectorIndex = source.sector;
		link.reciprocalWallIndex = source.nextwall;
		link.nextSectorIndex = source.nextsector;
		link.start[0] = source.pos.X;
		link.start[1] = source.pos.Y;
		link.end[0] = endWall->pos.X;
		link.end[1] = endWall->pos.Y;
		link.ordinaryReciprocal = IsOrdinaryReciprocal(sourceFacts, reciprocalFacts);
		return link;
	}

	bool HasLivePositiveAperture(const FrontierWallLink& link)
	{
		if (!link.ordinaryReciprocal || link.sectorIndex < 0 || link.nextSectorIndex < 0 ||
			(unsigned)link.sectorIndex >= sector.Size() || (unsigned)link.nextSectorIndex >= sector.Size())
		{
			return false;
		}
		const sectortype& front = sector[(unsigned)link.sectorIndex];
		const sectortype& back = sector[(unsigned)link.nextSectorIndex];
		auto apertureAt = [&](double x, double y)
		{
			return std::min(getflorzofslopeptr(&front, x, y), getflorzofslopeptr(&back, x, y)) -
				std::max(getceilzofslopeptr(&front, x, y), getceilzofslopeptr(&back, x, y));
		};
		const double startAperture = apertureAt(link.start[0], link.start[1]);
		const double endAperture = apertureAt(link.end[0], link.end[1]);
		return std::isfinite(startAperture) && std::isfinite(endAperture) &&
			std::max(startAperture, endAperture) > kBoundsEpsilon;
	}

	ClosureDescriptor BuildLiveClosureDescriptor(int32_t sectorIndex)
	{
		if (sectorIndex < 0 || (unsigned)sectorIndex >= sector.Size()) return {};
		const sectortype& candidate = sector[(unsigned)sectorIndex];
		std::vector<ClosureEdge> edges;
		edges.reserve(candidate.walls.Size());
		for (const walltype& candidateWall : candidate.walls)
		{
			const walltype* endWall = candidateWall.point2Wall();
			if (endWall == nullptr) return {};
			ClosureEdge edge;
			edge.start[0] = candidateWall.pos.X;
			edge.start[1] = candidateWall.pos.Y;
			edge.end[0] = endWall->pos.X;
			edge.end[1] = endWall->pos.Y;
			edge.wallIndex = wall.IndexOf(&candidateWall);
			edge.nextSector = candidateWall.nextsector;
			edge.ordinaryPortal = candidateWall.portalflags == 0 && candidateWall.nextsector >= 0;
			edges.push_back(edge);
		}
		const bool collapsed = candidate.floorz == candidate.ceilingz &&
			candidate.floorheinum == candidate.ceilingheinum;
		return AnalyzeClosure(sectorIndex, collapsed, edges);
	}
}

void NRISpatialAbsenceFrontierIndex::Build(
	const std::vector<uint32_t>& reachedSectorIndices,
	const std::vector<uint32_t>& authoredClosureSectorIndices)
{
	stats = {};
	built = true;
	struct CachedLink
	{
		bool resolved = false;
		FrontierWallLink link;
	};
	struct CachedClosure
	{
		bool resolved = false;
		ClosureDescriptor descriptor;
	};
	std::vector<CachedLink> linkCache(wall.Size());
	std::vector<CachedClosure> closureCache(sector.Size());

	auto getLink = [&](int32_t wallIndex)
	{
		if (wallIndex < 0 || (unsigned)wallIndex >= wall.Size()) return FrontierWallLink{};
		CachedLink& cached = linkCache[(size_t)wallIndex];
		if (!cached.resolved)
		{
			cached.resolved = true;
			cached.link = BuildLiveWallLink(wallIndex);
			stats.reciprocalLinkTestCount++;
		}
		return cached.link;
	};
	auto getClosure = [&](int32_t sectorIndex)
	{
		if (sectorIndex < 0 || (unsigned)sectorIndex >= sector.Size() ||
			!std::binary_search(authoredClosureSectorIndices.begin(), authoredClosureSectorIndices.end(),
				(uint32_t)sectorIndex))
		{
			return ClosureDescriptor{};
		}
		CachedClosure& cached = closureCache[(size_t)sectorIndex];
		if (!cached.resolved)
		{
			cached.resolved = true;
			cached.descriptor = BuildLiveClosureDescriptor(sectorIndex);
			stats.closureDescriptorBuildCount++;
			stats.closureDescriptorHitCount += cached.descriptor.valid ? 1u : 0u;
		}
		return cached.descriptor;
	};
	BuildRoutes(
		sector.Size(), reachedSectorIndices,
		[](int32_t sectorIndex, const auto& visit)
		{
			for (const walltype& source : sector[(unsigned)sectorIndex].walls)
				visit(wall.IndexOf(&source));
		},
		getLink, getClosure, HasLivePositiveAperture,
		directRoutes, closureRoutes, stats);
}

const NRISpatialAbsenceFrontierRoute* NRISpatialAbsenceFrontierIndex::FindDirect(int32_t negativeSector) const
{
	if (!built || negativeSector < 0 || (size_t)negativeSector >= directRoutes.size() ||
		!directRoutes[(size_t)negativeSector].IsValid())
	{
		return nullptr;
	}
	return &directRoutes[(size_t)negativeSector];
}

const std::vector<NRISpatialAbsenceFrontierRoute>* NRISpatialAbsenceFrontierIndex::FindClosures(int32_t negativeSector) const
{
	if (!built || negativeSector < 0 || (size_t)negativeSector >= closureRoutes.size() ||
		closureRoutes[(size_t)negativeSector].empty())
	{
		return nullptr;
	}
	return &closureRoutes[(size_t)negativeSector];
}

bool RunNRISpatialAbsenceFrontierIndexSelfTests(std::string* failureReason)
{
	auto fail = [&](const char* reason)
	{
		if (failureReason != nullptr) *failureReason = reason;
		return false;
	};

	const WallLinkFacts ordinary = { 0, 0, 10, 1, 0, 0 };
	WallLinkFacts reciprocal = { 10, 1, 0, 0, 0, 0 };
	if (!IsOrdinaryReciprocal(ordinary, reciprocal))
		return fail("valid reciprocal link was rejected");
	reciprocal.portalFlags = 1;
	if (IsOrdinaryReciprocal(ordinary, reciprocal))
		return fail("portal-flagged reciprocal link was accepted");
	reciprocal = { 10, 1, 0, 0, 0, (uint32_t)CSTAT_WALL_MASKED };
	if (IsOrdinaryReciprocal(ordinary, reciprocal))
		return fail("masked reciprocal link was accepted");
	reciprocal = { 10, 1, 0, 0, 0, (uint32_t)CSTAT_WALL_1WAY };
	if (IsOrdinaryReciprocal(ordinary, reciprocal))
		return fail("one-way reciprocal link was accepted");
	reciprocal = { 10, 1, 9, 0, 0, 0 };
	if (IsOrdinaryReciprocal(ordinary, reciprocal))
		return fail("nonreciprocal link was accepted");

	std::vector<std::vector<int32_t>> sectorWalls(8u);
	sectorWalls[0] = { 0, 1, 2 };
	sectorWalls[1] = { 3, 4, 5 };
	std::vector<FrontierWallLink> links(80u);
	auto setLink = [&](int32_t wallIndex, int32_t owner, int32_t next, int32_t reciprocalWall)
	{
		links[(size_t)wallIndex].wallIndex = wallIndex;
		links[(size_t)wallIndex].sectorIndex = owner;
		links[(size_t)wallIndex].nextSectorIndex = next;
		links[(size_t)wallIndex].reciprocalWallIndex = reciprocalWall;
		links[(size_t)wallIndex].ordinaryReciprocal = true;
	};
	setLink(0, 0, 3, 30);
	setLink(1, 0, 4, 40);
	setLink(2, 0, 5, 50);
	setLink(3, 1, 3, 31);
	setLink(4, 1, 7, 70);
	setLink(5, 1, 6, 62);
	setLink(51, 5, 6, 60);
	setLink(71, 7, 6, 61);
	std::vector<ClosureDescriptor> closures(8u);
	closures[5] = { 5, true, { 0, 6 }, { 50, 51 } };
	closures[7] = { 7, true, { 1, 6 }, { 70, 71 } };
	std::vector<bool> aperture(80u, false);
	aperture[0] = true;
	aperture[3] = true;
	aperture[5] = true;
	uint32_t apertureCalls = 0;
	std::vector<NRISpatialAbsenceFrontierRoute> direct;
	std::vector<std::vector<NRISpatialAbsenceFrontierRoute>> closure;
	NRISpatialAbsenceFrontierIndexStats testStats;
	const std::vector<uint32_t> reached = { 0u, 1u };
	BuildRoutes(
		sectorWalls.size(), reached,
		[&](int32_t sectorIndex, const auto& visit)
		{
			for (int32_t wallIndex : sectorWalls[(size_t)sectorIndex]) visit(wallIndex);
		},
		[&](int32_t wallIndex)
		{
			return wallIndex >= 0 && (size_t)wallIndex < links.size() ?
				links[(size_t)wallIndex] : FrontierWallLink{};
		},
		[&](int32_t sectorIndex)
		{
			return sectorIndex >= 0 && (size_t)sectorIndex < closures.size() ?
				closures[(size_t)sectorIndex] : ClosureDescriptor{};
		},
		[&](const FrontierWallLink& link)
		{
			apertureCalls++;
			return aperture[(size_t)link.wallIndex];
		},
		direct, closure, testStats);
	if (!direct[3].IsValid() || direct[3].reachedSector != 0 || direct[3].reachedWall != 0)
		return fail("direct route did not preserve first reached-sector/wall order");
	if (!direct[6].IsValid() || direct[6].closureSector >= 0 || closure[6].empty())
		return fail("direct route was not available ahead of closure candidates");
	if (direct[4].IsValid())
		return fail("zero-aperture direct route was accepted");
	if (closure[6].size() != 2u || closure[6][0].closureSector != 5 ||
		closure[6][0].negativeWall != 60 || closure[6][1].closureSector != 7 ||
		closure[6][1].negativeWall != 61)
	{
		return fail("closure routes did not preserve reached-sector/wall order");
	}
	if (apertureCalls != 3u || testStats.apertureTestCount != 3u)
		return fail("closure routing performed aperture work");

	std::vector<NRISpatialAbsenceFrontierRoute> reachedClosureDirect;
	std::vector<std::vector<NRISpatialAbsenceFrontierRoute>> reachedClosureRoutes;
	NRISpatialAbsenceFrontierIndexStats reachedClosureStats;
	const std::vector<uint32_t> reachedWithClosure = { 0u, 1u, 5u };
	BuildRoutes(
		sectorWalls.size(), reachedWithClosure,
		[&](int32_t sectorIndex, const auto& visit)
		{
			for (int32_t wallIndex : sectorWalls[(size_t)sectorIndex]) visit(wallIndex);
		},
		[&](int32_t wallIndex)
		{
			return wallIndex >= 0 && (size_t)wallIndex < links.size() ?
				links[(size_t)wallIndex] : FrontierWallLink{};
		},
		[&](int32_t sectorIndex)
		{
			return sectorIndex >= 0 && (size_t)sectorIndex < closures.size() ?
				closures[(size_t)sectorIndex] : ClosureDescriptor{};
		},
		[&](const FrontierWallLink& link) { return aperture[(size_t)link.wallIndex]; },
		reachedClosureDirect, reachedClosureRoutes, reachedClosureStats);
	if (reachedClosureRoutes[6].empty() || reachedClosureRoutes[6][0].closureSector != 5)
		return fail("reached authored-closure destination lost its closure candidate");

	if (failureReason != nullptr) failureReason->clear();
	return true;
}
