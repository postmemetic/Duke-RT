#include "nri_wall_decals.h"

#include "nri_scene_bridge.h"
#include "nri_wall_decal_projection.h"
#include "coreactor.h"
#include "gamecontrol.h"
#include "gamefuncs.h"
#include "hw_voxels.h"
#include "texinfo.h"
#include "texturemanager.h"

#include <algorithm>
#include <cmath>

EXTERN_CVAR(Bool, r_voxels)

namespace nri_scene
{
namespace
{
bool IsCandidate(const spritetypebase& sprite)
{
	return sprite.sectp != nullptr && sprite.scale.X > 0.0 && sprite.scale.Y > 0.0 &&
		(sprite.cstat & CSTAT_SPRITE_INVISIBLE) == 0 &&
		(sprite.cstat & CSTAT_SPRITE_TRANSLUCENT) != 0 &&
		(sprite.cstat & CSTAT_SPRITE_ALIGNMENT_MASK) == CSTAT_SPRITE_ALIGNMENT_WALL;
}

void FindReceivers(const tspritetype& sprite, const HWWall& quad, std::vector<walltype*>& receivers)
{
	receivers.clear();
	// Stock markers can start over a unit off their wall, and crack initialization
	// moves them another half unit. A two-unit envelope covers those placements;
	// exact receiver identity, side and overlap still prevent corner bleed.
	constexpr double attachmentDistance = 2.0;
	constexpr double parallelCosine = 0.999961923; // half a degree, including map angle rounding
	const DVector2 a(quad.glseg.x1, -quad.glseg.y1);
	const DVector2 b(quad.glseg.x2, -quad.glseg.y2);
	const auto span = b - a;
	const double spanSquared = span.LengthSquared();
	if (spanSquared <= 1.e-8) return;
	const auto facing = sprite.Angles.Yaw.ToVector();
	double bestDistance = attachmentDistance;
	for (auto& candidate : sprite.sectp->walls)
	{
		const auto delta = candidate.delta();
		if (delta.LengthSquared() <= 1.e-8) continue;
		const auto normal = delta.Rotated90CCW().Unit();
		const double alignment = normal.X * facing.X + normal.Y * facing.Y;
		if (std::abs(alignment) < parallelCosine ||
			((sprite.cstat & CSTAT_SPRITE_ONE_SIDE) && alignment <= 0.0)) continue;
		const double distance = std::abs((sprite.pos.X - candidate.pos.X) * normal.X +
			(sprite.pos.Y - candidate.pos.Y) * normal.Y);
		if (distance > attachmentDistance || distance > bestDistance + 1.e-4) continue;
		const auto start = candidate.pos - a;
		const auto end = candidate.pos + delta - a;
		const double t0 = (start.X * span.X + start.Y * span.Y) / spanSquared;
		const double t1 = (end.X * span.X + end.Y * span.Y) / spanSquared;
		if (std::max(t0, t1) <= 0.0 || std::min(t0, t1) >= 1.0) continue;
		if (distance < bestDistance - 1.e-4)
		{
			receivers.clear();
			bestDistance = distance;
		}
		receivers.push_back(&candidate);
	}
}
}

void CaptureWallDecals(HWDrawInfo& view, std::vector<WallDecalCapture>& decals)
{
	decals.clear();
	if (gi == nullptr) return;
	tspriteArray sprites = {};
	TSpriteIterator<DCoreActor> iterator;
	while (DCoreActor* actor = iterator.Next())
	{
		if (actor->exists() && (actor->ObjectFlags & OF_EuthanizeMe) == 0 && IsCandidate(actor->spr))
			renderAddTsprite(sprites, actor);
	}
	const auto& vp = view.Viewpoint;
	gi->processSprites(sprites, DVector3(vp.Pos.X, -vp.Pos.Y, -vp.Pos.Z), DAngle::fromBam(vp.RotAngle), vp.TicFrac);
	std::vector<walltype*> receivers;
	for (unsigned i = 0; i < sprites.Size(); ++i)
	{
		auto* sprite = sprites.get(i);
		auto* actor = sprite->ownerActor;
		if (actor == nullptr || !IsCandidate(*sprite)) continue;
		auto texid = sprite->spritetexture();
		if (!(sprite->cstat2 & CSTAT2_SPRITE_NOANIMATE))
			tileUpdatePicnum(texid, actor->GetIndex() & 16383);
		sprite->setspritetexture(texid);
		if (!texid.isValid()) continue;
		if (r_voxels && !(actor->sprext.renderflags & SPREXT_NOTMD) &&
			!(sprite->cstat2 & CSTAT2_SPRITE_NOMODEL))
		{
			const auto voxel = GetExtInfo(texid).tiletovox;
			if (voxel >= 0 && voxmodels[voxel] != nullptr) continue;
		}
		if (sprite->cstat2 & CSTAT2_SPRITE_FULLBRIGHT) sprite->shade = -127;
		if (actor->sprext.renderflags & SPREXT_AWAY1)
			sprite->pos += sprite->Angles.Yaw.ToVector() * 0.125;
		else if (actor->sprext.renderflags & SPREXT_AWAY2)
			sprite->pos -= sprite->Angles.Yaw.ToVector() * 0.125;

		HWWall quad = {};
		if (!quad.PrepareWallSprite(nullptr, sprite, sprite->sectp) ||
			quad.alpha <= 0.0f || quad.alpha >= 0.999f ||
			quad.RenderStyle.BlendOp != STYLEOP_Add || quad.RenderStyle.SrcAlpha != STYLEALPHA_Src ||
			quad.RenderStyle.DestAlpha != STYLEALPHA_InvSrc || quad.RenderStyle.Flags != 0)
			continue;
		FindReceivers(*sprite, quad, receivers);
		if (receivers.empty()) continue;

		WallDecalCapture capture = {};
		auto& data = capture.projection;
		if (!BuildWallDecalProjection(quad.glseg.x1, quad.glseg.y1, quad.glseg.x2, quad.glseg.y2,
			quad.zbottom[0], quad.ztop[0], quad.tcs[HWWall::LOLFT].u, quad.tcs[HWWall::LORGT].u,
			quad.tcs[HWWall::LOLFT].v, quad.tcs[HWWall::UPLFT].v, data))
			continue;
		data.actorIndex = actor->GetIndex();
		data.alpha = quad.alpha;
		capture.source.material = MakeMaterialRef(quad.texture, quad.palette, quad.shade, quad.alpha,
			MaterialFlag_Sprite | MaterialFlag_AlphaClip);
		capture.source.provenance.actorIndex = actor->GetIndex();
		capture.source.provenance.sourceType = SurfaceSourceType::DrawListWall;
		// A single mark can span several collinear map walls. Give each exact
		// receiver the same projector, without painting adjacent corners/planes.
		for (const auto* receiver : receivers)
		{
			const auto unitNormal = receiver->delta().Rotated90CCW().Unit();
			data.plane[0] = (float)unitNormal.X;
			data.plane[2] = (float)-unitNormal.Y;
			data.plane[3] = (float)-(receiver->pos.X * unitNormal.X + receiver->pos.Y * unitNormal.Y);
			data.wallIndex = wall.IndexOf(receiver);
			data.sectorIndex = receiver->sector;
			capture.source.provenance.sectorIndex = data.sectorIndex;
			capture.source.provenance.wallIndex = data.wallIndex;
			decals.push_back(capture);
		}
	}
	// Map/actor order is stable across viewpoints, including overlapping marks.
	std::sort(decals.begin(), decals.end(), [](const auto& a, const auto& b)
	{
		if (a.projection.wallIndex != b.projection.wallIndex)
			return a.projection.wallIndex < b.projection.wallIndex;
		return a.projection.actorIndex < b.projection.actorIndex;
	});
}
}
