#include "nri_acceleration.h"
#include "nri_cvars.h"

#include "nri_renderer.h"
#include "nri_diagnostic_names.h"
#include "nri_persistent_voxel_services.h"
#include "nri_scene_upload.h"
#include "nri_shader_contracts.h"
#include "nri_upload_hash.h"
#include "nri_world_tlas_policy.h"
#include "../scene/nri_hash.h"
#include "../system/nri_gpu_timing.h"
#include "../system/nri_renderdevice.h"
#include "../../hwrenderer/data/hw_clock.h"
#include "c_cvars.h"
#include "perf_capture.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <unordered_map>


namespace
{
	double DurationMs(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end)
	{
		return std::chrono::duration<double, std::milli>(end - start).count();
	}

	bool ShouldCollectAccelerationPerfTiming()
	{
		return ((int)perf_looptraceframes > 0 || (!!nri_pttemporaltrace && (int)nri_pttraceframes > 0)) || (bool)nri_ptslowdowntrace || PerfCompactCaptureTimingActive();
	}

	class ScopedPtPerfTimer
	{
	public:
		explicit ScopedPtPerfTimer(double& targetMs)
			: mTarget(ShouldCollectAccelerationPerfTiming() ? &targetMs : nullptr)
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

	uint32_t FloatBits(float value)
	{
		uint32_t bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		return bits;
	}

	uint64_t BuildTlasInstancePayloadHash(const std::vector<nri::TopLevelInstance>& instances)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = nri_scene::HashCombine64(hash, (uint64_t)instances.size());
		for (const nri::TopLevelInstance& instance : instances)
		{
			hash = nri_scene::HashCombine64(hash, (uint64_t)instance.instanceId);
			hash = nri_scene::HashCombine64(hash, (uint64_t)instance.mask);
			hash = nri_scene::HashCombine64(hash, (uint64_t)instance.shaderBindingTableLocalOffset);
			hash = nri_scene::HashCombine64(hash, (uint64_t)instance.flags);
			hash = nri_scene::HashCombine64(hash, instance.accelerationStructureHandle);
			for (uint32_t row = 0; row < 3; ++row)
			{
				hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(instance.transform[row][0]));
				hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(instance.transform[row][1]));
				hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(instance.transform[row][2]));
				hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(instance.transform[row][3]));
			}
		}

		return hash;
	}

	uint64_t BuildSceneInstancePayloadHash(const std::vector<SceneInstanceData>& sceneInstances)
	{
		const uint64_t size = (uint64_t)sceneInstances.size() * sizeof(SceneInstanceData);
		uint64_t hash = 1469598103934665603ull;
		hash = nri_scene::HashCombine64(hash, (uint64_t)sceneInstances.size());
		hash = nri_scene::HashCombine64(hash, NRIHashUploadPayloadBytes(sceneInstances.empty() ? nullptr : sceneInstances.data(), size));
		return hash;
	}
}

bool NRIAccelerationStructureManager::BuildDynamic(NRIRenderer& renderer, const nri_scene::GeometryData& geometry)
{
	return BuildDynamic(
		renderer,
		geometry,
		0u,
		(uint32_t)geometry.indices.size(),
		(uint32_t)geometry.primitives.size(),
		renderer.GetCurrentDynamicBottomLevelAS(),
		true);
}

bool NRIAccelerationStructureManager::BuildDynamic(
	NRIRenderer& renderer,
	const nri_scene::GeometryData& geometry,
	uint32_t indexOffset,
	uint32_t indexCount,
	uint32_t primitiveCount,
	NRIAccelerationStructureResource& outAccelerationStructure,
	bool updateDynamicPerfStats)
{
	if (indexOffset > geometry.indices.size() || indexOffset + indexCount > geometry.indices.size())
	{
		return false;
	}
	return BuildBottomLevel(
		renderer,
		renderer.GetCurrentDynamicVertexBuffer(),
		renderer.GetCurrentDynamicIndexBuffer(),
		0u,
		(uint32_t)geometry.vertices.size(),
		indexOffset,
		indexCount,
		primitiveCount,
		outAccelerationStructure,
		updateDynamicPerfStats);
}

bool NRIAccelerationStructureManager::BuildBottomLevel(
	NRIRenderer& renderer,
	const NRIBufferResource& vertexBuffer,
	const NRIBufferResource& indexBuffer,
	uint32_t vertexOffset,
	uint32_t vertexCount,
	uint32_t indexOffset,
	uint32_t indexCount,
	uint32_t primitiveCount,
	NRIAccelerationStructureResource& outAccelerationStructure,
	bool updateDynamicPerfStats,
	NRIBufferResource* buildScratchBuffer,
	nri::AccelerationStructureBits buildFlags)
{
	Clocker clock(NriPTAcceleration);
	ScopedPtPerfTimer perfTimer(renderer.mLastPerfShellTraceStats.dynamicAsMs);
	if (updateDynamicPerfStats)
	{
		renderer.mLastPerfShellTraceStats.dynamicAsPrimitiveCount = primitiveCount;
		renderer.mLastPerfShellTraceStats.dynamicAsVertexCount = vertexCount;
		renderer.mLastPerfShellTraceStats.dynamicAsIndexCount = indexCount;
	}
	if (primitiveCount == 0 || vertexCount == 0 || indexCount == 0 || vertexBuffer.buffer == nullptr || indexBuffer.buffer == nullptr)
	{
		return false;
	}

	nri::BottomLevelGeometryDesc dynamicGeometryDesc = {};
	bool reuseAccelerationStructure = false;
	{
		ScopedPtPerfTimer phaseTimer(renderer.mLastPerfShellTraceStats.dynamicAsSetupMs);
		dynamicGeometryDesc.flags = nri::BottomLevelGeometryBits::OPAQUE_GEOMETRY;
		dynamicGeometryDesc.type = nri::BottomLevelGeometryType::TRIANGLES;
		dynamicGeometryDesc.triangles.vertexBuffer = vertexBuffer.buffer;
		dynamicGeometryDesc.triangles.vertexOffset = (uint64_t)vertexOffset * sizeof(nri_scene::SceneVertex);
		dynamicGeometryDesc.triangles.vertexNum = vertexCount;
		dynamicGeometryDesc.triangles.vertexStride = sizeof(nri_scene::SceneVertex);
		dynamicGeometryDesc.triangles.vertexFormat = nri::Format::RGB32_SFLOAT;
		dynamicGeometryDesc.triangles.indexBuffer = indexBuffer.buffer;
		dynamicGeometryDesc.triangles.indexOffset = (uint64_t)indexOffset * sizeof(uint32_t);
		dynamicGeometryDesc.triangles.indexNum = indexCount;
		dynamicGeometryDesc.triangles.indexType = nri::IndexType::UINT32;

		reuseAccelerationStructure =
			updateDynamicPerfStats &&
			outAccelerationStructure.accelerationStructure != nullptr &&
			outAccelerationStructure.buildVertexBuffer == vertexBuffer.buffer &&
			outAccelerationStructure.buildIndexBuffer == indexBuffer.buffer &&
			outAccelerationStructure.buildVertexOffset == vertexOffset &&
			outAccelerationStructure.buildVertexCount == vertexCount &&
			outAccelerationStructure.buildIndexOffset == indexOffset &&
			outAccelerationStructure.buildIndexCount == indexCount &&
			outAccelerationStructure.buildPrimitiveCount == primitiveCount;
	}

	if (reuseAccelerationStructure)
	{
		if (updateDynamicPerfStats)
		{
			renderer.mLastPerfShellTraceStats.dynamicAsReuseCount++;
		}
	}
	else
	{
		renderer.RetireResidentAccelerationStructure(outAccelerationStructure);
	}

	nri::AccelerationStructureDesc blasDesc = {};
	blasDesc.type = nri::AccelerationStructureType::BOTTOM_LEVEL;
	blasDesc.flags = buildFlags;
	blasDesc.geometryOrInstanceNum = 1;
	blasDesc.geometries = &dynamicGeometryDesc;
	const bool createdAs = reuseAccelerationStructure || [&]()
	{
		ScopedPtPerfTimer phaseTimer(renderer.mLastPerfShellTraceStats.dynamicAsCreateMs);
		if (updateDynamicPerfStats)
		{
			renderer.mLastPerfShellTraceStats.dynamicAsCreateCalls++;
		}
		return renderer.mFrameBuffer->mRayTracing.CreateCommittedAccelerationStructure(*renderer.mFrameBuffer->mDevice, nri::MemoryLocation::DEVICE, 0.0f, blasDesc, outAccelerationStructure.accelerationStructure) == nri::Result::SUCCESS;
	}();
	if (!createdAs)
	{
		return false;
	}

	if (!reuseAccelerationStructure || outAccelerationStructure.memorySize == 0)
	{
		nri::MemoryDesc memoryDesc = {};
		renderer.mFrameBuffer->mRayTracing.GetAccelerationStructureMemoryDesc(*outAccelerationStructure.accelerationStructure, nri::MemoryLocation::DEVICE, memoryDesc);
		outAccelerationStructure.memorySize = memoryDesc.size;
		outAccelerationStructure.memoryLocation = nri::MemoryLocation::DEVICE;
	}
	if (updateDynamicPerfStats)
	{
		renderer.mLastPerfShellTraceStats.dynamicAsMemoryBytes = outAccelerationStructure.memorySize;
	}

	uint64_t requiredScratchSize = updateDynamicPerfStats ? outAccelerationStructure.buildScratchSize : 0;
	if (requiredScratchSize == 0)
	{
		ScopedPtPerfTimer phaseTimer(renderer.mLastPerfShellTraceStats.dynamicAsScratchMs);
		if (updateDynamicPerfStats)
		{
			renderer.mLastPerfShellTraceStats.dynamicAsScratchQueries++;
		}
		requiredScratchSize = renderer.mFrameBuffer->mRayTracing.GetAccelerationStructureBuildScratchBufferSize(*outAccelerationStructure.accelerationStructure);
		if (updateDynamicPerfStats)
		{
			outAccelerationStructure.buildScratchSize = requiredScratchSize;
		}
	}
	if (updateDynamicPerfStats)
	{
		renderer.mLastPerfShellTraceStats.dynamicAsScratchRequestedBytes = requiredScratchSize;
	}
	NRIBufferResource& scratchBuffer = buildScratchBuffer != nullptr ? *buildScratchBuffer : renderer.mScratchBuffer;
	if (scratchBuffer.buffer == nullptr || scratchBuffer.size < requiredScratchSize)
	{
		renderer.RetireResidentBufferResource(scratchBuffer);
		{
			ScopedPtPerfTimer phaseTimer(renderer.mLastPerfShellTraceStats.dynamicAsScratchMs);
			if (updateDynamicPerfStats)
			{
				renderer.mLastPerfShellTraceStats.dynamicAsScratchGrowCount++;
			}
			if (!renderer.CreateBufferWithoutView(scratchBuffer, requiredScratchSize, 16, nri::BufferUsageBits::SCRATCH_BUFFER))
			{
				return false;
			}
		}
	}

	nri::BuildBottomLevelAccelerationStructureDesc dynamicBuild = {};
	dynamicBuild.dst = outAccelerationStructure.accelerationStructure;
	dynamicBuild.geometries = &dynamicGeometryDesc;
	dynamicBuild.geometryNum = 1;
	dynamicBuild.scratchBuffer = scratchBuffer.buffer;
	dynamicBuild.scratchOffset = 0;
	{
		ScopedPtPerfTimer phaseTimer(renderer.mLastPerfShellTraceStats.dynamicAsBuildMs);
		renderer.mFrameBuffer->mRayTracing.CmdBuildBottomLevelAccelerationStructures(*renderer.mFrameBuffer->mCommandBuffer, &dynamicBuild, 1);
		renderer.NoteWorldBlasContentChanged();
	}

	nri::BufferBarrierDesc barriers[2] = {};
	barriers[0].buffer = renderer.mFrameBuffer->mRayTracing.GetAccelerationStructureBuffer(*outAccelerationStructure.accelerationStructure);
	barriers[0].before = NRIResourceAccelerationStructureWriteAccess();
	barriers[0].after = NRIResourceAccelerationStructureReadAccess();
	barriers[1].buffer = scratchBuffer.buffer;
	barriers[1].before = NRIResourceAccelerationStructureScratchAccess();
	barriers[1].after = NRIResourceAccelerationStructureScratchAccess();

	nri::BarrierDesc barrierDesc = {};
	barrierDesc.buffers = barriers;
	barrierDesc.bufferNum = 2;
	{
		ScopedPtPerfTimer phaseTimer(renderer.mLastPerfShellTraceStats.dynamicAsBarrierMs);
		renderer.mFrameBuffer->mCore.CmdBarrier(*renderer.mFrameBuffer->mCommandBuffer, barrierDesc);
	}
	renderer.mBuiltDynamicSceneASLastFrame = true;
	outAccelerationStructure.buildVertexBuffer = vertexBuffer.buffer;
	outAccelerationStructure.buildIndexBuffer = indexBuffer.buffer;
	outAccelerationStructure.buildVertexOffset = vertexOffset;
	outAccelerationStructure.buildVertexCount = vertexCount;
	outAccelerationStructure.buildIndexOffset = indexOffset;
	outAccelerationStructure.buildIndexCount = indexCount;
	outAccelerationStructure.buildPrimitiveCount = primitiveCount;
	outAccelerationStructure.buildScratchSize = requiredScratchSize;
	outAccelerationStructure.buildFlags = buildFlags;
	outAccelerationStructure.buildType = nri::AccelerationStructureType::BOTTOM_LEVEL;
	outAccelerationStructure.buildTypeValid = true;
	outAccelerationStructure.uncompactedMemorySize = outAccelerationStructure.memorySize;
	outAccelerationStructure.compacted = false;
	if (updateDynamicPerfStats)
	{
		renderer.mDynamicSceneLastFrame.asBuildCount++;
	}
	return true;
}

