#include "nri_frame_fence_audit.h"
#include "c_cvars.h"
#include "perf_audit.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#include <d3d12.h>
#endif

// Mode 1 observes frame fences. Mode 2 also samples each command segment when GPU timing is enabled.
CVAR(Int, nri_ptframefencetrace, 0, 0)

namespace nri_fence_audit
{
	bool Enabled() { return nri_ptframefencetrace > 0 && PerfAuditActive(); }
	bool DenseGpuTiming() { return nri_ptframefencetrace >= 2 && PerfAuditActive(); }

	uint64_t CpuTicks()
	{
#ifdef _WIN32
		LARGE_INTEGER value;
		QueryPerformanceCounter(&value);
		return value.QuadPart;
#else
		return PerfAuditClockNs();
#endif
	}

	Wait BeginWait(nri::CoreInterface& core, nri::Fence* fence)
	{
		if (!Enabled() || !fence) return {};
		const uint64_t completed = core.GetFenceValue(*fence);
		return {CpuTicks(), completed};
	}

	static void RecordClock(nri::CoreInterface& core, nri::GraphicsAPI api, nri::Queue* queue, uint64_t frame)
	{
		// Each anchor is in the same queue clock domain as its command timestamps.
		// Calibrating outside the fence wait keeps driver-call time out of the measurement.
		static PerfAuditToken previous;
		static uint64_t previousFrame = 0;
		static nri::Queue* previousQueue = nullptr;
		const auto token = PerfAuditCurrentToken();
		if (token.epoch == previous.epoch && queue == previousQueue && frame >= previousFrame && frame - previousFrame < 64) return;
		previous = token;
		previousFrame = frame;
		previousQueue = queue;
		uint64_t gpu = 0, cpu = 0, gpuHz = 0, cpuHz = 0;
		bool valid = false;
#ifdef _WIN32
		LARGE_INTEGER frequency;
		QueryPerformanceFrequency(&frequency);
		cpuHz = frequency.QuadPart;
		if (api == nri::GraphicsAPI::D3D12 && queue)
		{
			auto* native = static_cast<ID3D12CommandQueue*>(core.GetQueueNativeObject(queue));
			valid = native && SUCCEEDED(native->GetTimestampFrequency(&gpuHz)) &&
				SUCCEEDED(native->GetClockCalibration(&gpu, &cpu)) && gpuHz && cpuHz;
		}
#else
		cpuHz = 1000000000;
#endif
		char fields[384];
		std::snprintf(fields, sizeof(fields), "\"frame_index\":%" PRIu64 ",\"calibrated\":%s,\"cpu_ticks\":%" PRIu64
			",\"cpu_hz\":%" PRIu64 ",\"gpu_ticks\":%" PRIu64 ",\"gpu_hz\":%" PRIu64,
			frame, valid ? "true" : "false", cpu, cpuHz, gpu, gpuHz);
		PerfAuditWriteEvent("frame_fence_clock", token, fields);
	}

	void EndWait(const Wait& wait, nri::CoreInterface& core, nri::Fence* fence,
		nri::GraphicsAPI api, nri::Queue* queue, uint64_t frame, uint32_t slot,
		const std::vector<NRIFrameShell::QueuedFrame>& queuedFrames)
	{
		if (!wait.startTicks || !fence || slot >= queuedFrames.size()) return;
		const uint64_t end = CpuTicks();
		const uint64_t completed = core.GetFenceValue(*fence);
		const auto& prior = queuedFrames[slot];
		const uint64_t target = NRIFrameShell::RecycleFenceValue(frame, queuedFrames.size());
		uint64_t latest = 0;
		for (const auto& queued : queuedFrames)
			if (queued.hasSubmittedWork) latest = (std::max)(latest, queued.lastSubmittedFenceValue);
		char fields[768];
		std::snprintf(fields, sizeof(fields), "\"frame_index\":%" PRIu64 ",\"queued_slot\":%u,\"queued_frames\":%zu,"
			"\"target\":%" PRIu64 ",\"latest_submitted\":%" PRIu64 ",\"slot_submitted\":%s,\"slot_frame\":%" PRIu64
			",\"slot_fence\":%" PRIu64 ",\"completed_before\":%" PRIu64 ",\"completed_after\":%" PRIu64
			",\"fence_read_valid\":%s,\"cpu_start_ticks\":%" PRIu64 ",\"cpu_end_ticks\":%" PRIu64,
			frame, slot, queuedFrames.size(), target, latest, prior.hasSubmittedWork ? "true" : "false",
			prior.lastSubmittedFrameIndex, prior.lastSubmittedFenceValue, wait.completedBefore, completed,
			wait.completedBefore != UINT64_MAX && completed != UINT64_MAX ? "true" : "false", wait.startTicks, end);
		PerfAuditWriteEvent("frame_fence_recycle", PerfAuditCurrentToken(), fields);
		RecordClock(core, api, queue, frame);
	}

	void RecordSubmit(uint64_t frame, uint32_t slot, uint32_t image, uint64_t frameFence,
		uint64_t commandFence, uint64_t startTicks, uint64_t endTicks, nri::Result result, bool waitedForImage)
	{
		char fields[448];
		std::snprintf(fields, sizeof(fields), "\"frame_index\":%" PRIu64 ",\"queued_slot\":%u,\"image\":%u,"
			"\"frame_fence\":%" PRIu64 ",\"command_fence\":%" PRIu64 ",\"waits_on_image\":%s,\"submitted\":%s,"
			"\"cpu_start_ticks\":%" PRIu64 ",\"cpu_end_ticks\":%" PRIu64,
			frame, slot, image, frameFence, commandFence, waitedForImage ? "true" : "false",
			result == nri::Result::SUCCESS ? "true" : "false", startTicks, endTicks);
		PerfAuditWriteEvent("frame_fence_submit", PerfAuditCurrentToken(), fields);
	}
}
