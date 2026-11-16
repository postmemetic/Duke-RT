//-------------------------------------------------------------------------
/*
Copyright (C) 1996, 2003 - 3D Realms Entertainment
Copyright (C) 2020 - Christoph Oelckers

This file is part of Raze

Duke Nukem 3D is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

Original Source: 1996 - Todd Replogle
Prepared for public release: 03/21/2003 - Charlie Wiederhold, 3D Realms

*/
//-------------------------------------------------------------------------

#include "ns.h"
#include "global.h"
#include "dukeactor.h"
#include "gamefuncs.h"
#include "models/modeldata.h"

#include "buildtiles.h"
#include "actor_presentation_snapshot.h"

BEGIN_DUKE_NS

void drawshadows(tspriteArray& tsprites, tspritetype* t, DDukeActor* h)
{
	if (r_shadows && !(h->flags1 & SFLAG_NOSHADOW) && !(h->spr.cstat2 & CSTAT2_SPRITE_NOSHADOW))
	{
		auto sectp = t->sectp;
		double floorz;

		if ((sectp->lotag & 0xff) > 2 || h->spr.statnum == STAT_PROJECTILE || h->spr.statnum == STAT_MISC || (h->flags2 & SFLAG2_FLOATING))
			floorz = sectp->floorz;
		else
			floorz = h->floorz;

		const auto spactpos = getPlayer(screenpeek)->GetActor()->getPosWithOffsetZ();

		if (h->spr.pos.Z - floorz < 8 && spactpos.Z < floorz)
		{
			auto shadowspr = tsprites.newTSprite();
			*shadowspr = *t;

			shadowspr->statnum = STAT_TEMP;

			shadowspr->scale.Y = (max(t->scale.Y * 0.125, 0.0625));
			shadowspr->shade = 127;
			shadowspr->cstat |= CSTAT_SPRITE_TRANSLUCENT;

			shadowspr->pos.Z = floorz;
			shadowspr->pal = 4;

			if (hw_models && modelManager.CheckModel(t->spritetexture(), t->pal))
			{
				shadowspr->scale.Y = (0);
				// 512:trans reverse
				//1024:tell MD2SPRITE.C to use Z-buffer hacks to hide overdraw issues (todo: use a stencil to do this right.)
				shadowspr->clipdist |= TSPR_FLAGS_MDHACK;
				shadowspr->cstat |= CSTAT_SPRITE_TRANS_FLIP;
			}
			else
			{
				// Alter the shadow's position so that it appears behind the sprite itself.
				auto look = (shadowspr->pos.XY() - spactpos.XY()).Angle();
				shadowspr->pos += look.ToVector() * 2;
			}
		}
	}
}

// ---------------------------------------------------------------------------
//
// some ugly stuff here: RRRA forces some animations fullbright, 
// but there is no good way to set this up for CON in any decent way.
// These frames are being hacked in here. No need to make this configurable, though. 
// For our new format this can be done as a real feature.
//
// ---------------------------------------------------------------------------

bool RRRAFullbrightHack(tspritetype* t, int k)
{
	if (t->ownerActor->IsKindOf(RedneckBillyRayClass))
	{
		return k >= 102 && k <= 151;
	}
	else if (t->ownerActor->IsKindOf(RedneckBikerClass))
	{
		return (k >= 54 && k <= 58) || (k >= 84 && k <= 88);
	}
	else if (t->ownerActor->IsKindOf(RedneckCheerleaderClass))
	{
		return k >= 102 && k <= 151;
	}
	return false;
}

static void ResolveSpriteAnimationFrame(tspritetype* t, const ActorSpriteAnimationState& animation, const DVector2& viewVec, DAngle viewang, bool suppressDirection)
{
	if (animation.enabled)// && (t->cstat & CSTAT_SPRITE_ALIGNMENT_MASK) != CSTAT_SPRITE_ALIGNMENT_SLAB)
	{
		DAngle kang;
		int k = 0, l = 0;
		if (animation.actionActive)
		{
			l = animation.rotationType;

			if (suppressDirection)
			{
				k = 0;
				t->cstat &= ~CSTAT_SPRITE_XFLIP;
			}
			else switch (l)
			{
			case 2:
				k = angletorotation1(t->Angles.Yaw, viewang, 8, 1);
				break;

			case 3:
			case 4:
				k = angletorotation1(t->Angles.Yaw, viewang, 7);
				if (k > 3)
				{
					t->cstat |= CSTAT_SPRITE_XFLIP;
					k = 7 - k;
				}
				else t->cstat &= ~CSTAT_SPRITE_XFLIP;
				break;

			case 5:
				kang = (t->pos.XY() - viewVec).Angle();
				k = angletorotation1(t->Angles.Yaw, kang);
				if (k > 4)
				{
					k = 8 - k;
					t->cstat |= CSTAT_SPRITE_XFLIP;
				}
				else t->cstat &= ~CSTAT_SPRITE_XFLIP;
				break;
			case 7:
				kang = (t->pos.XY() - viewVec).Angle();
				k = angletorotation2(t->Angles.Yaw, kang);
				if (k > 6)
				{
					k = 12 - k;
					t->cstat |= CSTAT_SPRITE_XFLIP;
				}
				else t->cstat &= ~CSTAT_SPRITE_XFLIP;
				break;
			case 8:
				k = angletorotation1(t->Angles.Yaw, viewang);
				t->cstat &= ~CSTAT_SPRITE_XFLIP;
				break;
			default:
				if (animation.rrZombieRotation)
				{
					kang = (t->pos.XY() - viewVec).Angle();
					k = angletorotation1(t->Angles.Yaw, kang);
					if (k > 4) { k = 8 - k; t->cstat |= CSTAT_SPRITE_XFLIP; }
					else t->cstat &= ~CSTAT_SPRITE_XFLIP;
				}
				break;
			}

			k += animation.actionOffset + l * animation.currentFrame;
			int texid = t->spritetexture().GetIndex() + k; // we cannot work with texture IDs here because their arithmetics are limited.

			if ((animation.fullbrightKind == 1 && k >= 102 && k <= 151) ||
				(animation.fullbrightKind == 2 && ((k >= 54 && k <= 58) || (k >= 84 && k <= 88)))) t->shade = -127;

			FGameTexture* tex = TexMan.GameByIndex(texid);
			if (l > 0)
			{
				while(1)
				{
					if (!tex || tex->isValid()) break;
					texid -= l;       //back up one frame if this one is invald.
					tex = TexMan.GameByIndex(texid);
				}
			}

			if (!tex)
			{
				t->setspritetexture(FNullTextureID());
				t->scale = DVector2(0, 0);
			}
			else t->setspritetexture(tex->GetID());

		}

	}
}

