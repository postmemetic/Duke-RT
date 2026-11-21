#pragma once

#include "nri_local.h"

#include <cstdint>
#include <vector>

class NRIFrameShell
{
public:
	static constexpr uint32_t FrameSequenceHistorySize = 8;
	static constexpr uint32_t QueuedFrameCount = 3;

	static uint64_t RecycleFenceValue(uint64_t frameIndex, std::size_t queuedFrames)
	{
		return queuedFrames && frameIndex >= queuedFrames ? 1 + frameIndex - queuedFrames : 0;
	}

	struct QueuedFrame
	{
		nri::CommandAllocator* commandAllocator = nullptr;
		nri::CommandBuffer* commandBuffer = nullptr;
		uint64_t lastSubmittedFenceValue = 0;
		uint64_t lastSubmittedFrameIndex = 0;
		bool hasSubmittedWork = false;
	};

	struct FrameSequenceEntry
	{
		uint64_t frameNumber = 0;
		uint64_t frameIndex = 0;
		uint64_t submittedFenceValue = 0;
		uint32_t queuedFrameIndex = 0;
		uint32_t acquiredImageIndex = 0;
		uint32_t acquireSemaphoreIndex = 0;
		uint32_t presentedImageIndex = 0;
		uint32_t releaseSemaphoreIndex = 0;
		nri::Result presentResult = nri::Result::FAILURE;
		bool sanityFrameUsed = false;
		bool valid = false;
	};

	struct FrameBoundaryDebugStats
	{
		uint64_t frameNumber = 0;
		uint64_t frameIndex = 0;
		double waitMs = 0.0;
		double waitForPresentMs = 0.0;
		double acquireMs = 0.0;
		double submitMs = 0.0;
		double presentMs = 0.0;
		uint64_t submittedFenceValue = 0;
		nri::Result waitForPresentResult = nri::Result::FAILURE;
		nri::Result acquireResult = nri::Result::FAILURE;
		nri::Result presentResult = nri::Result::FAILURE;
		uint32_t queuedFrameIndex = 0;
		uint32_t swapChainImageIndex = 0;
		uint32_t acquireSemaphoreIndex = 0;
		bool sanityModeEnabled = false;
		bool sanityFrameUsed = false;
		bool sceneTargetSelected = false;
		bool pathTracedSceneRendered = false;
		bool sceneCopiedToPresent = false;
		bool postProcessInvoked = false;
	};
};
