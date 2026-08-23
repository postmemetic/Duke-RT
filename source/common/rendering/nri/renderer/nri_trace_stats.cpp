#include "nri_trace_stats.h"

#include <algorithm>
#include <cstring>
#include <vector>


namespace
{
	static constexpr uint32_t NRI_TRACE_SHADER_STATS_COUNTER_COUNT = NRI_TRACE_SHADER_STAT_COUNT;
}

uint64_t NRITraceShaderStatsFenceServices::GetRecordingCommandFenceValue() const
{
	return getRecordingCommandFenceValue != nullptr ? getRecordingCommandFenceValue(user) : 0;
}

bool NRITraceShaderStatsFenceServices::IsCommandFenceValueComplete(uint64_t fenceValue) const
{
	return fenceValue != 0 && isCommandFenceValueComplete != nullptr &&
		isCommandFenceValueComplete(user, fenceValue);
}

bool NRITraceShaderStatsFenceServices::IsCommandFenceValueAbandoned(uint64_t fenceValue) const
{
	return fenceValue != 0 && isCommandFenceValueAbandoned != nullptr &&
		isCommandFenceValueAbandoned(user, fenceValue);
}

bool NRITraceShaderStats::CreateBufferWithoutViewAtLocation(
	const NRIResourceServices& services,
	NRIBufferResource& resource,
	uint64_t size,
	uint32_t stride,
	nri::BufferUsageBits usage,
	nri::MemoryLocation memoryLocation)
{
	const NRIResourceContext& context = services.context;
	if (context.device == nullptr || context.core == nullptr)
	{
		return false;
	}

	services.DestroyBufferResource(resource);
	nri::BufferDesc desc = {};
	desc.size = size;
	desc.structureStride = stride;
	desc.usage = usage;
	if (context.core->CreateCommittedBuffer(*context.device, memoryLocation, 0.0f, desc, resource.buffer) != nri::Result::SUCCESS)
	{
		return false;
	}

	nri::MemoryDesc memoryDesc = {};
	context.core->GetBufferMemoryDesc(*resource.buffer, memoryLocation, memoryDesc);
	resource.size = desc.size;
	resource.memorySize = memoryDesc.size;
	resource.memoryLocation = memoryLocation;
	resource.usedSize = size;
	resource.stride = stride;
	return true;
}

bool NRITraceShaderStats::Ensure(const NRIResourceServices& services)
{
	const NRIResourceContext& context = services.context;
	if (context.device == nullptr || context.core == nullptr)
	{
		return false;
	}

	constexpr uint32_t kStride = sizeof(uint32_t);
	const uint64_t byteSize = (uint64_t)NRI_TRACE_SHADER_STATS_COUNTER_COUNT * kStride;
	if (mStatsBuffer.buffer == nullptr || mStatsBuffer.shaderView == nullptr)
	{
		services.DestroyBufferResource(mStatsBuffer);
		nri::BufferDesc desc = {};
		desc.size = byteSize;
		desc.structureStride = kStride;
		desc.usage = NRIResourceFlags(
			nri::BufferUsageBits::SHADER_RESOURCE_STORAGE,
			nri::BufferUsageBits::SHADER_RESOURCE);
		if (context.core->CreateCommittedBuffer(*context.device, nri::MemoryLocation::DEVICE, 0.0f, desc, mStatsBuffer.buffer) != nri::Result::SUCCESS)
		{
			return false;
		}

		nri::MemoryDesc memoryDesc = {};
		context.core->GetBufferMemoryDesc(*mStatsBuffer.buffer, nri::MemoryLocation::DEVICE, memoryDesc);
		mStatsBuffer.size = desc.size;
		mStatsBuffer.memorySize = memoryDesc.size;
		mStatsBuffer.memoryLocation = nri::MemoryLocation::DEVICE;
		mStatsBuffer.usedSize = byteSize;
		mStatsBuffer.stride = kStride;

		nri::BufferViewDesc viewDesc = {};
		viewDesc.buffer = mStatsBuffer.buffer;
		viewDesc.type = nri::BufferView::STORAGE_STRUCTURED_BUFFER;
		viewDesc.offset = 0;
		viewDesc.size = nri::WHOLE_SIZE;
		viewDesc.structureStride = kStride;
		if (context.core->CreateBufferView(viewDesc, mStatsBuffer.shaderView) != nri::Result::SUCCESS)
		{
			return false;
		}
	}

	for (ReadbackSlot& slot : mReadbackSlots)
	{
		if (slot.buffer.buffer == nullptr &&
			!CreateBufferWithoutViewAtLocation(
				services,
				slot.buffer,
				byteSize,
				kStride,
				nri::BufferUsageBits::NONE,
				nri::MemoryLocation::HOST_READBACK))
		{
			return false;
		}
	}

	if (mZeroBuffer.buffer == nullptr)
	{
		if (!CreateBufferWithoutViewAtLocation(
			services,
			mZeroBuffer,
			byteSize,
			kStride,
			nri::BufferUsageBits::NONE,
			nri::MemoryLocation::DEVICE_UPLOAD))
		{
			return false;
		}

		void* mapped = context.core->MapBuffer(*mZeroBuffer.buffer, 0, byteSize);
		if (mapped == nullptr)
		{
			return false;
		}
		std::memset(mapped, 0, (size_t)byteSize);
		context.core->UnmapBuffer(*mZeroBuffer.buffer);
	}

	return true;
}

