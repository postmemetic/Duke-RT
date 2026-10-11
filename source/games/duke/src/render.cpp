//-------------------------------------------------------------------------
/*
Copyright (C) 1996, 2003 - 3D Realms Entertainment

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
#include "build.h"
#include "v_video.h"
#include "prediction.h"
#include "automap.h"
#include "dukeactor.h"
#include "interpolate.h"
#include "input_lineage.h"
#include "render.h"

// temporary hack to pass along RRRA's global fog. Needs to be done better later.
extern PalEntry GlobalMapFog;
extern float GlobalFogDensity;

BEGIN_DUKE_NS

namespace
{
	bool gMirrorPlayerVisibilityCaptureOverride = false;
}

//---------------------------------------------------------------------------
//
// Floor Over Floor

// If standing in sector with SE42
// then draw viewing to SE41 and raise all =hi SE43 cielings.

// If standing in sector with SE43
// then draw viewing to SE40 and lower all =hi SE42 floors.

// If standing in sector with SE44
// then draw viewing to SE40.

// If standing in sector with SE45
// then draw viewing to SE41.
//
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
//
// 
//
//---------------------------------------------------------------------------

void GameInterface::UpdateCameras(double smoothratio)
{
	const int VIEWSCREEN_ACTIVE_DISTANCE = 1024;
	static TObjPtr<DDukeActor*> sPreviousViewCamera;

	if (camsprite == nullptr)
		return;

	auto p = getPlayer(screenpeek);
	if (p->newOwner != nullptr)
	{
		if (sPreviousViewCamera == nullptr)
		{
			screen->NotifyPathTracingCameraCut("camera-enter");
		}
		else if (sPreviousViewCamera != p->newOwner)
		{
			screen->NotifyPathTracingCameraCut("camera-switch");
		}

		// Active camera use already renders the main view from the camera actor.
		// Updating the offscreen monitor canvas in the same frame forces a second
		// render at a different size, which is unsupported by the current NRI PT path.
		camsprite->SetOwner(p->newOwner);
		sPreviousViewCamera = p->newOwner;
		return;
	}
	if (sPreviousViewCamera != nullptr)
	{
		screen->NotifyPathTracingCameraCut("camera-exit");

		// The first frame after leaving camera view still renders the normal main
		// scene at window size. Skip the monitor refresh in that transition frame
		// so NRI PT does not bounce between offscreen and main-view frame sizes.
		sPreviousViewCamera = nullptr;
		return;
	}

	if (camsprite->GetOwner() && (p->GetActor()->spr.pos - camsprite->spr.pos).Length() < VIEWSCREEN_ACTIVE_DISTANCE)
	{
		auto tex = TexMan.FindGameTexture("VIEWSCR", ETextureType::Any);
		if (!tex || !tex->GetTexture()->isCanvas()) return;

		auto canvas = static_cast<FCanvasTexture*>(tex->GetTexture());

		screen->RenderTextureView(canvas, [=](IntRect& rect)
			{
				auto camera = camsprite->GetOwner();
				display_mirror = 2; // should really be 'display external view'.
				auto cstat = camera->spr.cstat;
				camera->spr.cstat = CSTAT_SPRITE_INVISIBLE;
				render_camtex(camera, camera->spr.pos, camera->sector(), DRotator(maphoriz(-camera->spr.shade), camera->interpolatedyaw(smoothratio), nullAngle), tex, rect, smoothratio);
				camera->spr.cstat = cstat;
				display_mirror = 0;
			});
	}
}

static FCanvasTexture* GetViewscreenCanvas()
{
	auto tex = TexMan.FindGameTexture("VIEWSCR", ETextureType::Any);
	if (tex == nullptr || tex->GetTexture() == nullptr || !tex->GetTexture()->isCanvas())
	{
		return nullptr;
	}

	return static_cast<FCanvasTexture*>(tex->GetTexture());
}

void GameInterface::EnterPortal(DCoreActor* viewer, int type)
{
	if (type == PORTAL_WALL_MIRROR) display_mirror++;
}

void GameInterface::LeavePortal(DCoreActor* viewer, int type) 
{
	if (type == PORTAL_WALL_MIRROR) display_mirror--;
}

void GameInterface::SetMirrorPlayerVisibilityCaptureOverride(bool enabled)
{
	SetMirrorPlayerVisibilityCaptureActive(enabled);
}

bool GameInterface::GetMirrorPlayerVisibilityCaptureOverride() const
{
	return IsMirrorPlayerVisibilityCaptureActive();
}

bool GameInterface::GetGeoEffect(GeoEffect* eff, sectortype* viewsector)
{
	if (isRR() && viewsector->lotag == 848)
	{
		eff->geocnt = geocnt;
		eff->geosector = geosector;
		eff->geosectorwarp = geosectorwarp;
		eff->geosectorwarp2 = geosectorwarp2;
		eff->geox = geox;
		eff->geoy = geoy;
		eff->geox2 = geox2;
		eff->geoy2 = geoy2;
		return true;
	}
	return false;
}

bool GameInterface::GetRuntimeLinkDebugState(RuntimeLinkDebugState* state)
{
	if (state == nullptr)
	{
		return false;
	}

	*state = {};

	const auto p = getPlayer(screenpeek);
	if (p == nullptr || !p->insector())
	{
		return false;
	}

	const auto playerSector = p->cursector;
	const auto actor = p->GetActor();
	const auto actorSector = actor != nullptr ? actor->sector() : nullptr;
	if (playerSector == nullptr || actor == nullptr)
	{
		return false;
	}

	int effectiveLotag = playerSector->lotag;
	bool specialWaterSector = false;
	if (effectiveLotag == 867)
	{
		DukeSectIterator it(playerSector);
		while (auto act = it.Next())
		{
			if (act->GetClass() == RedneckWaterSurfaceClass && act->spr.pos.Z - 8 < actor->getOffsetZ())
			{
				effectiveLotag = ST_2_UNDERWATER;
				break;
			}
		}
	}
	else if (effectiveLotag == 848 && tilesurface(playerSector->floortexture) == TSURF_SPECIALWATER)
	{
		effectiveLotag = ST_1_ABOVE_WATER;
		specialWaterSector = true;
	}

	state->available = true;
	state->specialWaterSector = specialWaterSector;
	state->playerSectorIndex = sectindex(playerSector);
	state->playerSectorLotag = playerSector->lotag;
	state->playerSectorHitag = playerSector->hitag;
	state->effectiveSectorLotag = effectiveLotag;
	state->actorSectorIndex = actorSector != nullptr ? sectindex(actorSector) : -1;
	state->actorSectorLotag = actorSector != nullptr ? actorSector->lotag : 0;
	state->actorSectorHitag = actorSector != nullptr ? actorSector->hitag : 0;
	state->onWarpingSector = p->on_warping_sector;
	state->transporterHold = p->transporter_hold;
	state->rrGeoCount = geocnt;
	return true;
}

bool GameInterface::GetNightVisionState(RuntimeNightVisionState* state)
{
	if (state == nullptr)
	{
		return false;
	}

	*state = {};

	const auto p = getPlayer(screenpeek);
	if (p == nullptr)
	{
		return false;
	}

	state->available = true;
	state->mode = RuntimeNightVisionMode::Duke;
	state->viewEligible = p->newOwner == nullptr && ud.cameraactor == nullptr;
	state->remainingSeconds = max((int)p->heat_amount, 0) * (1.0f / 120.0f);
	state->enabled = state->viewEligible && p->heat_on != 0 && p->heat_amount > 0;
	state->strength01 = state->enabled ? 1.0f : 0.0f;
	return true;
}

bool GameInterface::GetRuntimeLinkDebugTaggedSectorInfo(int sectorIndex, RuntimeTaggedSectorDebugInfo* info)
{
	if (info == nullptr || sectorIndex < 0 || (unsigned)sectorIndex >= sector.Size())
	{
		return false;
	}

	*info = {};
	auto* sect = &sector[(unsigned)sectorIndex];
	info->available = true;
	info->sectorIndex = sectorIndex;
	info->lotag = sect->lotag;
	info->hitag = sect->hitag;

	DukeSectIterator it(sect);
	while (auto act = it.Next())
	{
		if (!iseffector(act))
		{
			continue;
		}

		if (info->effectorCount < countof(info->effectorLotags))
		{
			info->effectorLotags[info->effectorCount] = act->spr.lotag;
			info->effectorHitags[info->effectorCount] = act->spr.hitag;
		}
		info->effectorCount++;
	}

	return true;
}

//---------------------------------------------------------------------------
//
// RRRA's drug distortion effect
//
//---------------------------------------------------------------------------
int DrugTimer;

static int getdrugmode(DDukePlayer *p, int oyrepeat)
{
	int now = I_GetBuildTime() >> 1;	// this function works on a 60 fps setup.
	if (playrunning() && p->DrugMode > 0)
	{
		if (now - DrugTimer > 4 || now - DrugTimer < 0) DrugTimer = now - 1;
		while (DrugTimer < now)
		{
			DrugTimer++;
			int var_8c;
			if (p->drug_stat[0] == 0)
			{
				p->drug_stat[1]++;
				var_8c = oyrepeat + p->drug_stat[1] * 5000;
				if (oyrepeat * 3 < var_8c)
				{
					p->drug_aspect = oyrepeat * 3;
					p->drug_stat[0] = 2;
				}
				else
				{
					p->drug_aspect = var_8c;
				}
			}
			else if (p->drug_stat[0] == 3)
			{
				p->drug_stat[1]--;
				var_8c = oyrepeat + p->drug_stat[1] * 5000;
				if (var_8c < oyrepeat)
				{
					p->DrugMode = 0;
					p->drug_stat[0] = 0;
					p->drug_stat[2] = 0;
					p->drug_stat[1] = 0;
					p->drug_aspect = oyrepeat;
				}
				else
				{
					p->drug_aspect = var_8c;
				}
			}
			else if (p->drug_stat[0] == 2)
			{
				if (p->drug_stat[2] > 30)
				{
					p->drug_stat[0] = 1;
				}
				else
				{
					p->drug_stat[2]++;
					p->drug_aspect = oyrepeat * 3 + p->drug_stat[2] * 500;
				}
			}
			else
			{
				if (p->drug_stat[2] < 1)
				{
					p->drug_stat[0] = 2;
					p->DrugMode--;
					if (p->DrugMode == 1)
						p->drug_stat[0] = 3;
				}
				else
				{
					p->drug_stat[2]--;
					p->drug_aspect = oyrepeat * 3 + p->drug_stat[2] * 500;
				}
			}
		}
		return p->drug_aspect;
	}
	else
	{
		DrugTimer = now;
		return oyrepeat;
	}
}

//---------------------------------------------------------------------------
//
//
//
//---------------------------------------------------------------------------

void displayrooms(int snum, double interpfrac, bool sceneonly)
{
	DVector3 cpos;
	DRotator cangles;

	DDukePlayer* p = getPlayer(snum);

	// update render angles.
	p->updateCameraAngles(interpfrac);

	if (automapMode == am_full || !p->insector())
		return;

	// Do not light up the fog in RRRA's E2L1. Ideally this should apply to all foggy levels but all others use lookup table hacks for their fog.
	if (ud.fogactive)
	{
		p->visibility = ud.const_visibility;
	}
	g_visibility = ud.const_visibility;
	g_relvisibility = p->visibility - ud.const_visibility;
	GlobalMapFog = ud.fogactive ? 0x999999 : 0;
	GlobalFogDensity = ud.fogactive ? 350.f : 0.f;

	DoInterpolations(interpfrac);

	setgamepalette(BASEPAL);

	float fov = (float)r_fov;
	auto sect = p->cursector;

	DDukeActor* viewer;
	bool camview = false;

	if (ud.cameraactor)
	{
		viewer = ud.cameraactor;
		camview = true;

		if (viewer->spr.yint < 0) viewer->spr.yint = -100;
		else if (viewer->spr.yint > 199) viewer->spr.yint = 300;

		cpos = viewer->spr.pos.plusZ(-4);
		cangles = DRotator(maphoriz(-viewer->spr.yint), viewer->interpolatedyaw(interpfrac), nullAngle);
		sect = viewer->sector();
	}
	else
	{
		if (isRRRA() && p->DrugMode)
		{
			double fovdelta = atan(getdrugmode(p, 65536) * (1. / 65536.)) * (360. / pi::pi()) - 90.;
			fov = (float)clamp<double>(r_fov + fovdelta * 0.6, r_fov, 150.);
		}

		// The camera texture must be rendered with the base palette, so this is the only place where the current global palette can be set.
		// The setting here will be carried over to the rendering of the weapon sprites, but other 2D content will always default to the main palette.
		setgamepalette(setpal(p));

		// use player's actor initially.
		viewer = p->GetActor();

		if ((snum == myconnectindex) && (numplayers > 1))
		{
			cpos = interpolatedvalue(omypos, mypos, interpfrac);
			cangles = DRotator(interpolatedvalue(omyhoriz + omyhorizoff, myhoriz + myhorizoff, interpfrac), interpolatedvalue(omyang, myang, interpfrac), nullAngle);
		}
		else
		{
			cpos = viewer->getRenderPos(interpfrac);
			cangles = p->getRenderAngles(interpfrac);
		}

		if (p->newOwner != nullptr)
		{
			viewer = p->newOwner;
			cpos = viewer->spr.pos;
			cangles = DRotator(maphoriz(-viewer->spr.shade), viewer->interpolatedyaw(interpfrac), nullAngle);
			sect = viewer->sector();
			interpfrac = 1.;
			camview = true;
		}
		else if (p->over_shoulder_on == 0)
		{
			if (cl_viewbob) cpos.Z += interpolatedvalue(p->opyoff, p->pyoff, interpfrac);
		}
		else
		{
			auto adjustment = isRR() ? 15 : 12;
			cpos.Z -= adjustment;

			if (!calcChaseCamPos(cpos, viewer, &sect, cangles, interpfrac, 64.))
			{
				cpos.Z += adjustment;
				calcChaseCamPos(cpos, viewer, &sect, cangles, interpfrac, 64.);
			}
		}

		double cz = p->GetActor()->ceilingz;
		double fz = p->GetActor()->floorz;

		if (ud.earthquaketime > 0 && p->on_ground == 1)
		{
			cpos.Z += 1 - (((ud.earthquaketime) & 1) * 2.);
			cangles.Yaw += DAngle::fromBuild((2 - ((ud.earthquaketime) & 2)) << 2);
		}

		if (p->GetActor()->spr.pal == 1) cpos.Z -= 18;

		else if (p->spritebridge == 0 && p->newOwner == nullptr)
		{
			cpos.Z = min(max(cpos.Z, p->truecz + 4), p->truefz - 4);
		}

		if (sect)
		{
			calcSlope(sect, cpos, &cz, &fz);
			cpos.Z = min(max(cpos.Z, cz + 4), fz - 4);
		}
	}

	auto cstat = viewer->spr.cstat;
	if (camview) viewer->spr.cstat = CSTAT_SPRITE_INVISIBLE;
	if (!sceneonly) drawweapon(interpfrac);
	PerfInputLineageNoteViewCapture((float)cangles.Yaw.Degrees(), (float)cangles.Pitch.Degrees());
	render_drawrooms(viewer, cpos, sect, cangles, interpfrac, fov);
	if (p->newOwner != nullptr)
	{
		if (auto* canvas = GetViewscreenCanvas(); canvas != nullptr)
		{
			screen->SnapshotCurrentViewToCanvas(canvas);
		}
	}
	viewer->spr.cstat = cstat;

	//GLInterface.SetMapFog(false);
	RestoreInterpolations();

	if (!ud.fogactive)
	{
		if (PlayClock < lastvisinc)
		{
			if (abs(p->visibility - ud.const_visibility) > 8)
				p->visibility += (ud.const_visibility - p->visibility) >> 2;
		}
		else p->visibility = ud.const_visibility;
	}
}

bool GameInterface::GenerateSavePic()
{
	displayrooms(myconnectindex, 1., true);
	return true;
}

void GameInterface::processSprites(tspriteArray& tsprites, const DVector3& view, DAngle viewang, double interpfrac)
{
	fi.animatesprites(tsprites, view.XY(), viewang, interpfrac);
}

bool IsMirrorPlayerVisibilityCaptureActive()
{
	return gMirrorPlayerVisibilityCaptureOverride;
}

void SetMirrorPlayerVisibilityCaptureActive(bool active)
{
	gMirrorPlayerVisibilityCaptureOverride = active;
}


END_DUKE_NS
