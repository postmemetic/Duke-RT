#include "nri_scene_upload.h"
#include "nri_cvars.h"

#include "nri_renderer.h"

#include "nri_descriptor_sets.h"
#include "../scene/nri_hash.h"
#include "nri_runtime_mutation_trace.h"
#include "nri_scene_lights.h"
#include "nri_scene_upload_dirty_plan.h"
#include "nri_scene_upload_identity.h"
#include "nri_shader_contracts.h"
#include "nri_static_scene_geometry.h"
#include "nri_upload_hash.h"
#include "c_cvars.h"
#include "perf_capture.h"
#include "../../hwrenderer/data/hw_clock.h"

#include <algorithm>
#include <chrono>
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

	struct ScenePortalData
	{
		uint32_t traversalClass = 0;
		uint32_t kind = 0;
		uint32_t targetLocalSpaceIndex = UINT32_MAX;
		uint32_t flags = 0;
		float delta[3] = {};
		uint32_t reserved0 = 0;
	};

	static uint32_t GetPortalTraversalClass(nri_scene::PTPortalKind kind)
	{
		switch (kind)
		{
		case nri_scene::PTPortalKind::WallMirror:
		case nri_scene::PTPortalKind::SectorFloorMirror:
		case nri_scene::PTPortalKind::SectorCeilingMirror:
			return NRI_PORTAL_TRAVERSAL_CLASS_REFLECTIVE;

		case nri_scene::PTPortalKind::WallView:
		case nri_scene::PTPortalKind::SectorFloorStack:
		case nri_scene::PTPortalKind::SectorCeilingStack:
			return NRI_PORTAL_TRAVERSAL_CLASS_SPACE_TRANSFER;

		case nri_scene::PTPortalKind::WallToSprite:
			return NRI_PORTAL_TRAVERSAL_CLASS_RUNTIME_BOUND;

		default:
			return NRI_PORTAL_TRAVERSAL_CLASS_NONE;
		}
	}

	static std::vector<ScenePortalData> BuildScenePortalData(const nri_scene::PTMapWorld& mapWorld)
	{
		std::vector<ScenePortalData> portals;
		portals.reserve(std::max<size_t>(mapWorld.portals.size(), 1u));

		for (const auto& portal : mapWorld.portals)
		{
			ScenePortalData data = {};
			data.traversalClass = GetPortalTraversalClass(portal.kind);
			data.kind = (uint32_t)portal.kind;
			data.flags = portal.runtimeBoundTarget ? NRI_PORTAL_FLAG_RUNTIME_BOUND : 0u;
			if (portal.targetCount > 0 && portal.firstTarget < mapWorld.portalTargets.size())
			{
				data.targetLocalSpaceIndex = mapWorld.portalTargets[portal.firstTarget].localSpaceIndex;
			}
			data.delta[0] = (float)portal.delta[0];
			data.delta[1] = (float)portal.delta[1];
			data.delta[2] = (float)portal.delta[2];
			portals.push_back(data);
		}

		if (portals.empty())
		{
			portals.push_back({});
		}

		return portals;
	}
}

#include "../system/nri_renderdevice.h"

#include <cstring>

namespace
{
	struct NRIReprojectionData
	{
		float currentViewToClip[16] = {};
		float previousViewToClip[16] = {};
		float currentWorldToView[16] = {};
		float previousWorldToView[16] = {};
		float currentJitter[2] = {};
		float previousJitter[2] = {};
	};
	static_assert(sizeof(NRIReprojectionData) == 272);

	static bool ShouldTraceSceneBufferDirtyRanges()
	{
		return (int)perf_looptraceframes > 0;
	}

	static uint64_t HashUploadPayloadBytes(const void* data, uint64_t size)
	{
		return NRIHashUploadPayloadBytes(data, size);
	}

	static uint64_t HashSceneDataPayload(const void* data, uint64_t size, uint64_t count)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = nri_scene::HashCombine64(hash, count);
		hash = nri_scene::HashCombine64(hash, HashUploadPayloadBytes(data, size));
		return hash != 0 ? hash : 1;
	}

	static uint64_t HashPrimitiveRewriteProvenancePayload(const std::vector<nri_scene::SurfaceProvenance>& provenanceList)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = nri_scene::HashCombine64(hash, (uint64_t)provenanceList.size());
		for (const nri_scene::SurfaceProvenance& provenance : provenanceList)
		{
			hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)provenance.sourceType);
			hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)(provenance.sectorIndex + 1));
			hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)(provenance.wallIndex + 1));
			hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)(provenance.sectionIndex + 1));
			hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)(provenance.mapChunkIndex + 1));
			hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)(provenance.nextSectorIndex + 1));
			hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)(provenance.actorIndex + 1));
			hash = nri_scene::HashCombine64(hash, (uint64_t)provenance.drawListType);
			hash = nri_scene::HashCombine64(hash, (uint64_t)provenance.cstat);
			hash = nri_scene::HashCombine64(hash, (uint64_t)provenance.materialFlags);
		}
		return hash != 0 ? hash : 1;
	}

	static uint64_t HashPrimitiveRewriteVisibilityIdentity(const nri_scene::PTMapWorld& mapWorld)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = nri_scene::HashCombine64(hash, mapWorld.valid ? 1ull : 0ull);
		hash = nri_scene::HashCombine64(hash, mapWorld.buildSerial);
		hash = nri_scene::HashCombine64(hash, (uint64_t)mapWorld.chunks.size());
		hash = nri_scene::HashCombine64(hash, (uint64_t)mapWorld.stats.chunkCount);
		for (const nri_scene::PTMapChunk& chunk : mapWorld.chunks)
		{
			hash = nri_scene::HashCombine64(hash, (uint64_t)chunk.chunkIndex);
			hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)(chunk.sectorIndex + 1));
			hash = nri_scene::HashCombine64(hash, (uint64_t)chunk.firstSurface);
			hash = nri_scene::HashCombine64(hash, (uint64_t)chunk.surfaceCount);
		}
		return hash != 0 ? hash : 1;
	}

	static bool StructuredBufferUpdateNeedsWait(
		const NRIBufferResource& resource,
		const void* data,
		uint64_t size,
		uint32_t stride)
	{
		const uint64_t requiredSize = std::max<uint64_t>(size, stride);
		const bool needsGrowth =
			resource.buffer == nullptr ||
			resource.shaderView == nullptr ||
			resource.stride != stride ||
			resource.size < requiredSize;
		if (needsGrowth)
		{
			return resource.buffer != nullptr || resource.shaderView != nullptr;
		}

		return data != nullptr && size != 0;
	}

	static uint64_t EstimateSceneDataFrameResourceCapacity(const NRIBufferResource& resource, uint64_t size, uint32_t stride)
	{
		const uint64_t requiredSize = std::max<uint64_t>(size, stride);
		if (resource.buffer == nullptr ||
			resource.shaderView == nullptr ||
			resource.stride != stride ||
			resource.size < requiredSize)
		{
			return GetNRIGrownBufferSize(resource.size, requiredSize, stride);
		}

		return resource.size;
	}

	static NRISceneDataLightBufferReuseView BuildSceneDataLightBufferReuseView(const NRIBufferResource& resource)
	{
		NRISceneDataLightBufferReuseView view = {};
		view.resourceIdentity = reinterpret_cast<uintptr_t>(resource.buffer);
		view.descriptorIdentity = reinterpret_cast<uintptr_t>(resource.shaderView);
		view.resourceReady = resource.buffer != nullptr;
		view.descriptorReady = resource.shaderView != nullptr;
		view.usedSize = resource.usedSize;
		view.capacity = resource.size;
		view.stride = resource.stride;
		return view;
	}

	static uint64_t EstimateSceneDataFrameSlotCapacity(
		const NRISceneDataFrameSlot& slot,
		uint64_t reprojectionSize,
		uint64_t visibleChunkSize,
		uint64_t visibleFlatPlaneSize,
		uint64_t spatialAbsenceSize,
		uint64_t spatialAbsenceTypedSize,
		uint64_t sceneInstanceSize,
		uint64_t runtimeLightSize,
		uint64_t runtimeLightTileHeaderSize,
		uint64_t runtimeLightTileIndexSize,
		uint64_t sectorLightHeaderSize,
		uint64_t sectorLightSize)
	{
		return
			EstimateSceneDataFrameResourceCapacity(slot.reprojectionBuffer, reprojectionSize, (uint32_t)reprojectionSize) +
			EstimateSceneDataFrameResourceCapacity(slot.visibleChunkBuffer, visibleChunkSize, sizeof(uint32_t)) +
			EstimateSceneDataFrameResourceCapacity(slot.visibleFlatPlaneBuffer, visibleFlatPlaneSize, sizeof(uint32_t)) +
			EstimateSceneDataFrameResourceCapacity(slot.spatialAbsenceBuffer, spatialAbsenceSize, sizeof(NRISpatialAbsenceGpuRecord)) +
			EstimateSceneDataFrameResourceCapacity(slot.spatialAbsenceTypedBuffer, spatialAbsenceTypedSize, sizeof(NRISpatialAbsenceGpuBlock)) +
			EstimateSceneDataFrameResourceCapacity(slot.sceneInstanceBuffer, sceneInstanceSize, sizeof(SceneInstanceData)) +
			EstimateSceneDataFrameResourceCapacity(slot.runtimeLightBuffer, runtimeLightSize, sizeof(NRIRuntimePointLightGpuData)) +
			EstimateSceneDataFrameResourceCapacity(slot.runtimeLightTileHeaderBuffer, runtimeLightTileHeaderSize, sizeof(NRIRuntimeLightTileHeaderGpuData)) +
			EstimateSceneDataFrameResourceCapacity(slot.runtimeLightTileIndexBuffer, runtimeLightTileIndexSize, sizeof(uint32_t)) +
			EstimateSceneDataFrameResourceCapacity(slot.sectorLightHeaderBuffer, sectorLightHeaderSize, sizeof(NRISectorLightHeaderGpuData)) +
			EstimateSceneDataFrameResourceCapacity(slot.sectorLightBuffer, sectorLightSize, sizeof(NRISectorLightGpuData));
	}

	static void NoteSceneDataWaitEvent(
		NRIRenderer::PerfShellTraceStats& stats,
		const char* reason,
		const char* buffer,
		double waitMs)
	{
		const uint32_t eventIndex = stats.sceneDataSetWaitCount;
		if (eventIndex < 2)
		{
			stats.sceneDataSetWaitEventReason[eventIndex] = reason != nullptr ? reason : "unlabeled";
			stats.sceneDataSetWaitEventBuffer[eventIndex] = buffer != nullptr ? buffer : "unknown";
			stats.sceneDataSetWaitEventMs[eventIndex] = waitMs;
		}
	}

}

void NRIRenderer::ResetSceneBufferFrameStats()
{
	auto resetStats = [](SceneBufferDebugStats& stats)
	{
		stats.bytesUploadedLastFrame = 0;
		stats.growEventsLastFrame = 0;
		stats.overwriteEventsLastFrame = 0;
		stats.growthOldBytesLastFrame = 0;
		stats.growthRequestedBytesLastFrame = 0;
		stats.growthAllocatedBytesLastFrame = 0;
	};
	mVertexBufferStats.bytesUploadedLastFrame = 0;
	mVertexBufferStats.growEventsLastFrame = 0;
	mVertexBufferStats.overwriteEventsLastFrame = 0;
	mVertexBufferStats.growthOldBytesLastFrame = 0;
	mVertexBufferStats.growthRequestedBytesLastFrame = 0;
	mVertexBufferStats.growthAllocatedBytesLastFrame = 0;
	mIndexBufferStats.bytesUploadedLastFrame = 0;
	mIndexBufferStats.growEventsLastFrame = 0;
	mIndexBufferStats.overwriteEventsLastFrame = 0;
	mIndexBufferStats.growthOldBytesLastFrame = 0;
	mIndexBufferStats.growthRequestedBytesLastFrame = 0;
	mIndexBufferStats.growthAllocatedBytesLastFrame = 0;
	mPrimitiveBufferStats.bytesUploadedLastFrame = 0;
	mPrimitiveBufferStats.growEventsLastFrame = 0;
	mPrimitiveBufferStats.overwriteEventsLastFrame = 0;
	mPrimitiveBufferStats.growthOldBytesLastFrame = 0;
	mPrimitiveBufferStats.growthRequestedBytesLastFrame = 0;
	mPrimitiveBufferStats.growthAllocatedBytesLastFrame = 0;
	mMaterialBufferStats.bytesUploadedLastFrame = 0;
	mMaterialBufferStats.growEventsLastFrame = 0;
	mMaterialBufferStats.overwriteEventsLastFrame = 0;
	mMaterialBufferStats.growthOldBytesLastFrame = 0;
	mMaterialBufferStats.growthRequestedBytesLastFrame = 0;
	mMaterialBufferStats.growthAllocatedBytesLastFrame = 0;
	resetStats(mSceneInstanceBufferStats);
	resetStats(mPortalBufferStats);
	resetStats(mSpatialAbsenceBufferStats);
	resetStats(mSpatialAbsenceTypedBufferStats);
	for (SceneDataFrameSlot& slot : mSceneDataFrameRing)
	{
		resetStats(slot.reprojectionStats);
		resetStats(slot.visibleChunkStats);
		resetStats(slot.visibleFlatPlaneStats);
		resetStats(slot.spatialAbsenceStats);
		resetStats(slot.spatialAbsenceTypedStats);
		resetStats(slot.sceneInstanceStats);
		resetStats(slot.portalStats);
		resetStats(slot.runtimeLightStats);
		resetStats(slot.runtimeLightTileHeaderStats);
		resetStats(slot.runtimeLightTileIndexStats);
		resetStats(slot.emissivePrimitiveHeaderStats);
		resetStats(slot.emissivePrimitiveStats);
		resetStats(slot.emissivePrimitiveCdfStats);
		resetStats(slot.emissiveMaterialResponseStats);
		resetStats(slot.sectorLightHeaderStats);
		resetStats(slot.sectorLightStats);
	}
}

const NRIBufferResource& NRIRenderer::GetActiveVertexBuffer() const
{
	return mBoundDynamicPrimitiveCount > 0 ? GetCurrentDynamicVertexBuffer() : mStaticVertexBuffer;
}

const NRIBufferResource& NRIRenderer::GetActiveIndexBuffer() const
{
	return mBoundDynamicPrimitiveCount > 0 ? GetCurrentDynamicIndexBuffer() : mStaticIndexBuffer;
}

const NRIBufferResource& NRIRenderer::GetActivePrimitiveBuffer() const
{
	return mBoundDynamicPrimitiveCount > 0 ? GetCurrentDynamicPrimitiveBuffer() : mStaticPrimitiveBuffer;
}

const NRIBufferResource& NRIRenderer::GetActiveMaterialBuffer() const
{
	return mBoundDynamicMaterialCount > 0 ? GetCurrentDynamicMaterialBuffer() : mStaticMaterialBuffer;
}

NRIRenderer::SceneUploadBufferRingSlot& NRIRenderer::GetCurrentSceneUploadBufferRingSlot()
{
	const uint32_t queuedFrameCount =
		mFrameBuffer != nullptr && !mFrameBuffer->mQueuedFrames.empty() ?
		(uint32_t)mFrameBuffer->mQueuedFrames.size() :
		1u;
	if (mSceneUploadBufferRing.size() < queuedFrameCount)
	{
		mSceneUploadBufferRing.resize(queuedFrameCount);
	}

	return mSceneUploadBufferRing[GetCurrentQueuedFrameIndex() % (uint32_t)mSceneUploadBufferRing.size()];
}

const NRIRenderer::SceneUploadBufferRingSlot* NRIRenderer::GetCurrentSceneUploadBufferRingSlot() const
{
	if (mSceneUploadBufferRing.empty())
	{
		return nullptr;
	}

	return &mSceneUploadBufferRing[GetCurrentQueuedFrameIndex() % (uint32_t)mSceneUploadBufferRing.size()];
}

NRIRenderer::SceneDataFrameSlot& NRIRenderer::GetCurrentSceneDataFrameSlot()
{
	const uint32_t queuedFrameCount =
		mFrameBuffer != nullptr && !mFrameBuffer->mQueuedFrames.empty() ?
		(uint32_t)mFrameBuffer->mQueuedFrames.size() :
		1u;
	if (mSceneDataFrameRing.size() < queuedFrameCount)
	{
		mSceneDataFrameRing.resize(queuedFrameCount);
	}

	return mSceneDataFrameRing[GetCurrentQueuedFrameIndex() % (uint32_t)mSceneDataFrameRing.size()];
}

const NRIRenderer::SceneDataFrameSlot* NRIRenderer::GetCurrentSceneDataFrameSlot() const
{
	if (mSceneDataFrameRing.empty())
	{
		return nullptr;
	}

	return &mSceneDataFrameRing[GetCurrentQueuedFrameIndex() % (uint32_t)mSceneDataFrameRing.size()];
}

bool NRIRenderer::ShouldUseSceneDataFrameRing() const
{
	return (bool)nri_ptscenedataring && mSceneDataFrameRingDisabledFrameIndex != mFrameIndex;
}

uint64_t NRIRenderer::GetSceneDataFrameRingCapacityBytes() const
{
	uint64_t capacityBytes = 0;
	for (const SceneDataFrameSlot& slot : mSceneDataFrameRing)
	{
		capacityBytes += slot.CapacityBytes();
	}
	return capacityBytes;
}

void NRIRenderer::NoteSceneDataFrameRingTelemetry(const SceneDataFrameSlot* slot, bool enabled, bool fallback, bool overCap)
{
	PerfShellTraceStats& stats = mLastPerfShellTraceStats;
	(void)fallback;
	const bool frameOverCap = overCap || mSceneDataFrameRingOverCapFrameIndex == mFrameIndex;
	stats.sceneDataSetFrameSlot = GetCurrentQueuedFrameIndex();
	stats.sceneDataSetFrameSlotCount = (uint32_t)mSceneDataFrameRing.size();
	stats.sceneDataSetFrameSlotEnabled = enabled ? 1u : 0u;
	stats.sceneDataSetFrameSlotFallbacks = mSceneDataFrameRingFallbackCount;
	uint32_t overCapCount = mSceneDataFrameRingOverCapCount;
	if (overCapCount == 0 && (int)nri_ptscenedataringmaxbytes > 0)
	{
		overCapCount = mSceneDataFrameRingFallbackCount;
	}
	stats.sceneDataSetFrameSlotOverCap = overCapCount + (frameOverCap && overCapCount == 0 ? 1u : 0u);
	stats.sceneDataSetFrameSlotWaits = mSceneDataFrameRingSlotWaitCount;
	stats.sceneDataSetFrameRingCapacityBytes = GetSceneDataFrameRingCapacityBytes();
	stats.sceneDataSetFrameRingHighWaterBytes = mSceneDataFrameRingHighWaterBytes;
	if (slot != nullptr)
	{
		stats.sceneDataSetFrameSlotUsedBytes = slot->UsedBytes();
		stats.sceneDataSetFrameSlotCapacityBytes = slot->CapacityBytes();
		stats.sceneDataSetFrameSlotGrows = slot->GrowEventsLastFrame();
		mSceneDataFrameRingHighWaterBytes = std::max(mSceneDataFrameRingHighWaterBytes, stats.sceneDataSetFrameRingCapacityBytes);
		stats.sceneDataSetFrameRingHighWaterBytes = mSceneDataFrameRingHighWaterBytes;
	}
}

NRIBufferResource& NRIRenderer::GetCurrentDynamicVertexBuffer()
{
	return GetCurrentSceneUploadBufferRingSlot().vertexBuffer;
}

NRIBufferResource& NRIRenderer::GetCurrentDynamicIndexBuffer()
{
	return GetCurrentSceneUploadBufferRingSlot().indexBuffer;
}

NRIBufferResource& NRIRenderer::GetCurrentDynamicPrimitiveBuffer()
{
	return GetCurrentSceneUploadBufferRingSlot().primitiveBuffer;
}

NRIBufferResource& NRIRenderer::GetCurrentDynamicMaterialBuffer()
{
	return GetCurrentSceneUploadBufferRingSlot().materialBuffer;
}

NRIAccelerationStructureResource& NRIRenderer::GetCurrentDynamicBottomLevelAS()
{
	return GetCurrentSceneUploadBufferRingSlot().dynamicBottomLevelAS;
}

NRIBufferResource& NRIRenderer::GetCurrentTlasInstanceBuffer()
{
	return GetCurrentWorldTlasFrameSlot().instanceBuffer;
}

NRIWorldTlasFrameSlot& NRIRenderer::GetCurrentWorldTlasFrameSlot()
{
	const uint32_t queuedFrameCount =
		mFrameBuffer != nullptr && !mFrameBuffer->mQueuedFrames.empty() ?
		(uint32_t)mFrameBuffer->mQueuedFrames.size() :
		1u;
	return mWorldTlasFrameSlots.Get(GetCurrentQueuedFrameIndex(), queuedFrameCount);
}

const NRIBufferResource& NRIRenderer::GetCurrentDynamicVertexBuffer() const
{
	const SceneUploadBufferRingSlot* slot = GetCurrentSceneUploadBufferRingSlot();
	return slot != nullptr ? slot->vertexBuffer : mVertexBuffer;
}

const NRIBufferResource& NRIRenderer::GetCurrentDynamicIndexBuffer() const
{
	const SceneUploadBufferRingSlot* slot = GetCurrentSceneUploadBufferRingSlot();
	return slot != nullptr ? slot->indexBuffer : mIndexBuffer;
}

const NRIBufferResource& NRIRenderer::GetCurrentDynamicPrimitiveBuffer() const
{
	const SceneUploadBufferRingSlot* slot = GetCurrentSceneUploadBufferRingSlot();
	return slot != nullptr ? slot->primitiveBuffer : mPrimitiveBuffer;
}

const NRIBufferResource& NRIRenderer::GetCurrentDynamicMaterialBuffer() const
{
	const SceneUploadBufferRingSlot* slot = GetCurrentSceneUploadBufferRingSlot();
	return slot != nullptr ? slot->materialBuffer : mMaterialBuffer;
}

const NRIAccelerationStructureResource* NRIRenderer::GetCurrentDynamicBottomLevelAS() const
{
	const SceneUploadBufferRingSlot* slot = GetCurrentSceneUploadBufferRingSlot();
	return slot != nullptr ? &slot->dynamicBottomLevelAS : nullptr;
}

const NRIWorldTlasFrameSlot* NRIRenderer::GetCurrentWorldTlasFrameSlot() const
{
	return mWorldTlasFrameSlots.Find(GetCurrentQueuedFrameIndex());
}

bool NRIRenderer::HasAnyDynamicBottomLevelAS() const
{
	for (const SceneUploadBufferRingSlot& slot : mSceneUploadBufferRing)
	{
		if (slot.dynamicBottomLevelAS.accelerationStructure != nullptr ||
			slot.dynamicBottomLevelAS.descriptor != nullptr)
		{
			return true;
		}
	}

	return false;
}

NRIRenderer::ResidentUploadScratchFrame& NRIRenderer::GetResidentUploadScratchFrame()
{
	const uint32_t frameSlot = GetCurrentQueuedFrameIndex() % (uint32_t)mResidentUploadScratchFrames.size();
	auto& frameScratch = mResidentUploadScratchFrames[frameSlot];
	if (frameScratch.frameIndex != mFrameIndex)
	{
		for (NRIBufferResource& retired : frameScratch.retiredBuffers)
		{
			DestroyBufferResource(retired);
		}
		frameScratch.retiredBuffers.clear();
		for (NRIAccelerationStructureResource& retired : frameScratch.retiredAccelerationStructures)
		{
			DestroyAccelerationStructureResource(retired);
		}
		frameScratch.retiredAccelerationStructures.clear();
		frameScratch.frameIndex = mFrameIndex;
		frameScratch.vertex.cursor = 0;
		frameScratch.vertex.copySourceActive = false;
		frameScratch.index.cursor = 0;
		frameScratch.index.copySourceActive = false;
		frameScratch.primitive.cursor = 0;
		frameScratch.primitive.copySourceActive = false;
		frameScratch.material.cursor = 0;
		frameScratch.material.copySourceActive = false;
	}

	return frameScratch;
}

