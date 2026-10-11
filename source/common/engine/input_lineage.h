#pragma once

#include <cstdint>

// Bounded input-to-present diagnostics. All timestamps are from one unscaled
// steady clock and are expressed in microseconds. The capture is armed by
// perf_inputlineageframes and starts at the next presentation boundary.

struct PerfInputLineageViewSnapshot
{
	uint64_t window = 0;
	uint64_t presentationGeneration = 0;
	uint64_t producedThrough = 0;
	uint64_t commandConsumedThrough = 0;
	uint64_t commandBuiltThrough = 0;
	uint64_t renderConsumedThrough = 0;
	uint64_t renderCursorThrough = 0;
	uint64_t captureUs = 0;
	float yawDegrees = 0.0f;
	float pitchDegrees = 0.0f;
	bool syncInput = false;
	bool valid = false;
};

uint64_t PerfInputLineageNowUs();
bool PerfInputLineageActive();
void PerfInputLineageBeginPresentation(uint64_t presentationGeneration);
void PerfInputLineageEndPresentation();
void PerfInputLineageAbort(const char* reason);

// Installed only while DispatchMessage handles a Win32 MSG. messageTimeMs is
// MSG.time; queueResidenceMs is an unsigned GetTickCount()-MSG.time estimate.
void PerfInputLineageSetWindowsMessageTiming(
	bool valid,
	uint32_t messageTimeMs,
	uint32_t queueResidenceMs,
	uint64_t dispatchUs);
void PerfInputLineageClearWindowsMessageTiming();

uint64_t PerfInputLineageRecordMousePost(int rawX, int rawY, float postX, float postY);
void PerfInputLineageNoteMouseDispatch(uint64_t sequence);
void PerfInputLineageNoteMouseExcluded(uint64_t sequence);
void PerfInputLineageNoteMouseRoute(bool yawLook, bool pitchLook);
void PerfInputLineageNoteCommandSample(bool processedByMovement);
void PerfInputLineageDiscardPendingMouse();
void PerfInputLineageNoteTiccmdBuild(float yawDegrees, float pitchDegrees);
void PerfInputLineageNoteRenderMouseSample(
	bool yawLook,
	bool pitchLook,
	float yawDegrees,
	float pitchDegrees,
	bool lateLatch);
void PerfInputLineageNoteLatePump(uint64_t durationUs, bool routingInvalidated, bool latchApplied);
void PerfInputLineageNoteInputMode(bool syncInput);
void PerfInputLineageNoteViewCapture(float yawDegrees, float pitchDegrees);
PerfInputLineageViewSnapshot PerfInputLineageGetLatestViewSnapshot();

void PerfInputLineageNoteNriSubmit(
	uint64_t presentationGeneration,
	const PerfInputLineageViewSnapshot& view,
	uint64_t nriFrame,
	uint32_t queuedFrameIndex,
	uint32_t swapChainImageIndex,
	uint64_t attemptedFence,
	bool completedFenceValid,
	uint64_t completedFence,
	uint32_t outstandingBefore,
	uint32_t outstandingAfter,
	uint64_t submitStartUs,
	uint64_t submitEndUs,
	int32_t submitResult,
	bool submitAccepted);
void PerfInputLineageNoteNriPresent(
	uint64_t presentationGeneration,
	uint64_t nriFrame,
	uint64_t presentStartUs,
	uint64_t presentEndUs,
	int32_t presentResult,
	bool presentAccepted,
	bool frameGenerationPath);
