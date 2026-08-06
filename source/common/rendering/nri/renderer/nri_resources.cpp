#include "nri_resources.h"
#include "nri_cvars.h"

#include "nri_renderer.h"
#include "nri_persistent_voxel_services.h"
#include "nri_renderer_settings.h"
#include "nri_sky_environment.h"
#include "nri_static_scene_geometry.h"
#include "../system/nri_renderdevice.h"
#include "c_cvars.h"
#include "perf_capture.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>


namespace
{
	bool ShouldTraceResourcePerf()
	{
		return PerfLoopTraceActive() || ShouldEmitRendererTemporalTraceLogs() || PerfCompactCaptureTimingActive();
	}

	double DurationMs(const std::chrono::steady_clock::time_point& start, const std::chrono::steady_clock::time_point& end)
	{
		return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start).count();
	}
}

uint64_t GetNRIGrownBufferSize(uint64_t currentCapacity, uint64_t requiredSize, uint32_t stride)
{
	const uint64_t minimumCapacity = std::max<uint64_t>(requiredSize, stride);
	if (currentCapacity >= minimumCapacity && currentCapacity != 0)
	{
		return currentCapacity;
	}

	if (currentCapacity == 0)
	{
		return minimumCapacity;
	}

	uint64_t newCapacity = std::max<uint64_t>(currentCapacity, stride);
	while (newCapacity < minimumCapacity)
	{
		const uint64_t doubled = newCapacity <= std::numeric_limits<uint64_t>::max() / 2 ? newCapacity * 2 : std::numeric_limits<uint64_t>::max();
		if (doubled <= newCapacity)
		{
			return minimumCapacity;
		}
		newCapacity = doubled;
	}

	return newCapacity;
}

uint64_t GetNRISceneUploadGrownBufferSize(uint64_t currentCapacity, uint64_t requiredSize, uint32_t stride)
{
	const uint64_t minimumCapacity = std::max<uint64_t>(requiredSize, stride);
	if (currentCapacity >= minimumCapacity && currentCapacity != 0)
	{
		return currentCapacity;
	}
	if (currentCapacity == 0)
	{
		constexpr uint64_t kMinInitialHeadroom = 1ull * 1024ull * 1024ull;
		constexpr uint64_t kMaxInitialHeadroom = 16ull * 1024ull * 1024ull;
		const uint64_t proportionalHeadroom = minimumCapacity / 4u;
		const uint64_t headroom = std::min<uint64_t>(
			std::max<uint64_t>(proportionalHeadroom, kMinInitialHeadroom),
			kMaxInitialHeadroom);
		uint64_t initialCapacity =
			minimumCapacity <= std::numeric_limits<uint64_t>::max() - headroom ?
			minimumCapacity + headroom :
			minimumCapacity;
		if (stride > 1)
		{
			const uint64_t remainder = initialCapacity % stride;
			if (remainder != 0)
			{
				const uint64_t alignmentPadding = stride - remainder;
				initialCapacity =
					initialCapacity <= std::numeric_limits<uint64_t>::max() - alignmentPadding ?
					initialCapacity + alignmentPadding :
					minimumCapacity;
			}
		}
		return std::max<uint64_t>(initialCapacity, minimumCapacity);
	}

	uint64_t newCapacity = std::max<uint64_t>(currentCapacity, stride);
	while (newCapacity < minimumCapacity)
	{
		const uint64_t doubled =
			newCapacity <= std::numeric_limits<uint64_t>::max() / 2 ?
			newCapacity * 2 :
			std::numeric_limits<uint64_t>::max();
		if (doubled <= newCapacity)
		{
			return minimumCapacity;
		}
		newCapacity = doubled;
	}
	return newCapacity;
}