void NRIRenderer::ResetResidentUploadScratchFrame(const char* reason)
{
	const uint32_t frameSlot = GetCurrentQueuedFrameIndex() % (uint32_t)mResidentUploadScratchFrames.size();
	auto& frameScratch = mResidentUploadScratchFrames[frameSlot];
	const uint32_t retiredBufferCount = (uint32_t)frameScratch.retiredBuffers.size();
	const uint32_t retiredAccelerationCount = (uint32_t)frameScratch.retiredAccelerationStructures.size();

	for (NRIBufferResource& retired : frameScratch.retiredBuffers)
	{
		DestroyBufferResource(retired);
	}
	frameScratch.retiredBuffers.clear();
	for (NRIAccelerationStructureResource& retired : frameScratch.retiredAccelerationStructures)
	{
		DestroyAccelerationStructureResource(retired);
	}
	frameScratch.retiredAccelerationStructures.clear();

	const uint64_t vertexBytes = frameScratch.vertex.cursor;
	const uint64_t indexBytes = frameScratch.index.cursor;
	const uint64_t primitiveBytes = frameScratch.primitive.cursor;
	const uint64_t materialBytes = frameScratch.material.cursor;
	frameScratch.frameIndex = mFrameIndex;
	frameScratch.vertex.cursor = 0;
	frameScratch.vertex.copySourceActive = false;
	frameScratch.index.cursor = 0;
	frameScratch.index.copySourceActive = false;
	frameScratch.primitive.cursor = 0;
	frameScratch.primitive.copySourceActive = false;
	frameScratch.material.cursor = 0;
	frameScratch.material.copySourceActive = false;

	if ((int)nri_ptloadingtrace >= 2 && (vertexBytes != 0 || indexBytes != 0 || primitiveBytes != 0 || materialBytes != 0 || retiredBufferCount != 0 || retiredAccelerationCount != 0))
	{
		Printf("NRI PT preload upload scratch: event=reset reason=%s frame=%u slot=%u vertex=%llu index=%llu primitive=%llu material=%llu retired_buffers=%u retired_as=%u\n",
			reason != nullptr ? reason : "unknown",
			mFrameIndex,
			frameSlot,
			(unsigned long long)vertexBytes,
			(unsigned long long)indexBytes,
			(unsigned long long)primitiveBytes,
			(unsigned long long)materialBytes,
			retiredBufferCount,
			retiredAccelerationCount);
	}
}

nri::DescriptorSet* NRIRenderer::GetCurrentSceneTextureSet() const
{
	return NRIDescriptorSetManager::GetCurrentSceneTextureSet(*this);
}

nri::DescriptorSet* NRIRenderer::GetCurrentSceneDataSet() const
{
	return NRIDescriptorSetManager::GetCurrentSceneDataSet(*this);
}

bool NRIRenderer::IsCurrentSceneDataDescriptorsInitialized() const
{
	return NRIDescriptorSetManager::IsCurrentSceneDataDescriptorsInitialized(*this);
}

void NRIRenderer::SetCurrentSceneDataDescriptorsInitialized(bool value)
{
	NRIDescriptorSetManager::SetCurrentSceneDataDescriptorsInitialized(*this, value);
}

void NRIRenderer::TraceSharedDescriptorRewrite(const char* setName, const char* reason, uint64_t descriptorHash, uint32_t descriptorCount, bool sceneTextureSet)
{
	NRIDescriptorSetManager::TraceSharedDescriptorRewrite(*this, setName, reason, descriptorHash, descriptorCount, sceneTextureSet);
}

bool NRISceneUploadManager::SceneDataDescriptorsReady(NRIRenderer& renderer)
{
	if (!renderer.IsCurrentSceneDataDescriptorsInitialized() || renderer.GetCurrentSceneDataSet() == nullptr)
	{
		return false;
	}

	for (const nri::Descriptor* descriptor : renderer.mSceneDataDescriptors)
	{
		if (descriptor == nullptr)
		{
			return false;
		}
	}

	return true;
}

bool NRISceneUploadManager::UpdateSceneDataDescriptorSlot(
	NRIRenderer& renderer,
	uint32_t slot,
	nri::Descriptor* descriptor,
	const char* reason)
{
	if (slot >= renderer.mSceneDataDescriptors.size() ||
		renderer.mSceneDataDescriptors[slot] == descriptor)
	{
		return true;
	}

	renderer.mSceneDataDescriptors[slot] = descriptor;
	if (SceneDataDescriptorsReady(renderer))
	{
		if (renderer.ShouldUseSceneDataFrameRing() &&
			renderer.mSceneDataSnapshotFrameIndex != renderer.mFrameIndex)
		{
			renderer.mLastPerfShellTraceStats.sceneDataSetDeferredDescriptorUpdateCount++;
			return true;
		}

		return NRIDescriptorSetManager::CommitSceneDataDescriptors(renderer, reason);
	}

	return true;
}

bool NRISceneUploadManager::WaitIfStructuredUpdateNeedsIt(
	NRIRenderer& renderer,
	NRIBufferResource& resource,
	const void* data,
	uint64_t size,
	uint32_t stride,
	bool* ioWaitedForWrites)
{
	if (ioWaitedForWrites != nullptr &&
		!*ioWaitedForWrites &&
		StructuredBufferUpdateNeedsWait(resource, data, size, stride))
	{
		renderer.WaitForCommandsTracked("scene_data_upload");
		*ioWaitedForWrites = true;
	}

	return true;
}

bool NRISceneUploadManager::EnsureSceneDataBatched(
	NRIRenderer& renderer,
	bool& waitedForWrites,
	const char* reason,
	NRIBufferResource& resource,
	SceneBufferDebugStats& stats,
	const char* bufferLabel,
	const void* data,
	uint64_t size,
	uint32_t stride,
	nri::BufferUsageBits usage,
	nri::AccessStage after,
	double& uploadMs,
	uint64_t& requestedBytes,
	uint64_t& uploadedBytes,
	bool writesQuiesced)
{
	bool needsWait = false;
	{
		ScopedPtPerfTimer waitCheckTimer(renderer.mLastPerfShellTraceStats.sceneDataSetWaitCheckMs);
		needsWait = !writesQuiesced && !waitedForWrites && StructuredBufferUpdateNeedsWait(resource, data, size, stride);
	}
	if (needsWait)
	{
		const auto waitStart = std::chrono::steady_clock::now();
		renderer.WaitForCommandsTracked("scene_data_upload");
		const double waitMs = DurationMs(waitStart, std::chrono::steady_clock::now());
		renderer.mLastPerfShellTraceStats.sceneDataSetWaitMs += waitMs;
		NoteSceneDataWaitEvent(renderer.mLastPerfShellTraceStats, reason, bufferLabel, waitMs);
		renderer.mLastPerfShellTraceStats.sceneDataSetWaitCount++;
		waitedForWrites = true;
	}

	bool updated = false;
	{
		ScopedPtPerfTimer uploadTimer(uploadMs);
		updated = EnsureStructuredBuffer(
			renderer,
			resource,
			stats,
			data,
			size,
			stride,
			usage,
			after,
			writesQuiesced || waitedForWrites,
			"scene_data_upload");
	}
	if (updated)
	{
		requestedBytes += size;
		uploadedBytes += stats.bytesUploadedLastFrame;
		renderer.mLastPerfShellTraceStats.sceneDataSetResourceGrowEvents += stats.growEventsLastFrame;
		renderer.mLastPerfShellTraceStats.sceneDataSetResourceOverwriteEvents += stats.overwriteEventsLastFrame;
	}
	return updated;
}

bool NRISceneUploadManager::UpdateReprojectionBuffer(NRIRenderer& renderer, bool* ioWaitedForWrites, bool allowSceneDataRing)
{
	NRIReprojectionData data = {};
	std::memcpy(data.currentViewToClip, renderer.mCurrentViewToClip, sizeof(data.currentViewToClip));
	std::memcpy(data.previousViewToClip, renderer.mPreviousViewToClip, sizeof(data.previousViewToClip));
	std::memcpy(data.currentWorldToView, renderer.mCurrentWorldToView, sizeof(data.currentWorldToView));
	std::memcpy(data.previousWorldToView, renderer.mPreviousWorldToView, sizeof(data.previousWorldToView));
	std::memcpy(data.currentJitter, renderer.mCurrentJitter, sizeof(data.currentJitter));
	std::memcpy(data.previousJitter, renderer.mPreviousJitter, sizeof(data.previousJitter));

	NRISceneDataFrameSlot* sceneDataSlot =
		allowSceneDataRing && renderer.ShouldUseSceneDataFrameRing() ? &renderer.GetCurrentSceneDataFrameSlot() : nullptr;
	NRIBufferResource& reprojectionBuffer = sceneDataSlot != nullptr ? sceneDataSlot->reprojectionBuffer : renderer.mReprojectionBuffer;
	SceneBufferDebugStats& reprojectionStats = sceneDataSlot != nullptr ? sceneDataSlot->reprojectionStats : renderer.mReprojectionBufferStats;

	WaitIfStructuredUpdateNeedsIt(renderer, reprojectionBuffer, &data, sizeof(data), sizeof(data), sceneDataSlot != nullptr ? nullptr : ioWaitedForWrites);
	if (!renderer.EnsureStructuredBuffer(
		reprojectionBuffer,
		reprojectionStats,
		&data,
		sizeof(data),
		sizeof(data),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIResourceComputeShaderResourceAccess(),
		sceneDataSlot != nullptr || (ioWaitedForWrites != nullptr && *ioWaitedForWrites),
		"scene_data_upload"))
	{
		return false;
	}

	return UpdateSceneDataDescriptorSlot(renderer, 18, reprojectionBuffer.shaderView, "reprojection_refresh");
}

bool NRISceneUploadManager::UpdateVisibleChunkBuffer(NRIRenderer& renderer, bool* ioWaitedForWrites, bool allowSceneDataRing)
{
	const uint32_t defaultVisibleChunkWord = 0u;
	const void* visibleChunkData = renderer.mCurrentVisibleChunkWords.empty() ? (const void*)&defaultVisibleChunkWord : renderer.mCurrentVisibleChunkWords.data();
	const size_t visibleChunkSize = renderer.mCurrentVisibleChunkWords.empty() ? sizeof(uint32_t) : renderer.mCurrentVisibleChunkWords.size() * sizeof(uint32_t);

	NRISceneDataFrameSlot* sceneDataSlot =
		allowSceneDataRing && renderer.ShouldUseSceneDataFrameRing() ? &renderer.GetCurrentSceneDataFrameSlot() : nullptr;
	NRIBufferResource& visibleChunkBuffer = sceneDataSlot != nullptr ? sceneDataSlot->visibleChunkBuffer : renderer.mVisibleChunkBuffer;
	SceneBufferDebugStats& visibleChunkStats = sceneDataSlot != nullptr ? sceneDataSlot->visibleChunkStats : renderer.mVisibleChunkBufferStats;

	WaitIfStructuredUpdateNeedsIt(renderer, visibleChunkBuffer, visibleChunkData, visibleChunkSize, sizeof(uint32_t), sceneDataSlot != nullptr ? nullptr : ioWaitedForWrites);
	if (!renderer.EnsureStructuredBuffer(
		visibleChunkBuffer,
		visibleChunkStats,
		visibleChunkData,
		visibleChunkSize,
		sizeof(uint32_t),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIResourceComputeShaderResourceAccess(),
		sceneDataSlot != nullptr || (ioWaitedForWrites != nullptr && *ioWaitedForWrites),
		"scene_data_upload"))
	{
		return false;
	}

	return UpdateSceneDataDescriptorSlot(renderer, 19, visibleChunkBuffer.shaderView, "visible_chunk_refresh");
}

bool NRISceneUploadManager::UpdateVisibleFlatPlaneBuffer(NRIRenderer& renderer, bool* ioWaitedForWrites, bool allowSceneDataRing)
{
	const uint32_t defaultVisibleFlatPlaneWord = 0u;
	const void* visibleFlatPlaneData = renderer.mCurrentVisibleFlatPlaneWords.empty() ? (const void*)&defaultVisibleFlatPlaneWord : renderer.mCurrentVisibleFlatPlaneWords.data();
	const size_t visibleFlatPlaneSize = renderer.mCurrentVisibleFlatPlaneWords.empty() ? sizeof(uint32_t) : renderer.mCurrentVisibleFlatPlaneWords.size() * sizeof(uint32_t);

	NRISceneDataFrameSlot* sceneDataSlot =
		allowSceneDataRing && renderer.ShouldUseSceneDataFrameRing() ? &renderer.GetCurrentSceneDataFrameSlot() : nullptr;
	NRIBufferResource& visibleFlatPlaneBuffer = sceneDataSlot != nullptr ? sceneDataSlot->visibleFlatPlaneBuffer : renderer.mVisibleFlatPlaneBuffer;
	SceneBufferDebugStats& visibleFlatPlaneStats = sceneDataSlot != nullptr ? sceneDataSlot->visibleFlatPlaneStats : renderer.mVisibleFlatPlaneBufferStats;

	WaitIfStructuredUpdateNeedsIt(renderer, visibleFlatPlaneBuffer, visibleFlatPlaneData, visibleFlatPlaneSize, sizeof(uint32_t), sceneDataSlot != nullptr ? nullptr : ioWaitedForWrites);
	if (!renderer.EnsureStructuredBuffer(
		visibleFlatPlaneBuffer,
		visibleFlatPlaneStats,
		visibleFlatPlaneData,
		visibleFlatPlaneSize,
		sizeof(uint32_t),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIResourceComputeShaderResourceAccess(),
		sceneDataSlot != nullptr || (ioWaitedForWrites != nullptr && *ioWaitedForWrites),
		"scene_data_upload"))
	{
		return false;
	}

	return UpdateSceneDataDescriptorSlot(renderer, 20, visibleFlatPlaneBuffer.shaderView, "visible_flat_refresh");
}

bool NRISceneUploadManager::UpdateSpatialAbsenceBuffer(NRIRenderer& renderer, bool* ioWaitedForWrites, bool allowSceneDataRing)
{
	static const NRISpatialAbsenceGpuRecord defaultFailOpenRecord = {};
	static const NRISpatialAbsenceGpuBlock defaultFailOpenTypedBlock = {};
	const std::vector<NRISpatialAbsenceGpuRecord>& records = renderer.mSpatialAbsenceGate.GetSnapshot().gpuRecords;
	const void* data = records.empty() ? (const void*)&defaultFailOpenRecord : records.data();
	const size_t size = records.empty() ? sizeof(defaultFailOpenRecord) : records.size() * sizeof(NRISpatialAbsenceGpuRecord);
	const std::vector<NRISpatialAbsenceGpuBlock>& typedBlocks = renderer.mSpatialAbsenceGpuSnapshot.blocks;
	const void* typedData = typedBlocks.empty() ? (const void*)&defaultFailOpenTypedBlock : typedBlocks.data();
	const size_t typedSize = typedBlocks.empty() ? sizeof(defaultFailOpenTypedBlock) :
		typedBlocks.size() * sizeof(NRISpatialAbsenceGpuBlock);

	NRISceneDataFrameSlot* sceneDataSlot =
		allowSceneDataRing && renderer.ShouldUseSceneDataFrameRing() ? &renderer.GetCurrentSceneDataFrameSlot() : nullptr;
	NRIBufferResource& buffer = sceneDataSlot != nullptr ? sceneDataSlot->spatialAbsenceBuffer : renderer.mSpatialAbsenceBuffer;
	SceneBufferDebugStats& stats = sceneDataSlot != nullptr ? sceneDataSlot->spatialAbsenceStats : renderer.mSpatialAbsenceBufferStats;
	NRIBufferResource& typedBuffer = sceneDataSlot != nullptr ? sceneDataSlot->spatialAbsenceTypedBuffer : renderer.mSpatialAbsenceTypedBuffer;
	SceneBufferDebugStats& typedStats = sceneDataSlot != nullptr ? sceneDataSlot->spatialAbsenceTypedStats : renderer.mSpatialAbsenceTypedBufferStats;

	WaitIfStructuredUpdateNeedsIt(renderer, buffer, data, size, sizeof(NRISpatialAbsenceGpuRecord), sceneDataSlot != nullptr ? nullptr : ioWaitedForWrites);
	if (!renderer.EnsureStructuredBuffer(
		buffer,
		stats,
		data,
		size,
		sizeof(NRISpatialAbsenceGpuRecord),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIResourceComputeShaderResourceAccess(),
		sceneDataSlot != nullptr || (ioWaitedForWrites != nullptr && *ioWaitedForWrites),
		"scene_data_upload"))
	{
		return false;
	}

	WaitIfStructuredUpdateNeedsIt(renderer, typedBuffer, typedData, typedSize, sizeof(NRISpatialAbsenceGpuBlock),
		sceneDataSlot != nullptr ? nullptr : ioWaitedForWrites);
	if (!renderer.EnsureStructuredBuffer(
		typedBuffer,
		typedStats,
		typedData,
		typedSize,
		sizeof(NRISpatialAbsenceGpuBlock),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIResourceComputeShaderResourceAccess(),
		sceneDataSlot != nullptr || (ioWaitedForWrites != nullptr && *ioWaitedForWrites),
		"scene_data_upload"))
	{
		return false;
	}

	return UpdateSceneDataDescriptorSlot(renderer, NRI_SCENE_DATA_SPATIAL_ABSENCE_RAW_SLOT,
		buffer.shaderView, "spatial_absence_refresh") &&
		UpdateSceneDataDescriptorSlot(renderer, NRI_SCENE_DATA_SPATIAL_ABSENCE_TYPED_SLOT,
			typedBuffer.shaderView, "spatial_absence_typed_refresh");
}

bool NRIRenderer::UploadSceneBuffers(
	const nri_scene::GeometryData& geometry,
	const std::vector<nri_scene::MaterialData>& materials,
	const std::vector<SceneBufferUploadDomainSpan>* domainSpans)
{
	return UploadSceneBuffers(GetCurrentSceneUploadBufferRingSlot(), geometry, materials, domainSpans);
}