void applyanimations(tspritetype* t, DDukeActor* h, const DVector2& viewVec, DAngle viewang)
{
	tspritetype alternate = *t;
	alternate.ApplySpriteFallbackFrame();
	ActorSpriteAnimationState animation;
	gi->CaptureActorSpriteAnimation(*h, animation);
	t->spriteFallbackActionBase = alternate.spritetexture();
	t->spriteFallbackActionShade = alternate.shade;
	t->spriteFallbackActionApplied = animation.enabled && animation.actionActive;
	ResolveSpriteAnimationFrame(&alternate, animation, viewVec, viewang, false);
	t->spriteFallbackActionResolvedShade = alternate.shade;
	ResolveSpriteAnimationFrame(t, animation, viewVec, viewang,
		tilehasmodelorvoxel(h->spr.spritetexture(), h->spr.pal) != 0);
	t->SetSpriteFallbackFrame(alternate);

	// These are game-visible side effects of the original animation path.
	// Resolve the presentation-only alternate without repeating them.
	if (!(h->flags2 & SFLAG2_DONTANIMATE))
	{
		if (h->curAction->name != NAME_None && h->dispictex.isValid())
			h->dispictex = t->spritetexture();
		if ((h->flags4 & SFLAG4_FLASHFRAME0) && t->spritetexture() == h->spr.spritetexture())
		{
			getPlayer(screenpeek)->visibility = -127;
			lastvisinc = PlayClock + 32;
		}
	}
}

void GameInterface::CaptureActorSpriteAnimation(const DCoreActor& actor, ActorSpriteAnimationState& out) const
{
	out = {};
	if (!actor.IsKindOf(RUNTIME_CLASS(DDukeActor))) return;
	const auto& h = static_cast<const DDukeActor&>(actor);
	out.available = true;
	out.enabled = (h.flags2 & SFLAG2_DONTANIMATE) == 0;
	out.actionActive = h.curAction != nullptr && h.curAction->name != NAME_None;
	if (h.curAction != nullptr)
	{
		out.rotationType = h.curAction->rotationtype;
		out.actionOffset = h.curAction->offset;
	}
	out.currentFrame = h.curframe;
	out.forcedRotation = (h.flags2 & SFLAG2_ALWAYSROTATE1) ? 8 :
		(h.flags2 & SFLAG2_ALWAYSROTATE2) ? 12 : 0;
	out.rrZombieRotation = isRR() && badguy(&h) && h.spr.statnum == STAT_ZOMBIEACTOR && h.spr.extra > 0;
	if (isRRRA())
	{
		if (h.IsKindOf(RedneckBillyRayClass) || h.IsKindOf(RedneckCheerleaderClass)) out.fullbrightKind = 1;
		else if (h.IsKindOf(RedneckBikerClass)) out.fullbrightKind = 2;
	}
}

bool GameInterface::ResolveActorSpriteFallbackFrame(const ActorPresentationState& state, tspritetype& sprite,
	const DVector2& viewVec, DAngle viewang) const
{
	if (!state.spriteAnimation.available) return false;
	const auto& animation = state.spriteAnimation;
	int forcedRotation = sprite.spriteFallbackRotationFrames != 0 ? sprite.spriteFallbackRotationFrames : animation.forcedRotation;
	if (sprite.clipdist & TSPR_ROTATE8FRAMES) forcedRotation = 8;
	else if (sprite.clipdist & TSPR_ROTATE12FRAMES) forcedRotation = 12;
	if (forcedRotation != 0)
	{
		int k = forcedRotation == 8 ? angletorotation1(sprite.Angles.Yaw, viewang) : angletorotation2(sprite.Angles.Yaw, viewang);
		const int half = forcedRotation / 2;
		if (k > half) { k = forcedRotation - k; sprite.cstat |= CSTAT_SPRITE_XFLIP; }
		else sprite.cstat &= ~CSTAT_SPRITE_XFLIP;
		FTextureID base = sprite.spriteFallbackRotationBase;
		if (!base.isValid()) base.SetIndex(state.textureId);
		sprite.setspritetexture(base + k);
	}
	else if (sprite.spriteFallbackActionApplied)
	{
		sprite.setspritetexture(sprite.spriteFallbackActionBase);
	}
	if (sprite.spriteFallbackActionApplied)
	{
		if (sprite.shade == sprite.spriteFallbackActionResolvedShade) sprite.shade = sprite.spriteFallbackActionShade;
		ResolveSpriteAnimationFrame(&sprite, animation, viewVec, viewang, false);
	}
	return sprite.spriteFallbackActionApplied || forcedRotation != 0 || sprite.spritetexture().GetIndex() == state.textureId;
}

END_DUKE_NS