void NRIRenderer::WaitForCommandsTracked(const char* reason)
{
	if (mFrameBuffer == nullptr)
	{
		return;
	}

	const bool trace = ShouldTraceResourcePerf();
	const auto start = trace ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
	mFrameBuffer->WaitForCommands(true);
	if (trace)
	{
		const double waitMs = DurationMs(start, std::chrono::steady_clock::now());
		mLastPerfResourceTraceStats.waitCalls++;
		mLastPerfResourceTraceStats.waitMs += waitMs;
		if (reason != nullptr)
		{
			if (std::strcmp(reason, "resident_chunk_write") == 0)
			{
				mLastPerfResourceTraceStats.residentChunkWriteWaitCalls++;
				mLastPerfResourceTraceStats.residentChunkWriteWaitMs += waitMs;
			}
			else if (std::strcmp(reason, "resident_chunk_blas_rebuild") == 0)
			{
				mLastPerfResourceTraceStats.residentChunkBlasRebuildWaitCalls++;
				mLastPerfResourceTraceStats.residentChunkBlasRebuildWaitMs += waitMs;
			}
			else if (std::strcmp(reason, "scene_data_upload") == 0)
			{
				mLastPerfResourceTraceStats.sceneDataUploadWaitCalls++;
				mLastPerfResourceTraceStats.sceneDataUploadWaitMs += waitMs;
			}
			else if (std::strcmp(reason, "scene_buffer_upload") == 0)
			{
				mLastPerfResourceTraceStats.sceneBufferUploadWaitCalls++;
				mLastPerfResourceTraceStats.sceneBufferUploadWaitMs += waitMs;
			}
			else if (std::strcmp(reason, "emissive_sampling_upload") == 0)
			{
				mLastPerfResourceTraceStats.emissiveSamplingUploadWaitCalls++;
				mLastPerfResourceTraceStats.emissiveSamplingUploadWaitMs += waitMs;
			}
			else if (std::strcmp(reason, "world_tlas_instance_upload") == 0)
			{
				mLastPerfResourceTraceStats.worldTlasInstanceUploadWaitCalls++;
				mLastPerfResourceTraceStats.worldTlasInstanceUploadWaitMs += waitMs;
			}
			else if (std::strcmp(reason, "world_tlas_scratch_resize") == 0)
			{
				mLastPerfResourceTraceStats.worldTlasScratchResizeWaitCalls++;
				mLastPerfResourceTraceStats.worldTlasScratchResizeWaitMs += waitMs;
			}
			else if (std::strcmp(reason, "emissive_tlas_instance_upload") == 0)
			{
				mLastPerfResourceTraceStats.emissiveTlasInstanceUploadWaitCalls++;
				mLastPerfResourceTraceStats.emissiveTlasInstanceUploadWaitMs += waitMs;
			}
			else if (std::strcmp(reason, "emissive_tlas_scratch_resize") == 0)
			{
				mLastPerfResourceTraceStats.emissiveTlasScratchResizeWaitCalls++;
				mLastPerfResourceTraceStats.emissiveTlasScratchResizeWaitMs += waitMs;
			}
			else
			{
				mLastPerfResourceTraceStats.otherWaitCalls++;
				mLastPerfResourceTraceStats.otherWaitMs += waitMs;
			}
		}
		else
		{
			mLastPerfResourceTraceStats.otherWaitCalls++;
			mLastPerfResourceTraceStats.otherWaitMs += waitMs;
		}
	}
}

void NRIRenderer::DestroyCachedTextures()
{
	mStaticMapScene.texturesResident = false;
	for (auto& skyTexture : mSkyEnvironment.CachedTextures())
	{
		mFrameBuffer->DestroyTextureResource(skyTexture.resource);
	}
	mSkyEnvironment.ClearCache();
	for (auto& texture : mSceneTextures.CachedTextures())
	{
		mFrameBuffer->DestroyTextureResource(texture.resource);
	}
	mSceneTextures.ClearCachedTextures(mFrameBuffer);
	mSceneTextures.SlotTable().Reset();
	mSceneMaterialFrameCache.Reset();
	mSceneTextureFrameCache.Reset();
	std::fill(mSceneTextureSetHashValid.begin(), mSceneTextureSetHashValid.end(), 0);
}

