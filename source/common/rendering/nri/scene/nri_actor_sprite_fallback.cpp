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
#include "c_dispatch.h"
#include "perf_audit.h"

#include <algorithm>
#include <utility>
#include <cstdio>

EXTERN_CVAR(Bool, nri_ptactorrepresentationtrace)
EXTERN_CVAR(Int, nri_ptactorrepresentationactor)

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
	uint32_t gDescriptorTraceCount = 0;

	void TraceDescriptorRejection(ActorSpriteFallbackCandidate& candidate, const ActorPresentationState& actor,
		const tspritetype& sprite, const HWDrawInfo& view, const char* reason)
	{
		// Preserve failure metadata even when detailed tracing targets a different
		// actor. The renderer emits only bounded missing-body rows from this copy.
		candidate.descriptorReason = reason;
		candidate.descriptorTexture = sprite.spritetexture().GetIndex();
		candidate.recipeCertified = actor.spriteAnimation.recipeCertified;
		if (!(bool)nri_ptactorrepresentationtrace || !PerfAuditActive() || gDescriptorTraceCount >= 128 ||
			((int)nri_ptactorrepresentationactor >= 0 && (int)nri_ptactorrepresentationactor != actor.actorIndex)) return;
		auto placedPosition = sprite.pos;
		if (actor.renderFlags & SPREXT_AWAY1) placedPosition += sprite.Angles.Yaw.ToVector() * 0.125;
		else if (actor.renderFlags & SPREXT_AWAY2) placedPosition -= sprite.Angles.Yaw.ToVector() * 0.125;
		const auto distance = placedPosition.XY() - DVector2(view.Viewpoint.Pos.X, -view.Viewpoint.Pos.Y);
		char fields[1280];
		std::snprintf(fields, sizeof(fields), "\"actor\":%d,\"identity\":%llu,\"world\":%llu,\"lifetime\":%llu,"
			"\"capture_frame\":%llu,\"reason\":\"%s\",\"animation_current\":%s,\"recipe_certified\":%s,"
			"\"texture\":%d,\"authority_texture\":%d,\"recipe_texture\":%d,\"sector\":%d,\"cstat\":%u,"
			"\"scale\":[%.6f,%.6f],\"pos\":[%.6f,%.6f,%.6f],\"view\":[%.6f,%.6f,%.6f],"
			"\"view_distance_squared\":%.9f,\"actor_alpha\":%.6f,\"presentation_hidden\":%s,\"primary_hidden\":%s",
			actor.actorIndex, (unsigned long long)candidate.identityKey, (unsigned long long)actor.owner.ownerWorldEpoch,
			(unsigned long long)actor.owner.ownerLifetimeGeneration, (unsigned long long)gFrame, reason,
			candidate.animationCurrent ? "true" : "false", actor.spriteAnimation.recipeCertified ? "true" : "false",
			sprite.spritetexture().GetIndex(), actor.textureId, actor.spriteAnimation.recipeTextureId, actor.physicalSectorIndex,
			(unsigned)sprite.cstat, sprite.scale.X, sprite.scale.Y, placedPosition.X, placedPosition.Y, placedPosition.Z,
			(double)view.Viewpoint.Pos.X, (double)-view.Viewpoint.Pos.Y, (double)-view.Viewpoint.Pos.Z,
			distance.LengthSquared(), (double)actor.alpha, candidate.presentationHidden ? "true" : "false", candidate.primaryHidden ? "true" : "false");
		PerfAuditWriteEvent("actor_sprite_descriptor", PerfAuditCurrentToken(), fields);
		++gDescriptorTraceCount;
	}

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
		bool luminous, uint32_t drawListType, SurfaceRef& surface, const char** rejection = nullptr, bool* primaryHidden = nullptr)
	{
		if (primaryHidden != nullptr) *primaryHidden = false;
		const auto reject = [&](const char* reason) { if (rejection != nullptr) *rejection = reason; return false; };
		if (sprite.scale.X == 0 || sprite.scale.Y == 0) return reject("zero-scale");
		if (!sprite.spritetexture().isValid()) return reject("invalid-texture-id");
		if ((sprite.cstat & CSTAT_SPRITE_INVISIBLE) != 0) return reject("invisible");
		if (sprite.sectp == nullptr) return reject("missing-sector");
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
			if (!wall.PrepareWallSprite(nullptr, &sprite, sprite.sectp, &actor.alpha)) return reject("wall-prepare");
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
			if (!facing.PrepareFacingSprite(&view, &sprite, sprite.sectp, &actor.alpha, true, primaryHidden))
			{
				const auto* art = TexMan.GetGameTexture(sprite.spritetexture());
				if (art == nullptr || !art->isValid()) return reject("invalid-art");
				return reject("facing-prepare");
			}
			texture = facing.texture;
			alpha = facing.alpha;
			style = facing.RenderStyle;
			flags |= MaterialFlag_FacingBillboard;
			surface.vertices.push_back(BuildCapturedVertex(facing.x1, facing.z1, facing.y1, facing.ul, facing.vt));
			surface.vertices.push_back(BuildCapturedVertex(facing.x2, facing.z1, facing.y2, facing.ur, facing.vt));
			surface.vertices.push_back(BuildCapturedVertex(facing.x1, facing.z2, facing.y1, facing.ul, facing.vb));
			surface.vertices.push_back(BuildCapturedVertex(facing.x2, facing.z2, facing.y2, facing.ur, facing.vb));
		}
		else return reject("unsupported-alignment");
		const bool supportedBlend = style.BlendOp == STYLEOP_Add && style.SrcAlpha == STYLEALPHA_Src &&
			style.DestAlpha == STYLEALPHA_InvSrc && style.Flags == 0;
		if (!supportedBlend) return reject("unsupported-blend");
		if (alpha <= (1.0f / 255.0f)) return reject("transparent");
		if (sprite.statnum == 99) return reject("temporary-shadow");
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
	const bool enabled = (bool)nri_ptactorspritefallback && (bool)r_voxels;
	SetActorSpriteFallbackRecipesRequested(enabled);
	if (!enabled)
	{
		ResetActorSpriteFallbackCandidates();
		return;
	}
	gFrame = frame;
	gDescriptorTraceCount = 0;
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
		retained.candidate.presentationHidden = false;
		retained.candidate.primaryHidden = false;
		retained.candidate.transientGeometryCaptured = false;
		retained.candidate.descriptorReason = "not-refreshed";
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
	// The optional snapshot recipe can be absent on the first enabled frame.
	// Capture read-only metadata for offset normalization, never replay Animate.
	if (!next.authority.spriteAnimation.available && gi != nullptr)
		gi->CaptureActorSpriteAnimation(*source.Sprite->ownerActor, next.authority.spriteAnimation);
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
	next.candidate.descriptorReason = "ready";
	next.candidate.descriptorTexture = next.sprite.spritetexture().GetIndex();
	next.candidate.recipeCertified = next.authority.spriteAnimation.recipeCertified;
	const char* rejection = "capture-prepare";
	if (!BuildSurface(di, next.sprite, *actor, next.luminous, drawListType, next.candidate.surface, &rejection, &next.candidate.primaryHidden))
	{
		TraceDescriptorRejection(next.candidate, *actor, next.sprite, di, rejection);
		return false;
	}
	if (next.candidate.primaryHidden) TraceDescriptorRejection(next.candidate, *actor, next.sprite, di, "primary-view-overlap-retained");
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
		// A transient voxel has no canonical retained BLAS, but its original
		// sprite can still serve offscreen rays. Current transient output wins
		// during representation selection, preventing a duplicate body.
		if (candidate.capturedThisFrame) continue;
		const auto* actor = LookupActorPresentationByOwnerKey(retained.authority.owner);
		if (!Eligible(actor)) { candidate.descriptorReason = "authority-unavailable"; continue; }
		if (actor->physicalSectorIndex < 0 || (unsigned)actor->physicalSectorIndex >= sector.Size())
		{
			candidate.descriptorReason = "invalid-sector";
			continue;
		}
		tspritetype sprite = retained.sprite;
		if (sprite.spriteFallbackTileBase.isValid()) sprite.setspritetexture(sprite.spriteFallbackTileBase);
		const auto& before = retained.authority;
		// Preserve game-authored presentation offsets while applying live placement.
		sprite.pos += DVector3(actor->position.x - before.position.x,
			actor->position.y - before.position.y, actor->position.z - before.position.z);
		// The captured template already contains its scripted constant offset.
		// Remove it before the pure current recipe applies the new offset once.
		if (before.spriteAnimation.recipeCertified && actor->spriteAnimation.recipeCertified)
			sprite.pos.Z -= before.spriteAnimation.recipeZOffset;
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
		if (!candidate.animationCurrent && candidate.simulationGeneration != GetActorPresentationSnapshot().simulationGeneration)
		{
			TraceDescriptorRejection(candidate, *actor, sprite, view, "recipe-uncertified");
			continue;
		}
		if (candidate.animationCurrent && (sprite.scale.X == 0 || sprite.scale.Y == 0))
		{
			candidate.presentationHidden = true;
			SetAuthority(candidate, *actor);
			TraceDescriptorRejection(candidate, *actor, sprite, view, "authored-hidden");
			continue;
		}
		auto texture = sprite.spritetexture();
		if (!(sprite.cstat2 & CSTAT2_SPRITE_NOANIMATE)) tileUpdatePicnum(texture, actor->actorIndex & 16383);
		sprite.setspritetexture(texture);
		if (sprite.cstat2 & CSTAT2_SPRITE_FULLBRIGHT) sprite.shade = -127;
		SurfaceRef surface;
		const char* rejection = "refresh-prepare";
		if (!BuildSurface(view, sprite, *actor, retained.luminous, candidate.surface.provenance.drawListType, surface, &rejection, &candidate.primaryHidden))
		{
			TraceDescriptorRejection(candidate, *actor, sprite, view, rejection);
			continue;
		}
		if (candidate.primaryHidden) TraceDescriptorRejection(candidate, *actor, sprite, view, "primary-view-overlap-retained");
		candidate.surface = std::move(surface);
		candidate.descriptorCurrent = true;
		candidate.descriptorReason = "ready";
		candidate.descriptorTexture = sprite.spritetexture().GetIndex();
		candidate.recipeCertified = actor->spriteAnimation.recipeCertified;
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
	SetActorSpriteFallbackRecipesRequested(false);
	gSprites.clear();
	gCandidates.clear();
	gFrame = 0;
}

