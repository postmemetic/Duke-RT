#include "nri_descriptor_sets.h"
#include "nri_cvars.h"

#include "../scene/nri_hash.h"
#include "nri_renderer.h"
#include "nri_shader_contracts.h"
#include "../system/nri_renderdevice.h"
#include "c_cvars.h"
#include "perf_capture.h"
#include "printf.h"

#include <algorithm>
#include <chrono>
#include <cstdint>


namespace
{
	static uint64_t HashDescriptorList(const nri::Descriptor* const* descriptors, size_t count)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = nri_scene::HashCombine64(hash, (uint64_t)count);
		for (size_t i = 0; i < count; ++i)
		{
			hash = nri_scene::HashCombine64(hash, (uint64_t)(uintptr_t)descriptors[i]);
		}
		return hash;
	}

	static bool ShouldTraceDescriptorCoherency()
	{
		return (int)nri_ptactorspritetrace > 0 && (int)nri_pttraceframes > 0;
	}

	static bool ShouldCollectDescriptorTiming()
	{
		return (int)nri_pttraceframes > 0 || (int)perf_looptraceframes > 0 || (bool)nri_ptslowdowntrace || PerfCompactCaptureTimingActive();
	}

	static double DurationMs(const std::chrono::steady_clock::time_point& start, const std::chrono::steady_clock::time_point& end)
	{
		return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start).count();
	}

	class ScopedDescriptorPerfTimer
	{
	public:
		explicit ScopedDescriptorPerfTimer(double& targetMs)
			: mTarget(ShouldCollectDescriptorTiming() ? &targetMs : nullptr)
		{
			if (mTarget != nullptr)
			{
				mStart = std::chrono::steady_clock::now();
			}
		}

		~ScopedDescriptorPerfTimer()
		{
			if (mTarget != nullptr)
			{
				*mTarget += DurationMs(mStart, std::chrono::steady_clock::now());
			}
		}

	private:
		double* mTarget = nullptr;
		std::chrono::steady_clock::time_point mStart = {};
	};
}