void NRITraceShaderStats::Destroy(const NRIResourceServices& services)
{
	services.DestroyBufferResource(mStatsBuffer);
	services.DestroyBufferResource(mZeroBuffer);
	for (ReadbackSlot& slot : mReadbackSlots)
	{
		services.DestroyBufferResource(slot.buffer);
		slot = {};
	}
	mObserver = {};
	mNextCopySerial = 1;
	mReadbackConsumerFence = 0;
	mNextCopySlot = 0;
}

void NRITraceShaderStats::ResetBuffer(const NRIResourceServices& services, bool enabled)
{
	const NRIResourceContext& context = services.context;
	if (!enabled || context.commandBuffer == nullptr || context.core == nullptr || !Ensure(services))
	{
		return;
	}

	const uint64_t byteSize = (uint64_t)NRI_TRACE_SHADER_STATS_COUNTER_COUNT * sizeof(uint32_t);
	nri::BufferBarrierDesc beforeBarriers[2] = {};
	beforeBarriers[0].buffer = mZeroBuffer.buffer;
	beforeBarriers[0].before = {};
	beforeBarriers[0].after = NRIResourceCopySourceAccess();
	beforeBarriers[1].buffer = mStatsBuffer.buffer;
	beforeBarriers[1].before = {};
	beforeBarriers[1].after = NRIResourceCopyDestinationAccess();
	nri::BarrierDesc beforeDesc = {};
	beforeDesc.buffers = beforeBarriers;
	beforeDesc.bufferNum = 2;
	context.core->CmdBarrier(*context.commandBuffer, beforeDesc);
	context.core->CmdCopyBuffer(
		*context.commandBuffer,
		*mStatsBuffer.buffer,
		0,
		*mZeroBuffer.buffer,
		0,
		byteSize);

	nri::BufferBarrierDesc afterBarrier = {};
	afterBarrier.buffer = mStatsBuffer.buffer;
	afterBarrier.before = NRIResourceCopyDestinationAccess();
	afterBarrier.after = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
	nri::BarrierDesc afterDesc = {};
	afterDesc.buffers = &afterBarrier;
	afterDesc.bufferNum = 1;
	context.core->CmdBarrier(*context.commandBuffer, afterDesc);
}

void NRITraceShaderStats::ClearReadbackSlot(uint32_t slotIndex)
{
	if (slotIndex >= mReadbackSlots.size())
	{
		return;
	}
	ReadbackSlot& slot = mReadbackSlots[slotIndex];
	slot.attribution.clear();
	slot.frameNumber = 0;
	slot.fenceValue = 0;
	slot.copySerial = 0;
	slot.pending = false;
}