bool NRIAccelerationStructureManager::BuildEmissiveTopLevel(NRIRenderer& renderer)
{
	ScopedPtPerfTimer perfTimer(renderer.mLastPerfShellTraceStats.emissiveTlasMs);
	renderer.mEmissiveTlasInstanceCount = 0;
	renderer.mEmissiveTlasStaticInstanceCount = 0;
	renderer.mEmissiveTlasDynamicInstanceCount = 0;
	renderer.mLastPerfShellTraceStats.emissiveAsEnabled = (bool)nri_ptemissivetlas;
	renderer.mLastPerfShellTraceStats.emissiveAsRecords = (uint32_t)renderer.mBoundEmissivePrimitiveRecords.size();
	for (const NRIRenderer::EmissivePrimitiveDebugRecord& record : renderer.mBoundEmissivePrimitiveRecords)
	{
		if (record.dataSource == nri_diag::SceneDataSourceStatic)
		{
			renderer.mLastPerfShellTraceStats.emissiveAsRecordsStatic++;
		}
		else if (record.dataSource == nri_diag::SceneDataSourceDynamic)
		{
			renderer.mLastPerfShellTraceStats.emissiveAsRecordsDynamic++;
		}
		else if (record.dataSource == nri_diag::SceneDataSourcePersistentVoxel)
		{
			renderer.mLastPerfShellTraceStats.emissiveAsRecordsPersistentVoxel++;
		}
	}

	if (!nri_ptemissivetlas ||
		renderer.mBoundEmissivePrimitiveRecords.empty() ||
		renderer.mBoundSceneInstances.empty())
	{
		renderer.DestroyBufferResource(renderer.mEmissiveTlasInstanceBuffer);
		renderer.RetireTopLevelAccelerationStructure(renderer.mEmissiveTopLevelAS);
		renderer.mEmissiveTlasInstancePayloadCacheValid = false;
		renderer.mEmissiveTlasInstancePayloadHash = 0;
		return true;
	}

	std::unordered_map<uint32_t, uint32_t> staticSceneInstanceByPrimitiveOffset;
	staticSceneInstanceByPrimitiveOffset.reserve(renderer.mBoundSceneInstances.size());
	const bool dynamicPartitionRouteActive = !renderer.mSelectedDynamicOverlayBlasOccurrences.empty();
	uint32_t dynamicSceneInstanceIndex = UINT32_MAX;
	for (uint32_t sceneInstanceIndex = 0; sceneInstanceIndex < (uint32_t)renderer.mBoundSceneInstances.size(); ++sceneInstanceIndex)
	{
		const SceneInstanceData& sceneInstance = renderer.mBoundSceneInstances[sceneInstanceIndex];
		if (sceneInstance.dataSource == nri_diag::SceneDataSourceStatic)
		{
			staticSceneInstanceByPrimitiveOffset.emplace(sceneInstance.primitiveBase, sceneInstanceIndex);
		}
		else if (!dynamicPartitionRouteActive &&
			sceneInstance.dataSource == nri_diag::SceneDataSourceDynamic &&
			dynamicSceneInstanceIndex == UINT32_MAX)
		{
			dynamicSceneInstanceIndex = sceneInstanceIndex;
		}
	}

	std::vector<uint8_t> emissiveStaticChunks(renderer.mStaticMapScene.chunks.size(), 0u);
	bool includeDynamicInstance = false;
	std::vector<uint8_t> emissiveDynamicPartitions(
		renderer.mSelectedDynamicOverlayBlasOccurrences.size(),
		0u);
	bool unmatchedDynamicPartitionRecord = false;
	const NRIAccelerationStructureResource* dynamicBottomLevelAS = &renderer.GetCurrentDynamicBottomLevelAS();
	const auto findStaticChunkIndexForPrimitive = [&](uint32_t primitiveIndex) -> int32_t
	{
		for (uint32_t chunkIndex = 0; chunkIndex < (uint32_t)renderer.mStaticMapScene.chunks.size(); ++chunkIndex)
		{
			const auto& chunk = renderer.mStaticMapScene.chunks[chunkIndex];
			if (!chunk.active)
			{
				continue;
			}
			const uint32_t chunkBegin = chunk.primitiveOffset;
			const uint32_t chunkEnd = chunkBegin + chunk.primitiveCount;
			if (primitiveIndex >= chunkBegin && primitiveIndex < chunkEnd)
			{
				return (int32_t)chunkIndex;
			}
		}

		return -1;
	};

	for (const NRIRenderer::EmissivePrimitiveDebugRecord& record : renderer.mBoundEmissivePrimitiveRecords)
	{
		if (record.dataSource == nri_diag::SceneDataSourceStatic)
		{
			const int32_t chunkIndex = findStaticChunkIndexForPrimitive(record.primitiveIndex);
			if (chunkIndex >= 0)
			{
				emissiveStaticChunks[(size_t)chunkIndex] = 1u;
				renderer.mLastPerfShellTraceStats.emissiveAsStaticRecordMatchedChunks++;
			}
			else
			{
				renderer.mLastPerfShellTraceStats.emissiveAsStaticRecordUnmatchedChunks++;
			}
		}
		else if (record.dataSource == nri_diag::SceneDataSourceDynamic)
		{
			renderer.mLastPerfShellTraceStats.emissiveAsDynamicRecordCount++;
			if (dynamicPartitionRouteActive)
			{
				bool matchedPartition = false;
				if (record.primitiveIndex != UINT32_MAX && record.primitiveCount != 0)
				{
					const uint64_t recordBegin = record.primitiveIndex;
					const uint64_t recordEnd = recordBegin + record.primitiveCount;
					for (size_t partitionIndex = 0;
						partitionIndex < renderer.mSelectedDynamicOverlayBlasOccurrences.size();
						++partitionIndex)
					{
						const NRIRenderer::SelectedDynamicOverlayBlasOccurrence& occurrence =
							renderer.mSelectedDynamicOverlayBlasOccurrences[partitionIndex];
						const uint64_t partitionBegin = occurrence.primitiveOffset;
						const uint64_t partitionEnd = partitionBegin + occurrence.primitiveCount;
						if (recordBegin >= partitionBegin && recordEnd <= partitionEnd)
						{
							emissiveDynamicPartitions[partitionIndex] = 1u;
							matchedPartition = true;
							break;
						}
					}
				}
				unmatchedDynamicPartitionRecord |= !matchedPartition;
			}
			else if (dynamicSceneInstanceIndex != UINT32_MAX &&
				dynamicBottomLevelAS != nullptr &&
				dynamicBottomLevelAS->accelerationStructure != nullptr)
			{
				includeDynamicInstance = true;
			}
		}
		else if (record.dataSource == nri_diag::SceneDataSourcePersistentVoxel)
		{
			renderer.mLastPerfShellTraceStats.emissiveAsPersistentVoxelIgnoredRecords++;
		}
	}

	if (unmatchedDynamicPartitionRecord)
	{
		std::fill(emissiveDynamicPartitions.begin(), emissiveDynamicPartitions.end(), 1u);
	}

	std::vector<nri::TopLevelInstance> instances;
	instances.reserve(
		renderer.mStaticMapScene.chunks.size() +
		(dynamicPartitionRouteActive ? emissiveDynamicPartitions.size() : (includeDynamicInstance ? 1u : 0u)));
	for (size_t chunkIndex = 0; chunkIndex < renderer.mStaticMapScene.chunks.size(); ++chunkIndex)
	{
		if (emissiveStaticChunks[chunkIndex] == 0u)
		{
			continue;
		}

		const auto& chunk = renderer.mStaticMapScene.chunks[chunkIndex];
		if (!chunk.active || chunk.accelerationStructure.accelerationStructure == nullptr)
		{
			continue;
		}

		const auto sceneInstanceIt = staticSceneInstanceByPrimitiveOffset.find(chunk.primitiveOffset);
		if (sceneInstanceIt == staticSceneInstanceByPrimitiveOffset.end())
		{
			continue;
		}

		nri::TopLevelInstance instance = {};
		instance.transform[0][0] = 1.0f;
		instance.transform[1][1] = 1.0f;
		instance.transform[2][2] = 1.0f;
		instance.instanceId = sceneInstanceIt->second;
		instance.mask = NRI_TLAS_MASK_ALL_WORKLOADS;
		instance.shaderBindingTableLocalOffset = 0;
		instance.flags = nri::TopLevelInstanceBits::TRIANGLE_CULL_DISABLE;
		instance.accelerationStructureHandle = renderer.mFrameBuffer->mRayTracing.GetAccelerationStructureHandle(*chunk.accelerationStructure.accelerationStructure);
		instances.push_back(instance);
		renderer.mEmissiveTlasStaticInstanceCount++;
		renderer.mLastPerfShellTraceStats.emissiveAsStaticChunkRefs++;
		if (instance.mask == NRI_TLAS_MASK_ALL_WORKLOADS)
		{
			renderer.mLastPerfShellTraceStats.emissiveAsMaskAllWorkloadsRefs++;
		}
		else
		{
			renderer.mLastPerfShellTraceStats.emissiveAsMaskOtherRefs++;
		}
	}

	if (includeDynamicInstance)
	{
		nri::TopLevelInstance instance = {};
		instance.transform[0][0] = 1.0f;
		instance.transform[1][1] = 1.0f;
		instance.transform[2][2] = 1.0f;
		instance.instanceId = dynamicSceneInstanceIndex;
		instance.mask = NRI_TLAS_MASK_ALL_WORKLOADS;
		instance.shaderBindingTableLocalOffset = 0;
		instance.flags = nri::TopLevelInstanceBits::TRIANGLE_CULL_DISABLE;
		instance.accelerationStructureHandle = renderer.mFrameBuffer->mRayTracing.GetAccelerationStructureHandle(*dynamicBottomLevelAS->accelerationStructure);
		instances.push_back(instance);
		renderer.mEmissiveTlasDynamicInstanceCount = 1;
		renderer.mLastPerfShellTraceStats.emissiveAsDynamicAggregateRefs = 1;
		if (instance.mask == NRI_TLAS_MASK_ALL_WORKLOADS)
		{
			renderer.mLastPerfShellTraceStats.emissiveAsMaskAllWorkloadsRefs++;
		}
		else
		{
			renderer.mLastPerfShellTraceStats.emissiveAsMaskOtherRefs++;
		}
	}
	else if (dynamicPartitionRouteActive)
	{
		for (size_t partitionIndex = 0; partitionIndex < emissiveDynamicPartitions.size(); ++partitionIndex)
		{
			if (emissiveDynamicPartitions[partitionIndex] == 0u)
			{
				continue;
			}

			const NRIRenderer::SelectedDynamicOverlayBlasOccurrence& occurrence =
				renderer.mSelectedDynamicOverlayBlasOccurrences[partitionIndex];
			if (occurrence.accelerationStructure == nullptr ||
				occurrence.accelerationStructure->accelerationStructure == nullptr ||
				occurrence.sceneInstanceIndex >= renderer.mBoundSceneInstances.size() ||
				renderer.mBoundSceneInstances[occurrence.sceneInstanceIndex].dataSource != nri_diag::SceneDataSourceDynamic)
			{
				continue;
			}

			nri::TopLevelInstance instance = {};
			instance.transform[0][0] = 1.0f;
			instance.transform[1][1] = 1.0f;
			instance.transform[2][2] = 1.0f;
			instance.instanceId = occurrence.sceneInstanceIndex;
			instance.mask = NRI_TLAS_MASK_ALL_WORKLOADS;
			instance.shaderBindingTableLocalOffset = 0;
			instance.flags = nri::TopLevelInstanceBits::TRIANGLE_CULL_DISABLE;
			instance.accelerationStructureHandle = renderer.mFrameBuffer->mRayTracing.GetAccelerationStructureHandle(
				*occurrence.accelerationStructure->accelerationStructure);
			instances.push_back(instance);
			renderer.mEmissiveTlasDynamicInstanceCount++;
			renderer.mLastPerfShellTraceStats.emissiveAsDynamicAggregateRefs++;
			if (instance.mask == NRI_TLAS_MASK_ALL_WORKLOADS)
			{
				renderer.mLastPerfShellTraceStats.emissiveAsMaskAllWorkloadsRefs++;
			}
			else
			{
				renderer.mLastPerfShellTraceStats.emissiveAsMaskOtherRefs++;
			}
		}
	}

	if (instances.empty())
	{
		renderer.DestroyBufferResource(renderer.mEmissiveTlasInstanceBuffer);
		renderer.RetireTopLevelAccelerationStructure(renderer.mEmissiveTopLevelAS);
		renderer.mEmissiveTlasInstancePayloadCacheValid = false;
		renderer.mEmissiveTlasInstancePayloadHash = 0;
		return true;
	}

	const uint64_t payloadHash = BuildTlasInstancePayloadHash(instances);
	if (renderer.mEmissiveTlasInstancePayloadCacheValid &&
		renderer.mEmissiveTlasInstancePayloadHash == payloadHash &&
		renderer.mEmissiveTlasInstanceBuffer.buffer != nullptr &&
		renderer.mEmissiveTopLevelAS.accelerationStructure != nullptr)
	{
		renderer.mEmissiveTlasInstanceCount = (uint32_t)instances.size();
		renderer.mLastPerfShellTraceStats.emissiveAsPayloadCacheHits++;
		return true;
	}
	renderer.mLastPerfShellTraceStats.emissiveAsPayloadCacheMisses++;

	renderer.RetireTopLevelAccelerationStructure(renderer.mEmissiveTopLevelAS);
	if (!renderer.EnsureStructuredBuffer(
		renderer.mEmissiveTlasInstanceBuffer,
		renderer.mEmissiveTlasInstanceBufferStats,
		instances.data(),
		instances.size() * sizeof(nri::TopLevelInstance),
		sizeof(nri::TopLevelInstance),
		nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT,
		NRIResourceAccelerationStructureBuildInputAccess(),
		false,
		"emissive_tlas_instance_upload"))
	{
		return false;
	}

	nri::AccelerationStructureDesc tlasDesc = {};
	tlasDesc.type = nri::AccelerationStructureType::TOP_LEVEL;
	tlasDesc.flags = nri::AccelerationStructureBits::PREFER_FAST_TRACE;
	tlasDesc.geometryOrInstanceNum = (uint32_t)instances.size();
	if (renderer.mFrameBuffer->mRayTracing.CreateCommittedAccelerationStructure(*renderer.mFrameBuffer->mDevice, nri::MemoryLocation::DEVICE, 0.0f, tlasDesc, renderer.mEmissiveTopLevelAS.accelerationStructure) != nri::Result::SUCCESS)
	{
		return false;
	}

	{
		nri::MemoryDesc memoryDesc = {};
		renderer.mFrameBuffer->mRayTracing.GetAccelerationStructureMemoryDesc(*renderer.mEmissiveTopLevelAS.accelerationStructure, nri::MemoryLocation::DEVICE, memoryDesc);
		renderer.mEmissiveTopLevelAS.memorySize = memoryDesc.size;
		renderer.mEmissiveTopLevelAS.memoryLocation = nri::MemoryLocation::DEVICE;
	}

	const uint64_t requiredScratchSize = renderer.mFrameBuffer->mRayTracing.GetAccelerationStructureBuildScratchBufferSize(*renderer.mEmissiveTopLevelAS.accelerationStructure);
	if (renderer.mEmissiveTopLevelScratchBuffer.buffer == nullptr || renderer.mEmissiveTopLevelScratchBuffer.size < requiredScratchSize)
	{
		if (renderer.mEmissiveTopLevelScratchBuffer.buffer != nullptr)
		{
			renderer.WaitForCommandsTracked("emissive_tlas_scratch_resize");
		}
		renderer.DestroyBufferResource(renderer.mEmissiveTopLevelScratchBuffer);
		if (!renderer.CreateBufferWithoutView(renderer.mEmissiveTopLevelScratchBuffer, requiredScratchSize, 16, nri::BufferUsageBits::SCRATCH_BUFFER))
		{
			return false;
		}
	}

	if (renderer.mFrameBuffer->mRayTracing.CreateAccelerationStructureDescriptor(*renderer.mEmissiveTopLevelAS.accelerationStructure, renderer.mEmissiveTopLevelAS.descriptor) != nri::Result::SUCCESS)
	{
		return false;
	}

	nri::BuildTopLevelAccelerationStructureDesc tlasBuild = {};
	tlasBuild.dst = renderer.mEmissiveTopLevelAS.accelerationStructure;
	tlasBuild.instanceNum = (uint32_t)instances.size();
	tlasBuild.instanceBuffer = renderer.mEmissiveTlasInstanceBuffer.buffer;
	tlasBuild.instanceOffset = 0;
	tlasBuild.scratchBuffer = renderer.mEmissiveTopLevelScratchBuffer.buffer;
	tlasBuild.scratchOffset = 0;
	renderer.mFrameBuffer->mRayTracing.CmdBuildTopLevelAccelerationStructures(*renderer.mFrameBuffer->mCommandBuffer, &tlasBuild, 1);

	nri::BufferBarrierDesc tlasBarrier = {};
	tlasBarrier.buffer = renderer.mFrameBuffer->mRayTracing.GetAccelerationStructureBuffer(*renderer.mEmissiveTopLevelAS.accelerationStructure);
	tlasBarrier.before = NRIResourceAccelerationStructureWriteAccess();
	tlasBarrier.after = NRIResourceComputeAccelerationStructureReadAccess();

	nri::BarrierDesc barrierDesc = {};
	barrierDesc.buffers = &tlasBarrier;
	barrierDesc.bufferNum = 1;
	renderer.mFrameBuffer->mCore.CmdBarrier(*renderer.mFrameBuffer->mCommandBuffer, barrierDesc);

	renderer.mEmissiveTlasInstanceCount = (uint32_t)instances.size();
	renderer.mEmissiveTlasBuildCount++;
	renderer.mEmissiveTlasInstancePayloadCacheValid = true;
	renderer.mEmissiveTlasInstancePayloadHash = payloadHash;
	return true;
}