bool NRIDescriptorSetManager::AllocateDescriptorSets(NRIRenderer& renderer)
{
	const uint32_t queuedFrameCount = renderer.mFrameBuffer != nullptr ? std::max(1u, (uint32_t)renderer.mFrameBuffer->mQueuedFrames.size()) : 1u;
	renderer.mSceneTextureSets.assign(queuedFrameCount, nullptr);
	renderer.mSceneTextureSetHashes.assign(queuedFrameCount, 0);
	renderer.mSceneTextureSetHashValid.assign(queuedFrameCount, 0);
	renderer.mSceneDataSets.assign(queuedFrameCount, nullptr);
	renderer.mSceneDataDescriptorsInitialized.assign(queuedFrameCount, 0u);
	renderer.mSceneDataDescriptorMapEpochs.assign(queuedFrameCount, 0ull);
	renderer.mSceneDataDescriptorBuildEpochs.assign(queuedFrameCount, 0ull);
	const uint32_t sceneDataSnapshotCount = std::max(8u, queuedFrameCount * 4u);
	renderer.mSceneDataSnapshots.clear();
	renderer.mSceneDataSnapshots.resize(sceneDataSnapshotCount);
	renderer.mActiveSceneDataSet = nullptr;
	renderer.mActiveSceneDataSnapshot = nullptr;
	renderer.mActiveSceneDataSetFrameIndex = UINT64_MAX;
	renderer.mSceneDataSnapshotCursor = 0;

	auto allocateSets = [&](nri::PipelineLayout* layout, uint32_t setIndex, auto& sets) -> bool
	{
		for (nri::DescriptorSet*& set : sets)
		{
			if (renderer.mFrameBuffer->mCore.AllocateDescriptorSets(*renderer.mFrameBuffer->mDescriptorPool, *layout, setIndex, &set, 1, 0) != nri::Result::SUCCESS)
			{
				return false;
			}
		}

		return true;
	};

	bool sceneDataSnapshotsAllocated = true;
	for (NRIRenderer::SceneDataDescriptorSnapshot& snapshot : renderer.mSceneDataSnapshots)
	{
		if (renderer.mFrameBuffer->mCore.AllocateDescriptorSets(*renderer.mFrameBuffer->mDescriptorPool, *renderer.mPipelineLayout, 2, &snapshot.sceneDataSet, 1, 0) != nri::Result::SUCCESS)
		{
			sceneDataSnapshotsAllocated = false;
			break;
		}
	}

	return
		renderer.mFrameBuffer->mCore.AllocateDescriptorSets(*renderer.mFrameBuffer->mDescriptorPool, *renderer.mPipelineLayout, 0, &renderer.mSamplerSet, 1, 0) == nri::Result::SUCCESS &&
		allocateSets(renderer.mPipelineLayout, 1, renderer.mSceneTextureSets) &&
		allocateSets(renderer.mPipelineLayout, 2, renderer.mSceneDataSets) &&
		sceneDataSnapshotsAllocated &&
		renderer.mFrameBuffer->mCore.AllocateDescriptorSets(*renderer.mFrameBuffer->mDescriptorPool, *renderer.mPipelineLayout, 3, &renderer.mFrameTextureSet, 1, 0) == nri::Result::SUCCESS &&
		renderer.mFrameBuffer->mCore.AllocateDescriptorSets(*renderer.mFrameBuffer->mDescriptorPool, *renderer.mPipelineLayout, 4, &renderer.mOutputSet, 1, 0) == nri::Result::SUCCESS &&
		renderer.mFrameBuffer->mCore.AllocateDescriptorSets(*renderer.mFrameBuffer->mDescriptorPool, *renderer.mPipelineLayout, 3, &renderer.mCompositionFrameTextureSet, 1, 0) == nri::Result::SUCCESS &&
		renderer.mFrameBuffer->mCore.AllocateDescriptorSets(*renderer.mFrameBuffer->mDescriptorPool, *renderer.mPipelineLayout, 4, &renderer.mCompositionOutputSet, 1, 0) == nri::Result::SUCCESS &&
		renderer.mFrameBuffer->mCore.AllocateDescriptorSets(*renderer.mFrameBuffer->mDescriptorPool, *renderer.mPipelineLayout, 3, &renderer.mUpscalerPrepassFrameTextureSet, 1, 0) == nri::Result::SUCCESS &&
		renderer.mFrameBuffer->mCore.AllocateDescriptorSets(*renderer.mFrameBuffer->mDescriptorPool, *renderer.mPipelineLayout, 4, &renderer.mUpscalerPrepassOutputSet, 1, 0) == nri::Result::SUCCESS &&
		renderer.mFrameBuffer->mCore.AllocateDescriptorSets(*renderer.mFrameBuffer->mDescriptorPool, *renderer.mTaaPipelineLayout, 0, &renderer.mTaaFrameTextureSet, 1, 0) == nri::Result::SUCCESS &&
		renderer.mFrameBuffer->mCore.AllocateDescriptorSets(*renderer.mFrameBuffer->mDescriptorPool, *renderer.mTaaPipelineLayout, 1, &renderer.mTaaOutputSet, 1, 0) == nri::Result::SUCCESS &&
		renderer.mFrameBuffer->mCore.AllocateDescriptorSets(*renderer.mFrameBuffer->mDescriptorPool, *renderer.mTaaPipelineLayout, 0, &renderer.mTemporalReactiveInputSet, 1, 0) == nri::Result::SUCCESS &&
		renderer.mFrameBuffer->mCore.AllocateDescriptorSets(*renderer.mFrameBuffer->mDescriptorPool, *renderer.mTaaPipelineLayout, 1, &renderer.mTemporalReactiveOutputSet, 1, 0) == nri::Result::SUCCESS &&
		renderer.mFrameBuffer->mCore.AllocateDescriptorSets(*renderer.mFrameBuffer->mDescriptorPool, *renderer.mPresentPipelineLayout, 0, &renderer.mRawPresentFrameTextureSet, 1, 0) == nri::Result::SUCCESS &&
		renderer.mFrameBuffer->mCore.AllocateDescriptorSets(*renderer.mFrameBuffer->mDescriptorPool, *renderer.mPresentPipelineLayout, 1, &renderer.mRawPresentOutputSet, 1, 0) == nri::Result::SUCCESS &&
		renderer.mFrameBuffer->mCore.AllocateDescriptorSets(*renderer.mFrameBuffer->mDescriptorPool, *renderer.mPresentPipelineLayout, 0, &renderer.mFinalPresentFrameTextureSet, 1, 0) == nri::Result::SUCCESS &&
		renderer.mFrameBuffer->mCore.AllocateDescriptorSets(*renderer.mFrameBuffer->mDescriptorPool, *renderer.mPresentPipelineLayout, 1, &renderer.mFinalPresentOutputSet, 1, 0) == nri::Result::SUCCESS &&
		allocateSets(renderer.mBloomPipelineLayout, 0, renderer.mBloomInputSets) &&
		allocateSets(renderer.mBloomPipelineLayout, 1, renderer.mBloomOutputSets) &&
		renderer.mFrameBuffer->mCore.AllocateDescriptorSets(*renderer.mFrameBuffer->mDescriptorPool, *renderer.mExposurePipelineLayout, 0, &renderer.mExposureInputSets[0], 1, 0) == nri::Result::SUCCESS &&
		renderer.mFrameBuffer->mCore.AllocateDescriptorSets(*renderer.mFrameBuffer->mDescriptorPool, *renderer.mExposurePipelineLayout, 1, &renderer.mExposureOutputSets[0], 1, 0) == nri::Result::SUCCESS &&
		renderer.mFrameBuffer->mCore.AllocateDescriptorSets(*renderer.mFrameBuffer->mDescriptorPool, *renderer.mExposurePipelineLayout, 0, &renderer.mExposureInputSets[1], 1, 0) == nri::Result::SUCCESS &&
		renderer.mFrameBuffer->mCore.AllocateDescriptorSets(*renderer.mFrameBuffer->mDescriptorPool, *renderer.mExposurePipelineLayout, 1, &renderer.mExposureOutputSets[1], 1, 0) == nri::Result::SUCCESS &&
		allocateSets(renderer.mVoxelComputePipelineLayout, NRI_VOXEL_COMPUTE_SET_INPUTS, renderer.mVoxelComputeInputSets) &&
		allocateSets(renderer.mVoxelComputePipelineLayout, NRI_VOXEL_COMPUTE_SET_OUTPUTS, renderer.mVoxelComputeOutputSets);
}

