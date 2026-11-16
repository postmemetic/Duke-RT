#include "nri_actor_sprite_fallback.h"

#include "nri_scene_bridge.h"
#include "nri_surface_builder.h"
#include "../renderer/nri_cvars.h"
#include "actor_presentation_snapshot.h"
#include "coreactor.h"
#include "gamestruct.h"
#include "texturemanager.h"
#include "gametexture.h"
#include "buildtiles.h"

#include <algorithm>
#include <utility>

namespace nri_scene
{
namespace
{
	struct RetainedSprite
	{
		ActorSpriteFallbackCandidate candidate;
		ActorPresentationState authority;
		tspritetype sprite = {};
		bool rootCapture = false;
		bool luminous = false;
	};
	std::vector<RetainedSprite> gSprites;
	std::vector<ActorSpriteFallbackCandidate> gCandidates;
	uint64_t gFrame = 0;

	bool Eligible(const ActorPresentationState* actor)
	{
		return actor != nullptr && actor->authorityCurrent && actor->publicationEligible &&
			(actor->renderFlags & SPREXT_NOTMD) == 0 && (actor->cstat2 & CSTAT2_SPRITE_NOMODEL) == 0;
	}

	void SetAuthority(ActorSpriteFallbackCandidate& candidate, const ActorPresentationState& actor)
	{
		candidate.ownerWorldEpoch = actor.owner.ownerWorldEpoch;
		candidate.ownerLifetimeGeneration = (uint64_t)actor.owner.ownerLifetimeGeneration;
		candidate.placementGeneration = actor.placementGeneration;
		candidate.placementStateHash = actor.placementStateHash;
		candidate.actorIndex = actor.actorIndex;
		candidate.physicalSectorIndex = actor.physicalSectorIndex;
		candidate.simulationGeneration = GetActorPresentationSnapshot().simulationGeneration;
	}