bool NRIRenderer::UploadSceneBuffers(
	SceneUploadBufferRingSlot& uploadSlot,
	const nri_scene::GeometryData& geometry,
	const std::vector<nri_scene::MaterialData>& materials,
	const std::vector<SceneBufferUploadDomainSpan>* domainSpans)
{
	Clocker clock(NriPTSceneBuffers);
	NRIBufferResource& vertexBuffer = uploadSlot.vertexBuffer;
	NRIBufferResource& indexBuffer = uploadSlot.indexBuffer;
	NRIBufferResource& primitiveBuffer = uploadSlot.primitiveBuffer;
	NRIBufferResource& materialBuffer = uploadSlot.materialBuffer;
	std::vector<uint8_t>& vertexMirror = uploadSlot.vertexMirror;
	std::vector<uint8_t>& indexMirror = uploadSlot.indexMirror;
	std::vector<uint8_t>& primitiveMirror = uploadSlot.primitiveMirror;
	std::vector<uint8_t>& materialMirror = uploadSlot.materialMirror;
	mVertexBufferStats.bytesUploadedLastFrame = 0;
	mVertexBufferStats.growEventsLastFrame = 0;
	mVertexBufferStats.overwriteEventsLastFrame = 0;
	mVertexBufferStats.growthOldBytesLastFrame = 0;
	mVertexBufferStats.growthRequestedBytesLastFrame = 0;
	mVertexBufferStats.growthAllocatedBytesLastFrame = 0;
	mIndexBufferStats.bytesUploadedLastFrame = 0;
	mIndexBufferStats.growEventsLastFrame = 0;
	mIndexBufferStats.overwriteEventsLastFrame = 0;
	mIndexBufferStats.growthOldBytesLastFrame = 0;
	mIndexBufferStats.growthRequestedBytesLastFrame = 0;
	mIndexBufferStats.growthAllocatedBytesLastFrame = 0;
	mPrimitiveBufferStats.bytesUploadedLastFrame = 0;
	mPrimitiveBufferStats.growEventsLastFrame = 0;
	mPrimitiveBufferStats.overwriteEventsLastFrame = 0;
	mPrimitiveBufferStats.growthOldBytesLastFrame = 0;
	mPrimitiveBufferStats.growthRequestedBytesLastFrame = 0;
	mPrimitiveBufferStats.growthAllocatedBytesLastFrame = 0;
	mMaterialBufferStats.bytesUploadedLastFrame = 0;
	mMaterialBufferStats.growEventsLastFrame = 0;
	mMaterialBufferStats.overwriteEventsLastFrame = 0;
	mMaterialBufferStats.growthOldBytesLastFrame = 0;
	mMaterialBufferStats.growthRequestedBytesLastFrame = 0;
	mMaterialBufferStats.growthAllocatedBytesLastFrame = 0;
	{
		mLastPerfShellTraceStats.sceneSelectBufferUploadVertexRequestedBytes = geometry.vertices.size() * sizeof(nri_scene::SceneVertex);
		mLastPerfShellTraceStats.sceneSelectBufferUploadIndexRequestedBytes = geometry.indices.size() * sizeof(uint32_t);
		mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRequestedBytes = geometry.primitives.size() * sizeof(nri_scene::PrimitiveData);
		mLastPerfShellTraceStats.sceneSelectBufferUploadMaterialRequestedBytes = materials.size() * sizeof(nri_scene::MaterialData);
	}
	using SceneUploadBufferKind = NRISceneUploadBufferKind;
	const auto getDomainEntry = [&](SceneBufferUploadDomain domain) -> PerfShellTraceStats::SceneBufferUploadDomainTraceEntry*
	{
		const size_t domainIndex = (size_t)domain;
		if (domainIndex >= SceneBufferUploadDomainCount)
		{
			return nullptr;
		}
		return &mLastPerfShellTraceStats.sceneSelectBufferUploadDomains[domainIndex];
	};
	const auto getSpanByteRange =
		[](const SceneBufferUploadDomainSpan& span, SceneUploadBufferKind kind, uint64_t& outOffset, uint64_t& outSize)
	{
		switch (kind)
		{
		case SceneUploadBufferKind::Vertex:
			outOffset = (uint64_t)span.vertexOffset * sizeof(nri_scene::SceneVertex);
			outSize = (uint64_t)span.vertexCount * sizeof(nri_scene::SceneVertex);
			break;
		case SceneUploadBufferKind::Index:
			outOffset = (uint64_t)span.indexOffset * sizeof(uint32_t);
			outSize = (uint64_t)span.indexCount * sizeof(uint32_t);
			break;
		case SceneUploadBufferKind::Primitive:
			outOffset = (uint64_t)span.primitiveOffset * sizeof(nri_scene::PrimitiveData);
			outSize = (uint64_t)span.primitiveCount * sizeof(nri_scene::PrimitiveData);
			break;
		case SceneUploadBufferKind::Material:
			outOffset = (uint64_t)span.materialOffset * sizeof(nri_scene::MaterialData);
			outSize = (uint64_t)span.materialCount * sizeof(nri_scene::MaterialData);
			break;
		case SceneUploadBufferKind::Provenance:
		case SceneUploadBufferKind::Count:
			outOffset = 0;
			outSize = 0;
			break;
		}
	};
	const auto addDomainPayload =
		[&](SceneUploadBufferKind kind, bool skipped)
	{
		if (domainSpans == nullptr)
		{
			return;
		}
		for (const SceneBufferUploadDomainSpan& span : *domainSpans)
		{
			uint64_t offset = 0;
			uint64_t size = 0;
			getSpanByteRange(span, kind, offset, size);
			if (size == 0)
			{
				continue;
			}
			auto* domain = getDomainEntry(span.domain);
			if (domain == nullptr)
			{
				continue;
			}
			domain->payloadBytes += size;
			domain->hashChecks++;
			if (!skipped)
			{
				domain->hashMisses++;
			}
			switch (kind)
			{
			case SceneUploadBufferKind::Vertex: domain->vertexPayloadBytes += size; break;
			case SceneUploadBufferKind::Index: domain->indexPayloadBytes += size; break;
			case SceneUploadBufferKind::Primitive: domain->primitivePayloadBytes += size; break;
			case SceneUploadBufferKind::Material: domain->materialPayloadBytes += size; break;
			}
		}
	};
	const auto addDomainFullUpload =
		[&](SceneUploadBufferKind kind)
	{
		if (domainSpans == nullptr)
		{
			return;
		}
		for (const SceneBufferUploadDomainSpan& span : *domainSpans)
		{
			uint64_t offset = 0;
			uint64_t size = 0;
			getSpanByteRange(span, kind, offset, size);
			if (size == 0)
			{
				continue;
			}
			auto* domain = getDomainEntry(span.domain);
			if (domain == nullptr)
			{
				continue;
			}
			domain->uploadedBytes += size;
			if (kind == SceneUploadBufferKind::Primitive)
			{
				domain->primitiveUploadedBytes += size;
			}
			else if (kind == SceneUploadBufferKind::Material)
			{
				domain->materialUploadedBytes += size;
			}
		}
	};
	const auto addDomainRangeBytes =
		[&](SceneUploadBufferKind kind, const std::vector<SceneUploadDirtyRange>& ranges, bool countDirty)
	{
		if (domainSpans == nullptr)
		{
			return;
		}
		for (const SceneBufferUploadDomainSpan& span : *domainSpans)
		{
			uint64_t spanOffset = 0;
			uint64_t spanSize = 0;
			getSpanByteRange(span, kind, spanOffset, spanSize);
			if (spanSize == 0)
			{
				continue;
			}
			const uint64_t spanEnd = spanOffset + spanSize;
			uint64_t domainBytes = 0;
			uint32_t domainRanges = 0;
			for (const SceneUploadDirtyRange& range : ranges)
			{
				const uint64_t rangeEnd = range.byteOffset + range.size;
				const uint64_t overlapStart = std::max(spanOffset, range.byteOffset);
				const uint64_t overlapEnd = std::min(spanEnd, rangeEnd);
				if (overlapEnd > overlapStart)
				{
					domainBytes += overlapEnd - overlapStart;
					domainRanges++;
				}
			}
			if (domainBytes == 0)
			{
				continue;
			}
			auto* domain = getDomainEntry(span.domain);
			if (domain == nullptr)
			{
				continue;
			}
			if (countDirty)
			{
				domain->dirtyRanges += domainRanges;
				domain->dirtyChangedBytes += domainBytes;
				domain->dirtyUploadedBytes += domainBytes;
			}
			else
			{
				domain->uploadedBytes += domainBytes;
				if (kind == SceneUploadBufferKind::Primitive)
				{
					domain->primitiveUploadedBytes += domainBytes;
				}
				else if (kind == SceneUploadBufferKind::Material)
				{
					domain->materialUploadedBytes += domainBytes;
				}
			}
		}
	};
	const auto addDomainWait =
		[&](SceneUploadBufferKind kind, double waitMs)
	{
		if (domainSpans == nullptr || waitMs <= 0.0)
		{
			return;
		}
		uint64_t totalBytes = 0;
		for (const SceneBufferUploadDomainSpan& span : *domainSpans)
		{
			uint64_t offset = 0;
			uint64_t size = 0;
			getSpanByteRange(span, kind, offset, size);
			totalBytes += size;
		}
		if (totalBytes == 0)
		{
			return;
		}
		for (const SceneBufferUploadDomainSpan& span : *domainSpans)
		{
			uint64_t offset = 0;
			uint64_t size = 0;
			getSpanByteRange(span, kind, offset, size);
			if (size == 0)
			{
				continue;
			}
			auto* domain = getDomainEntry(span.domain);
			if (domain != nullptr)
			{
				domain->waitMs += waitMs * ((double)size / (double)totalBytes);
			}
		}
	};
	const auto addDomainGrowth =
		[&](SceneUploadBufferKind kind, uint64_t requestedBytes, uint64_t allocatedBytes)
	{
		if (domainSpans == nullptr || requestedBytes == 0 || allocatedBytes == 0)
		{
			return;
		}
		uint64_t totalBytes = 0;
		for (const SceneBufferUploadDomainSpan& span : *domainSpans)
		{
			uint64_t offset = 0;
			uint64_t size = 0;
			getSpanByteRange(span, kind, offset, size);
			totalBytes += size;
		}
		if (totalBytes == 0)
		{
			return;
		}
		for (const SceneBufferUploadDomainSpan& span : *domainSpans)
		{
			uint64_t offset = 0;
			uint64_t size = 0;
			getSpanByteRange(span, kind, offset, size);
			if (size == 0)
			{
				continue;
			}
			auto* domain = getDomainEntry(span.domain);
			if (domain != nullptr)
			{
				domain->growthEvents++;
				domain->growthRequestedBytes += (uint64_t)((double)requestedBytes * ((double)size / (double)totalBytes));
				domain->growthAllocatedBytes += (uint64_t)((double)allocatedBytes * ((double)size / (double)totalBytes));
			}
		}
	};
	const auto buildProducerPayloadHash =
		[&](SceneUploadBufferKind kind, const void* payloadData, uint64_t payloadSize, uint32_t payloadStride, uint64_t extraIdentity, uint64_t& outHash, bool& outQuarantined) -> bool
	{
		outQuarantined = false;
		mLastPerfShellTraceStats.sceneSelectBufferUploadProducerStampChecks++;
		NRISceneUploadPayloadView payload = {};
		payload.data = payloadData;
		payload.byteSize = payloadSize;
		payload.stride = payloadStride;
		payload.extraIdentity = extraIdentity;
		const NRISceneUploadIdentityBuildResult built =
			BuildNRISceneUploadPayloadIdentity(domainSpans, kind, payload, &mSceneUploadIdentityValidator);
		outQuarantined = built.stats.quarantinedSpans != 0;
		mLastPerfShellTraceStats.sceneSelectBufferUploadProducerStampStampedBytes += built.stats.stampedBytes;
		mLastPerfShellTraceStats.sceneSelectBufferUploadProducerStampFallbackBytes += built.stats.fallbackBytes;
		mLastPerfShellTraceStats.sceneSelectBufferUploadProducerStampFallbackSpans += built.stats.fallbackSpans;
		mLastPerfShellTraceStats.sceneSelectBufferUploadProducerStampCoverageRejects += built.stats.coverageRejects;
		for (size_t domainIndex = 0; domainIndex < built.stats.domainChecks.size(); ++domainIndex)
		{
			auto& domain = mLastPerfShellTraceStats.sceneSelectBufferUploadDomains[domainIndex];
			domain.stampChecks += built.stats.domainChecks[domainIndex];
			domain.stampMisses += built.stats.domainFallbacks[domainIndex];
		}
		if (!built.completeCoverage)
		{
			mLastPerfShellTraceStats.sceneSelectBufferUploadProducerStampFallbacks++;
			return false;
		}
		outHash = built.identity;
		if (built.usedOnlyProducerStamps)
		{
			mLastPerfShellTraceStats.sceneSelectBufferUploadProducerStampUses++;
		}
		else
		{
			mLastPerfShellTraceStats.sceneSelectBufferUploadProducerStampFallbacks++;
		}
		switch (kind)
		{
		case SceneUploadBufferKind::Vertex:
			mLastPerfShellTraceStats.sceneSelectBufferUploadProducerStampVertexUses++;
			break;
		case SceneUploadBufferKind::Index:
			mLastPerfShellTraceStats.sceneSelectBufferUploadProducerStampIndexUses++;
			break;
		case SceneUploadBufferKind::Primitive:
			mLastPerfShellTraceStats.sceneSelectBufferUploadProducerStampPrimitiveUses++;
			break;
		case SceneUploadBufferKind::Material:
			mLastPerfShellTraceStats.sceneSelectBufferUploadProducerStampMaterialUses++;
			break;
		case SceneUploadBufferKind::Provenance:
		case SceneUploadBufferKind::Count:
			break;
		}
		return true;
	};
	const auto buildProducerProvenanceHash =
		[&](const std::vector<nri_scene::SurfaceProvenance>& provenance, uint64_t& outHash, bool& outQuarantined) -> bool
	{
		return buildProducerPayloadHash(
			SceneUploadBufferKind::Provenance,
			provenance.empty() ? nullptr : provenance.data(),
			(uint64_t)provenance.size() * sizeof(nri_scene::SurfaceProvenance),
			sizeof(nri_scene::SurfaceProvenance),
			0,
			outHash,
			outQuarantined);
	};
	const uint64_t primitiveInputSize = geometry.primitives.size() * sizeof(nri_scene::PrimitiveData);
	uint64_t primitiveInputPayloadHash = 0;
	uint64_t primitiveProvenanceHash = 0;
	uint64_t primitiveVisibilityIdentityHash = 0;
	bool primitiveInputQuarantined = false;
	bool primitiveProvenanceQuarantined = false;
	bool forceValidationVertexUpload = false;
	bool forceValidationIndexUpload = false;
	bool forceValidationPrimitiveUpload = false;
	bool forceValidationMaterialUpload = false;
	const std::vector<nri_scene::PrimitiveData>* gpuPrimitives = nullptr;
	{
		ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteMs);
		if (buildProducerPayloadHash(SceneUploadBufferKind::Primitive, geometry.primitives.data(), primitiveInputSize, sizeof(nri_scene::PrimitiveData), 0, primitiveInputPayloadHash, primitiveInputQuarantined))
		{
			mLastPerfShellTraceStats.sceneSelectBufferUploadProducerStampRewritePrimitiveUses++;
		}
		else
		{
			ScopedPtPerfTimer hashTimer(mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewritePrimitiveHashMs);
			primitiveInputPayloadHash = HashUploadPayloadBytes(geometry.primitives.data(), primitiveInputSize);
		}
		if (buildProducerProvenanceHash(geometry.primitiveProvenance, primitiveProvenanceHash, primitiveProvenanceQuarantined))
		{
			mLastPerfShellTraceStats.sceneSelectBufferUploadProducerStampRewriteProvenanceUses++;
		}
		else
		{
			ScopedPtPerfTimer hashTimer(mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteProvenanceHashMs);
			primitiveProvenanceHash = HashPrimitiveRewriteProvenancePayload(geometry.primitiveProvenance);
		}
		{
			ScopedPtPerfTimer hashTimer(mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteVisibilityHashMs);
			const bool cacheHit =
				mPrimitiveVisibilityIdentityCache.valid &&
				mPrimitiveVisibilityIdentityCache.mapValid == mMapWorld.valid &&
				mPrimitiveVisibilityIdentityCache.mapBuildSerial == mMapWorld.buildSerial &&
				mPrimitiveVisibilityIdentityCache.chunkCount == mMapWorld.chunks.size() &&
				mPrimitiveVisibilityIdentityCache.statsChunkCount == mMapWorld.stats.chunkCount;
			if (cacheHit)
			{
				primitiveVisibilityIdentityHash = mPrimitiveVisibilityIdentityCache.identity;
				mLastPerfShellTraceStats.sceneSelectBufferUploadVisibilityIdentityCacheHits++;
			}
			else
			{
				primitiveVisibilityIdentityHash = HashPrimitiveRewriteVisibilityIdentity(mMapWorld);
				mPrimitiveVisibilityIdentityCache.valid = true;
				mPrimitiveVisibilityIdentityCache.mapValid = mMapWorld.valid;
				mPrimitiveVisibilityIdentityCache.mapBuildSerial = mMapWorld.buildSerial;
				mPrimitiveVisibilityIdentityCache.chunkCount = (uint32_t)mMapWorld.chunks.size();
				mPrimitiveVisibilityIdentityCache.statsChunkCount = mMapWorld.stats.chunkCount;
				mPrimitiveVisibilityIdentityCache.identity = primitiveVisibilityIdentityHash;
				mLastPerfShellTraceStats.sceneSelectBufferUploadVisibilityIdentityCacheBuilds++;
			}
			const int visibilityValidationInterval = (int)nri_ptsceneuploadvalidateinterval;
			if (cacheHit && visibilityValidationInterval > 0 && mFrameIndex % (uint64_t)visibilityValidationInterval == 0)
			{
				const uint64_t exactVisibilityIdentity = HashPrimitiveRewriteVisibilityIdentity(mMapWorld);
				mLastPerfShellTraceStats.sceneSelectBufferUploadVisibilityIdentityValidationChecks++;
				if (exactVisibilityIdentity != primitiveVisibilityIdentityHash)
				{
					mLastPerfShellTraceStats.sceneSelectBufferUploadVisibilityIdentityValidationMismatches++;
					primitiveVisibilityIdentityHash = exactVisibilityIdentity;
					mPrimitiveVisibilityIdentityCache.identity = exactVisibilityIdentity;
				}
			}
		}
		const int identityValidationInterval = (int)nri_ptsceneuploadvalidateinterval;
		if (domainSpans != nullptr && identityValidationInterval > 0 && mFrameIndex % (uint64_t)identityValidationInterval == 0)
		{
			NRISceneUploadIdentityValidationStats validation = {};
			NRISceneUploadPayloadView primitivePayload = {};
			primitivePayload.data = geometry.primitives.data();
			primitivePayload.byteSize = primitiveInputSize;
			primitivePayload.stride = sizeof(nri_scene::PrimitiveData);
			if (!mSceneUploadIdentityValidator.Validate(*domainSpans, SceneUploadBufferKind::Primitive, primitivePayload, validation))
			{
				primitiveInputPayloadHash = HashUploadPayloadBytes(geometry.primitives.data(), primitiveInputSize);
				forceValidationPrimitiveUpload = true;
			}
			mLastPerfShellTraceStats.sceneSelectBufferUploadIdentityValidationChecks += validation.checks;
			mLastPerfShellTraceStats.sceneSelectBufferUploadIdentityValidationMismatches += validation.mismatches;

			validation = {};
			NRISceneUploadPayloadView provenancePayload = {};
			provenancePayload.data = geometry.primitiveProvenance.data();
			provenancePayload.byteSize = (uint64_t)geometry.primitiveProvenance.size() * sizeof(nri_scene::SurfaceProvenance);
			provenancePayload.stride = sizeof(nri_scene::SurfaceProvenance);
			if (!mSceneUploadIdentityValidator.Validate(*domainSpans, SceneUploadBufferKind::Provenance, provenancePayload, validation))
			{
				primitiveProvenanceHash = HashPrimitiveRewriteProvenancePayload(geometry.primitiveProvenance);
				forceValidationPrimitiveUpload = true;
			}
			mLastPerfShellTraceStats.sceneSelectBufferUploadIdentityValidationChecks += validation.checks;
			mLastPerfShellTraceStats.sceneSelectBufferUploadIdentityValidationMismatches += validation.mismatches;
		}
		mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteCacheChecks++;
		if (mSelectPrimitiveRewriteCache.valid &&
			mSelectPrimitiveRewriteCache.primitivePayloadHash == primitiveInputPayloadHash &&
			mSelectPrimitiveRewriteCache.primitiveProvenanceHash == primitiveProvenanceHash &&
			mSelectPrimitiveRewriteCache.visibilityIdentityHash == primitiveVisibilityIdentityHash &&
			mSelectPrimitiveRewriteCache.primitiveCount == geometry.primitives.size() &&
			mSelectPrimitiveRewriteCache.primitives.size() == geometry.primitives.size())
		{
			mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteCacheHits++;
			gpuPrimitives = &mSelectPrimitiveRewriteCache.primitives;
		}
		else
		{
			if (!mSelectPrimitiveRewriteCache.valid)
			{
				mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteCacheRejectInvalid++;
			}
			else
			{
				if (mSelectPrimitiveRewriteCache.primitivePayloadHash != primitiveInputPayloadHash)
				{
					mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteCacheRejectPrimitive++;
				}
				if (mSelectPrimitiveRewriteCache.primitiveProvenanceHash != primitiveProvenanceHash)
				{
					mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteCacheRejectProvenance++;
				}
				if (mSelectPrimitiveRewriteCache.visibilityIdentityHash != primitiveVisibilityIdentityHash)
				{
					mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteCacheRejectVisibility++;
				}
				if (mSelectPrimitiveRewriteCache.primitiveCount != geometry.primitives.size() ||
					mSelectPrimitiveRewriteCache.primitives.size() != geometry.primitives.size())
				{
					mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteCacheRejectCount++;
				}
			}
			mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteCacheMisses++;
			{
				ScopedPtPerfTimer copyTimer(mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteCopyMs);
				mSelectPrimitiveRewriteCache.primitives.assign(geometry.primitives.begin(), geometry.primitives.end());
			}
			{
				ScopedPtPerfTimer resolveTimer(mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteResolveMs);
				std::vector<nri_scene::PrimitiveData>& rewrittenPrimitives = mSelectPrimitiveRewriteCache.primitives;
				const size_t primitiveCount = std::min(rewrittenPrimitives.size(), geometry.primitiveProvenance.size());
				for (size_t primitiveIndex = 0; primitiveIndex < primitiveCount; ++primitiveIndex)
				{
					const nri_scene::SurfaceProvenance& provenance = geometry.primitiveProvenance[primitiveIndex];
					mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteResolvePrimitives++;
					int32_t chunkIndex = provenance.mapChunkIndex;
					if (chunkIndex >= 0)
					{
						mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteResolveMapChunk++;
					}
					else
					{
						mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteResolveSectorFallback++;
						chunkIndex = nri_static_scene_geometry::FindMapChunkIndexForSector(mMapWorld, provenance.sectorIndex);
						if (chunkIndex < 0)
						{
							mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteResolveSectorMiss++;
						}
					}
					rewrittenPrimitives[primitiveIndex].reserved0 = chunkIndex >= 0 ? (uint32_t)chunkIndex : UINT32_MAX;
				}
				for (size_t primitiveIndex = primitiveCount; primitiveIndex < rewrittenPrimitives.size(); ++primitiveIndex)
				{
					rewrittenPrimitives[primitiveIndex].reserved0 = UINT32_MAX;
				}
			}

			{
				ScopedPtPerfTimer storeTimer(mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRewriteStoreMs);
				mSelectPrimitiveRewriteCache.valid = true;
				mSelectPrimitiveRewriteCache.primitivePayloadHash = primitiveInputPayloadHash;
				mSelectPrimitiveRewriteCache.primitiveProvenanceHash = primitiveProvenanceHash;
				mSelectPrimitiveRewriteCache.visibilityIdentityHash = primitiveVisibilityIdentityHash;
				mSelectPrimitiveRewriteCache.primitiveCount = geometry.primitives.size();
			}
			gpuPrimitives = &mSelectPrimitiveRewriteCache.primitives;
		}
	}

	const uint64_t vertexSize = geometry.vertices.size() * sizeof(nri_scene::SceneVertex);
	const uint64_t indexSize = geometry.indices.size() * sizeof(uint32_t);
	const uint64_t primitiveSize = gpuPrimitives != nullptr ? gpuPrimitives->size() * sizeof(nri_scene::PrimitiveData) : 0;
	const uint64_t materialSize = materials.size() * sizeof(nri_scene::MaterialData);
	uint64_t vertexPayloadHash = 0;
	uint64_t indexPayloadHash = 0;
	uint64_t primitivePayloadHash = 0;
	uint64_t materialPayloadHash = 0;
	uint64_t finalMaterialProductIdentity = 0;
	bool vertexQuarantined = false;
	bool indexQuarantined = false;
	bool primitiveQuarantined = false;
	bool materialQuarantined = false;
	{
		ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashMs);
		if (!buildProducerPayloadHash(SceneUploadBufferKind::Vertex, geometry.vertices.data(), vertexSize, sizeof(nri_scene::SceneVertex), 0, vertexPayloadHash, vertexQuarantined))
		{
			vertexPayloadHash = HashUploadPayloadBytes(geometry.vertices.data(), vertexSize);
		}
		if (!buildProducerPayloadHash(SceneUploadBufferKind::Index, geometry.indices.data(), indexSize, sizeof(uint32_t), 0, indexPayloadHash, indexQuarantined))
		{
			indexPayloadHash = HashUploadPayloadBytes(geometry.indices.data(), indexSize);
		}
		if (!buildProducerPayloadHash(SceneUploadBufferKind::Primitive, gpuPrimitives != nullptr && !gpuPrimitives->empty() ? gpuPrimitives->data() : nullptr, primitiveSize, sizeof(nri_scene::PrimitiveData), primitiveVisibilityIdentityHash, primitivePayloadHash, primitiveQuarantined))
		{
			primitivePayloadHash = HashUploadPayloadBytes(gpuPrimitives != nullptr && !gpuPrimitives->empty() ? gpuPrimitives->data() : nullptr, primitiveSize);
		}
		finalMaterialProductIdentity = HashUploadPayloadBytes(materials.data(), materialSize);
		if (!buildProducerPayloadHash(SceneUploadBufferKind::Material, materials.data(), materialSize, sizeof(nri_scene::MaterialData), finalMaterialProductIdentity, materialPayloadHash, materialQuarantined))
		{
			materialPayloadHash = HashUploadPayloadBytes(materials.data(), materialSize);
		}

		const int identityValidationInterval = (int)nri_ptsceneuploadvalidateinterval;
		if (domainSpans != nullptr && identityValidationInterval > 0 && mFrameIndex % (uint64_t)identityValidationInterval == 0)
		{
			const auto validatePayload =
				[&](SceneUploadBufferKind kind, const void* data, uint64_t size, uint32_t stride, uint64_t extraIdentity, uint64_t& identity, bool& forceUpload)
			{
					NRISceneUploadPayloadView payload = {};
					payload.data = data;
					payload.byteSize = size;
					payload.stride = stride;
					payload.extraIdentity = extraIdentity;
					NRISceneUploadIdentityValidationStats validation = {};
					if (!mSceneUploadIdentityValidator.Validate(*domainSpans, kind, payload, validation))
					{
						identity = HashUploadPayloadBytes(data, size);
						forceUpload = true;
					}
					mLastPerfShellTraceStats.sceneSelectBufferUploadIdentityValidationChecks += validation.checks;
					mLastPerfShellTraceStats.sceneSelectBufferUploadIdentityValidationMismatches += validation.mismatches;
				};
			validatePayload(SceneUploadBufferKind::Vertex, geometry.vertices.data(), vertexSize, sizeof(nri_scene::SceneVertex), 0, vertexPayloadHash, forceValidationVertexUpload);
			validatePayload(SceneUploadBufferKind::Index, geometry.indices.data(), indexSize, sizeof(uint32_t), 0, indexPayloadHash, forceValidationIndexUpload);
			validatePayload(SceneUploadBufferKind::Material, materials.data(), materialSize, sizeof(nri_scene::MaterialData), finalMaterialProductIdentity, materialPayloadHash, forceValidationMaterialUpload);
		}
		if (forceValidationPrimitiveUpload || primitiveInputQuarantined || primitiveProvenanceQuarantined || primitiveQuarantined)
		{
			primitivePayloadHash = HashUploadPayloadBytes(
				gpuPrimitives != nullptr && !gpuPrimitives->empty() ? gpuPrimitives->data() : nullptr,
				primitiveSize);
		}
	}
	forceValidationVertexUpload = forceValidationVertexUpload || vertexQuarantined;
	forceValidationIndexUpload = forceValidationIndexUpload || indexQuarantined;
	forceValidationPrimitiveUpload = forceValidationPrimitiveUpload || primitiveInputQuarantined || primitiveProvenanceQuarantined || primitiveQuarantined;
	forceValidationMaterialUpload = forceValidationMaterialUpload || materialQuarantined;

	// Scene upload buffers are ringed by queued frame. The frame shell waits before
	// reusing a queued-frame slot, so the selected slot is safe to overwrite here.
	bool waitedForWrites = true;
	NRISceneUploadDirtyPlan vertexTypedPlan = {};
	NRISceneUploadDirtyPlan indexTypedPlan = {};
	NRISceneUploadDirtyPlan primitiveTypedPlan = {};
	NRISceneUploadDirtyPlan materialTypedPlan = {};
	if (domainSpans != nullptr && uploadSlot.publishedSpanIdentitiesValid)
	{
		const uint64_t maxGapBytes = (uint64_t)(int)nri_ptscenebufferdirtyrangegap;
		vertexTypedPlan = BuildNRISceneUploadDirtyPlan(
			*domainSpans, uploadSlot.publishedSpans, SceneUploadBufferKind::Vertex,
			vertexSize, sizeof(nri_scene::SceneVertex), 0, 0, maxGapBytes);
		indexTypedPlan = BuildNRISceneUploadDirtyPlan(
			*domainSpans, uploadSlot.publishedSpans, SceneUploadBufferKind::Index,
			indexSize, sizeof(uint32_t), 0, 0, maxGapBytes);
		primitiveTypedPlan = BuildNRISceneUploadDirtyPlan(
			*domainSpans, uploadSlot.publishedSpans, SceneUploadBufferKind::Primitive,
			primitiveSize, sizeof(nri_scene::PrimitiveData), primitiveVisibilityIdentityHash,
			uploadSlot.publishedPrimitiveExtraIdentity, maxGapBytes);
		materialTypedPlan = BuildNRISceneUploadDirtyPlan(
			*domainSpans, uploadSlot.publishedSpans, SceneUploadBufferKind::Material,
			materialSize, sizeof(nri_scene::MaterialData), finalMaterialProductIdentity,
			uploadSlot.publishedMaterialExtraIdentity, maxGapBytes);
	}
	const auto notePayloadHashState =
		[&](const NRIBufferResource& resource,
			uint64_t payloadHash,
			uint64_t payloadSize,
			uint32_t payloadStride,
			uint32_t& bufferHitCount,
			uint32_t& bufferSkipCount,
			uint32_t& bufferMissCount) -> bool
	{
		mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashChecks++;
		if (resource.buffer == nullptr || resource.shaderView == nullptr || resource.payloadHash == 0)
		{
			mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashMisses++;
			mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashRejectMissing++;
			bufferMissCount++;
			return false;
		}
		const uint64_t requiredSize = std::max<uint64_t>(payloadSize, payloadStride);
		if (resource.size < requiredSize ||
			resource.usedSize != payloadSize ||
			resource.payloadSize != payloadSize)
		{
			mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashMisses++;
			mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashRejectSize++;
			bufferMissCount++;
			return false;
		}
		if (resource.stride != payloadStride ||
			resource.payloadStride != payloadStride)
		{
			mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashMisses++;
			mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashRejectStride++;
			bufferMissCount++;
			return false;
		}
		if (resource.payloadHash == payloadHash)
		{
			mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashHits++;
			mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashSkips++;
			bufferHitCount++;
			bufferSkipCount++;
			return true;
		}

		mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashMisses++;
		bufferMissCount++;
		return false;
	};
	struct SceneUploadDirtyRangeStats
	{
		uint32_t rawRanges = 0;
		uint32_t coalescedRanges = 0;
		uint32_t rejectedCoalesces = 0;
		uint64_t changedBytes = 0;
		uint64_t uploadedBytes = 0;
		uint64_t gapBytes = 0;
	};
	const auto noteDirtyRanges =
		[&](const std::vector<uint8_t>& mirror,
			const void* bufferData,
			uint64_t bufferSize,
			bool skipUpload,
			bool forceFullDirty,
			std::vector<SceneUploadDirtyRange>* outRanges) -> SceneUploadDirtyRangeStats
	{
		if (outRanges != nullptr)
		{
			outRanges->clear();
		}
		SceneUploadDirtyRangeStats result = {};
		mLastPerfShellTraceStats.sceneSelectBufferUploadDirtyRangeChecks++;
		if (skipUpload)
		{
			mLastPerfShellTraceStats.sceneSelectBufferUploadDirtyRangeSkips++;
			return result;
		}
		if (bufferSize == 0)
		{
			return result;
		}
		if (forceFullDirty || bufferData == nullptr || mirror.empty() || mirror.size() != bufferSize)
		{
			mLastPerfShellTraceStats.sceneSelectBufferUploadDirtyRangeSourceFull++;
			if (forceFullDirty || bufferData == nullptr)
			{
				mLastPerfShellTraceStats.sceneSelectBufferUploadDirtyRangeForcedFull++;
			}
			if (mirror.empty())
			{
				mLastPerfShellTraceStats.sceneSelectBufferUploadDirtyRangeMissingMirror++;
			}
			else if (mirror.size() != bufferSize)
			{
				mLastPerfShellTraceStats.sceneSelectBufferUploadDirtyRangeSizeMismatch++;
			}
			result.rawRanges = 1;
			result.coalescedRanges = 1;
			result.changedBytes = bufferSize;
			result.uploadedBytes = bufferSize;
			if (outRanges != nullptr)
			{
				outRanges->push_back({ 0, bufferSize });
			}
			return result;
		}

		mLastPerfShellTraceStats.sceneSelectBufferUploadDirtyRangeSourceByteScan++;
		const uint64_t maxGapBytes = (uint64_t)(int)nri_ptscenebufferdirtyrangegap;
		const uint8_t* current = static_cast<const uint8_t*>(bufferData);
		const uint8_t* previous = mirror.data();
		const size_t byteCount = (size_t)bufferSize;
		bool hasCoalescedRange = false;
		size_t coalescedStart = 0;
		size_t coalescedEnd = 0;
		size_t cursor = 0;
		while (cursor < byteCount)
		{
			while (cursor < byteCount && current[cursor] == previous[cursor])
			{
				cursor++;
			}
			if (cursor >= byteCount)
			{
				break;
			}
			const size_t rangeStart = cursor;
			while (cursor < byteCount && current[cursor] != previous[cursor])
			{
				cursor++;
			}
			const size_t rangeEnd = cursor;
			result.rawRanges++;
			result.changedBytes += (uint64_t)(rangeEnd - rangeStart);
			if (!hasCoalescedRange)
			{
				hasCoalescedRange = true;
				coalescedStart = rangeStart;
				coalescedEnd = rangeEnd;
				result.coalescedRanges = 1;
				continue;
			}

			const uint64_t gapBytes = (uint64_t)(rangeStart - coalescedEnd);
			if (gapBytes <= maxGapBytes)
			{
				result.gapBytes += gapBytes;
				coalescedEnd = rangeEnd;
			}
			else
			{
				result.uploadedBytes += (uint64_t)(coalescedEnd - coalescedStart);
				if (outRanges != nullptr)
				{
					outRanges->push_back({ (uint64_t)coalescedStart, (uint64_t)(coalescedEnd - coalescedStart) });
				}
				result.rejectedCoalesces++;
				result.coalescedRanges++;
				coalescedStart = rangeStart;
				coalescedEnd = rangeEnd;
			}
		}
		if (hasCoalescedRange)
		{
			result.uploadedBytes += (uint64_t)(coalescedEnd - coalescedStart);
			if (outRanges != nullptr)
			{
				outRanges->push_back({ (uint64_t)coalescedStart, (uint64_t)(coalescedEnd - coalescedStart) });
			}
		}
		return result;
	};
	const auto addDirtyRangeStats =
		[&](const SceneUploadDirtyRangeStats& dirtyStats,
			uint32_t& bufferRangeCount,
			uint64_t& bufferChangedBytes,
			uint64_t& bufferUploadedBytes)
	{
		mLastPerfShellTraceStats.sceneSelectBufferUploadDirtyRangeRawRanges += dirtyStats.rawRanges;
		mLastPerfShellTraceStats.sceneSelectBufferUploadDirtyRangeCoalescedRanges += dirtyStats.coalescedRanges;
		mLastPerfShellTraceStats.sceneSelectBufferUploadDirtyRangeRejectedCoalesces += dirtyStats.rejectedCoalesces;
		mLastPerfShellTraceStats.sceneSelectBufferUploadDirtyRangeChangedBytes += dirtyStats.changedBytes;
		mLastPerfShellTraceStats.sceneSelectBufferUploadDirtyRangeUploadedBytes += dirtyStats.uploadedBytes;
		mLastPerfShellTraceStats.sceneSelectBufferUploadDirtyRangeGapBytes += dirtyStats.gapBytes;
		bufferRangeCount = dirtyStats.coalescedRanges;
		bufferChangedBytes = dirtyStats.changedBytes;
		bufferUploadedBytes = dirtyStats.uploadedBytes;
	};
	const auto updatePayloadMirror =
		[](std::vector<uint8_t>& mirror, const void* bufferData, uint64_t bufferSize)
	{
		if (bufferData == nullptr || bufferSize == 0)
		{
			mirror.clear();
			return;
		}
		const uint8_t* bytes = static_cast<const uint8_t*>(bufferData);
		mirror.assign(bytes, bytes + (size_t)bufferSize);
	};
	const auto updatePayloadMirrorRanges =
		[](std::vector<uint8_t>& mirror, const void* bufferData, const std::vector<SceneUploadDirtyRange>& ranges)
	{
		if (bufferData == nullptr || mirror.empty())
		{
			return;
		}
		const uint8_t* bytes = static_cast<const uint8_t*>(bufferData);
		for (const SceneUploadDirtyRange& range : ranges)
		{
			if (range.size != 0 && range.byteOffset <= mirror.size() && range.size <= mirror.size() - range.byteOffset)
			{
				std::memcpy(mirror.data() + range.byteOffset, bytes + range.byteOffset, (size_t)range.size);
			}
		}
	};
	const auto updateStructuredBufferRanges =
		[&](NRIBufferResource& resource,
			SceneBufferDebugStats& stats,
			std::vector<uint8_t>& payloadMirror,
			const void* bufferData,
			uint64_t bufferSize,
			uint32_t bufferStride,
			uint64_t payloadHash,
			const std::vector<SceneUploadDirtyRange>& ranges,
			nri::AccessStage afterAccess,
			double& uploadMs,
			uint64_t& uploadedBytes,
			uint32_t& growEvents,
			uint32_t& overwriteEvents,
			uint32_t& bufferRangeUploadCount) -> bool
	{
		const uint8_t* bytes = static_cast<const uint8_t*>(bufferData);
		uint64_t rangeBytes = 0;
		for (const SceneUploadDirtyRange& range : ranges)
		{
			if (bytes == nullptr ||
				range.size == 0 ||
				range.byteOffset > bufferSize ||
				range.size > bufferSize - range.byteOffset ||
				range.byteOffset > resource.size ||
				range.size > resource.size - range.byteOffset)
			{
				return false;
			}
			rangeBytes += range.size;
		}

		bool result = true;
		{
			ScopedPtPerfTimer perfTimer(uploadMs);
			for (const SceneUploadDirtyRange& range : ranges)
			{
				void* mapped = mFrameBuffer->mCore.MapBuffer(*resource.buffer, range.byteOffset, range.size);
				if (mapped == nullptr)
				{
					result = false;
					break;
				}
				std::memcpy(mapped, bytes + range.byteOffset, (size_t)range.size);
				mFrameBuffer->mCore.UnmapBuffer(*resource.buffer);
			}

			// This path only updates persistently mapped DEVICE_UPLOAD buffers. Their
			// native state is fixed and GPU-readable, so host writes need no transition.
			(void)afterAccess;
		}
		if (!result)
		{
			return false;
		}

		stats.bytesUploadedLastFrame = rangeBytes;
		stats.growEventsLastFrame = 0;
		stats.overwriteEventsLastFrame = 1;
		stats.growthOldBytesLastFrame = 0;
		stats.growthRequestedBytesLastFrame = 0;
		stats.growthAllocatedBytesLastFrame = 0;
		stats.uploadCount++;
		stats.overwriteCount++;
		stats.peakUsedBytes = std::max(stats.peakUsedBytes, bufferSize);
		NotePerfBufferUpload(&stats, rangeBytes, false, "scene_buffer_upload_range", -1);

		resource.usedSize = bufferSize;
		resource.payloadHash = payloadHash;
		resource.payloadSize = bufferSize;
		resource.payloadStride = bufferStride;
		updatePayloadMirrorRanges(payloadMirror, bufferData, ranges);

		uploadedBytes = rangeBytes;
		growEvents = 0;
		overwriteEvents = 1;
		mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashUploads++;
		mLastPerfShellTraceStats.sceneSelectBufferUploadRangeUploads++;
		mLastPerfShellTraceStats.sceneSelectBufferUploadRangeUploadedBytes += rangeBytes;
		bufferRangeUploadCount++;
		return true;
	};
	const auto ensureStructuredBufferBatched =
		[&](NRIBufferResource& resource,
			SceneBufferDebugStats& stats,
			std::vector<uint8_t>& payloadMirror,
			std::vector<SceneUploadDirtyRange>* dirtyRangeScratch,
			const NRISceneUploadDirtyPlan* typedPlan,
			const void* bufferData,
			uint64_t bufferSize,
			uint32_t bufferStride,
			uint64_t payloadHash,
			bool skipUpload,
			bool allowRangeUpload,
			bool forceIdentityUpload,
			nri::BufferUsageBits usageBits,
			nri::AccessStage afterAccess,
			double& uploadMs,
			uint64_t& uploadedBytes,
			uint32_t& growEvents,
			uint32_t& overwriteEvents,
			uint32_t& dirtyRanges,
			uint64_t& dirtyChangedBytes,
			uint64_t& dirtyUploadedBytes,
			uint32_t& bufferRangeUploadCount,
			SceneUploadBufferKind bufferKind) -> bool
	{
		const uint64_t requiredSize = std::max<uint64_t>(bufferSize, bufferStride);
		const bool forceFullDirty =
			resource.buffer == nullptr ||
			resource.shaderView == nullptr ||
			resource.memoryLocation != nri::MemoryLocation::DEVICE_UPLOAD ||
			resource.payloadHash == 0 ||
			resource.size < requiredSize ||
			resource.usedSize != bufferSize ||
			resource.payloadSize != bufferSize ||
			resource.stride != bufferStride ||
			resource.payloadStride != bufferStride ||
			forceIdentityUpload ||
			(typedPlan != nullptr && typedPlan->forceFull);
		const bool traceDirtyRanges = ShouldTraceSceneBufferDirtyRanges();
		SceneUploadDirtyRangeStats dirtyStats = {};
		const bool collectDirtyRanges = allowRangeUpload && (traceDirtyRanges || (!skipUpload && !forceFullDirty));
		if (!collectDirtyRanges)
		{
			if (dirtyRangeScratch != nullptr)
			{
				dirtyRangeScratch->clear();
			}
			dirtyRanges = 0;
			dirtyChangedBytes = 0;
			dirtyUploadedBytes = 0;
		}
		else if (typedPlan != nullptr && typedPlan->typed && !forceFullDirty)
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectBufferUploadDirtyRangeMs);
			mLastPerfShellTraceStats.sceneSelectBufferUploadDirtyRangeSourceTyped++;
			dirtyStats.rawRanges = typedPlan->rawRanges;
			dirtyStats.coalescedRanges = (uint32_t)typedPlan->ranges.size();
			dirtyStats.rejectedCoalesces = typedPlan->rejectedCoalesces;
			dirtyStats.changedBytes = typedPlan->changedBytes;
			dirtyStats.uploadedBytes = typedPlan->uploadBytes;
			dirtyStats.gapBytes = typedPlan->gapBytes;
			if (dirtyRangeScratch != nullptr)
			{
				*dirtyRangeScratch = typedPlan->ranges;
			}
			addDirtyRangeStats(dirtyStats, dirtyRanges, dirtyChangedBytes, dirtyUploadedBytes);
			if (dirtyRangeScratch != nullptr)
			{
				addDomainRangeBytes(bufferKind, *dirtyRangeScratch, true);
			}
		}
		else
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectBufferUploadDirtyRangeMs);
			dirtyStats = noteDirtyRanges(payloadMirror, bufferData, bufferSize, skipUpload, forceFullDirty, dirtyRangeScratch);
			addDirtyRangeStats(dirtyStats, dirtyRanges, dirtyChangedBytes, dirtyUploadedBytes);
			if (dirtyRangeScratch != nullptr)
			{
				addDomainRangeBytes(bufferKind, *dirtyRangeScratch, true);
			}
		}

		if (skipUpload)
		{
			resource.usedSize = bufferSize;
			uploadedBytes = 0;
			growEvents = 0;
			overwriteEvents = 0;
			return true;
		}

		const bool canRangeUpload =
			allowRangeUpload &&
			!forceFullDirty &&
			dirtyRangeScratch != nullptr &&
			!dirtyRangeScratch->empty() &&
			dirtyStats.uploadedBytes != 0 &&
			dirtyStats.uploadedBytes < bufferSize;
		bool useRangeUpload = false;
		if (canRangeUpload)
		{
			const uint32_t maxRangeCount = (uint32_t)(int)nri_ptscenebufferrangeuploadmaxranges;
			const uint32_t maxUploadPercent = (uint32_t)(int)nri_ptscenebufferrangeuploadmaxpercent;
			if (dirtyStats.coalescedRanges > maxRangeCount)
			{
				mLastPerfShellTraceStats.sceneSelectBufferUploadRangeFallbacks++;
				mLastPerfShellTraceStats.sceneSelectBufferUploadRangeFallbackFragmented++;
			}
			else if (dirtyStats.uploadedBytes * 100u >= bufferSize * maxUploadPercent)
			{
				mLastPerfShellTraceStats.sceneSelectBufferUploadRangeFallbacks++;
				mLastPerfShellTraceStats.sceneSelectBufferUploadRangeFallbackLarge++;
			}
			else
			{
				useRangeUpload = true;
			}
		}
		else if (allowRangeUpload && !forceFullDirty && dirtyRangeScratch != nullptr && dirtyRangeScratch->empty() && dirtyStats.changedBytes == 0)
		{
			resource.usedSize = bufferSize;
			resource.payloadHash = payloadHash;
			resource.payloadSize = bufferSize;
			resource.payloadStride = bufferStride;
			uploadedBytes = 0;
			growEvents = 0;
			overwriteEvents = 0;
			return true;
		}

		bool needsWait = false;
		if (!waitedForWrites)
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectBufferUploadWaitCheckMs);
			needsWait = StructuredBufferUpdateNeedsWait(resource, bufferData, bufferSize, bufferStride);
		}
		if (!waitedForWrites && needsWait)
		{
			const auto waitStart = std::chrono::steady_clock::now();
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectBufferUploadWaitMs);
			mLastPerfShellTraceStats.sceneSelectBufferUploadWaitCount++;
			WaitForCommandsTracked("scene_buffer_upload");
			addDomainWait(bufferKind, DurationMs(waitStart, std::chrono::steady_clock::now()));
			waitedForWrites = true;
		}

		if (useRangeUpload)
		{
			if (updateStructuredBufferRanges(resource, stats, payloadMirror, bufferData, bufferSize, bufferStride, payloadHash, *dirtyRangeScratch, afterAccess, uploadMs, uploadedBytes, growEvents, overwriteEvents, bufferRangeUploadCount))
			{
				addDomainRangeBytes(bufferKind, *dirtyRangeScratch, false);
				return true;
			}
			mLastPerfShellTraceStats.sceneSelectBufferUploadRangeFallbacks++;
		}

		bool result = false;
		{
			ScopedPtPerfTimer perfTimer(uploadMs);
			result = EnsureStructuredBuffer(resource, stats, bufferData, bufferSize, bufferStride, usageBits, afterAccess, waitedForWrites, "scene_buffer_upload");
		}
		uploadedBytes = stats.bytesUploadedLastFrame;
		growEvents = stats.growEventsLastFrame;
		overwriteEvents = stats.overwriteEventsLastFrame;
		if (result)
		{
			if (stats.growEventsLastFrame != 0)
			{
				mLastPerfShellTraceStats.sceneSelectBufferUploadGrowthEvents += stats.growEventsLastFrame;
				mLastPerfShellTraceStats.sceneSelectBufferUploadGrowthOldBytes += stats.growthOldBytesLastFrame;
				mLastPerfShellTraceStats.sceneSelectBufferUploadGrowthRequestedBytes += stats.growthRequestedBytesLastFrame;
				mLastPerfShellTraceStats.sceneSelectBufferUploadGrowthAllocatedBytes += stats.growthAllocatedBytesLastFrame;
				if (stats.growthAllocatedBytesLastFrame > stats.growthRequestedBytesLastFrame)
				{
					mLastPerfShellTraceStats.sceneSelectBufferUploadGrowthHeadroomBytes +=
						stats.growthAllocatedBytesLastFrame - stats.growthRequestedBytesLastFrame;
				}
				addDomainGrowth(bufferKind, stats.growthRequestedBytesLastFrame, stats.growthAllocatedBytesLastFrame);
			}
			resource.payloadHash = payloadHash;
			resource.payloadSize = bufferSize;
			resource.payloadStride = bufferStride;
			updatePayloadMirror(payloadMirror, bufferData, bufferSize);
			mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashUploads++;
			addDomainFullUpload(bufferKind);
		}
		return result;
	};

	const bool skipVertexUpload = notePayloadHashState(vertexBuffer, vertexPayloadHash, vertexSize, sizeof(nri_scene::SceneVertex), mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashVertexHits, mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashVertexSkips, mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashVertexMisses);
	const bool skipIndexUpload = notePayloadHashState(indexBuffer, indexPayloadHash, indexSize, sizeof(uint32_t), mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashIndexHits, mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashIndexSkips, mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashIndexMisses);
	const bool skipPrimitiveUpload = notePayloadHashState(primitiveBuffer, primitivePayloadHash, primitiveSize, sizeof(nri_scene::PrimitiveData), mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashPrimitiveHits, mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashPrimitiveSkips, mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashPrimitiveMisses);
	const bool skipMaterialUpload = notePayloadHashState(materialBuffer, materialPayloadHash, materialSize, sizeof(nri_scene::MaterialData), mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashMaterialHits, mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashMaterialSkips, mLastPerfShellTraceStats.sceneSelectBufferUploadPayloadHashMaterialMisses);
	addDomainPayload(SceneUploadBufferKind::Vertex, skipVertexUpload);
	addDomainPayload(SceneUploadBufferKind::Index, skipIndexUpload);
	addDomainPayload(SceneUploadBufferKind::Primitive, skipPrimitiveUpload);
	addDomainPayload(SceneUploadBufferKind::Material, skipMaterialUpload);
	const bool uploaded =
		ensureStructuredBufferBatched(vertexBuffer, mVertexBufferStats, vertexMirror, &mSceneUploadVertexDirtyRangeScratch, (vertexTypedPlan.typed || vertexTypedPlan.forceFull) ? &vertexTypedPlan : nullptr, geometry.vertices.data(), vertexSize, sizeof(nri_scene::SceneVertex), vertexPayloadHash, skipVertexUpload, vertexTypedPlan.typed, forceValidationVertexUpload, NRIResourceFlags(nri::BufferUsageBits::SHADER_RESOURCE, nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT), NRIResourceAccelerationStructureBuildInputAccess(), mLastPerfShellTraceStats.sceneSelectBufferUploadVertexMs, mLastPerfShellTraceStats.sceneSelectBufferUploadVertexUploadedBytes, mLastPerfShellTraceStats.sceneSelectBufferUploadVertexGrowEvents, mLastPerfShellTraceStats.sceneSelectBufferUploadVertexOverwriteEvents, mLastPerfShellTraceStats.sceneSelectBufferUploadVertexDirtyRanges, mLastPerfShellTraceStats.sceneSelectBufferUploadVertexDirtyChangedBytes, mLastPerfShellTraceStats.sceneSelectBufferUploadVertexDirtyUploadedBytes, mLastPerfShellTraceStats.sceneSelectBufferUploadVertexRangeUploads, SceneUploadBufferKind::Vertex) &&
		ensureStructuredBufferBatched(indexBuffer, mIndexBufferStats, indexMirror, &mSceneUploadIndexDirtyRangeScratch, (indexTypedPlan.typed || indexTypedPlan.forceFull) ? &indexTypedPlan : nullptr, geometry.indices.data(), indexSize, sizeof(uint32_t), indexPayloadHash, skipIndexUpload, indexTypedPlan.typed, forceValidationIndexUpload, NRIResourceFlags(nri::BufferUsageBits::SHADER_RESOURCE, nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT), NRIResourceAccelerationStructureBuildInputAccess(), mLastPerfShellTraceStats.sceneSelectBufferUploadIndexMs, mLastPerfShellTraceStats.sceneSelectBufferUploadIndexUploadedBytes, mLastPerfShellTraceStats.sceneSelectBufferUploadIndexGrowEvents, mLastPerfShellTraceStats.sceneSelectBufferUploadIndexOverwriteEvents, mLastPerfShellTraceStats.sceneSelectBufferUploadIndexDirtyRanges, mLastPerfShellTraceStats.sceneSelectBufferUploadIndexDirtyChangedBytes, mLastPerfShellTraceStats.sceneSelectBufferUploadIndexDirtyUploadedBytes, mLastPerfShellTraceStats.sceneSelectBufferUploadIndexRangeUploads, SceneUploadBufferKind::Index) &&
		ensureStructuredBufferBatched(primitiveBuffer, mPrimitiveBufferStats, primitiveMirror, &mSceneUploadPrimitiveDirtyRangeScratch, (primitiveTypedPlan.typed || primitiveTypedPlan.forceFull) ? &primitiveTypedPlan : nullptr, gpuPrimitives != nullptr && !gpuPrimitives->empty() ? gpuPrimitives->data() : nullptr, primitiveSize, sizeof(nri_scene::PrimitiveData), primitivePayloadHash, skipPrimitiveUpload, true, forceValidationPrimitiveUpload, nri::BufferUsageBits::SHADER_RESOURCE, NRIResourceComputeShaderResourceAccess(), mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveMs, mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveUploadedBytes, mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveGrowEvents, mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveOverwriteEvents, mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveDirtyRanges, mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveDirtyChangedBytes, mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveDirtyUploadedBytes, mLastPerfShellTraceStats.sceneSelectBufferUploadPrimitiveRangeUploads, SceneUploadBufferKind::Primitive) &&
		ensureStructuredBufferBatched(materialBuffer, mMaterialBufferStats, materialMirror, &mSceneUploadMaterialDirtyRangeScratch, (materialTypedPlan.typed || materialTypedPlan.forceFull) ? &materialTypedPlan : nullptr, materials.data(), materialSize, sizeof(nri_scene::MaterialData), materialPayloadHash, skipMaterialUpload, true, forceValidationMaterialUpload, nri::BufferUsageBits::SHADER_RESOURCE, NRIResourceComputeShaderResourceAccess(), mLastPerfShellTraceStats.sceneSelectBufferUploadMaterialMs, mLastPerfShellTraceStats.sceneSelectBufferUploadMaterialUploadedBytes, mLastPerfShellTraceStats.sceneSelectBufferUploadMaterialGrowEvents, mLastPerfShellTraceStats.sceneSelectBufferUploadMaterialOverwriteEvents, mLastPerfShellTraceStats.sceneSelectBufferUploadMaterialDirtyRanges, mLastPerfShellTraceStats.sceneSelectBufferUploadMaterialDirtyChangedBytes, mLastPerfShellTraceStats.sceneSelectBufferUploadMaterialDirtyUploadedBytes, mLastPerfShellTraceStats.sceneSelectBufferUploadMaterialRangeUploads, SceneUploadBufferKind::Material);
	if (uploaded)
	{
		if (domainSpans != nullptr)
		{
			uploadSlot.publishedSpans = *domainSpans;
			uploadSlot.publishedPrimitiveExtraIdentity = primitiveVisibilityIdentityHash;
			uploadSlot.publishedMaterialExtraIdentity = finalMaterialProductIdentity;
			uploadSlot.publishedSpanIdentitiesValid = true;
		}
		else
		{
			uploadSlot.publishedSpans.clear();
			uploadSlot.publishedPrimitiveExtraIdentity = 0;
			uploadSlot.publishedMaterialExtraIdentity = 0;
			uploadSlot.publishedSpanIdentitiesValid = false;
		}
	}
	return uploaded;
}


