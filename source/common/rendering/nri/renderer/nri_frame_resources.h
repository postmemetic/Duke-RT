#pragma once

#include "nri_resources.h"
#include "nri_scene_upload_identity.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

class NRIRenderer;

class NRIFrameResources
{
public:
	static bool EnsureFrameResources(NRIRenderer& renderer, uint32_t outputWidth, uint32_t outputHeight, uint32_t targetWidth, uint32_t targetHeight);
	static bool CreateFrameTexture(NRIRenderer& renderer, uint32_t slot, uint32_t width, uint32_t height, nri::Format format);
	static nri::Format ResolveFinalSceneFormat(const NRIRenderer& renderer);
	static void DestroyFrameTextures(NRIRenderer& renderer);
};

struct SceneBufferDebugStats
{
	const char* label = "";
	uint32_t growthCount = 0;
	uint32_t overwriteCount = 0;
	uint32_t uploadCount = 0;
	uint32_t growEventsLastFrame = 0;
	uint32_t overwriteEventsLastFrame = 0;
	uint64_t bytesUploadedLastFrame = 0;
	uint64_t growthOldBytesLastFrame = 0;
	uint64_t growthRequestedBytesLastFrame = 0;
	uint64_t growthAllocatedBytesLastFrame = 0;
	uint64_t peakUsedBytes = 0;
};

enum ResidentUploadKind
{
	ResidentUploadKind_Vertex = 0,
	ResidentUploadKind_Index = 1,
	ResidentUploadKind_Primitive = 2,
	ResidentUploadKind_Material = 3,
};

struct SceneInstanceData
{
	uint32_t primitiveBase = 0;
	uint32_t dataSource = 0;
	uint32_t materialBase = 0;
	uint32_t materialCount = UINT32_MAX;
	uint32_t visibilityChunk = UINT32_MAX;
	uint32_t metadata0 = 0;
	uint32_t metadata1 = 0;
	uint32_t metadata2 = 0;
	float currentTransform[12] =
	{
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f
	};
	float previousTransform[12] =
	{
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f
	};
};

// Shadow-proxy records deliberately reuse their certified exact primitive and
// material range. Shadow rays do not gate visibility chunks, leaving this bit
// available for CPU/readback attribution without changing the shader ABI.
static constexpr uint32_t NRI_VOXEL_SHADOW_PROXY_VISIBILITY_BIT = 0x80000000u;
static constexpr uint32_t NRI_VOXEL_SHADOW_PROXY_PRIMITIVE_MASK = 0x7fffffffu;

inline uint32_t EncodeNRIVoxelShadowProxyVisibility(uint32_t primitiveCount)
{
	return NRI_VOXEL_SHADOW_PROXY_VISIBILITY_BIT |
		std::min(primitiveCount, NRI_VOXEL_SHADOW_PROXY_PRIMITIVE_MASK - 1u);
}

inline bool IsNRIVoxelShadowProxyVisibility(uint32_t visibilityChunk)
{
	return visibilityChunk != UINT32_MAX &&
		(visibilityChunk & NRI_VOXEL_SHADOW_PROXY_VISIBILITY_BIT) != 0u;
}

inline uint32_t DecodeNRIVoxelShadowProxyPrimitiveCount(uint32_t visibilityChunk)
{
	return IsNRIVoxelShadowProxyVisibility(visibilityChunk) ?
		visibilityChunk & NRI_VOXEL_SHADOW_PROXY_PRIMITIVE_MASK : 0u;
}

static_assert(sizeof(SceneInstanceData) == 128, "SceneInstanceData must match the HLSL scene instance layout");
static_assert(offsetof(SceneInstanceData, currentTransform) == 32, "SceneInstanceData currentTransform offset must match HLSL");
static_assert(offsetof(SceneInstanceData, previousTransform) == 80, "SceneInstanceData previousTransform offset must match HLSL");

struct SceneUploadBufferRingSlot
{
	NRIBufferResource vertexBuffer;
	NRIBufferResource indexBuffer;
	NRIBufferResource primitiveBuffer;
	NRIBufferResource materialBuffer;
	NRIBufferResource fluidBindingBuffer;
	NRIBufferResource fluidFrameBuffer;
	NRIBufferResource fluidPanBuffer;
	NRIAccelerationStructureResource dynamicBottomLevelAS;
	SceneBufferDebugStats fluidBindingStats = { "FluidBinding" };
	SceneBufferDebugStats fluidFrameStats = { "FluidFrame" };
	SceneBufferDebugStats fluidPanStats = { "FluidPan" };
	std::vector<uint8_t> vertexMirror;
	std::vector<uint8_t> indexMirror;
	std::vector<uint8_t> primitiveMirror;
	std::vector<uint8_t> materialMirror;
	std::vector<NRISceneBufferUploadDomainSpan> publishedSpans;
	uint64_t publishedPrimitiveExtraIdentity = 0;
	uint64_t publishedMaterialExtraIdentity = 0;
	bool publishedSpanIdentitiesValid = false;
};
