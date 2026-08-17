/*
** mainloop.cpp
** Implements the main game loop
**
**---------------------------------------------------------------------------
** Copyright 2020 Christoph Oelckers
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
*/


// For TryRunTics the following applies:
//-----------------------------------------------------------------------------
//
// $Id:$
//
// Copyright (C) 1993-1996 by id Software, Inc.
// Copyright 1999-2016 Randy Heit
// Copyright 2002-2020 Christoph Oelckers
//
// This source is available for distribution and/or modification
// only under the terms of the DOOM Source Code License as
// published by id Software. All rights reserved.
//
// The source is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// FITNESS FOR A PARTICULAR PURPOSE. See the DOOM Source Code License
// for more details.
//
// $Log:$
//
// DESCRIPTION:
//		DOOM Network game communication and protocol,
//		all OS independent parts.
//
//-----------------------------------------------------------------------------


#include <chrono>
#include <thread>
#include "c_cvars.h"
#include "i_time.h"
#include "d_net.h"
#include "gamecontrol.h"
#include "gameupdate.h"
#include "actor_presentation_snapshot.h"
#include "lightoverlay_editor.h"
#include "lightoverlay_smoke_editor.h"
#include "voxelpolicy_editor.h"
#include "c_console.h"
#include "razemenu.h"
#include "i_system.h"
#include "raze_sound.h"
#include "raze_music.h"
#include "vm.h"
#include "gamestate.h"
#include "screenjob_.h"
#include "c_console.h"
#include "uiinput.h"
#include "v_video.h"
#include "mapinfo.h"
#include "gamecvars.h"
#include "palette.h"
#include "build.h"
#include "automap.h"
#include "statusbar.h"
#include "gamestruct.h"
#include "savegamehelp.h"
#include "v_draw.h"
#include "gamehud.h"
#include "common/rendering/nri/scene/nri_scene_bridge.h"
#include "wipe.h"
#include "i_interface.h"
#include "texinfo.h"
#include "texturemanager.h"
#include "gameinput.h"
#include "d_eventbase.h"
#include "perf_capture.h"
#include "hw_clock.h"

CVAR(Bool, vid_activeinbackground, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, r_ticstability, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vid_dontdowait, false, CVAR_ARCHIVE|CVAR_GLOBALCONFIG)
EXTERN_CVAR(Bool, cl_capfps)
CVAR(Bool, cl_resumesavegame, true, CVAR_ARCHIVE)
EXTERN_CVAR (Bool, vid_vsync)
EXTERN_CVAR (Int, vid_maxfps)
EXTERN_CVAR (Int, perf_looptraceframes)
EXTERN_CVAR(Int, nri_ptloadingtrace)

static uint64_t stabilityticduration = 0;
static uint64_t stabilitystarttime = 0;

DCorePlayer* PlayerArray[MAXPLAYERS];

IMPLEMENT_CLASS(DCorePlayer, true, true)
IMPLEMENT_POINTERS_START(DCorePlayer)
IMPLEMENT_POINTER(actor)
IMPLEMENT_POINTERS_END

void MarkPlayers()
{
	GC::MarkArray(PlayerArray, MAXPLAYERS);
}

bool r_NoInterpolate;
extern bool demoplayback;
CUSTOM_CVAR(Int, perf_fixedsimulationframes, 0, 0)
{
	if (self < 0)
	{
		self = 0;
	}
	else if (self > 4096)
	{
		self = 4096;
	}
}

// The fixed-presentation performance control owns this freeze only when it
// started it. Wipes, movie playback, and other engine clients retain ownership
// of any freeze that was already active.
static bool perfFixedSimulationOwnsTimeFreeze = false;

int entertic;
int oldentertics;
int gametic;
int nextwipe = wipe_None;

FString savename;
FString BackupSaveGame;

void DoLoadGame(const char* name);

bool sendsave;
FString	savedescription;
FString	savegamefile;

namespace
{
	uint64_t gGameUpdateGeneration = 1;
	uint64_t gEngineUpdateGeneration = 1;
	uint64_t gPresentationGeneration = 0;
	uint32_t gGameUpdateTicksThisPresentation = 0;

	struct PerfTryRunTicsTraceStats
	{
		bool doWait = false;
		bool pausedReturn = false;
		bool fixedSimulationReturn = false;
		uint32_t fixedSimulationSuppressedTailTicks = 0;
		bool zeroCountReturn = false;
		bool waitLoopReturn = false;
		int realtics = 0;
		int availabletics = 0;
		int counts = 0;
		int lowtic = 0;
		int waitLoopIterations = 0;
		int ticksRun = 0;
		double durationMs = 0.0;
	};

	struct PerfDisplayTraceStats
	{
		bool skippedInactive = false;
		bool levelRendered = false;
		double beginFrameMs = 0.0;
		double renderMs = 0.0;
		double overlayMs = 0.0;
		double updateMs = 0.0;
	};

	struct Perf2DProducerDelta
	{
		int commands = 0;
		int vertices = 0;
		int indices = 0;
		double ms = 0.0;
	};

	struct Perf2DProducerTraceStats
	{
		bool introSkipped = false;
		Perf2DProducerDelta fullscreenBlends;
		Perf2DProducerDelta mapTitle;
		Perf2DProducerDelta chat;
		Perf2DProducerDelta console;
		Perf2DProducerDelta menu;
		Perf2DProducerDelta stats;
		Perf2DProducerDelta rate;
		Perf2DProducerDelta drawtile;
		Perf2DProducerDelta overlays;
		int totalCommands = 0;
		int totalVertices = 0;
		int totalIndices = 0;
	};

	static PerfTryRunTicsTraceStats perfTryRunTicsTraceStats;
	static PerfDisplayTraceStats perfDisplayTraceStats;
	static Perf2DProducerTraceStats perf2DProducerTraceStats;

	struct Perf2DSnapshot
	{
		int commands = 0;
		int vertices = 0;
		int indices = 0;
	};

	static Perf2DSnapshot Capture2DSnapshot()
	{
		if (twod == nullptr)
		{
			return {};
		}

		Perf2DSnapshot snapshot;
		snapshot.commands = twod->mData.Size();
		snapshot.vertices = twod->mVertices.Size();
		snapshot.indices = twod->mIndices.Size();
		return snapshot;
	}

	template<typename Func>
	static Perf2DProducerDelta Trace2DProducer(Func&& func)
	{
		const auto before = Capture2DSnapshot();
		const double startMs = I_msTimeF();
		func();
		const auto after = Capture2DSnapshot();

		Perf2DProducerDelta delta;
		delta.commands = after.commands - before.commands;
		delta.vertices = after.vertices - before.vertices;
		delta.indices = after.indices - before.indices;
		delta.ms = I_msTimeF() - startMs;
		return delta;
	}

	static const char* GetGameStateName(int state)
	{
		switch (state)
		{
		case GS_STARTUP: return "startup";
		case GS_LEVEL: return "level";
		case GS_MENUSCREEN: return "menu";
		case GS_FULLCONSOLE: return "console";
		case GS_CUTSCENE: return "cutscene";
		case GS_INTRO: return "intro";
		default: return "unknown";
		}
	}
}

GameUpdateSnapshot GetGameUpdateSnapshot()
{
	GameUpdateSnapshot snapshot = {};
	snapshot.presentationGeneration = gPresentationGeneration;
	snapshot.engineUpdateGeneration = gEngineUpdateGeneration;
	snapshot.simulationGeneration = gGameUpdateGeneration;
	snapshot.ticksExecutedThisPresentation = gGameUpdateTicksThisPresentation;
	return snapshot;
}

//==========================================================================
//
// 
//
//==========================================================================

void G_BuildTiccmd(ticcmd_t* cmd) 
{
	if (sendsave)
	{
		sendsave = false;
		Net_WriteByte(DEM_SAVEGAME);
		Net_WriteString(savegamefile.GetChars());
		Net_WriteString(savedescription.GetChars());
		savegamefile = "";
	}
	cmd->ucmd = {};
	gameInput.getInput(&cmd->ucmd);
	localcmdsync[maketic % LOCALCMDTICS] = gameInput.SyncInput();
	cmd->consistency = consistency[myconnectindex][(maketic / ticdup) % BACKUPTICS];
}

//==========================================================================
//
//
//
//==========================================================================
bool newGameStarted;
static bool gPendingPathTracingLevelPreload = false;
static bool gPathTracingLevelPreloadHeldScreenJobCompletion = false;
static bool gPathTracingLevelPreloadAwaitingFirstLevelFrame = false;
static bool gPathTracingLevelPreloadFirstLevelFrameCaptured = false;
static bool gPathTracingLevelPreloadSimulationHoldActive = false;
static bool gPathTracingLevelPreloadNeeded = false;
static bool gPathTracingLevelPreloadFinalCheckNeeded = false;
static uint64_t gLevelTransitionSerial = 0;
static uint64_t gPathTracingLevelLoadClockSerial = 0;
static uint32_t gPathTracingLevelLoadClockRebases = 0;
static uint32_t gPathTracingLevelLoadClockHoldFrames = 0;
static uint32_t gPathTracingLevelLoadClockSuppressedTicks = 0;
static double gPathTracingLevelLoadClockExcludedMs = 0.0;
static bool gPathTracingLevelLoadClockAwaitingFirstPostRelease = false;

static bool IsPathTracingLevelLoadBoundaryActive()
{
	return gPendingPathTracingLevelPreload ||
		gPathTracingLevelPreloadAwaitingFirstLevelFrame ||
		gPathTracingLevelPreloadFinalCheckNeeded ||
		gPathTracingLevelPreloadSimulationHoldActive;
}

static bool UsePathTracingLevelLoadClockPolicy()
{
	return IsPathTracingLevelLoadBoundaryActive() && !netgame && !demoplayback;
}