void NRIRenderer::DestroySceneBuffers()
{
	mSceneUploadProducerGenerations.Reset();
	mSceneUploadIdentityValidator.Reset();
	mPrimitiveVisibilityIdentityCache = {};
	ResetPersistentVoxelBlasCompaction();
	mStaticMapScene.buffersResident = false;
	nri_static_scene_geometry::ResetStaticMapChunkAtlas(mStaticMapChunkAtlas);
	ResetResidentMapChunkRegistry();
	ResetPersistentDynamicEmissiveCache();
	mPersistentVoxels.Reset("destroy-scene-buffers", true, (int)nri_ptloadingtrace >= 1 || (bool)nri_voxelstats, BuildNRIPersistentVoxelResetServices(*this));
	mVoxelRepresentationPolicy.Reset();
	ResetDynamicOverlayBlasCache();
	DestroyBufferResource(mStaticVertexBuffer);
	DestroyBufferResource(mStaticIndexBuffer);
	DestroyBufferResource(mStaticPrimitiveBuffer);
	DestroyBufferResource(mStaticMaterialBuffer);
	NRIPersistentVoxelDestroyServices persistentVoxelDestroyServices = {};
	persistentVoxelDestroyServices.user = this;
	persistentVoxelDestroyServices.destroyBuffer = [](void* user, NRIBufferResource& resource)
	{
		static_cast<NRIRenderer*>(user)->DestroyBufferResource(resource);
	};
	mPersistentVoxels.DestroyArenaBuffers(persistentVoxelDestroyServices);
	DestroyBufferResource(mVertexBuffer);
	DestroyBufferResource(mIndexBuffer);
	DestroyBufferResource(mPrimitiveBuffer);
	DestroyBufferResource(mMaterialBuffer);
	for (SceneUploadBufferRingSlot& slot : mSceneUploadBufferRing)
	{
		DestroyAccelerationStructureResource(slot.dynamicBottomLevelAS);
		DestroyBufferResource(slot.vertexBuffer);
		DestroyBufferResource(slot.indexBuffer);
		DestroyBufferResource(slot.primitiveBuffer);
		DestroyBufferResource(slot.materialBuffer);
	}
	mSceneUploadBufferRing.clear();
	for (SceneDataFrameSlot& slot : mSceneDataFrameRing)
	{
		DestroyBufferResource(slot.reprojectionBuffer);
		DestroyBufferResource(slot.visibleChunkBuffer);
		DestroyBufferResource(slot.visibleFlatPlaneBuffer);
		DestroyBufferResource(slot.spatialAbsenceBuffer);
		DestroyBufferResource(slot.sceneInstanceBuffer);
		DestroyBufferResource(slot.portalBuffer);
		DestroyBufferResource(slot.runtimeLightBuffer);
		DestroyBufferResource(slot.runtimeLightTileHeaderBuffer);
		DestroyBufferResource(slot.runtimeLightTileIndexBuffer);
		DestroyBufferResource(slot.emissivePrimitiveHeaderBuffer);
		DestroyBufferResource(slot.emissivePrimitiveBuffer);
		DestroyBufferResource(slot.emissivePrimitiveCdfBuffer);
		DestroyBufferResource(slot.emissiveMaterialResponseBuffer);
		DestroyBufferResource(slot.sectorLightHeaderBuffer);
		DestroyBufferResource(slot.sectorLightBuffer);
	}
	mSceneDataFrameRing.clear();
	for (SceneDataDescriptorSnapshot& snapshot : mSceneDataSnapshots)
	{
		DestroyBufferResource(snapshot.sceneInstanceBuffer);
		snapshot = {};
	}
	mSceneDataSnapshots.clear();
	mActiveSceneDataSet = nullptr;
	mActiveSceneDataSnapshot = nullptr;
	mActiveSceneDataSetFrameIndex = UINT64_MAX;
	mSceneDataSnapshotCursor = 0;
	mSceneDataFrameRingHighWaterBytes = 0;
	mSceneDataFrameRingFallbackCount = 0;
	mSceneDataFrameRingOverCapCount = 0;
	mSceneDataFrameRingSlotWaitCount = 0;
	mSceneDataFrameRingDisabledFrameIndex = UINT32_MAX;
	mSceneDataFrameRingOverCapFrameIndex = UINT32_MAX;
	DestroyBufferResource(mEmissiveTlasInstanceBuffer);
	DestroyBufferResource(mSceneInstanceBuffer);
	DestroyBufferResource(mPortalBuffer);
	DestroyBufferResource(mRuntimeLightBuffer);
	DestroyBufferResource(mRuntimeLightTileHeaderBuffer);
	DestroyBufferResource(mRuntimeLightTileIndexBuffer);
	DestroyBufferResource(mEmissivePrimitiveHeaderBuffer);
	DestroyBufferResource(mEmissivePrimitiveBuffer);
	DestroyBufferResource(mEmissivePrimitiveCdfBuffer);
	DestroyBufferResource(mEmissiveMaterialResponseBuffer);
	DestroyBufferResource(mSectorLightHeaderBuffer);
	DestroyBufferResource(mSectorLightBuffer);
	DestroyBufferResource(mReprojectionBuffer);
	DestroyBufferResource(mVisibleChunkBuffer);
	DestroyBufferResource(mVisibleFlatPlaneBuffer);
	DestroyBufferResource(mSpatialAbsenceBuffer);
	mTraceShaderStats.Destroy(BuildResourceServices());
	DestroyBufferResource(mScratchBuffer);
	DestroyBufferResource(mResidentStaticBlasScratchBuffer);
	DestroyBufferResource(mEmissiveTopLevelScratchBuffer);
	for (auto& frameScratch : mResidentUploadScratchFrames)
	{
		DestroyBufferResource(frameScratch.vertex.buffer);
		DestroyBufferResource(frameScratch.index.buffer);
		DestroyBufferResource(frameScratch.primitive.buffer);
		DestroyBufferResource(frameScratch.material.buffer);
		for (NRIBufferResource& retired : frameScratch.retiredBuffers)
		{
			DestroyBufferResource(retired);
		}
		for (NRIAccelerationStructureResource& retired : frameScratch.retiredAccelerationStructures)
		{
			DestroyAccelerationStructureResource(retired);
		}
		frameScratch = {};
	}
	DestroyAccelerationStructureResource(mEmissiveTopLevelAS);
	for (uint8_t& initialized : mSceneDataDescriptorsInitialized)
	{
		initialized = 0u;
	}
	std::fill(mSceneDataDescriptorMapEpochs.begin(), mSceneDataDescriptorMapEpochs.end(), 0ull);
	std::fill(mSceneDataDescriptorBuildEpochs.begin(), mSceneDataDescriptorBuildEpochs.end(), 0ull);
	mSceneDataDescriptors.fill(nullptr);
	mCurrentSceneTextureDescriptors.clear();
	mBoundStaticPrimitiveCount = 0;
	mBoundDynamicPrimitiveCount = 0;
	mBoundStaticMaterialCount = 0;
	mBoundDynamicMaterialCount = 0;
	mBoundPortalCount = 0;
	mBoundRuntimeLightCount = 0;
	mBoundRuntimeLightTileCountX = 0;
	mBoundRuntimeLightTileCountY = 0;
	mBoundRuntimeLightTileSize = 0;
	mBoundRuntimeLightTileIndexCount = 0;
	mBoundRuntimeLightMaxTileOccupancy = 0;
	mRuntimeLightPayloadCacheValid = false;
	mRuntimeLightPayloadHash = 0;
	mRuntimeLightClusterCacheValid = false;
	mRuntimeLightClusterPayloadHash = 0;
	mRuntimeLightClusterCameraHash = 0;
	mRuntimeLightSceneDataDirty = false;
	mPortalPayloadCacheValid = false;
	mPortalPayloadHash = 0;
	mPortalPayloadBuildSerial = 0;
	mPortalPayloadCount = 0;
	mSceneDataSnapshotGenerationCounter = 0;
	mSceneDataSnapshotGeneration = 0;
	mSceneDataSnapshotFrameIndex = UINT64_MAX;
	mSceneDataSnapshotSceneInstanceHash = 0;
	mSceneDataSnapshotTlasInstanceHash = 0;
	mSceneDataSnapshotPortalHash = 0;
	mSceneDataSnapshotQueuedFrameIndex = UINT32_MAX;
	mSceneDataSnapshotSceneInstanceCount = 0;
	mSceneDataSnapshotTlasInstanceCount = 0;
	mSceneDataSnapshotPortalCount = 0;
	mSceneDataDescriptorGeneration = 0;
	mLastWorldTlasInstancePayloadHash = 0;
	mLastWorldTlasInstanceFrameIndex = UINT64_MAX;
	mLastWorldTlasInstanceCount = 0;
	mBoundEmissivePrimitiveCount = 0;
	mBoundEmissiveDominantPrimitive = UINT32_MAX;
	mBoundEmissiveDominantTile = 0;
	mBoundEmissiveDominantFlags = 0;
	mBoundEmissiveDominantDataSource = 0;
	mEmissiveSamplingPayloadCacheValid = false;
	mEmissiveSamplingPayloadHash = 0;
	mEmissiveSectorResponsePayloadCacheValid = false;
	mEmissiveSectorResponsePayloadHash = 0;
	mSceneLights.ResetEmissiveSectorResponseCaches();
	mEmissiveTlasInstanceCount = 0;
	mEmissiveTlasStaticInstanceCount = 0;
	mEmissiveTlasDynamicInstanceCount = 0;
	mEmissiveTlasBuildCount = 0;
	mEmissiveTlasInstancePayloadCacheValid = false;
	mEmissiveTlasInstancePayloadHash = 0;
	mBoundEmissiveTotalPower = 0.0f;
	mBoundEmissiveDominantPower = 0.0f;
	mBoundEmissivePrimitiveRecords.clear();
	mBoundSceneInstances.clear();
	mBoundSectorLightSectorCount = 0;
	mBoundSectorLightActiveCount = 0;
	mBoundSectorLightPulsingCount = 0;
	mBoundSectorLightDominantSector = UINT32_MAX;
	mBoundSectorLightDominantContribution = 0.0f;
	mSectorLightingPayloadCacheValid = false;
	mSectorLightingPayloadHash = 0;
}

