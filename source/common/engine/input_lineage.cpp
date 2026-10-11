#include "input_lineage.h"

#include "c_cvars.h"
#include "printf.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <type_traits>

CUSTOM_CVAR(Int, perf_inputlineageframes, 0, 0)
{
	if (self < 0) self = 0;
	else if (self > 512) self = 512;
}

namespace
{
	constexpr uint32_t MaxMouseRecords = 16384;
	constexpr uint32_t MaxRequestedFrames = 512;
	constexpr uint32_t DrainFrameCount = 4;
	constexpr uint32_t MaxFrameRecords = MaxRequestedFrames + DrainFrameCount;
	constexpr uint8_t RouteYaw = 1u << 0;
	constexpr uint8_t RoutePitch = 1u << 1;

	struct WindowsMessageTiming
	{
		uint32_t messageTimeMs = 0;
		uint32_t queueResidenceMs = 0;
		uint64_t dispatchUs = 0;
		bool valid = false;
	};

	struct MouseRecord
	{
		uint64_t sequence = 0;
		uint64_t postUs = 0;
		uint64_t dispatchUs = 0;
		uint64_t commandUs = 0;
		uint64_t buildUs = 0;
		uint64_t cameraUs = 0;
		uint64_t viewUs = 0;
		uint64_t postPresentation = 0;
		uint64_t commandPresentation = 0;
		uint64_t buildPresentation = 0;
		uint64_t cameraPresentation = 0;
		uint64_t viewPresentation = 0;
		uint32_t messageTimeMs = 0;
		uint32_t queueResidenceMs = 0;
		int rawX = 0;
		int rawY = 0;
		float postX = 0.0f;
		float postY = 0.0f;
		uint8_t commandRoute = 0;
		uint8_t renderRoute = 0;
		bool windowsTimingValid = false;
		bool syncInput = false;
		bool gameplayDispatched = false;
		bool excluded = false;
		bool discarded = false;
		bool renderConsidered = false;
		bool cameraLateLatched = false;
	};

	struct FrameRecord
	{
		uint64_t presentation = 0;
		uint64_t firstPosted = 0;
		uint64_t lastPosted = 0;
		uint64_t producedThrough = 0;
		uint64_t commandThrough = 0;
		uint64_t buildThrough = 0;
		uint64_t renderThrough = 0;
		uint64_t renderCursorThrough = 0;
		uint32_t posted = 0;
		uint32_t dispatched = 0;
		uint32_t commandConsumed = 0;
		uint32_t commandBuilt = 0;
		uint32_t renderConsumed = 0;
		uint32_t renderEarlyConsumed = 0;
		uint32_t renderLateConsumed = 0;
		float postedX = 0.0f;
		float postedY = 0.0f;
		float commandX = 0.0f;
		float commandY = 0.0f;
		float renderX = 0.0f;
		float renderY = 0.0f;
		float ticcmdYawDegrees = 0.0f;
		float ticcmdPitchDegrees = 0.0f;
		float cameraYawDegrees = 0.0f;
		float cameraPitchDegrees = 0.0f;
		float lateCameraYawDegrees = 0.0f;
		float lateCameraPitchDegrees = 0.0f;
		uint64_t latePumpUs = 0;
		uint32_t latePumpCalls = 0;
		uint32_t latePumpInvalidations = 0;
		uint32_t lateLatchApplies = 0;
		uint64_t maxQueueUs = 0;
		uint64_t maxDispatchToCommandUs = 0;
		uint64_t maxDispatchToCameraUs = 0;
		uint64_t maxCameraToViewUs = 0;
		PerfInputLineageViewSnapshot view;

		uint64_t nriFrame = 0;
		uint64_t submitPresentation = 0;
		uint64_t attemptedFence = 0;
		uint64_t completedFence = 0;
		uint64_t submitStartUs = 0;
		uint64_t submitEndUs = 0;
		uint64_t presentStartUs = 0;
		uint64_t presentEndUs = 0;
		uint32_t queuedFrameIndex = 0;
		uint32_t swapChainImageIndex = 0;
		uint32_t outstandingBefore = 0;
		uint32_t outstandingAfter = 0;
		int32_t submitResult = 0;
		int32_t presentResult = 0;
		bool completedFenceValid = false;
		bool submitSeen = false;
		bool submitAccepted = false;
		bool submitPresentationMatch = false;
		bool submitViewMatch = false;
		bool presentSeen = false;
		bool presentAccepted = false;
		bool presentJoinMatch = false;
		bool frameGenerationPath = false;
	};

