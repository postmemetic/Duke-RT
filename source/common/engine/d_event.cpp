/*
** c_dispatch.cpp
** Functions for executing console commands and aliases
**
**---------------------------------------------------------------------------
** Copyright 1998-2016 Randy Heit
** Copyright 2003-2019 Christoph Oelckers
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

#include <algorithm>

#include "c_bind.h"
#include "d_eventbase.h"
#include "c_console.h"
#include "d_gui.h"
#include "menu.h"
#include "utf8.h"
#include "m_joy.h"
#include "vm.h"
#include "gamestate.h"
#include "i_interface.h"
#include "c_cvars.h"
#include "i_time.h"
#include "keydef.h"
#include "input_lineage.h"

int eventhead;
int eventtail;
event_t events[MAXEVENTS];

CUSTOM_CVAR(Int, perf_looptraceframes, 0, 0)
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

CUSTOM_CVAR(Int, in_uidebouncems, 120, 0)
{
	if (self < 0)
	{
		self = 0;
	}
	else if (self > 500)
	{
		self = 500;
	}
}

static uint64_t GuiKeySuppressUntil[256];

static int NormalizeGuiDebounceKey(int key)
{
	if (key >= 'A' && key <= 'Z')
	{
		return key + ('a' - 'A');
	}
	return key;
}

static int GuiDebounceKeyFromRawEvent(const event_t* ev)
{
	if (ev->data2 != 0)
	{
		return NormalizeGuiDebounceKey(ev->data2);
	}
	if (ev->data1 == KEY_ESCAPE)
	{
		return GK_ESCAPE;
	}
	if (ev->data1 == KEY_GRAVE)
	{
		return '`';
	}
	return 0;
}

static bool IsDebouncedGuiKeyEvent(const event_t* ev)
{
	if (ev->type != EV_GUI_Event ||
		(ev->subtype != EV_GUI_KeyDown &&
		ev->subtype != EV_GUI_KeyRepeat &&
		ev->subtype != EV_GUI_Char))
	{
		return false;
	}
	const int key = NormalizeGuiDebounceKey(ev->data1);
	return key > 0 &&
		key < 256 &&
		GuiKeySuppressUntil[key] != 0 &&
		I_msTime() <= GuiKeySuppressUntil[key];
}

static void NoteRawKeyForGuiDebounce(const event_t* ev)
{
	if (ev->type != EV_KeyDown || in_uidebouncems <= 0)
	{
		return;
	}
	const int key = GuiDebounceKeyFromRawEvent(ev);
	if (key > 0 && key < 256)
	{
		GuiKeySuppressUntil[key] = I_msTime() + (uint64_t)in_uidebouncems;
	}
}

CVAR(Float, m_sensitivity_x, 2.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, m_sensitivity_y, 2.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, invertmouse, false, CVAR_GLOBALCONFIG | CVAR_ARCHIVE);  // Invert mouse look down/up?
CVAR(Bool, invertmousex, false,	CVAR_GLOBALCONFIG | CVAR_ARCHIVE);  // Invert mouse look left/right?

static PerfLoopInputTraceStats perfLoopInputTraceStats;
static PerfLoop2DProducerTraceStats perfLoop2DProducerTraceStats;
static PerfLoopCameraTraceStats perfLoopCameraTraceStats;
static PerfLoop2DTextLabel perfLoop2DCurrentTextLabel = PerfLoop2DTextLabel::Other;
static bool perfLoopHasLastRenderAngles = false;
static float perfLoopLastRenderYawDegrees = 0.0f;
static float perfLoopLastRenderPitchDegrees = 0.0f;

static PerfLoop2DTextTraceCounter& Get2DTextTraceCounter(PerfLoop2DTextLabel label)
{
	switch (label)
	{
	case PerfLoop2DTextLabel::ConsoleVersion:
		return perfLoop2DProducerTraceStats.consoleVersionText;
	case PerfLoop2DTextLabel::ConsoleBody:
		return perfLoop2DProducerTraceStats.consoleBodyText;
	case PerfLoop2DTextLabel::ConsoleCommandLine:
		return perfLoop2DProducerTraceStats.consoleCommandLineText;
	case PerfLoop2DTextLabel::Hud:
		return perfLoop2DProducerTraceStats.hudText;
	case PerfLoop2DTextLabel::Stats:
		return perfLoop2DProducerTraceStats.statsText;
	case PerfLoop2DTextLabel::Rate:
		return perfLoop2DProducerTraceStats.rateText;
	case PerfLoop2DTextLabel::Other:
	default:
		return perfLoop2DProducerTraceStats.otherText;
	}
}

static float NormalizeAngleDeltaDegrees(float degrees)
{
	while (degrees <= -180.0f) degrees += 360.0f;
	while (degrees > 180.0f) degrees -= 360.0f;
	return degrees;
}

bool PerfLoopTraceActive()
{
	return perf_looptraceframes > 0;
}

void PerfLoopTraceResetInputStats()
{
	perfLoopInputTraceStats = {};
}

PerfLoopInputTraceStats PerfLoopTraceGetInputStats()
{
	return perfLoopInputTraceStats;
}

void PerfLoopTraceReset2DProducerStats()
{
	perfLoop2DProducerTraceStats = {};
	perfLoop2DCurrentTextLabel = PerfLoop2DTextLabel::Other;
}

PerfLoop2DProducerTraceStats PerfLoopTraceGet2DProducerStats()
{
	return perfLoop2DProducerTraceStats;
}

void PerfLoopTraceResetCameraStats()
{
	perfLoopCameraTraceStats = {};
}

PerfLoopCameraTraceStats PerfLoopTraceGetCameraStats()
{
	return perfLoopCameraTraceStats;
}

void PerfLoopTraceNoteHandleevents()
{
	if (!PerfLoopTraceActive())
		return;

	perfLoopInputTraceStats.handleeventsCalls++;
}

void PerfLoopTraceNoteIStartTic()
{
	if (!PerfLoopTraceActive())
		return;

	perfLoopInputTraceStats.startTicCalls++;
}

void PerfLoopTraceNoteIGetEvent(uint32_t peekedMessages)
{
	if (!PerfLoopTraceActive())
		return;

	perfLoopInputTraceStats.iGetEventCalls++;
	perfLoopInputTraceStats.peekedMessages += peekedMessages;
	perfLoopInputTraceStats.maxMessageBurst = std::max(perfLoopInputTraceStats.maxMessageBurst, peekedMessages);
}

void PerfLoopTraceNoteRawInputMessage(bool isMouse, bool isKeyboard)
{
	if (!PerfLoopTraceActive())
		return;

	perfLoopInputTraceStats.rawInputMessages++;
	if (isMouse) perfLoopInputTraceStats.rawMousePackets++;
	if (isKeyboard) perfLoopInputTraceStats.rawKeyboardPackets++;
}

void PerfLoopTraceNoteRawMousePacket(bool accepted, int dx, int dy)
{
	if (!PerfLoopTraceActive())
		return;

	if (!accepted)
	{
		perfLoopInputTraceStats.rawMouseDroppedPackets++;
		return;
	}

	if (dx != 0 || dy != 0)
	{
		perfLoopInputTraceStats.rawMouseMovePackets++;
	}
}

void PerfLoopTraceNoteMousePost(float x, float y)
{
	if (!PerfLoopTraceActive())
		return;

	perfLoopInputTraceStats.postedMouseMoves++;
	perfLoopInputTraceStats.postedMouseX += x;
	perfLoopInputTraceStats.postedMouseY += y;
}

void PerfLoopTraceNoteMouseDispatch(float x, float y)
{
	if (!PerfLoopTraceActive())
		return;

	perfLoopInputTraceStats.dispatchedMouseMoves++;
	perfLoopInputTraceStats.dispatchedMouseX += x;
	perfLoopInputTraceStats.dispatchedMouseY += y;
}

void PerfLoopTraceNoteMouseRoute(bool yawLook, bool pitchLook, float x, float y)
{
	PerfInputLineageNoteMouseRoute(yawLook, pitchLook);
	if (!PerfLoopTraceActive())
		return;

	if (x != 0.0f)
	{
		if (yawLook) perfLoopInputTraceStats.mouseYawLookSamples++;
		else perfLoopInputTraceStats.mouseStrafeSamples++;
	}
	if (y != 0.0f)
	{
		if (pitchLook) perfLoopInputTraceStats.mousePitchLookSamples++;
		else perfLoopInputTraceStats.mouseAimMoveSamples++;
	}
}

void PerfLoopTraceNoteGameInputSample(float x, float y)
{
	if (!PerfLoopTraceActive())
		return;

	perfLoopInputTraceStats.sampledMouseInputs++;
	perfLoopInputTraceStats.sampledMouseX += x;
	perfLoopInputTraceStats.sampledMouseY += y;
}

void PerfLoopTraceNoteTiccmdBuild(float yawDegrees, float pitchDegrees)
{
	PerfInputLineageNoteTiccmdBuild(yawDegrees, pitchDegrees);
	if (!PerfLoopTraceActive())
		return;

	perfLoopInputTraceStats.ticcmdBuilds++;
	perfLoopInputTraceStats.ticcmdYawDegrees += yawDegrees;
	perfLoopInputTraceStats.ticcmdPitchDegrees += pitchDegrees;
}

void PerfLoopTraceNotePlayerYawApply(float yawDegrees)
{
	if (!PerfLoopTraceActive())
		return;

	perfLoopInputTraceStats.yawApplyCalls++;
	perfLoopInputTraceStats.appliedYawDegrees += yawDegrees;
}

void PerfLoopTraceNotePlayerPitchApply(float pitchDegrees)
{
	if (!PerfLoopTraceActive())
		return;

	perfLoopInputTraceStats.pitchApplyCalls++;
	perfLoopInputTraceStats.appliedPitchDegrees += pitchDegrees;
}

void PerfLoopTraceNoteFastCameraApply(float yawDegrees, float pitchDegrees)
{
	if (!PerfLoopTraceActive())
		return;

	perfLoopInputTraceStats.fastCameraApplyCalls++;
	perfLoopInputTraceStats.fastCameraYawDegrees += yawDegrees;
	perfLoopInputTraceStats.fastCameraPitchDegrees += pitchDegrees;
}

void PerfLoopTraceNoteInputMode(bool syncInput, double inputScale)
{
	PerfInputLineageNoteInputMode(syncInput);
	if (!PerfLoopTraceActive())
		return;

	perfLoopCameraTraceStats.syncInput = syncInput;
	perfLoopCameraTraceStats.inputScale = inputScale;
}

void PerfLoopTraceNoteCommandSync(bool syncInput)
{
	if (!PerfLoopTraceActive())
		return;

	perfLoopCameraTraceStats.cmdSyncInput = syncInput;
}

void PerfLoopTraceNoteActorYaw(float cmdYawDegrees, float deltaYawDegrees, float currentYawDegrees)
{
	if (!PerfLoopTraceActive())
		return;

	perfLoopCameraTraceStats.actorYawCalls++;
	perfLoopCameraTraceStats.consumedCmdYawDegrees += cmdYawDegrees;
	perfLoopCameraTraceStats.actorYawDeltaDegrees += deltaYawDegrees;
	perfLoopCameraTraceStats.actorYawDegrees = currentYawDegrees;
}

void PerfLoopTraceNoteActorPitch(float cmdPitchDegrees, float deltaPitchDegrees, float currentPitchDegrees)
{
	if (!PerfLoopTraceActive())
		return;

	perfLoopCameraTraceStats.actorPitchCalls++;
	perfLoopCameraTraceStats.consumedCmdPitchDegrees += cmdPitchDegrees;
	perfLoopCameraTraceStats.actorPitchDeltaDegrees += deltaPitchDegrees;
	perfLoopCameraTraceStats.actorPitchDegrees = currentPitchDegrees;
}

void PerfLoopTraceNoteCameraAngles(float deltaYawDegrees, float deltaPitchDegrees, float currentYawDegrees, float currentPitchDegrees, bool reset)
{
	if (!PerfLoopTraceActive())
		return;

	if (reset) perfLoopCameraTraceStats.cameraResetCalls++;
	else perfLoopCameraTraceStats.cameraUpdateCalls++;
	perfLoopCameraTraceStats.cameraYawDeltaDegrees += deltaYawDegrees;
	perfLoopCameraTraceStats.cameraPitchDeltaDegrees += deltaPitchDegrees;
	perfLoopCameraTraceStats.cameraYawDegrees = currentYawDegrees;
	perfLoopCameraTraceStats.cameraPitchDegrees = currentPitchDegrees;
}

void PerfLoopTraceNoteRenderAngles(float yawDegrees, float pitchDegrees)
{
	if (!PerfLoopTraceActive())
		return;

	if (perfLoopCameraTraceStats.renderCalls == 0 && perfLoopHasLastRenderAngles)
	{
		perfLoopCameraTraceStats.renderFrameDeltaYawDegrees = NormalizeAngleDeltaDegrees(yawDegrees - perfLoopLastRenderYawDegrees);
		perfLoopCameraTraceStats.renderFrameDeltaPitchDegrees = pitchDegrees - perfLoopLastRenderPitchDegrees;
	}
	perfLoopCameraTraceStats.renderCalls++;
	perfLoopCameraTraceStats.renderYawDegrees = yawDegrees;
	perfLoopCameraTraceStats.renderPitchDegrees = pitchDegrees;
	perfLoopHasLastRenderAngles = true;
	perfLoopLastRenderYawDegrees = yawDegrees;
	perfLoopLastRenderPitchDegrees = pitchDegrees;
}

void PerfLoopTraceNoteStatusBar2D(const PerfLoop2DProducerDelta& delta)
{
	if (!PerfLoopTraceActive())
		return;

	perfLoop2DProducerTraceStats.statusBar = delta;
}

void PerfLoopTraceNoteAltHud2D(const PerfLoop2DProducerDelta& delta)
{
	if (!PerfLoopTraceActive())
		return;

	perfLoop2DProducerTraceStats.altHud = delta;
}

void PerfLoopTraceNoteCrosshair2D(const PerfLoop2DProducerDelta& delta)
{
	if (!PerfLoopTraceActive())
		return;

	perfLoop2DProducerTraceStats.crosshair = delta;
}

PerfLoop2DTextLabel PerfLoopTracePush2DTextLabel(PerfLoop2DTextLabel label)
{
	const auto previous = perfLoop2DCurrentTextLabel;
	if (PerfLoopTraceActive())
	{
		perfLoop2DCurrentTextLabel = label;
	}
	return previous;
}

void PerfLoopTracePop2DTextLabel(PerfLoop2DTextLabel previous)
{
	if (!PerfLoopTraceActive())
		return;

	perfLoop2DCurrentTextLabel = previous;
}

void PerfLoopTraceNote2DTextDraw(uint32_t glyphs, uint32_t commands)
{
	if (!PerfLoopTraceActive())
		return;

	auto& counter = Get2DTextTraceCounter(perfLoop2DCurrentTextLabel);
	counter.calls++;
	counter.glyphs += glyphs;
	counter.commands += commands;
}

void PerfLoopTraceNoteConsoleVisibleLines(uint32_t visibleLines)
{
	if (!PerfLoopTraceActive())
		return;

	perfLoop2DProducerTraceStats.consoleVisibleLines += visibleLines;
}

void PerfLoopTraceNoteConsoleBackgroundCommands(uint32_t commands)
{
	if (!PerfLoopTraceActive())
		return;

	perfLoop2DProducerTraceStats.consoleBackgroundCommands += commands;
}


//==========================================================================
//
// D_ProcessEvents
//
// Send all the events of the given timestamp down the responder chain.
// Events are asynchronous inputs generally generated by the game user.
// Events can be discarded if no responder claims them
//
//==========================================================================

void D_ProcessEvents (void)
{
	FixedBitArray<NUM_KEYS> keywasdown;
	TArray<event_t*> delayedevents;

	keywasdown.Zero();
	while (eventtail != eventhead)
	{
		event_t *ev = &events[eventtail];
		eventtail = (eventtail + 1) & (MAXEVENTS - 1);

		if (ev->type == EV_KeyUp && keywasdown[ev->data1])
		{
			delayedevents.Push(ev);
			continue;
		}

		if (ev->type == EV_None)
			continue;
		if (ev->type == EV_DeviceChange)
			UpdateJoystickMenu(I_UpdateDeviceList());

		// allow the game to intercept Escape before dispatching it.
		if (ev->type != EV_KeyDown || ev->data1 != KEY_ESCAPE || !sysCallbacks.WantEscape || !sysCallbacks.WantEscape())
		{
			if (gamestate != GS_INTRO) // GS_INTRO blocks the UI.
			{
				if (C_Responder(ev))
					continue;				// console ate the event
				if (M_Responder(ev))
					continue;				// menu ate the event
			}
		}

		if (sysCallbacks.G_Responder(ev) && ev->type == EV_KeyDown) keywasdown.Set(ev->data1);
	}

	for (auto ev: delayedevents)
	{
		D_PostEvent(ev);
	}
}

//==========================================================================
//
// D_RemoveNextCharEvent
//
// Removes the next EV_GUI_Char event in the input queue. Used by the menu,
// since it (generally) consumes EV_GUI_KeyDown events and not EV_GUI_Char
// events, and it needs to ensure that there is no left over input when it's
// done. If there are multiple EV_GUI_KeyDowns before the EV_GUI_Char, then
// there are dead chars involved, so those should be removed, too. We do
// this by changing the message type to EV_None rather than by actually
// removing the event from the queue.
// 
//==========================================================================

void D_RemoveNextCharEvent()
{
	assert(events[eventtail].type == EV_GUI_Event && events[eventtail].subtype == EV_GUI_KeyDown);
	for (int evnum = eventtail; evnum != eventhead; evnum = (evnum+1) & (MAXEVENTS-1))
	{
		event_t *ev = &events[evnum];
		if (ev->type != EV_GUI_Event)
			break;
		if (ev->subtype == EV_GUI_KeyDown || ev->subtype == EV_GUI_Char)
		{
			ev->type = EV_None;
			if (ev->subtype == EV_GUI_Char)
				break;
		}
		else
		{
			break;
		}
	}
}


//==========================================================================
//
// D_PostEvent
//
// Called by the I/O functions when input is detected.
//
//==========================================================================

void D_PostEvent(event_t* ev)
{
	if (IsDebouncedGuiKeyEvent(ev))
	{
		return;
	}
	NoteRawKeyForGuiDebounce(ev);

	// Do not post duplicate consecutive EV_DeviceChange events.
	if (ev->type == EV_DeviceChange && events[eventhead].type == EV_DeviceChange)
	{
		return;
	}
	if (sysCallbacks.DispatchEvent && sysCallbacks.DispatchEvent(ev))
		return;

	if (PerfLoopTraceActive())
	{
		if (ev->type == EV_KeyDown) perfLoopInputTraceStats.keyDownEvents++;
		else if (ev->type == EV_KeyUp) perfLoopInputTraceStats.keyUpEvents++;
		else if (ev->type == EV_DeviceChange) perfLoopInputTraceStats.deviceChangeEvents++;

		int occupancy = eventhead - eventtail;
		if (occupancy < 0) occupancy += MAXEVENTS;
		perfLoopInputTraceStats.eventQueueHighWater = std::max(perfLoopInputTraceStats.eventQueueHighWater, (uint32_t)occupancy);

		const int nexthead = (eventhead + 1) & (MAXEVENTS - 1);
		if (nexthead == eventtail)
		{
			perfLoopInputTraceStats.eventQueueOverflows++;
		}
	}

	events[eventhead] = *ev;
	eventhead = (eventhead + 1) & (MAXEVENTS - 1);
}


void PostMouseMove(int xx, int yy)
{
	event_t ev{};

	ev.x = float(xx) * m_sensitivity_x;
	ev.y = -float(yy) * m_sensitivity_y;

	if (invertmousex) ev.x = -ev.x;
	if (invertmouse) ev.y = -ev.y;

	if (ev.x || ev.y)
	{
		ev.type = EV_Mouse;
		ev.inputSequence = PerfInputLineageRecordMousePost(xx, yy, ev.x, ev.y);
		PerfLoopTraceNoteMousePost(ev.x, ev.y);
		D_PostEvent(&ev);
	}
}


FInputEvent::FInputEvent(const event_t *ev)
{
	Type = (EGenericEvent)ev->type;
	// we don't want the modders to remember what weird fields mean what for what events.
	KeyScan = 0;
	KeyChar = 0;
	MouseX = 0;
	MouseY = 0;
	switch (Type)
	{
	case EV_None:
		break;
	case EV_KeyDown:
	case EV_KeyUp:
		KeyScan = ev->data1;
		KeyChar = ev->data2;
		KeyString = FString(char(ev->data1));
		break;
	case EV_Mouse:
		MouseX = int(ev->x);
		MouseY = int(ev->y);
		break;
	default:
		break; // EV_DeviceChange = wat?
	}
}

FUiEvent::FUiEvent(const event_t *ev)
{
	Type = (EGUIEvent)ev->subtype;
	KeyChar = 0;
	IsShift = false;
	IsAlt = false;
	IsCtrl = false;
	MouseX = 0;
	MouseY = 0;
	// we don't want the modders to remember what weird fields mean what for what events.
	switch (ev->subtype)
	{
	case EV_GUI_None:
		break;
	case EV_GUI_KeyDown:
	case EV_GUI_KeyRepeat:
	case EV_GUI_KeyUp:
		KeyChar = ev->data1;
		KeyString = FString(char(ev->data1));
		IsShift = !!(ev->data3 & GKM_SHIFT);
		IsAlt = !!(ev->data3 & GKM_ALT);
		IsCtrl = !!(ev->data3 & GKM_CTRL);
		break;
	case EV_GUI_Char:
		KeyChar = ev->data1;
		KeyString = MakeUTF8(ev->data1);
		IsAlt = !!ev->data2; // only true for Win32, not sure about SDL
		break;
	default: // mouse event
			 // note: SDL input doesn't seem to provide these at all
			 //Printf("Mouse data: %d, %d, %d, %d\n", ev->x, ev->y, ev->data1, ev->data2);
		MouseX = ev->data1;
		MouseY = ev->data2;
		IsShift = !!(ev->data3 & GKM_SHIFT);
		IsAlt = !!(ev->data3 & GKM_ALT);
		IsCtrl = !!(ev->data3 & GKM_CTRL);
		break;
	}
}

DEFINE_FIELD_X(UiEvent, FUiEvent, Type);
DEFINE_FIELD_X(UiEvent, FUiEvent, KeyString);
DEFINE_FIELD_X(UiEvent, FUiEvent, KeyChar);
DEFINE_FIELD_X(UiEvent, FUiEvent, MouseX);
DEFINE_FIELD_X(UiEvent, FUiEvent, MouseY);
DEFINE_FIELD_X(UiEvent, FUiEvent, IsShift);
DEFINE_FIELD_X(UiEvent, FUiEvent, IsAlt);
DEFINE_FIELD_X(UiEvent, FUiEvent, IsCtrl);

DEFINE_FIELD_X(InputEvent, FInputEvent, Type);
DEFINE_FIELD_X(InputEvent, FInputEvent, KeyScan);
DEFINE_FIELD_X(InputEvent, FInputEvent, KeyString);
DEFINE_FIELD_X(InputEvent, FInputEvent, KeyChar);
DEFINE_FIELD_X(InputEvent, FInputEvent, MouseX);
DEFINE_FIELD_X(InputEvent, FInputEvent, MouseY);
