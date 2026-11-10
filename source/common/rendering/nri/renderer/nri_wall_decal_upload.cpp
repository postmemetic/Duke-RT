#include "nri_scene_upload.h"
#include "nri_wall_decal_audit.h"

#include "nri_renderer.h"
#include "nri_cvars.h"
#include "nri_shader_contracts.h"
#include "c_cvars.h"

#include <algorithm>
#include <cstring>

bool NRISceneUploadManager::UpdateWallDecalBuffers(
	NRIRenderer& renderer, bool* ioWaitedForWrites, bool allowSceneDataRing)
{
	PerfAuditScope auditUpload(PerfAuditMetric::WallDecalUploadNs);
	static thread_local NRIWallDecalAuditCall previousUpload;
	const bool audit = NoteNRIWallDecalAuditCall(&renderer, previousUpload,
		PerfAuditMetric::WallDecalUploadCalls, PerfAuditMetric::WallDecalUploadRepeatedCalls);
	const nri_scene::WallDecalHeaderGpuData emptyHeader = {};
	const nri_scene::WallDecalGpuData emptyDecal = {};
	const auto& headers = renderer.mWallDecals.Headers();
	const auto& decals = renderer.mWallDecals.Records();
	if ((int)nri_pttraceframes > 0 && renderer.mWallDecalTraceFrameIndex != renderer.mFrameIndex)
	{
		renderer.mWallDecalTraceFrameIndex = renderer.mFrameIndex;
		uint32_t resolvedCount = 0;
		for (const auto& decal : decals)
			resolvedCount += decal.textureIndex < NRI_MAX_SCENE_TEXTURES ? 1u : 0u;
		Printf("NRI PT wall decals: frame=%u walls=%u records=%u resolved=%u\n",
			renderer.mFrameIndex, (uint32_t)headers.size(), (uint32_t)decals.size(), resolvedCount);
		for (size_t i = 0; i < std::min<size_t>(decals.size(), 16u); ++i)
		{
			const auto& decal = decals[i];
			Printf("NRI PT wall decal: frame=%u record=%u actor=%u wall=%u sector=%u texture=%u palette=%u alpha=%.3f flags=%u plane=(%.3f,%.3f,%.3f,%.3f) uv=(%.3f,%.3f,%.3f,%.3f)\n",
				renderer.mFrameIndex, (uint32_t)i, decal.actorIndex, decal.wallIndex, decal.sectorIndex,
				decal.textureIndex, decal.paletteIndex, decal.alpha, decal.flags,
				decal.plane[0], decal.plane[1], decal.plane[2], decal.plane[3],
				decal.uvBounds[0], decal.uvBounds[1], decal.uvBounds[2], decal.uvBounds[3]);
		}
	}
	const void* headerData = headers.empty() ? static_cast<const void*>(&emptyHeader) : headers.data();
	const void* decalData = decals.empty() ? static_cast<const void*>(&emptyDecal) : decals.data();
	const uint64_t headerSize = std::max<size_t>(headers.size(), 1u) * sizeof(emptyHeader);
	const uint64_t decalSize = std::max<size_t>(decals.size(), 1u) * sizeof(emptyDecal);

	NRISceneDataFrameSlot* slot =
		allowSceneDataRing && renderer.ShouldUseSceneDataFrameRing() ? &renderer.GetCurrentSceneDataFrameSlot() : nullptr;
	NRIBufferResource& headerBuffer = slot != nullptr ? slot->wallDecalHeaderBuffer : renderer.mWallDecalHeaderBuffer;
	NRIBufferResource& decalBuffer = slot != nullptr ? slot->wallDecalBuffer : renderer.mWallDecalBuffer;
	SceneBufferDebugStats& headerStats = slot != nullptr ? slot->wallDecalHeaderStats : renderer.mWallDecalHeaderBufferStats;
	SceneBufferDebugStats& decalStats = slot != nullptr ? slot->wallDecalStats : renderer.mWallDecalBufferStats;
	if (audit)
	{
		PerfAuditAdd(slot != nullptr ? PerfAuditMetric::WallDecalRingUploadCalls : PerfAuditMetric::WallDecalFallbackUploadCalls);
		PerfAuditAdd(PerfAuditMetric::WallDecalHeaderRequestedBytes, headerSize);
		PerfAuditAdd(PerfAuditMetric::WallDecalRecordRequestedBytes, decalSize);
	}
	// Structured views expose the retained allocation, including a previous
	// map's larger wall domain. Clear its tail so an empty/smaller capture cannot
	// accidentally reuse old decal ranges through shader GetDimensions().
	const uint64_t headerStorageSize = GetNRIGrownBufferSize(headerBuffer.size, headerSize, sizeof(emptyHeader));
	std::vector<nri_scene::WallDecalHeaderGpuData> referenceHeaders;
	if (headerStorageSize > headerSize)
	{
		PerfAuditScope auditPadding(PerfAuditMetric::WallDecalHeaderPaddingNs);
		auto& padding = renderer.mWallDecals.HeaderPadding();
		const uint64_t previousCapacity = padding.CapacityBytes();
		uint64_t allocatedBytes = 0;
		if (!padding.Quarantined())
		{
			const auto& padded = padding.Prepare(headers, headerStorageSize / sizeof(emptyHeader));
			headerData = padded.data();
			allocatedBytes = padding.CapacityBytes() > previousCapacity ? padding.CapacityBytes() : 0;
			if ((bool)nri_ptdecalreusevalidate)
			{
				// Independent original padding path, deliberately allocating a fresh
				// zero-filled vector before copying the current live prefix.
				referenceHeaders.resize(headerStorageSize / sizeof(emptyHeader));
				std::copy(headers.begin(), headers.end(), referenceHeaders.begin());
				PerfAuditAdd(PerfAuditMetric::WallDecalHeaderPaddingValidationChecks);
				if (allocatedBytes == 0) PerfAuditAdd(PerfAuditMetric::WallDecalHeaderPaddingHitValidationChecks);
				if (std::memcmp(headerData, referenceHeaders.data(), (size_t)headerStorageSize) != 0)
				{
					PerfAuditAdd(PerfAuditMetric::WallDecalHeaderPaddingValidationMismatches);
					PerfAuditAdd(PerfAuditMetric::WallDecalHeaderPaddingQuarantines);
					padding.Quarantine();
					headerData = referenceHeaders.data();
					Printf("NRI PT wall decal reuse: padding_mismatch=1 action=reference-fallback-quarantine\n");
				}
			}
		}
		else
		{
			referenceHeaders.resize(headerStorageSize / sizeof(emptyHeader));
			std::copy(headers.begin(), headers.end(), referenceHeaders.begin());
			headerData = referenceHeaders.data();
			allocatedBytes = referenceHeaders.capacity() * sizeof(emptyHeader);
		}
		if (audit)
		{
			if (allocatedBytes != 0)
			{
				PerfAuditAdd(PerfAuditMetric::WallDecalHeaderPaddingAllocations);
				PerfAuditAdd(PerfAuditMetric::WallDecalHeaderPaddingAllocatedBytes, allocatedBytes);
			}
			else PerfAuditAdd(PerfAuditMetric::WallDecalHeaderPaddingReuseCalls);
			PerfAuditAdd(PerfAuditMetric::WallDecalHeaderPaddingTailBytes, headerStorageSize - headerSize);
			PerfAuditSet(PerfAuditMetric::WallDecalHeaderPaddingCapacityBytes, padding.CapacityBytes());
		}
	}

	const auto upload = [&](NRIBufferResource& buffer, SceneBufferDebugStats& stats,
		const void* data, uint64_t size, uint32_t stride)
	{
		{
			PerfAuditScope auditWait(PerfAuditMetric::WallDecalBufferWaitNs);
			WaitIfStructuredUpdateNeedsIt(renderer, buffer, data, size, stride, slot != nullptr ? nullptr : ioWaitedForWrites);
		}
		bool uploaded;
		{
			PerfAuditScope auditWrite(PerfAuditMetric::WallDecalBufferUploadNs);
			uploaded = renderer.EnsureStructuredBuffer(
				buffer, stats, data, size, stride, nri::BufferUsageBits::SHADER_RESOURCE,
				NRIResourceComputeShaderResourceAccess(),
				slot != nullptr || (ioWaitedForWrites != nullptr && *ioWaitedForWrites), "scene_data_upload");
		}
		if (audit)
		{
			PerfAuditAdd(PerfAuditMetric::WallDecalBufferGrowthEvents, stats.growEventsLastFrame);
			PerfAuditAdd(PerfAuditMetric::WallDecalBufferGrowthAllocatedBytes, stats.growthAllocatedBytesLastFrame);
			if (uploaded)
			{
				PerfAuditAdd(PerfAuditMetric::WallDecalBufferWrites);
				PerfAuditAdd(PerfAuditMetric::WallDecalBufferUploadedBytes, size);
			}
		}
		return uploaded;
	};
	if (!upload(headerBuffer, headerStats, headerData, headerStorageSize, sizeof(emptyHeader)) ||
		!upload(decalBuffer, decalStats, decalData, decalSize, sizeof(emptyDecal)))
	{
		if (audit) PerfAuditAdd(PerfAuditMetric::WallDecalUploadFailures);
		return false;
	}
	if (audit)
	{
		PerfAuditSet(PerfAuditMetric::WallDecalHeaderBufferCapacityBytes, headerBuffer.size);
		PerfAuditSet(PerfAuditMetric::WallDecalRecordBufferCapacityBytes, decalBuffer.size);
	}

	PerfAuditScope auditDescriptor(PerfAuditMetric::WallDecalDescriptorPublishNs);
	renderer.mSceneDataDescriptors[NRI_SCENE_DATA_WALL_DECAL_HEADER_SLOT] = headerBuffer.shaderView;
	renderer.mSceneDataDescriptors[NRI_SCENE_DATA_WALL_DECAL_SLOT] = decalBuffer.shaderView;
	if (SceneDataDescriptorsReady(renderer))
	{
		// A reused scene may keep its current queued descriptor set without a full
		// rebuild. Publish only the decal range so unrelated per-frame bindings
		// retain their existing ownership and this fresh capture is never deferred.
		const nri::Descriptor* descriptors[] = { headerBuffer.shaderView, decalBuffer.shaderView };
		nri::UpdateDescriptorRangeDesc update = {};
		update.descriptorSet = renderer.GetCurrentSceneDataSet();
		update.rangeIndex = 0;
		update.baseDescriptor = NRI_SCENE_DATA_WALL_DECAL_HEADER_SLOT;
		update.descriptors = descriptors;
		update.descriptorNum = 2;
		auto& previousPublication = LastNRIWallDecalAuditPublication();
		NRIWallDecalAuditDescriptorTuple current;
		if (audit)
		{
			const auto token = PerfAuditCurrentToken();
			current.owner = &renderer;
			current.set = update.descriptorSet;
			current.headerBuffer = headerBuffer.buffer;
			current.recordBuffer = decalBuffer.buffer;
			current.headerView = headerBuffer.shaderView;
			current.recordView = decalBuffer.shaderView;
			current.epoch = token.epoch;
			current.presentation = token.presentation;
			current.mapEpoch = renderer.mMapWorld.valid ? renderer.mMapWorld.buildSerial : 0;
			current.buildEpoch = renderer.mStaticMapScene.valid ? renderer.mStaticMapScene.buildSerial : 0;
			current.generation = renderer.mSceneDataDescriptorGeneration;
			if (current.SameKnownPublication(previousPublication))
				PerfAuditAdd(PerfAuditMetric::WallDecalDescriptorExactRepeatCandidates);
		}
		renderer.BuildResourceServices().context.core->UpdateDescriptorRanges(&update, 1);
		renderer.mLastPerfShellTraceStats.sceneDataSetDescriptorUpdateCount++;
		renderer.mSceneDataDescriptorGeneration++;
		renderer.mLastPerfShellTraceStats.sceneDataDescriptorGeneration = renderer.mSceneDataDescriptorGeneration;
		if (audit)
		{
			PerfAuditAdd(PerfAuditMetric::WallDecalDescriptorUpdates);
			PerfAuditAdd(PerfAuditMetric::WallDecalDescriptorRowsWritten, 2);
			current.generation = renderer.mSceneDataDescriptorGeneration;
			previousPublication = current;
		}
	}
	return true;
}