bool NRIAccelerationStructureManager::BuildTopLevel(
	NRIRenderer& renderer,
	const std::vector<nri::TopLevelInstance>& instances,
	uint32_t sceneBufferMask,
	bool reuseDestination)
{
	NRIWorldTlasFrameSlot& frameSlot = renderer.GetCurrentWorldTlasFrameSlot();
	return BuildTopLevel(
		renderer,
		instances,
		sceneBufferMask,
		frameSlot.accelerationStructure,
		frameSlot.instanceBuffer,
		frameSlot.scratchBuffer,
		&renderer.mStaticVertexBuffer,
		&renderer.mStaticIndexBuffer,
		&renderer.mActiveTlasInstanceCount,
		true,
		true,
		true,
		reuseDestination);
}

bool NRIAccelerationStructureManager::BuildTopLevel(
	NRIRenderer& renderer,
	const std::vector<nri::TopLevelInstance>& instances,
	uint32_t sceneBufferMask,
	NRIAccelerationStructureResource& topLevelAS,
	NRIBufferResource& tlasInstanceBuffer,
	NRIBufferResource& topLevelScratchBuffer,
	const NRIBufferResource* staticVertexBuffer,
	const NRIBufferResource* staticIndexBuffer,
	uint32_t* outTlasInstanceCount,
	bool updateLiveState,
	bool tlasInstanceWritesQuiesced,
	bool allowUpdate,
	bool reuseDestination)
{
	Clocker clock(NriPTAcceleration);
	ScopedPtPerfTimer perfTimer(renderer.mLastPerfShellTraceStats.worldTlasMs);
	renderer.mLastPerfShellTraceStats.worldTlasBuildCalls++;
	renderer.mLastPerfShellTraceStats.worldTlasInstanceCount = (uint32_t)instances.size();
	renderer.mLastWorldTlasInstancePayloadHash = BuildTlasInstancePayloadHash(instances);
	renderer.mLastWorldTlasSceneInstancePayloadHash = 0;
	renderer.mLastWorldTlasInstanceFrameIndex = renderer.mFrameIndex;
	renderer.mLastWorldTlasInstanceCount = (uint32_t)instances.size();
	if (instances.empty())
	{
		return false;
	}
	const nri::AccelerationStructureBits expectedBuildFlags = allowUpdate ?
		NRIResourceFlags(
			nri::AccelerationStructureBits::PREFER_FAST_TRACE,
			nri::AccelerationStructureBits::ALLOW_UPDATE) :
		nri::AccelerationStructureBits::PREFER_FAST_TRACE;
	if (reuseDestination)
	{
		if (!allowUpdate ||
			topLevelAS.accelerationStructure == nullptr ||
			topLevelAS.descriptor == nullptr ||
			topLevelAS.buildFlags != expectedBuildFlags ||
			!topLevelAS.buildTypeValid ||
			topLevelAS.buildType != nri::AccelerationStructureType::TOP_LEVEL ||
			topLevelAS.compacted ||
			topLevelAS.buildPrimitiveCount < instances.size() ||
			topLevelAS.buildScratchSize == 0 ||
			tlasInstanceBuffer.buffer == nullptr ||
			tlasInstanceBuffer.shaderView == nullptr ||
			tlasInstanceBuffer.stride != sizeof(nri::TopLevelInstance) ||
			!NRIResourceUsageIncludes(
				tlasInstanceBuffer.usage,
				nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT) ||
			topLevelScratchBuffer.buffer == nullptr)
		{
			return false;
		}
	}
	else
	{
		ScopedPtPerfTimer phaseTimer(renderer.mLastPerfShellTraceStats.worldTlasRetireMs);
		renderer.RetireTopLevelAccelerationStructure(topLevelAS);
	}

	static NRIRenderer::SceneBufferDebugStats sTlasInstanceStats = { "TLASInstance" };
	{
		ScopedPtPerfTimer phaseTimer(renderer.mLastPerfShellTraceStats.worldTlasInstanceUploadMs);
		if (!renderer.EnsureStructuredBuffer(
			tlasInstanceBuffer,
			sTlasInstanceStats,
			instances.data(),
			instances.size() * sizeof(nri::TopLevelInstance),
			sizeof(nri::TopLevelInstance),
			nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT,
			NRIResourceAccelerationStructureBuildInputAccess(),
			tlasInstanceWritesQuiesced,
			"world_tlas_instance_upload"))
		{
			return false;
		}
	}

	if (!reuseDestination)
	{
		nri::AccelerationStructureDesc tlasDesc = {};
		tlasDesc.type = nri::AccelerationStructureType::TOP_LEVEL;
		tlasDesc.flags = expectedBuildFlags;
		tlasDesc.geometryOrInstanceNum = allowUpdate ?
			(uint32_t)std::max<uint64_t>(
				instances.size(),
				tlasInstanceBuffer.size / sizeof(nri::TopLevelInstance)) :
			(uint32_t)instances.size();
		ScopedPtPerfTimer phaseTimer(renderer.mLastPerfShellTraceStats.worldTlasCreateMs);
		renderer.mLastPerfShellTraceStats.worldTlasCreateCalls++;
		if (renderer.mFrameBuffer->mRayTracing.CreateCommittedAccelerationStructure(*renderer.mFrameBuffer->mDevice, nri::MemoryLocation::DEVICE, 0.0f, tlasDesc, topLevelAS.accelerationStructure) != nri::Result::SUCCESS)
		{
			return false;
		}
		topLevelAS.buildPrimitiveCount = tlasDesc.geometryOrInstanceNum;
		topLevelAS.buildFlags = tlasDesc.flags;
		topLevelAS.buildType = tlasDesc.type;
		topLevelAS.buildTypeValid = true;
		topLevelAS.compacted = false;
	}

	if (!reuseDestination)
	{
		ScopedPtPerfTimer phaseTimer(renderer.mLastPerfShellTraceStats.worldTlasMemoryMs);
		nri::MemoryDesc memoryDesc = {};
		renderer.mFrameBuffer->mRayTracing.GetAccelerationStructureMemoryDesc(*topLevelAS.accelerationStructure, nri::MemoryLocation::DEVICE, memoryDesc);
		topLevelAS.memorySize = memoryDesc.size;
		topLevelAS.memoryLocation = nri::MemoryLocation::DEVICE;
		renderer.mLastPerfShellTraceStats.worldTlasMemoryBytes = memoryDesc.size;
	}
	else
	{
		renderer.mLastPerfShellTraceStats.worldTlasMemoryBytes = topLevelAS.memorySize;
	}

	{
		ScopedPtPerfTimer phaseTimer(renderer.mLastPerfShellTraceStats.worldTlasScratchMs);
		const uint64_t buildScratchSize = reuseDestination ?
			topLevelAS.buildScratchSize :
			renderer.mFrameBuffer->mRayTracing.GetAccelerationStructureBuildScratchBufferSize(*topLevelAS.accelerationStructure);
		const uint64_t updateScratchSize = reuseDestination ?
			topLevelAS.updateScratchSize :
			(allowUpdate ?
				renderer.mFrameBuffer->mRayTracing.GetAccelerationStructureUpdateScratchBufferSize(*topLevelAS.accelerationStructure) :
				0);
		if (!reuseDestination)
		{
			renderer.mLastPerfShellTraceStats.worldTlasScratchQueries += allowUpdate ? 2u : 1u;
		}
		const uint64_t requiredScratchSize = std::max(buildScratchSize, updateScratchSize);
		topLevelAS.buildScratchSize = buildScratchSize;
		topLevelAS.updateScratchSize = updateScratchSize;
		renderer.mLastPerfShellTraceStats.worldTlasBuildScratchRequestedBytes = buildScratchSize;
		renderer.mLastPerfShellTraceStats.worldTlasUpdateScratchRequestedBytes = updateScratchSize;
		renderer.mLastPerfShellTraceStats.worldTlasScratchRequestedBytes = requiredScratchSize;
		if (topLevelScratchBuffer.buffer == nullptr || topLevelScratchBuffer.size < requiredScratchSize)
		{
			renderer.mLastPerfShellTraceStats.worldTlasScratchGrowCount++;
			if (!tlasInstanceWritesQuiesced && topLevelScratchBuffer.buffer != nullptr)
			{
				renderer.WaitForCommandsTracked("world_tlas_scratch_resize");
			}

			NRIBufferResource oldScratchBuffer = topLevelScratchBuffer;
			topLevelScratchBuffer = {};
			if (!renderer.CreateBufferWithoutViewAtLocation(
				topLevelScratchBuffer,
				requiredScratchSize,
				16,
				nri::BufferUsageBits::SCRATCH_BUFFER,
				nri::MemoryLocation::DEVICE))
			{
				topLevelScratchBuffer = oldScratchBuffer;
				return false;
			}
			if (tlasInstanceWritesQuiesced)
			{
				renderer.RetireResidentBufferResource(oldScratchBuffer);
			}
			else
			{
				renderer.DestroyBufferResource(oldScratchBuffer);
			}
		}
		renderer.mLastPerfShellTraceStats.worldTlasScratchAllocatedBytes = topLevelScratchBuffer.size;
	}

	if (!reuseDestination)
	{
		ScopedPtPerfTimer phaseTimer(renderer.mLastPerfShellTraceStats.worldTlasDescriptorMs);
		renderer.mLastPerfShellTraceStats.worldTlasDescriptorCreateCalls++;
		if (renderer.mFrameBuffer->mRayTracing.CreateAccelerationStructureDescriptor(*topLevelAS.accelerationStructure, topLevelAS.descriptor) != nri::Result::SUCCESS)
		{
			return false;
		}
	}

	nri::BuildTopLevelAccelerationStructureDesc tlasBuild = {};
	tlasBuild.dst = topLevelAS.accelerationStructure;
	tlasBuild.instanceNum = (uint32_t)instances.size();
	tlasBuild.instanceBuffer = tlasInstanceBuffer.buffer;
	tlasBuild.instanceOffset = 0;
	tlasBuild.scratchBuffer = topLevelScratchBuffer.buffer;
	tlasBuild.scratchOffset = 0;
	if (reuseDestination)
	{
		nri::BufferBarrierDesc beforeBuildBarrier = {};
		beforeBuildBarrier.buffer = renderer.mFrameBuffer->mRayTracing.GetAccelerationStructureBuffer(*topLevelAS.accelerationStructure);
		beforeBuildBarrier.before = NRIResourceComputeAccelerationStructureReadAccess();
		beforeBuildBarrier.after = NRIResourceAccelerationStructureWriteAccess();
		nri::BarrierDesc beforeBuildBarrierDesc = {};
		beforeBuildBarrierDesc.buffers = &beforeBuildBarrier;
		beforeBuildBarrierDesc.bufferNum = 1;
		ScopedPtPerfTimer barrierTimer(renderer.mLastPerfShellTraceStats.worldTlasBarrierMs);
		renderer.mFrameBuffer->mCore.CmdBarrier(*renderer.mFrameBuffer->mCommandBuffer, beforeBuildBarrierDesc);
	}
	{
		ScopedPtPerfTimer phaseTimer(renderer.mLastPerfShellTraceStats.worldTlasBuildMs);
		NRIScopedGpuTiming worldTlasGpuTiming(renderer.mFrameBuffer, NRIGpuTimingScope::WorldTlas);
		renderer.mFrameBuffer->mCore.CmdBeginAnnotation(*renderer.mFrameBuffer->mCommandBuffer, "Raze.WorldTLAS.Build", nri::BGRA_UNUSED);
		renderer.mFrameBuffer->mRayTracing.CmdBuildTopLevelAccelerationStructures(*renderer.mFrameBuffer->mCommandBuffer, &tlasBuild, 1);
		renderer.mFrameBuffer->mCore.CmdEndAnnotation(*renderer.mFrameBuffer->mCommandBuffer);
	}

	nri::BufferBarrierDesc tlasBarrier = {};
	tlasBarrier.buffer = renderer.mFrameBuffer->mRayTracing.GetAccelerationStructureBuffer(*topLevelAS.accelerationStructure);
	tlasBarrier.before = NRIResourceAccelerationStructureWriteAccess();
	tlasBarrier.after = NRIResourceComputeAccelerationStructureReadAccess();

	std::vector<nri::BufferBarrierDesc> barriers;
	barriers.reserve(5);
	barriers.push_back(tlasBarrier);
	if ((sceneBufferMask & NRIRenderer::SceneDataBufferMask_Static) != 0 && staticVertexBuffer != nullptr && staticIndexBuffer != nullptr)
	{
		nri::BufferBarrierDesc vertexBarrier = {};
		vertexBarrier.buffer = staticVertexBuffer->buffer;
		vertexBarrier.before = NRIResourceAccelerationStructureBuildInputAccess();
		vertexBarrier.after = NRIResourceComputeShaderResourceAccess();
		barriers.push_back(vertexBarrier);

		nri::BufferBarrierDesc indexBarrier = {};
		indexBarrier.buffer = staticIndexBuffer->buffer;
		indexBarrier.before = NRIResourceAccelerationStructureBuildInputAccess();
		indexBarrier.after = NRIResourceComputeShaderResourceAccess();
		barriers.push_back(indexBarrier);
	}
	if ((sceneBufferMask & NRIRenderer::SceneDataBufferMask_Dynamic) != 0)
	{
		const NRIBufferResource& dynamicVertexBuffer = renderer.GetCurrentDynamicVertexBuffer();
		const NRIBufferResource& dynamicIndexBuffer = renderer.GetCurrentDynamicIndexBuffer();
		nri::BufferBarrierDesc vertexBarrier = {};
		vertexBarrier.buffer = dynamicVertexBuffer.buffer;
		vertexBarrier.before = NRIResourceAccelerationStructureBuildInputAccess();
		vertexBarrier.after = NRIResourceComputeShaderResourceAccess();
		barriers.push_back(vertexBarrier);

		nri::BufferBarrierDesc indexBarrier = {};
		indexBarrier.buffer = dynamicIndexBuffer.buffer;
		indexBarrier.before = NRIResourceAccelerationStructureBuildInputAccess();
		indexBarrier.after = NRIResourceComputeShaderResourceAccess();
		barriers.push_back(indexBarrier);
	}

	nri::BarrierDesc barrierDesc = {};
	barrierDesc.buffers = barriers.data();
	barrierDesc.bufferNum = (uint32_t)barriers.size();
	{
		ScopedPtPerfTimer phaseTimer(renderer.mLastPerfShellTraceStats.worldTlasBarrierMs);
		renderer.mLastPerfShellTraceStats.worldTlasBarrierCount =
			barrierDesc.bufferNum + (reuseDestination ? 1u : 0u);
		renderer.mFrameBuffer->mCore.CmdBarrier(*renderer.mFrameBuffer->mCommandBuffer, barrierDesc);
	}
	if (outTlasInstanceCount != nullptr)
	{
		*outTlasInstanceCount = (uint32_t)instances.size();
	}

	if (updateLiveState)
	{
		renderer.mActiveTlasInstanceCount = (uint32_t)instances.size();
		if ((sceneBufferMask & NRIRenderer::SceneDataBufferMask_Static) != 0 &&
			(sceneBufferMask & NRIRenderer::SceneDataBufferMask_Dynamic) == 0)
		{
			renderer.mStaticMapScene.tlasInstanceCount = (uint32_t)instances.size();
			renderer.mStaticMapScene.accelerationResident = true;
			renderer.mBuiltStaticMapSceneASLastFrame = true;
		}
	}
	return true;
}