NRIResourceContext NRIRenderer::BuildResourceContext() const
{
	NRIResourceContext context = {};
	context.device = mFrameBuffer != nullptr ? mFrameBuffer->mDevice : nullptr;
	context.core = mFrameBuffer != nullptr ? &mFrameBuffer->mCore : nullptr;
	context.commandBuffer = mFrameBuffer != nullptr ? mFrameBuffer->mCommandBuffer : nullptr;
	return context;
}

NRIResourceServices NRIRenderer::BuildResourceServices()
{
	NRIResourceServices services = {};
	services.context = BuildResourceContext();
	services.user = this;
	services.waitForCommands = [](void* user, const char* reason)
	{
		static_cast<NRIRenderer*>(user)->WaitForCommandsTracked(reason);
	};
	services.destroyBufferResource = [](void* user, NRIBufferResource& resource)
	{
		static_cast<NRIRenderer*>(user)->DestroyBufferResource(resource);
	};
	return services;
}

bool NRIRenderer::CreateStructuredBuffer(NRIBufferResource& resource, const void* data, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after)
{
	return NRISceneUploadManager::CreateStructuredBuffer(*this, resource, data, size, stride, usage, after);
}

bool NRIRenderer::EnsureStructuredBuffer(NRIBufferResource& resource, SceneBufferDebugStats& stats, const void* data, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after, bool writesQuiesced, const char* waitReason)
{
	return NRISceneUploadManager::EnsureStructuredBuffer(*this, resource, stats, data, size, stride, usage, after, writesQuiesced, waitReason);
}