	struct Capture
	{
		std::array<MouseRecord, MaxMouseRecords> mouse = {};
		std::array<FrameRecord, MaxFrameRecords> frames = {};
		std::array<uint64_t, MaxMouseRecords> percentileScratch = {};
		uint64_t window = 0;
		uint64_t nextSequence = 1;
		uint64_t currentPresentation = 0;
		uint64_t producedThrough = 0;
		uint64_t dispatchedThrough = 0;
		uint64_t commandThrough = 0;
		uint64_t buildThrough = 0;
		uint64_t renderThrough = 0;
		uint64_t renderCursorThrough = 0;
		uint64_t viewThrough = 0;
		uint32_t requestedFrames = 0;
		uint32_t observedFrames = 0;
		uint32_t drainFramesRemaining = 0;
		uint32_t mouseHighWater = 0;
		uint32_t overwritten = 0;
		uint32_t missingStageRecords = 0;
		uint32_t duplicateStages = 0;
		uint32_t excludedEvents = 0;
		uint32_t discardedEvents = 0;
		uint64_t totalPosted = 0;
		uint64_t totalDispatched = 0;
		uint64_t totalCommand = 0;
		uint64_t totalRender = 0;
		double totalPostedX = 0.0;
		double totalPostedY = 0.0;
		double totalCommandX = 0.0;
		double totalCommandY = 0.0;
		double totalRenderX = 0.0;
		double totalRenderY = 0.0;
		PerfInputLineageViewSnapshot latestView;
		const char* abortReason = "none";
		bool active = false;
		bool admittingMouse = false;
		bool syncInput = false;
	};

	Capture gCapture;
	thread_local WindowsMessageTiming gWindowsMessageTiming;
	uint64_t gNextWindow = 1;
	static_assert(std::is_trivially_copyable_v<Capture>);

	FrameRecord* CurrentFrame()
	{
		if (!gCapture.active || gCapture.observedFrames >= MaxFrameRecords)
		{
			return nullptr;
		}
		return &gCapture.frames[gCapture.observedFrames];
	}

	MouseRecord* FindMouse(uint64_t sequence)
	{
		if (sequence == 0) return nullptr;
		MouseRecord& record = gCapture.mouse[(sequence - 1) % MaxMouseRecords];
		return record.sequence == sequence ? &record : nullptr;
	}

	template<typename Function>
	void ForSequenceRange(uint64_t first, uint64_t last, Function function)
	{
		if (first == 0 || last < first) return;
		for (uint64_t sequence = first; sequence <= last; ++sequence)
		{
			MouseRecord* record = FindMouse(sequence);
			if (record != nullptr) function(*record);
			else gCapture.missingStageRecords++;
			if (sequence == UINT64_MAX) break;
		}
	}

	uint64_t Percentile(uint32_t count, uint32_t numerator, uint32_t denominator)
	{
		if (count == 0) return 0;
		std::sort(gCapture.percentileScratch.begin(), gCapture.percentileScratch.begin() + count);
		const uint32_t index = std::min(count - 1, (count * numerator + denominator - 1) / denominator - 1);
		return gCapture.percentileScratch[index];
	}

	template<typename ValueFunction, typename Predicate>
	void PrintMousePercentiles(const char* bucket, ValueFunction value, Predicate predicate)
	{
		uint32_t count = 0;
		uint64_t maximum = 0;
		for (const MouseRecord& record : gCapture.mouse)
		{
			if (record.sequence == 0 || !predicate(record)) continue;
			const uint64_t sample = value(record);
			gCapture.percentileScratch[count++] = sample;
			maximum = std::max(maximum, sample);
		}
		const uint64_t p50 = Percentile(count, 50, 100);
		const uint64_t p95 = Percentile(count, 95, 100);
		Printf("PERF input lineage bucket: schema=2 window=%llu bucket=%s samples=%u p50_us=%llu p95_us=%llu max_us=%llu\n",
			(unsigned long long)gCapture.window,
			bucket,
			count,
			(unsigned long long)p50,
			(unsigned long long)p95,
			(unsigned long long)maximum);
	}