bool NRIDescriptorSetManager::UpdateSamplerSet(NRIRenderer& renderer)
{
	const nri::Descriptor* descriptors[NRI_SAMPLER_DESCRIPTOR_NUM] = {
		renderer.mFrameBuffer->mSamplers[(size_t)NRISamplerMode::WrapLinear],
		renderer.mFrameBuffer->mSamplers[(size_t)NRISamplerMode::ClampLinear],
		renderer.mFrameBuffer->mSamplers[(size_t)NRISamplerMode::WrapPoint],
		renderer.mFrameBuffer->mSamplers[(size_t)NRISamplerMode::ClampPoint]
	};
	nri::UpdateDescriptorRangeDesc update = {};
	update.descriptorSet = renderer.mSamplerSet;
	update.rangeIndex = 0;
	update.descriptors = descriptors;
	update.descriptorNum = NRI_SAMPLER_DESCRIPTOR_NUM;
	renderer.mFrameBuffer->mCore.UpdateDescriptorRanges(&update, 1);
	return true;
}

bool NRIDescriptorSetManager::CommitSceneDataDescriptors(NRIRenderer& renderer, const char* reason)
{
	{
		ScopedDescriptorPerfTimer descriptorValidateTimer(renderer.mLastPerfShellTraceStats.sceneDataSetDescriptorValidateMs);
		for (const nri::Descriptor* descriptor : renderer.mSceneDataDescriptors)
		{
			if (descriptor == nullptr)
			{
				renderer.mLastPerfShellTraceStats.sceneDataSetDescriptorNullCount++;
				return false;
			}
		}
	}

	nri::DescriptorSet* sceneDataSet = GetCurrentSceneDataSet(renderer);
	if (sceneDataSet == nullptr)
	{
		return false;
	}

	nri::UpdateDescriptorRangeDesc update = {};
	update.descriptorSet = sceneDataSet;
	update.rangeIndex = 0;
	update.descriptors = reinterpret_cast<const nri::Descriptor* const*>(renderer.mSceneDataDescriptors.data());
	update.descriptorNum = NRI_SCENE_DATA_DESCRIPTOR_NUM;
	{
		ScopedDescriptorPerfTimer descriptorUpdateTimer(renderer.mLastPerfShellTraceStats.sceneDataSetDescriptorUpdateMs);
		renderer.mFrameBuffer->mCore.UpdateDescriptorRanges(&update, 1);
	}
	renderer.mLastPerfShellTraceStats.sceneDataSetDescriptorUpdateCount++;
	renderer.mSceneDataDescriptorGeneration++;
	SetCurrentSceneDataDescriptorsInitialized(renderer, true);
	{
		ScopedDescriptorPerfTimer descriptorHashTimer(renderer.mLastPerfShellTraceStats.sceneDataSetDescriptorHashMs);
		TraceSharedDescriptorRewrite(
			renderer,
			"scene_data",
			reason != nullptr ? reason : "unlabeled",
			HashDescriptorList(reinterpret_cast<const nri::Descriptor* const*>(renderer.mSceneDataDescriptors.data()), renderer.mSceneDataDescriptors.size()),
			NRI_SCENE_DATA_DESCRIPTOR_NUM,
			false);
	}
	return true;
}

