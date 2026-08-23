#pragma once

#include "NRI.h"
#include "Extensions/NRIDeviceCreation.h"
#include "Extensions/NRIHelper.h"
#include "Extensions/NRILowLatency.h"
#include "Extensions/NRIRayTracing.h"
#include "Extensions/NRIStreamer.h"
#include "Extensions/NRISwapChain.h"
#include "Extensions/NRIUpscaler.h"

#include <vector>

class NRIHardwareTexture;

enum class NRISamplerMode : uint8_t
{
	ClampLinear,
	WrapLinear,
	ClampPoint,
	WrapPoint,
	Count
};

struct NRITextureResource
{
	nri::Texture* texture = nullptr;
	nri::Descriptor* shaderView = nullptr;
	nri::Descriptor* storageView = nullptr;
	nri::Descriptor* colorAttachmentView = nullptr;
	nri::DescriptorSet* textureSet = nullptr;
	nri::AccessLayoutStage state = {};
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t mipCount = 1;
	uint32_t layerNum = 1;
	nri::Format format = nri::Format::UNKNOWN;
	uint64_t memorySize = 0;
	nri::MemoryLocation memoryLocation = nri::MemoryLocation::DEVICE;
	nri::TextureType type = nri::TextureType::TEXTURE_2D;
	nri::TextureView shaderViewType = nri::TextureView::TEXTURE;
	nri::TextureUsageBits usage = nri::TextureUsageBits::NONE;
	bool owned = false;
};

struct NRIShaderConstants
{
	float InvViewportSize[2] = { 1.0f, 1.0f };
	uint32_t Flags = 0;
	float ScreenFade = 1.0f;
	float OutputInfo[4] = { 1.0f, 1.0f, 0.0f, 0.0f };

	float ObjectColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	float AddColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	float VertexColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

	float ModelMatrix[16] = {
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f
	};
	float TexMatrix[16] = {
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f
	};
};

enum NRI2DShaderFlags : uint32_t
{
	NRI2D_Textured = 1u << 0,
	NRI2D_AlphaFromRed = 1u << 1,
	NRI2D_Invert = 1u << 2,
	NRI2D_OutputHdrLinear = 1u << 3,
};

static_assert(sizeof(NRIShaderConstants) <= 224, "NRIShaderConstants must stay within the validated 2D root-constant budget.");

struct NRISwapChainImage
{
	NRITextureResource target;
	nri::Fence* acquireSemaphore = nullptr;
	nri::Fence* releaseSemaphore = nullptr;
};

inline nri::AccessLayoutStage NRIColorAttachmentState()
{
	return { nri::AccessBits::COLOR_ATTACHMENT, nri::Layout::COLOR_ATTACHMENT, nri::StageBits::COLOR_ATTACHMENT };
}

inline nri::AccessLayoutStage NRIShaderResourceState()
{
	return { nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE, nri::StageBits::FRAGMENT_SHADER };
}

inline nri::AccessLayoutStage NRIComputeShaderResourceState()
{
	return { nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE, nri::StageBits::COMPUTE_SHADER };
}

inline nri::AccessLayoutStage NRIAccelerationStructureBuildInputState()
{
	return { nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE, nri::StageBits::NONE };
}

inline nri::AccessLayoutStage NRIComputeStorageState()
{
	return { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::Layout::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
}

inline nri::AccessLayoutStage NRICopySourceState()
{
	return { nri::AccessBits::COPY_SOURCE, nri::Layout::COPY_SOURCE, nri::StageBits::COPY };
}

inline nri::AccessLayoutStage NRICopyDestinationState()
{
	return { nri::AccessBits::COPY_DESTINATION, nri::Layout::COPY_DESTINATION, nri::StageBits::COPY };
}
