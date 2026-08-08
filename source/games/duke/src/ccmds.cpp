//-------------------------------------------------------------------------
/*
Copyright (C) 1996, 2003 - 3D Realms Entertainment
Copyright 2020 Christoph Oelckers

This file is part of Duke Nukem 3D version 1.5 - Atomic Edition

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
Modifications for JonoF's port by Jonathon Fowler (jf@jonof.id.au)
*/
//-------------------------------------------------------------------------

#include "ns.h"

#include "duke3d.h"
#include "mapinfo.h"
#include "cheathandler.h"
#include "c_dispatch.h"
#include "gamestate.h"
#include "gamefuncs.h"
#include "dukeactor.h"
#include "gameupdate.h"

#include <cerrno>
#include <cmath>
#include <limits>

EXTERN_CVAR(Int, developer)

BEGIN_DUKE_NS

int getlabelvalue(const char* text);

static int ccmd_spawn(CCmdFuncPtr parm)
{
	FTextureID texid = FNullTextureID();
	int picno = -1;
	int x = 0, y = 0, z = 0;
	ESpriteFlags cstat = 0;
	PClassActor* cls = nullptr;
	unsigned int pal = 0;
	DAngle ang = nullAngle;
	int set = 0;

#if 0 // fixme - route through the network and this limitation becomes irrelevant
	if (netgame || numplayers > 1 || !(ps[myconnectindex].gm & MODE_GAME)) {
		Printf("spawn: Can't spawn sprites in multiplayer games or demos\n");
		return CCMD_OK;
	}
#endif

	switch (parm->numparms) {
	case 7: // x,y,z
		x = atol(parm->parms[4]);
		y = atol(parm->parms[5]);
		z = atol(parm->parms[6]);
		set |= 8;
		[[fallthrough]];
	case 4: // ang
		ang = DAngle::fromDeg(atoi(parm->parms[3])); set |= 4;
		[[fallthrough]];
	case 3: // cstat
		cstat = ESpriteFlags::FromInt(atoi(parm->parms[2])); set |= 2;
		[[fallthrough]];
	case 2: // pal
		pal = (uint8_t)atol(parm->parms[1]); set |= 1;
		[[fallthrough]];
	case 1: // tile number
		if (isdigit((uint8_t)parm->parms[0][0])) {
			picno = (unsigned short)atol(parm->parms[0]);
			cls = GetSpawnType(picno);
		}
		else 
		{
			cls = PClass::FindActor(parm->parms[0]);
			if (!cls)
			{
				texid = TexMan.CheckForTexture(parm->parms[0], ETextureType::Any, FTextureManager::TEXMAN_TryAny | FTextureManager::TEXMAN_ReturnAll);
				if (texid.isValid())
				{
					picno = legacyTileNum(texid);
				}
				if (picno < 0)
				{
					picno = getlabelvalue(parm->parms[0]);
				}
				cls = GetSpawnType(picno);
			}
		}

		if (cls == nullptr && !texid.isValid())
		{
			Printf("spawn: Invalid actor type '%s'\n", parm->parms[0]);
			return CCMD_OK;
		}
		break;
	default:
		return CCMD_SHOWHELP;
	}

	const auto pact = getPlayer(myconnectindex)->GetActor();
	if (DDukeActor* spawned = !cls ? spawnsprite(pact, picno) : spawn(pact, cls))
	{
		if (set & 1) spawned->spr.pal = (uint8_t)pal;
		if (set & 2) spawned->spr.cstat = ESpriteFlags::FromInt(cstat);
		if (set & 4) spawned->spr.Angles.Yaw = ang;
		if (set & 8) SetActor(spawned, DVector3( x, y, z ));
		if (spawned->spr.scale.isZero()) spawned->spr.scale = isRR() ? DVector2(0.5, 0.5) : DVector2(1., 1.); // nake sure it's visible.

		if (spawned->sector() == nullptr)
		{
			Printf("spawn: Sprite cannot be spawned into null space\n");
			spawned->Destroy();
		}
	}

	return CCMD_OK;
}