static const char* GetPathTracingLevelLoadClockPolicyName()
{
	if (netgame) return "skip-network";
	if (demoplayback) return "skip-demo";
	return "single-player-rebase";
}

static void ResetPathTracingLevelLoadClockMetrics()
{
	gPathTracingLevelLoadClockSerial = gLevelTransitionSerial;
	gPathTracingLevelLoadClockRebases = 0;
	gPathTracingLevelLoadClockHoldFrames = 0;
	gPathTracingLevelLoadClockSuppressedTicks = 0;
	gPathTracingLevelLoadClockExcludedMs = 0.0;
	gPathTracingLevelLoadClockAwaitingFirstPostRelease = false;
}

static void RebasePathTracingLevelLoadClock(const char* stage, double excludedMs, bool resetInput = false)
{
	if (!IsPathTracingLevelLoadBoundaryActive())
	{
		return;
	}

	const bool apply = UsePathTracingLevelLoadClockPolicy();
	const int clockBefore = I_GetTime();
	if (apply)
	{
		I_ResetFrameTime();
		// Some blocking paths refresh the cached frame time internally. Align
		// both SP tic producers explicitly so none of that already-observed
		// elapsed time survives this named boundary.
		oldentertics = I_GetTime();
		Net_ClearFifo();
		oldentertics = I_GetTime();
		if (resetInput)
		{
			I_ResetInputTime();
		}
		++gPathTracingLevelLoadClockRebases;
		gPathTracingLevelLoadClockExcludedMs += std::max(0.0, excludedMs);
	}
	const int clockAfter = I_GetTime();

	const bool summaryStage = strcmp(stage, "begin") == 0 || strcmp(stage, "first-frame-release") == 0;
	if ((int)nri_ptloadingtrace >= (summaryStage ? 1 : 2))
	{
		Printf("PERF level load clock: event=rebase stage=%s transition_serial=%llu policy=%s applied=%u excluded_ms=%.3f clock_before=%d clock_after=%d gametic=%d presentation=%llu\n",
			stage,
			(unsigned long long)gPathTracingLevelLoadClockSerial,
			GetPathTracingLevelLoadClockPolicyName(),
			apply ? 1u : 0u,
			excludedMs,
			clockBefore,
			clockAfter,
			gametic,
			(unsigned long long)gPresentationGeneration);
	}
}

static void PrintPathTracingLevelPreloadActorCacheStats(const char* event)
{
	if ((int)nri_ptloadingtrace < 1)
	{
		return;
	}
	const nri_scene::PersistentVoxelActorCacheStats stats = nri_scene::GetPersistentVoxelActorCacheStats();
	Printf("NRI PT loading gate: event=%s actor_cache_entries=%u actor_cache_ready=%u actor_cache_captured=%u actor_cache_prims=%u actor_cache_serial=%llu actor_cache_frame=%llu gamestate=%s gameaction=%d gametic=%d\n",
		event != nullptr && *event != '\0' ? event : "actor-cache-stats",
		stats.entries,
		stats.readyEntries,
		stats.capturedThisFrame,
		stats.primitiveCount,
		(unsigned long long)stats.serial,
		(unsigned long long)stats.frame,
		GetGameStateName(gamestate),
		(int)gameaction,
		gametic);
}

static void CancelPendingPathTracingLevelPreload()
{
	if ((int)nri_ptloadingtrace >= 1 && gPendingPathTracingLevelPreload)
	{
		Printf("NRI PT loading gate: event=cancel gamestate=%s gameaction=%d screen=%u screen_pending=%u\n",
			GetGameStateName(gamestate),
			(int)gameaction,
			screen != nullptr ? 1u : 0u,
			screen != nullptr && screen->IsPathTracingLevelPreloadPending() ? 1u : 0u);
	}
	gPendingPathTracingLevelPreload = false;
	gPathTracingLevelPreloadHeldScreenJobCompletion = false;
	gPathTracingLevelPreloadAwaitingFirstLevelFrame = false;
	gPathTracingLevelPreloadFirstLevelFrameCaptured = false;
	gPathTracingLevelPreloadSimulationHoldActive = false;
	gPathTracingLevelPreloadNeeded = false;
	gPathTracingLevelPreloadFinalCheckNeeded = false;
	nri_scene::SetPersistentVoxelActorStartupTransientMode(false, "cancel-level-preload");
	if (screen != nullptr)
	{
		screen->CancelPathTracingLevelPreload();
	}
}

static FString GetLevelTransitionMapName(MapRecord* map)
{
	if (map == nullptr)
	{
		return FString();
	}
	return FString(map->LabelName());
}

LevelTransitionInfo G_BeginLevelTransition(LevelTransitionReason reason, MapRecord* newLevel)
{
	LevelTransitionInfo info;
	info.reason = reason;
	info.serial = ++gLevelTransitionSerial;
	info.oldLevel = currentLevel;
	info.newLevel = newLevel;
	info.oldLevelName = GetLevelTransitionMapName(info.oldLevel);
	info.newLevelName = GetLevelTransitionMapName(info.newLevel);

	CancelPendingPathTracingLevelPreload();
	if (screen != nullptr)
	{
		screen->NotifyLevelUnloadBegin(info);
	}
	return info;
}

void G_CompleteLevelUnload(const LevelTransitionInfo& info)
{
	gi->FreeLevelData();
	if (screen != nullptr)
	{
		screen->NotifyLevelUnloadComplete(info);
	}
}

void G_NotifyLevelLoadBegin(const LevelTransitionInfo& info, MapRecord* loadedLevel)
{
	LevelTransitionInfo loadInfo = info;
	loadInfo.newLevel = loadedLevel != nullptr ? loadedLevel : currentLevel;
	loadInfo.newLevelName = GetLevelTransitionMapName(loadInfo.newLevel);
	if (screen != nullptr)
	{
		screen->NotifyLevelLoadBegin(loadInfo);
	}
	gPathTracingLevelPreloadNeeded = true;
}

static void FinalizePendingLevelStart()
{
	if ((int)nri_ptloadingtrace >= 1)
	{
		Printf("NRI PT loading gate: event=finalize gamestate=%s gameaction=%d pending=%u screen_pending=%u\n",
			GetGameStateName(gamestate),
			(int)gameaction,
			gPendingPathTracingLevelPreload ? 1u : 0u,
			screen != nullptr && screen->IsPathTracingLevelPreloadPending() ? 1u : 0u);
	}
	CancelPendingPathTracingLevelPreload();
	gPathTracingLevelPreloadNeeded = false;
	gameaction = ga_level;
	ResetStatusBar();
	gameInput.resetCrouchToggle();
}

static bool BeginPathTracingLevelPreloadGate()
{
	const double beginStartMs = I_msTimeF();
	if (screen == nullptr || !screen->StartPathTracingLevelPreload())
	{
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=begin result=skip reason=%s gamestate=%s gameaction=%d\n",
				screen == nullptr ? "no-screen" : "start-declined",
				GetGameStateName(gamestate),
				(int)gameaction);
		}
		return false;
	}

	gPendingPathTracingLevelPreload = true;
	gPathTracingLevelPreloadNeeded = false;
	gPathTracingLevelPreloadHeldScreenJobCompletion = false;
	gPathTracingLevelPreloadAwaitingFirstLevelFrame = false;
	gPathTracingLevelPreloadFirstLevelFrameCaptured = false;
	gPathTracingLevelPreloadSimulationHoldActive = false;
	ResetPathTracingLevelLoadClockMetrics();
	nri_scene::SetPersistentVoxelActorStartupTransientMode(true, "begin-level-preload");
	const bool retainedLoadingScreen = IsScreenJobRetainedForLevelLoad();
	bool loadingScreenStarted = false;
	if (!retainedLoadingScreen && cl_loadingscreens && globalCutscenes.LoadingScreen.isdefined())
	{
		loadingScreenStarted = StartCutscene(globalCutscenes.LoadingScreen, SJ_BLOCKUI | SJ_NOCLEAR, [](bool) {});
	}
	gameaction = ga_intermission;
	RebasePathTracingLevelLoadClock("begin", I_msTimeF() - beginStartMs);
	if ((int)nri_ptloadingtrace >= 1)
	{
		Printf("NRI PT loading gate: event=begin result=pending loading_screen=%u loading_screen_started=%u loading_screen_retained=%u loading_runner=%u gamestate=%s gameaction=%d screen_pending=%u\n",
			cl_loadingscreens && globalCutscenes.LoadingScreen.isdefined() ? 1u : 0u,
			loadingScreenStarted ? 1u : 0u,
			retainedLoadingScreen ? 1u : 0u,
			cutscene.runner != nullptr ? 1u : 0u,
			GetGameStateName(gamestate),
			(int)gameaction,
			screen != nullptr && screen->IsPathTracingLevelPreloadPending() ? 1u : 0u);
	}
	return true;
}