	void PrintCapture()
	{
		Printf("PERF input lineage start: schema=2 window=%llu requested_frames=%u capacity=%u first_presentation=%llu clock=steady_us windows_queue=MSG.time_estimate render_view=duke_render_drawrooms late_latch=provisional actual_display_timestamp=unavailable\n",
			(unsigned long long)gCapture.window,
			gCapture.requestedFrames,
			MaxMouseRecords,
			(unsigned long long)(gCapture.observedFrames != 0 ? gCapture.frames[0].presentation : gCapture.currentPresentation));
		for (uint32_t index = 0; index < gCapture.observedFrames; ++index)
		{
			const FrameRecord& frame = gCapture.frames[index];
			const uint64_t viewToSubmitUs = frame.view.valid && frame.submitSeen && frame.submitStartUs >= frame.view.captureUs
				? frame.submitStartUs - frame.view.captureUs : 0;
			const uint64_t submitToPresentUs = frame.submitSeen && frame.presentSeen && frame.presentEndUs >= frame.submitStartUs
				? frame.presentEndUs - frame.submitStartUs : 0;
			Printf(
				"PERF input lineage frame: schema=2 window=%llu ordinal=%u presentation_gen=%llu first_seq=%llu last_seq=%llu produced_through=%llu command_through=%llu build_through=%llu render_through=%llu render_cursor_through=%llu posted=%u dispatched=%u command=%u built=%u render=%u render_early=%u render_late=%u posted_delta=(%.3f,%.3f) sampled_post_delta=(%.3f,%.3f) render_eligible_post_delta=(%.3f,%.3f) ticcmd_deg=(%.4f,%.4f) mouse_camera_apply_deg=(%.4f,%.4f) late_mouse_camera_apply_deg=(%.4f,%.4f) late_pump_calls=%u late_pump_us=%llu late_pump_invalidations=%u late_latch_calls=%u max_queue_us=%llu max_dispatch_command_us=%llu max_dispatch_camera_us=%llu max_camera_render_view_us=%llu render_view_valid=%d render_view_seq=%llu render_view_cursor=%llu render_view_us=%llu render_view_angles=(%.4f,%.4f) sync=%d nri_submit=%d nri_frame=%llu queued_frame=%u image=%u attempted_fence=%llu fence_snapshot_valid=%d completed_fence_pre_submit=%llu depth_inference_valid=%d depth_before_submit=%u depth_after_submit_upper_bound=%u submit_ok=%d submit_result=%d submit_call_us=%llu submit_presentation_match=%d submit_view_match=%d render_view_submit_us=%llu present_seen=%d present_ok=%d causal_present_ok=%d present_result=%d present_call_us=%llu present_join_match=%d submit_present_us=%llu framegen=%d actual_display_timestamp=unavailable\n",
				(unsigned long long)gCapture.window,
				index,
				(unsigned long long)frame.presentation,
				(unsigned long long)frame.firstPosted,
				(unsigned long long)frame.lastPosted,
				(unsigned long long)frame.producedThrough,
				(unsigned long long)frame.commandThrough,
				(unsigned long long)frame.buildThrough,
				(unsigned long long)frame.renderThrough,
				(unsigned long long)frame.renderCursorThrough,
				frame.posted, frame.dispatched, frame.commandConsumed, frame.commandBuilt, frame.renderConsumed,
				frame.renderEarlyConsumed, frame.renderLateConsumed,
				frame.postedX, frame.postedY, frame.commandX, frame.commandY, frame.renderX, frame.renderY,
				frame.ticcmdYawDegrees, frame.ticcmdPitchDegrees,
				frame.cameraYawDegrees, frame.cameraPitchDegrees,
				frame.lateCameraYawDegrees, frame.lateCameraPitchDegrees,
				frame.latePumpCalls,
				(unsigned long long)frame.latePumpUs,
				frame.latePumpInvalidations,
				frame.lateLatchApplies,
				(unsigned long long)frame.maxQueueUs,
				(unsigned long long)frame.maxDispatchToCommandUs,
				(unsigned long long)frame.maxDispatchToCameraUs,
				(unsigned long long)frame.maxCameraToViewUs,
				frame.view.valid ? 1 : 0,
				(unsigned long long)frame.view.renderConsumedThrough,
				(unsigned long long)frame.view.renderCursorThrough,
				(unsigned long long)frame.view.captureUs,
				frame.view.yawDegrees, frame.view.pitchDegrees,
				frame.view.syncInput ? 1 : 0,
				frame.submitSeen ? 1 : 0,
				(unsigned long long)frame.nriFrame,
				frame.queuedFrameIndex,
				frame.swapChainImageIndex,
				(unsigned long long)frame.attemptedFence,
				frame.completedFenceValid ? 1 : 0,
				(unsigned long long)frame.completedFence,
				frame.completedFenceValid ? 1 : 0,
				frame.outstandingBefore,
				frame.outstandingAfter,
				frame.submitAccepted ? 1 : 0,
				frame.submitResult,
				(unsigned long long)(frame.submitEndUs >= frame.submitStartUs ? frame.submitEndUs - frame.submitStartUs : 0),
				frame.submitPresentationMatch ? 1 : 0,
				frame.submitViewMatch ? 1 : 0,
				(unsigned long long)viewToSubmitUs,
				frame.presentSeen ? 1 : 0,
				frame.presentAccepted ? 1 : 0,
				frame.submitAccepted && frame.presentAccepted && frame.presentJoinMatch ? 1 : 0,
				frame.presentResult,
				(unsigned long long)(frame.presentEndUs >= frame.presentStartUs ? frame.presentEndUs - frame.presentStartUs : 0),
				frame.presentJoinMatch ? 1 : 0,
				(unsigned long long)submitToPresentUs,
				frame.frameGenerationPath ? 1 : 0);
		}

		const uint64_t firstRetainedSequence = gCapture.nextSequence > MaxMouseRecords
			? gCapture.nextSequence - MaxMouseRecords : 1;
		for (uint64_t sequence = firstRetainedSequence; sequence < gCapture.nextSequence; ++sequence)
		{
			const MouseRecord* record = FindMouse(sequence);
			if (record == nullptr) continue;
			Printf(
				"PERF input lineage event: schema=2 window=%llu seq=%llu raw=(%d,%d) post=(%.4f,%.4f) msg_time_ms32=%u windows_queue_us_est=%llu post_us=%llu dispatch_us=%llu command_us=%llu build_us=%llu camera_us=%llu render_view_us=%llu post_presentation=%llu command_presentation=%llu build_presentation=%llu camera_presentation=%llu render_view_presentation=%llu gameplay_dispatch=%d excluded=%d discarded=%d command_route_yaw=%d command_route_pitch=%d render_route_yaw=%d render_route_pitch=%d render_considered=%d camera_phase=%s sync=%d command_consumed=%d command_built=%d camera_applied=%d render_viewed=%d\n",
				(unsigned long long)gCapture.window,
				(unsigned long long)record->sequence,
				record->rawX, record->rawY, record->postX, record->postY,
				record->messageTimeMs,
				(unsigned long long)(record->windowsTimingValid ? uint64_t(record->queueResidenceMs) * 1000 : 0),
				(unsigned long long)record->postUs,
				(unsigned long long)record->dispatchUs,
				(unsigned long long)record->commandUs,
				(unsigned long long)record->buildUs,
				(unsigned long long)record->cameraUs,
				(unsigned long long)record->viewUs,
				(unsigned long long)record->postPresentation,
				(unsigned long long)record->commandPresentation,
				(unsigned long long)record->buildPresentation,
				(unsigned long long)record->cameraPresentation,
				(unsigned long long)record->viewPresentation,
				record->gameplayDispatched ? 1 : 0,
				record->excluded ? 1 : 0,
				record->discarded ? 1 : 0,
				(record->commandRoute & RouteYaw) != 0 ? 1 : 0,
				(record->commandRoute & RoutePitch) != 0 ? 1 : 0,
				(record->renderRoute & RouteYaw) != 0 ? 1 : 0,
				(record->renderRoute & RoutePitch) != 0 ? 1 : 0,
				record->renderConsidered ? 1 : 0,
				record->cameraUs == 0 ? "none" : (record->cameraLateLatched ? "late" : "early"),
				record->syncInput ? 1 : 0,
				record->commandUs != 0 ? 1 : 0,
				record->buildUs != 0 ? 1 : 0,
				record->cameraUs != 0 ? 1 : 0,
				record->viewUs != 0 ? 1 : 0);
		}

		PrintMousePercentiles("windows_queue_estimate",
			[](const MouseRecord& record) { return uint64_t(record.queueResidenceMs) * 1000; },
			[](const MouseRecord& record) { return record.windowsTimingValid; });
		PrintMousePercentiles("dispatch_to_command",
			[](const MouseRecord& record) { return record.commandUs - record.dispatchUs; },
			[](const MouseRecord& record) { return record.dispatchUs != 0 && record.commandUs >= record.dispatchUs; });
		PrintMousePercentiles("dispatch_to_camera",
			[](const MouseRecord& record) { return record.cameraUs - record.dispatchUs; },
			[](const MouseRecord& record) { return record.dispatchUs != 0 && record.cameraUs >= record.dispatchUs; });
		PrintMousePercentiles("camera_to_view",
			[](const MouseRecord& record) { return record.viewUs - record.cameraUs; },
			[](const MouseRecord& record) { return record.cameraUs != 0 && record.viewUs >= record.cameraUs; });

		uint32_t unresolvedDispatch = 0;
		uint32_t unresolvedCommand = 0;
		uint32_t unresolvedBuild = 0;
		uint32_t unresolvedCamera = 0;
		uint32_t unresolvedSyncCamera = 0;
		uint32_t unresolvedRenderDecision = 0;
		uint32_t unresolvedView = 0;
		uint32_t frameJoinMismatches = 0;
		uint32_t missingSubmits = 0;
		uint32_t missingPresents = 0;
		for (const MouseRecord& record : gCapture.mouse)
		{
			if (record.sequence == 0) continue;
			if (!record.gameplayDispatched && !record.excluded) unresolvedDispatch++;
			if (record.gameplayDispatched && record.commandUs == 0 && !record.discarded) unresolvedCommand++;
			if (record.commandUs != 0 && record.buildUs == 0) unresolvedBuild++;
			if (record.gameplayDispatched && !record.excluded && !record.discarded && !record.renderConsidered)
				unresolvedRenderDecision++;
			const bool eligible = ((record.renderRoute & RouteYaw) != 0 && record.postX != 0.0f) ||
				((record.renderRoute & RoutePitch) != 0 && record.postY != 0.0f);
			if (record.renderConsidered && eligible && record.cameraUs == 0)
			{
				if (record.syncInput) unresolvedSyncCamera++;
				else unresolvedCamera++;
			}
			if (record.cameraUs != 0 && record.viewUs == 0) unresolvedView++;
		}
		for (uint32_t index = 0; index < gCapture.observedFrames; ++index)
		{
			const FrameRecord& frame = gCapture.frames[index];
			if (!frame.submitSeen) missingSubmits++;
			if (!frame.presentSeen) missingPresents++;
			if (frame.submitSeen && (!frame.submitPresentationMatch || !frame.submitViewMatch)) frameJoinMismatches++;
			if (frame.presentSeen && !frame.presentJoinMatch) frameJoinMismatches++;
		}
		const bool incomplete = gCapture.overwritten != 0 || gCapture.missingStageRecords != 0 ||
			gCapture.duplicateStages != 0 || unresolvedDispatch != 0 || unresolvedCommand != 0 ||
			unresolvedBuild != 0 || unresolvedCamera != 0 || unresolvedRenderDecision != 0 ||
			unresolvedView != 0 || frameJoinMismatches != 0 ||
			missingSubmits != 0 || missingPresents != 0 ||
			(gCapture.abortReason != nullptr && gCapture.abortReason[0] != 'n');
		Printf(
			"PERF input lineage complete: schema=2 window=%llu status=%s reason=%s requested_frames=%u observed_frames=%u capacity=%u mouse_high_water=%u overwritten=%u missing_stage=%u duplicate_stage=%u first_seq=%llu produced_through=%llu dispatched_through=%llu command_through=%llu build_through=%llu render_through=%llu render_cursor_through=%llu excluded=%u discarded=%u unresolved_dispatch=%u unresolved_command=%u unresolved_build=%u unresolved_render_decision=%u unresolved_camera=%u unresolved_sync_camera=%u unresolved_render_view=%u missing_submit=%u missing_present=%u frame_join_mismatch=%u posted=%llu dispatched=%llu command=%llu render=%llu posted_delta=(%.6f,%.6f) command_delta=(%.6f,%.6f) render_eligible_delta=(%.6f,%.6f) actual_display_timestamp=unavailable\n",
			(unsigned long long)gCapture.window,
			incomplete ? "incomplete" : "complete",
			gCapture.abortReason,
			gCapture.requestedFrames,
			gCapture.observedFrames,
			MaxMouseRecords,
			gCapture.mouseHighWater,
			gCapture.overwritten,
			gCapture.missingStageRecords,
			gCapture.duplicateStages,
			(unsigned long long)(gCapture.nextSequence > 1 ? firstRetainedSequence : 0),
			(unsigned long long)gCapture.producedThrough,
			(unsigned long long)gCapture.dispatchedThrough,
			(unsigned long long)gCapture.commandThrough,
			(unsigned long long)gCapture.buildThrough,
			(unsigned long long)gCapture.renderThrough,
			(unsigned long long)gCapture.renderCursorThrough,
			gCapture.excludedEvents,
			gCapture.discardedEvents,
			unresolvedDispatch,
			unresolvedCommand,
			unresolvedBuild,
			unresolvedRenderDecision,
			unresolvedCamera,
			unresolvedSyncCamera,
			unresolvedView,
			missingSubmits,
			missingPresents,
			frameJoinMismatches,
			(unsigned long long)gCapture.totalPosted,
			(unsigned long long)gCapture.totalDispatched,
			(unsigned long long)gCapture.totalCommand,
			(unsigned long long)gCapture.totalRender,
			gCapture.totalPostedX, gCapture.totalPostedY,
			gCapture.totalCommandX, gCapture.totalCommandY,
			gCapture.totalRenderX, gCapture.totalRenderY);
	}