void NRITraceShaderStats::UpdateObserverSnapshot(NRITraceShaderStatsSnapshot& outStats) const
{
	outStats.observer = mObserver;
	outStats.observer.pendingReadbackCount = 0;
	for (const ReadbackSlot& slot : mReadbackSlots)
	{
		outStats.observer.pendingReadbackCount += slot.pending ? 1u : 0u;
	}
}

void NRITraceShaderStats::CopyForReadback(
	const NRIResourceServices& services,
	const NRITraceShaderStatsCopyInput& input)
{
	const NRIResourceContext& context = services.context;
	if (!input.enabled || context.commandBuffer == nullptr || context.core == nullptr)
	{
		return;
	}
	mObserver.copiesRequested++;

	const uint64_t fenceValue = input.fences.GetRecordingCommandFenceValue();
	if (fenceValue == 0)
	{
		mObserver.copiesDroppedNoFence++;
		return;
	}
	if (!Ensure(services))
	{
		return;
	}

	for (uint32_t slotIndex = 0; slotIndex < mReadbackSlots.size(); ++slotIndex)
	{
		ReadbackSlot& slot = mReadbackSlots[slotIndex];
		if (slot.pending && input.fences.IsCommandFenceValueAbandoned(slot.fenceValue))
		{
			ClearReadbackSlot(slotIndex);
			mObserver.readbacksAbandoned++;
		}
	}

	uint32_t selectedSlot = NRI_TRACE_SHADER_INVALID_READBACK_SLOT;
	for (uint32_t offset = 0; offset < mReadbackSlots.size(); ++offset)
	{
		const uint32_t slotIndex = (mNextCopySlot + offset) % (uint32_t)mReadbackSlots.size();
		if (!mReadbackSlots[slotIndex].pending)
		{
			selectedSlot = slotIndex;
			break;
		}
	}
	if (selectedSlot == NRI_TRACE_SHADER_INVALID_READBACK_SLOT)
	{
		mObserver.copiesDroppedBusy++;
		return;
	}

	ReadbackSlot& slot = mReadbackSlots[selectedSlot];
	slot.attribution.clear();
	if (input.boundSceneInstances != nullptr)
	{
		const uint32_t rowCount = std::min<uint32_t>(
			(uint32_t)input.boundSceneInstances->size(),
			NRI_TRACE_SHADER_INSTANCE_BUCKET_COUNT);
		slot.attribution.reserve(rowCount);
		for (uint32_t instanceId = 0; instanceId < rowCount; ++instanceId)
		{
			const SceneInstanceData& instance = (*input.boundSceneInstances)[instanceId];
			slot.attribution.push_back({
				instance.primitiveBase,
				0,
				instance.dataSource,
				instance.metadata0,
				instance.metadata1,
				instance.metadata2,
				DecodeNRIVoxelShadowProxyPrimitiveCount(instance.visibilityChunk)
			});
		}
		ResolveNRITraceShaderAttributionPrimitiveCounts(
			slot.attribution,
			{ input.staticPrimitiveCount, input.dynamicPrimitiveCount, input.persistentVoxelPrimitiveCount });
	}

	const uint64_t byteSize = (uint64_t)NRI_TRACE_SHADER_STATS_COUNTER_COUNT * sizeof(uint32_t);
	nri::BufferBarrierDesc beforeBarriers[2] = {};
	beforeBarriers[0].buffer = mStatsBuffer.buffer;
	beforeBarriers[0].before = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
	beforeBarriers[0].after = NRIResourceCopySourceAccess();
	beforeBarriers[1].buffer = slot.buffer.buffer;
	beforeBarriers[1].before = slot.initialized ? NRIResourceCopyDestinationAccess() : nri::AccessStage{};
	beforeBarriers[1].after = NRIResourceCopyDestinationAccess();
	nri::BarrierDesc beforeDesc = {};
	beforeDesc.buffers = beforeBarriers;
	beforeDesc.bufferNum = 2;
	context.core->CmdBarrier(*context.commandBuffer, beforeDesc);
	context.core->CmdCopyBuffer(
		*context.commandBuffer,
		*slot.buffer.buffer,
		0,
		*mStatsBuffer.buffer,
		0,
		byteSize);
	slot.frameNumber = input.frameNumber;
	slot.fenceValue = fenceValue;
	slot.copySerial = mNextCopySerial++;
	slot.pending = true;
	slot.initialized = true;
	mNextCopySlot = (selectedSlot + 1u) % (uint32_t)mReadbackSlots.size();
	mObserver.copiesRecorded++;
	mObserver.attributionRowsCopied += slot.attribution.size();
	mObserver.attributionBytesCopied += slot.attribution.size() * sizeof(NRITraceShaderInstanceAttribution);
}