static bool TickPendingPathTracingLevelPreloadGate()
{
	if (!gPendingPathTracingLevelPreload || screen == nullptr || !screen->IsPathTracingLevelPreloadPending())
	{
		return false;
	}

	const double tickStartMs = I_msTimeF();
	const bool preloadReady = screen->TickPathTracingLevelPreload();
	RebasePathTracingLevelLoadClock("preload-pump", I_msTimeF() - tickStartMs);
	if ((int)nri_ptloadingtrace >= 2)
	{
		Printf("NRI PT loading gate: event=pre-frame-tick ready=%u gamestate=%s gameaction=%d screen_pending=%u\n",
			preloadReady ? 1u : 0u,
			GetGameStateName(gamestate),
			(int)gameaction,
			screen->IsPathTracingLevelPreloadPending() ? 1u : 0u);
	}
	if (!preloadReady)
	{
		return false;
	}

	FinalizePendingLevelStart();
	nri_scene::SetPersistentVoxelActorStartupTransientMode(true, "await-first-level-frame");
	gPathTracingLevelPreloadFinalCheckNeeded = true;
	gPathTracingLevelPreloadFirstLevelFrameCaptured = false;
	gPathTracingLevelPreloadSimulationHoldActive = false;
	gPathTracingLevelPreloadAwaitingFirstLevelFrame = true;
	if ((int)nri_ptloadingtrace >= 1)
	{
		Printf("NRI PT loading gate: event=preload-ready-await-level-frame gamestate=%s gameaction=%d gametic=%d loading_runner=%u\n",
			GetGameStateName(gamestate),
			(int)gameaction,
			gametic,
			cutscene.runner != nullptr ? 1u : 0u);
	}
	return true;
}

static bool RunPathTracingLevelPreloadFinalCheck()
{
	if (!gPathTracingLevelPreloadFinalCheckNeeded)
	{
		return true;
	}

	if (screen == nullptr)
	{
		gPathTracingLevelPreloadFinalCheckNeeded = false;
		return true;
	}

	if (!screen->IsPathTracingLevelPreloadPending())
	{
		if (!screen->StartPathTracingLevelPreload())
		{
			gPathTracingLevelPreloadFinalCheckNeeded = false;
			return true;
		}
		gPendingPathTracingLevelPreload = true;
	}

	if (gPathTracingLevelPreloadFirstLevelFrameCaptured)
	{
		nri_scene::SetPersistentVoxelActorStartupTransientMode(false, "first-level-capture-final-drain");
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=first-level-capture-final-drain gamestate=%s gameaction=%d gametic=%d hold=%u screen_pending=%u\n",
				GetGameStateName(gamestate),
				(int)gameaction,
				gametic,
				gPathTracingLevelPreloadSimulationHoldActive ? 1u : 0u,
				screen->IsPathTracingLevelPreloadPending() ? 1u : 0u);
			PrintPathTracingLevelPreloadActorCacheStats("first-level-capture-final-drain-cache");
		}
	}

	const double finalCheckStartMs = I_msTimeF();
	for (uint32_t pass = 0; pass < 8u && screen->IsPathTracingLevelPreloadPending(); ++pass)
	{
		(void)screen->TickPathTracingLevelPreload();
	}
	RebasePathTracingLevelLoadClock("final-check-pump", I_msTimeF() - finalCheckStartMs);

	if (screen->IsPathTracingLevelPreloadPending())
	{
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=final-check result=wait gamestate=%s gameaction=%d screen_pending=%u\n",
				GetGameStateName(gamestate),
				(int)gameaction,
				screen->IsPathTracingLevelPreloadPending() ? 1u : 0u);
		}
		gamestate = GS_CUTSCENE;
		gameaction = ga_level;
		return false;
	}

	gPendingPathTracingLevelPreload = false;
	gPathTracingLevelPreloadFinalCheckNeeded = false;
	{
		const auto drainStart = std::chrono::steady_clock::now();
		screen->WaitForCommands(true);
		screen->NotifyPathTracingLevelPreloadFinalCheckRelease();
		const auto drainEnd = std::chrono::steady_clock::now();
		const double drainMs = std::chrono::duration<double, std::milli>(drainEnd - drainStart).count();
		RebasePathTracingLevelLoadClock("final-command-drain", drainMs);
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=final-check-drain ms=%.3f\n", drainMs);
		}
	}
	if ((int)nri_ptloadingtrace >= 1)
	{
		Printf("NRI PT loading gate: event=final-check result=ready gamestate=%s gameaction=%d\n",
			GetGameStateName(gamestate),
			(int)gameaction);
	}
	return true;
}

void NewGame(MapRecord* map, int skill, bool ns = false)
{
	const LevelTransitionInfo transition = G_BeginLevelTransition(LevelTransitionReason::NewGame, map);
	G_CompleteLevelUnload(transition);
	newGameStarted = true;
	ShowIntermission(nullptr, map, nullptr, [=](bool) { 
		gi->NewGame(map, skill, ns);
		G_NotifyLevelLoadBegin(transition);
		if (!BeginPathTracingLevelPreloadGate())
		{
			FinalizePendingLevelStart();
			if (IsScreenJobRetainedForLevelLoad())
			{
				EndScreenJob();
			}
		}
		});
}

//==========================================================================
//
//
//
//==========================================================================

static void GameTicker()
{
	handleevents();

	// Todo: Migrate state changes to here instead of doing them ad-hoc
	while (gameaction != ga_nothing)
	{
		auto ga = gameaction;
		gameaction = ga_nothing;
		switch (ga)
		{
		case ga_autoloadgame:
			C_FlushDisplay();
			if (BackupSaveGame.IsNotEmpty() && cl_resumesavegame)
			{
				DoLoadGame(BackupSaveGame.GetChars());
			}
			else
			{
				g_nextmap = currentLevel;
				FX_StopAllSounds();
				S_SetReverb(0);
				NewGame(g_nextmap, -1);
				BackupSaveGame = "";
			}
			break;

		case ga_completed:
			FX_StopAllSounds();
			S_SetReverb(0);
			gi->LevelCompleted(g_nextmap, g_nextskill);
			break;

		case ga_nextlevel:
		{
			const LevelTransitionInfo transition = G_BeginLevelTransition(LevelTransitionReason::NextLevel, g_nextmap);
			G_CompleteLevelUnload(transition);
			gameaction = ga_level;
			gi->NextLevel(g_nextmap, g_nextskill);
			G_NotifyLevelLoadBegin(transition);
			ResetStatusBar();
			if (!isBlood()) M_Autosave();
			break;
		}

		case ga_newgame:
			FX_StopAllSounds();
			[[fallthrough]];
		case ga_newgamenostopsound:
			DeleteScreenJob();
			S_SetReverb(0);
			C_FlushDisplay();
			BackupSaveGame = "";
			NewGame(g_nextmap, g_nextskill, ga == ga_newgamenostopsound);
			break;

		case ga_startup:
			Mus_Stop();
			FX_StopAllSounds();
			{
				const LevelTransitionInfo transition = G_BeginLevelTransition(LevelTransitionReason::Startup);
				G_CompleteLevelUnload(transition);
			}
			gamestate = GS_STARTUP;
			break;

		case ga_mainmenu:
			FX_StopAllSounds();
			if (isBlood()) Mus_Stop();
			[[fallthrough]];
		case ga_mainmenunostopsound:
			{
				const LevelTransitionInfo transition = G_BeginLevelTransition(LevelTransitionReason::MainMenu);
				G_CompleteLevelUnload(transition);
			}
			gamestate = GS_MENUSCREEN;
			M_StartControlPanel(ga == ga_mainmenu);
			M_SetMenu(NAME_Mainmenu);
			break;

		case ga_creditsmenu:
			FX_StopAllSounds();
			{
				const LevelTransitionInfo transition = G_BeginLevelTransition(LevelTransitionReason::Credits);
				G_CompleteLevelUnload(transition);
			}
			gamestate = GS_MENUSCREEN;
			M_StartControlPanel(false);
			M_SetMenu(NAME_Mainmenu);
			M_SetMenu(NAME_CreditsMenu);
			break;

		case ga_savegame:
			G_DoSaveGame(true, false, savegamefile.GetChars(), savedescription.GetChars());
			gameaction = ga_nothing;
			savegamefile = "";
			savedescription = "";
			break;

		case ga_loadgame:
		case ga_loadgamehidecon:
		//case ga_autoloadgame:
			G_DoLoadGame();
			break;

		case ga_autosave:
			if (gamestate == GS_LEVEL && !newGameStarted) M_Autosave();
			newGameStarted = false;
			break;

		case ga_level:
			if (gPathTracingLevelPreloadNeeded && !gPendingPathTracingLevelPreload)
			{
				if (BeginPathTracingLevelPreloadGate())
				{
					break;
				}
				gPathTracingLevelPreloadNeeded = false;
				if (IsScreenJobRetainedForLevelLoad())
				{
					EndScreenJob();
				}
			}
			if (gPathTracingLevelPreloadAwaitingFirstLevelFrame &&
				gPathTracingLevelPreloadFirstLevelFrameCaptured &&
				!gPathTracingLevelPreloadFinalCheckNeeded)
			{
				gameaction = ga_level;
				return;
			}
			if (!gPathTracingLevelPreloadFinalCheckNeeded)
			{
				CancelPendingPathTracingLevelPreload();
			}
			else
			{
				gPendingPathTracingLevelPreload = false;
			}
			Net_ClearFifo();
			inputState.ClearAllInput();
			gameInput.Clear();
			gamestate = GS_LEVEL;
			if (gPathTracingLevelPreloadFinalCheckNeeded &&
				gPathTracingLevelPreloadAwaitingFirstLevelFrame &&
				!gPathTracingLevelPreloadFirstLevelFrameCaptured)
			{
				if (gametic == 0)
				{
					gPathTracingLevelPreloadAwaitingFirstLevelFrame = false;
					if ((int)nri_ptloadingtrace >= 1)
					{
						Printf("NRI PT loading gate: event=first-level-capture-skip reason=gametic-zero gamestate=%s gameaction=%d gametic=%d\n",
							GetGameStateName(gamestate),
							(int)gameaction,
							gametic);
					}
					if (!RunPathTracingLevelPreloadFinalCheck())
					{
						return;
					}
					return;
				}
				gameaction = ga_level;
				if ((int)nri_ptloadingtrace >= 1)
				{
					Printf("NRI PT loading gate: event=first-level-capture-begin gamestate=%s gameaction=%d gametic=%d\n",
						GetGameStateName(gamestate),
						(int)gameaction,
						gametic);
				}
				return;
			}
			if (!RunPathTracingLevelPreloadFinalCheck())
			{
				return;
			}
			if (gPathTracingLevelPreloadAwaitingFirstLevelFrame &&
				gPathTracingLevelPreloadFirstLevelFrameCaptured)
			{
				gameaction = ga_level;
			}
			return;

		case ga_intro:
			gamestate = GS_INTRO;
			break;

		case ga_intermission:
			gamestate = GS_CUTSCENE;
			break;

		case ga_fullconsole:
			C_FullConsole();
			Mus_Stop();
			gameaction = ga_nothing;
			break;

		case ga_endscreenjob:
			if (IsPathTracingLevelLoadBoundaryActive())
			{
				if ((int)nri_ptloadingtrace >= 1)
				{
					Printf("NRI PT loading gate: event=screenjob-end-held source=queued-action gamestate=%s gameaction=%d loading_runner=%u\n",
						GetGameStateName(gamestate),
						(int)gameaction,
						cutscene.runner != nullptr ? 1u : 0u);
				}
			}
			else if (!netgame && !demoplayback && IsLevelTransitionScreenJob())
			{
				CompleteLevelTransitionScreenJob();
			}
			else
			{
				EndScreenJob();
			}
			break;

			// for later
		// case ga_recordgame,			// start a new demo recording (later)
		// case ga_loadgameplaydemo,	// load a savegame and play a demo.

		default:
			break;
		}
		C_AdjustBottom();
	}

	// get commands, check consistancy, and build new consistancy check
	int buf = (gametic / ticdup) % BACKUPTICS;

	for (int i = 0; i < MAXPLAYERS; i++)
	{
		if (playeringame[i])
		{
			ticcmd_t* cmd = &PlayerArray[i]->cmd;
			ticcmd_t* newcmd = &netcmds[i][buf];
			PlayerArray[i]->lastcmd = *cmd;
			PlayerArray[i]->resetCameraAngles();

			if ((gametic % ticdup) == 0)
			{
				RunNetSpecs(i, buf);
			}
#if 0
			if (demorecording)
			{
				G_WriteDemoTiccmd(newcmd, i, buf);
			}
			if (demoplayback)
			{
				G_ReadDemoTiccmd(cmd, i);
			}
			else
#endif
			{
				*cmd = *newcmd;
				PlayerArray[i]->cmdSyncInput = netcmdsync[i][buf];
				if (i == myconnectindex && PerfLoopTraceActive())
				{
					PerfLoopTraceNoteCommandSync(PlayerArray[i]->cmdSyncInput);
				}
			}


			if (netgame && /*!demoplayback &&*/ (gametic % ticdup) == 0)
			{
#if 0
				//players[i].inconsistant = 0;
				if (gametic > BACKUPTICS * ticdup && consistancy[i][buf] != cmd->consistancy)
				{
					players[i].inconsistant = gametic - BACKUPTICS * ticdup;
				}
#endif
				consistency[i][buf] = gi->GetPlayerChecksum(i);
			}
		}
	}

	C_RunDelayedCommands();
	updatePauseStatus();

	switch (gamestate)
	{
	default:
	case GS_STARTUP:
		gi->Startup();
		break;

	case GS_LEVEL:
		gameupdatetime.Reset();
		gameupdatetime.Clock();
		gameInput.ResetInputSync();
		gi->Ticker();
		TickStatusBar();
		levelTextTime--;
		gameupdatetime.Unclock();
		break;

	case GS_MENUSCREEN:
	case GS_FULLCONSOLE:
		break;
	case GS_CUTSCENE:
	case GS_INTRO:
	{
		TickPendingPathTracingLevelPreloadGate();
		const bool levelLoadPresentationHeld = IsPathTracingLevelLoadBoundaryActive();
		const bool screenJobComplete = ScreenJobTick();
		if (screenJobComplete && levelLoadPresentationHeld && !gPathTracingLevelPreloadHeldScreenJobCompletion)
		{
			gPathTracingLevelPreloadHeldScreenJobCompletion = true;
			if ((int)nri_ptloadingtrace >= 1)
			{
				Printf("NRI PT loading gate: event=screenjob-complete-held gamestate=%s gameaction=%d screen_pending=%u\n",
					GetGameStateName(gamestate),
					(int)gameaction,
					screen != nullptr && screen->IsPathTracingLevelPreloadPending() ? 1u : 0u);
			}
		}
		if (screenJobComplete && !levelLoadPresentationHeld)
		{
			// synchronize termination with the playsim.
			Net_WriteByte(DEM_ENDSCREENJOB);
		}
		break;
	}

	}
}