	void ResetCapture(uint32_t requestedFrames, uint64_t presentationGeneration)
	{
		const uint64_t window = gNextWindow++;
		std::memset(&gCapture, 0, sizeof(gCapture));
		gCapture.window = window;
		gCapture.nextSequence = 1;
		gCapture.requestedFrames = std::min(requestedFrames, MaxRequestedFrames);
		gCapture.currentPresentation = presentationGeneration;
		gCapture.abortReason = "none";
		gCapture.active = true;
		gCapture.admittingMouse = true;
		gWindowsMessageTiming = {};
	}
}

uint64_t PerfInputLineageNowUs()
{
	using Clock = std::chrono::steady_clock;
	return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
}

bool PerfInputLineageActive()
{
	return gCapture.active;
}

void PerfInputLineageBeginPresentation(uint64_t presentationGeneration)
{
	if (!gCapture.active)
	{
		if ((int)perf_inputlineageframes <= 0) return;
		ResetCapture((uint32_t)(int)perf_inputlineageframes, presentationGeneration);
		perf_inputlineageframes = 0;
	}
	if (gCapture.observedFrames >= MaxFrameRecords) return;
	gCapture.currentPresentation = presentationGeneration;
	gCapture.latestView = {};
	FrameRecord& frame = gCapture.frames[gCapture.observedFrames];
	frame = {};
	frame.presentation = presentationGeneration;
}