bool NRIDescriptorSetManager::UpdateFrameTextureSet(NRIRenderer& renderer)
{
	return UpdateFrameTextureSet(renderer, renderer.mFrameTextureSet, renderer.mFrameInputDescriptors);
}

bool NRIDescriptorSetManager::UpdateFrameTextureSet(NRIRenderer& renderer, nri::DescriptorSet* set, const std::array<nri::Descriptor*, NRI_INPUT_DESCRIPTOR_NUM>& descriptors)
{
	const nri::Descriptor* rawDescriptors[NRI_INPUT_DESCRIPTOR_NUM] = {};
	for (size_t i = 0; i < NRI_INPUT_DESCRIPTOR_NUM; ++i)
	{
		rawDescriptors[i] = descriptors[i];
	}

	nri::UpdateDescriptorRangeDesc update = {};
	update.descriptorSet = set;
	update.rangeIndex = 0;
	update.descriptors = rawDescriptors;
	update.descriptorNum = NRI_INPUT_DESCRIPTOR_NUM;
	renderer.mFrameBuffer->mCore.UpdateDescriptorRanges(&update, 1);
	return true;
}

bool NRIDescriptorSetManager::UpdateOutputSet(NRIRenderer& renderer)
{
	return UpdateOutputSet(renderer, renderer.mOutputSet, renderer.mOutputDescriptors);
}

bool NRIDescriptorSetManager::UpdateOutputSet(NRIRenderer& renderer, nri::DescriptorSet* set, const std::array<nri::Descriptor*, NRI_OUTPUT_DESCRIPTOR_NUM>& descriptors)
{
	if (!renderer.mTraceShaderStats.Ensure(renderer.BuildResourceServices()))
	{
		return false;
	}

	const nri::Descriptor* rawDescriptors[NRI_OUTPUT_DESCRIPTOR_NUM] = {};
	for (size_t i = 0; i < NRI_OUTPUT_DESCRIPTOR_NUM; ++i)
	{
		rawDescriptors[i] = descriptors[i];
	}

	nri::UpdateDescriptorRangeDesc update = {};
	update.descriptorSet = set;
	update.rangeIndex = 0;
	update.descriptors = rawDescriptors;
	update.descriptorNum = NRI_OUTPUT_DESCRIPTOR_NUM;
	renderer.mFrameBuffer->mCore.UpdateDescriptorRanges(&update, 1);

	const nri::Descriptor* traceStatsDescriptor = renderer.mTraceShaderStats.Descriptor();
	nri::UpdateDescriptorRangeDesc statsUpdate = {};
	statsUpdate.descriptorSet = set;
	statsUpdate.rangeIndex = 1;
	statsUpdate.descriptors = &traceStatsDescriptor;
	statsUpdate.descriptorNum = NRI_TRACE_SHADER_STATS_DESCRIPTOR_NUM;
	renderer.mFrameBuffer->mCore.UpdateDescriptorRanges(&statsUpdate, 1);
	return true;
}