void DrawOverlays()
{
	NetUpdate();			// send out any new accumulation
	TickVoxelPolicyEditor();
	TickMapSmokeEmitterEditor();
	DrawMapSmokeEmitterEditorOverlay();
	TickActorLightEditor();

	const auto overlayStart = Capture2DSnapshot();
	if (gamestate != GS_INTRO) // do not draw overlays on the intros
	{
		if (PerfLoopTraceActive())
		{
			perf2DProducerTraceStats.chat = Trace2DProducer([]() { CT_Drawer(); });
			perf2DProducerTraceStats.console = Trace2DProducer([]() { C_DrawConsole(); });
			perf2DProducerTraceStats.menu = Trace2DProducer([]() { M_Drawer(); });
			perf2DProducerTraceStats.stats = Trace2DProducer([]()
			{
				PerfLoop2DTextScope textScope(PerfLoop2DTextLabel::Stats);
				FStat::PrintStat(twod);
			});
		}
		else
		{
			// Draw overlay elements
			CT_Drawer();
			C_DrawConsole();
			M_Drawer();
			FStat::PrintStat(twod);
		}
	}
	else if (PerfLoopTraceActive())
	{
		perf2DProducerTraceStats.introSkipped = true;
	}

	if (PerfLoopTraceActive())
	{
		perf2DProducerTraceStats.rate = Trace2DProducer([]() { DrawRateStuff(); });
		const auto overlayEnd = Capture2DSnapshot();
		perf2DProducerTraceStats.overlays.commands = overlayEnd.commands - overlayStart.commands;
		perf2DProducerTraceStats.overlays.vertices = overlayEnd.vertices - overlayStart.vertices;
		perf2DProducerTraceStats.overlays.indices = overlayEnd.indices - overlayStart.indices;
		perf2DProducerTraceStats.overlays.ms =
			perf2DProducerTraceStats.chat.ms +
			perf2DProducerTraceStats.console.ms +
			perf2DProducerTraceStats.menu.ms +
			perf2DProducerTraceStats.stats.ms +
			perf2DProducerTraceStats.rate.ms;
	}
	else
	{
		DrawRateStuff();
	}
}

//==========================================================================
//
// Display
//
//==========================================================================
CVAR(String, drawtile, "", 0)	// debug stuff. Draws the tile with the given number on top of thze HUD