bool NRIRenderer::UpdateStructuredBufferRange(NRIBufferResource& resource, uint64_t byteOffset, const void* data, uint64_t size, nri::AccessStage after)
{
	return NRISceneUploadManager::UpdateStructuredBufferRange(*this, resource, byteOffset, data, size, after);
}

bool NRIRenderer::CreateBufferWithoutView(NRIBufferResource& resource, uint64_t size, uint32_t stride, nri::BufferUsageBits usage)
{
	if (resource.buffer != nullptr)
	{
		WaitForCommandsTracked();
	}

	return CreateBufferWithoutViewAtLocation(resource, size, stride, usage, nri::MemoryLocation::DEVICE);
}

bool NRIRenderer::CreateBufferWithoutViewAtLocation(NRIBufferResource& resource, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::MemoryLocation memoryLocation)
{
	const NRIResourceContext resourceContext = BuildResourceContext();
	DestroyBufferResource(resource);

	nri::BufferDesc desc = {};
	desc.size = std::max<uint64_t>(size, stride);
	desc.structureStride = stride;
	desc.usage = usage;
	if (resourceContext.core->CreateCommittedBuffer(*resourceContext.device, memoryLocation, 0.0f, desc, resource.buffer) != nri::Result::SUCCESS)
	{
		return false;
	}

	nri::MemoryDesc memoryDesc = {};
	resourceContext.core->GetBufferMemoryDesc(*resource.buffer, memoryLocation, memoryDesc);
	resource.size = desc.size;
	resource.memorySize = memoryDesc.size;
	resource.usedSize = size;
	resource.stride = stride;
	resource.usage = usage;
	resource.memoryLocation = memoryLocation;
	return true;
}

bool NRIRenderer::EnsureResidentArenaBuffer(NRIBufferResource& resource, uint64_t requiredSize, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after)
{
	const uint64_t alignedRequiredSize = std::max<uint64_t>(requiredSize, stride);
	if (resource.buffer != nullptr &&
		resource.shaderView != nullptr &&
		resource.memoryLocation == nri::MemoryLocation::DEVICE &&
		resource.stride == stride &&
		NRIResourceUsageIncludes(resource.usage, usage) &&
		resource.size >= alignedRequiredSize)
	{
		resource.usedSize = std::max(resource.usedSize, requiredSize);
		return true;
	}

	NRIBufferResource oldResource = resource;
	resource = {};
	const auto growthStart = std::chrono::steady_clock::now();
	const uint64_t oldSize = oldResource.size;
	const uint64_t oldUsedSize = oldResource.usedSize;
	const bool retiredOldResource = oldResource.buffer != nullptr;

	const uint64_t grownSize = GetNRIGrownBufferSize(oldResource.size, alignedRequiredSize, stride);
	if (!CreateBufferWithoutViewAtLocation(resource, grownSize, stride, usage, nri::MemoryLocation::DEVICE))
	{
		resource = oldResource;
		return false;
	}

	nri::BufferViewDesc viewDesc = {};
	viewDesc.buffer = resource.buffer;
	viewDesc.type = nri::BufferView::STRUCTURED_BUFFER;
	viewDesc.offset = 0;
	viewDesc.size = nri::WHOLE_SIZE;
	viewDesc.structureStride = stride;
	if (mFrameBuffer->mCore.CreateBufferView(viewDesc, resource.shaderView) != nri::Result::SUCCESS)
	{
		DestroyBufferResource(resource);
		resource = oldResource;
		return false;
	}
	if (NRIResourceUsageIncludes(usage, nri::BufferUsageBits::SHADER_RESOURCE_STORAGE))
	{
		nri::BufferViewDesc storageViewDesc = viewDesc;
		storageViewDesc.type = nri::BufferView::STORAGE_STRUCTURED_BUFFER;
		if (mFrameBuffer->mCore.CreateBufferView(storageViewDesc, resource.storageView) != nri::Result::SUCCESS)
		{
			DestroyBufferResource(resource);
			resource = oldResource;
			return false;
		}
	}
	resource.usedSize = requiredSize;
	uint64_t copiedSize = 0;

	if (oldResource.buffer != nullptr && mFrameBuffer->mCommandBuffer != nullptr)
	{
		const uint64_t copySize = std::min(oldResource.usedSize, resource.size);
		copiedSize = copySize;
		if (copySize > 0)
		{
			nri::BufferBarrierDesc beforeBarriers[2] = {};
			beforeBarriers[0].buffer = oldResource.buffer;
			beforeBarriers[0].before = after;
			beforeBarriers[0].after = NRIResourceCopySourceAccess();
			beforeBarriers[1].buffer = resource.buffer;
			beforeBarriers[1].before = {};
			beforeBarriers[1].after = NRIResourceCopyDestinationAccess();
			nri::BarrierDesc beforeBarrierDesc = {};
			beforeBarrierDesc.buffers = beforeBarriers;
			beforeBarrierDesc.bufferNum = 2;
			mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, beforeBarrierDesc);

			mFrameBuffer->mCore.CmdCopyBuffer(
				*mFrameBuffer->mCommandBuffer,
				*resource.buffer,
				0,
				*oldResource.buffer,
				0,
				copySize);

			nri::BufferBarrierDesc afterBarrier = {};
			afterBarrier.buffer = resource.buffer;
			afterBarrier.before = NRIResourceCopyDestinationAccess();
			afterBarrier.after = after;
			nri::BarrierDesc afterBarrierDesc = {};
			afterBarrierDesc.buffers = &afterBarrier;
			afterBarrierDesc.bufferNum = 1;
			mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, afterBarrierDesc);
		}

		auto& frameScratch = GetResidentUploadScratchFrame();
		frameScratch.retiredBuffers.push_back(oldResource);
	}
	else if (oldResource.buffer != nullptr || oldResource.shaderView != nullptr || oldResource.storageView != nullptr)
	{
		DestroyBufferResource(oldResource);
	}
	if ((int)nri_ptvoxelcomputetrace > 0 || (int)perf_looptraceframes > 0)
	{
		Printf("PERF pt resident arena growth NRI: frame=%u stride=%u old_size=%llu old_used=%llu required=%llu allocated=%llu headroom=%llu copied=%llu retired=%u ms=%.3f\n",
			mFrameIndex,
			stride,
			(unsigned long long)oldSize,
			(unsigned long long)oldUsedSize,
			(unsigned long long)requiredSize,
			(unsigned long long)resource.size,
			(unsigned long long)(resource.size - std::min(resource.size, requiredSize)),
			(unsigned long long)copiedSize,
			retiredOldResource ? 1u : 0u,
			DurationMs(growthStart, std::chrono::steady_clock::now()));
	}

	return true;
}

bool NRIRenderer::EnsureResidentUploadScratchBuffer(ResidentBufferUploadScratch& scratch, ResidentUploadScratchFrame& frameScratch, uint64_t requiredSize)
{
	constexpr uint32_t kResidentUploadScratchStride = 16u;
	const uint64_t alignedRequiredSize = std::max<uint64_t>(requiredSize, kResidentUploadScratchStride);
	if (scratch.buffer.buffer != nullptr &&
		scratch.buffer.memoryLocation == nri::MemoryLocation::DEVICE_UPLOAD &&
		scratch.buffer.size >= alignedRequiredSize)
	{
		return true;
	}

	const uint64_t grownSize = GetNRIGrownBufferSize(scratch.buffer.size, alignedRequiredSize, kResidentUploadScratchStride);
	if (scratch.buffer.buffer != nullptr || scratch.buffer.shaderView != nullptr)
	{
		frameScratch.retiredBuffers.push_back(scratch.buffer);
		scratch.buffer = {};
		scratch.cursor = 0;
		scratch.copySourceActive = false;
	}
	const bool created = CreateBufferWithoutViewAtLocation(
		scratch.buffer,
		grownSize,
		kResidentUploadScratchStride,
		nri::BufferUsageBits::NONE,
		nri::MemoryLocation::DEVICE_UPLOAD);
	if (created)
	{
		mLastPerfShellTraceStats.runtimeMutationResidentApplyStageScratchGrowCount++;
		mLastPerfShellTraceStats.runtimeMutationResidentApplyStageScratchGrowBytes += grownSize;
	}
	return created;
}

bool NRIRenderer::StageResidentBufferCopyRange(NRIBufferResource& resource, uint64_t byteOffset, const void* data, uint64_t size, nri::AccessStage after, int uploadKind)
{
	if (resource.buffer == nullptr ||
		data == nullptr ||
		size == 0 ||
		byteOffset > resource.size ||
		size > resource.size - byteOffset)
	{
		return false;
	}

	if (mFrameBuffer == nullptr || mFrameBuffer->mCommandBuffer == nullptr)
	{
		return false;
	}

	constexpr uint64_t kResidentUploadScratchAlignment = 16u;
	auto& frameScratch = GetResidentUploadScratchFrame();

	ResidentBufferUploadScratch* scratch = nullptr;
	switch (uploadKind)
	{
	case ResidentUploadKind_Vertex: scratch = &frameScratch.vertex; break;
	case ResidentUploadKind_Index: scratch = &frameScratch.index; break;
	case ResidentUploadKind_Primitive: scratch = &frameScratch.primitive; break;
	case ResidentUploadKind_Material: scratch = &frameScratch.material; break;
	default: return false;
	}

	const uint64_t scratchOffset =
		(scratch->cursor + kResidentUploadScratchAlignment - 1u) &
		~(kResidentUploadScratchAlignment - 1u);
	const uint64_t requiredSize = scratchOffset + size;
	if (!EnsureResidentUploadScratchBuffer(*scratch, frameScratch, requiredSize))
	{
		return false;
	}

	void* mapped = mFrameBuffer->mCore.MapBuffer(*scratch->buffer.buffer, scratchOffset, size);
	if (mapped == nullptr)
	{
		return false;
	}

	std::memcpy(mapped, data, (size_t)size);
	mFrameBuffer->mCore.UnmapBuffer(*scratch->buffer.buffer);

	if (!scratch->copySourceActive)
	{
		nri::BufferBarrierDesc sourceBarrier = {};
		sourceBarrier.buffer = scratch->buffer.buffer;
		sourceBarrier.before = {};
		sourceBarrier.after = NRIResourceCopySourceAccess();

		nri::BarrierDesc sourceBarrierDesc = {};
		sourceBarrierDesc.buffers = &sourceBarrier;
		sourceBarrierDesc.bufferNum = 1;
		mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, sourceBarrierDesc);
		scratch->copySourceActive = true;
	}

	nri::BufferBarrierDesc beforeCopyBarrier = {};
	beforeCopyBarrier.buffer = resource.buffer;
	beforeCopyBarrier.before = after;
	beforeCopyBarrier.after = NRIResourceCopyDestinationAccess();

	nri::BarrierDesc beforeCopyBarrierDesc = {};
	beforeCopyBarrierDesc.buffers = &beforeCopyBarrier;
	beforeCopyBarrierDesc.bufferNum = 1;
	mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, beforeCopyBarrierDesc);

	mFrameBuffer->mCore.CmdCopyBuffer(
		*mFrameBuffer->mCommandBuffer,
		*resource.buffer,
		byteOffset,
		*scratch->buffer.buffer,
		scratchOffset,
		size);

	nri::BufferBarrierDesc afterCopyBarrier = {};
	afterCopyBarrier.buffer = resource.buffer;
	afterCopyBarrier.before = NRIResourceCopyDestinationAccess();
	afterCopyBarrier.after = after;

	nri::BarrierDesc afterCopyBarrierDesc = {};
	afterCopyBarrierDesc.buffers = &afterCopyBarrier;
	afterCopyBarrierDesc.bufferNum = 1;
	mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, afterCopyBarrierDesc);

	scratch->cursor = scratchOffset + size;
	return true;
}

void NRIRenderer::RetireResidentBufferResource(NRIBufferResource& resource)
{
	if (resource.buffer == nullptr && resource.shaderView == nullptr)
	{
		return;
	}

	if (mFrameBuffer != nullptr &&
		mFrameBuffer->mCommandBuffer != nullptr &&
		!mResidentUploadScratchFrames.empty())
	{
		auto& frameScratch = GetResidentUploadScratchFrame();
		frameScratch.retiredBuffers.push_back(resource);
		resource = {};
		return;
	}

	DestroyBufferResource(resource);
}

void NRIRenderer::RetireResidentAccelerationStructure(NRIAccelerationStructureResource& resource)
{
	if (resource.accelerationStructure == nullptr && resource.descriptor == nullptr)
	{
		return;
	}

	if (mFrameBuffer == nullptr ||
		mFrameBuffer->mCommandBuffer == nullptr ||
		mResidentUploadScratchFrames.empty())
	{
		DestroyAccelerationStructureResource(resource);
		return;
	}

	auto& frameScratch = GetResidentUploadScratchFrame();
	frameScratch.retiredAccelerationStructures.push_back(resource);
	resource = {};
}

void NRIRenderer::RetireTopLevelAccelerationStructure(NRIAccelerationStructureResource& resource)
{
	if (resource.accelerationStructure == nullptr && resource.descriptor == nullptr)
	{
		return;
	}

	RetireResidentAccelerationStructure(resource);
}

bool NRIRenderer::IsFrameFenceValueComplete(uint64_t fenceValue) const
{
	return mFrameBuffer != nullptr && mFrameBuffer->IsFrameFenceValueComplete(fenceValue);
}

bool NRIRenderer::IsCommandFenceValueComplete(uint64_t fenceValue) const
{
	return mFrameBuffer != nullptr && mFrameBuffer->IsCommandFenceValueComplete(fenceValue);
}

uint64_t NRIRenderer::GetRecordingFrameFenceValue() const
{
	if (mFrameBuffer == nullptr || !mFrameBuffer->mFrameBegun || !mFrameBuffer->mCommandBufferOpen)
	{
		return 0;
	}
	return 1u + mFrameBuffer->mFrameIndex;
}

uint64_t NRIRenderer::GetRecordingCommandFenceValue() const
{
	return mFrameBuffer != nullptr ? mFrameBuffer->GetRecordingCommandFenceValue() : 0;
}

bool NRIRenderer::EnsureResidentStructuredBuffer(NRIBufferResource& resource, SceneBufferDebugStats& stats, const void* data, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after, const char* waitReason, int uploadKind)
{
	const uint64_t requiredSize = std::max<uint64_t>(size, stride);
	const bool needsGrowth =
		resource.buffer == nullptr ||
		resource.shaderView == nullptr ||
		resource.memoryLocation != nri::MemoryLocation::DEVICE ||
		resource.stride != stride ||
		resource.size < requiredSize;

	stats.bytesUploadedLastFrame = size;
	stats.growEventsLastFrame = 0;
	stats.overwriteEventsLastFrame = 0;
	stats.growthOldBytesLastFrame = 0;
	stats.growthRequestedBytesLastFrame = 0;
	stats.growthAllocatedBytesLastFrame = 0;
	stats.uploadCount++;
	stats.peakUsedBytes = std::max(stats.peakUsedBytes, size);
	NotePerfBufferUpload(&stats, size, needsGrowth, waitReason, uploadKind);

	if (needsGrowth)
	{
		const uint64_t oldSize = resource.size;
		const uint64_t grownSize = GetNRIGrownBufferSize(resource.size, requiredSize, stride);
		NRIBufferResource oldResource = resource;
		NRIBufferResource newResource = {};
		if (!CreateBufferWithoutViewAtLocation(newResource, grownSize, stride, usage, nri::MemoryLocation::DEVICE))
		{
			return false;
		}

		nri::BufferViewDesc viewDesc = {};
		viewDesc.buffer = newResource.buffer;
		viewDesc.type = nri::BufferView::STRUCTURED_BUFFER;
		viewDesc.offset = 0;
		viewDesc.size = nri::WHOLE_SIZE;
		viewDesc.structureStride = stride;
		if (mFrameBuffer->mCore.CreateBufferView(viewDesc, newResource.shaderView) != nri::Result::SUCCESS)
		{
			DestroyBufferResource(newResource);
			return false;
		}

		newResource.usedSize = size;
		if (data != nullptr && size != 0)
		{
			if (!StageResidentBufferCopyRange(newResource, 0, data, size, after, uploadKind))
			{
				DestroyBufferResource(newResource);
				return false;
			}
		}

		resource = newResource;
		RetireResidentBufferResource(oldResource);
		stats.growthCount++;
		stats.growEventsLastFrame = 1;
		stats.growthOldBytesLastFrame = oldSize;
		stats.growthRequestedBytesLastFrame = requiredSize;
		stats.growthAllocatedBytesLastFrame = grownSize;
		return true;
	}
	else
	{
		stats.overwriteCount++;
		stats.overwriteEventsLastFrame = 1;
	}

	resource.usedSize = size;
	if (data != nullptr && size != 0)
	{
		if (!StageResidentBufferCopyRange(resource, 0, data, size, after, uploadKind))
		{
			return false;
		}
	}

	return true;
}