void PerfInputLineageEndPresentation()
{
	if (!gCapture.active) return;
	FrameRecord* frame = CurrentFrame();
	if (frame != nullptr)
	{
		frame->producedThrough = gCapture.producedThrough;
		frame->commandThrough = gCapture.commandThrough;
		frame->buildThrough = gCapture.buildThrough;
		frame->renderThrough = gCapture.renderThrough;
		frame->renderCursorThrough = gCapture.renderCursorThrough;
		gCapture.observedFrames++;
	}
	if (gCapture.admittingMouse && gCapture.observedFrames >= gCapture.requestedFrames)
	{
		gCapture.admittingMouse = false;
		gCapture.drainFramesRemaining = DrainFrameCount;
	}
	else if (!gCapture.admittingMouse && gCapture.drainFramesRemaining > 0)
	{
		gCapture.drainFramesRemaining--;
	}
	if (!gCapture.admittingMouse && gCapture.drainFramesRemaining == 0)
	{
		PrintCapture();
		gCapture.active = false;
		perf_inputlineageframes = 0;
	}
}

void PerfInputLineageAbort(const char* reason)
{
	if (!gCapture.active) return;
	gCapture.abortReason = reason != nullptr ? reason : "unknown";
	PrintCapture();
	gCapture.active = false;
	perf_inputlineageframes = 0;
	gWindowsMessageTiming = {};
}