void Display()
{
	if (screen == nullptr)
	{
		perfDisplayTraceStats.skippedInactive = true;
		return;
	}
	const double levelLoadDisplayStartMs = I_msTimeF();

	const bool pathTracingGuiCaptureActive =
		gamestate == GS_LEVEL &&
		(M_Active() || System_WantGuiCapture());
	screen->SetPathTracingGuiCaptureState(pathTracingGuiCaptureActive);

	if (!AppActive && (screen->IsFullscreen() || !vid_activeinbackground))
	{
		perfDisplayTraceStats.skippedInactive = true;
		return;
	}
	
	FTexture* wipestart = nullptr;
	if (nextwipe != wipe_None)
	{
		wipestart = screen->WipeStartScreen();
	}

	double stageStart = I_msTimeF();
	screen->FrameTime = I_msTimeFS();
	tileUpdateAnimations();
	screen->BeginFrame();
	perfDisplayTraceStats.beginFrameMs += I_msTimeF() - stageStart;
	twodpsp.Clear();
	twodpsp.SetSize(screen->GetWidth(), screen->GetHeight());
	twodpsp.ClearClipRect();
	twod->Clear();
	//twod->SetSize(screen->GetWidth(), screen->GetHeight());
	twod->Begin(screen->GetWidth(), screen->GetHeight());
	twod->ClearClipRect();
	bool levelRenderedThisFrame = false;
	bool rebaseLevelLoadCaptureAfterPresent = false;
	bool releaseLevelLoadBoundaryAfterPresent = false;
	switch (gamestate)
	{
	case GS_MENUSCREEN:
	case GS_FULLCONSOLE:
		gi->DrawBackground();
		break;

	case GS_INTRO:
	case GS_CUTSCENE:
		ScreenJobDraw();
		break;

	case GS_LEVEL:
		if (gametic != 0)
		{
			levelRenderedThisFrame = true;
			perfDisplayTraceStats.levelRendered = true;
			stageStart = I_msTimeF();
			screen->FrameTime = I_msTimeFS();
			screen->BeginFrame();
			screen->SetSceneRenderTarget(gl_ssao != 0);
			//updateModelInterpolation();
			gi->Render();
			if (PerfLoopTraceActive())
			{
				perf2DProducerTraceStats.fullscreenBlends = Trace2DProducer([]() { DrawFullscreenBlends(); });
				perf2DProducerTraceStats.mapTitle = Trace2DProducer([]() { drawMapTitle(); });
			}
			else
			{
				DrawFullscreenBlends();
				drawMapTitle();
			}
			perfDisplayTraceStats.renderMs += I_msTimeF() - stageStart;
			break;
		}
		[[fallthrough]];

	default:
		twod->ClearScreen();
		break;
	}

	if (gPathTracingLevelPreloadAwaitingFirstLevelFrame && gamestate == GS_LEVEL)
	{
		if (cutscene.runner != nullptr)
		{
			ScreenJobDraw();
		}
		if (levelRenderedThisFrame)
		{
			if (!gPathTracingLevelPreloadFirstLevelFrameCaptured)
			{
				gPathTracingLevelPreloadFirstLevelFrameCaptured = true;
				gPathTracingLevelPreloadSimulationHoldActive = true;
				rebaseLevelLoadCaptureAfterPresent = true;
				if ((int)nri_ptloadingtrace >= 1)
				{
					Printf("NRI PT loading gate: event=first-level-capture-complete gamestate=%s gameaction=%d gametic=%d final_check=%u\n",
						GetGameStateName(gamestate),
						(int)gameaction,
						gametic,
						gPathTracingLevelPreloadFinalCheckNeeded ? 1u : 0u);
					Printf("NRI PT loading gate: event=simulation-hold-begin gamestate=%s gameaction=%d gametic=%d\n",
						GetGameStateName(gamestate),
						(int)gameaction,
						gametic);
					PrintPathTracingLevelPreloadActorCacheStats("first-level-capture-complete-cache");
				}
			}
			else if (!gPathTracingLevelPreloadFinalCheckNeeded)
			{
				releaseLevelLoadBoundaryAfterPresent = true;
			}
		}
	}
	
	stageStart = I_msTimeF();
	if (nextwipe == wipe_None)
	{
		DrawOverlays();
		if (drawtile[0])
		{
			auto drawTileFunc = []()
			{
				auto tex = TexMan.CheckForTexture(drawtile, ETextureType::Any);
				if (!tex.isValid()) tex = tileGetTextureID(atoi(drawtile));
				if (tex.isValid())
				{
					auto tx = TexMan.GetGameTexture(tex, true);
					if (tx)
					{
						int width = (int)tx->GetDisplayWidth();
						int height = (int)tx->GetDisplayHeight();
						int dwidth, dheight;
						if (width > height)
						{
							dwidth = screen->GetWidth() / 4;
							dheight = height * dwidth / width;
						}
						else
						{
							dheight = screen->GetHeight() / 4;
							dwidth = width * dheight / height;
						}
						DrawTexture(twod, tx, 0, 0, DTA_DestWidth, dwidth, DTA_DestHeight, dheight, TAG_DONE);
					}
				}
			};
			if (PerfLoopTraceActive())
			{
				perf2DProducerTraceStats.drawtile = Trace2DProducer(drawTileFunc);
			}
			else
			{
				drawTileFunc();
			}
		}
	}
	else
	{
		PerformWipe(wipestart, screen->WipeEndScreen(), nextwipe, true, DrawOverlays);
		nextwipe = wipe_None;
	}
	perfDisplayTraceStats.overlayMs += I_msTimeF() - stageStart;

	stageStart = I_msTimeF();
	if (PerfLoopTraceActive())
	{
		const auto preUpdate2D = Capture2DSnapshot();
		perf2DProducerTraceStats.totalCommands = preUpdate2D.commands;
		perf2DProducerTraceStats.totalVertices = preUpdate2D.vertices;
		perf2DProducerTraceStats.totalIndices = preUpdate2D.indices;
	}
	screen->Update();
	perfDisplayTraceStats.updateMs += I_msTimeF() - stageStart;

	if (releaseLevelLoadBoundaryAfterPresent)
	{
		nri_scene::SetPersistentVoxelActorStartupTransientMode(false, "first-level-frame-release");
		if ((int)nri_ptloadingtrace >= 1)
		{
			PrintPathTracingLevelPreloadActorCacheStats("first-level-frame-release-cache");
			Printf("NRI PT loading gate: event=simulation-hold-end gamestate=%s gameaction=%d gametic=%d loading_runner=%u\n",
				GetGameStateName(gamestate),
				(int)gameaction,
				gametic,
				cutscene.runner != nullptr ? 1u : 0u);
			Printf("NRI PT loading gate: event=first-level-frame-release gamestate=%s gameaction=%d gametic=%d loading_runner=%u\n",
				GetGameStateName(gamestate),
				(int)gameaction,
				gametic,
				cutscene.runner != nullptr ? 1u : 0u);
		}
		screen->NotifyPathTracingLevelFirstFrameRelease();
		if (cutscene.runner != nullptr)
		{
			EndScreenJob();
		}
		RebasePathTracingLevelLoadClock("first-frame-release", I_msTimeF() - levelLoadDisplayStartMs, true);
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("PERF level load clock: event=release transition_serial=%llu policy=%s rebases=%u excluded_ms=%.3f hold_frames=%u suppressed_ticks=%u gametic=%d presentation=%llu\n",
				(unsigned long long)gPathTracingLevelLoadClockSerial,
				GetPathTracingLevelLoadClockPolicyName(),
				gPathTracingLevelLoadClockRebases,
				gPathTracingLevelLoadClockExcludedMs,
				gPathTracingLevelLoadClockHoldFrames,
				gPathTracingLevelLoadClockSuppressedTicks,
				gametic,
				(unsigned long long)gPresentationGeneration);
		}
		gPathTracingLevelLoadClockAwaitingFirstPostRelease = true;
		gPathTracingLevelPreloadAwaitingFirstLevelFrame = false;
		gPathTracingLevelPreloadFirstLevelFrameCaptured = false;
		gPathTracingLevelPreloadSimulationHoldActive = false;
		gPathTracingLevelPreloadHeldScreenJobCompletion = false;
		if (gameaction == ga_level)
		{
			gameaction = ga_nothing;
		}
	}
	else if (rebaseLevelLoadCaptureAfterPresent)
	{
		RebasePathTracingLevelLoadClock("first-frame-capture", I_msTimeF() - levelLoadDisplayStartMs);
	}
}

//==========================================================================
//
// Forces playsim processing time to be consistent across frames.
// This improves interpolation for frames in between tics.
//
// With this cvar off the mods with a high playsim processing time will appear
// less smooth as the measured time used for interpolation will vary.
//
//==========================================================================

static void TicStabilityWait()
{
	using namespace std::chrono;
	using namespace std::this_thread;

	if (!r_ticstability)
		return;

	uint64_t start = duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
	while (true)
	{
		uint64_t cur = duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
		if (cur - start > stabilityticduration)
			break;
	}
}