bool NRISceneUploadManager::UpdateSceneDataSet(
	NRIRenderer& renderer,
	const NRIBufferResource& staticVertexBuffer,
	const NRIBufferResource& staticIndexBuffer,
	const NRIBufferResource& staticPrimitiveBuffer,
	const NRIBufferResource& staticMaterialBuffer,
	const NRIBufferResource& dynamicVertexBuffer,
	const NRIBufferResource& dynamicIndexBuffer,
	const NRIBufferResource& dynamicPrimitiveBuffer,
	const NRIBufferResource& dynamicMaterialBuffer,
	const std::vector<SceneInstanceData>& sceneInstances,
	uint32_t staticPrimitiveCount,
	uint32_t dynamicPrimitiveCount,
	uint32_t staticMaterialCount,
	uint32_t dynamicMaterialCount,
	const char* reason)
{
	ScopedPtPerfTimer perfTimer(renderer.mLastPerfShellTraceStats.sceneDataSetMs);
	renderer.mLastPerfShellTraceStats.sceneDataSetCalls++;
	renderer.mActiveSceneDataSet = nullptr;
	renderer.mActiveSceneDataSnapshot = nullptr;
	renderer.mActiveSceneDataSetFrameIndex = UINT64_MAX;
	renderer.SetCurrentSceneDataDescriptorsInitialized(false);
	bool waitedForWrites = false;
	const char* sceneDataReason = reason != nullptr ? reason : "scene_data_full_rebuild";
	const auto ensureSceneDataBatched = [&](NRIBufferResource& resource, SceneBufferDebugStats& stats, const char* bufferLabel, const void* data, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after, double& uploadMs, uint64_t& requestedBytes, uint64_t& uploadedBytes, bool writesQuiesced) -> bool
	{
		return EnsureSceneDataBatched(renderer, waitedForWrites, sceneDataReason, resource, stats, bufferLabel, data, size, stride, usage, after, uploadMs, requestedBytes, uploadedBytes, writesQuiesced);
	};
	const auto acquireSceneDataDescriptorSnapshot = [&]() -> NRIRenderer::SceneDataDescriptorSnapshot*
	{
		if (renderer.mSceneDataSnapshots.empty() || renderer.mFrameBuffer == nullptr)
		{
			return nullptr;
		}

		const uint32_t snapshotIndex = renderer.mSceneDataSnapshotCursor++ % (uint32_t)renderer.mSceneDataSnapshots.size();
		NRIRenderer::SceneDataDescriptorSnapshot& snapshot = renderer.mSceneDataSnapshots[snapshotIndex];
		if (snapshot.sceneDataSet == nullptr)
		{
			return nullptr;
		}
		const uint64_t recordingFenceValue = renderer.GetRecordingCommandFenceValue();
		if (recordingFenceValue == 0)
		{
			return nullptr;
		}

		if (!renderer.IsCommandFenceValueComplete(snapshot.retireFenceValue))
		{
			renderer.WaitForCommandsTracked("scene_data_snapshot_reuse");
		}

		snapshot.retireFenceValue = recordingFenceValue;
		snapshot.descriptorsInitialized = false;
		snapshot.publishedMapEpoch = 0;
		snapshot.publishedBuildEpoch = 0;
		return &snapshot;
	};

	if (sceneInstances.empty())
	{
		return false;
	}

	std::vector<ScenePortalData> scenePortals;
	{
		ScopedPtPerfTimer portalTimer(renderer.mLastPerfShellTraceStats.sceneDataSetPortalMs);
		scenePortals = BuildScenePortalData(renderer.mMapWorld);
	}

	const uint32_t defaultVisibleChunkWord = 0u;
	const uint32_t defaultVisibleFlatPlaneWord = 0u;
	const uint64_t reprojectionSize = sizeof(NRIReprojectionData);
	const uint64_t visibleChunkSize = renderer.mCurrentVisibleChunkWords.empty() ? sizeof(defaultVisibleChunkWord) : renderer.mCurrentVisibleChunkWords.size() * sizeof(uint32_t);
	const uint64_t visibleFlatPlaneSize = renderer.mCurrentVisibleFlatPlaneWords.empty() ? sizeof(defaultVisibleFlatPlaneWord) : renderer.mCurrentVisibleFlatPlaneWords.size() * sizeof(uint32_t);
	const uint64_t spatialAbsenceSize = std::max<size_t>(
		renderer.mSpatialAbsenceGate.GetSnapshot().gpuRecords.size(), 1u) * sizeof(NRISpatialAbsenceGpuRecord);
	const uint64_t spatialAbsenceTypedSize = std::max<size_t>(
		renderer.mSpatialAbsenceGpuSnapshot.blocks.size(), 1u) * sizeof(NRISpatialAbsenceGpuBlock);
	const uint64_t sceneInstanceSize = sceneInstances.size() * sizeof(SceneInstanceData);
	const uint64_t portalSize = scenePortals.size() * sizeof(ScenePortalData);
	const uint64_t portalHash = HashSceneDataPayload(scenePortals.empty() ? nullptr : scenePortals.data(), portalSize, scenePortals.size());
	const uint64_t portalBuildSerial = renderer.mMapWorld.valid ? renderer.mMapWorld.buildSerial : 0ull;
	const uint64_t tlasInstanceHash = renderer.mLastWorldTlasInstancePayloadHash;
	const uint32_t tlasInstanceCount = renderer.mLastWorldTlasInstanceCount;
	const bool hasCurrentWorldTlasSceneInstanceHash =
		renderer.mLastWorldTlasInstanceFrameIndex == renderer.mFrameIndex &&
		renderer.mLastWorldTlasSceneInstancePayloadHash != 0 &&
		tlasInstanceCount == (uint32_t)sceneInstances.size();
	const uint64_t sceneInstanceHash =
		hasCurrentWorldTlasSceneInstanceHash ?
		renderer.mLastWorldTlasSceneInstancePayloadHash :
		HashSceneDataPayload(sceneInstances.data(), sceneInstanceSize, sceneInstances.size());
	const bool sceneInstancesMatchLastWorldTlas =
		renderer.mLastWorldTlasSceneInstancePayloadHash != 0 &&
		renderer.mLastWorldTlasSceneInstancePayloadHash == sceneInstanceHash;
	const bool sceneInstanceCanUseFrameSlot =
		renderer.mLastWorldTlasInstanceFrameIndex == renderer.mFrameIndex &&
		tlasInstanceCount != 0 &&
		tlasInstanceCount == (uint32_t)sceneInstances.size() &&
		sceneInstancesMatchLastWorldTlas;
	const uint32_t activeRuntimeLightCount = (uint32_t)renderer.mSceneLights.GetAnalyticLights().activeLights.size();
	const uint64_t estimatedRuntimeLightSize = (uint64_t)activeRuntimeLightCount * sizeof(NRIRuntimePointLightGpuData);
	const uint64_t estimatedRuntimeLightTileHeaderSize =
		(uint64_t)renderer.mBoundRuntimeLightTileCountX *
		renderer.mBoundRuntimeLightTileCountY *
		sizeof(NRIRuntimeLightTileHeaderGpuData);
	const uint64_t estimatedRuntimeLightTileIndexSize =
		(uint64_t)renderer.mBoundRuntimeLightTileIndexCount * sizeof(uint32_t);
	const uint64_t estimatedSectorLightHeaderSize = sizeof(NRISectorLightHeaderGpuData);
	const uint64_t estimatedSectorLightSize =
		(uint64_t)renderer.mBoundSectorLightSectorCount * sizeof(NRISectorLightGpuData);
	bool useSceneDataFrameRing = renderer.ShouldUseSceneDataFrameRing();
	bool sceneDataFrameRingFallback = false;
	bool sceneDataFrameRingOverCap = false;
	NRISceneDataFrameSlot* sceneDataFrameSlot = nullptr;
	if (useSceneDataFrameRing)
	{
		sceneDataFrameSlot = &renderer.GetCurrentSceneDataFrameSlot();
		const uint64_t maxRingBytes = (uint64_t)(int)nri_ptscenedataringmaxbytes;
		if (maxRingBytes != 0)
		{
			const uint64_t currentRingCapacity = renderer.GetSceneDataFrameRingCapacityBytes();
			const uint64_t currentSlotCapacity = sceneDataFrameSlot->CapacityBytes();
			const uint64_t estimatedSlotCapacity = EstimateSceneDataFrameSlotCapacity(
				*sceneDataFrameSlot,
				reprojectionSize,
				visibleChunkSize,
				visibleFlatPlaneSize,
				spatialAbsenceSize,
				spatialAbsenceTypedSize,
				0u,
				estimatedRuntimeLightSize,
				estimatedRuntimeLightTileHeaderSize,
				estimatedRuntimeLightTileIndexSize,
				estimatedSectorLightHeaderSize,
				estimatedSectorLightSize);
			if (currentRingCapacity - currentSlotCapacity + estimatedSlotCapacity > maxRingBytes)
			{
				renderer.mSceneDataFrameRingDisabledFrameIndex = renderer.mFrameIndex;
				renderer.mSceneDataFrameRingOverCapFrameIndex = renderer.mFrameIndex;
				renderer.mSceneDataFrameRingFallbackCount++;
				renderer.mSceneDataFrameRingOverCapCount++;
				sceneDataFrameRingFallback = true;
				sceneDataFrameRingOverCap = true;
				useSceneDataFrameRing = false;
				sceneDataFrameSlot = nullptr;
			}
		}
	}
	renderer.NoteSceneDataFrameRingTelemetry(sceneDataFrameSlot, useSceneDataFrameRing, sceneDataFrameRingFallback, sceneDataFrameRingOverCap);
	NRIRenderer::SceneDataDescriptorSnapshot* sceneDataSnapshot =
		sceneDataFrameSlot != nullptr && sceneInstanceCanUseFrameSlot ?
		acquireSceneDataDescriptorSnapshot() :
		nullptr;
	const bool sceneInstanceUsesFrameSlot = sceneDataSnapshot != nullptr;
	if (sceneInstanceUsesFrameSlot)
	{
		renderer.mActiveSceneDataSet = sceneDataSnapshot->sceneDataSet;
		renderer.mActiveSceneDataSnapshot = sceneDataSnapshot;
		renderer.mActiveSceneDataSetFrameIndex = renderer.mFrameIndex;
	}

	{
		ScopedPtPerfTimer reprojectionTimer(renderer.mLastPerfShellTraceStats.sceneDataSetReprojectionMs);
		if (!UpdateReprojectionBuffer(renderer, &waitedForWrites, useSceneDataFrameRing))
		{
			return false;
		}
	}

	{
		ScopedPtPerfTimer visibleFlatTimer(renderer.mLastPerfShellTraceStats.sceneDataSetVisibleFlatPlaneMs);
		if (!UpdateVisibleFlatPlaneBuffer(renderer, &waitedForWrites, useSceneDataFrameRing))
		{
			return false;
		}
	}

	{
		ScopedPtPerfTimer visibleChunkTimer(renderer.mLastPerfShellTraceStats.sceneDataSetVisibleChunkMs);
		if (!UpdateVisibleChunkBuffer(renderer, &waitedForWrites, useSceneDataFrameRing))
		{
			return false;
		}
	}

	if (!UpdateSpatialAbsenceBuffer(renderer, &waitedForWrites, useSceneDataFrameRing))
	{
		return false;
	}

	renderer.mBoundRuntimeLightCount = 0;

	NRIBufferResource& sceneInstanceBuffer =
		sceneInstanceUsesFrameSlot ? sceneDataSnapshot->sceneInstanceBuffer : renderer.mSceneInstanceBuffer;
	SceneBufferDebugStats& sceneInstanceStats =
		sceneInstanceUsesFrameSlot ? sceneDataSnapshot->sceneInstanceStats : renderer.mSceneInstanceBufferStats;
	const bool sceneInstanceNeedsUpload =
		sceneInstanceUsesFrameSlot ||
		!renderer.mSceneInstancePayloadCacheValid ||
		renderer.mSceneInstancePayloadHash != sceneInstanceHash ||
		renderer.mSceneInstancePayloadCount != (uint32_t)sceneInstances.size() ||
		sceneInstanceBuffer.buffer == nullptr ||
		sceneInstanceBuffer.shaderView == nullptr;
	if (sceneInstanceNeedsUpload)
	{
		if (!ensureSceneDataBatched(
			sceneInstanceBuffer,
			sceneInstanceStats,
			"scene_instance",
			sceneInstances.data(),
			sceneInstanceSize,
			sizeof(SceneInstanceData),
			nri::BufferUsageBits::SHADER_RESOURCE,
			NRIResourceComputeShaderResourceAccess(),
			renderer.mLastPerfShellTraceStats.sceneDataSetSceneInstanceMs,
			renderer.mLastPerfShellTraceStats.sceneDataSetSceneInstanceRequestedBytes,
			renderer.mLastPerfShellTraceStats.sceneDataSetSceneInstanceUploadedBytes,
			sceneInstanceUsesFrameSlot))
		{
			return false;
		}
		if (!sceneInstanceUsesFrameSlot)
		{
			renderer.mSceneInstancePayloadCacheValid = true;
			renderer.mSceneInstancePayloadHash = sceneInstanceHash;
			renderer.mSceneInstancePayloadCount = (uint32_t)sceneInstances.size();
		}
	}
	renderer.mBoundSceneInstances = sceneInstances;

	const bool portalNeedsUpload =
		!renderer.mPortalPayloadCacheValid ||
		renderer.mPortalPayloadHash != portalHash ||
		renderer.mPortalPayloadBuildSerial != portalBuildSerial ||
		renderer.mPortalPayloadCount != (uint32_t)scenePortals.size() ||
		renderer.mPortalBuffer.shaderView == nullptr ||
		renderer.mPortalBuffer.usedSize < portalSize;
	if (portalNeedsUpload)
	{
		if (!ensureSceneDataBatched(
			renderer.mPortalBuffer,
			renderer.mPortalBufferStats,
			"portal",
			scenePortals.data(),
			portalSize,
			sizeof(ScenePortalData),
			nri::BufferUsageBits::SHADER_RESOURCE,
			NRIResourceComputeShaderResourceAccess(),
			renderer.mLastPerfShellTraceStats.sceneDataSetPortalMs,
			renderer.mLastPerfShellTraceStats.sceneDataSetPortalRequestedBytes,
			renderer.mLastPerfShellTraceStats.sceneDataSetPortalUploadedBytes,
			false))
		{
			return false;
		}
		renderer.mPortalPayloadCacheValid = true;
		renderer.mPortalPayloadHash = portalHash;
		renderer.mPortalPayloadBuildSerial = portalBuildSerial;
		renderer.mPortalPayloadCount = (uint32_t)scenePortals.size();
	}

	uint32_t snapshotMismatchCount = 0;
	if (renderer.mLastWorldTlasInstanceFrameIndex == renderer.mFrameIndex &&
		tlasInstanceCount != 0 &&
		tlasInstanceCount != (uint32_t)sceneInstances.size())
	{
		snapshotMismatchCount++;
	}

	uint64_t runtimeLightPayloadHash = 0;
	{
		ScopedPtPerfTimer hashTimer(renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightHashMs);
		runtimeLightPayloadHash = renderer.mSceneLights.BuildRuntimeLightPayloadHash();
	}
	const bool runtimeLightUsesFrameSlot = sceneDataFrameSlot != nullptr;
	NRIBufferResource& runtimeLightBuffer =
		runtimeLightUsesFrameSlot ? sceneDataFrameSlot->runtimeLightBuffer : renderer.mRuntimeLightBuffer;
	SceneBufferDebugStats& runtimeLightStats =
		runtimeLightUsesFrameSlot ? sceneDataFrameSlot->runtimeLightStats : renderer.mRuntimeLightBufferStats;
	uint32_t boundRuntimeLightCount = activeRuntimeLightCount;
	const bool runtimeLightFrameSlotCacheHit =
		runtimeLightUsesFrameSlot &&
		sceneDataFrameSlot->lightReuse.runtimeLight.CanReuse(
			runtimeLightPayloadHash,
			activeRuntimeLightCount,
			BuildSceneDataLightBufferReuseView(runtimeLightBuffer),
			sizeof(NRIRuntimePointLightGpuData));
	const bool runtimeLightNeedsUpload =
		runtimeLightUsesFrameSlot ?
		!runtimeLightFrameSlotCacheHit :
		(!renderer.mRuntimeLightPayloadCacheValid ||
			renderer.mRuntimeLightPayloadHash != runtimeLightPayloadHash ||
			runtimeLightBuffer.shaderView == nullptr);
	if (runtimeLightNeedsUpload)
	{
		if (runtimeLightUsesFrameSlot)
		{
			sceneDataFrameSlot->lightReuse.runtimeLight.Invalidate();
		}
		else
		{
			renderer.mRuntimeLightPayloadCacheValid = false;
		}
		renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightUploads++;
		std::vector<NRIRuntimePointLightGpuData> runtimeLights;
		{
			ScopedPtPerfTimer runtimeLightTimer(renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightUploadMs);
			renderer.mSceneLights.BuildRuntimePointLightUpload(runtimeLights);
		}
		if (!ensureSceneDataBatched(
			runtimeLightBuffer,
			runtimeLightStats,
			"runtime_light",
			runtimeLights.empty() ? nullptr : runtimeLights.data(),
			runtimeLights.size() * sizeof(NRIRuntimePointLightGpuData),
			sizeof(NRIRuntimePointLightGpuData),
			nri::BufferUsageBits::SHADER_RESOURCE,
			NRIResourceComputeShaderResourceAccess(),
			renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightUploadMs,
			renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightRequestedBytes,
			renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightUploadedBytes,
			runtimeLightUsesFrameSlot))
		{
			return false;
		}

		if (runtimeLightUsesFrameSlot)
		{
			sceneDataFrameSlot->lightReuse.runtimeLight.Commit(
				runtimeLightPayloadHash,
				(uint32_t)runtimeLights.size(),
				BuildSceneDataLightBufferReuseView(runtimeLightBuffer));
		}
		else
		{
			renderer.mRuntimeLightPayloadCacheValid = true;
			renderer.mRuntimeLightPayloadHash = runtimeLightPayloadHash;
		}
	}
	else
	{
		if (runtimeLightUsesFrameSlot)
		{
			boundRuntimeLightCount = sceneDataFrameSlot->lightReuse.runtimeLight.lightCount;
		}
		renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightCacheHits++;
	}

	uint32_t runtimeLightTileCountX = 0;
	uint32_t runtimeLightTileCountY = 0;
	uint32_t runtimeLightTileIndexCount = 0;
	uint32_t runtimeLightMaxTileOccupancy = 0;
	uint64_t runtimeLightClusterCameraHash = 0;
	{
		ScopedPtPerfTimer runtimeLightClusterTimer(renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightClusterMs);
		runtimeLightClusterCameraHash = renderer.mSceneLights.BuildRuntimeLightClusterCameraHash(renderer.BuildRuntimeLightClusterInput());
	}
	const uint64_t runtimeLightClusterPayloadHash =
		nri_scene::HashCombine64(runtimeLightPayloadHash, runtimeLightClusterCameraHash);
	NRIBufferResource& runtimeLightTileHeaderBuffer =
		runtimeLightUsesFrameSlot ? sceneDataFrameSlot->runtimeLightTileHeaderBuffer : renderer.mRuntimeLightTileHeaderBuffer;
	SceneBufferDebugStats& runtimeLightTileHeaderStats =
		runtimeLightUsesFrameSlot ? sceneDataFrameSlot->runtimeLightTileHeaderStats : renderer.mRuntimeLightTileHeaderBufferStats;
	NRIBufferResource& runtimeLightTileIndexBuffer =
		runtimeLightUsesFrameSlot ? sceneDataFrameSlot->runtimeLightTileIndexBuffer : renderer.mRuntimeLightTileIndexBuffer;
	SceneBufferDebugStats& runtimeLightTileIndexStats =
		runtimeLightUsesFrameSlot ? sceneDataFrameSlot->runtimeLightTileIndexStats : renderer.mRuntimeLightTileIndexBufferStats;
	const bool runtimeLightClusterFrameSlotCacheHit =
		runtimeLightUsesFrameSlot &&
		sceneDataFrameSlot->lightReuse.runtimeLightCluster.CanReuse(
			runtimeLightClusterPayloadHash,
			BuildSceneDataLightBufferReuseView(runtimeLightTileHeaderBuffer),
			sizeof(NRIRuntimeLightTileHeaderGpuData),
			BuildSceneDataLightBufferReuseView(runtimeLightTileIndexBuffer),
			sizeof(uint32_t));
	const bool runtimeLightClusterNeedsUpload =
		runtimeLightUsesFrameSlot ?
		!runtimeLightClusterFrameSlotCacheHit :
		(!renderer.mRuntimeLightClusterCacheValid ||
			renderer.mRuntimeLightClusterPayloadHash != runtimeLightClusterPayloadHash ||
			runtimeLightTileHeaderBuffer.shaderView == nullptr ||
			runtimeLightTileIndexBuffer.shaderView == nullptr);
	if (runtimeLightClusterNeedsUpload)
	{
		if (runtimeLightUsesFrameSlot)
		{
			sceneDataFrameSlot->lightReuse.runtimeLightCluster.Invalidate();
		}
		else
		{
			renderer.mRuntimeLightClusterCacheValid = false;
		}
		renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightClusterUploads++;
		std::vector<NRIRuntimeLightTileHeaderGpuData> runtimeLightTileHeaders;
		std::vector<uint32_t> runtimeLightTileIndices;
		{
			ScopedPtPerfTimer runtimeLightClusterTimer(renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightClusterMs);
			renderer.mSceneLights.BuildRuntimeLightClusterUpload(
				renderer.BuildRuntimeLightClusterInput(),
				runtimeLightTileHeaders,
				runtimeLightTileIndices,
				runtimeLightTileCountX,
				runtimeLightTileCountY,
				runtimeLightTileIndexCount,
				runtimeLightMaxTileOccupancy);
		}
		if (!ensureSceneDataBatched(
			runtimeLightTileHeaderBuffer,
			runtimeLightTileHeaderStats,
			"runtime_light_tile_header",
			runtimeLightTileHeaders.data(),
			runtimeLightTileHeaders.size() * sizeof(NRIRuntimeLightTileHeaderGpuData),
			sizeof(NRIRuntimeLightTileHeaderGpuData),
			nri::BufferUsageBits::SHADER_RESOURCE,
			NRIResourceComputeShaderResourceAccess(),
			renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightClusterMs,
			renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightClusterRequestedBytes,
			renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightClusterUploadedBytes,
			runtimeLightUsesFrameSlot))
		{
			return false;
		}

		if (!ensureSceneDataBatched(
			runtimeLightTileIndexBuffer,
			runtimeLightTileIndexStats,
			"runtime_light_tile_index",
			runtimeLightTileIndices.data(),
			runtimeLightTileIndices.size() * sizeof(uint32_t),
			sizeof(uint32_t),
			nri::BufferUsageBits::SHADER_RESOURCE,
			NRIResourceComputeShaderResourceAccess(),
			renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightClusterMs,
			renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightClusterRequestedBytes,
			renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightClusterUploadedBytes,
			runtimeLightUsesFrameSlot))
		{
			return false;
		}

		if (runtimeLightUsesFrameSlot)
		{
			sceneDataFrameSlot->lightReuse.runtimeLightCluster.Commit(
				runtimeLightClusterPayloadHash,
				runtimeLightTileCountX,
				runtimeLightTileCountY,
				runtimeLightTileIndexCount,
				runtimeLightMaxTileOccupancy,
				BuildSceneDataLightBufferReuseView(runtimeLightTileHeaderBuffer),
				BuildSceneDataLightBufferReuseView(runtimeLightTileIndexBuffer));
		}
		else
		{
			renderer.mRuntimeLightClusterCacheValid = true;
			renderer.mRuntimeLightClusterPayloadHash = runtimeLightClusterPayloadHash;
			renderer.mRuntimeLightClusterCameraHash = runtimeLightClusterCameraHash;
		}
	}
	else
	{
		if (runtimeLightUsesFrameSlot)
		{
			runtimeLightTileCountX = sceneDataFrameSlot->lightReuse.runtimeLightCluster.tileCountX;
			runtimeLightTileCountY = sceneDataFrameSlot->lightReuse.runtimeLightCluster.tileCountY;
			runtimeLightTileIndexCount = sceneDataFrameSlot->lightReuse.runtimeLightCluster.tileIndexCount;
			runtimeLightMaxTileOccupancy = sceneDataFrameSlot->lightReuse.runtimeLightCluster.maxTileOccupancy;
		}
		else
		{
			runtimeLightTileCountX = renderer.mBoundRuntimeLightTileCountX;
			runtimeLightTileCountY = renderer.mBoundRuntimeLightTileCountY;
			runtimeLightTileIndexCount = renderer.mBoundRuntimeLightTileIndexCount;
			runtimeLightMaxTileOccupancy = renderer.mBoundRuntimeLightMaxTileOccupancy;
		}
		renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightClusterCacheHits++;
	}

	if (renderer.mEmissivePrimitiveHeaderBuffer.shaderView == nullptr ||
		renderer.mEmissivePrimitiveBuffer.shaderView == nullptr ||
		renderer.mEmissivePrimitiveCdfBuffer.shaderView == nullptr ||
		renderer.mEmissiveMaterialResponseBuffer.shaderView == nullptr)
	{
		renderer.mLastPerfShellTraceStats.sceneDataSetEmissiveUploads++;
		NRIEmissivePrimitiveHeaderGpuData emissiveHeader = {};
		std::vector<NRIEmissivePrimitiveGpuData> emissivePrimitives;
		std::vector<NRIEmissivePrimitiveShaderData> emissiveShaderPrimitives;
		std::vector<float> emissiveCdf;
		std::vector<NRIEmissiveMaterialResponseGpuData> emissiveMaterialResponses;
		std::vector<NRIEmissivePrimitiveDebugRecord> ignoredEmissiveDebugRecords;
		{
			ScopedPtPerfTimer emissiveTimer(renderer.mLastPerfShellTraceStats.sceneDataSetEmissiveMs);
			renderer.mSceneLights.BuildEmissiveSamplingUpload({}, emissiveHeader, emissivePrimitives, emissiveCdf, emissiveMaterialResponses, ignoredEmissiveDebugRecords);
		}
		emissiveShaderPrimitives.reserve(emissivePrimitives.size());
		for (const NRIEmissivePrimitiveGpuData& primitive : emissivePrimitives)
		{
			emissiveShaderPrimitives.push_back(PackNRIEmissivePrimitiveShaderData(primitive));
		}
		if (!ensureSceneDataBatched(
			renderer.mEmissivePrimitiveHeaderBuffer,
			renderer.mEmissivePrimitiveHeaderBufferStats,
			"emissive_header",
			&emissiveHeader,
			sizeof(emissiveHeader),
			sizeof(NRIEmissivePrimitiveHeaderGpuData),
			nri::BufferUsageBits::SHADER_RESOURCE,
			NRIResourceComputeShaderResourceAccess(),
			renderer.mLastPerfShellTraceStats.sceneDataSetEmissiveMs,
			renderer.mLastPerfShellTraceStats.sceneDataSetEmissiveRequestedBytes,
			renderer.mLastPerfShellTraceStats.sceneDataSetEmissiveUploadedBytes,
			false))
		{
			return false;
		}

		if (!ensureSceneDataBatched(
			renderer.mEmissivePrimitiveBuffer,
			renderer.mEmissivePrimitiveBufferStats,
			"emissive_primitives",
			emissiveShaderPrimitives.empty() ? nullptr : emissiveShaderPrimitives.data(),
			emissiveShaderPrimitives.empty() ? 0u : emissiveShaderPrimitives.size() * sizeof(NRIEmissivePrimitiveShaderData),
			sizeof(NRIEmissivePrimitiveShaderData),
			nri::BufferUsageBits::SHADER_RESOURCE,
			NRIResourceComputeShaderResourceAccess(),
			renderer.mLastPerfShellTraceStats.sceneDataSetEmissiveMs,
			renderer.mLastPerfShellTraceStats.sceneDataSetEmissiveRequestedBytes,
			renderer.mLastPerfShellTraceStats.sceneDataSetEmissiveUploadedBytes,
			false))
		{
			return false;
		}

		if (!ensureSceneDataBatched(
			renderer.mEmissivePrimitiveCdfBuffer,
			renderer.mEmissivePrimitiveCdfBufferStats,
			"emissive_cdf",
			emissiveCdf.data(),
			emissiveCdf.size() * sizeof(float),
			sizeof(float),
			nri::BufferUsageBits::SHADER_RESOURCE,
			NRIResourceComputeShaderResourceAccess(),
			renderer.mLastPerfShellTraceStats.sceneDataSetEmissiveMs,
			renderer.mLastPerfShellTraceStats.sceneDataSetEmissiveRequestedBytes,
			renderer.mLastPerfShellTraceStats.sceneDataSetEmissiveUploadedBytes,
			false))
		{
			return false;
		}

		if (!ensureSceneDataBatched(
			renderer.mEmissiveMaterialResponseBuffer,
			renderer.mEmissiveMaterialResponseBufferStats,
			"emissive_material_response",
			emissiveMaterialResponses.empty() ? nullptr : emissiveMaterialResponses.data(),
			emissiveMaterialResponses.empty() ? 0u : emissiveMaterialResponses.size() * sizeof(NRIEmissiveMaterialResponseGpuData),
			sizeof(NRIEmissiveMaterialResponseGpuData),
			nri::BufferUsageBits::SHADER_RESOURCE,
			NRIResourceComputeShaderResourceAccess(),
			renderer.mLastPerfShellTraceStats.sceneDataSetEmissiveMs,
			renderer.mLastPerfShellTraceStats.sceneDataSetEmissiveRequestedBytes,
			renderer.mLastPerfShellTraceStats.sceneDataSetEmissiveUploadedBytes,
			false))
		{
			return false;
		}
	}
	else
	{
		renderer.mLastPerfShellTraceStats.sceneDataSetEmissiveCacheHits++;
	}

	uint64_t sectorLightingPayloadHash = 0;
	{
		ScopedPtPerfTimer sectorLightTimer(renderer.mLastPerfShellTraceStats.sceneDataSetSectorLightMs);
		renderer.UpdateBoundSectorLightingState();
		sectorLightingPayloadHash = renderer.mSceneLights.BuildSectorLightingPayloadHash(NRIGetSectorLightMultiplier(), nri_ptsectorlighting);
	}
	NRIBufferResource& sectorLightHeaderBuffer =
		runtimeLightUsesFrameSlot ? sceneDataFrameSlot->sectorLightHeaderBuffer : renderer.mSectorLightHeaderBuffer;
	SceneBufferDebugStats& sectorLightHeaderStats =
		runtimeLightUsesFrameSlot ? sceneDataFrameSlot->sectorLightHeaderStats : renderer.mSectorLightHeaderBufferStats;
	NRIBufferResource& sectorLightBuffer =
		runtimeLightUsesFrameSlot ? sceneDataFrameSlot->sectorLightBuffer : renderer.mSectorLightBuffer;
	SceneBufferDebugStats& sectorLightStats =
		runtimeLightUsesFrameSlot ? sceneDataFrameSlot->sectorLightStats : renderer.mSectorLightBufferStats;
	const bool sectorLightFrameSlotCacheHit =
		runtimeLightUsesFrameSlot &&
		sceneDataFrameSlot->lightReuse.sectorLight.CanReuse(
			sectorLightingPayloadHash,
			renderer.mBoundSectorLightSectorCount,
			BuildSceneDataLightBufferReuseView(sectorLightHeaderBuffer),
			sizeof(NRISectorLightHeaderGpuData),
			BuildSceneDataLightBufferReuseView(sectorLightBuffer),
			sizeof(NRISectorLightGpuData));
	const bool sectorLightNeedsUpload =
		runtimeLightUsesFrameSlot ?
		!sectorLightFrameSlotCacheHit :
		(!renderer.mSectorLightingPayloadCacheValid ||
			renderer.mSectorLightingPayloadHash != sectorLightingPayloadHash ||
			sectorLightHeaderBuffer.shaderView == nullptr ||
			sectorLightBuffer.shaderView == nullptr);
	if (sectorLightNeedsUpload)
	{
		if (runtimeLightUsesFrameSlot)
		{
			sceneDataFrameSlot->lightReuse.sectorLight.Invalidate();
		}
		else
		{
			renderer.mSectorLightingPayloadCacheValid = false;
		}
		renderer.mLastPerfShellTraceStats.sceneDataSetSectorLightUploads++;
		NRISectorLightHeaderGpuData sectorLightHeader = {};
		std::vector<NRISectorLightGpuData> sectorLights;
		{
			ScopedPtPerfTimer sectorLightTimer(renderer.mLastPerfShellTraceStats.sceneDataSetSectorLightMs);
			renderer.UpdateBoundSectorLightingState();
			renderer.mSceneLights.BuildSectorLightingUpload(NRIGetSectorLightMultiplier(), nri_ptsectorlighting, sectorLightHeader, sectorLights);
		}
		if (!ensureSceneDataBatched(
			sectorLightHeaderBuffer,
			sectorLightHeaderStats,
			"sector_header",
			&sectorLightHeader,
			sizeof(sectorLightHeader),
			sizeof(NRISectorLightHeaderGpuData),
			nri::BufferUsageBits::SHADER_RESOURCE,
			NRIResourceComputeShaderResourceAccess(),
			renderer.mLastPerfShellTraceStats.sceneDataSetSectorLightMs,
			renderer.mLastPerfShellTraceStats.sceneDataSetSectorLightRequestedBytes,
			renderer.mLastPerfShellTraceStats.sceneDataSetSectorLightUploadedBytes,
			runtimeLightUsesFrameSlot))
		{
			return false;
		}

		if (!ensureSceneDataBatched(
			sectorLightBuffer,
			sectorLightStats,
			"sector_data",
			sectorLights.empty() ? nullptr : sectorLights.data(),
			sectorLights.empty() ? 0u : sectorLights.size() * sizeof(NRISectorLightGpuData),
			sizeof(NRISectorLightGpuData),
			nri::BufferUsageBits::SHADER_RESOURCE,
			NRIResourceComputeShaderResourceAccess(),
			renderer.mLastPerfShellTraceStats.sceneDataSetSectorLightMs,
			renderer.mLastPerfShellTraceStats.sceneDataSetSectorLightRequestedBytes,
			renderer.mLastPerfShellTraceStats.sceneDataSetSectorLightUploadedBytes,
			runtimeLightUsesFrameSlot))
		{
			return false;
		}

		if (runtimeLightUsesFrameSlot)
		{
			sceneDataFrameSlot->lightReuse.sectorLight.Commit(
				sectorLightingPayloadHash,
				(uint32_t)sectorLights.size(),
				BuildSceneDataLightBufferReuseView(sectorLightHeaderBuffer),
				BuildSceneDataLightBufferReuseView(sectorLightBuffer));
		}
		else
		{
			renderer.mSectorLightingPayloadCacheValid = true;
			renderer.mSectorLightingPayloadHash = sectorLightingPayloadHash;
		}
	}
	else
	{
		if (runtimeLightUsesFrameSlot)
		{
			renderer.mBoundSectorLightSectorCount = sceneDataFrameSlot->lightReuse.sectorLight.sectorCount;
		}
		renderer.mLastPerfShellTraceStats.sceneDataSetSectorLightCacheHits++;
	}

	auto selectView = [](const NRIBufferResource& primary, const NRIBufferResource& fallback) -> nri::Descriptor*
	{
		return primary.shaderView != nullptr ? primary.shaderView : fallback.shaderView;
	};
	const NRIBufferResource& reprojectionDescriptorBuffer =
		sceneDataFrameSlot != nullptr ? sceneDataFrameSlot->reprojectionBuffer : renderer.mReprojectionBuffer;
	const NRIBufferResource& visibleChunkDescriptorBuffer =
		sceneDataFrameSlot != nullptr ? sceneDataFrameSlot->visibleChunkBuffer : renderer.mVisibleChunkBuffer;
	const NRIBufferResource& visibleFlatPlaneDescriptorBuffer =
		sceneDataFrameSlot != nullptr ? sceneDataFrameSlot->visibleFlatPlaneBuffer : renderer.mVisibleFlatPlaneBuffer;
	const NRIBufferResource& spatialAbsenceDescriptorBuffer =
		sceneDataFrameSlot != nullptr ? sceneDataFrameSlot->spatialAbsenceBuffer : renderer.mSpatialAbsenceBuffer;
	const NRIBufferResource& spatialAbsenceTypedDescriptorBuffer =
		sceneDataFrameSlot != nullptr ? sceneDataFrameSlot->spatialAbsenceTypedBuffer : renderer.mSpatialAbsenceTypedBuffer;
	const NRIBufferResource& runtimeLightDescriptorBuffer =
		sceneDataFrameSlot != nullptr ? sceneDataFrameSlot->runtimeLightBuffer : renderer.mRuntimeLightBuffer;
	const NRIBufferResource& runtimeLightTileHeaderDescriptorBuffer =
		sceneDataFrameSlot != nullptr ? sceneDataFrameSlot->runtimeLightTileHeaderBuffer : renderer.mRuntimeLightTileHeaderBuffer;
	const NRIBufferResource& runtimeLightTileIndexDescriptorBuffer =
		sceneDataFrameSlot != nullptr ? sceneDataFrameSlot->runtimeLightTileIndexBuffer : renderer.mRuntimeLightTileIndexBuffer;
	const NRIBufferResource& sectorLightHeaderDescriptorBuffer =
		sceneDataFrameSlot != nullptr ? sceneDataFrameSlot->sectorLightHeaderBuffer : renderer.mSectorLightHeaderBuffer;
	const NRIBufferResource& sectorLightDescriptorBuffer =
		sceneDataFrameSlot != nullptr ? sceneDataFrameSlot->sectorLightBuffer : renderer.mSectorLightBuffer;
	const NRIBufferResource& sceneInstanceDescriptorBuffer =
		sceneInstanceUsesFrameSlot ? sceneDataSnapshot->sceneInstanceBuffer : renderer.mSceneInstanceBuffer;
	{
		ScopedPtPerfTimer descriptorBuildTimer(renderer.mLastPerfShellTraceStats.sceneDataSetDescriptorBuildMs);
		renderer.mSceneDataDescriptors.fill(nullptr);
		renderer.mSceneDataDescriptors[0] = selectView(staticVertexBuffer, dynamicVertexBuffer);
		renderer.mSceneDataDescriptors[1] = selectView(staticIndexBuffer, dynamicIndexBuffer);
		renderer.mSceneDataDescriptors[2] = selectView(staticPrimitiveBuffer, dynamicPrimitiveBuffer);
		renderer.mSceneDataDescriptors[3] = selectView(staticMaterialBuffer, dynamicMaterialBuffer);
		renderer.mSceneDataDescriptors[4] = selectView(dynamicVertexBuffer, staticVertexBuffer);
		renderer.mSceneDataDescriptors[5] = selectView(dynamicIndexBuffer, staticIndexBuffer);
		renderer.mSceneDataDescriptors[6] = selectView(dynamicPrimitiveBuffer, staticPrimitiveBuffer);
		renderer.mSceneDataDescriptors[7] = selectView(dynamicMaterialBuffer, staticMaterialBuffer);
		renderer.mSceneDataDescriptors[8] = sceneInstanceDescriptorBuffer.shaderView;
		renderer.mSceneDataDescriptors[9] = renderer.mPortalBuffer.shaderView;
		renderer.mSceneDataDescriptors[10] = runtimeLightDescriptorBuffer.shaderView;
		renderer.mSceneDataDescriptors[11] = runtimeLightTileHeaderDescriptorBuffer.shaderView;
		renderer.mSceneDataDescriptors[12] = runtimeLightTileIndexDescriptorBuffer.shaderView;
		renderer.mSceneDataDescriptors[13] = renderer.mEmissivePrimitiveHeaderBuffer.shaderView;
		renderer.mSceneDataDescriptors[14] = renderer.mEmissivePrimitiveBuffer.shaderView;
		renderer.mSceneDataDescriptors[15] = renderer.mEmissivePrimitiveCdfBuffer.shaderView;
		renderer.mSceneDataDescriptors[16] = sectorLightHeaderDescriptorBuffer.shaderView;
		renderer.mSceneDataDescriptors[17] = sectorLightDescriptorBuffer.shaderView;
		renderer.mSceneDataDescriptors[18] = reprojectionDescriptorBuffer.shaderView;
		renderer.mSceneDataDescriptors[19] = visibleChunkDescriptorBuffer.shaderView;
		renderer.mSceneDataDescriptors[20] = visibleFlatPlaneDescriptorBuffer.shaderView;
		const NRIPersistentVoxelDescriptorSnapshot persistentVoxelDescriptors =
			renderer.mPersistentVoxels.BuildDescriptorSnapshot(dynamicVertexBuffer, dynamicIndexBuffer, dynamicPrimitiveBuffer, dynamicMaterialBuffer);
		renderer.mSceneDataDescriptors[21] = persistentVoxelDescriptors.vertex;
		renderer.mSceneDataDescriptors[22] = persistentVoxelDescriptors.index;
		renderer.mSceneDataDescriptors[23] = persistentVoxelDescriptors.primitive;
		renderer.mSceneDataDescriptors[24] = persistentVoxelDescriptors.material;
		renderer.mSceneDataDescriptors[25] = renderer.mEmissiveMaterialResponseBuffer.shaderView;
		renderer.mSceneDataDescriptors[NRI_SCENE_DATA_SPATIAL_ABSENCE_RAW_SLOT] = spatialAbsenceDescriptorBuffer.shaderView;
		renderer.mSceneDataDescriptors[NRI_SCENE_DATA_SPATIAL_ABSENCE_TYPED_SLOT] = spatialAbsenceTypedDescriptorBuffer.shaderView;
	}

	{
		ScopedPtPerfTimer descriptorValidateTimer(renderer.mLastPerfShellTraceStats.sceneDataSetDescriptorValidateMs);
		for (const nri::Descriptor* descriptor : renderer.mSceneDataDescriptors)
		{
			if (descriptor == nullptr)
			{
				renderer.mLastPerfShellTraceStats.sceneDataSetDescriptorNullCount++;
				return false;
			}
		}
	}

	if (!NRIDescriptorSetManager::CommitSceneDataDescriptors(renderer, sceneDataReason))
	{
		return false;
	}

	renderer.mSceneDataSnapshotGeneration = ++renderer.mSceneDataSnapshotGenerationCounter;
	renderer.mSceneDataSnapshotFrameIndex = renderer.mFrameIndex;
	renderer.mSceneDataSnapshotQueuedFrameIndex = renderer.GetCurrentQueuedFrameIndex();
	renderer.mSceneDataSnapshotSceneInstanceHash = sceneInstanceHash;
	renderer.mSceneDataSnapshotTlasInstanceHash = tlasInstanceHash;
	renderer.mSceneDataSnapshotPortalHash = portalHash;
	renderer.mSceneDataSnapshotSceneInstanceCount = (uint32_t)sceneInstances.size();
	renderer.mSceneDataSnapshotTlasInstanceCount = tlasInstanceCount;
	renderer.mSceneDataSnapshotPortalCount = (uint32_t)scenePortals.size();
	if (sceneDataFrameSlot != nullptr)
	{
		sceneDataFrameSlot->snapshotGeneration = renderer.mSceneDataSnapshotGeneration;
		sceneDataFrameSlot->sceneInstanceHash = sceneInstanceHash;
		sceneDataFrameSlot->tlasInstanceHash = tlasInstanceHash;
		sceneDataFrameSlot->portalHash = portalHash;
		sceneDataFrameSlot->sceneInstanceCount = (uint32_t)sceneInstances.size();
		sceneDataFrameSlot->tlasInstanceCount = tlasInstanceCount;
		sceneDataFrameSlot->portalCount = (uint32_t)scenePortals.size();
	}
	renderer.mLastPerfShellTraceStats.sceneDataSnapshotGeneration = renderer.mSceneDataSnapshotGeneration;
	renderer.mLastPerfShellTraceStats.sceneDataDescriptorGeneration = renderer.mSceneDataDescriptorGeneration;
	renderer.mLastPerfShellTraceStats.sceneDataSceneInstanceHash = sceneInstanceHash;
	renderer.mLastPerfShellTraceStats.sceneDataTlasInstanceHash = tlasInstanceHash;
	renderer.mLastPerfShellTraceStats.sceneDataPortalHash = portalHash;
	renderer.mLastPerfShellTraceStats.sceneDataSnapshotMismatchCount += snapshotMismatchCount;
	renderer.mLastPerfShellTraceStats.sceneDataSceneInstanceCount = (uint32_t)sceneInstances.size();
	renderer.mLastPerfShellTraceStats.sceneDataTlasInstanceCount = tlasInstanceCount;
	renderer.mLastPerfShellTraceStats.sceneDataPortalCount = (uint32_t)scenePortals.size();
	renderer.NoteSceneDataFrameRingTelemetry(sceneDataFrameSlot, useSceneDataFrameRing, false, sceneDataFrameRingOverCap);

	renderer.mBoundStaticPrimitiveCount = staticPrimitiveCount;
	renderer.mBoundDynamicPrimitiveCount = dynamicPrimitiveCount;
	renderer.mBoundStaticMaterialCount = staticMaterialCount;
	renderer.mBoundDynamicMaterialCount = dynamicMaterialCount;
	renderer.mBoundPortalCount = renderer.mMapWorld.valid ? (uint32_t)renderer.mMapWorld.portals.size() : 0u;
	renderer.mBoundRuntimeLightCount = boundRuntimeLightCount;
	renderer.mBoundRuntimeLightTileCountX = runtimeLightTileCountX;
	renderer.mBoundRuntimeLightTileCountY = runtimeLightTileCountY;
	renderer.mBoundRuntimeLightTileSize = NRI_RUNTIME_LIGHT_TILE_SIZE;
	renderer.mBoundRuntimeLightTileIndexCount = runtimeLightTileIndexCount;
	renderer.mBoundRuntimeLightMaxTileOccupancy = runtimeLightMaxTileOccupancy;
	renderer.mRuntimeLightSceneDataDirty = false;
	return true;
}