bool NRIAccelerationStructureManager::UpdateTopLevel(
	NRIRenderer& renderer,
	const std::vector<nri::TopLevelInstance>& instances,
	uint32_t sceneBufferMask,
	NRIWorldTlasFrameSlot& frameSlot,
	const std::vector<NRIWorldTlasDirtyInstanceRange>& dirtyRanges,
	bool uploadDirtyRanges)
{
	Clocker clock(NriPTAcceleration);
	ScopedPtPerfTimer perfTimer(renderer.mLastPerfShellTraceStats.worldTlasMs);
	ScopedPtPerfTimer updateTimer(renderer.mLastPerfShellTraceStats.worldTlasUpdateMs);
	NRIAccelerationStructureResource& topLevelAS = frameSlot.accelerationStructure;
	NRIBufferResource& tlasInstanceBuffer = frameSlot.instanceBuffer;
	NRIBufferResource& topLevelScratchBuffer = frameSlot.scratchBuffer;
	if (instances.empty() ||
		frameSlot.publishedBuildInstanceCount != instances.size() ||
		topLevelAS.accelerationStructure == nullptr ||
		topLevelAS.descriptor == nullptr ||
		tlasInstanceBuffer.buffer == nullptr ||
		topLevelScratchBuffer.buffer == nullptr ||
		((uint32_t)topLevelAS.buildFlags & (uint32_t)nri::AccelerationStructureBits::ALLOW_UPDATE) == 0)
	{
		return false;
	}

	uint64_t dirtyBytes = 0;
	uint32_t dirtyInstanceCount = 0;
	for (const NRIWorldTlasDirtyInstanceRange& range : dirtyRanges)
	{
		if (range.instanceCount == 0 ||
			range.firstInstance >= instances.size() ||
			range.instanceCount > instances.size() - range.firstInstance)
		{
			return false;
		}
		dirtyInstanceCount += range.instanceCount;
		dirtyBytes += (uint64_t)range.instanceCount * sizeof(nri::TopLevelInstance);
	}
	if (uploadDirtyRanges && dirtyRanges.empty())
	{
		return false;
	}

	uint64_t updateScratchSize = topLevelAS.updateScratchSize;
	if (updateScratchSize == 0)
	{
		renderer.mLastPerfShellTraceStats.worldTlasScratchQueries++;
		updateScratchSize = renderer.mFrameBuffer->mRayTracing.GetAccelerationStructureUpdateScratchBufferSize(*topLevelAS.accelerationStructure);
		topLevelAS.updateScratchSize = updateScratchSize;
	}
	const uint64_t requiredScratchSize = std::max(topLevelAS.buildScratchSize, updateScratchSize);
	renderer.mLastPerfShellTraceStats.worldTlasUpdateScratchRequestedBytes = updateScratchSize;
	renderer.mLastPerfShellTraceStats.worldTlasScratchRequestedBytes = requiredScratchSize;
	renderer.mLastPerfShellTraceStats.worldTlasScratchAllocatedBytes = topLevelScratchBuffer.size;
	if (topLevelScratchBuffer.size < requiredScratchSize)
	{
		return false;
	}

	if (uploadDirtyRanges)
	{
		ScopedPtPerfTimer uploadTimer(renderer.mLastPerfShellTraceStats.worldTlasInstanceUploadMs);
		for (const NRIWorldTlasDirtyInstanceRange& range : dirtyRanges)
		{
			const uint64_t byteOffset = (uint64_t)range.firstInstance * sizeof(nri::TopLevelInstance);
			const uint64_t byteCount = (uint64_t)range.instanceCount * sizeof(nri::TopLevelInstance);
			if (!renderer.UpdateStructuredBufferRange(
				tlasInstanceBuffer,
				byteOffset,
				instances.data() + range.firstInstance,
				byteCount,
				NRIResourceAccelerationStructureBuildInputAccess()))
			{
				return false;
			}
		}
	}

	nri::BufferBarrierDesc beforeUpdateBarrier = {};
	beforeUpdateBarrier.buffer = renderer.mFrameBuffer->mRayTracing.GetAccelerationStructureBuffer(*topLevelAS.accelerationStructure);
	beforeUpdateBarrier.before = NRIResourceComputeAccelerationStructureReadAccess();
	beforeUpdateBarrier.after = NRIResourceAccelerationStructureReadWriteAccess();
	nri::BarrierDesc beforeUpdateBarrierDesc = {};
	beforeUpdateBarrierDesc.buffers = &beforeUpdateBarrier;
	beforeUpdateBarrierDesc.bufferNum = 1;
	{
		ScopedPtPerfTimer barrierTimer(renderer.mLastPerfShellTraceStats.worldTlasBarrierMs);
		renderer.mFrameBuffer->mCore.CmdBarrier(*renderer.mFrameBuffer->mCommandBuffer, beforeUpdateBarrierDesc);
	}

	nri::BuildTopLevelAccelerationStructureDesc tlasUpdate = {};
	tlasUpdate.dst = topLevelAS.accelerationStructure;
	tlasUpdate.src = topLevelAS.accelerationStructure;
	tlasUpdate.instanceNum = (uint32_t)instances.size();
	tlasUpdate.instanceBuffer = tlasInstanceBuffer.buffer;
	tlasUpdate.instanceOffset = 0;
	tlasUpdate.scratchBuffer = topLevelScratchBuffer.buffer;
	tlasUpdate.scratchOffset = 0;
	{
		ScopedPtPerfTimer buildTimer(renderer.mLastPerfShellTraceStats.worldTlasBuildMs);
		NRIScopedGpuTiming worldTlasGpuTiming(renderer.mFrameBuffer, NRIGpuTimingScope::WorldTlas);
		renderer.mFrameBuffer->mCore.CmdBeginAnnotation(*renderer.mFrameBuffer->mCommandBuffer, "Raze.WorldTLAS.Update", nri::BGRA_UNUSED);
		renderer.mFrameBuffer->mRayTracing.CmdBuildTopLevelAccelerationStructures(*renderer.mFrameBuffer->mCommandBuffer, &tlasUpdate, 1);
		renderer.mFrameBuffer->mCore.CmdEndAnnotation(*renderer.mFrameBuffer->mCommandBuffer);
	}

	std::vector<nri::BufferBarrierDesc> barriers;
	barriers.reserve(1);
	nri::BufferBarrierDesc tlasBarrier = {};
	tlasBarrier.buffer = renderer.mFrameBuffer->mRayTracing.GetAccelerationStructureBuffer(*topLevelAS.accelerationStructure);
	tlasBarrier.before = NRIResourceAccelerationStructureWriteAccess();
	tlasBarrier.after = NRIResourceComputeAccelerationStructureReadAccess();
	barriers.push_back(tlasBarrier);

	nri::BarrierDesc barrierDesc = {};
	barrierDesc.buffers = barriers.data();
	barrierDesc.bufferNum = (uint32_t)barriers.size();
	{
		ScopedPtPerfTimer barrierTimer(renderer.mLastPerfShellTraceStats.worldTlasBarrierMs);
		renderer.mFrameBuffer->mCore.CmdBarrier(*renderer.mFrameBuffer->mCommandBuffer, barrierDesc);
	}
	renderer.mLastPerfShellTraceStats.worldTlasBuildCalls++;
	renderer.mLastPerfShellTraceStats.worldTlasUpdateCalls++;
	renderer.mLastPerfShellTraceStats.worldTlasInstanceCount = (uint32_t)instances.size();
	renderer.mLastPerfShellTraceStats.worldTlasUpdateDirtyRangeCount += uploadDirtyRanges ? (uint32_t)dirtyRanges.size() : 0;
	renderer.mLastPerfShellTraceStats.worldTlasUpdateDirtyInstanceCount += uploadDirtyRanges ? dirtyInstanceCount : 0;
	renderer.mLastPerfShellTraceStats.worldTlasUpdateDirtyBytes += uploadDirtyRanges ? dirtyBytes : 0;
	renderer.mLastPerfShellTraceStats.worldTlasBarrierCount = 1u + barrierDesc.bufferNum;
	renderer.mLastWorldTlasInstancePayloadHash = uploadDirtyRanges ?
		BuildTlasInstancePayloadHash(instances) :
		frameSlot.publishedInstancePayloadHash;
	renderer.mLastWorldTlasSceneInstancePayloadHash = 0;
	renderer.mLastWorldTlasInstanceFrameIndex = renderer.mFrameIndex;
	renderer.mLastWorldTlasInstanceCount = (uint32_t)instances.size();
	renderer.mActiveTlasInstanceCount = (uint32_t)instances.size();
	if ((sceneBufferMask & NRIRenderer::SceneDataBufferMask_Static) != 0 &&
		(sceneBufferMask & NRIRenderer::SceneDataBufferMask_Dynamic) == 0)
	{
		renderer.mStaticMapScene.tlasInstanceCount = (uint32_t)instances.size();
		renderer.mStaticMapScene.accelerationResident = true;
		renderer.mBuiltStaticMapSceneASLastFrame = true;
	}
	return true;
}