static void TicStabilityBegin()
{
	using namespace std::chrono;
	stabilitystarttime = duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

static void TicStabilityEnd()
{
	using namespace std::chrono;
	uint64_t stabilityendtime = duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
	stabilityticduration = min(stabilityendtime - stabilitystarttime, (uint64_t)1'000'000);
}

//==========================================================================
//
// The most important function in the engine.
//
//==========================================================================

void TryRunTics (void)
{
	int 		i;
	int 		lowtic;
	int 		realtics;
	int 		availabletics;
	int 		counts;
	int 		numplaying;
	const double traceStartMs = I_msTimeF();
	perfTryRunTicsTraceStats = {};
	gGameUpdateTicksThisPresentation = 0;

	// Keep presentation, renderer publication, fence retirement, and GPU work
	// running while a bounded performance capture holds authoritative gameplay
	// state fixed. This diagnostic control is deliberately session-only and is
	// never permitted in network games, demos, or outside an active level.
	// Fixed presentations must bypass I_WaitForTic: frozen time cannot advance.
	if ((int)perf_fixedsimulationframes > 0 &&
		gamestate == GS_LEVEL && !netgame && !demoplayback)
	{
		if (!perfFixedSimulationOwnsTimeFreeze && !I_IsTimeFrozen())
		{
			I_FreezeTime(true);
			perfFixedSimulationOwnsTimeFreeze = true;
		}
		perf_fixedsimulationframes = (int)perf_fixedsimulationframes - 1;
		perfTryRunTicsTraceStats.fixedSimulationReturn = true;
		perfTryRunTicsTraceStats.durationMs = I_msTimeF() - traceStartMs;
		return;
	}
	if (perfFixedSimulationOwnsTimeFreeze)
	{
		I_FreezeTime(false);
		perfFixedSimulationOwnsTimeFreeze = false;
	}
	if ((int)perf_fixedsimulationframes > 0)
	{
		perf_fixedsimulationframes = 0;
	}

	// If paused, do not eat more CPU time than we need, because it
	// will all be wasted anyway.
	bool doWait = (cl_capfps || pauseext || (r_NoInterpolate && !M_IsAnimated() && gamestate != GS_CUTSCENE && gamestate != GS_INTRO));

	if (vid_dontdowait && ((vid_maxfps > 0) || (vid_vsync == true)))
		doWait = false;
	perfTryRunTicsTraceStats.doWait = doWait;

	// get real tics
	if (doWait)
	{
		entertic = I_WaitForTic (oldentertics);
	}
	else
	{
		entertic = I_GetTime ();
	}
	realtics = entertic - oldentertics;
	oldentertics = entertic;
	perfTryRunTicsTraceStats.realtics = realtics;

	// get available tics
	NetUpdate ();

	if (pauseext)
	{
		perfTryRunTicsTraceStats.pausedReturn = true;
		perfTryRunTicsTraceStats.durationMs = I_msTimeF() - traceStartMs;
		return;
	}

	lowtic = INT_MAX;
	numplaying = 0;
	for (i = 0; i < doomcom.numnodes; i++)
	{
		if (nodeingame[i])
		{
			numplaying++;
			if (nettics[i] < lowtic)
				lowtic = nettics[i];
		}
	}

	availabletics = lowtic - gametic / ticdup;
	perfTryRunTicsTraceStats.availabletics = availabletics;
	perfTryRunTicsTraceStats.lowtic = lowtic;

	// decide how many tics to run
	if (realtics < availabletics-1)
		counts = realtics+1;
	else if (realtics < availabletics)
		counts = realtics;
	else
		counts = availabletics;
	if (UsePathTracingLevelLoadClockPolicy())
	{
		++gPathTracingLevelLoadClockHoldFrames;
		if (counts > 1)
		{
			gPathTracingLevelLoadClockSuppressedTicks += (uint32_t)(counts - 1);
			counts = 1;
		}
	}
	if (gPathTracingLevelLoadClockAwaitingFirstPostRelease && !netgame && !demoplayback && counts > 1)
	{
		// Close the named boundary with one ordinary simulation tick even when
		// the local command FIFO has an extra predictive tic available.
		counts = 1;
	}
	perfTryRunTicsTraceStats.counts = counts;
	if (gPathTracingLevelLoadClockAwaitingFirstPostRelease)
	{
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("PERF level load clock: event=first-post-release transition_serial=%llu policy=%s realtics=%d counts=%d gametic=%d gamestate=%s presentation=%llu\n",
				(unsigned long long)gPathTracingLevelLoadClockSerial,
				GetPathTracingLevelLoadClockPolicyName(),
				realtics,
				counts,
				gametic,
				GetGameStateName(gamestate),
				(unsigned long long)gPresentationGeneration);
		}
		gPathTracingLevelLoadClockAwaitingFirstPostRelease = false;
	}

	// Uncapped framerate needs seprate checks
	if (counts == 0 && !doWait)
	{
		TicStabilityWait();

		// Check possible stall conditions
		Net_CheckLastReceived(counts);
		if (realtics >= 1)
		{
			C_Ticker();
			M_Ticker();
			++gEngineUpdateGeneration;
			if (gEngineUpdateGeneration == 0) gEngineUpdateGeneration = 1;
			// Repredict the player for new buffered movement
#if 0
			gi->Unpredict();
			gi->Predict(myconnectindex);
#endif
		}
		if (!gameInput.SyncInput())
		{
			gameInput.getInput();
		}
		perfTryRunTicsTraceStats.zeroCountReturn = true;
		perfTryRunTicsTraceStats.durationMs = I_msTimeF() - traceStartMs;
		return;
	}

	if (counts < 1)
		counts = 1;

	// wait for new tics if needed
	while (lowtic < gametic + counts)
	{
		perfTryRunTicsTraceStats.waitLoopIterations++;
		NetUpdate ();
		lowtic = INT_MAX;

		for (i = 0; i < doomcom.numnodes; i++)
			if (nodeingame[i] && nettics[i] < lowtic)
				lowtic = nettics[i];

		lowtic = lowtic * ticdup;

		if (lowtic < gametic)
			I_Error ("TryRunTics: lowtic < gametic");

		// Check possible stall conditions
		Net_CheckLastReceived (counts);

		// Update time returned by I_GetTime, but only if we are stuck in this loop
		if (lowtic < gametic + counts)
			I_SetFrameTime();

		// don't stay in here forever -- give the menu a chance to work
		if (I_GetTime () - entertic >= 1)
		{
			C_Ticker ();
			M_Ticker ();
			++gEngineUpdateGeneration;
			if (gEngineUpdateGeneration == 0) gEngineUpdateGeneration = 1;
			// Repredict the player for new buffered movement
#if 0
			gi->Unpredict();
			gi->Predict(myconnectindex);
#endif
			perfTryRunTicsTraceStats.waitLoopReturn = true;
			perfTryRunTicsTraceStats.durationMs = I_msTimeF() - traceStartMs;
			return;
		}
	}

	//Tic lowtic is high enough to process this gametic. Clear all possible waiting info
	hadlate = false;
#if 0
	for (i = 0; i < MAXPLAYERS; i++)
		players[i].waiting = false;
#endif
	lastglobalrecvtime = I_GetTime (); //Update the last time the game tic'd over

	// run the count tics
	if (counts > 0)
	{
#if 0
		gi->Unpredict();
#endif
		while (counts--)
		{
			perfTryRunTicsTraceStats.ticksRun++;
			TicStabilityBegin();
			if (gametic > lowtic)
			{
				I_Error ("gametic>lowtic");
			}
#if 0
			if (advancedemo)
			{
				D_DoAdvanceDemo ();
			}
#endif
			C_Ticker ();
			M_Ticker ();
			GameTicker();
			++gEngineUpdateGeneration;
			if (gEngineUpdateGeneration == 0) gEngineUpdateGeneration = 1;
			++gGameUpdateGeneration;
			if (gGameUpdateGeneration == 0)
			{
				gGameUpdateGeneration = 1;
			}
			PublishActorPresentationSnapshot(gGameUpdateGeneration);
			++gGameUpdateTicksThisPresentation;
			gametic++;

			NetUpdate ();	// check for new console commands
			TicStabilityEnd();
			if ((int)perf_fixedsimulationframes > 0 &&
				gamestate == GS_LEVEL && !netgame && !demoplayback && counts > 0)
			{
				// A delayed console command can arm fixed simulation from inside
				// GameTicker after this catch-up batch was calculated. Preserve the
				// state reached by the arming tick instead of consuming the stale
				// remainder before the first fixed presentation.
				perfTryRunTicsTraceStats.fixedSimulationSuppressedTailTicks = (uint32_t)counts;
				counts = 0;
			}
			if (UsePathTracingLevelLoadClockPolicy() && counts > 0)
			{
				// The named loading boundary may begin inside a batch that was
				// calculated before map/preload work started. Do not spend the
				// remainder as catch-up ticks before another loading presentation.
				gPathTracingLevelLoadClockSuppressedTicks += (uint32_t)counts;
				counts = 0;
				++gPathTracingLevelLoadClockHoldFrames;
			}
		}
#if 0
		gi->Predict(myconnectindex);
#endif
		gi->UpdateSounds();
		soundEngine->UpdateSounds(I_GetTime());
	}
	else
	{
		TicStabilityWait();
	}
	perfTryRunTicsTraceStats.durationMs = I_msTimeF() - traceStartMs;
}


//==========================================================================
//
// MainLoop - will never return aside from exceptions being thrown.
//
//==========================================================================