void PerfInputLineageSetWindowsMessageTiming(bool valid, uint32_t messageTimeMs, uint32_t queueResidenceMs, uint64_t dispatchUs)
{
	gWindowsMessageTiming = { messageTimeMs, queueResidenceMs, dispatchUs, valid };
}

void PerfInputLineageClearWindowsMessageTiming()
{
	gWindowsMessageTiming = {};
}

uint64_t PerfInputLineageRecordMousePost(int rawX, int rawY, float postX, float postY)
{
	if (!gCapture.active || !gCapture.admittingMouse || (rawX == 0 && rawY == 0)) return 0;
	const uint64_t sequence = gCapture.nextSequence++;
	MouseRecord& record = gCapture.mouse[(sequence - 1) % MaxMouseRecords];
	if (record.sequence != 0 && record.sequence != sequence) gCapture.overwritten++;
	record = {};
	record.sequence = sequence;
	record.postUs = PerfInputLineageNowUs();
	record.dispatchUs = gWindowsMessageTiming.valid ? gWindowsMessageTiming.dispatchUs : record.postUs;
	record.postPresentation = gCapture.currentPresentation;
	record.messageTimeMs = gWindowsMessageTiming.messageTimeMs;
	record.queueResidenceMs = gWindowsMessageTiming.queueResidenceMs;
	record.rawX = rawX;
	record.rawY = rawY;
	record.postX = postX;
	record.postY = postY;
	record.windowsTimingValid = gWindowsMessageTiming.valid;
	gCapture.producedThrough = sequence;
	gCapture.totalPosted++;
	gCapture.totalPostedX += postX;
	gCapture.totalPostedY += postY;
	gCapture.mouseHighWater = std::min<uint32_t>(MaxMouseRecords, std::max<uint32_t>(gCapture.mouseHighWater, (uint32_t)sequence));
	if (FrameRecord* frame = CurrentFrame())
	{
		if (frame->firstPosted == 0) frame->firstPosted = sequence;
		frame->lastPosted = sequence;
		frame->posted++;
		frame->postedX += postX;
		frame->postedY += postY;
		if (record.windowsTimingValid) frame->maxQueueUs = std::max(frame->maxQueueUs, uint64_t(record.queueResidenceMs) * 1000);
	}
	return sequence;
}

void PerfInputLineageNoteMouseDispatch(uint64_t sequence)
{
	if (!gCapture.active || sequence == 0) return;
	MouseRecord* record = FindMouse(sequence);
	if (record == nullptr) { gCapture.missingStageRecords++; return; }
	if (record->gameplayDispatched) { gCapture.duplicateStages++; return; }
	record->gameplayDispatched = true;
	if (record->dispatchUs == 0) record->dispatchUs = PerfInputLineageNowUs();
	gCapture.dispatchedThrough = std::max(gCapture.dispatchedThrough, sequence);
	gCapture.totalDispatched++;
	if (FrameRecord* frame = CurrentFrame()) frame->dispatched++;
}

void PerfInputLineageNoteMouseExcluded(uint64_t sequence)
{
	if (!gCapture.active || sequence == 0) return;
	MouseRecord* record = FindMouse(sequence);
	if (record == nullptr) { gCapture.missingStageRecords++; return; }
	if (record->gameplayDispatched || record->excluded) { gCapture.duplicateStages++; return; }
	record->excluded = true;
	gCapture.excludedEvents++;
}