void NRIRenderer::DestroyBufferResource(NRIBufferResource& resource)
{
	if (resource.shaderView != nullptr)
	{
		mFrameBuffer->mCore.DestroyDescriptor(resource.shaderView);
		resource.shaderView = nullptr;
	}
	if (resource.storageView != nullptr)
	{
		mFrameBuffer->mCore.DestroyDescriptor(resource.storageView);
		resource.storageView = nullptr;
	}

	if (resource.buffer != nullptr)
	{
		mFrameBuffer->mCore.DestroyBuffer(resource.buffer);
		resource.buffer = nullptr;
	}

	resource.size = 0;
	resource.memorySize = 0;
	resource.usedSize = 0;
	resource.payloadHash = 0;
	resource.payloadSize = 0;
	resource.stride = 0;
	resource.payloadStride = 0;
	resource.usage = nri::BufferUsageBits::NONE;
	resource.memoryLocation = nri::MemoryLocation::DEVICE;
}

void NRIRenderer::DestroyAccelerationStructureResource(NRIAccelerationStructureResource& resource)
{
	if (resource.descriptor != nullptr)
	{
		mFrameBuffer->mCore.DestroyDescriptor(resource.descriptor);
		resource.descriptor = nullptr;
	}

	if (resource.accelerationStructure != nullptr)
	{
		mFrameBuffer->mRayTracing.DestroyAccelerationStructure(resource.accelerationStructure);
		resource.accelerationStructure = nullptr;
	}

	resource.memorySize = 0;
	resource.buildScratchSize = 0;
	resource.updateScratchSize = 0;
	resource.buildVertexBuffer = nullptr;
	resource.buildIndexBuffer = nullptr;
	resource.buildVertexOffset = 0;
	resource.buildVertexCount = 0;
	resource.buildIndexOffset = 0;
	resource.buildIndexCount = 0;
	resource.buildPrimitiveCount = 0;
	resource.buildFlags = nri::AccelerationStructureBits::NONE;
	resource.buildType = nri::AccelerationStructureType::TOP_LEVEL;
	resource.buildTypeValid = false;
	resource.uncompactedMemorySize = 0;
	resource.compacted = false;
	resource.memoryLocation = nri::MemoryLocation::DEVICE;
}