void MainLoop ()
{
	int lasttic = 0;
	uint64_t traceFrame = 0;

	// Clamp the timer to TICRATE until the playloop has been entered.
	r_NoInterpolate = true;

	if (userConfig.CommandMap.IsNotEmpty())
	{
		auto maprecord = FindMapByName(userConfig.CommandMap.GetChars());
		if (maprecord == nullptr)
		{
			maprecord = SetupUserMap(userConfig.CommandMap.GetChars(), g_gameType & GAMEFLAG_DUKE? "dethtoll.mid" : nullptr);
		}
		userConfig.CommandMap = "";
		if (maprecord)
		{
			DeferredStartGame(maprecord, g_nextskill);
		}
	}

	for (;;)
	{
		try
		{
			traceFrame++;
			++gPresentationGeneration;
			if (gPresentationGeneration == 0) gPresentationGeneration = 1;
			PerfCompactCaptureBeginOuterFrame(gPresentationGeneration);
			if (PerfLoopTraceActive() || PerfCompactCaptureTimingActive())
			{
				if (PerfLoopTraceActive())
				{
					PerfLoopTraceResetInputStats();
					PerfLoopTraceReset2DProducerStats();
					PerfLoopTraceResetCameraStats();
				}
				perfTryRunTicsTraceStats = {};
				perfDisplayTraceStats = {};
				perf2DProducerTraceStats = {};
			}

			// frame syncronous IO operations
			const double frameStartMs = I_msTimeF();
			double startFrameMs = 0.0;
			if (gametic > lasttic)
			{
				const double stageStartMs = I_msTimeF();
				lasttic = gametic;
				I_StartFrame ();
				startFrameMs = I_msTimeF() - stageStartMs;
			}
			I_SetFrameTime();

			// update the scale factor for unsynchronised input here.
			gameInput.UpdateInputScale();
			if (PerfLoopTraceActive())
			{
				PerfLoopTraceNoteInputMode(gameInput.SyncInput(), gameInput.GetInputScale());
			}

			const double tryRunStartMs = I_msTimeF();
			TryRunTics (); // will run at least one tic
			const double tryRunMs = I_msTimeF() - tryRunStartMs;
			// Update display, next frame, with current state.
			const double startTicStartMs = I_msTimeF();
			I_StartTic();
			const double startTicMs = I_msTimeF() - startTicStartMs;

			const double displayStartMs = I_msTimeF();
			Display();
			const double displayMs = I_msTimeF() - displayStartMs;
			const double musicStartMs = I_msTimeF();
			Mus_UpdateMusic();		// must be at the end.
			const double musicMs = I_msTimeF() - musicStartMs;
			if (PerfCompactCaptureTimingActive())
			{
				const auto renderTrace = GetPerfRenderTraceStats();
				PerfCompactOuterFrame compact = {};
				compact.traceFrame = traceFrame;
				compact.presentationGeneration = gPresentationGeneration;
				compact.simulationGeneration = gGameUpdateGeneration;
				compact.engineGeneration = gEngineUpdateGeneration;
				compact.gametic = gametic;
				compact.startFrameMs = startFrameMs;
				compact.tryMs = tryRunMs;
				compact.tryTracedMs = perfTryRunTicsTraceStats.durationMs;
				compact.displayMs = displayMs;
				compact.displayBeginMs = perfDisplayTraceStats.beginFrameMs;
				compact.displayRenderMs = perfDisplayTraceStats.renderMs;
				compact.displayOverlayMs = perfDisplayTraceStats.overlayMs;
				compact.displayUpdateMs = perfDisplayTraceStats.updateMs;
				compact.startTicMs = startTicMs;
				compact.musicMs = musicMs;
				compact.frameMs = I_msTimeF() - frameStartMs;
				compact.nriTotalMs = renderTrace.nriAllMs;
				compact.nriInitializeMs = renderTrace.nriInitializeMs;
				compact.nriFrameResourcesMs = renderTrace.nriFrameResourcesMs;
				compact.nriUpdateStateMs = renderTrace.nriUpdateStateMs;
				compact.nriSceneCaptureMs = renderTrace.nriSceneCaptureMs;
				compact.nriGeometryBuildMs = renderTrace.nriGeometryBuildMs;
				compact.nriMaterialBuildMs = renderTrace.nriMaterialBuildMs;
				compact.nriSceneTexturesMs = renderTrace.nriSceneTexturesMs;
				compact.nriSceneBuffersMs = renderTrace.nriSceneBuffersMs;
				compact.nriAccelerationMs = renderTrace.nriAccelerationMs;
				compact.nriFrameGraphMs = renderTrace.nriFrameGraphMs;
				compact.nriTraceMs = renderTrace.nriTraceOpaqueMs;
				compact.nriDenoiseMs = renderTrace.nriDenoiserMs;
				compact.nriComposeMs = renderTrace.nriCompositionMs;
				compact.nriUpscaleMs = renderTrace.nriUpscaleMs;
				compact.nriFinalMs = renderTrace.nriFinalMs;
				compact.realtics = perfTryRunTicsTraceStats.realtics;
				compact.availabletics = perfTryRunTicsTraceStats.availabletics;
				compact.counts = perfTryRunTicsTraceStats.counts;
				compact.ticks = perfTryRunTicsTraceStats.ticksRun;
				compact.waitLoops = perfTryRunTicsTraceStats.waitLoopIterations;
				compact.doWait = perfTryRunTicsTraceStats.doWait;
				compact.zeroReturn = perfTryRunTicsTraceStats.zeroCountReturn;
				compact.waitReturn = perfTryRunTicsTraceStats.waitLoopReturn;
				compact.pausedReturn = perfTryRunTicsTraceStats.pausedReturn;
				compact.fixedSimulationReturn = perfTryRunTicsTraceStats.fixedSimulationReturn;
				compact.fixedSimulationSuppressedTailTicks = perfTryRunTicsTraceStats.fixedSimulationSuppressedTailTicks;
				compact.displaySkipped = perfDisplayTraceStats.skippedInactive;
				compact.levelRendered = perfDisplayTraceStats.levelRendered;
				compact.stateIsLevel = gamestate == GS_LEVEL;
				compact.nriActive = renderTrace.nriActive;
				PerfCompactCaptureEndOuterFrame(compact);
			}

			if (PerfLoopTraceActive())
			{
				const auto inputTrace = PerfLoopTraceGetInputStats();
				const auto cameraTrace = PerfLoopTraceGetCameraStats();
				const auto renderTrace = GetPerfRenderTraceStats();
				const double frameMs = I_msTimeF() - frameStartMs;
				Printf(
					"PERF loop trace: frame=%llu presentation_gen=%llu simulation_gen=%llu engine_gen=%llu state=%s gametic=%d startframe_ms=%.3f try_ms=%.3f try_traced_ms=%.3f display_ms=%.3f display_begin_ms=%.3f display_render_ms=%.3f display_overlay_ms=%.3f display_update_ms=%.3f starttic_ms=%.3f music_ms=%.3f frame_ms=%.3f do_wait=%d realtics=%d avail=%d counts=%d ticks=%d wait_loops=%d zero_return=%d wait_return=%d paused_return=%d fixed_return=%d fixed_tail_suppressed=%u display_skip=%d level_rendered=%d\n",
					(unsigned long long)traceFrame,
					(unsigned long long)gPresentationGeneration,
					(unsigned long long)gGameUpdateGeneration,
					(unsigned long long)gEngineUpdateGeneration,
					GetGameStateName(gamestate),
					gametic,
					startFrameMs,
					tryRunMs,
					perfTryRunTicsTraceStats.durationMs,
					displayMs,
					perfDisplayTraceStats.beginFrameMs,
					perfDisplayTraceStats.renderMs,
					perfDisplayTraceStats.overlayMs,
					perfDisplayTraceStats.updateMs,
					startTicMs,
					musicMs,
					frameMs,
					perfTryRunTicsTraceStats.doWait ? 1 : 0,
					perfTryRunTicsTraceStats.realtics,
					perfTryRunTicsTraceStats.availabletics,
					perfTryRunTicsTraceStats.counts,
					perfTryRunTicsTraceStats.ticksRun,
					perfTryRunTicsTraceStats.waitLoopIterations,
					perfTryRunTicsTraceStats.zeroCountReturn ? 1 : 0,
					perfTryRunTicsTraceStats.waitLoopReturn ? 1 : 0,
					perfTryRunTicsTraceStats.pausedReturn ? 1 : 0,
					perfTryRunTicsTraceStats.fixedSimulationReturn ? 1 : 0,
					perfTryRunTicsTraceStats.fixedSimulationSuppressedTailTicks,
					perfDisplayTraceStats.skippedInactive ? 1 : 0,
					perfDisplayTraceStats.levelRendered ? 1 : 0);
				Printf(
					"PERF render trace: frame=%llu hw_all=%.3f hw_finish=%.3f hw_render=%.3f hw_setup=%.3f hw_portal=%.3f hw_post=%.3f hw_drawcalls=%.3f wall_render=%.3f wall_setup=%.3f wall_clip=%.3f bsp=%.3f flat_render=%.3f flat_setup=%.3f sprite_render=%.3f sprite_setup=%.3f twod=%.3f finish3d=%.3f mt_wait=%.3f wt_total=%.3f walls=%d flats=%d sprites=%d decals=%d portals=%d verts=%d flat_verts=%d flat_prims=%d\n",
					(unsigned long long)traceFrame,
					renderTrace.allMs,
					renderTrace.finishMs,
					renderTrace.renderAllMs,
					renderTrace.processAllMs,
					renderTrace.portalAllMs,
					renderTrace.postProcessMs,
					renderTrace.drawCallsMs,
					renderTrace.renderWallMs,
					renderTrace.setupWallMs,
					renderTrace.clipWallMs,
					renderTrace.bspMs,
					renderTrace.renderFlatMs,
					renderTrace.setupFlatMs,
					renderTrace.renderSpriteMs,
					renderTrace.setupSpriteMs,
					renderTrace.twoDMs,
					renderTrace.finish3DMs,
					renderTrace.mtWaitMs,
					renderTrace.wtTotalMs,
					renderTrace.renderedWalls,
					renderTrace.renderedFlats,
					renderTrace.renderedSprites,
					renderTrace.renderedDecals,
					renderTrace.renderedPortals,
					renderTrace.renderedVertices,
					renderTrace.flatVertexCount,
					renderTrace.flatPrimitiveCount);
				if (renderTrace.nriActive)
				{
					Printf(
						"PERF render trace NRI: frame=%llu total=%.3f init=%.3f res=%.3f state=%.3f capture=%.3f geo=%.3f mats=%.3f palette=%.3f textures=%.3f buffers=%.3f as=%.3f bootstrap=%.3f graph=%.3f copy=%.3f wait=%.3f wait_present=%.3f acquire=%.3f submit=%.3f present=%.3f trace=%.3f denoise=%.3f compose=%.3f upscale=%.3f final=%.3f raw_present=%.3f final_present=%.3f\n",
						(unsigned long long)traceFrame,
						renderTrace.nriAllMs,
						renderTrace.nriInitializeMs,
						renderTrace.nriFrameResourcesMs,
						renderTrace.nriUpdateStateMs,
						renderTrace.nriSceneCaptureMs,
						renderTrace.nriGeometryBuildMs,
						renderTrace.nriMaterialBuildMs,
						renderTrace.nriPaletteUploadMs,
						renderTrace.nriSceneTexturesMs,
						renderTrace.nriSceneBuffersMs,
						renderTrace.nriAccelerationMs,
						renderTrace.nriBootstrapDispatchMs,
						renderTrace.nriFrameGraphMs,
						renderTrace.nriCopyFinalMs,
						renderTrace.nriFrameWaitMs,
						renderTrace.nriWaitPresentMs,
						renderTrace.nriAcquireSwapMs,
						renderTrace.nriQueueSubmitMs,
						renderTrace.nriQueuePresentMs,
						renderTrace.nriTraceOpaqueMs,
						renderTrace.nriDenoiserMs,
						renderTrace.nriCompositionMs,
						renderTrace.nriUpscaleMs,
						renderTrace.nriFinalMs,
						renderTrace.nriRawPresentMs,
						renderTrace.nriFinalPresentMs);
				}
				Printf(
					"PERF twod producer trace: frame=%llu total_cmds=%d total_verts=%d total_indices=%d intro_skip=%d overlays_cmds=%d overlays_ms=%.3f fsblend_cmds=%d fsblend_ms=%.3f maptitle_cmds=%d maptitle_ms=%.3f statusbar_cmds=%d statusbar_ms=%.3f althud_cmds=%d althud_ms=%.3f crosshair_cmds=%d crosshair_ms=%.3f chat_cmds=%d chat_ms=%.3f console_cmds=%d console_ms=%.3f menu_cmds=%d menu_ms=%.3f stats_cmds=%d stats_ms=%.3f rate_cmds=%d rate_ms=%.3f drawtile_cmds=%d drawtile_ms=%.3f\n",
					(unsigned long long)traceFrame,
					perf2DProducerTraceStats.totalCommands,
					perf2DProducerTraceStats.totalVertices,
					perf2DProducerTraceStats.totalIndices,
					perf2DProducerTraceStats.introSkipped ? 1 : 0,
					perf2DProducerTraceStats.overlays.commands,
					perf2DProducerTraceStats.overlays.ms,
					perf2DProducerTraceStats.fullscreenBlends.commands,
					perf2DProducerTraceStats.fullscreenBlends.ms,
					perf2DProducerTraceStats.mapTitle.commands,
					perf2DProducerTraceStats.mapTitle.ms,
					PerfLoopTraceGet2DProducerStats().statusBar.commands,
					PerfLoopTraceGet2DProducerStats().statusBar.ms,
					PerfLoopTraceGet2DProducerStats().altHud.commands,
					PerfLoopTraceGet2DProducerStats().altHud.ms,
					PerfLoopTraceGet2DProducerStats().crosshair.commands,
					PerfLoopTraceGet2DProducerStats().crosshair.ms,
					perf2DProducerTraceStats.chat.commands,
					perf2DProducerTraceStats.chat.ms,
					perf2DProducerTraceStats.console.commands,
					perf2DProducerTraceStats.console.ms,
					perf2DProducerTraceStats.menu.commands,
					perf2DProducerTraceStats.menu.ms,
					perf2DProducerTraceStats.stats.commands,
					perf2DProducerTraceStats.stats.ms,
					perf2DProducerTraceStats.rate.commands,
					perf2DProducerTraceStats.rate.ms,
					perf2DProducerTraceStats.drawtile.commands,
					perf2DProducerTraceStats.drawtile.ms);
				const auto twodProducerStats = PerfLoopTraceGet2DProducerStats();
				const auto consoleVisibleGlyphs =
					twodProducerStats.consoleVersionText.glyphs +
					twodProducerStats.consoleBodyText.glyphs +
					twodProducerStats.consoleCommandLineText.glyphs;
				Printf(
					"PERF twod text trace: frame=%llu console_version_calls=%u console_version_glyphs=%u console_version_cmds=%u console_body_calls=%u console_body_glyphs=%u console_body_cmds=%u console_cmd_calls=%u console_cmd_glyphs=%u console_cmd_cmds=%u hud_calls=%u hud_glyphs=%u hud_cmds=%u stats_calls=%u stats_glyphs=%u stats_cmds=%u rate_calls=%u rate_glyphs=%u rate_cmds=%u other_calls=%u other_glyphs=%u other_cmds=%u\n",
					(unsigned long long)traceFrame,
					twodProducerStats.consoleVersionText.calls,
					twodProducerStats.consoleVersionText.glyphs,
					twodProducerStats.consoleVersionText.commands,
					twodProducerStats.consoleBodyText.calls,
					twodProducerStats.consoleBodyText.glyphs,
					twodProducerStats.consoleBodyText.commands,
					twodProducerStats.consoleCommandLineText.calls,
					twodProducerStats.consoleCommandLineText.glyphs,
					twodProducerStats.consoleCommandLineText.commands,
					twodProducerStats.hudText.calls,
					twodProducerStats.hudText.glyphs,
					twodProducerStats.hudText.commands,
					twodProducerStats.statsText.calls,
					twodProducerStats.statsText.glyphs,
					twodProducerStats.statsText.commands,
					twodProducerStats.rateText.calls,
					twodProducerStats.rateText.glyphs,
					twodProducerStats.rateText.commands,
					twodProducerStats.otherText.calls,
					twodProducerStats.otherText.glyphs,
					twodProducerStats.otherText.commands);
				Printf(
					"PERF twod console detail: frame=%llu visible_lines=%u visible_glyphs=%u bg_cmds=%u version_cmds=%u body_cmds=%u cmdline_cmds=%u version_glyphs=%u body_glyphs=%u cmdline_glyphs=%u\n",
					(unsigned long long)traceFrame,
					twodProducerStats.consoleVisibleLines,
					consoleVisibleGlyphs,
					twodProducerStats.consoleBackgroundCommands,
					twodProducerStats.consoleVersionText.commands,
					twodProducerStats.consoleBodyText.commands,
					twodProducerStats.consoleCommandLineText.commands,
					twodProducerStats.consoleVersionText.glyphs,
					twodProducerStats.consoleBodyText.glyphs,
					twodProducerStats.consoleCommandLineText.glyphs);
				Printf(
					"PERF input trace: frame=%llu getevent=%u starttic_calls=%u handleevents=%u msgs=%u burst=%u raw_input=%u raw_keyboard=%u raw_mouse=%u raw_mouse_moves=%u raw_mouse_drop=%u posted_mouse=%u dispatched_mouse=%u sampled_mouse=%u route_yaw=%u route_strafe=%u route_pitch=%u route_aimmove=%u ticcmds=%u yaw_apply=%u pitch_apply=%u fast_camera=%u posted_delta=(%.1f,%.1f) dispatched_delta=(%.1f,%.1f) sampled_delta=(%.1f,%.1f) ticcmd_deg=(%.3f,%.3f) applied_deg=(%.3f,%.3f) fast_camera_deg=(%.3f,%.3f) key_down=%u key_up=%u device_change=%u queue_hw=%u queue_overflow=%u\n",
					(unsigned long long)traceFrame,
					inputTrace.iGetEventCalls,
					inputTrace.startTicCalls,
					inputTrace.handleeventsCalls,
					inputTrace.peekedMessages,
					inputTrace.maxMessageBurst,
					inputTrace.rawInputMessages,
					inputTrace.rawKeyboardPackets,
					inputTrace.rawMousePackets,
					inputTrace.rawMouseMovePackets,
					inputTrace.rawMouseDroppedPackets,
					inputTrace.postedMouseMoves,
					inputTrace.dispatchedMouseMoves,
					inputTrace.sampledMouseInputs,
					inputTrace.mouseYawLookSamples,
					inputTrace.mouseStrafeSamples,
					inputTrace.mousePitchLookSamples,
					inputTrace.mouseAimMoveSamples,
					inputTrace.ticcmdBuilds,
					inputTrace.yawApplyCalls,
					inputTrace.pitchApplyCalls,
					inputTrace.fastCameraApplyCalls,
					inputTrace.postedMouseX,
					inputTrace.postedMouseY,
					inputTrace.dispatchedMouseX,
					inputTrace.dispatchedMouseY,
					inputTrace.sampledMouseX,
					inputTrace.sampledMouseY,
					inputTrace.ticcmdYawDegrees,
					inputTrace.ticcmdPitchDegrees,
					inputTrace.appliedYawDegrees,
					inputTrace.appliedPitchDegrees,
					inputTrace.fastCameraYawDegrees,
					inputTrace.fastCameraPitchDegrees,
					inputTrace.keyDownEvents,
					inputTrace.keyUpEvents,
					inputTrace.deviceChangeEvents,
					inputTrace.eventQueueHighWater,
					inputTrace.eventQueueOverflows);
				Printf(
					"PERF camera trace: frame=%llu sync=%d cmd_sync=%d input_scale=%.6f actor_yaw_calls=%u actor_pitch_calls=%u cam_updates=%u cam_resets=%u render_calls=%u cmd_deg=(%.3f,%.3f) actor_delta=(%.3f,%.3f) actor=(%.3f,%.3f) camera_delta=(%.3f,%.3f) camera=(%.3f,%.3f) render=(%.3f,%.3f) render_frame_delta=(%.3f,%.3f)\n",
					(unsigned long long)traceFrame,
					cameraTrace.syncInput ? 1 : 0,
					cameraTrace.cmdSyncInput ? 1 : 0,
					cameraTrace.inputScale,
					cameraTrace.actorYawCalls,
					cameraTrace.actorPitchCalls,
					cameraTrace.cameraUpdateCalls,
					cameraTrace.cameraResetCalls,
					cameraTrace.renderCalls,
					cameraTrace.consumedCmdYawDegrees,
					cameraTrace.consumedCmdPitchDegrees,
					cameraTrace.actorYawDeltaDegrees,
					cameraTrace.actorPitchDeltaDegrees,
					cameraTrace.actorYawDegrees,
					cameraTrace.actorPitchDegrees,
					cameraTrace.cameraYawDeltaDegrees,
					cameraTrace.cameraPitchDeltaDegrees,
					cameraTrace.cameraYawDegrees,
					cameraTrace.cameraPitchDegrees,
					cameraTrace.renderYawDegrees,
					cameraTrace.renderPitchDegrees,
					cameraTrace.renderFrameDeltaYawDegrees,
					cameraTrace.renderFrameDeltaPitchDegrees);
				if (renderTrace.nriActive)
				{
					Printf("----------end perf trace frame %llu\n",
						(unsigned long long)traceFrame);
				}
				const int remainingTraceFrames = (int)perf_looptraceframes - 1;
				perf_looptraceframes = remainingTraceFrames > 0 ? remainingTraceFrames : 0;
			}
		}
		catch (CRecoverableError &error)
		{
			PerfCompactCaptureAbort("recoverable-error");
			if (PerfLoopTraceActive())
			{
				Printf("PERF loop trace caught: frame=%llu type=recoverable state=%s gametic=%d\n",
					(unsigned long long)traceFrame,
					GetGameStateName(gamestate),
					gametic);
			}
			if (error.GetMessage ())
			{
				Printf (PRINT_BOLD, "\n%s\n", error.GetMessage());
			}
			gi->ErrorCleanup();
			M_ClearMenus();
			C_FullConsole();
			gameaction = ga_nothing;
		}
		catch (CVMAbortException &error)
		{
			PerfCompactCaptureAbort("vm-abort");
			if (PerfLoopTraceActive())
			{
				Printf("PERF loop trace caught: frame=%llu type=vmabort state=%s gametic=%d\n",
					(unsigned long long)traceFrame,
					GetGameStateName(gamestate),
					gametic);
			}
			error.MaybePrintMessage();
			Printf("%s", error.stacktrace.GetChars());
			gi->ErrorCleanup();
			twod->SetOffset(DVector2(0, 0));
			M_ClearMenus();
			C_FullConsole();
		}
	}
}
