#pragma once
#include <stdint.h>
#include "d_gui.h"
#include "zstring.h"

// Input event types.
enum EGenericEvent
{
	EV_None,
	EV_KeyDown,		// data1: scan code, data2: Qwerty ASCII code
	EV_KeyUp,		// same
	EV_Mouse,		// x, y: mouse movement deltas
	EV_GUI_Event,	// subtype specifies actual event
	EV_DeviceChange,// a device has been connected or removed
};

// Event structure.
struct event_t
{
	uint8_t		type;
	uint8_t		subtype;
	int16_t 	data1;		// keys / mouse/joystick buttons
	int16_t		data2;
	int16_t		data3;
	float 		x;			// mouse/joystick x move
	float 		y;			// mouse/joystick y move
	uint64_t	inputSequence = 0;	// trace-only mouse lineage; zero when capture is inactive
};



// Called by IO functions when input is detected.
void D_PostEvent (event_t* ev);
void D_RemoveNextCharEvent();
void D_ProcessEvents(void);
void PostMouseMove(int x, int y);

enum
{
	MAXEVENTS = 128
};

extern	event_t 		events[MAXEVENTS];
extern int eventhead;
extern int eventtail;

struct PerfLoopInputTraceStats
{
	uint32_t iGetEventCalls = 0;
	uint32_t startTicCalls = 0;
	uint32_t handleeventsCalls = 0;
	uint32_t peekedMessages = 0;
	uint32_t maxMessageBurst = 0;
	uint32_t rawInputMessages = 0;
	uint32_t rawKeyboardPackets = 0;
	uint32_t rawMousePackets = 0;
	uint32_t rawMouseMovePackets = 0;
	uint32_t rawMouseDroppedPackets = 0;
	uint32_t postedMouseMoves = 0;
	uint32_t dispatchedMouseMoves = 0;
	uint32_t sampledMouseInputs = 0;
	uint32_t keyDownEvents = 0;
	uint32_t keyUpEvents = 0;
	uint32_t deviceChangeEvents = 0;
	uint32_t eventQueueHighWater = 0;
	uint32_t eventQueueOverflows = 0;
	uint32_t mouseYawLookSamples = 0;
	uint32_t mouseStrafeSamples = 0;
	uint32_t mousePitchLookSamples = 0;
	uint32_t mouseAimMoveSamples = 0;
	uint32_t ticcmdBuilds = 0;
	uint32_t yawApplyCalls = 0;
	uint32_t pitchApplyCalls = 0;
	uint32_t fastCameraApplyCalls = 0;
	float postedMouseX = 0.0f;
	float postedMouseY = 0.0f;
	float dispatchedMouseX = 0.0f;
	float dispatchedMouseY = 0.0f;
	float sampledMouseX = 0.0f;
	float sampledMouseY = 0.0f;
	float ticcmdYawDegrees = 0.0f;
	float ticcmdPitchDegrees = 0.0f;
	float appliedYawDegrees = 0.0f;
	float appliedPitchDegrees = 0.0f;
	float fastCameraYawDegrees = 0.0f;
	float fastCameraPitchDegrees = 0.0f;
};

struct PerfLoop2DProducerDelta
{
	int commands = 0;
	int vertices = 0;
	int indices = 0;
	double ms = 0.0;
};

enum class PerfLoop2DTextLabel : uint8_t
{
	Other = 0,
	ConsoleVersion,
	ConsoleBody,
	ConsoleCommandLine,
	Hud,
	Stats,
	Rate,
};

struct PerfLoop2DTextTraceCounter
{
	uint32_t calls = 0;
	uint32_t glyphs = 0;
	uint32_t commands = 0;
};

struct PerfLoop2DProducerTraceStats
{
	PerfLoop2DProducerDelta statusBar;
	PerfLoop2DProducerDelta altHud;
	PerfLoop2DProducerDelta crosshair;
	uint32_t consoleVisibleLines = 0;
	uint32_t consoleBackgroundCommands = 0;
	PerfLoop2DTextTraceCounter consoleVersionText;
	PerfLoop2DTextTraceCounter consoleBodyText;
	PerfLoop2DTextTraceCounter consoleCommandLineText;
	PerfLoop2DTextTraceCounter hudText;
	PerfLoop2DTextTraceCounter statsText;
	PerfLoop2DTextTraceCounter rateText;
	PerfLoop2DTextTraceCounter otherText;
};

