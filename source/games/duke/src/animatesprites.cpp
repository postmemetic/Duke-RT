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
#include "enemy_voxel_family.h"
#include "vm.h"
#include "c_dispatch.h"
#include <unordered_map>
#include <cstring>

EXTERN_CVAR(Bool, wt_commentary)
CVAR(Int, duke_sprite_recipe_checks, 0, 0)

// Native setter touches only the supplied temporary sprite; the self-test uses
// it as the ordinary-path oracle without invoking Animate or mutating an actor.
void tspritetype_setSpritePic(tspritetype* sprite, DCoreActor* actor, unsigned index);

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

static void CaptureSpriteAction(const DDukeActor& h, ActorSpriteAnimationState& out);

void applyanimations(tspritetype* t, DDukeActor* h, const DVector2& viewVec, DAngle viewang)
{
	tspritetype alternate = *t;
	alternate.ApplySpriteFallbackFrame();
	ActorSpriteAnimationState animation;
	CaptureSpriteAction(*h, animation);
	t->spriteFallbackActionBase = alternate.spritetexture();
	t->spriteFallbackActionShade = alternate.shade;
	t->spriteFallbackActionApplied = animation.enabled && animation.actionActive;
	ResolveSpriteAnimationFrame(&alternate, animation, viewVec, viewang, false);
	t->spriteFallbackActionResolvedShade = alternate.shade;
	ResolveSpriteAnimationFrame(t, animation, viewVec, viewang,
		tilehasmodelorvoxel(h->spr.spritetexture(), h->spr.pal) != 0);
	if (animation.recipeTextureId >= 0)
	{
		FTextureID voxelStart;
		voxelStart.SetIndex(animation.recipeTextureId);
		t->setspritetexture(voxelStart);
	}
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

static void CaptureSpriteAction(const DDukeActor& h, ActorSpriteAnimationState& out)
{
	out = {};
	out.available = true;
	out.enabled = (h.flags2 & SFLAG2_DONTANIMATE) == 0;
	out.actionActive = h.curAction != nullptr && h.curAction->name != NAME_None;
	// The snapshot's default recipe uses this image for a retained voxel.
	// Both the normal and retained 2D paths keep the original marker image.
	if (!out.actionActive && (h.spr.picnum == 2711 || h.spr.picnum == 2761))
		out.recipeTextureId = ResolveEnemyVoxelDirectionalStart(h);
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

enum class SpriteRecipe { Unknown, Default, Marker, ForceSphere, Destructible, BloodPool, Puke, BloodSplat, Neon, Recon, Scrap,
	Shade16, Commentary, Shell, Jibs, ClockSpark, CoolExplosion, FirelaserTrail, CrystalAmmo, AtomicHealth, LyingDead,
	WaterBubble, LaserLine, Fire, Burning };

static int ResolveScrapSpriteFrame(int spriteExtra, int counter)
{
	const int frame = spriteExtra + counter;
	return frame >= 0 && frame < 30 ? frame : 18; // ScrapMax / Scrap3 in scrap.zs.
}

static bool ScrapSpriteFrameIsBrighter(int frame)
{
	static const bool brighter[] = { 1,1,1,1,1,1,1,1,0,0,1,0,0,0,1,0,0,0,1,0,0,0,1,0,0,0,1,0,0,0 };
	return brighter[frame];
}

static SpriteRecipe GetSpriteRecipe(const DDukeActor& actor)
{
	static const unsigned slot = GetVirtualIndex(RUNTIME_CLASS(DDukeActor), "animate");
	const auto* cls = actor.GetClass();
	if (slot == ~0u || slot >= cls->Virtuals.Size()) return SpriteRecipe::Unknown;
	const auto* function = cls->Virtuals[slot];
	struct CachedRecipe { const VMFunction* function; SpriteRecipe recipe; };
	static std::unordered_map<const PClass*, CachedRecipe> cache;
	const auto found = cache.find(cls);
	if (found != cache.end() && found->second.function == function) return found->second.recipe;
	SpriteRecipe recipe = function == RUNTIME_CLASS(DDukeActor)->Virtuals[slot] ? SpriteRecipe::Default : SpriteRecipe::Unknown;
	if (recipe == SpriteRecipe::Unknown)
	{
		const struct { const char* name; SpriteRecipe recipe; } known[] = {
			{ "DukeRespawnMarker", SpriteRecipe::Marker }, { "DukeForceSphere", SpriteRecipe::ForceSphere },
			{ "DukeGenericDestructible", SpriteRecipe::Destructible }, { "DukeBloodPool", SpriteRecipe::BloodPool },
			{ "DukePuke", SpriteRecipe::Puke }, { "DukeBloodSplat1", SpriteRecipe::BloodSplat },
			{ "DukeNeon", SpriteRecipe::Neon }, { "DukeRecon", SpriteRecipe::Recon },
			{ "DukeScrap", SpriteRecipe::Scrap }, { "DukeFootprints", SpriteRecipe::BloodPool },
			{ "DukeCrack", SpriteRecipe::Shade16 }, { "DukeBulletHole", SpriteRecipe::Shade16 },
			{ "DeveloperCommentary", SpriteRecipe::Commentary }, { "DukeShell", SpriteRecipe::Shell },
			{ "DukeJibs1", SpriteRecipe::Jibs }, { "DukeShrinkSpark", SpriteRecipe::ClockSpark },
			{ "DukeGrowSpark", SpriteRecipe::ClockSpark }, { "DukeCoolExplosion1", SpriteRecipe::CoolExplosion },
			{ "DukeFirelaserTrail", SpriteRecipe::FirelaserTrail }, { "DukeCrystalAmmo", SpriteRecipe::CrystalAmmo },
			{ "DukeAtomicHealth", SpriteRecipe::AtomicHealth }, { "DukePlayerLyingDead", SpriteRecipe::LyingDead },
			{ "DukeWaterBubble", SpriteRecipe::WaterBubble }, { "DukeLaserLine", SpriteRecipe::LaserLine },
			{ "DukeFire", SpriteRecipe::Fire }, { "DukeBurning", SpriteRecipe::Burning }
		};
		for (const auto& entry : known)
		{
			const auto* builtin = PClass::FindClass(entry.name);
			if (builtin != nullptr && slot < builtin->Virtuals.Size() && function == builtin->Virtuals[slot])
			{
				recipe = entry.recipe;
				break;
			}
		}
	}
	cache[cls] = { function, recipe };
	return recipe;
}

static void CaptureSpriteSetFrame(const DDukeActor& actor, int index, ActorSpriteAnimationState& out)
{
	const auto& sprites = static_cast<PClassActor*>(actor.GetClass())->ActorInfo()->SpriteSet;
	if ((unsigned)index < sprites.Size())
	{
		out.recipeTextureId = sprites[index].GetIndex();
		out.recipeTextureSet = true;
	}
	else if (index == -1)
	{
		out.recipeTextureId = actor.dispictex.GetIndex();
		out.recipeTextureSet = true;
	}
	// SetSpritePic leaves its input unchanged for every other out-of-range
	// index. In particular ShotgunShell has one entry but requests counter&1.
}

static void CaptureShellSpriteRecipe(const DDukeActor& actor, int counter, ActorSpriteAnimationState& out)
{
	out.recipeCertified = true;
	const auto& sprites = static_cast<PClassActor*>(actor.GetClass())->ActorInfo()->SpriteSet;
	if (sprites.Size() > 0) CaptureSpriteSetFrame(actor, counter & 1, out);
	out.recipeCstatClear = (uint16_t)CSTAT_SPRITE_XFLIP | (uint16_t)CSTAT_SPRITE_YFLIP;
	out.recipeCstatSet = counter <= 1 ? out.recipeCstatClear : counter <= 2 ? (uint16_t)CSTAT_SPRITE_YFLIP : 0;
}

void GameInterface::CaptureActorSpriteAnimation(const DCoreActor& actor, ActorSpriteAnimationState& out) const
{
	out = {};
	if (!actor.IsKindOf(RUNTIME_CLASS(DDukeActor))) return;
	const auto& h = static_cast<const DDukeActor&>(actor);
	CaptureSpriteAction(h, out);
	CaptureEnemyVoxelFamily(h, out);
	out.voxelPoseInterpolated = !(h.flags1 & SFLAG_NOINTERPOLATE);
	// Player-specific view/weapon copies and Redneck's additional native rules
	// require their own recipes. Keep their freshly captured descriptors usable,
	// without certifying an arbitrary old presentation for a later tick.
	if (isRR() || h.isPlayer() || h.sector() == nullptr || h.GetClass() == DukeSectorEffectorClass) return;
	const auto* sect = h.sector();
	out.recipeShade = h.spr.shade;
	out.recipePalette = h.spr.pal;
	const bool keepShade = (!(h.flags2 & SFLAG2_FORCESECTORSHADE) && (h.spr.cstat & CSTAT_SPRITE_ALIGNMENT_WALL)) ||
		(badguy(&h) && h.spr.extra > 0) || h.spr.statnum == STAT_PLAYER;
	if (keepShade)
	{
		if (sect->shadedsector == 1 && h.spr.statnum != STAT_ACTOR) out.recipeShade = 16;
	}
	else out.recipeShade = clamp<int>(sect->ceilingstat & CSTAT_SECTOR_SKY ? sect->ceilingshade : sect->floorshade, -127, 127);

	// Match the actual virtual implementation, not merely an ancestor class:
	// a mod overriding Animate must not inherit a false built-in certificate.
	const auto recipe = GetSpriteRecipe(h);
	const auto spriteSetFrame = [&](int index)
	{
		CaptureSpriteSetFrame(h, index, out);
	};
	out.recipeHandled = recipe != SpriteRecipe::Default;
	if (!out.recipeHandled) out.recipeCertified = true;
	else if (recipe == SpriteRecipe::Marker)
	{
		out.recipeCertified = true;
		out.recipeHidden = ud.marker == 0;
	}
	else if (recipe == SpriteRecipe::ForceSphere)
	{
		out.recipeCertified = true;
		const auto* owner = h.ownerActor.Get();
		const auto* player = getPlayer(screenpeek)->GetActor();
		out.forceSphereChild = h.spr.statnum == STAT_MISC && owner != nullptr && player != nullptr;
		if (out.forceSphereChild)
		{
			out.sphereOwnerPosition = { owner->spr.pos.X, owner->spr.pos.Y };
			out.viewPlayerPosition = { player->spr.pos.X, player->spr.pos.Y };
		}
	}
	else if (recipe == SpriteRecipe::Destructible)
	{
		// ScriptVar's API lacks const access; these are reads only, never VM calls.
		const auto* fullbright = static_cast<const int*>(const_cast<DDukeActor&>(h).ScriptVar(NAME_fullbright, nullptr));
		const auto* broken = static_cast<const int*>(const_cast<DDukeActor&>(h).ScriptVar(FName("broken"), nullptr));
		out.recipeCertified = fullbright != nullptr && broken != nullptr && *broken >= 0 && *broken < 32;
		if (out.recipeCertified && ((uint32_t)*fullbright & (1u << *broken))) out.recipeShade = -127;
	}
	else if (recipe == SpriteRecipe::BloodPool)
	{
		out.recipeCertified = true;
		if (h.spr.shade == 127) out.recipeShade = 127;
		if (h.spr.pal == 6) out.recipeShade = -127;
	}
	else if (recipe == SpriteRecipe::Puke)
	{
		out.recipeCertified = true;
		if (h.spr.shade == 127) out.recipeShade = 127;
	}
	else if (recipe == SpriteRecipe::BloodSplat)
	{
		out.recipeCertified = true;
		out.recipeShade = h.spr.pal == 6 ? -127 : 16;
	}
	else if (recipe == SpriteRecipe::Neon)
	{
		out.recipeCertified = true;
		out.recipeShade = h.spr.shade;
	}
	else if (recipe == SpriteRecipe::Recon)
	{
		const auto& sprites = static_cast<PClassActor*>(h.GetClass())->ActorInfo()->SpriteSet;
		const unsigned index = abs(h.temp_data[3]) > 64 ? 1u : 0u;
		out.recipeCertified = index < sprites.Size();
		if (out.recipeCertified) out.recipeTextureId = sprites[index].GetIndex();
	}
	else if (recipe == SpriteRecipe::Scrap)
	{
		out.recipeCertified = true;
		out.recipeEmptyArtHides = true;
		out.recipeTextureId = h.spr.spritetexture().GetIndex();
		if (h.spriteextra == 10 && h.spr.yint > 0)
		{
			// setWeaponOrAmmoSprite is a no-op outside its 15-element table.
			if (h.spr.yint <= 15 && gs.weaponsandammosprites[h.spr.yint - 1] != nullptr)
				out.recipeTextureId = GetDefaultByType(gs.weaponsandammosprites[h.spr.yint - 1])->spr.spritetexture().GetIndex();
		}
		else
		{
			const auto& sprites = static_cast<PClassActor*>(h.GetClass())->ActorInfo()->SpriteSet;
			const unsigned frame = ResolveScrapSpriteFrame(h.spriteextra, h.counter);
			out.recipeCertified = frame < sprites.Size();
			if (out.recipeCertified) out.recipeTextureId = sprites[frame].GetIndex();
			if (ScrapSpriteFrameIsBrighter(frame)) out.recipeShade -= 6;
		}
	}
	else if (recipe == SpriteRecipe::Shade16)
	{
		out.recipeCertified = true;
		out.recipeShade = 16;
	}
	else if (recipe == SpriteRecipe::Commentary)
	{
		out.recipeCertified = true;
		out.recipeHidden = !(bool)wt_commentary;
		if (!out.recipeHidden) out.recipeShade = h.spr.shade;
	}
	else if (recipe == SpriteRecipe::Shell)
	{
		CaptureShellSpriteRecipe(h, h.counter, out);
	}
	else if (recipe == SpriteRecipe::Jibs)
	{
		out.recipeCertified = true;
		if (h.spritesetindex == 1) spriteSetFrame(1 + h.counter);
		if (h.spr.pal == 6) out.recipeShade = -120;
		if (sect->shadedsector == 1) out.recipeShade = 16;
	}
	else if (recipe == SpriteRecipe::ClockSpark || recipe == SpriteRecipe::CoolExplosion)
	{
		out.recipeCertified = true;
		spriteSetFrame(recipe == SpriteRecipe::ClockSpark ? (PlayClock >> 4) & 3 : clamp<int>(h.spr.shade >> 1, 0, 19));
	}
	else if (recipe == SpriteRecipe::FirelaserTrail)
	{
		// Duke's Animate changes only self.extra. The ordinary call remains the
		// sole writer; its presentation itself is a no-op outside Redneck.
		out.recipeCertified = true;
	}
	else if (recipe == SpriteRecipe::CrystalAmmo || recipe == SpriteRecipe::AtomicHealth || recipe == SpriteRecipe::LyingDead)
	{
		out.recipeCertified = true;
		out.recipeHandled = false;
		if (recipe == SpriteRecipe::CrystalAmmo) out.recipeShade = int(BobVal(PlayClock << 4) * 16);
		else out.recipeZOffset = recipe == SpriteRecipe::AtomicHealth ? -4 : 24;
	}
	else if (recipe == SpriteRecipe::WaterBubble)
	{
		out.recipeCertified = true;
		out.recipeHandled = false;
		if (tilesurface(sect->floortexture) == TSURF_SLIME) out.recipePalette = 7;
		else if (!lookups.noFloorPal(sect->floorpal)) out.recipePalette = SectorPaletteForRendering(sect, out.recipePalette);
	}
	else if (recipe == SpriteRecipe::LaserLine)
	{
		out.recipeCertified = true;
		const auto* owner = h.ownerActor.Get();
		if (owner != nullptr)
		{
			if (sect->lotag == 2) out.recipePalette = 8;
			out.recipePositionZMode = 2;
			out.recipePositionZ = owner->spr.pos.Z - 3;
			out.recipeHideYOnly = gs.lasermode == 2 && getPlayer(screenpeek)->heat_on == 0;
		}
	}
	else if (recipe == SpriteRecipe::Fire || recipe == SpriteRecipe::Burning)
	{
		const auto* owner = h.ownerActor.Get();
		// Local-player burning has a mirror/view-specific visibility contract.
		// Do not turn that into universal absence or revive it in the main view.
		if (recipe == SpriteRecipe::Burning && owner != nullptr && owner->spr.statnum == STAT_PLAYER) return;
		out.recipeCertified = true;
		out.recipeHandled = false;
		out.recipeCstatSet = CSTAT_SPRITE_YCENTER;
		if (recipe == SpriteRecipe::Fire || !(h.flags1 & SFLAG_NOFLOORFIRE))
		{
			out.recipePositionZMode = 1;
			out.recipePositionZ = getflorzofslopeptr(sect, h.spr.pos.X, h.spr.pos.Y);
			out.recipeFloorSlope = { getflorzofslopeptr(sect, h.spr.pos.X + 1, h.spr.pos.Y) - out.recipePositionZ,
				getflorzofslopeptr(sect, h.spr.pos.X, h.spr.pos.Y + 1) - out.recipePositionZ };
		}
	}
	if (!out.recipeCertified) return;
	if (sect->floorpal && !(h.flags2 & SFLAG2_NOFLOORPAL) && !lookups.noFloorPal(sect->floorpal))
		out.recipeFloorPalette = SectorPaletteForRendering(sect, out.recipePalette);
	if (!out.recipeHandled)
	{
		out.recipeNativeHidden = sect->floortexture == mirrortex;
		const auto* player = getPlayer(screenpeek);
		out.heatVision = (h.spr.statnum == STAT_DUMMYPLAYER || badguy(&h)) && player->heat_amount > 0 && player->heat_on;
	}
}

static bool ApplySpritePresentationRecipe(const ActorPresentationState& state, tspritetype& sprite)
{
	if (!state.spriteAnimation.available) return false;
	const auto& animation = state.spriteAnimation;
	if (!animation.recipeCertified) return false;
	sprite.shade = animation.recipeShade;
	sprite.pal = animation.recipePalette;
	if (animation.recipeTextureSet || animation.recipeTextureId >= 0)
	{
		FTextureID texture;
		texture.SetIndex(animation.recipeTextureId);
		sprite.setspritetexture(texture);
	}
	sprite.cstat = (sprite.cstat & ~ESpriteFlags::FromInt(animation.recipeCstatClear)) |
		ESpriteFlags::FromInt(animation.recipeCstatSet);
	sprite.pos.Z += animation.recipeZOffset;
	if (animation.recipePositionZMode == 1)
		sprite.pos.Z = animation.recipePositionZ + (sprite.pos.X - state.position.x) * animation.recipeFloorSlope.x +
			(sprite.pos.Y - state.position.y) * animation.recipeFloorSlope.y;
	else if (animation.recipePositionZMode == 2) sprite.pos.Z = animation.recipePositionZ;
	bool hidden = animation.recipeHidden;
	if (animation.forceSphereChild)
	{
		const DVector2 owner(animation.sphereOwnerPosition.x, animation.sphereOwnerPosition.y);
		const DVector2 player(animation.viewPlayerPosition.x, animation.viewPlayerPosition.y);
		const auto toPlayer = owner - player;
		const auto toSprite = owner - sprite.pos.XY();
		if (absangle(toPlayer.Angle(), toSprite.Angle()) > DAngle::fromDeg(90) && toSprite.LengthSquared() < toPlayer.LengthSquared()) hidden = true;
	}
	if (hidden) sprite.scale = DVector2(0, 0);
	else if (animation.recipeHideYOnly) sprite.scale.Y = 0;
	return true;
}

bool GameInterface::ResolveActorRetainedVoxelFrame(const ActorPresentationState& state, tspritetype& sprite,
	double interpolation) const
{
	const auto& animation = state.spriteAnimation;
	const auto& family = animation.enemyVoxelFamily;
	if (!animation.enemy || !family || !family->complete || !animation.recipeCertified) return false;
	sprite.ownerActor = nullptr;
	sprite.pos = DVector3(state.position.x, state.position.y, state.position.z);
	sprite.Angles.Yaw = DAngle::fromDeg(state.angles.yawDegrees);
	if (animation.voxelPoseInterpolated)
	{
		sprite.pos = interpolatedvalue(DVector3(state.previousPosition.x, state.previousPosition.y, state.previousPosition.z),
			sprite.pos, interpolation);
		sprite.Angles.Yaw = interpolatedvalue(DAngle::fromDeg(state.previousAngles.yawDegrees), sprite.Angles.Yaw, interpolation);
	}
	sprite.scale = DVector2(state.scale.x, state.scale.y);
	sprite.xoffset = state.spriteXOffset; sprite.yoffset = state.spriteYOffset;
	sprite.cstat = ESpriteFlags::FromInt(uint16_t(state.cstat)); sprite.cstat2 = state.cstat2;
	sprite.statnum = state.statNumber;
	FTextureID base; base.SetIndex(state.textureId); sprite.setspritetexture(base);
	if (!ApplySpritePresentationRecipe(state, sprite)) return false;
	// The ordinary mounted-voxel path suppresses directional ART selection.
	// Reuse its action resolver; do not run Animate or any gameplay callback.
	if (animation.forcedRotation != 0 || (sprite.clipdist & (TSPR_ROTATE8FRAMES | TSPR_ROTATE12FRAMES)))
		sprite.cstat &= ~CSTAT_SPRITE_XFLIP;
	if (animation.recipeFloorPalette >= 0 && !(sprite.clipdist & TSPR_NOFLOORPAL)) sprite.pal = animation.recipeFloorPalette;
	if (!animation.recipeHandled) ResolveSpriteAnimationFrame(&sprite, animation, {}, {}, true);
	if (animation.heatVision) { sprite.pal = 6; sprite.shade = 0; }
	if (animation.recipeNativeHidden) sprite.scale = DVector2(0, 0);
	return true;
}

bool GameInterface::ResolveActorSpriteFallbackFrame(const ActorPresentationState& state, tspritetype& sprite,
	const DVector2& viewVec, DAngle viewang) const
{
	if (!ApplySpritePresentationRecipe(state, sprite)) return false;
	const auto& animation = state.spriteAnimation;
	int forcedRotation = animation.forcedRotation;
	if (sprite.clipdist & TSPR_ROTATE8FRAMES) forcedRotation = 8;
	else if (sprite.clipdist & TSPR_ROTATE12FRAMES) forcedRotation = 12;
	if (forcedRotation != 0)
	{
		int k = forcedRotation == 8 ? angletorotation1(sprite.Angles.Yaw, viewang) : angletorotation2(sprite.Angles.Yaw, viewang);
		const int half = forcedRotation / 2;
		if (k > half) { k = forcedRotation - k; sprite.cstat |= CSTAT_SPRITE_XFLIP; }
		else sprite.cstat &= ~CSTAT_SPRITE_XFLIP;
		FTextureID base = sprite.spriteFallbackRotationBase;
		// Ordinary applyRotation1/2 uses the actor texture (or an explicit
		// caller base), even when Animate just selected another SpriteSet image.
		// Preserve that ordering instead of using recipeTextureId here.
		if (!base.isValid()) base.SetIndex(state.textureId);
		sprite.setspritetexture(base + k);
	}
	else if (!animation.recipeHandled)
	{
		FTextureID base;
		base.SetIndex(state.textureId);
		sprite.setspritetexture(base);
	}
	if (animation.recipeFloorPalette >= 0 && !(sprite.clipdist & TSPR_NOFLOORPAL)) sprite.pal = animation.recipeFloorPalette;
	if (!animation.recipeHandled)
	{
		ResolveSpriteAnimationFrame(&sprite, animation, viewVec, viewang, false);
	}
	if (animation.heatVision) { sprite.pal = 6; sprite.shade = 0; }
	bool hidden = animation.recipeNativeHidden;
	// A certified script can select authored empty art (e.g. SCRAP6I is 0x0).
	// This is intentional absence, not failed GPU texture admission. Holding a
	// previous voxel here would revive a body the ordinary sprite path hides.
	if (animation.recipeEmptyArtHides)
	{
		const auto* resolvedTexture = TexMan.GetGameTexture(sprite.spritetexture());
		if (resolvedTexture == nullptr || !resolvedTexture->isValid()) hidden = true;
	}
	if (hidden) sprite.scale = DVector2(0, 0);
	return true;
}

// Compare against the one normal VM invocation. This never invokes Animate a
// second time or mutates an actor/random stream. A session-only budget makes
// the copy and metadata capture diagnostic work, absent from normal frames.
void CheckActorSpriteRecipeParity(DDukeActor* actor, const tspritetype& before, const tspritetype& actual, bool handled)
{
	if ((int)duke_sprite_recipe_checks <= 0 || actor == nullptr || gi == nullptr ||
		GetSpriteRecipe(*actor) == SpriteRecipe::Default) return;
	ActorPresentationState state;
	state.position = { actor->spr.pos.X, actor->spr.pos.Y, actor->spr.pos.Z };
	gi->CaptureActorSpriteAnimation(*actor, state.spriteAnimation);
	if (!state.spriteAnimation.recipeCertified) return;
	auto predicted = before;
	const bool applied = ApplySpritePresentationRecipe(state, predicted);
	const auto delta = predicted.pos - actual.pos;
	const bool pass = applied && handled == state.spriteAnimation.recipeHandled &&
		predicted.spritetexture() == actual.spritetexture() && predicted.shade == actual.shade && predicted.pal == actual.pal &&
		predicted.cstat == actual.cstat && predicted.scale == actual.scale && delta.LengthSquared() < 1e-12 &&
		predicted.Angles.Yaw == actual.Angles.Yaw;
	duke_sprite_recipe_checks = (int)duke_sprite_recipe_checks - 1;
	Printf("Duke sprite recipe self-test: result=%s class=%s remaining=%d texture=%d/%d shade=%d/%d pal=%d/%d cstat=%u/%u z=%.9f/%.9f handled=%d/%d\n",
		pass ? "pass" : "fail", actor->GetClass()->TypeName.GetChars(), (int)duke_sprite_recipe_checks,
		predicted.spritetexture().GetIndex(), actual.spritetexture().GetIndex(), (int)predicted.shade, (int)actual.shade,
		(int)predicted.pal, (int)actual.pal, (unsigned)predicted.cstat, (unsigned)actual.cstat,
		predicted.pos.Z, actual.pos.Z, (int)state.spriteAnimation.recipeHandled, (int)handled);
}

// Exercises the production ordinary sprite resolver against retained refresh.
// No actor, VM callback, random stream, global voxel toggle or simulation write.
CCMD(duke_sprite_fallback_selftest)
{
	const auto base = TexMan.CheckForTexture("LIZTROOP", ETextureType::Any);
	if (!base.isValid() || gi == nullptr)
	{
		Printf("Duke sprite fallback self-test: result=fail reason=load-a-Duke-map-first\n");
		return;
	}
	uint32_t cases = 0, missingFrames = 0;
	const auto equal = [](const tspritetype& a, const tspritetype& b)
	{
		return a.spritetexture() == b.spritetexture() && a.cstat == b.cstat &&
			a.scale == b.scale && a.shade == b.shade && a.pal == b.pal;
	};
	const int rotations[] = { 0, 2, 3, 4, 5, 7, 8 };
	const int frames[] = { 0, 1, 3, 16, 8192 };
	const int forced[] = { 0, 8, 12 };
	for (int rotation : rotations) for (int frame : frames) for (int direction : forced)
		for (int angle = -17; angle < 2048; angle += 63)
	{
		ActorPresentationState state;
		state.textureId = base.GetIndex();
		auto& animation = state.spriteAnimation;
		animation.available = animation.enabled = animation.actionActive = animation.recipeCertified = true;
		animation.rotationType = rotation;
		animation.currentFrame = frame;
		animation.forcedRotation = direction;
		animation.recipeShade = 23;
		animation.recipePalette = 2;
		tspritetype raw = {};
		raw.setspritetexture(base);
		raw.spriteFallbackRotationBase = base;
		raw.shade = 23;
		raw.pal = 2;
		raw.scale = DVector2(0.5, 0.75);
		raw.pos = DVector3(64, 32, 0);
		raw.Angles.Yaw = DAngle::fromBuild(angle);
		raw.cstat |= CSTAT_SPRITE_XFLIP;
		const auto viewAngle = DAngle::fromBuild(97);
		const DVector2 view(-24, 11);
		auto expected = raw;
		if (direction == 8) ResolveSpriteRotation1(nullptr, &expected, viewAngle, base, false);
		if (direction == 12) ResolveSpriteRotation2(nullptr, &expected, viewAngle, base, false);
		ResolveSpriteAnimationFrame(&expected, animation, view, viewAngle, false);
		auto retained = raw;
		if (!gi->ResolveActorSpriteFallbackFrame(state, retained, view, viewAngle) || !equal(expected, retained))
		{
			Printf("Duke sprite fallback self-test: result=fail reason=parity rotation=%d frame=%d forced=%d angle=%d\n", rotation, frame, direction, angle);
			return;
		}
		++cases;
		if (retained.scale.X == 0 || retained.scale.Y == 0) ++missingFrames;
	}
	ActorPresentationState state;
	state.textureId = base.GetIndex();
	state.spriteAnimation.available = state.spriteAnimation.recipeCertified = true;
	state.spriteAnimation.recipeShade = -127;
	state.spriteAnimation.recipePalette = 2;
	tspritetype sprite = {};
	sprite.setspritetexture(base);
	sprite.scale = DVector2(1, 1);
	if (!gi->ResolveActorSpriteFallbackFrame(state, sprite, {}, {}) || sprite.shade != -127)
	{
		Printf("Duke sprite fallback self-test: result=fail reason=fullbright\n"); return;
	}
	state.spriteAnimation.heatVision = true;
	gi->ResolveActorSpriteFallbackFrame(state, sprite, {}, {});
	if (sprite.shade != 0 || sprite.pal != 6) { Printf("Duke sprite fallback self-test: result=fail reason=heat-on\n"); return; }
	state.spriteAnimation.heatVision = false;
	gi->ResolveActorSpriteFallbackFrame(state, sprite, {}, {});
	if (sprite.shade != -127 || sprite.pal != 2) { Printf("Duke sprite fallback self-test: result=fail reason=heat-off\n"); return; }
	state.spriteAnimation.recipeHidden = true;
	gi->ResolveActorSpriteFallbackFrame(state, sprite, {}, {});
	if (sprite.scale.X != 0 || sprite.scale.Y != 0) { Printf("Duke sprite fallback self-test: result=fail reason=intentional-hidden\n"); return; }
	state.spriteAnimation.recipeHidden = false;
	state.spriteAnimation.forceSphereChild = true;
	state.spriteAnimation.viewPlayerPosition = { 10, 0 };
	sprite.scale = DVector2(1, 1);
	sprite.pos = DVector3(-1, 0, 0);
	gi->ResolveActorSpriteFallbackFrame(state, sprite, {}, {});
	if (sprite.scale.X != 0) { Printf("Duke sprite fallback self-test: result=fail reason=sphere-hidden\n"); return; }
	sprite.scale = DVector2(1, 1);
	sprite.pos = DVector3(1, 0, 0);
	gi->ResolveActorSpriteFallbackFrame(state, sprite, {}, {});
	if (sprite.scale.X == 0) { Printf("Duke sprite fallback self-test: result=fail reason=sphere-visible\n"); return; }
	state.spriteAnimation.recipeCertified = false;
	if (gi->ResolveActorSpriteFallbackFrame(state, sprite, {}, {})) { Printf("Duke sprite fallback self-test: result=fail reason=unknown-script-certified\n"); return; }
	const char* scrapNames[] = { "SCRAP6H", "SCRAP6I", "SCRAP1" };
	for (unsigned index = 0; index < 3; ++index)
	{
		const auto texture = TexMan.CheckForTexture(scrapNames[index], ETextureType::Any, FTextureManager::TEXMAN_TryAny | FTextureManager::TEXMAN_ReturnAll);
		state.spriteAnimation = {};
		state.spriteAnimation.available = state.spriteAnimation.recipeCertified = state.spriteAnimation.recipeHandled = true;
		state.spriteAnimation.recipeEmptyArtHides = true;
		state.spriteAnimation.recipeTextureId = texture.GetIndex();
		sprite.scale = DVector2(1, 1);
		if (!gi->ResolveActorSpriteFallbackFrame(state, sprite, {}, {}) || (sprite.scale.X == 0) != (index == 1))
		{
			Printf("Duke sprite fallback self-test: result=fail reason=scrap-authored-blank frame=%s\n", scrapNames[index]); return;
		}
	}
	if (ResolveScrapSpriteFrame(8, 1) != 9 || ResolveScrapSpriteFrame(0, -1) != 18 ||
		ResolveScrapSpriteFrame(26, 4) != 18 || !ScrapSpriteFrameIsBrighter(7) || ScrapSpriteFrameIsBrighter(8))
	{
		Printf("Duke sprite fallback self-test: result=fail reason=scrap-frame-boundaries\n"); return;
	}
	state.spriteAnimation = {};
	auto& recipe = state.spriteAnimation;
	recipe.available = recipe.recipeCertified = recipe.recipeHandled = true;
	recipe.recipeCstatClear = (uint16_t)CSTAT_SPRITE_XFLIP | (uint16_t)CSTAT_SPRITE_YFLIP;
	recipe.recipeCstatSet = CSTAT_SPRITE_YFLIP;
	recipe.recipeZOffset = -4;
	state.position = { 10, 20, 100 };
	sprite.pos = DVector3(10, 20, 100);
	sprite.scale = DVector2(0.5, 0.75);
	sprite.cstat = CSTAT_SPRITE_XFLIP | CSTAT_SPRITE_BLOCK;
	if (!ApplySpritePresentationRecipe(state, sprite) || sprite.pos.Z != 96 ||
		(sprite.cstat & CSTAT_SPRITE_XFLIP) || !(sprite.cstat & CSTAT_SPRITE_YFLIP) || !(sprite.cstat & CSTAT_SPRITE_BLOCK))
	{
		Printf("Duke sprite fallback self-test: result=fail reason=shell-flags-atomic-offset\n"); return;
	}
	recipe.recipePositionZMode = 1;
	recipe.recipePositionZ = 100;
	recipe.recipeFloorSlope = { 0.5, -0.25 };
	sprite.pos = DVector3(14, 24, -400);
	if (!ApplySpritePresentationRecipe(state, sprite) || sprite.pos.Z != 101)
	{
		Printf("Duke sprite fallback self-test: result=fail reason=fire-floor-plane\n"); return;
	}
	recipe.recipePositionZMode = 2;
	recipe.recipePositionZ = 42;
	recipe.recipeHideYOnly = true;
	if (!ApplySpritePresentationRecipe(state, sprite) || sprite.pos.Z != 42 || sprite.scale.X != 0.5 || sprite.scale.Y != 0)
	{
		Printf("Duke sprite fallback self-test: result=fail reason=laser-owner-z-hidden\n"); return;
	}
	recipe.recipeHideYOnly = false;
	recipe.recipeNativeHidden = true;
	sprite.scale = DVector2(0.5, 0.75);
	if (!ApplySpritePresentationRecipe(state, sprite) || sprite.scale.Y != 0.75 ||
		!gi->ResolveActorSpriteFallbackFrame(state, sprite, {}, {}) || sprite.scale.X != 0 || sprite.scale.Y != 0)
	{
		Printf("Duke sprite fallback self-test: result=fail reason=native-hidden-order\n"); return;
	}
	for (int direction : { 8, 12 }) for (int selectedFrame : { 1, 13 })
	{
		recipe = {};
		recipe.available = recipe.recipeCertified = recipe.recipeHandled = true;
		recipe.forcedRotation = direction;
		recipe.recipeTextureId = base.GetIndex() + selectedFrame;
		sprite = {};
		sprite.setspritetexture(base);
		sprite.spriteFallbackRotationBase = base;
		sprite.scale = DVector2(1, 1);
		sprite.Angles.Yaw = DAngle::fromDeg(135);
		auto expected = sprite;
		expected.setspritetexture(base + selectedFrame);
		if (direction == 8) ResolveSpriteRotation1(nullptr, &expected, {}, base, false);
		else ResolveSpriteRotation2(nullptr, &expected, {}, base, false);
		if (!gi->ResolveActorSpriteFallbackFrame(state, sprite, {}, {}) || !equal(expected, sprite))
		{
			Printf("Duke sprite fallback self-test: result=fail reason=script-frame-forced-rotation forced=%d frame=%d\n", direction, selectedFrame); return;
		}
	}
	uint32_t spriteSetCases = 0, shellCases = 0;
	for (const char* name : { "DukeShell", "DukeShotgunShell" })
	{
		const auto* cls = PClass::FindClass(name);
		auto* defaults = cls != nullptr ? GetDefaultByType(cls) : nullptr;
		if (defaults == nullptr || !defaults->IsKindOf(RUNTIME_CLASS(DDukeActor)))
		{
			Printf("Duke sprite fallback self-test: result=fail reason=shell-class class=%s\n", name); return;
		}
		const auto& actor = *static_cast<const DDukeActor*>(defaults);
		const int size = (int)static_cast<PClassActor*>(actor.GetClass())->ActorInfo()->SpriteSet.Size();
		if (size != (std::strcmp(name, "DukeShotgunShell") == 0 ? 1 : 2))
		{
			Printf("Duke sprite fallback self-test: result=fail reason=shell-spriteset-size class=%s size=%d\n", name, size); return;
		}
		for (int index : { -2, -1, 0, 1, size, size + 3 })
		{
			recipe = {};
			recipe.available = recipe.recipeCertified = recipe.recipeHandled = true;
			sprite = {};
			sprite.setspritetexture(base); // Deliberately different: out-of-range is a no-op.
			sprite.scale = DVector2(1, 1);
			auto expected = sprite;
			::tspritetype_setSpritePic(&expected, defaults, (unsigned)index);
			CaptureSpriteSetFrame(actor, index, recipe);
			if (!ApplySpritePresentationRecipe(state, sprite) || sprite.spritetexture() != expected.spritetexture())
			{
				Printf("Duke sprite fallback self-test: result=fail reason=spriteset-native-parity class=%s index=%d\n", name, index); return;
			}
			++spriteSetCases;
		}
		for (int counter = 0; counter < 4; ++counter)
		{
			recipe = {};
			recipe.available = recipe.recipeHandled = true;
			sprite = {};
			sprite.setspritetexture(actor.spr.spritetexture());
			sprite.scale = DVector2(1, 1);
			sprite.cstat = CSTAT_SPRITE_BLOCK;
			auto expected = sprite;
			::tspritetype_setSpritePic(&expected, defaults, (unsigned)(counter & 1));
			expected.cstat |= CSTAT_SPRITE_XFLIP | CSTAT_SPRITE_YFLIP;
			if (counter > 1) expected.cstat &= ~CSTAT_SPRITE_XFLIP;
			if (counter > 2) expected.cstat &= ~(CSTAT_SPRITE_XFLIP | CSTAT_SPRITE_YFLIP);
			CaptureShellSpriteRecipe(actor, counter, recipe);
			if (!ApplySpritePresentationRecipe(state, sprite) || !equal(expected, sprite))
			{
				Printf("Duke sprite fallback self-test: result=fail reason=shell-cycle class=%s counter=%d\n", name, counter); return;
			}
			++shellCases;
		}
	}
	Printf("Duke sprite fallback self-test: result=pass parity_cases=%u missing_frame_cases=%u recipe_cases=19 spriteset_cases=%u shell_cases=%u\n", cases, missingFrames, spriteSetCases, shellCases);
}

END_DUKE_NS