bool NRISceneUploadManager::UpdateRuntimeLightAndSectorSceneData(NRIRenderer& renderer, const char* reason)
{
	ScopedPtPerfTimer perfTimer(renderer.mLastPerfShellTraceStats.sceneDataSetMs);
	renderer.mLastPerfShellTraceStats.sceneDataSetCalls++;
	const char* sceneDataReason = reason != nullptr ? reason : "runtime_light_sector_refresh";
	if (!SceneDataDescriptorsReady(renderer))
	{
		return false;
	}

	bool waitedForWrites = false;
	const auto ensureSceneDataBatched = [&](NRIBufferResource& resource, SceneBufferDebugStats& stats, const char* bufferLabel, const void* data, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after, double& uploadMs, uint64_t& requestedBytes, uint64_t& uploadedBytes, bool writesQuiesced) -> bool
	{
		return EnsureSceneDataBatched(renderer, waitedForWrites, sceneDataReason, resource, stats, bufferLabel, data, size, stride, usage, after, uploadMs, requestedBytes, uploadedBytes, writesQuiesced);
	};
	NRISceneDataFrameSlot* sceneDataFrameSlot = renderer.ShouldUseSceneDataFrameRing() ? &renderer.GetCurrentSceneDataFrameSlot() : nullptr;
	const bool runtimeLightUsesFrameSlot = sceneDataFrameSlot != nullptr;

	uint64_t runtimeLightPayloadHash = 0;
	{
		ScopedPtPerfTimer hashTimer(renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightHashMs);
		runtimeLightPayloadHash = renderer.mSceneLights.BuildRuntimeLightPayloadHash();
	}
	NRIBufferResource& runtimeLightBuffer =
		runtimeLightUsesFrameSlot ? sceneDataFrameSlot->runtimeLightBuffer : renderer.mRuntimeLightBuffer;
	SceneBufferDebugStats& runtimeLightStats =
		runtimeLightUsesFrameSlot ? sceneDataFrameSlot->runtimeLightStats : renderer.mRuntimeLightBufferStats;
	const uint32_t activeRuntimeLightCount = (uint32_t)renderer.mSceneLights.GetAnalyticLights().activeLights.size();
	uint32_t boundRuntimeLightCount = activeRuntimeLightCount;
	const bool runtimeLightFrameSlotCacheHit =
		runtimeLightUsesFrameSlot &&
		sceneDataFrameSlot->lightReuse.runtimeLight.CanReuse(
			runtimeLightPayloadHash,
			activeRuntimeLightCount,
			BuildSceneDataLightBufferReuseView(runtimeLightBuffer),
			sizeof(NRIRuntimePointLightGpuData));
	const bool runtimeLightNeedsUpload =
		runtimeLightUsesFrameSlot ?
		!runtimeLightFrameSlotCacheHit :
		(!renderer.mRuntimeLightPayloadCacheValid ||
			renderer.mRuntimeLightPayloadHash != runtimeLightPayloadHash ||
			runtimeLightBuffer.shaderView == nullptr);
	if (runtimeLightNeedsUpload)
	{
		if (runtimeLightUsesFrameSlot)
		{
			sceneDataFrameSlot->lightReuse.runtimeLight.Invalidate();
		}
		else
		{
			renderer.mRuntimeLightPayloadCacheValid = false;
		}
		renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightUploads++;
		std::vector<NRIRuntimePointLightGpuData> runtimeLights;
		{
			ScopedPtPerfTimer runtimeLightTimer(renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightUploadMs);
			renderer.mSceneLights.BuildRuntimePointLightUpload(runtimeLights);
		}
		if (!ensureSceneDataBatched(
			runtimeLightBuffer,
			runtimeLightStats,
			"runtime_light",
			runtimeLights.empty() ? nullptr : runtimeLights.data(),
			runtimeLights.size() * sizeof(NRIRuntimePointLightGpuData),
			sizeof(NRIRuntimePointLightGpuData),
			nri::BufferUsageBits::SHADER_RESOURCE,
			NRIResourceComputeShaderResourceAccess(),
			renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightUploadMs,
			renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightRequestedBytes,
			renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightUploadedBytes,
			runtimeLightUsesFrameSlot))
		{
			return false;
		}

		if (runtimeLightUsesFrameSlot)
		{
			sceneDataFrameSlot->lightReuse.runtimeLight.Commit(
				runtimeLightPayloadHash,
				(uint32_t)runtimeLights.size(),
				BuildSceneDataLightBufferReuseView(runtimeLightBuffer));
		}
		else
		{
			renderer.mRuntimeLightPayloadCacheValid = true;
			renderer.mRuntimeLightPayloadHash = runtimeLightPayloadHash;
		}
	}
	else
	{
		if (runtimeLightUsesFrameSlot)
		{
			boundRuntimeLightCount = sceneDataFrameSlot->lightReuse.runtimeLight.lightCount;
		}
		renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightCacheHits++;
	}

	uint32_t runtimeLightTileCountX = 0;
	uint32_t runtimeLightTileCountY = 0;
	uint32_t runtimeLightTileIndexCount = 0;
	uint32_t runtimeLightMaxTileOccupancy = 0;
	uint64_t runtimeLightClusterCameraHash = 0;
	{
		ScopedPtPerfTimer runtimeLightClusterTimer(renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightClusterMs);
		runtimeLightClusterCameraHash = renderer.mSceneLights.BuildRuntimeLightClusterCameraHash(renderer.BuildRuntimeLightClusterInput());
	}
	const uint64_t runtimeLightClusterPayloadHash =
		nri_scene::HashCombine64(runtimeLightPayloadHash, runtimeLightClusterCameraHash);
	NRIBufferResource& runtimeLightTileHeaderBuffer =
		runtimeLightUsesFrameSlot ? sceneDataFrameSlot->runtimeLightTileHeaderBuffer : renderer.mRuntimeLightTileHeaderBuffer;
	SceneBufferDebugStats& runtimeLightTileHeaderStats =
		runtimeLightUsesFrameSlot ? sceneDataFrameSlot->runtimeLightTileHeaderStats : renderer.mRuntimeLightTileHeaderBufferStats;
	NRIBufferResource& runtimeLightTileIndexBuffer =
		runtimeLightUsesFrameSlot ? sceneDataFrameSlot->runtimeLightTileIndexBuffer : renderer.mRuntimeLightTileIndexBuffer;
	SceneBufferDebugStats& runtimeLightTileIndexStats =
		runtimeLightUsesFrameSlot ? sceneDataFrameSlot->runtimeLightTileIndexStats : renderer.mRuntimeLightTileIndexBufferStats;
	const bool runtimeLightClusterFrameSlotCacheHit =
		runtimeLightUsesFrameSlot &&
		sceneDataFrameSlot->lightReuse.runtimeLightCluster.CanReuse(
			runtimeLightClusterPayloadHash,
			BuildSceneDataLightBufferReuseView(runtimeLightTileHeaderBuffer),
			sizeof(NRIRuntimeLightTileHeaderGpuData),
			BuildSceneDataLightBufferReuseView(runtimeLightTileIndexBuffer),
			sizeof(uint32_t));
	const bool runtimeLightClusterNeedsUpload =
		runtimeLightUsesFrameSlot ?
		!runtimeLightClusterFrameSlotCacheHit :
		(!renderer.mRuntimeLightClusterCacheValid ||
			renderer.mRuntimeLightClusterPayloadHash != runtimeLightClusterPayloadHash ||
			runtimeLightTileHeaderBuffer.shaderView == nullptr ||
			runtimeLightTileIndexBuffer.shaderView == nullptr);
	if (runtimeLightClusterNeedsUpload)
	{
		if (runtimeLightUsesFrameSlot)
		{
			sceneDataFrameSlot->lightReuse.runtimeLightCluster.Invalidate();
		}
		else
		{
			renderer.mRuntimeLightClusterCacheValid = false;
		}
		renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightClusterUploads++;
		std::vector<NRIRuntimeLightTileHeaderGpuData> runtimeLightTileHeaders;
		std::vector<uint32_t> runtimeLightTileIndices;
		{
			ScopedPtPerfTimer runtimeLightClusterTimer(renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightClusterMs);
			renderer.mSceneLights.BuildRuntimeLightClusterUpload(
				renderer.BuildRuntimeLightClusterInput(),
				runtimeLightTileHeaders,
				runtimeLightTileIndices,
				runtimeLightTileCountX,
				runtimeLightTileCountY,
				runtimeLightTileIndexCount,
				runtimeLightMaxTileOccupancy);
		}
		if (!ensureSceneDataBatched(
			runtimeLightTileHeaderBuffer,
			runtimeLightTileHeaderStats,
			"runtime_light_tile_header",
			runtimeLightTileHeaders.data(),
			runtimeLightTileHeaders.size() * sizeof(NRIRuntimeLightTileHeaderGpuData),
			sizeof(NRIRuntimeLightTileHeaderGpuData),
			nri::BufferUsageBits::SHADER_RESOURCE,
			NRIResourceComputeShaderResourceAccess(),
			renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightClusterMs,
			renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightClusterRequestedBytes,
			renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightClusterUploadedBytes,
			runtimeLightUsesFrameSlot))
		{
			return false;
		}

		if (!ensureSceneDataBatched(
			runtimeLightTileIndexBuffer,
			runtimeLightTileIndexStats,
			"runtime_light_tile_index",
			runtimeLightTileIndices.data(),
			runtimeLightTileIndices.size() * sizeof(uint32_t),
			sizeof(uint32_t),
			nri::BufferUsageBits::SHADER_RESOURCE,
			NRIResourceComputeShaderResourceAccess(),
			renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightClusterMs,
			renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightClusterRequestedBytes,
			renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightClusterUploadedBytes,
			runtimeLightUsesFrameSlot))
		{
			return false;
		}

		if (runtimeLightUsesFrameSlot)
		{
			sceneDataFrameSlot->lightReuse.runtimeLightCluster.Commit(
				runtimeLightClusterPayloadHash,
				runtimeLightTileCountX,
				runtimeLightTileCountY,
				runtimeLightTileIndexCount,
				runtimeLightMaxTileOccupancy,
				BuildSceneDataLightBufferReuseView(runtimeLightTileHeaderBuffer),
				BuildSceneDataLightBufferReuseView(runtimeLightTileIndexBuffer));
		}
		else
		{
			renderer.mRuntimeLightClusterCacheValid = true;
			renderer.mRuntimeLightClusterPayloadHash = runtimeLightClusterPayloadHash;
			renderer.mRuntimeLightClusterCameraHash = runtimeLightClusterCameraHash;
		}
	}
	else
	{
		if (runtimeLightUsesFrameSlot)
		{
			runtimeLightTileCountX = sceneDataFrameSlot->lightReuse.runtimeLightCluster.tileCountX;
			runtimeLightTileCountY = sceneDataFrameSlot->lightReuse.runtimeLightCluster.tileCountY;
			runtimeLightTileIndexCount = sceneDataFrameSlot->lightReuse.runtimeLightCluster.tileIndexCount;
			runtimeLightMaxTileOccupancy = sceneDataFrameSlot->lightReuse.runtimeLightCluster.maxTileOccupancy;
		}
		else
		{
			runtimeLightTileCountX = renderer.mBoundRuntimeLightTileCountX;
			runtimeLightTileCountY = renderer.mBoundRuntimeLightTileCountY;
			runtimeLightTileIndexCount = renderer.mBoundRuntimeLightTileIndexCount;
			runtimeLightMaxTileOccupancy = renderer.mBoundRuntimeLightMaxTileOccupancy;
		}
		renderer.mLastPerfShellTraceStats.sceneDataSetRuntimeLightClusterCacheHits++;
	}

	uint64_t sectorLightingPayloadHash = 0;
	{
		ScopedPtPerfTimer sectorLightTimer(renderer.mLastPerfShellTraceStats.sceneDataSetSectorLightMs);
		renderer.UpdateBoundSectorLightingState();
		sectorLightingPayloadHash = renderer.mSceneLights.BuildSectorLightingPayloadHash(NRIGetSectorLightMultiplier(), nri_ptsectorlighting);
	}
	NRIBufferResource& sectorLightHeaderBuffer =
		runtimeLightUsesFrameSlot ? sceneDataFrameSlot->sectorLightHeaderBuffer : renderer.mSectorLightHeaderBuffer;
	SceneBufferDebugStats& sectorLightHeaderStats =
		runtimeLightUsesFrameSlot ? sceneDataFrameSlot->sectorLightHeaderStats : renderer.mSectorLightHeaderBufferStats;
	NRIBufferResource& sectorLightBuffer =
		runtimeLightUsesFrameSlot ? sceneDataFrameSlot->sectorLightBuffer : renderer.mSectorLightBuffer;
	SceneBufferDebugStats& sectorLightStats =
		runtimeLightUsesFrameSlot ? sceneDataFrameSlot->sectorLightStats : renderer.mSectorLightBufferStats;
	const bool sectorLightFrameSlotCacheHit =
		runtimeLightUsesFrameSlot &&
		sceneDataFrameSlot->lightReuse.sectorLight.CanReuse(
			sectorLightingPayloadHash,
			renderer.mBoundSectorLightSectorCount,
			BuildSceneDataLightBufferReuseView(sectorLightHeaderBuffer),
			sizeof(NRISectorLightHeaderGpuData),
			BuildSceneDataLightBufferReuseView(sectorLightBuffer),
			sizeof(NRISectorLightGpuData));
	const bool sectorLightNeedsUpload =
		runtimeLightUsesFrameSlot ?
		!sectorLightFrameSlotCacheHit :
		(!renderer.mSectorLightingPayloadCacheValid ||
			renderer.mSectorLightingPayloadHash != sectorLightingPayloadHash ||
			sectorLightHeaderBuffer.shaderView == nullptr ||
			sectorLightBuffer.shaderView == nullptr);
	if (sectorLightNeedsUpload)
	{
		if (runtimeLightUsesFrameSlot)
		{
			sceneDataFrameSlot->lightReuse.sectorLight.Invalidate();
		}
		else
		{
			renderer.mSectorLightingPayloadCacheValid = false;
		}
		renderer.mLastPerfShellTraceStats.sceneDataSetSectorLightUploads++;
		NRISectorLightHeaderGpuData sectorLightHeader = {};
		std::vector<NRISectorLightGpuData> sectorLights;
		{
			ScopedPtPerfTimer sectorLightTimer(renderer.mLastPerfShellTraceStats.sceneDataSetSectorLightMs);
			renderer.UpdateBoundSectorLightingState();
			renderer.mSceneLights.BuildSectorLightingUpload(NRIGetSectorLightMultiplier(), nri_ptsectorlighting, sectorLightHeader, sectorLights);
		}
		if (!ensureSceneDataBatched(
			sectorLightHeaderBuffer,
			sectorLightHeaderStats,
			"sector_header",
			&sectorLightHeader,
			sizeof(sectorLightHeader),
			sizeof(NRISectorLightHeaderGpuData),
			nri::BufferUsageBits::SHADER_RESOURCE,
			NRIResourceComputeShaderResourceAccess(),
			renderer.mLastPerfShellTraceStats.sceneDataSetSectorLightMs,
			renderer.mLastPerfShellTraceStats.sceneDataSetSectorLightRequestedBytes,
			renderer.mLastPerfShellTraceStats.sceneDataSetSectorLightUploadedBytes,
			runtimeLightUsesFrameSlot))
		{
			return false;
		}

		if (!ensureSceneDataBatched(
			sectorLightBuffer,
			sectorLightStats,
			"sector_data",
			sectorLights.empty() ? nullptr : sectorLights.data(),
			sectorLights.empty() ? 0u : sectorLights.size() * sizeof(NRISectorLightGpuData),
			sizeof(NRISectorLightGpuData),
			nri::BufferUsageBits::SHADER_RESOURCE,
			NRIResourceComputeShaderResourceAccess(),
			renderer.mLastPerfShellTraceStats.sceneDataSetSectorLightMs,
			renderer.mLastPerfShellTraceStats.sceneDataSetSectorLightRequestedBytes,
			renderer.mLastPerfShellTraceStats.sceneDataSetSectorLightUploadedBytes,
			runtimeLightUsesFrameSlot))
		{
			return false;
		}

		if (runtimeLightUsesFrameSlot)
		{
			sceneDataFrameSlot->lightReuse.sectorLight.Commit(
				sectorLightingPayloadHash,
				(uint32_t)sectorLights.size(),
				BuildSceneDataLightBufferReuseView(sectorLightHeaderBuffer),
				BuildSceneDataLightBufferReuseView(sectorLightBuffer));
		}
		else
		{
			renderer.mSectorLightingPayloadCacheValid = true;
			renderer.mSectorLightingPayloadHash = sectorLightingPayloadHash;
		}
	}
	else
	{
		if (runtimeLightUsesFrameSlot)
		{
			renderer.mBoundSectorLightSectorCount = sceneDataFrameSlot->lightReuse.sectorLight.sectorCount;
		}
		renderer.mLastPerfShellTraceStats.sceneDataSetSectorLightCacheHits++;
	}

	bool descriptorsChanged = false;
	auto updateDescriptor = [&](uint32_t slot, nri::Descriptor* descriptor)
	{
		if (renderer.mSceneDataDescriptors[slot] != descriptor)
		{
			renderer.mSceneDataDescriptors[slot] = descriptor;
			descriptorsChanged = true;
		}
	};
	updateDescriptor(10, runtimeLightBuffer.shaderView);
	updateDescriptor(11, runtimeLightTileHeaderBuffer.shaderView);
	updateDescriptor(12, runtimeLightTileIndexBuffer.shaderView);
	updateDescriptor(16, sectorLightHeaderBuffer.shaderView);
	updateDescriptor(17, sectorLightBuffer.shaderView);
	if (!SceneDataDescriptorsReady(renderer))
	{
		return false;
	}
	if (descriptorsChanged &&
		!NRIDescriptorSetManager::CommitSceneDataDescriptors(renderer, sceneDataReason))
	{
		return false;
	}

	renderer.mBoundRuntimeLightCount = boundRuntimeLightCount;
	renderer.mBoundRuntimeLightTileCountX = runtimeLightTileCountX;
	renderer.mBoundRuntimeLightTileCountY = runtimeLightTileCountY;
	renderer.mBoundRuntimeLightTileSize = NRI_RUNTIME_LIGHT_TILE_SIZE;
	renderer.mBoundRuntimeLightTileIndexCount = runtimeLightTileIndexCount;
	renderer.mBoundRuntimeLightMaxTileOccupancy = runtimeLightMaxTileOccupancy;
	renderer.mRuntimeLightSceneDataDirty = false;
	renderer.mLastPerfShellTraceStats.sceneDataDescriptorGeneration = renderer.mSceneDataDescriptorGeneration;
	return true;
}

