#include "nri_wall_decal_receivers.h"

#include "gamefuncs.h"
#include "hw_drawstructs.h"

#include <algorithm>
#include <cmath>

namespace nri_scene
{
namespace
{
constexpr double kPlaneTolerance = 1.e-4;
constexpr double kParallelCosine = 0.999961923; // Half a degree, including map angle rounding.

double Dot(const DVector2& a, const DVector2& b)
{
	return a.X * b.X + a.Y * b.Y;
}

bool SamePoint(const DVector2& a, const DVector2& b)
{
	return (a - b).LengthSquared() <= kPlaneTolerance * kPlaneTolerance;
}

bool OverlapsSpan(const walltype& candidate, const DVector2& a, const DVector2& span)
{
	const double t0 = Dot(candidate.pos - a, span);
	const double t1 = Dot(candidate.point2Wall()->pos - a, span);
	return std::max(t0, t1) > 0.0 && std::min(t0, t1) < span.LengthSquared();
}

bool SharesPlane(const walltype& candidate, const walltype& anchor, const DVector2& normal)
{
	const auto delta = candidate.delta();
	return delta.LengthSquared() > 1.e-8 &&
		Dot(delta.Rotated90CCW().Unit(), normal) >= kParallelCosine &&
		std::abs(Dot(candidate.pos - anchor.pos, normal)) <= kPlaneTolerance &&
		std::abs(Dot(candidate.point2Wall()->pos - anchor.pos, normal)) <= kPlaneTolerance;
}

bool CanCrossSeam(const walltype& seam, const walltype* pair, const DVector2& point,
	const HWWall& quad)
{
	if (pair == nullptr || seam.nextSector() == nullptr || pair->nextWall() != &seam ||
		pair->sector != seam.nextsector || pair->nextsector != seam.sector ||
		seam.portalflags != 0 || pair->portalflags != 0 ||
		((seam.cstat | pair->cstat) & (CSTAT_WALL_MASKED | CSTAT_WALL_1WAY)) != 0 ||
		!SamePoint(seam.pos, pair->point2Wall()->pos) ||
		!SamePoint(seam.point2Wall()->pos, pair->pos))
		return false;

	double ceiling, floor, nextCeiling, nextFloor;
	calcSlope(seam.sectorp(), point.X, point.Y, &ceiling, &floor);
	calcSlope(seam.nextSector(), point.X, point.Y, &nextCeiling, &nextFloor);
	// Records retain the original full-height projector. Only continue when the
	// whole artwork fits the opening at this seam; partial-height continuation
	// would need a separately clipped projector for the new receiver.
	return std::isfinite(ceiling) && std::isfinite(floor) &&
		std::isfinite(nextCeiling) && std::isfinite(nextFloor) &&
		std::max(ceiling, nextCeiling) <= -quad.ztop[0] + kPlaneTolerance &&
		std::min(floor, nextFloor) >= -quad.zbottom[0] - kPlaneTolerance;
}

walltype* FindContinuation(const walltype& receiver, bool atEnd, const walltype& anchor,
	const DVector2& normal, const DVector2& a, const DVector2& span, const HWWall& quad)
{
	const auto point = atEnd ? receiver.point2Wall()->pos : receiver.pos;
	const double along = Dot(point - a, span);
	if (along <= 0.0 || along >= span.LengthSquared()) return nullptr;

	auto* candidate = atEnd ? receiver.point2Wall() : receiver.lastWall();
	std::vector<walltype*> visited;
	while (candidate != nullptr)
	{
		if (!SamePoint(atEnd ? candidate->pos : candidate->point2Wall()->pos, point) ||
			std::find(visited.begin(), visited.end(), candidate) != visited.end())
			return nullptr;
		visited.push_back(candidate);
		if (SharesPlane(*candidate, anchor, normal))
			return OverlapsSpan(*candidate, a, span) ? candidate : nullptr;

		// Walk the sector fan at this endpoint, staying on the visible side of
		// the original backing plane. Never cross the backing wall itself or
		// follow a portal elsewhere in the sector to an unrelated wall.
		const auto otherPoint = atEnd ? candidate->point2Wall()->pos : candidate->pos;
		if (Dot(otherPoint - anchor.pos, normal) <= kPlaneTolerance) return nullptr;
		auto* pair = candidate->nextWall();
		if (!CanCrossSeam(*candidate, pair, point, quad)) return nullptr;
		candidate = atEnd ? pair->point2Wall() : pair->lastWall();
	}
	return nullptr;
}
}

void FindWallDecalReceivers(const tspritetype& sprite, const HWWall& quad,
	std::vector<walltype*>& receivers)
{
	receivers.clear();
	const DVector2 a(quad.glseg.x1, -quad.glseg.y1);
	const DVector2 b(quad.glseg.x2, -quad.glseg.y2);
	const auto span = b - a;
	const double spanSquared = span.LengthSquared();
	if (sprite.sectp == nullptr || !std::isfinite(spanSquared) || spanSquared <= 1.e-8 ||
		!std::isfinite(quad.ztop[0]) || !std::isfinite(quad.zbottom[0]) ||
		quad.ztop[0] <= quad.zbottom[0]) return;
	const auto facing = sprite.Angles.Yaw.ToVector();
	// Preserve the authored attachment envelope, including cracks moved away
	// from the wall by gameplay initialization. Pick the initial plane locally.
	double bestDistance = 2.0;
	for (auto& candidate : sprite.sectp->walls)
	{
		const auto delta = candidate.delta();
		if (delta.LengthSquared() <= 1.e-8) continue;
		const auto normal = delta.Rotated90CCW().Unit();
		const double alignment = Dot(normal, facing);
		if (std::abs(alignment) < kParallelCosine ||
			((sprite.cstat & CSTAT_SPRITE_ONE_SIDE) && alignment <= 0.0)) continue;
		const double distance = std::abs(Dot(sprite.pos.XY() - candidate.pos, normal));
		if (distance > 2.0 || distance > bestDistance + kPlaneTolerance ||
			!OverlapsSpan(candidate, a, span)) continue;
		if (distance < bestDistance - kPlaneTolerance)
		{
			receivers.clear();
			bestDistance = distance;
		}
		receivers.push_back(&candidate);
	}

	const size_t seedCount = receivers.size();
	for (size_t seed = 0; seed < seedCount; ++seed)
	{
		const auto* anchor = receivers[seed];
		const auto normal = anchor->delta().Rotated90CCW().Unit();
		std::vector<walltype*> pending = { receivers[seed] };
		for (size_t index = 0; index < pending.size(); ++index)
		{
			for (bool atEnd : { false, true })
			{
				auto* next = FindContinuation(*pending[index], atEnd, *anchor, normal, a, span, quad);
				if (next == nullptr || std::find(receivers.begin(), receivers.end(), next) != receivers.end())
					continue;
				receivers.push_back(next);
				pending.push_back(next);
			}
		}
	}
}
}