nri::DescriptorSet* NRIDescriptorSetManager::GetCurrentSceneTextureSet(const NRIRenderer& renderer)
{
	const uint32_t queuedFrameIndex = renderer.GetCurrentQueuedFrameIndex();
	return queuedFrameIndex < renderer.mSceneTextureSets.size() ? renderer.mSceneTextureSets[queuedFrameIndex] : nullptr;
}

nri::DescriptorSet* NRIDescriptorSetManager::GetCurrentSceneDataSet(const NRIRenderer& renderer)
{
	if (renderer.mActiveSceneDataSet != nullptr && renderer.mActiveSceneDataSetFrameIndex == renderer.mFrameIndex)
	{
		return renderer.mActiveSceneDataSet;
	}

	const uint32_t queuedFrameIndex = renderer.GetCurrentQueuedFrameIndex();
	return queuedFrameIndex < renderer.mSceneDataSets.size() ? renderer.mSceneDataSets[queuedFrameIndex] : nullptr;
}

bool NRIDescriptorSetManager::IsCurrentSceneDataDescriptorsInitialized(const NRIRenderer& renderer)
{
	const uint64_t currentMapEpoch = renderer.mMapWorld.valid ? renderer.mMapWorld.buildSerial : 0ull;
	const uint64_t currentBuildEpoch = renderer.mStaticMapScene.valid ? renderer.mStaticMapScene.buildSerial : 0ull;
	if (renderer.mActiveSceneDataSnapshot != nullptr &&
		renderer.mActiveSceneDataSet == renderer.mActiveSceneDataSnapshot->sceneDataSet &&
		renderer.mActiveSceneDataSetFrameIndex == renderer.mFrameIndex)
	{
		return renderer.mActiveSceneDataSnapshot->descriptorsInitialized &&
			renderer.mActiveSceneDataSnapshot->publishedMapEpoch == currentMapEpoch &&
			renderer.mActiveSceneDataSnapshot->publishedBuildEpoch == currentBuildEpoch;
	}

	const uint32_t queuedFrameIndex = renderer.GetCurrentQueuedFrameIndex();
	return queuedFrameIndex < renderer.mSceneDataDescriptorsInitialized.size() &&
		queuedFrameIndex < renderer.mSceneDataDescriptorMapEpochs.size() &&
		queuedFrameIndex < renderer.mSceneDataDescriptorBuildEpochs.size() &&
		renderer.mSceneDataDescriptorsInitialized[queuedFrameIndex] != 0 &&
		renderer.mSceneDataDescriptorMapEpochs[queuedFrameIndex] == currentMapEpoch &&
		renderer.mSceneDataDescriptorBuildEpochs[queuedFrameIndex] == currentBuildEpoch;
}

void NRIDescriptorSetManager::SetCurrentSceneDataDescriptorsInitialized(NRIRenderer& renderer, bool value)
{
	const uint64_t currentMapEpoch = renderer.mMapWorld.valid ? renderer.mMapWorld.buildSerial : 0ull;
	const uint64_t currentBuildEpoch = renderer.mStaticMapScene.valid ? renderer.mStaticMapScene.buildSerial : 0ull;
	if (renderer.mActiveSceneDataSnapshot != nullptr &&
		renderer.mActiveSceneDataSet == renderer.mActiveSceneDataSnapshot->sceneDataSet &&
		renderer.mActiveSceneDataSetFrameIndex == renderer.mFrameIndex)
	{
		renderer.mActiveSceneDataSnapshot->descriptorsInitialized = value;
		renderer.mActiveSceneDataSnapshot->publishedMapEpoch = value ? currentMapEpoch : 0ull;
		renderer.mActiveSceneDataSnapshot->publishedBuildEpoch = value ? currentBuildEpoch : 0ull;
		return;
	}

	const uint32_t queuedFrameIndex = renderer.GetCurrentQueuedFrameIndex();
	if (queuedFrameIndex >= renderer.mSceneDataDescriptorsInitialized.size() ||
		queuedFrameIndex >= renderer.mSceneDataDescriptorMapEpochs.size() ||
		queuedFrameIndex >= renderer.mSceneDataDescriptorBuildEpochs.size())
	{
		return;
	}

	renderer.mSceneDataDescriptorsInitialized[queuedFrameIndex] = value ? 1u : 0u;
	renderer.mSceneDataDescriptorMapEpochs[queuedFrameIndex] = value ? currentMapEpoch : 0ull;
	renderer.mSceneDataDescriptorBuildEpochs[queuedFrameIndex] = value ? currentBuildEpoch : 0ull;
}