bool NRIRenderer::PreGrowLevelSceneResourcesForLoading()
{
	ScopedPtPerfTimer preGrowTimer(mLastPerfShellTraceStats.sceneDataPreGrowMs);
	mLastPerfShellTraceStats.sceneDataPreGrowCalls++;

	auto& instances = mSelectTopLevelInstanceScratch;
	auto& sceneInstances = mSelectSceneInstanceScratch;
	instances.clear();
	sceneInstances.clear();
	BuildStaticMapInstances(instances, sceneInstances);

	const NRIPersistentVoxelOverlayStats persistentVoxelStats = mPersistentVoxels.BuildOverlayStats();
	const uint32_t estimatedPersistentVoxelInstances = mPersistentVoxels.HasRenderableOverlay() ? persistentVoxelStats.actorCount : 0u;
	const uint32_t dynamicOverlayHeadroomInstances = 1u;
	const uint32_t estimatedInstanceCount =
		(uint32_t)instances.size() +
		estimatedPersistentVoxelInstances +
		dynamicOverlayHeadroomInstances;

	if (!EnsureTopLevelAccelerationStructureCapacity(estimatedInstanceCount))
	{
		return false;
	}

	auto ensureCapacity = [&](NRIBufferResource& resource, SceneBufferDebugStats& stats, uint64_t bytes, uint32_t stride) -> bool
	{
		if (!NRISceneUploadManager::EnsureStructuredBufferCapacity(
			*this,
			resource,
			stats,
			bytes,
			stride,
			nri::BufferUsageBits::SHADER_RESOURCE,
			"scene_data_upload"))
		{
			return false;
		}
		if (stats.growEventsLastFrame != 0)
		{
			mLastPerfShellTraceStats.sceneDataPreGrowResourceGrowEvents++;
			mLastPerfShellTraceStats.sceneDataPreGrowRequestedBytes += stats.growthRequestedBytesLastFrame;
			mLastPerfShellTraceStats.sceneDataPreGrowAllocatedBytes += stats.growthAllocatedBytesLastFrame;
		}
		return true;
	};

	const std::vector<ScenePortalData> scenePortals = BuildScenePortalData(mMapWorld);
	std::vector<NRIRuntimePointLightGpuData> runtimeLights;
	mSceneLights.BuildRuntimePointLightUpload(runtimeLights);

	uint32_t runtimeLightTileCountX = 0;
	uint32_t runtimeLightTileCountY = 0;
	uint32_t runtimeLightTileIndexCount = 0;
	uint32_t runtimeLightMaxTileOccupancy = 0;
	std::vector<NRIRuntimeLightTileHeaderGpuData> runtimeLightTileHeaders;
	std::vector<uint32_t> runtimeLightTileIndices;
	mSceneLights.BuildRuntimeLightClusterUpload(
		BuildRuntimeLightClusterInput(),
		runtimeLightTileHeaders,
		runtimeLightTileIndices,
		runtimeLightTileCountX,
		runtimeLightTileCountY,
		runtimeLightTileIndexCount,
		runtimeLightMaxTileOccupancy);

	NRIEmissivePrimitiveHeaderGpuData emissiveHeader = {};
	std::vector<NRIEmissivePrimitiveGpuData> emissivePrimitives;
	std::vector<float> emissiveCdf;
	std::vector<NRIEmissiveMaterialResponseGpuData> emissiveMaterialResponses;
	std::vector<NRIEmissivePrimitiveDebugRecord> ignoredEmissiveDebugRecords;
	mSceneLights.BuildEmissiveSamplingUpload({}, emissiveHeader, emissivePrimitives, emissiveCdf, emissiveMaterialResponses, ignoredEmissiveDebugRecords);

	UpdateBoundSectorLightingState();
	NRISectorLightHeaderGpuData sectorLightHeader = {};
	std::vector<NRISectorLightGpuData> sectorLights;
	mSceneLights.BuildSectorLightingUpload(NRIGetSectorLightMultiplier(), nri_ptsectorlighting, sectorLightHeader, sectorLights);

	const uint64_t estimatedVisibleChunkBytes =
		std::max<uint64_t>(((uint64_t)mMapWorld.chunks.size() + 31u) / 32u, 1u) * sizeof(uint32_t);
	const uint64_t estimatedVisibleFlatPlaneBytes =
		std::max<uint64_t>((((uint64_t)mMapWorld.chunks.size() * 2u) + 31u) / 32u, 1u) * sizeof(uint32_t);
	const uint64_t estimatedSpatialAbsenceBytes =
		(1u + (uint64_t)mMapWorld.chunks.size() + std::min<uint64_t>((uint64_t)mMapWorld.chunks.size() * 16u, 4096u)) *
		sizeof(NRISpatialAbsenceGpuRecord);
	const uint64_t estimatedSpatialAbsenceTypedBytes = estimatedSpatialAbsenceBytes;

	const bool ready =
		ensureCapacity(mSceneInstanceBuffer, mSceneInstanceBufferStats, (uint64_t)estimatedInstanceCount * sizeof(SceneInstanceData), sizeof(SceneInstanceData)) &&
		ensureCapacity(mPortalBuffer, mPortalBufferStats, scenePortals.size() * sizeof(ScenePortalData), sizeof(ScenePortalData)) &&
		ensureCapacity(mRuntimeLightBuffer, mRuntimeLightBufferStats, runtimeLights.size() * sizeof(NRIRuntimePointLightGpuData), sizeof(NRIRuntimePointLightGpuData)) &&
		ensureCapacity(mRuntimeLightTileHeaderBuffer, mRuntimeLightTileHeaderBufferStats, runtimeLightTileHeaders.size() * sizeof(NRIRuntimeLightTileHeaderGpuData), sizeof(NRIRuntimeLightTileHeaderGpuData)) &&
		ensureCapacity(mRuntimeLightTileIndexBuffer, mRuntimeLightTileIndexBufferStats, runtimeLightTileIndices.size() * sizeof(uint32_t), sizeof(uint32_t)) &&
		ensureCapacity(mEmissivePrimitiveHeaderBuffer, mEmissivePrimitiveHeaderBufferStats, sizeof(emissiveHeader), sizeof(NRIEmissivePrimitiveHeaderGpuData)) &&
		ensureCapacity(mEmissivePrimitiveBuffer, mEmissivePrimitiveBufferStats, emissivePrimitives.size() * sizeof(NRIEmissivePrimitiveShaderData), sizeof(NRIEmissivePrimitiveShaderData)) &&
		ensureCapacity(mEmissivePrimitiveCdfBuffer, mEmissivePrimitiveCdfBufferStats, emissiveCdf.size() * sizeof(float), sizeof(float)) &&
		ensureCapacity(mEmissiveMaterialResponseBuffer, mEmissiveMaterialResponseBufferStats, emissiveMaterialResponses.size() * sizeof(NRIEmissiveMaterialResponseGpuData), sizeof(NRIEmissiveMaterialResponseGpuData)) &&
		ensureCapacity(mSectorLightHeaderBuffer, mSectorLightHeaderBufferStats, sizeof(sectorLightHeader), sizeof(NRISectorLightHeaderGpuData)) &&
		ensureCapacity(mSectorLightBuffer, mSectorLightBufferStats, sectorLights.size() * sizeof(NRISectorLightGpuData), sizeof(NRISectorLightGpuData)) &&
		ensureCapacity(mReprojectionBuffer, mReprojectionBufferStats, sizeof(NRIReprojectionData), sizeof(NRIReprojectionData)) &&
		ensureCapacity(mVisibleChunkBuffer, mVisibleChunkBufferStats, estimatedVisibleChunkBytes, sizeof(uint32_t)) &&
		ensureCapacity(mVisibleFlatPlaneBuffer, mVisibleFlatPlaneBufferStats, estimatedVisibleFlatPlaneBytes, sizeof(uint32_t)) &&
		ensureCapacity(mSpatialAbsenceBuffer, mSpatialAbsenceBufferStats, estimatedSpatialAbsenceBytes, sizeof(NRISpatialAbsenceGpuRecord)) &&
		ensureCapacity(mSpatialAbsenceTypedBuffer, mSpatialAbsenceTypedBufferStats, estimatedSpatialAbsenceTypedBytes, sizeof(NRISpatialAbsenceGpuBlock));

	if ((int)nri_ptloadingtrace >= 1)
	{
		Printf("NRI PT loading gate: event=pre_grow result=%s instances=%u static_instances=%u persistent_instances=%u tlas_instance_grows=%u tlas_scratch_grows=%u tlas_deferred_as=%u tlas_deferred_instance=%u tlas_deferred_scratch=%u scene_resource_grows=%u tlas_instance_requested=%llu tlas_instance_allocated=%llu tlas_scratch_requested=%llu tlas_scratch_allocated=%llu scene_requested=%llu scene_allocated=%llu wait_ms=%.3f\n",
			ready ? "ready" : "failed",
			estimatedInstanceCount,
			(uint32_t)instances.size(),
			estimatedPersistentVoxelInstances,
			mLastPerfShellTraceStats.worldTlasPreGrowInstanceGrowCount,
			mLastPerfShellTraceStats.worldTlasPreGrowScratchGrowCount,
			mLastPerfShellTraceStats.worldTlasPreGrowDeferredAsSlots,
			mLastPerfShellTraceStats.worldTlasPreGrowDeferredInstanceSlots,
			mLastPerfShellTraceStats.worldTlasPreGrowDeferredScratchSlots,
			mLastPerfShellTraceStats.sceneDataPreGrowResourceGrowEvents,
			(unsigned long long)mLastPerfShellTraceStats.worldTlasPreGrowInstanceRequestedBytes,
			(unsigned long long)mLastPerfShellTraceStats.worldTlasPreGrowInstanceAllocatedBytes,
			(unsigned long long)mLastPerfShellTraceStats.worldTlasPreGrowScratchRequestedBytes,
			(unsigned long long)mLastPerfShellTraceStats.worldTlasPreGrowScratchAllocatedBytes,
			(unsigned long long)mLastPerfShellTraceStats.sceneDataPreGrowRequestedBytes,
			(unsigned long long)mLastPerfShellTraceStats.sceneDataPreGrowAllocatedBytes,
			mLastPerfShellTraceStats.worldTlasPreGrowWaitMs + mLastPerfShellTraceStats.sceneDataPreGrowWaitMs);
	}
	return ready;
}