bool NRIAccelerationStructureManager::EnsureTopLevelCapacity(NRIRenderer& renderer, uint32_t instanceCount)
{
	if (instanceCount == 0 || renderer.mFrameBuffer == nullptr)
	{
		return true;
	}

	ScopedPtPerfTimer perfTimer(renderer.mLastPerfShellTraceStats.worldTlasPreGrowMs);
	renderer.mLastPerfShellTraceStats.worldTlasPreGrowCalls++;
	renderer.mLastPerfShellTraceStats.worldTlasPreGrowInstanceCount = std::max(
		renderer.mLastPerfShellTraceStats.worldTlasPreGrowInstanceCount,
		instanceCount);

	const uint64_t instanceBytes = (uint64_t)instanceCount * sizeof(nri::TopLevelInstance);
	static NRIRenderer::SceneBufferDebugStats sTlasInstancePreGrowStats = { "TLASInstancePreGrow" };
	const uint32_t queuedFrameCount =
		renderer.mFrameBuffer != nullptr && !renderer.mFrameBuffer->mQueuedFrames.empty() ?
		(uint32_t)renderer.mFrameBuffer->mQueuedFrames.size() :
		1u;
	renderer.mWorldTlasFrameSlots.EnsureSlotCount(queuedFrameCount);
	NRIWorldTlasFrameSlot* currentFrameSlot = &renderer.GetCurrentWorldTlasFrameSlot();
	for (NRIWorldTlasFrameSlot& frameSlot : renderer.mWorldTlasFrameSlots.Slots())
	{
		const bool occupiedNonCurrentSlot =
			&frameSlot != currentFrameSlot &&
			(frameSlot.instanceBuffer.buffer != nullptr || frameSlot.publicationValid);
		if (&frameSlot != currentFrameSlot &&
			frameSlot.publicationValid &&
			frameSlot.accelerationStructure.accelerationStructure != nullptr &&
			frameSlot.accelerationStructure.buildPrimitiveCount < instanceCount)
		{
			renderer.mLastPerfShellTraceStats.worldTlasPreGrowDeferredAsSlots++;
		}
		if (occupiedNonCurrentSlot && frameSlot.instanceBuffer.size < instanceBytes)
		{
			renderer.mLastPerfShellTraceStats.worldTlasPreGrowDeferredInstanceSlots++;
			continue;
		}
		if (!NRISceneUploadManager::EnsureStructuredBufferCapacity(
			renderer,
			frameSlot.instanceBuffer,
			sTlasInstancePreGrowStats,
			instanceBytes,
			sizeof(nri::TopLevelInstance),
			nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT,
			"world_tlas_instance_capacity",
			true))
		{
			return false;
		}
		if (sTlasInstancePreGrowStats.growEventsLastFrame != 0)
		{
			renderer.mLastPerfShellTraceStats.worldTlasPreGrowInstanceGrowCount++;
			renderer.mLastPerfShellTraceStats.worldTlasPreGrowInstanceRequestedBytes +=
				sTlasInstancePreGrowStats.growthRequestedBytesLastFrame;
			renderer.mLastPerfShellTraceStats.worldTlasPreGrowInstanceAllocatedBytes +=
				sTlasInstancePreGrowStats.growthAllocatedBytesLastFrame;
			frameSlot.InvalidatePublication();
		}
	}

	nri::AccelerationStructureDesc tlasDesc = {};
	tlasDesc.type = nri::AccelerationStructureType::TOP_LEVEL;
	tlasDesc.flags = NRIResourceFlags(
		nri::AccelerationStructureBits::PREFER_FAST_TRACE,
		nri::AccelerationStructureBits::ALLOW_UPDATE);
	tlasDesc.geometryOrInstanceNum = instanceCount;
	for (const NRIWorldTlasFrameSlot& frameSlot : renderer.mWorldTlasFrameSlots.Slots())
	{
		tlasDesc.geometryOrInstanceNum = std::max(
			tlasDesc.geometryOrInstanceNum,
			(uint32_t)(frameSlot.instanceBuffer.size / sizeof(nri::TopLevelInstance)));
	}

	NRIAccelerationStructureResource probe = {};
	if (renderer.mFrameBuffer->mRayTracing.CreateCommittedAccelerationStructure(
		*renderer.mFrameBuffer->mDevice,
		nri::MemoryLocation::DEVICE,
		0.0f,
		tlasDesc,
		probe.accelerationStructure) != nri::Result::SUCCESS)
	{
		return false;
	}
	const uint64_t buildScratchSize =
		renderer.mFrameBuffer->mRayTracing.GetAccelerationStructureBuildScratchBufferSize(*probe.accelerationStructure);
	const uint64_t updateScratchSize =
		renderer.mFrameBuffer->mRayTracing.GetAccelerationStructureUpdateScratchBufferSize(*probe.accelerationStructure);
	const uint64_t requiredScratchSize = std::max(buildScratchSize, updateScratchSize);
	renderer.DestroyAccelerationStructureResource(probe);
	renderer.mLastPerfShellTraceStats.worldTlasPreGrowBuildScratchRequestedBytes =
		std::max(renderer.mLastPerfShellTraceStats.worldTlasPreGrowBuildScratchRequestedBytes, buildScratchSize);
	renderer.mLastPerfShellTraceStats.worldTlasPreGrowUpdateScratchRequestedBytes =
		std::max(renderer.mLastPerfShellTraceStats.worldTlasPreGrowUpdateScratchRequestedBytes, updateScratchSize);
	for (NRIWorldTlasFrameSlot& frameSlot : renderer.mWorldTlasFrameSlots.Slots())
	{
		NRIBufferResource& scratchBuffer = frameSlot.scratchBuffer;
		if (scratchBuffer.buffer != nullptr && scratchBuffer.size >= requiredScratchSize)
		{
			continue;
		}
		if (&frameSlot != currentFrameSlot && scratchBuffer.buffer != nullptr)
		{
			renderer.mLastPerfShellTraceStats.worldTlasPreGrowDeferredScratchSlots++;
			continue;
		}

		renderer.mLastPerfShellTraceStats.worldTlasPreGrowScratchGrowCount++;
		renderer.mLastPerfShellTraceStats.worldTlasPreGrowScratchRequestedBytes += requiredScratchSize;
		NRIBufferResource oldScratchBuffer = scratchBuffer;
		NRIBufferResource newScratchBuffer = {};
		if (!renderer.CreateBufferWithoutViewAtLocation(
			newScratchBuffer,
			requiredScratchSize,
			16,
			nri::BufferUsageBits::SCRATCH_BUFFER,
			nri::MemoryLocation::DEVICE))
		{
			return false;
		}
		scratchBuffer = newScratchBuffer;
		renderer.RetireResidentBufferResource(oldScratchBuffer);
		renderer.mLastPerfShellTraceStats.worldTlasPreGrowScratchAllocatedBytes += scratchBuffer.size;
	}

	return true;
}