CCMD(nri_actor_sprite_descriptor_selftest)
{
	const auto tripbomb = TexMan.CheckForTexture("TRIPBOMB", ETextureType::Any);
	const auto explosion = TexMan.CheckForTexture("EXPLOSION2", ETextureType::Any);
	if (sector.Size() == 0 || !tripbomb.isValid() || !explosion.isValid())
	{
		Printf("Actor sprite descriptor self-test: result=fail reason=load-a-Duke-map-first\n"); return;
	}
	HWDrawInfo view = {};
	view.Viewpoint.Pos = DVector3(-512, -512, 0);
	view.Viewpoint.ViewVector = DVector2(1, 0);
	// Private unlinked sector: no level mutation, clipping or wall attachment
	// from the loaded map can change these geometry assertions.
	sectortype testSector = {};
	testSector.ceilingz = -1024;
	testSector.floorz = 1024;
	ActorPresentationState actor;
	actor.actorIndex = 123;
	actor.physicalSectorIndex = 0;
	actor.position = { 4, 6, 8 };
	actor.previousPosition = { 3, 4, 5 };
	tspritetype sprite = {};
	sprite.setspritetexture(tripbomb);
	sprite.sectp = &testSector;
	sprite.pos = DVector3(4, 6, 8);
	sprite.scale = DVector2(0.25, 0.25);
	sprite.cstat = CSTAT_SPRITE_ALIGNMENT_WALL | CSTAT_SPRITE_ONE_SIDE;
	SurfaceRef wall;
	if (!BuildSurface(view, sprite, actor, false, 0, wall) || wall.vertices.size() != 4 ||
		!(wall.material.flags & MaterialFlag_OneWay) || wall.provenance.actorIndex != actor.actorIndex)
	{
		Printf("Actor sprite descriptor self-test: result=fail reason=tripbomb-wall\n"); return;
	}
	const auto originalUV = wall.vertices[0].uv[0];
	sprite.cstat |= CSTAT_SPRITE_XFLIP;
	SurfaceRef flipped;
	if (!BuildSurface(view, sprite, actor, false, 0, flipped) || flipped.vertices[0].uv[0] == originalUV)
	{
		Printf("Actor sprite descriptor self-test: result=fail reason=tripbomb-flip\n"); return;
	}
	for (const auto& vertex : flipped.vertices)
	{
		if (vertex.position[0] - vertex.prevPosition[0] != 1.0f ||
			vertex.position[1] - vertex.prevPosition[1] != -3.0f ||
			vertex.position[2] - vertex.prevPosition[2] != -2.0f)
		{
			Printf("Actor sprite descriptor self-test: result=fail reason=motion-space\n"); return;
		}
	}
	actor.alpha = 1;
	if (BuildSurface(view, sprite, actor, false, 0, flipped))
	{
		Printf("Actor sprite descriptor self-test: result=fail reason=alpha-hidden\n"); return;
	}
	actor.alpha = 0;
	sprite.setspritetexture(explosion);
	sprite.cstat = 0;
	sprite.shade = -127;
	SurfaceRef effect;
	if (!BuildSurface(view, sprite, actor, true, 0, effect) || effect.material.shade != -127 ||
		!(effect.material.flags & MaterialFlag_FacingBillboard) || !(effect.material.flags & MaterialFlag_AlphaClip))
	{
		Printf("Actor sprite descriptor self-test: result=fail reason=explosion-fullbright\n"); return;
	}
	sprite.scale = DVector2(0, 0);
	if (BuildSurface(view, sprite, actor, true, 0, effect))
	{
		Printf("Actor sprite descriptor self-test: result=fail reason=zero-scale\n"); return;
	}
	sprite.scale = DVector2(0.5, 0.5);
	view.Viewpoint.Pos = DVector3(sprite.pos.X, -sprite.pos.Y, 0);
	HWSprite facing = {};
	bool primaryHidden = false;
	if (facing.PrepareFacingSprite(&view, &sprite, sprite.sectp, &actor.alpha, false, &primaryHidden) || !primaryHidden)
	{
		Printf("Actor sprite descriptor self-test: result=fail reason=ordinary-primary-overlap\n"); return;
	}
	if (!BuildSurface(view, sprite, actor, true, 0, effect, nullptr, &primaryHidden) || !primaryHidden || effect.vertices.size() != 4 ||
		effect.vertices[0].position[2] == effect.vertices[1].position[2] || effect.vertices[0].position[1] == effect.vertices[2].position[1])
	{
		Printf("Actor sprite descriptor self-test: result=fail reason=retained-primary-overlap\n"); return;
	}
	view.Viewpoint.Pos.X += 1;
	if (!BuildSurface(view, sprite, actor, true, 0, effect, nullptr, &primaryHidden) || primaryHidden)
	{
		Printf("Actor sprite descriptor self-test: result=fail reason=primary-overlap-cleared\n"); return;
	}
	Printf("Actor sprite descriptor self-test: result=pass cases=9\n");
}
}