void NRITraceShaderStats::Readback(
	const NRIResourceServices& services,
	bool enabled,
	const NRITraceShaderStatsFenceServices& fences,
	NRITraceShaderStatsSnapshot& outStats)
{
	const NRIResourceContext& context = services.context;
	const uint64_t consumerFence = fences.GetRecordingCommandFenceValue();
	if (consumerFence != mReadbackConsumerFence)
	{
		outStats.valid = false;
		mReadbackConsumerFence = consumerFence;
	}
	if (context.core == nullptr)
	{
		return;
	}

	std::array<NRITraceShaderReadbackSlotObservation, NRI_TRACE_SHADER_READBACK_SLOT_COUNT> observations = {};
	for (uint32_t slotIndex = 0; slotIndex < mReadbackSlots.size(); ++slotIndex)
	{
		const ReadbackSlot& slot = mReadbackSlots[slotIndex];
		observations[slotIndex].pending = slot.pending;
		observations[slotIndex].copySerial = slot.copySerial;
		if (slot.pending)
		{
			observations[slotIndex].fenceAbandoned = fences.IsCommandFenceValueAbandoned(slot.fenceValue);
			if (!observations[slotIndex].fenceAbandoned)
			{
				observations[slotIndex].fenceComplete = fences.IsCommandFenceValueComplete(slot.fenceValue);
			}
		}
	}

	const NRITraceShaderReadbackDecision decision = SelectNRITraceShaderReadback(
		observations.data(),
		(uint32_t)observations.size());
	for (uint32_t slotIndex = 0; slotIndex < observations.size(); ++slotIndex)
	{
		if (observations[slotIndex].pending && observations[slotIndex].fenceAbandoned)
		{
			ClearReadbackSlot(slotIndex);
			mObserver.readbacksAbandoned++;
		}
	}

	if (decision.newestReadySlot == NRI_TRACE_SHADER_INVALID_READBACK_SLOT)
	{
		UpdateObserverSnapshot(outStats);
		return;
	}
	for (uint32_t slotIndex = 0; slotIndex < observations.size(); ++slotIndex)
	{
		if (slotIndex != decision.newestReadySlot && observations[slotIndex].pending &&
			observations[slotIndex].fenceComplete && !observations[slotIndex].fenceAbandoned)
		{
			ClearReadbackSlot(slotIndex);
			mObserver.readbacksSuperseded++;
		}
	}

	ReadbackSlot& slot = mReadbackSlots[decision.newestReadySlot];
	if (!enabled)
	{
		ClearReadbackSlot(decision.newestReadySlot);
		mObserver.readbacksSuperseded++;
		UpdateObserverSnapshot(outStats);
		return;
	}

	const uint64_t byteSize = (uint64_t)NRI_TRACE_SHADER_STATS_COUNTER_COUNT * sizeof(uint32_t);
	const void* mapped = context.core->MapBuffer(*slot.buffer.buffer, 0, byteSize);
	if (mapped == nullptr)
	{
		ClearReadbackSlot(decision.newestReadySlot);
		mObserver.readbackMapFailures++;
		UpdateObserverSnapshot(outStats);
		return;
	}

	outStats.valid = true;
	outStats.frameNumber = slot.frameNumber;
	std::memcpy(outStats.counters.data(), mapped, (size_t)byteSize);
	outStats.hotInstanceCount = 0;
	outStats.hotInstances = {};
	outStats.targetPixelAttribution = {};
	outStats.referencePixelAttribution = {};
	outStats.candidateAttribution = {};
	outStats.finalAttribution = {};
	outStats.surfaceProbe = {};
	const auto resolveAttribution = [&](uint32_t instanceId, bool valid) -> NRITraceShaderProbeAttribution
	{
		NRITraceShaderProbeAttribution result;
		if (!valid || instanceId >= slot.attribution.size())
		{
			return result;
		}
		const NRITraceShaderInstanceAttribution& row = slot.attribution[instanceId];
		result.valid = true;
		result.instanceId = instanceId;
		result.dataSource = row.dataSource;
		result.metadata0 = row.metadata0;
		result.metadata1 = row.metadata1;
		result.metadata2 = row.metadata2;
		return result;
	};
	const uint32_t targetBase = NRI_TRACE_SHADER_PROBE_TARGET_PIXEL_BASE;
	const uint32_t referenceBase = NRI_TRACE_SHADER_PROBE_REFERENCE_PIXEL_BASE;
	outStats.targetPixelAttribution = resolveAttribution(
		outStats.counters[targetBase + 2u], outStats.counters[targetBase] != 0u);
	outStats.referencePixelAttribution = resolveAttribution(
		outStats.counters[referenceBase + 2u], outStats.counters[referenceBase] != 0u);
	for (uint32_t rayKind = 0; rayKind < NRI_TRACE_SHADER_RAY_KIND_COUNT; ++rayKind)
	{
		const uint32_t rayBase = NRI_TRACE_SHADER_PROBE_RAY_BASE + rayKind * NRI_TRACE_SHADER_PROBE_RAY_STRIDE;
		outStats.candidateAttribution[rayKind] = resolveAttribution(
			outStats.counters[rayBase + NRI_TRACE_SHADER_PROBE_RAY_CANDIDATE_INSTANCE],
			outStats.counters[rayBase + NRI_TRACE_SHADER_PROBE_RAY_CANDIDATE_CLAIM] != 0u);
		outStats.finalAttribution[rayKind] = resolveAttribution(
			outStats.counters[rayBase + NRI_TRACE_SHADER_PROBE_RAY_FINAL_INSTANCE],
			outStats.counters[rayBase + NRI_TRACE_SHADER_PROBE_RAY_FINAL_VALID] != 0u);
	}
	const uint32_t surfaceProbeBase = NRI_TRACE_SHADER_SURFACE_PROBE_BASE;
	outStats.surfaceProbe.valid = outStats.counters[surfaceProbeBase] != 0u;
	if (outStats.surfaceProbe.valid)
	{
		outStats.surfaceProbe.dataSource = outStats.counters[surfaceProbeBase + 1u];
		outStats.surfaceProbe.instanceId = outStats.counters[surfaceProbeBase + 2u];
		outStats.surfaceProbe.primitiveIndex = outStats.counters[surfaceProbeBase + 3u];
		outStats.surfaceProbe.materialIndex = outStats.counters[surfaceProbeBase + 4u];
		outStats.surfaceProbe.textureIndex = outStats.counters[surfaceProbeBase + 5u];
		outStats.surfaceProbe.paletteIndex = outStats.counters[surfaceProbeBase + 6u];
		outStats.surfaceProbe.flags = outStats.counters[surfaceProbeBase + 7u];
		outStats.surfaceProbe.lightingFlags = outStats.counters[surfaceProbeBase + 8u];
	}

	struct TraceShaderHotCandidate
	{
		uint32_t instanceId = 0;
		uint32_t committed = 0;
		uint32_t accepted = 0;
	};

	std::vector<TraceShaderHotCandidate> hotCandidates;
	const uint32_t instanceBucketCount = std::min<uint32_t>((uint32_t)slot.attribution.size(), NRI_TRACE_SHADER_INSTANCE_BUCKET_COUNT);
	hotCandidates.reserve(instanceBucketCount);
	for (uint32_t instanceId = 0; instanceId < instanceBucketCount; ++instanceId)
	{
		const uint32_t committed = outStats.counters[NRI_TRACE_SHADER_INSTANCE_COMMITTED_BASE + instanceId];
		const uint32_t accepted = outStats.counters[NRI_TRACE_SHADER_INSTANCE_ACCEPTED_BASE + instanceId];
		if (committed == 0 && accepted == 0)
		{
			continue;
		}
		hotCandidates.push_back({ instanceId, committed, accepted });
	}
	std::sort(
		hotCandidates.begin(),
		hotCandidates.end(),
		[](const TraceShaderHotCandidate& a, const TraceShaderHotCandidate& b)
		{
			if (a.committed != b.committed)
			{
				return a.committed > b.committed;
			}
			return a.accepted > b.accepted;
		});

	const uint32_t hotCount = std::min<uint32_t>((uint32_t)hotCandidates.size(), NRI_TRACE_SHADER_HOT_INSTANCE_COUNT);
	outStats.hotInstanceCount = hotCount;
	for (uint32_t hotIndex = 0; hotIndex < hotCount; ++hotIndex)
	{
		const TraceShaderHotCandidate& candidate = hotCandidates[hotIndex];
		const NRITraceShaderInstanceAttribution& instance = slot.attribution[candidate.instanceId];
		NRITraceShaderHotInstance& hot = outStats.hotInstances[hotIndex];
		hot.instanceId = candidate.instanceId;
		hot.dataSource = instance.dataSource;
		hot.primitiveOffset = instance.primitiveOffset;
		hot.primitiveCount = instance.primitiveCount;
		hot.metadata0 = instance.metadata0;
		hot.metadata1 = instance.metadata1;
		hot.metadata2 = instance.metadata2;
		hot.shadowProxy = instance.explicitPrimitiveCount != 0u;
		hot.committed = candidate.committed;
		hot.accepted = candidate.accepted;
		hot.primaryCommitted = outStats.counters[NRI_TRACE_SHADER_INSTANCE_KIND_COMMITTED_BASE + 0u * NRI_TRACE_SHADER_INSTANCE_BUCKET_COUNT + candidate.instanceId];
		hot.ungatedCommitted = outStats.counters[NRI_TRACE_SHADER_INSTANCE_KIND_COMMITTED_BASE + 1u * NRI_TRACE_SHADER_INSTANCE_BUCKET_COUNT + candidate.instanceId];
		hot.sunCommitted = outStats.counters[NRI_TRACE_SHADER_INSTANCE_KIND_COMMITTED_BASE + 2u * NRI_TRACE_SHADER_INSTANCE_BUCKET_COUNT + candidate.instanceId];
		hot.pointCommitted = outStats.counters[NRI_TRACE_SHADER_INSTANCE_KIND_COMMITTED_BASE + 3u * NRI_TRACE_SHADER_INSTANCE_BUCKET_COUNT + candidate.instanceId];
		hot.emissiveCommitted = outStats.counters[NRI_TRACE_SHADER_INSTANCE_KIND_COMMITTED_BASE + 4u * NRI_TRACE_SHADER_INSTANCE_BUCKET_COUNT + candidate.instanceId];
		hot.fastEmissiveCommitted = outStats.counters[NRI_TRACE_SHADER_INSTANCE_KIND_COMMITTED_BASE + 5u * NRI_TRACE_SHADER_INSTANCE_BUCKET_COUNT + candidate.instanceId];
	}
	context.core->UnmapBuffer(*slot.buffer.buffer);
	ClearReadbackSlot(decision.newestReadySlot);
	mObserver.readbacksPublished++;
	UpdateObserverSnapshot(outStats);
}
