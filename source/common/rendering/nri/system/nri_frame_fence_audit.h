#pragma once

#include "nri_frame_shell.h"

namespace nri_fence_audit
{
	bool Enabled();
	bool DenseGpuTiming();
	uint64_t CpuTicks();

	struct Wait
	{
		uint64_t startTicks = 0;
		uint64_t completedBefore = 0;
	};

	Wait BeginWait(nri::CoreInterface& core, nri::Fence* fence);
	void EndWait(const Wait& wait, nri::CoreInterface& core, nri::Fence* fence,
		nri::GraphicsAPI api, nri::Queue* queue, uint64_t frame, uint32_t slot,
		const std::vector<NRIFrameShell::QueuedFrame>& queuedFrames);
	void RecordSubmit(uint64_t frame, uint32_t slot, uint32_t image, uint64_t frameFence,
		uint64_t commandFence, uint64_t startTicks, uint64_t endTicks, nri::Result result, bool waitedForImage);
}