void PerfInputLineageNoteMouseRoute(bool yawLook, bool pitchLook)
{
	if (!gCapture.active) return;
	const uint64_t first = gCapture.commandThrough + 1;
	const uint64_t last = gCapture.dispatchedThrough;
	if (last < first) return;
	ForSequenceRange(first, last, [&](MouseRecord& record)
	{
		if (!record.gameplayDispatched || record.excluded) return;
		record.commandRoute = (yawLook ? RouteYaw : 0) | (pitchLook ? RoutePitch : 0);
		record.syncInput = gCapture.syncInput;
	});
}

void PerfInputLineageNoteCommandSample(bool processedByMovement)
{
	if (!gCapture.active) return;
	const uint64_t nowUs = PerfInputLineageNowUs();
	const uint64_t first = gCapture.commandThrough + 1;
	const uint64_t last = gCapture.dispatchedThrough;
	ForSequenceRange(first, last, [&](MouseRecord& record)
	{
		if (!record.gameplayDispatched || record.excluded) return;
		if (!processedByMovement)
		{
			record.discarded = true;
			gCapture.discardedEvents++;
			return;
		}
		if (record.commandUs != 0) { gCapture.duplicateStages++; return; }
		record.commandUs = nowUs;
		record.commandPresentation = gCapture.currentPresentation;
		gCapture.totalCommand++;
		gCapture.totalCommandX += record.postX;
		gCapture.totalCommandY += record.postY;
		if (FrameRecord* frame = CurrentFrame())
		{
			frame->commandConsumed++;
			frame->commandX += record.postX;
			frame->commandY += record.postY;
			if (nowUs >= record.dispatchUs) frame->maxDispatchToCommandUs = std::max(frame->maxDispatchToCommandUs, nowUs - record.dispatchUs);
		}
	});
	if (last >= first) gCapture.commandThrough = last;
}

void PerfInputLineageDiscardPendingMouse()
{
	PerfInputLineageNoteCommandSample(false);
}

void PerfInputLineageNoteTiccmdBuild(float yawDegrees, float pitchDegrees)
{
	if (!gCapture.active) return;
	const uint64_t nowUs = PerfInputLineageNowUs();
	const uint64_t first = gCapture.buildThrough + 1;
	const uint64_t last = gCapture.commandThrough;
	ForSequenceRange(first, last, [&](MouseRecord& record)
	{
		if (!record.gameplayDispatched || record.excluded || record.discarded || record.commandUs == 0) return;
		if (record.buildUs != 0) { gCapture.duplicateStages++; return; }
		record.buildUs = nowUs;
		record.buildPresentation = gCapture.currentPresentation;
		if (FrameRecord* frame = CurrentFrame()) frame->commandBuilt++;
	});
	if (last >= first) gCapture.buildThrough = last;
	if (FrameRecord* frame = CurrentFrame())
	{
		frame->ticcmdYawDegrees += yawDegrees;
		frame->ticcmdPitchDegrees += pitchDegrees;
	}
}

void PerfInputLineageNoteRenderMouseSample(
	bool yawLook,
	bool pitchLook,
	float yawDegrees,
	float pitchDegrees,
	bool lateLatch)
{
	if (!gCapture.active) return;
	const uint64_t nowUs = PerfInputLineageNowUs();
	const uint64_t first = gCapture.renderCursorThrough + 1;
	const uint64_t last = gCapture.dispatchedThrough;
	ForSequenceRange(first, last, [&](MouseRecord& record)
	{
		if (!record.gameplayDispatched || record.excluded || record.discarded) return;
		if (record.renderConsidered) { gCapture.duplicateStages++; return; }
		record.renderConsidered = true;
		record.renderRoute = (yawLook ? RouteYaw : 0) | (pitchLook ? RoutePitch : 0);
		record.syncInput = gCapture.syncInput;
		const bool eligibleX = (record.renderRoute & RouteYaw) != 0 && record.postX != 0.0f;
		const bool eligibleY = (record.renderRoute & RoutePitch) != 0 && record.postY != 0.0f;
		if (!eligibleX && !eligibleY) return;
		if (record.syncInput) return;
		record.cameraUs = nowUs;
		record.cameraPresentation = gCapture.currentPresentation;
		record.cameraLateLatched = lateLatch;
		gCapture.renderThrough = std::max(gCapture.renderThrough, record.sequence);
		gCapture.totalRender++;
		gCapture.totalRenderX += eligibleX ? record.postX : 0.0f;
		gCapture.totalRenderY += eligibleY ? record.postY : 0.0f;
		if (FrameRecord* frame = CurrentFrame())
		{
			frame->renderConsumed++;
			if (lateLatch) frame->renderLateConsumed++;
			else frame->renderEarlyConsumed++;
			frame->renderX += eligibleX ? record.postX : 0.0f;
			frame->renderY += eligibleY ? record.postY : 0.0f;
			if (nowUs >= record.dispatchUs) frame->maxDispatchToCameraUs = std::max(frame->maxDispatchToCameraUs, nowUs - record.dispatchUs);
		}
	});
	if (last >= first) gCapture.renderCursorThrough = last;
	if (FrameRecord* frame = CurrentFrame())
	{
		frame->cameraYawDegrees += yawDegrees;
		frame->cameraPitchDegrees += pitchDegrees;
		if (lateLatch)
		{
			frame->lateCameraYawDegrees += yawDegrees;
			frame->lateCameraPitchDegrees += pitchDegrees;
		}
	}
}

