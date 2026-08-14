#include "nri_scene_upload.h"
#include "nri_cvars.h"

#include "nri_descriptor_sets.h"
#include "nri_emissive_sampling_upload_policy.h"
#include "nri_renderer.h"
#include "nri_runtime_mutation_trace.h"
#include "nri_scene_lights.h"
#include "printf.h"
#include "perf_capture.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

namespace
{
	static double DurationMs(const std::chrono::steady_clock::time_point& start, const std::chrono::steady_clock::time_point& end)
	{
		return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start).count();
	}

	static bool ShouldCollectSceneDataTiming()
	{
		return (int)nri_pttraceframes > 0 || (int)perf_looptraceframes > 0 || (bool)nri_ptslowdowntrace || PerfCompactCaptureTimingActive();
	}

	class ScopedPtPerfTimer
	{
	public:
		explicit ScopedPtPerfTimer(double& targetMs)
			: mTarget(ShouldCollectSceneDataTiming() ? &targetMs : nullptr)
		{
			if (mTarget != nullptr)
			{
				mStart = std::chrono::steady_clock::now();
			}
		}

		~ScopedPtPerfTimer()
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

	static NRIEmissiveSamplingUploadResourceInput BuildUploadResourceInput(
		const NRIBufferResource& resource,
		uint64_t payloadBytes,
		uint32_t payloadStride)
	{
		NRIEmissiveSamplingUploadResourceInput input = {};
		input.hasBuffer = resource.buffer != nullptr;
		input.hasShaderView = resource.shaderView != nullptr;
		input.capacityBytes = resource.size;
		input.currentStride = resource.stride;
		input.payloadBytes = payloadBytes;
		input.payloadStride = payloadStride;
		return input;
	}

	static uint64_t HashEmissiveStabilityValue(uint64_t hash, uint64_t value)
	{
		return hash ^ (value + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u));
	}

	static uint32_t EmissiveStabilityFloatBits(float value)
	{
		uint32_t bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		return bits;
	}

	static uint64_t HashEmissiveStabilityKeys(const std::vector<uint64_t>& keys)
	{
		uint64_t hash = 1469598103934665603ull;
		for (uint64_t key : keys)
		{
			hash = HashEmissiveStabilityValue(hash, key);
		}
		return hash;
	}

	static uint64_t SelectEmissiveStabilityKey(
		const std::vector<uint64_t>& keys,
		const std::vector<float>& cdf,
		float value)
	{
		if (keys.empty() || cdf.empty())
		{
			return 0;
		}

		const auto it = std::lower_bound(cdf.begin(), cdf.end(), value);
		const size_t index = std::min((size_t)std::distance(cdf.begin(), it), keys.size() - 1u);
		return keys[index];
	}
}
bool NRIRenderer::UpdateEmissiveSamplingBuffers(
	const EmissiveSamplingBuildContext& context,
	bool* ioWaitedForWrites,
	bool allowSceneDataFrameSlot)
{
	ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.emissiveUpdateMs);
	const uint64_t payloadHash = mSceneLights.BuildEmissiveSamplingPayloadHash(context);
	const uint64_t sectorResponsePayloadHash = mSceneLights.BuildEmissiveSectorResponsePayloadHash();
	const bool sectorResponseChanged =
		mEmissiveSectorResponsePayloadCacheValid &&
		mEmissiveSectorResponsePayloadHash != sectorResponsePayloadHash;
	const bool stabilityTraceEnabled = (bool)nri_ptemissivestabilitytrace;
	if (!stabilityTraceEnabled)
	{
		mEmissiveStabilityTraceValid = false;
		mEmissiveStabilityOrderedKeys.clear();
		mEmissiveStabilityCdf.clear();
	}
	NRISceneDataFrameSlot* frameSlot =
		allowSceneDataFrameSlot && ShouldUseSceneDataFrameRing() ? &GetCurrentSceneDataFrameSlot() : nullptr;
	NRIBufferResource& emissivePrimitiveHeaderBuffer =
		frameSlot != nullptr ? frameSlot->emissivePrimitiveHeaderBuffer : mEmissivePrimitiveHeaderBuffer;
	NRIBufferResource& emissivePrimitiveBuffer =
		frameSlot != nullptr ? frameSlot->emissivePrimitiveBuffer : mEmissivePrimitiveBuffer;
	NRIBufferResource& emissivePrimitiveCdfBuffer =
		frameSlot != nullptr ? frameSlot->emissivePrimitiveCdfBuffer : mEmissivePrimitiveCdfBuffer;
	NRIBufferResource& emissiveMaterialResponseBuffer =
		frameSlot != nullptr ? frameSlot->emissiveMaterialResponseBuffer : mEmissiveMaterialResponseBuffer;
	SceneBufferDebugStats& emissivePrimitiveHeaderStats =
		frameSlot != nullptr ? frameSlot->emissivePrimitiveHeaderStats : mEmissivePrimitiveHeaderBufferStats;
	SceneBufferDebugStats& emissivePrimitiveStats =
		frameSlot != nullptr ? frameSlot->emissivePrimitiveStats : mEmissivePrimitiveBufferStats;
	SceneBufferDebugStats& emissivePrimitiveCdfStats =
		frameSlot != nullptr ? frameSlot->emissivePrimitiveCdfStats : mEmissivePrimitiveCdfBufferStats;
	SceneBufferDebugStats& emissiveMaterialResponseStats =
		frameSlot != nullptr ? frameSlot->emissiveMaterialResponseStats : mEmissiveMaterialResponseBufferStats;
	const bool destinationCacheValid =
		frameSlot != nullptr ?
			frameSlot->emissiveSamplingPayloadValid && frameSlot->emissiveSamplingPayloadHash == payloadHash :
			mEmissiveSamplingPayloadCacheValid && mEmissiveSamplingPayloadHash == payloadHash;
	NRIEmissivePrimitiveHeaderGpuData emissiveHeader = {};
	std::vector<NRIEmissivePrimitiveGpuData> emissivePrimitives;
	std::vector<NRIEmissivePrimitiveShaderData> emissiveShaderPrimitives;
	std::vector<float> emissiveCdf;
	std::vector<NRIEmissiveMaterialResponseGpuData> emissiveMaterialResponses;
	std::vector<NRIEmissivePrimitiveDebugRecord> emissiveDebugRecords;
	SceneLightSystem::EmissiveSamplingUploadStats emissiveStats = {};
	if (stabilityTraceEnabled)
	{
		mSceneLights.BuildEmissiveSamplingUpload(context, emissiveHeader, emissivePrimitives, emissiveCdf, emissiveMaterialResponses, emissiveDebugRecords, &emissiveStats);
	}

	const auto commitEmissiveDescriptors = [&]()
	{
		mSceneDataDescriptors[13] = emissivePrimitiveHeaderBuffer.shaderView;
		mSceneDataDescriptors[14] = emissivePrimitiveBuffer.shaderView;
		mSceneDataDescriptors[15] = emissivePrimitiveCdfBuffer.shaderView;
		mSceneDataDescriptors[25] = emissiveMaterialResponseBuffer.shaderView;

		bool descriptorsReady = IsCurrentSceneDataDescriptorsInitialized() && GetCurrentSceneDataSet() != nullptr;
		if (descriptorsReady)
		{
			for (const nri::Descriptor* descriptor : mSceneDataDescriptors)
			{
				if (descriptor == nullptr)
				{
					descriptorsReady = false;
					break;
				}
			}
		}

		if (descriptorsReady)
		{
			NRIDescriptorSetManager::CommitSceneDataDescriptors(*this, "emissive_sampling_refresh");
		}
	};

	const auto emitStabilityTrace = [&](uint32_t uploadWaitCount)
	{
		if (!stabilityTraceEnabled)
		{
			return;
		}

		std::vector<uint64_t> orderedKeys;
		orderedKeys.reserve(emissiveDebugRecords.size());
		uint64_t livePowerHash = 1469598103934665603ull;
		uint64_t proposalWeightHash = 1469598103934665603ull;
		uint64_t cdfHash = 1469598103934665603ull;
		for (size_t i = 0; i < emissiveDebugRecords.size(); ++i)
		{
			const uint64_t stableKey = emissiveDebugRecords[i].stableKey;
			orderedKeys.push_back(stableKey);
			livePowerHash = HashEmissiveStabilityValue(livePowerHash, stableKey);
			livePowerHash = HashEmissiveStabilityValue(livePowerHash, EmissiveStabilityFloatBits(emissivePrimitives[i].powerEstimate));
			livePowerHash = HashEmissiveStabilityValue(livePowerHash, EmissiveStabilityFloatBits(emissivePrimitives[i].emissionScale));
			livePowerHash = HashEmissiveStabilityValue(livePowerHash, EmissiveStabilityFloatBits(emissivePrimitives[i].materialResponseScale));
			proposalWeightHash = HashEmissiveStabilityValue(proposalWeightHash, stableKey);
			proposalWeightHash = HashEmissiveStabilityValue(proposalWeightHash, EmissiveStabilityFloatBits(emissivePrimitives[i].selectionWeight));
		}
		for (float cdfValue : emissiveCdf)
		{
			cdfHash = HashEmissiveStabilityValue(cdfHash, EmissiveStabilityFloatBits(cdfValue));
		}

		std::vector<uint64_t> topologyKeys = orderedKeys;
		std::sort(topologyKeys.begin(), topologyKeys.end());
		const uint64_t topologyHash = HashEmissiveStabilityKeys(topologyKeys);
		const uint64_t orderedKeyHash = HashEmissiveStabilityKeys(orderedKeys);
		const bool topologyChanged = !mEmissiveStabilityTraceValid || topologyHash != mEmissiveStabilityTopologyHash;
		const bool propertiesChanged = !mEmissiveStabilityTraceValid || livePowerHash != mEmissiveStabilityLivePowerHash;
		const bool proposalChanged =
			!mEmissiveStabilityTraceValid ||
			orderedKeyHash != mEmissiveStabilityOrderedKeyHash ||
			proposalWeightHash != mEmissiveStabilityProposalWeightHash ||
			cdfHash != mEmissiveStabilityCdfHash;
		if (topologyChanged)
		{
			mEmissiveStabilityTopologyEpoch++;
		}
		if (proposalChanged)
		{
			mEmissiveStabilityDistributionEpoch++;
		}

		float cdfMaxDelta = 0.0f;
		uint32_t remapCount = 0;
		constexpr uint32_t FixedGridSampleCount = 4096u;
		if (mEmissiveStabilityTraceValid)
		{
			if (emissiveCdf.size() != mEmissiveStabilityCdf.size())
			{
				cdfMaxDelta = 1.0f;
			}
			else
			{
				for (size_t i = 0; i < emissiveCdf.size(); ++i)
				{
					cdfMaxDelta = std::max(cdfMaxDelta, std::fabs(emissiveCdf[i] - mEmissiveStabilityCdf[i]));
				}
			}
			for (uint32_t i = 0; i < FixedGridSampleCount; ++i)
			{
				const float value = ((float)i + 0.5f) / (float)FixedGridSampleCount;
				if (SelectEmissiveStabilityKey(orderedKeys, emissiveCdf, value) !=
					SelectEmissiveStabilityKey(mEmissiveStabilityOrderedKeys, mEmissiveStabilityCdf, value))
				{
					remapCount++;
				}
			}
		}

		Printf("PERF pt emissive stability NRI: frame=%u topology_epoch=%llu distribution_epoch=%llu candidate_count=%u active_count=%u retained_dark_count=%u reactivated=%u retired_missing=%u retired_replaced=%u proposal_record_count=%u topology_key_hash=0x%016llx ordered_key_hash=0x%016llx live_power_hash=0x%016llx proposal_weight_hash=0x%016llx cdf_hash=0x%016llx cdf_max_delta=%.9g fixed_grid_remap_count=%u fixed_grid_sample_count=%u topology_changed=%u properties_changed=%u proposal_changed=%u upload_wait=%u frame_gap_ms=%.3f reset_history=%u reset_reason=%s\n",
			mFrameIndex,
			(unsigned long long)mEmissiveStabilityTopologyEpoch,
			(unsigned long long)mEmissiveStabilityDistributionEpoch,
			(uint32_t)orderedKeys.size(),
			emissiveStats.proposalActiveCount,
			emissiveStats.proposalRetainedDarkCount,
			emissiveStats.proposalReactivatedCount,
			emissiveStats.proposalRetiredMissingCount,
			emissiveStats.proposalRetiredReplacedCount,
			emissiveStats.proposalRecordCount,
			(unsigned long long)topologyHash,
			(unsigned long long)orderedKeyHash,
			(unsigned long long)livePowerHash,
			(unsigned long long)proposalWeightHash,
			(unsigned long long)cdfHash,
			cdfMaxDelta,
			remapCount,
			FixedGridSampleCount,
			topologyChanged ? 1u : 0u,
			propertiesChanged ? 1u : 0u,
			proposalChanged ? 1u : 0u,
			uploadWaitCount,
			mHasPendingFrameGenerationRealFrameTime ? mPendingFrameGenerationRealFrameTimeMs : 0.0f,
			mResetHistory ? 1u : 0u,
			mResetHistory ? mLastHistoryResetReason.c_str() : "none");

		mEmissiveStabilityTraceValid = true;
		mEmissiveStabilityTopologyHash = topologyHash;
		mEmissiveStabilityOrderedKeyHash = orderedKeyHash;
		mEmissiveStabilityLivePowerHash = livePowerHash;
		mEmissiveStabilityProposalWeightHash = proposalWeightHash;
		mEmissiveStabilityCdfHash = cdfHash;
		mEmissiveStabilityOrderedKeys = std::move(orderedKeys);
		mEmissiveStabilityCdf = emissiveCdf;
	};

	if (destinationCacheValid &&
		emissivePrimitiveHeaderBuffer.shaderView != nullptr &&
		emissivePrimitiveBuffer.shaderView != nullptr &&
		emissivePrimitiveCdfBuffer.shaderView != nullptr &&
		emissiveMaterialResponseBuffer.shaderView != nullptr)
	{
		commitEmissiveDescriptors();
		if (frameSlot != nullptr)
		{
			mBoundEmissivePrimitiveCount = frameSlot->emissivePrimitiveCount;
			mBoundEmissiveDominantPrimitive = frameSlot->emissiveDominantPrimitive;
			mBoundEmissiveDominantTile = frameSlot->emissiveDominantTile;
			mBoundEmissiveDominantFlags = frameSlot->emissiveDominantFlags;
			mBoundEmissiveDominantDataSource = frameSlot->emissiveDominantDataSource;
			mBoundEmissiveTotalPower = frameSlot->emissiveTotalPower;
			mBoundEmissiveDominantPower = frameSlot->emissiveDominantPower;
			mBoundEmissivePrimitiveRecords = frameSlot->emissivePrimitiveDebugRecords;
			mEmissiveSamplingPayloadCacheValid = false;
			mEmissiveSamplingPayloadHash = 0;
		}
		mEmissiveSectorResponsePayloadCacheValid = true;
		mEmissiveSectorResponsePayloadHash = sectorResponsePayloadHash;
		emitStabilityTrace(0);
		return true;
	}

	if (!stabilityTraceEnabled)
	{
		mSceneLights.BuildEmissiveSamplingUpload(context, emissiveHeader, emissivePrimitives, emissiveCdf, emissiveMaterialResponses, emissiveDebugRecords, &emissiveStats);
	}
	emissiveShaderPrimitives.reserve(emissivePrimitives.size());
	for (const NRIEmissivePrimitiveGpuData& primitive : emissivePrimitives)
	{
		emissiveShaderPrimitives.push_back(PackNRIEmissivePrimitiveShaderData(primitive));
	}
	mLastPerfShellTraceStats.emissiveSamplingSurfaceStatic = emissiveStats.surfaceStatic;
	mLastPerfShellTraceStats.emissiveSamplingSurfaceCaptured = emissiveStats.surfaceCaptured;
	mLastPerfShellTraceStats.emissiveSamplingSurfaceRuntimeMutation = emissiveStats.surfaceRuntimeMutation;
	mLastPerfShellTraceStats.emissiveSamplingSurfaceDynamic = emissiveStats.surfaceDynamic + emissiveStats.surfaceLightOverlay;
	mLastPerfShellTraceStats.emissiveSamplingSurfacePersistentVoxel = emissiveStats.surfacePersistentVoxel;
	mLastPerfShellTraceStats.emissiveSamplingOutputStaticRecords = emissiveStats.outputStaticRecords;
	mLastPerfShellTraceStats.emissiveSamplingOutputDynamicRecords = emissiveStats.outputDynamicRecords;
	mLastPerfShellTraceStats.emissiveSamplingOutputPersistentVoxelRecords = emissiveStats.outputPersistentVoxelRecords;
	mLastPerfShellTraceStats.emissiveSamplingSkippedPersistentVoxelSurfaces = emissiveStats.skippedPersistentVoxelSurfaces;
	if (emissiveStats.proposalBoundGrowthCount > 0 && stabilityTraceEnabled)
	{
		Printf("NRI PT emissive proposal bound growth: frame=%u count=%u stable_key=0x%016llx old=%.9g new=%.9g source=%s reason=observed-above-bound\n",
			mFrameIndex,
			emissiveStats.proposalBoundGrowthCount,
			(unsigned long long)emissiveStats.lastProposalBoundGrowthStableKey,
			emissiveStats.lastProposalBoundGrowthOldWeight,
			emissiveStats.lastProposalBoundGrowthNewWeight,
			emissiveStats.lastProposalBoundGrowthWasAuthored ? "authored" : "observed");
	}

	const uint64_t headerBytes = sizeof(emissiveHeader);
	const uint64_t primitiveBytes = emissiveShaderPrimitives.size() * sizeof(NRIEmissivePrimitiveShaderData);
	const uint64_t cdfBytes = emissiveCdf.size() * sizeof(float);
	const uint64_t materialResponseBytes = emissiveMaterialResponses.size() * sizeof(NRIEmissiveMaterialResponseGpuData);
	const NRIEmissiveSamplingUploadResourceInput uploadInputs[] = {
		BuildUploadResourceInput(emissivePrimitiveHeaderBuffer, headerBytes, sizeof(NRIEmissivePrimitiveHeaderGpuData)),
		BuildUploadResourceInput(emissivePrimitiveBuffer, primitiveBytes, sizeof(NRIEmissivePrimitiveShaderData)),
		BuildUploadResourceInput(emissivePrimitiveCdfBuffer, cdfBytes, sizeof(float)),
		BuildUploadResourceInput(emissiveMaterialResponseBuffer, materialResponseBytes, sizeof(NRIEmissiveMaterialResponseGpuData)),
	};
	bool writesQuiesced = frameSlot != nullptr || (ioWaitedForWrites != nullptr && *ioWaitedForWrites);
	const NRIEmissiveSamplingUploadBatchDecision uploadDecision = NRIPlanEmissiveSamplingUploadBatch(
		uploadInputs,
		sizeof(uploadInputs) / sizeof(uploadInputs[0]),
		frameSlot != nullptr,
		writesQuiesced);
	uint32_t waitCount = 0;
	if (uploadDecision.waitRequired)
	{
		WaitForCommandsTracked("emissive_sampling_upload");
		writesQuiesced = true;
		waitCount = 1;
		if (ioWaitedForWrites != nullptr)
		{
			*ioWaitedForWrites = true;
		}
	}

	const auto ensureStructuredBufferBatched = [this, writesQuiesced](NRIBufferResource& resource, SceneBufferDebugStats& stats, const void* data, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after) -> bool
	{
		return EnsureStructuredBuffer(resource, stats, data, size, stride, usage, after, writesQuiesced, "emissive_sampling_upload");
	};

	if (!ensureStructuredBufferBatched(
		emissivePrimitiveHeaderBuffer,
		emissivePrimitiveHeaderStats,
		&emissiveHeader,
		sizeof(emissiveHeader),
		sizeof(NRIEmissivePrimitiveHeaderGpuData),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIResourceComputeShaderResourceAccess()))
	{
		return false;
	}

	if (!ensureStructuredBufferBatched(
		emissivePrimitiveBuffer,
		emissivePrimitiveStats,
		emissiveShaderPrimitives.empty() ? nullptr : emissiveShaderPrimitives.data(),
		emissiveShaderPrimitives.empty() ? 0u : emissiveShaderPrimitives.size() * sizeof(NRIEmissivePrimitiveShaderData),
		sizeof(NRIEmissivePrimitiveShaderData),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIResourceComputeShaderResourceAccess()))
	{
		return false;
	}

	if (!ensureStructuredBufferBatched(
		emissivePrimitiveCdfBuffer,
		emissivePrimitiveCdfStats,
		emissiveCdf.data(),
		emissiveCdf.size() * sizeof(float),
		sizeof(float),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIResourceComputeShaderResourceAccess()))
	{
		return false;
	}

	if (!ensureStructuredBufferBatched(
		emissiveMaterialResponseBuffer,
		emissiveMaterialResponseStats,
		emissiveMaterialResponses.empty() ? nullptr : emissiveMaterialResponses.data(),
		emissiveMaterialResponses.empty() ? 0u : emissiveMaterialResponses.size() * sizeof(NRIEmissiveMaterialResponseGpuData),
		sizeof(NRIEmissiveMaterialResponseGpuData),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIResourceComputeShaderResourceAccess()))
	{
		return false;
	}

	mBoundEmissivePrimitiveCount = emissiveHeader.activeCount;
	mBoundEmissiveTotalPower = emissiveHeader.totalPower;
	mBoundEmissiveDominantPrimitive = emissiveHeader.dominantIndex != UINT32_MAX && emissiveHeader.dominantIndex < emissiveDebugRecords.size() ? emissiveDebugRecords[emissiveHeader.dominantIndex].primitiveIndex : UINT32_MAX;
	mBoundEmissiveDominantTile = emissiveHeader.dominantIndex != UINT32_MAX && emissiveHeader.dominantIndex < emissivePrimitives.size() ? emissivePrimitives[emissiveHeader.dominantIndex].textureId : 0u;
	mBoundEmissiveDominantFlags = emissiveHeader.dominantIndex != UINT32_MAX && emissiveHeader.dominantIndex < emissivePrimitives.size() ? emissivePrimitives[emissiveHeader.dominantIndex].sourceFlags : 0u;
	mBoundEmissiveDominantDataSource = emissiveHeader.dominantIndex != UINT32_MAX && emissiveHeader.dominantIndex < emissivePrimitives.size() ? emissivePrimitives[emissiveHeader.dominantIndex].dataSource : 0u;
	mBoundEmissiveDominantPower = emissiveHeader.dominantIndex != UINT32_MAX && emissiveHeader.dominantIndex < emissivePrimitives.size() ? emissivePrimitives[emissiveHeader.dominantIndex].powerEstimate : 0.0f;
	emitStabilityTrace(waitCount);
	mBoundEmissivePrimitiveRecords = std::move(emissiveDebugRecords);

	commitEmissiveDescriptors();
	if (frameSlot != nullptr)
	{
		frameSlot->emissiveSamplingPayloadValid = true;
		frameSlot->emissiveSamplingPayloadHash = payloadHash;
		frameSlot->emissivePrimitiveCount = mBoundEmissivePrimitiveCount;
		frameSlot->emissiveDominantPrimitive = mBoundEmissiveDominantPrimitive;
		frameSlot->emissiveDominantTile = mBoundEmissiveDominantTile;
		frameSlot->emissiveDominantFlags = mBoundEmissiveDominantFlags;
		frameSlot->emissiveDominantDataSource = mBoundEmissiveDominantDataSource;
		frameSlot->emissiveTotalPower = mBoundEmissiveTotalPower;
		frameSlot->emissiveDominantPower = mBoundEmissiveDominantPower;
		frameSlot->emissivePrimitiveDebugRecords = mBoundEmissivePrimitiveRecords;
	}

	if (ShouldCollectSceneDataTiming())
	{
		const uint32_t growCount =
			emissivePrimitiveHeaderStats.growEventsLastFrame +
			emissivePrimitiveStats.growEventsLastFrame +
			emissivePrimitiveCdfStats.growEventsLastFrame +
			emissiveMaterialResponseStats.growEventsLastFrame;
		const uint32_t replaceCount =
			emissivePrimitiveHeaderStats.overwriteEventsLastFrame +
			emissivePrimitiveStats.overwriteEventsLastFrame +
			emissivePrimitiveCdfStats.overwriteEventsLastFrame +
			emissiveMaterialResponseStats.overwriteEventsLastFrame;
		const uint64_t payloadBytes = headerBytes + primitiveBytes + cdfBytes + materialResponseBytes;
		Printf("NRI PT emissive sampling upload: frame=%u destination=%s slot=%u payload_hash=0x%016llx bytes=%llu grow=%u replace=%u wait=%u wait_reason=%s source=static:%u,captured:%u,mutation:%u,dynamic:%u,persistent_voxel:%u output=static:%u,dynamic:%u,persistent_voxel:%u persistent_primitives_represented=%llu skipped_persistent=%u\n",
			mFrameIndex,
			frameSlot != nullptr ? "frame-slot" : "shared",
			frameSlot != nullptr ? GetCurrentQueuedFrameIndex() : UINT32_MAX,
			(unsigned long long)payloadHash,
			(unsigned long long)payloadBytes,
			growCount,
			replaceCount,
			waitCount,
			waitCount != 0 ? "shared-in-flight" : "none",
			emissiveStats.surfaceStatic,
			emissiveStats.surfaceCaptured,
			emissiveStats.surfaceRuntimeMutation,
			emissiveStats.surfaceDynamic + emissiveStats.surfaceLightOverlay,
			emissiveStats.surfacePersistentVoxel,
			emissiveStats.outputStaticRecords,
			emissiveStats.outputDynamicRecords,
			emissiveStats.outputPersistentVoxelRecords,
			(unsigned long long)emissiveStats.outputPersistentVoxelPrimitivesRepresented,
			emissiveStats.skippedPersistentVoxelSurfaces);
	}
	if (sectorResponseChanged && nri_runtime_mutation::ShouldTracePtPerf())
	{
		const auto& sectorRegistry = mSceneLights.GetSectorLighting();
		Printf("NRI PT emissive sampling refresh: frame=%u reason=sector-response-change primitives=%u total_power=%.3f dominant_primitive=%u dominant_tile=%u sector_response_hash=0x%016llx->0x%016llx response=boost:%u dim:%u neutral:%u\n",
			mFrameIndex,
			mBoundEmissivePrimitiveCount,
			mBoundEmissiveTotalPower,
			mBoundEmissiveDominantPrimitive,
			mBoundEmissiveDominantTile,
			(unsigned long long)mEmissiveSectorResponsePayloadHash,
			(unsigned long long)sectorResponsePayloadHash,
			sectorRegistry.responseBoostSectorCount,
			sectorRegistry.responseDimSectorCount,
			sectorRegistry.responseNeutralSectorCount);
	}
	mEmissiveSamplingPayloadCacheValid = frameSlot == nullptr;
	mEmissiveSamplingPayloadHash = frameSlot == nullptr ? payloadHash : 0;
	mEmissiveSectorResponsePayloadCacheValid = true;
	mEmissiveSectorResponsePayloadHash = sectorResponsePayloadHash;
	return true;
}