void NRIDescriptorSetManager::TraceSharedDescriptorRewrite(
	NRIRenderer& renderer,
	const char* setName,
	const char* reason,
	uint64_t descriptorHash,
	uint32_t descriptorCount,
	bool sceneTextureSet)
{
	if (sceneTextureSet)
	{
		renderer.mDescriptorCoherencyDebugStats.sceneTextureSetUpdates++;
		renderer.mDescriptorCoherencyDebugStats.lastSceneTextureDescriptorHash = descriptorHash;
		renderer.mDescriptorCoherencyDebugStats.lastSceneTextureDescriptorCount = descriptorCount;
		renderer.mDescriptorCoherencyDebugStats.lastSceneTextureReason = reason != nullptr ? reason : "unlabeled";
	}
	else
	{
		renderer.mDescriptorCoherencyDebugStats.sceneDataSetUpdates++;
		renderer.mDescriptorCoherencyDebugStats.lastSceneDataDescriptorHash = descriptorHash;
		renderer.mDescriptorCoherencyDebugStats.lastSceneDataDescriptorCount = descriptorCount;
		renderer.mDescriptorCoherencyDebugStats.lastSceneDataReason = reason != nullptr ? reason : "unlabeled";
	}

	uint32_t queuedFrameIndex = 0;
	uint64_t queuedFrameFence = 0;
	uint64_t submittedFence = 0;
	if (renderer.mFrameBuffer != nullptr)
	{
		queuedFrameIndex = renderer.mFrameBuffer->mCurrentQueuedFrameIndex;
		submittedFence = renderer.mFrameBuffer->mSubmittedFenceValue;
		if (queuedFrameIndex < renderer.mFrameBuffer->mQueuedFrames.size())
		{
			queuedFrameFence = renderer.mFrameBuffer->mQueuedFrames[queuedFrameIndex].lastSubmittedFenceValue;
		}
	}

	const uint32_t outstandingQueuedFrames = renderer.CountPotentialOutstandingQueuedFrames();
	if (sceneTextureSet)
	{
		renderer.mDescriptorCoherencyDebugStats.lastSceneTextureQueuedFrameIndex = queuedFrameIndex;
		renderer.mDescriptorCoherencyDebugStats.lastSceneTextureQueuedFrameFence = queuedFrameFence;
		renderer.mDescriptorCoherencyDebugStats.lastSceneTextureSubmittedFence = submittedFence;
		renderer.mDescriptorCoherencyDebugStats.lastSceneTextureOutstandingQueuedFrames = outstandingQueuedFrames;
	}
	else
	{
		renderer.mDescriptorCoherencyDebugStats.lastSceneDataQueuedFrameIndex = queuedFrameIndex;
		renderer.mDescriptorCoherencyDebugStats.lastSceneDataQueuedFrameFence = queuedFrameFence;
		renderer.mDescriptorCoherencyDebugStats.lastSceneDataSubmittedFence = submittedFence;
		renderer.mDescriptorCoherencyDebugStats.lastSceneDataOutstandingQueuedFrames = outstandingQueuedFrames;
	}

	if (!ShouldTraceDescriptorCoherency())
	{
		return;
	}

	Printf("NRI PT descriptor rewrite: frame=%u set=%s reason=%s hash=0x%llx descriptors=%u qframe=%u slot_fence=%llu submitted_fence=%llu outstanding_slots=%u\n",
		renderer.mFrameIndex,
		setName != nullptr ? setName : "unknown",
		reason != nullptr ? reason : "unlabeled",
		(unsigned long long)descriptorHash,
		descriptorCount,
		queuedFrameIndex,
		(unsigned long long)queuedFrameFence,
		(unsigned long long)submittedFence,
		outstandingQueuedFrames);
}