void NRIRenderer::ReleaseWorldAccelerationBuildScratch(const char* reason)
{
	const uint64_t scratchBytes = mScratchBuffer.memorySize;
	const uint32_t scratchBuffers = mScratchBuffer.buffer != nullptr ? 1u : 0u;
	if (scratchBuffers == 0)
	{
		return;
	}

	DestroyBufferResource(mScratchBuffer);
	if ((int)nri_ptloadingtrace >= 1)
	{
		Printf("NRI PT transient scratch: event=release reason=%s buffers=%u bytes=%llu\n",
			reason != nullptr ? reason : "unspecified",
			scratchBuffers,
			(unsigned long long)scratchBytes);
	}
}

bool NRIRenderer::BuildDynamicAccelerationStructure(const nri_scene::GeometryData& geometry)
{
	return NRIAccelerationStructureManager::BuildDynamic(*this, geometry);
}

bool NRIRenderer::BuildDynamicAccelerationStructure(
	const nri_scene::GeometryData& geometry,
	uint32_t indexOffset,
	uint32_t indexCount,
	uint32_t primitiveCount,
	NRIAccelerationStructureResource& outAccelerationStructure,
	bool updateDynamicPerfStats)
{
	return NRIAccelerationStructureManager::BuildDynamic(*this, geometry, indexOffset, indexCount, primitiveCount, outAccelerationStructure, updateDynamicPerfStats);
}