	bool BuildSurface(HWDrawInfo& view, tspritetype sprite, const ActorPresentationState& actor,
		bool luminous, uint32_t drawListType, SurfaceRef& surface)
	{
		if (sprite.scale.X == 0 || sprite.scale.Y == 0 || !sprite.spritetexture().isValid() ||
			(sprite.cstat & CSTAT_SPRITE_INVISIBLE) != 0 || sprite.sectp == nullptr) return false;
		if (actor.renderFlags & SPREXT_AWAY1) sprite.pos += sprite.Angles.Yaw.ToVector() * 0.125;
		else if (actor.renderFlags & SPREXT_AWAY2) sprite.pos -= sprite.Angles.Yaw.ToVector() * 0.125;
		FGameTexture* texture = nullptr;
		float alpha = 0.0f;
		FRenderStyle style;
		int shade = sprite.shade;
		int palette = sprite.pal;
		uint32_t flags = MaterialFlag_Sprite | MaterialFlag_AlphaClip;
		surface = {};
		surface.vertices.reserve(4);
		if ((sprite.cstat & CSTAT_SPRITE_ALIGNMENT_MASK) == CSTAT_SPRITE_ALIGNMENT_WALL)
		{
			HWWall wall = {};
			// Retained geometry is shared by all ray workloads. One-way behavior
			// belongs to the material, not a primary-camera capture rejection.
			if (!wall.PrepareWallSprite(nullptr, &sprite, sprite.sectp, &actor.alpha)) return false;
			texture = wall.texture;
			alpha = wall.alpha;
			style = wall.RenderStyle;
			const auto add = [&](float x, float y, float z, int corner)
			{
				surface.vertices.push_back(BuildCapturedVertex(x, z, y, wall.tcs[corner].u, wall.tcs[corner].v));
			};
			add(wall.glseg.x1, wall.glseg.y1, wall.ztop[0], HWWall::UPLFT);
			add(wall.glseg.x2, wall.glseg.y2, wall.ztop[1], HWWall::UPRGT);
			add(wall.glseg.x1, wall.glseg.y1, wall.zbottom[0], HWWall::LOLFT);
			add(wall.glseg.x2, wall.glseg.y2, wall.zbottom[1], HWWall::LORGT);
			NudgeActorSpriteFallbackWallSurface(wall, surface);
			if (sprite.cstat & CSTAT_SPRITE_ONE_SIDE) flags |= MaterialFlag_OneWay;
		}
		else if ((sprite.cstat & CSTAT_SPRITE_ALIGNMENT_MASK) == CSTAT_SPRITE_ALIGNMENT_FACING)
		{
			HWSprite facing = {};
			if (!facing.PrepareFacingSprite(&view, &sprite, sprite.sectp, &actor.alpha)) return false;
			texture = facing.texture;
			alpha = facing.alpha;
			style = facing.RenderStyle;
			flags |= MaterialFlag_FacingBillboard;
			surface.vertices.push_back(BuildCapturedVertex(facing.x1, facing.z1, facing.y1, facing.ul, facing.vt));
			surface.vertices.push_back(BuildCapturedVertex(facing.x2, facing.z1, facing.y2, facing.ur, facing.vt));
			surface.vertices.push_back(BuildCapturedVertex(facing.x1, facing.z2, facing.y1, facing.ul, facing.vb));
			surface.vertices.push_back(BuildCapturedVertex(facing.x2, facing.z2, facing.y2, facing.ur, facing.vb));
		}
		else return false;
		const bool supportedBlend = style.BlendOp == STYLEOP_Add && style.SrcAlpha == STYLEALPHA_Src &&
			style.DestAlpha == STYLEALPHA_InvSrc && style.Flags == 0;
		if (!supportedBlend || alpha <= (1.0f / 255.0f) || sprite.statnum == 99) return false;
		if (alpha < 0.999f && (luminous || shade <= -127)) flags |= MaterialFlag_TranslucentEffect;
		surface.material = MakeMaterialRef(texture, palette, shade, alpha, flags);
		surface.provenance.sourceType = SurfaceSourceType::FacingSprite;
		surface.provenance.actorIndex = actor.actorIndex;
		surface.provenance.sectorIndex = actor.physicalSectorIndex;
		surface.provenance.cstat = (uint32_t)sprite.cstat;
		surface.provenance.drawListType = drawListType;
		surface.provenance.materialFlags = surface.material.flags;
		const float delta[3] = { (float)(actor.position.x - actor.previousPosition.x),
			(float)(actor.previousPosition.z - actor.position.z), (float)(actor.previousPosition.y - actor.position.y) };
		for (auto& vertex : surface.vertices)
			for (unsigned axis = 0; axis < 3; ++axis) vertex.prevPosition[axis] = vertex.position[axis] - delta[axis];
		return true;
	}
}

void BeginActorSpriteFallbackFrame(uint64_t frame)
{
	if (!(bool)nri_ptactorspritefallback)
	{
		ResetActorSpriteFallbackCandidates();
		return;
	}
	gFrame = frame;
	gSprites.erase(std::remove_if(gSprites.begin(), gSprites.end(), [](const RetainedSprite& retained)
	{
		const auto* actor = LookupActorPresentationByOwnerKey(retained.authority.owner);
		return !Eligible(actor) || actor->actorIndex != retained.candidate.actorIndex;
	}), gSprites.end());
	for (auto& retained : gSprites)
	{
		retained.candidate.capturedThisFrame = false;
		retained.candidate.descriptorCurrent = false;
		retained.candidate.animationCurrent = false;
		retained.candidate.transientGeometryCaptured = false;
	}
}

bool CaptureActorSpriteFallbackCandidate(HWDrawInfo& di, const HWSprite& source,
	uint64_t identityKey, uint32_t drawListType, bool transientVoxel, bool indirectOnly)
{
	if (!(bool)nri_ptactorspritefallback) return false;
	if (source.Sprite == nullptr || source.Sprite->ownerActor == nullptr || identityKey == 0) return false;
	const auto* actor = LookupActorPresentationByActor(source.Sprite->ownerActor);
	if (!Eligible(actor)) return false;
	RetainedSprite next;
	next.authority = *actor;
	next.sprite = *source.Sprite;
	next.sprite.ApplySpriteFallbackFrame();
	next.luminous = source.Sprite->ownerActor->spr.shade <= -127;
	next.sprite.ownerActor = nullptr;
	next.rootCapture = di.outer == nullptr;
	next.candidate.identityKey = identityKey;
	SetAuthority(next.candidate, *actor);
	next.candidate.capturedFrame = gFrame;
	next.candidate.capturedThisFrame = true;
	next.candidate.descriptorCurrent = true;
	next.candidate.animationCurrent = true;
	next.candidate.indirectOnly = indirectOnly;
	next.candidate.transientVoxel = transientVoxel;
	if (!BuildSurface(di, next.sprite, *actor, next.luminous, drawListType, next.candidate.surface)) return false;
	auto found = std::find_if(gSprites.begin(), gSprites.end(), [identityKey](const RetainedSprite& item)
		{ return item.candidate.identityKey == identityKey; });
	if (found == gSprites.end()) gSprites.push_back(std::move(next));
	else if (!found->candidate.capturedThisFrame || next.rootCapture || !found->rootCapture)
	{
		next.candidate.transientGeometryCaptured = found->candidate.transientGeometryCaptured;
		*found = std::move(next);
	}
	return true;
}

void MarkActorSpriteFallbackTransientGeometry(uint64_t identityKey)
{
	for (auto& item : gSprites)
		if (item.candidate.identityKey == identityKey) item.candidate.transientGeometryCaptured = true;
}

void DiscardRetainedActorSpriteFallbackCandidate(int32_t actorIndex)
{
	if (actorIndex < 0) return;
	gSprites.erase(std::remove_if(gSprites.begin(), gSprites.end(), [actorIndex](const RetainedSprite& item)
	{
		return item.candidate.actorIndex == actorIndex && !item.candidate.capturedThisFrame;
	}), gSprites.end());
}

void RefreshRetainedActorSpriteFallbackCandidates(HWDrawInfo& view)
{
	if (!(bool)nri_ptactorspritefallback) return;
	for (auto& retained : gSprites)
	{
		auto& candidate = retained.candidate;
		if (candidate.capturedThisFrame || candidate.transientVoxel) continue;
		const auto* actor = LookupActorPresentationByOwnerKey(retained.authority.owner);
		if (!Eligible(actor) || actor->physicalSectorIndex < 0 || (unsigned)actor->physicalSectorIndex >= sector.Size()) continue;
		tspritetype sprite = retained.sprite;
		if (sprite.spriteFallbackTileBase.isValid()) sprite.setspritetexture(sprite.spriteFallbackTileBase);
		const auto& before = retained.authority;
		// Preserve game-authored presentation offsets while applying live placement.
		sprite.pos += DVector3(actor->position.x - before.position.x,
			actor->position.y - before.position.y, actor->position.z - before.position.z);
		sprite.Angles.Yaw += DAngle::fromDeg(actor->angles.yawDegrees - before.angles.yawDegrees);
		if (before.scale.x != 0) sprite.scale.X *= actor->scale.x / before.scale.x;
		if (before.scale.y != 0) sprite.scale.Y *= actor->scale.y / before.scale.y;
		sprite.xoffset += actor->spriteXOffset - before.spriteXOffset;
		sprite.yoffset += actor->spriteYOffset - before.spriteYOffset;
		if (actor->spriteShade != before.spriteShade) sprite.shade = actor->spriteShade;
		if (actor->spritePalette != before.spritePalette) sprite.pal = actor->spritePalette;
		// Apply only changed authoritative bits. Unchanged actor flags must not
		// undo a flag deliberately set or cleared by game presentation code.
		const auto changedFlags = ESpriteFlags::FromInt((uint16_t)(before.cstat ^ actor->cstat));
		const auto currentFlags = ESpriteFlags::FromInt((uint16_t)actor->cstat);
		sprite.cstat = (sprite.cstat & ~changedFlags) | (currentFlags & changedFlags);
		const uint16_t changedFlags2 = before.cstat2 ^ actor->cstat2;
		sprite.cstat2 = (uint16_t)((sprite.cstat2 & ~changedFlags2) | (actor->cstat2 & changedFlags2));
		sprite.sectp = &sector[actor->physicalSectorIndex];
		if (actor->textureId != before.textureId)
		{
			FTextureID texture;
			texture.SetIndex(actor->textureId);
			sprite.setspritetexture(texture);
			sprite.spriteFallbackActionBase = texture;
			sprite.spriteFallbackRotationBase = texture;
		}
		candidate.animationCurrent = gi != nullptr && gi->ResolveActorSpriteFallbackFrame(*actor, sprite,
			DVector2(view.Viewpoint.Pos.X, -view.Viewpoint.Pos.Y), DAngle::fromBam(view.Viewpoint.RotAngle));
		// Unknown script-only animation is not certified beyond its captured tick.
		if (!candidate.animationCurrent && candidate.simulationGeneration != GetActorPresentationSnapshot().simulationGeneration) continue;
		auto texture = sprite.spritetexture();
		if (!(sprite.cstat2 & CSTAT2_SPRITE_NOANIMATE)) tileUpdatePicnum(texture, actor->actorIndex & 16383);
		sprite.setspritetexture(texture);
		if (sprite.cstat2 & CSTAT2_SPRITE_FULLBRIGHT) sprite.shade = -127;
		SurfaceRef surface;
		if (!BuildSurface(view, sprite, *actor, retained.luminous, candidate.surface.provenance.drawListType, surface)) continue;
		candidate.surface = std::move(surface);
		candidate.descriptorCurrent = true;
		SetAuthority(candidate, *actor);
	}
}

const std::vector<ActorSpriteFallbackCandidate>& GetActorSpriteFallbackCandidates()
{
	gCandidates.clear();
	gCandidates.reserve(gSprites.size());
	for (const auto& retained : gSprites) gCandidates.push_back(retained.candidate);
	return gCandidates;
}

bool HasCurrentActorSpriteFallbackCandidate(uint64_t identityKey, uint64_t worldEpoch, uint64_t lifetimeGeneration)
{
	if (!(bool)nri_ptactorspritefallback) return false;
	for (const auto& item : gSprites)
	{
		const auto& candidate = item.candidate;
		if (candidate.identityKey == identityKey && candidate.ownerWorldEpoch == worldEpoch &&
			candidate.ownerLifetimeGeneration == lifetimeGeneration && candidate.descriptorCurrent &&
			candidate.surface.vertices.size() >= 3) return true;
	}
	return false;
}

void ResetActorSpriteFallbackCandidates()
{
	gSprites.clear();
	gCandidates.clear();
	gFrame = 0;
}
}