void PerfInputLineageNoteLatePump(uint64_t durationUs, bool routingInvalidated, bool latchApplied)
{
	FrameRecord* frame = CurrentFrame();
	if (frame == nullptr) return;
	frame->latePumpCalls++;
	frame->latePumpUs += durationUs;
	if (routingInvalidated) frame->latePumpInvalidations++;
	if (latchApplied) frame->lateLatchApplies++;
}

void PerfInputLineageNoteInputMode(bool syncInput)
{
	if (gCapture.active) gCapture.syncInput = syncInput;
}

void PerfInputLineageNoteViewCapture(float yawDegrees, float pitchDegrees)
{
	if (!gCapture.active) return;
	FrameRecord* frame = CurrentFrame();
	if (frame == nullptr || frame->view.valid) return;
	const uint64_t nowUs = PerfInputLineageNowUs();
	PerfInputLineageViewSnapshot view = {};
	view.window = gCapture.window;
	view.presentationGeneration = gCapture.currentPresentation;
	view.producedThrough = gCapture.producedThrough;
	view.commandConsumedThrough = gCapture.commandThrough;
	view.commandBuiltThrough = gCapture.buildThrough;
	view.renderConsumedThrough = gCapture.renderThrough;
	view.renderCursorThrough = gCapture.renderCursorThrough;
	view.captureUs = nowUs;
	view.yawDegrees = yawDegrees;
	view.pitchDegrees = pitchDegrees;
	view.syncInput = gCapture.syncInput;
	view.valid = true;
	frame->view = view;
	gCapture.latestView = view;
	const uint64_t first = gCapture.viewThrough + 1;
	const uint64_t last = gCapture.renderThrough;
	ForSequenceRange(first, last, [&](MouseRecord& record)
	{
		if (record.cameraUs == 0) return;
		record.viewUs = nowUs;
		record.viewPresentation = gCapture.currentPresentation;
		if (nowUs >= record.cameraUs) frame->maxCameraToViewUs = std::max(frame->maxCameraToViewUs, nowUs - record.cameraUs);
	});
	if (last >= first) gCapture.viewThrough = last;
}

PerfInputLineageViewSnapshot PerfInputLineageGetLatestViewSnapshot()
{
	return gCapture.active ? gCapture.latestView : PerfInputLineageViewSnapshot{};
}

void PerfInputLineageNoteNriSubmit(
	uint64_t presentationGeneration, const PerfInputLineageViewSnapshot& view,
	uint64_t nriFrame, uint32_t queuedFrameIndex, uint32_t swapChainImageIndex,
	uint64_t attemptedFence, bool completedFenceValid, uint64_t completedFence,
	uint32_t outstandingBefore, uint32_t outstandingAfter,
	uint64_t submitStartUs, uint64_t submitEndUs, int32_t submitResult, bool submitAccepted)
{
	FrameRecord* frame = CurrentFrame();
	if (frame == nullptr) return;
	frame->nriFrame = nriFrame;
	frame->submitPresentation = presentationGeneration;
	frame->submitPresentationMatch = presentationGeneration == frame->presentation;
	frame->submitViewMatch = view.valid && view.window == gCapture.window &&
		view.presentationGeneration == presentationGeneration && frame->view.valid &&
		frame->view.captureUs == view.captureUs;
	frame->queuedFrameIndex = queuedFrameIndex;
	frame->swapChainImageIndex = swapChainImageIndex;
	frame->attemptedFence = attemptedFence;
	frame->completedFenceValid = completedFenceValid;
	frame->completedFence = completedFence;
	frame->outstandingBefore = outstandingBefore;
	frame->outstandingAfter = outstandingAfter;
	frame->submitStartUs = submitStartUs;
	frame->submitEndUs = submitEndUs;
	frame->submitResult = submitResult;
	frame->submitSeen = true;
	frame->submitAccepted = submitAccepted;
}

void PerfInputLineageNoteNriPresent(uint64_t presentationGeneration, uint64_t nriFrame, uint64_t presentStartUs, uint64_t presentEndUs, int32_t presentResult, bool presentAccepted, bool frameGenerationPath)
{
	FrameRecord* frame = CurrentFrame();
	if (frame == nullptr) return;
	frame->presentStartUs = presentStartUs;
	frame->presentEndUs = presentEndUs;
	frame->presentResult = presentResult;
	frame->presentSeen = true;
	frame->presentAccepted = presentAccepted;
	frame->presentJoinMatch = frame->submitSeen && frame->submitPresentation == presentationGeneration && frame->nriFrame == nriFrame;
	frame->frameGenerationPath = frameGenerationPath;
}