bool NRIRenderer::BuildBottomLevelAccelerationStructure(
	const NRIBufferResource& vertexBuffer,
	const NRIBufferResource& indexBuffer,
	uint32_t vertexOffset,
	uint32_t vertexCount,
	uint32_t indexOffset,
	uint32_t indexCount,
	uint32_t primitiveCount,
	NRIAccelerationStructureResource& outAccelerationStructure,
	bool updateDynamicPerfStats,
	NRIBufferResource* buildScratchBuffer,
	nri::AccelerationStructureBits buildFlags)
{
	return NRIAccelerationStructureManager::BuildBottomLevel(
		*this,
		vertexBuffer,
		indexBuffer,
		vertexOffset,
		vertexCount,
		indexOffset,
		indexCount,
		primitiveCount,
		outAccelerationStructure,
		updateDynamicPerfStats,
		buildScratchBuffer,
		buildFlags);
}

bool NRIRenderer::BuildEmissiveTopLevelAccelerationStructure()
{
	return NRIAccelerationStructureManager::BuildEmissiveTopLevel(*this);
}

void NRIRenderer::NoteWorldBlasContentChanged()
{
	mWorldBlasContentGeneration++;
	if (mWorldBlasContentGeneration == 0)
	{
		mWorldBlasContentGeneration = 1;
	}
}

bool NRIRenderer::BuildTopLevelAccelerationStructure(const std::vector<nri::TopLevelInstance>& instances, uint32_t sceneBufferMask)
{
	const auto decisionStart = std::chrono::steady_clock::now();
	NRIWorldTlasFrameSlot& frameSlot = GetCurrentWorldTlasFrameSlot();
	const uint64_t recordingFence = GetRecordingCommandFenceValue();
	const uint64_t mapEpoch = mMapWorld.valid ? mMapWorld.buildSerial : 0;
	const uint64_t buildEpoch = mStaticMapScene.valid ? mStaticMapScene.buildSerial : 0;
	const uint64_t requiredInstanceBytes = instances.size() * sizeof(nri::TopLevelInstance);
	const bool instanceBytesEqual = frameSlot.PublishedInstanceBytesEqual(instances);

	NRIWorldTlasExactReuseInput reuseInput = {};
	reuseInput.publicationValid = frameSlot.publicationValid;
	reuseInput.hasAccelerationStructure = frameSlot.accelerationStructure.accelerationStructure != nullptr;
	reuseInput.hasDescriptor = frameSlot.accelerationStructure.descriptor != nullptr;
	reuseInput.hasInstanceBuffer =
		frameSlot.instanceBuffer.buffer != nullptr &&
		frameSlot.instanceBuffer.shaderView != nullptr &&
		frameSlot.instanceBuffer.stride == sizeof(nri::TopLevelInstance) &&
		NRIResourceUsageIncludes(
			frameSlot.instanceBuffer.usage,
			nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT);
	reuseInput.allocatedInstanceCapacity = frameSlot.accelerationStructure.buildPrimitiveCount;
	reuseInput.publishedBuildInstanceCount = frameSlot.publishedBuildInstanceCount;
	reuseInput.requiredInstanceCount = (uint32_t)instances.size();
	reuseInput.instanceBufferCapacityBytes = frameSlot.instanceBuffer.size;
	reuseInput.requiredInstanceBytes = requiredInstanceBytes;
	reuseInput.publishedMapEpoch = frameSlot.publishedMapEpoch;
	reuseInput.currentMapEpoch = mapEpoch;
	reuseInput.publishedBuildEpoch = frameSlot.publishedBuildEpoch;
	reuseInput.currentBuildEpoch = buildEpoch;
	reuseInput.publishedRecordingFence = frameSlot.publishedRecordingFence;
	reuseInput.currentRecordingFence = recordingFence;
	reuseInput.publishedFenceComplete =
		frameSlot.publishedRecordingFence != 0 &&
		IsCommandFenceValueComplete(frameSlot.publishedRecordingFence);
	reuseInput.publishedBlasGeneration = frameSlot.publishedBlasGeneration;
	reuseInput.currentBlasGeneration = mWorldBlasContentGeneration;
	reuseInput.instanceBytesEqual = instanceBytesEqual;
	const NRIWorldTlasExactReuseDecision reuseDecision = EvaluateNRIWorldTlasExactReuse(reuseInput);
	mLastPerfShellTraceStats.worldTlasBlasContentGeneration = mWorldBlasContentGeneration;

	if (reuseDecision.reuse)
	{
		mLastPerfShellTraceStats.worldTlasBuildCalls++;
		mLastPerfShellTraceStats.worldTlasExactReuseCalls++;
		mLastPerfShellTraceStats.worldTlasInstanceCount = (uint32_t)instances.size();
		mLastWorldTlasInstancePayloadHash = frameSlot.publishedInstancePayloadHash;
		mLastWorldTlasSceneInstancePayloadHash = 0;
		mLastWorldTlasInstanceFrameIndex = mFrameIndex;
		mLastWorldTlasInstanceCount = (uint32_t)instances.size();
		mActiveTlasInstanceCount = (uint32_t)instances.size();
		frameSlot.publishedRecordingFence = recordingFence;
		if ((sceneBufferMask & SceneDataBufferMask_Static) != 0 &&
			(sceneBufferMask & SceneDataBufferMask_Dynamic) == 0)
		{
			mStaticMapScene.tlasInstanceCount = (uint32_t)instances.size();
			mStaticMapScene.accelerationResident = true;
			mBuiltStaticMapSceneASLastFrame = true;
		}
		if (ShouldCollectAccelerationPerfTiming())
		{
			mLastPerfShellTraceStats.worldTlasMs += DurationMs(decisionStart, std::chrono::steady_clock::now());
		}
		return true;
	}

	const NRIWorldTlasDecision instanceDecision = ClassifyNRIWorldTlasInstances(
		frameSlot.publishedInstances,
		instances,
		frameSlot.publicationValid,
		frameSlot.accelerationStructure.buildPrimitiveCount);
	const bool updateEnabled =
		((uint32_t)frameSlot.accelerationStructure.buildFlags &
			(uint32_t)nri::AccelerationStructureBits::ALLOW_UPDATE) != 0;
	NRIWorldTlasUpdateDecision updateDecision = EvaluateNRIWorldTlasUpdate(
		instanceDecision,
		reuseInput,
		updateEnabled);
	if (ShouldCollectAccelerationPerfTiming())
	{
		mLastPerfShellTraceStats.worldTlasMs += DurationMs(decisionStart, std::chrono::steady_clock::now());
	}
	if (updateDecision.update)
	{
		const bool uploadDirtyRanges =
			(updateDecision.reasonMask & NRIWorldTlasUpdateReason_Transform) != 0;
		if (NRIAccelerationStructureManager::UpdateTopLevel(
			*this,
			instances,
			sceneBufferMask,
			frameSlot,
			instanceDecision.dirtyTransformRanges,
			uploadDirtyRanges))
		{
			mLastPerfShellTraceStats.worldTlasUpdateReasonMask |= updateDecision.reasonMask;
			if ((updateDecision.reasonMask & NRIWorldTlasUpdateReason_BlasGenerationOverride) != 0)
			{
				mLastPerfShellTraceStats.worldTlasBlasOverrideUpdateCalls++;
			}
			frameSlot.Publish(
				instances,
				mapEpoch,
				buildEpoch,
				recordingFence,
				mWorldBlasContentGeneration,
				mLastWorldTlasInstancePayloadHash);
			return true;
		}
		updateDecision.rejectReasonMask |= NRIWorldTlasUpdateRejectReason_Runtime;
	}

	const nri::AccelerationStructureBits liveBuildFlags = NRIResourceFlags(
		nri::AccelerationStructureBits::PREFER_FAST_TRACE,
		nri::AccelerationStructureBits::ALLOW_UPDATE);
	NRIWorldTlasFullBuildReuseInput fullBuildReuseInput = {};
	fullBuildReuseInput.publicationValid = frameSlot.publicationValid;
	fullBuildReuseInput.hasAccelerationStructure = reuseInput.hasAccelerationStructure;
	fullBuildReuseInput.hasDescriptor = reuseInput.hasDescriptor;
	fullBuildReuseInput.hasInstanceBuffer = reuseInput.hasInstanceBuffer;
	fullBuildReuseInput.hasScratchBuffer = frameSlot.scratchBuffer.buffer != nullptr;
	fullBuildReuseInput.updateEnabled = updateEnabled;
	fullBuildReuseInput.buildFlagsCompatible =
		frameSlot.accelerationStructure.buildFlags == liveBuildFlags;
	fullBuildReuseInput.buildTypeCompatible =
		frameSlot.accelerationStructure.buildTypeValid &&
		frameSlot.accelerationStructure.buildType == nri::AccelerationStructureType::TOP_LEVEL;
	fullBuildReuseInput.compacted = frameSlot.accelerationStructure.compacted;
	fullBuildReuseInput.destinationInComputeReadState = frameSlot.publicationValid;
	fullBuildReuseInput.allocatedInstanceCapacity = frameSlot.accelerationStructure.buildPrimitiveCount;
	fullBuildReuseInput.publishedBuildInstanceCount = frameSlot.publishedBuildInstanceCount;
	fullBuildReuseInput.requiredInstanceCount = (uint32_t)instances.size();
	fullBuildReuseInput.instanceBufferCapacityBytes = frameSlot.instanceBuffer.size;
	fullBuildReuseInput.requiredInstanceBytes = requiredInstanceBytes;
	fullBuildReuseInput.scratchBufferCapacityBytes = frameSlot.scratchBuffer.size;
	fullBuildReuseInput.cachedBuildScratchBytes = frameSlot.accelerationStructure.buildScratchSize;
	fullBuildReuseInput.requiredScratchBytes = std::max(
		frameSlot.accelerationStructure.buildScratchSize,
		frameSlot.accelerationStructure.updateScratchSize);
	fullBuildReuseInput.publishedMapEpoch = frameSlot.publishedMapEpoch;
	fullBuildReuseInput.currentMapEpoch = mapEpoch;
	fullBuildReuseInput.publishedBuildEpoch = frameSlot.publishedBuildEpoch;
	fullBuildReuseInput.currentBuildEpoch = buildEpoch;
	fullBuildReuseInput.publishedRecordingFence = frameSlot.publishedRecordingFence;
	fullBuildReuseInput.currentRecordingFence = recordingFence;
	fullBuildReuseInput.publishedFenceComplete = reuseInput.publishedFenceComplete;
	NRIWorldTlasFullBuildReuseDecision fullBuildReuseDecision =
		EvaluateNRIWorldTlasFullBuildReuse(fullBuildReuseInput);

	mLastPerfShellTraceStats.worldTlasFullBuildCalls++;
	mLastPerfShellTraceStats.worldTlasFullBuildReasonMask |= reuseDecision.rejectReasonMask;
	mLastPerfShellTraceStats.worldTlasFullBuildChangeReasonMask |= instanceDecision.reasonMask;
	mLastPerfShellTraceStats.worldTlasFullBuildUpdateRejectReasonMask |= updateDecision.rejectReasonMask;
	mLastPerfShellTraceStats.worldTlasFullBuildUpdateGateReasonMask |= updateDecision.gateRejectReasonMask;
	mLastPerfShellTraceStats.worldTlasFullBuildReuseRejectReasonMask |= fullBuildReuseDecision.rejectReasonMask;
	mLastPerfShellTraceStats.worldTlasFullBuildGrowthReasonMask |= fullBuildReuseDecision.growthReasonMask;
	const bool priorPublicationFenceUnsafe =
		frameSlot.publicationValid &&
		(fullBuildReuseDecision.sameRecordingFence || !reuseInput.publishedFenceComplete);
	if (priorPublicationFenceUnsafe)
	{
		RetireResidentBufferResource(frameSlot.instanceBuffer);
		RetireResidentBufferResource(frameSlot.scratchBuffer);
		if (fullBuildReuseDecision.sameRecordingFence)
		{
			mLastPerfShellTraceStats.worldTlasSameCommandResourceRotations++;
		}
	}
	if (!fullBuildReuseDecision.reuseDestination)
	{
		frameSlot.InvalidatePublication();
	}
	bool buildSucceeded = NRIAccelerationStructureManager::BuildTopLevel(
		*this,
		instances,
		sceneBufferMask,
		fullBuildReuseDecision.reuseDestination);
	if (!buildSucceeded && fullBuildReuseDecision.reuseDestination)
	{
		mLastPerfShellTraceStats.worldTlasFullBuildReuseRuntimeFallbacks++;
		mLastPerfShellTraceStats.worldTlasFullBuildReuseRejectReasonMask |=
			NRIWorldTlasFullBuildReuseRejectReason_Runtime;
		frameSlot.InvalidatePublication();
		buildSucceeded = NRIAccelerationStructureManager::BuildTopLevel(
			*this,
			instances,
			sceneBufferMask,
			false);
		fullBuildReuseDecision.reuseDestination = false;
	}
	if (!buildSucceeded)
	{
		return false;
	}
	if (fullBuildReuseDecision.reuseDestination)
	{
		mLastPerfShellTraceStats.worldTlasFullBuildDestinationReuseCalls++;
	}
	else
	{
		mLastPerfShellTraceStats.worldTlasFullBuildDestinationCreateCalls++;
	}
	if (fullBuildReuseDecision.growthReasonMask != NRIWorldTlasFullBuildGrowthReason_None)
	{
		mLastPerfShellTraceStats.worldTlasFullBuildGrowthCalls++;
	}

	frameSlot.Publish(
		instances,
		mapEpoch,
		buildEpoch,
		recordingFence,
		mWorldBlasContentGeneration,
		mLastWorldTlasInstancePayloadHash);
	return true;
}