namespace
{
	bool ParseRelocateInteger(const char* text, int& value)
	{
		if (text == nullptr || *text == '\0') return false;

		errno = 0;
		char* end = nullptr;
		const long parsed = strtol(text, &end, 10);
		if (errno == ERANGE || end == text || *end != '\0' ||
			parsed < std::numeric_limits<int>::min() || parsed > std::numeric_limits<int>::max())
		{
			return false;
		}

		value = (int)parsed;
		return true;
	}

	bool ParseRelocateCoordinate(const char* text, double& value)
	{
		if (text == nullptr || *text == '\0') return false;

		errno = 0;
		char* end = nullptr;
		const double parsed = strtod(text, &end);
		if (errno == ERANGE || end == text || *end != '\0' || !std::isfinite(parsed)) return false;

		value = parsed;
		return true;
	}

	void PrintActorRelocateRejection(const char* reason)
	{
		Printf("duke_test_actor_relocate: result=rejected reason=%s\n", reason);
	}
}

static int ccmd_test_actor_relocate(CCmdFuncPtr parm)
{
	if (parm->numparms != 7 && parm->numparms != 8) return CCMD_SHOWHELP;

	if ((int)developer <= 0)
	{
		PrintActorRelocateRejection("developer-disabled");
		return CCMD_OK;
	}
	if (gamestate != GS_LEVEL || numplayers != 1 || netgame || ud.recstat != 0)
	{
		PrintActorRelocateRejection("unsupported-game-state");
		return CCMD_OK;
	}

	int actorIndex = 0;
	int expectedSectorIndex = -1;
	int destinationSectorIndex = -1;
	double x = 0;
	double y = 0;
	double z = 0;
	if (!ParseRelocateInteger(parm->parms[0], actorIndex) || actorIndex < 0)
	{
		PrintActorRelocateRejection("invalid-actor-index");
		return CCMD_OK;
	}
	if (!ParseRelocateInteger(parm->parms[2], expectedSectorIndex) || !validSectorIndex(expectedSectorIndex))
	{
		PrintActorRelocateRejection("invalid-expected-sector");
		return CCMD_OK;
	}
	if (!ParseRelocateInteger(parm->parms[3], destinationSectorIndex) || !validSectorIndex(destinationSectorIndex))
	{
		PrintActorRelocateRejection("invalid-destination-sector");
		return CCMD_OK;
	}
	if (!ParseRelocateCoordinate(parm->parms[4], x) ||
		!ParseRelocateCoordinate(parm->parms[5], y) ||
		!ParseRelocateCoordinate(parm->parms[6], z))
	{
		PrintActorRelocateRejection("invalid-coordinate");
		return CCMD_OK;
	}

	const bool stop = parm->numparms == 8;
	if (stop && stricmp(parm->parms[7], "stop") != 0)
	{
		PrintActorRelocateRejection("invalid-option");
		return CCMD_OK;
	}

	auto expectedClass = PClass::FindActor(parm->parms[1]);
	if (expectedClass == nullptr || !expectedClass->IsDescendantOf(RUNTIME_CLASS(DDukeActor)))
	{
		PrintActorRelocateRejection("invalid-expected-class");
		return CCMD_OK;
	}

	DDukeActor* actor = nullptr;
	int matches = 0;
	DukeSpriteIterator iterator;
	while (auto candidate = iterator.Next())
	{
		if (candidate->exists() && (candidate->ObjectFlags & OF_EuthanizeMe) == 0 && candidate->GetIndex() == actorIndex)
		{
			actor = candidate;
			matches++;
		}
	}
	if (matches != 1 || actor == nullptr)
	{
		PrintActorRelocateRejection(matches == 0 ? "actor-not-found" : "ambiguous-actor-index");
		return CCMD_OK;
	}
	if (actor->isPlayer())
	{
		PrintActorRelocateRejection("player-actor");
		return CCMD_OK;
	}
	if (actor->GetClass() != expectedClass)
	{
		PrintActorRelocateRejection("class-mismatch");
		return CCMD_OK;
	}
	if (actor->sectno() != expectedSectorIndex)
	{
		PrintActorRelocateRejection("sector-mismatch");
		return CCMD_OK;
	}
	if (actor->actorstayput != nullptr)
	{
		PrintActorRelocateRejection("stayput-actor");
		return CCMD_OK;
	}

	auto destinationSector = &sector[destinationSectorIndex];
	if (!inside(x, y, destinationSector))
	{
		PrintActorRelocateRejection("destination-xy-outside-sector");
		return CCMD_OK;
	}
	const double ceilingz = getceilzofslopeptr(destinationSector, x, y);
	const double floorz = getflorzofslopeptr(destinationSector, x, y);
	if (ceilingz > floorz || z < ceilingz || z > floorz)
	{
		PrintActorRelocateRejection("destination-z-outside-sector");
		return CCMD_OK;
	}

	const DVector3 oldPosition = actor->spr.pos;
	const int oldSectorIndex = actor->sectno();
	actor->spr.pos = DVector3(x, y, z);
	if (actor->sector() != destinationSector) ChangeActorSect(actor, destinationSector);
	actor->backuploc();
	actor->ceilingz = ceilingz;
	actor->floorz = floorz;
	actor->cgg = 0;
	actor->timetosleep = 0;
	if (stop)
	{
		actor->vel.Zero();
		actor->ovel.Zero();
	}

	const GameUpdateSnapshot gameUpdate = GetGameUpdateSnapshot();
	Printf("duke_test_actor_relocate: result=relocated simulation_generation=%llu actor=%d class=%s old_sector=%d old_pos=(%.4f,%.4f,%.4f) new_sector=%d new_pos=(%.4f,%.4f,%.4f) stop=%s\n",
		(unsigned long long)gameUpdate.simulationGeneration,
		actor->GetIndex(),
		actor->GetClass()->TypeName.GetChars(),
		oldSectorIndex,
		oldPosition.X,
		oldPosition.Y,
		oldPosition.Z,
		actor->sectno(),
		actor->spr.pos.X,
		actor->spr.pos.Y,
		actor->spr.pos.Z,
		stop ? "true" : "false");
	return CCMD_OK;
}

