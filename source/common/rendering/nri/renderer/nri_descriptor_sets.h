#pragma once

#include "../system/nri_local.h"
#include "nri_shader_contracts.h"

#include <array>
#include <cstdint>

class NRIRenderer;

class NRIDescriptorSetManager
{
public:
	static bool AllocateDescriptorSets(NRIRenderer& renderer);
	static bool UpdateSamplerSet(NRIRenderer& renderer);
	static bool CommitSceneDataDescriptors(NRIRenderer& renderer, const char* reason);
	static bool UpdateFrameTextureSet(NRIRenderer& renderer);
	static bool UpdateFrameTextureSet(NRIRenderer& renderer, nri::DescriptorSet* set, const std::array<nri::Descriptor*, NRI_INPUT_DESCRIPTOR_NUM>& descriptors);
	static bool UpdateOutputSet(NRIRenderer& renderer);
	static bool UpdateOutputSet(NRIRenderer& renderer, nri::DescriptorSet* set, const std::array<nri::Descriptor*, NRI_OUTPUT_DESCRIPTOR_NUM>& descriptors);

	static nri::DescriptorSet* GetCurrentSceneTextureSet(const NRIRenderer& renderer);
	static nri::DescriptorSet* GetCurrentSceneDataSet(const NRIRenderer& renderer);
	static bool IsCurrentSceneDataDescriptorsInitialized(const NRIRenderer& renderer);
	static void SetCurrentSceneDataDescriptorsInitialized(NRIRenderer& renderer, bool value);
	static void TraceSharedDescriptorRewrite(
		NRIRenderer& renderer,
		const char* setName,
		const char* reason,
		uint64_t descriptorHash,
		uint32_t descriptorCount,
		bool sceneTextureSet);
};