bool NRIRenderer::BuildTopLevelAccelerationStructure(
	const std::vector<nri::TopLevelInstance>& instances,
	uint32_t sceneBufferMask,
	const std::vector<SceneInstanceData>& sceneInstances)
{
	if (!BuildTopLevelAccelerationStructure(instances, sceneBufferMask))
	{
		return false;
	}

	mLastWorldTlasSceneInstancePayloadHash = BuildSceneInstancePayloadHash(sceneInstances);
	return true;
}

bool NRIRenderer::BuildTopLevelAccelerationStructure(
	const std::vector<nri::TopLevelInstance>& instances,
	uint32_t sceneBufferMask,
	NRIAccelerationStructureResource& topLevelAS,
	NRIBufferResource& tlasInstanceBuffer,
	NRIBufferResource& topLevelScratchBuffer,
	const NRIBufferResource* staticVertexBuffer,
	const NRIBufferResource* staticIndexBuffer,
	uint32_t* outTlasInstanceCount,
	bool updateLiveState,
	bool tlasInstanceWritesQuiesced)
{
	return NRIAccelerationStructureManager::BuildTopLevel(
		*this,
		instances,
		sceneBufferMask,
		topLevelAS,
		tlasInstanceBuffer,
		topLevelScratchBuffer,
		staticVertexBuffer,
		staticIndexBuffer,
		outTlasInstanceCount,
		updateLiveState,
		tlasInstanceWritesQuiesced,
		false,
		false);
}

bool NRIRenderer::EnsureTopLevelAccelerationStructureCapacity(uint32_t instanceCount)
{
	return NRIAccelerationStructureManager::EnsureTopLevelCapacity(*this, instanceCount);
}

void NRIRenderer::DestroyDynamicBottomLevelAccelerationStructures()
{
	for (SceneUploadBufferRingSlot& slot : mSceneUploadBufferRing)
	{
		DestroyAccelerationStructureResource(slot.dynamicBottomLevelAS);
	}
}

void NRIRenderer::DestroyWorldTlasFrameSlots()
{
	NRIWorldTlasSlotLifecycleServices lifecycleServices = {};
	lifecycleServices.user = this;
	lifecycleServices.destroyBufferResource = [](void* user, NRIBufferResource& resource)
	{
		static_cast<NRIRenderer*>(user)->DestroyBufferResource(resource);
	};
	lifecycleServices.destroyAccelerationStructureResource = [](void* user, NRIAccelerationStructureResource& resource)
	{
		static_cast<NRIRenderer*>(user)->DestroyAccelerationStructureResource(resource);
	};
	mWorldTlasFrameSlots.Destroy(lifecycleServices);
}

void NRIRenderer::DestroyAccelerationStructures()
{
	mStaticMapScene.accelerationResident = false;
	DestroyWorldTlasFrameSlots();
	for (auto& chunk : mStaticMapScene.chunks)
	{
		DestroyAccelerationStructureResource(chunk.accelerationStructure);
		chunk.residentBlasScratchSizeCacheKey = nullptr;
		chunk.residentBlasBuildScratchSize = 0;
		chunk.residentBlasUpdateScratchSize = 0;
		chunk.residentBlasVertexBuffer = nullptr;
		chunk.residentBlasIndexBuffer = nullptr;
		chunk.residentBlasVertexNum = 0;
		chunk.residentBlasIndexOffset = 0;
		chunk.residentBlasIndexNum = 0;
	}
	DestroyDynamicBottomLevelAccelerationStructures();
	mPersistentVoxels.Reset("destroy-acceleration-structures", true, (int)nri_ptloadingtrace >= 1 || (bool)nri_voxelstats, BuildNRIPersistentVoxelResetServices(*this));
	mVoxelRepresentationPolicy.Reset();
	DestroyAccelerationStructureResource(mEmissiveTopLevelAS);
	mStaticAccelerationBuildSerial = 0;
	mActiveTlasInstanceCount = 0;
	mEmissiveTlasInstanceCount = 0;
	mEmissiveTlasStaticInstanceCount = 0;
	mEmissiveTlasDynamicInstanceCount = 0;
	mEmissiveTlasBuildCount = 0;
	mEmissiveTlasInstancePayloadCacheValid = false;
	mEmissiveTlasInstancePayloadHash = 0;
	SyncResidentMapChunkRegistryFromStaticScene();
}