void GameInterface::ToggleThirdPerson()
{
	if (gamestate != GS_LEVEL) return;
	const auto p = getPlayer(myconnectindex);
	if (!isRRRA() || (!p->OnMotorcycle && !p->OnBoat))
	{
		if (p->over_shoulder_on)
			p->over_shoulder_on = 0;
		else
		{
			p->over_shoulder_on = 1;
			cameradist = 0;
			cameraclock = INT_MIN;
		}
		FTA(QUOTE_VIEW_MODE_OFF + p->over_shoulder_on, p);
	}
}

void GameInterface::SwitchCoopView()
{
	if (gamestate != GS_LEVEL) return;
	if (ud.coop || ud.recstat == 2)
	{
		screenpeek = connectpoint2[screenpeek];
		if (screenpeek == -1) screenpeek = 0;
	}
}

void GameInterface::ToggleShowWeapon()
{
	if (gamestate != GS_LEVEL) return;
	cl_showweapon = cl_showweapon == 0;
	FTA(QUOTE_WEAPON_MODE_OFF - cl_showweapon, getPlayer(screenpeek));
}

bool GameInterface::WantEscape() 
{ 
	return getPlayer(myconnectindex)->newOwner != nullptr;
}


int registerosdcommands(void)
{
	C_RegisterFunction("spawn","spawn <typename> [palnum] [cstat] [ang] [x y z]: spawns a sprite with the given properties",ccmd_spawn);
	C_RegisterFunction("duke_test_actor_relocate",
		"duke_test_actor_relocate <actorIndex> <expectedClass> <expectedSector> <destSector> <x> <y> <z> [stop]: relocates one live actor for lifecycle validation",
		ccmd_test_actor_relocate);
	return 0;
}

END_DUKE_NS