struct PerfLoopCameraTraceStats
{
	bool syncInput = false;
	bool cmdSyncInput = false;
	double inputScale = 1.0;
	uint32_t actorYawCalls = 0;
	uint32_t actorPitchCalls = 0;
	uint32_t cameraUpdateCalls = 0;
	uint32_t cameraResetCalls = 0;
	uint32_t renderCalls = 0;
	float consumedCmdYawDegrees = 0.0f;
	float consumedCmdPitchDegrees = 0.0f;
	float actorYawDeltaDegrees = 0.0f;
	float actorPitchDeltaDegrees = 0.0f;
	float actorYawDegrees = 0.0f;
	float actorPitchDegrees = 0.0f;
	float cameraYawDeltaDegrees = 0.0f;
	float cameraPitchDeltaDegrees = 0.0f;
	float cameraYawDegrees = 0.0f;
	float cameraPitchDegrees = 0.0f;
	float renderYawDegrees = 0.0f;
	float renderPitchDegrees = 0.0f;
	float renderFrameDeltaYawDegrees = 0.0f;
	float renderFrameDeltaPitchDegrees = 0.0f;
};

bool PerfLoopTraceActive();
void PerfLoopTraceResetInputStats();
PerfLoopInputTraceStats PerfLoopTraceGetInputStats();
void PerfLoopTraceReset2DProducerStats();
PerfLoop2DProducerTraceStats PerfLoopTraceGet2DProducerStats();
void PerfLoopTraceResetCameraStats();
PerfLoopCameraTraceStats PerfLoopTraceGetCameraStats();
void PerfLoopTraceNoteHandleevents();
void PerfLoopTraceNoteIStartTic();
void PerfLoopTraceNoteIGetEvent(uint32_t peekedMessages);
void PerfLoopTraceNoteRawInputMessage(bool isMouse, bool isKeyboard);
void PerfLoopTraceNoteRawMousePacket(bool accepted, int dx, int dy);
void PerfLoopTraceNoteMousePost(float x, float y);
void PerfLoopTraceNoteMouseDispatch(float x, float y);
void PerfLoopTraceNoteMouseRoute(bool yawLook, bool pitchLook, float x, float y);
void PerfLoopTraceNoteGameInputSample(float x, float y);
void PerfLoopTraceNoteTiccmdBuild(float yawDegrees, float pitchDegrees);
void PerfLoopTraceNotePlayerYawApply(float yawDegrees);
void PerfLoopTraceNotePlayerPitchApply(float pitchDegrees);
void PerfLoopTraceNoteFastCameraApply(float yawDegrees, float pitchDegrees);
void PerfLoopTraceNoteInputMode(bool syncInput, double inputScale);
void PerfLoopTraceNoteCommandSync(bool syncInput);
void PerfLoopTraceNoteActorYaw(float cmdYawDegrees, float deltaYawDegrees, float currentYawDegrees);
void PerfLoopTraceNoteActorPitch(float cmdPitchDegrees, float deltaPitchDegrees, float currentPitchDegrees);
void PerfLoopTraceNoteCameraAngles(float deltaYawDegrees, float deltaPitchDegrees, float currentYawDegrees, float currentPitchDegrees, bool reset);
void PerfLoopTraceNoteRenderAngles(float yawDegrees, float pitchDegrees);
void PerfLoopTraceNoteStatusBar2D(const PerfLoop2DProducerDelta& delta);
void PerfLoopTraceNoteAltHud2D(const PerfLoop2DProducerDelta& delta);
void PerfLoopTraceNoteCrosshair2D(const PerfLoop2DProducerDelta& delta);
PerfLoop2DTextLabel PerfLoopTracePush2DTextLabel(PerfLoop2DTextLabel label);
void PerfLoopTracePop2DTextLabel(PerfLoop2DTextLabel previous);
void PerfLoopTraceNote2DTextDraw(uint32_t glyphs, uint32_t commands);
void PerfLoopTraceNoteConsoleVisibleLines(uint32_t visibleLines);
void PerfLoopTraceNoteConsoleBackgroundCommands(uint32_t commands);

class PerfLoop2DTextScope
{
public:
	explicit PerfLoop2DTextScope(PerfLoop2DTextLabel label)
		: mPrevious(PerfLoopTracePush2DTextLabel(label))
	{
	}

	~PerfLoop2DTextScope()
	{
		PerfLoopTracePop2DTextLabel(mPrevious);
	}

private:
	PerfLoop2DTextLabel mPrevious;
};

struct FUiEvent
{
	// this essentially translates event_t UI events to ZScript.
	EGUIEvent Type;
	// for keys/chars/whatever
	FString KeyString;
	int KeyChar;
	// for mouse
	int MouseX;
	int MouseY;
	// global (?)
	bool IsShift;
	bool IsCtrl;
	bool IsAlt;

	FUiEvent(const event_t *ev);
};

struct FInputEvent
{
	// this translates regular event_t events to ZScript (not UI, UI events are sent via DUiEvent and only if requested!)
	EGenericEvent Type = EV_None;
	// for keys
	int KeyScan;
	FString KeyString;
	int KeyChar;
	// for mouse
	int MouseX;
	int MouseY;

	FInputEvent(const event_t *ev);
};
