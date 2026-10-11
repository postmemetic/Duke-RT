/*
** texinfo.cpp
** Extended texture information / handling
**
**---------------------------------------------------------------------------
** Copyright 2019-2022 Christoph Oelckers
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
**
*/
#include "texturemanager.h"
#include "texinfo.h"
#include "m_crc32.h"
#include "buildtiles.h"
#include "c_cvars.h"
#include "printf.h"

// Session-only benchmark control. Pin texture animation before map capture
// without changing the clocks consumed by gameplay or performance timers.
CUSTOM_CVAR(Int, perf_tileanimationtime_ms, -1, 0)
{
	if (self < -1)
	{
		self = -1;
	}
}


//==========================================================================
//
//  Retrieve animation offset
//
//==========================================================================

static int tileAnimateOfs(FTextureID texid, int randomize)
{
	auto ext = GetExtInfo(texid);
	int framecount = ext.picanm.num;
	if (framecount > 0)
	{
		const int fixedTimeMs = perf_tileanimationtime_ms;
		int frametime = fixedTimeMs >= 0 ? int(int64_t(fixedTimeMs) * 120 / 1000) :
			(!isBlood() ? I_GetBuildTime() : PlayClock);
		static int lastReportedTimeMs = -1;
		if (fixedTimeMs != lastReportedTimeMs)
		{
			Printf("PERF tile animation clock: requested_ms=%d build_tics=%d pinned=%d blood=%d\n",
				fixedTimeMs, frametime, fixedTimeMs >= 0 ? 1 : 0, isBlood() ? 1 : 0);
			lastReportedTimeMs = fixedTimeMs;
		}

		if (isBlood() && randomize)
		{
			frametime += Bcrc32(&randomize, 2, 0);
		}

		int curframe = (frametime & 0x7fffffff) >> (ext.picanm.speed());

		switch (ext.picanm.type())
		{
		case PICANM_ANIMTYPE_FWD:
			return curframe % (framecount + 1);
		case PICANM_ANIMTYPE_BACK:
			return -(curframe % (framecount + 1));
		case PICANM_ANIMTYPE_OSC:
			curframe = curframe % (framecount << 1);
			if (curframe >= framecount) return (framecount << 1) - curframe;
			else return curframe;
		}
	}
	return 0;
}

void tileUpdatePicnum(FTextureID& tileptr, int randomize)
{
	tileptr = FSetTextureID(tileptr.GetIndex() + tileAnimateOfs(tileptr, randomize));
}

//===========================================================================
// 
//	update the animation info inside the texture manager.
//
//===========================================================================

void tileUpdateAnimations()
{
	for (unsigned i = 0; i < texExtInfo.Size(); i++)
	{
		auto& x = texExtInfo[i];
		if (x.picanm.type())
		{
			int j = i + tileAnimateOfs(FSetTextureID(i), false);
			TexMan.SetTranslation(FSetTextureID(i), FSetTextureID(j));
		}
	}
}

//==========================================================================
//
// 
//
//==========================================================================

int tilehasmodelorvoxel(FTextureID texid, int pal)
{
	if (r_voxels)
	{
		auto x = GetExtInfo(texid);
		if (x.tiletovox != -1) return true;
	}
	/*
	if (hw_models)
	{
		return modelManager.CheckModel(tilenume, pal);	// we have no models yet.
	}
	*/
	return false;
}

