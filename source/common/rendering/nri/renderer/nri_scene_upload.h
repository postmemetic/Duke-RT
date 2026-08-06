#pragma once

#include "nri_frame_resources.h"

#include <vector>

class NRIRenderer;

class NRISceneUploadManager
{
public:
	static bool CreateStructuredBuffer(
		NRIRenderer& renderer,
		NRIBufferResource& resource,
		const void* data,
		uint64_t size,
		uint32_t stride,
		nri::BufferUsageBits usage,
		nri::AccessStage after);

	static bool EnsureStructuredBuffer(
		NRIRenderer& renderer,
		NRIBufferResource& resource,
		SceneBufferDebugStats& stats,
		const void* data,
		uint64_t size,
		uint32_t stride,
		nri::BufferUsageBits usage,
		nri::AccessStage after,
		bool writesQuiesced,
		const char* waitReason);

	static bool EnsureStructuredBufferCapacity(
		NRIRenderer& renderer,
		NRIBufferResource& resource,
		SceneBufferDebugStats& stats,
		uint64_t size,
		uint32_t stride,
		nri::BufferUsageBits usage,
		const char* waitReason,
		bool writesQuiesced = false);

	static bool UpdateStructuredBufferRange(
		NRIRenderer& renderer,
		NRIBufferResource& resource,
		uint64_t byteOffset,
		const void* data,
		uint64_t size,
		nri::AccessStage after);

	static bool UpdateReprojectionBuffer(NRIRenderer& renderer, bool* ioWaitedForWrites, bool allowSceneDataRing = true);
	static bool UpdateVisibleChunkBuffer(NRIRenderer& renderer, bool* ioWaitedForWrites, bool allowSceneDataRing = true);
	static bool UpdateVisibleFlatPlaneBuffer(NRIRenderer& renderer, bool* ioWaitedForWrites, bool allowSceneDataRing = true);
	static bool UpdateSpatialAbsenceBuffer(NRIRenderer& renderer, bool* ioWaitedForWrites, bool allowSceneDataRing = true);
	static bool UpdateSceneDataSet(
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
		const char* reason);
	static bool UpdateRuntimeLightAndSectorSceneData(NRIRenderer& renderer, const char* reason);

private:
	static bool SceneDataDescriptorsReady(NRIRenderer& renderer);
	static bool UpdateSceneDataDescriptorSlot(NRIRenderer& renderer, uint32_t slot, nri::Descriptor* descriptor, const char* reason);
	static bool WaitIfStructuredUpdateNeedsIt(
		NRIRenderer& renderer,
		NRIBufferResource& resource,
		const void* data,
		uint64_t size,
		uint32_t stride,
		bool* ioWaitedForWrites);
	static bool EnsureSceneDataBatched(
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
		bool writesQuiesced);
};
