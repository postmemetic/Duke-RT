#include "nri_pipeline_state.h"

#include "nri_renderer.h"
#include "nri_cvars.h"
#include "nri_shader_contracts.h"
#include "../system/nri_renderdevice.h"
#include "printf.h"

#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace
{
	static nri::StageBits PipelineComputeStage()
	{
		return nri::StageBits::COMPUTE_SHADER;
	}
}

bool NRIPipelineStateManager::CreatePipelineLayout(NRIRenderer& renderer)
{
	nri::DescriptorRangeDesc samplerRange = {};
	samplerRange.baseRegisterIndex = 0;
	samplerRange.descriptorNum = NRI_SAMPLER_DESCRIPTOR_NUM;
	samplerRange.descriptorType = nri::DescriptorType::SAMPLER;
	samplerRange.shaderStages = PipelineComputeStage();

	nri::DescriptorRangeDesc sceneTextureRange = {};
	sceneTextureRange.baseRegisterIndex = 0;
	sceneTextureRange.descriptorNum = NRI_SCENE_DESCRIPTOR_NUM;
	sceneTextureRange.descriptorType = nri::DescriptorType::TEXTURE;
	sceneTextureRange.shaderStages = PipelineComputeStage();
	sceneTextureRange.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

	nri::DescriptorRangeDesc inputRange = {};
	inputRange.baseRegisterIndex = 0;
	inputRange.descriptorNum = NRI_INPUT_DESCRIPTOR_NUM;
	inputRange.descriptorType = nri::DescriptorType::TEXTURE;
	inputRange.shaderStages = PipelineComputeStage();
	inputRange.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

	nri::DescriptorRangeDesc sceneDataRange = {};
	sceneDataRange.baseRegisterIndex = 0;
	sceneDataRange.descriptorNum = NRI_SCENE_DATA_DESCRIPTOR_NUM;
	sceneDataRange.descriptorType = nri::DescriptorType::STRUCTURED_BUFFER;
	sceneDataRange.shaderStages = PipelineComputeStage();
	sceneDataRange.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

	nri::DescriptorRangeDesc outputRange = {};
	outputRange.baseRegisterIndex = 0;
	outputRange.descriptorNum = NRI_OUTPUT_DESCRIPTOR_NUM;
	outputRange.descriptorType = nri::DescriptorType::STORAGE_TEXTURE;
	outputRange.shaderStages = PipelineComputeStage();
	outputRange.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

	nri::DescriptorRangeDesc traceStatsRange = {};
	traceStatsRange.baseRegisterIndex = NRI_OUTPUT_DESCRIPTOR_NUM;
	traceStatsRange.descriptorNum = NRI_TRACE_SHADER_STATS_DESCRIPTOR_NUM;
	traceStatsRange.descriptorType = nri::DescriptorType::STORAGE_STRUCTURED_BUFFER;
	traceStatsRange.shaderStages = PipelineComputeStage();
	traceStatsRange.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

	nri::DescriptorRangeDesc outputRanges[2] = { outputRange, traceStatsRange };

	nri::DescriptorSetDesc descriptorSets[5] = {};
	descriptorSets[0].registerSpace = 0;
	descriptorSets[0].ranges = &samplerRange;
	descriptorSets[0].rangeNum = 1;
	descriptorSets[1].registerSpace = 1;
	descriptorSets[1].ranges = &sceneTextureRange;
	descriptorSets[1].rangeNum = 1;
	descriptorSets[1].flags = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;
	descriptorSets[2].registerSpace = 2;
	descriptorSets[2].ranges = &sceneDataRange;
	descriptorSets[2].rangeNum = 1;
	descriptorSets[2].flags = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;
	descriptorSets[3].registerSpace = 3;
	descriptorSets[3].ranges = &inputRange;
	descriptorSets[3].rangeNum = 1;
	descriptorSets[3].flags = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;
	descriptorSets[4].registerSpace = 4;
	descriptorSets[4].ranges = outputRanges;
	descriptorSets[4].rangeNum = (uint32_t)std::size(outputRanges);
	descriptorSets[4].flags = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;

	nri::RootConstantDesc rootConstant = {};
	rootConstant.registerIndex = 0;
	rootConstant.size = sizeof(NRITraceSceneConstants);
	rootConstant.shaderStages = PipelineComputeStage();

	nri::RootDescriptorDesc rootDescriptors[1] = {};
	rootDescriptors[0].registerIndex = 0;
	rootDescriptors[0].shaderStages = PipelineComputeStage();
	rootDescriptors[0].descriptorType = nri::DescriptorType::ACCELERATION_STRUCTURE;

	nri::PipelineLayoutDesc desc = {};
	desc.rootRegisterSpace = 5;
	desc.rootConstants = &rootConstant;
	desc.rootConstantNum = 1;
	desc.rootDescriptors = rootDescriptors;
	desc.rootDescriptorNum = (uint32_t)std::size(rootDescriptors);
	desc.descriptorSets = descriptorSets;
	desc.descriptorSetNum = (uint32_t)std::size(descriptorSets);
	desc.shaderStages = PipelineComputeStage();

	return renderer.mFrameBuffer->mCore.CreatePipelineLayout(*renderer.mFrameBuffer->mDevice, desc, renderer.mPipelineLayout) == nri::Result::SUCCESS;
}

bool NRIPipelineStateManager::EnsureIndirectRadianceCachePipeline(NRIRenderer& renderer)
{
	if (renderer.mFrameBuffer == nullptr || renderer.mFrameBuffer->mDevice == nullptr)
	{
		return false;
	}
	if (renderer.mPipelines[(size_t)NRIRenderer::PipelineSlot::TraceOpaqueCache] != nullptr)
	{
		return true;
	}

	// Keep this layout separate from the ordinary TraceOpaque layout so the
	// default-disabled route retains its existing descriptor contract.
	nri::DescriptorRangeDesc samplerRange = {};
	samplerRange.baseRegisterIndex = 0;
	samplerRange.descriptorNum = NRI_SAMPLER_DESCRIPTOR_NUM;
	samplerRange.descriptorType = nri::DescriptorType::SAMPLER;
	samplerRange.shaderStages = PipelineComputeStage();

	nri::DescriptorRangeDesc sceneTextureRange = {};
	sceneTextureRange.baseRegisterIndex = 0;
	sceneTextureRange.descriptorNum = NRI_SCENE_DESCRIPTOR_NUM;
	sceneTextureRange.descriptorType = nri::DescriptorType::TEXTURE;
	sceneTextureRange.shaderStages = PipelineComputeStage();
	sceneTextureRange.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

	nri::DescriptorRangeDesc sceneDataRange = {};
	sceneDataRange.baseRegisterIndex = 0;
	sceneDataRange.descriptorNum = NRI_SCENE_DATA_DESCRIPTOR_NUM;
	sceneDataRange.descriptorType = nri::DescriptorType::STRUCTURED_BUFFER;
	sceneDataRange.shaderStages = PipelineComputeStage();
	sceneDataRange.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

	nri::DescriptorRangeDesc inputRange = {};
	inputRange.baseRegisterIndex = 0;
	inputRange.descriptorNum = NRI_INPUT_DESCRIPTOR_NUM;
	inputRange.descriptorType = nri::DescriptorType::TEXTURE;
	inputRange.shaderStages = PipelineComputeStage();
	inputRange.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

	nri::DescriptorRangeDesc outputRange = {};
	outputRange.baseRegisterIndex = 0;
	outputRange.descriptorNum = NRI_OUTPUT_DESCRIPTOR_NUM;
	outputRange.descriptorType = nri::DescriptorType::STORAGE_TEXTURE;
	outputRange.shaderStages = PipelineComputeStage();
	outputRange.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

	nri::DescriptorRangeDesc traceStatsRange = {};
	traceStatsRange.baseRegisterIndex = NRI_OUTPUT_DESCRIPTOR_NUM;
	traceStatsRange.descriptorNum = NRI_TRACE_SHADER_STATS_DESCRIPTOR_NUM;
	traceStatsRange.descriptorType = nri::DescriptorType::STORAGE_STRUCTURED_BUFFER;
	traceStatsRange.shaderStages = PipelineComputeStage();
	traceStatsRange.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

	nri::DescriptorRangeDesc cacheRange = {};
	cacheRange.baseRegisterIndex = 0;
	cacheRange.descriptorNum = NRI_INDIRECT_RADIANCE_CACHE_DESCRIPTOR_NUM;
	cacheRange.descriptorType = nri::DescriptorType::STORAGE_STRUCTURED_BUFFER;
	cacheRange.shaderStages = PipelineComputeStage();

	nri::DescriptorRangeDesc outputRanges[2] = { outputRange, traceStatsRange };
	nri::DescriptorSetDesc descriptorSets[6] = {};
	descriptorSets[0].registerSpace = 0;
	descriptorSets[0].ranges = &samplerRange;
	descriptorSets[0].rangeNum = 1;
	descriptorSets[1].registerSpace = 1;
	descriptorSets[1].ranges = &sceneTextureRange;
	descriptorSets[1].rangeNum = 1;
	descriptorSets[1].flags = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;
	descriptorSets[2].registerSpace = 2;
	descriptorSets[2].ranges = &sceneDataRange;
	descriptorSets[2].rangeNum = 1;
	descriptorSets[2].flags = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;
	descriptorSets[3].registerSpace = 3;
	descriptorSets[3].ranges = &inputRange;
	descriptorSets[3].rangeNum = 1;
	descriptorSets[3].flags = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;
	descriptorSets[4].registerSpace = 4;
	descriptorSets[4].ranges = outputRanges;
	descriptorSets[4].rangeNum = (uint32_t)std::size(outputRanges);
	descriptorSets[4].flags = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;
	descriptorSets[NRI_INDIRECT_RADIANCE_CACHE_SET_INDEX].registerSpace = NRI_INDIRECT_RADIANCE_CACHE_REGISTER_SPACE;
	descriptorSets[NRI_INDIRECT_RADIANCE_CACHE_SET_INDEX].ranges = &cacheRange;
	descriptorSets[NRI_INDIRECT_RADIANCE_CACHE_SET_INDEX].rangeNum = 1;

	nri::RootConstantDesc rootConstant = {};
	rootConstant.registerIndex = 0;
	rootConstant.size = sizeof(NRITraceSceneConstants);
	rootConstant.shaderStages = PipelineComputeStage();

	nri::RootDescriptorDesc rootDescriptor = {};
	rootDescriptor.registerIndex = 0;
	rootDescriptor.shaderStages = PipelineComputeStage();
	rootDescriptor.descriptorType = nri::DescriptorType::ACCELERATION_STRUCTURE;

	nri::PipelineLayoutDesc desc = {};
	desc.rootRegisterSpace = 5;
	desc.rootConstants = &rootConstant;
	desc.rootConstantNum = 1;
	desc.rootDescriptors = &rootDescriptor;
	desc.rootDescriptorNum = 1;
	desc.descriptorSets = descriptorSets;
	desc.descriptorSetNum = (uint32_t)std::size(descriptorSets);
	desc.shaderStages = PipelineComputeStage();

	if (renderer.mIndirectRadianceCachePipelineLayout == nullptr &&
		renderer.mFrameBuffer->mCore.CreatePipelineLayout(
			*renderer.mFrameBuffer->mDevice,
			desc,
			renderer.mIndirectRadianceCachePipelineLayout) != nri::Result::SUCCESS)
	{
		return false;
	}

	const bool d3d12 = renderer.mFrameBuffer->GetSelectedAPI() == nri::GraphicsAPI::D3D12;
	const char* cacheStem = renderer.mSpatialAbsenceFormat == 1u ? "TraceOpaqueTypedCache.cs." :
		(renderer.mSpatialAbsenceFormat == 2u ? "variants/diagnostic/TraceOpaqueCompareCache.cs." : "TraceOpaqueCache.cs.");
	const std::string fileName = std::string(cacheStem) + (d3d12 ? "dxil" : "spirv");
	std::vector<uint8_t> shaderBlob;
	if (!renderer.mFrameBuffer->LoadShaderBlob(fileName.c_str(), shaderBlob))
	{
		Printf("NRI PT pipeline create failed: shader=%s reason=load\n", fileName.c_str());
		return false;
	}

	nri::ShaderDesc shader = {};
	shader.stage = nri::StageBits::COMPUTE_SHADER;
	shader.bytecode = shaderBlob.data();
	shader.size = shaderBlob.size();
	shader.entryPointName = "main";
	nri::ComputePipelineDesc pipeline = {};
	pipeline.pipelineLayout = renderer.mIndirectRadianceCachePipelineLayout;
	pipeline.shader = shader;
	const nri::Result result = renderer.mFrameBuffer->mCore.CreateComputePipeline(
		*renderer.mFrameBuffer->mDevice,
		pipeline,
		renderer.mPipelines[(size_t)NRIRenderer::PipelineSlot::TraceOpaqueCache]);
	if (result != nri::Result::SUCCESS)
	{
		Printf("NRI PT pipeline create failed: shader=%s slot=%u result=%d\n",
			fileName.c_str(),
			(unsigned)NRIRenderer::PipelineSlot::TraceOpaqueCache,
			(int)result);
		return false;
	}
	return true;
}

bool NRIPipelineStateManager::CreateTaaPipelineLayout(NRIRenderer& renderer)
{
	nri::DescriptorRangeDesc inputRange = {};
	inputRange.baseRegisterIndex = 0;
	inputRange.descriptorNum = 6;
	inputRange.descriptorType = nri::DescriptorType::TEXTURE;
	inputRange.shaderStages = PipelineComputeStage();
	inputRange.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

	nri::DescriptorRangeDesc outputRange = {};
	outputRange.baseRegisterIndex = 0;
	outputRange.descriptorNum = 1;
	outputRange.descriptorType = nri::DescriptorType::STORAGE_TEXTURE;
	outputRange.shaderStages = PipelineComputeStage();
	outputRange.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

	nri::DescriptorSetDesc descriptorSets[2] = {};
	descriptorSets[0].registerSpace = 0;
	descriptorSets[0].ranges = &inputRange;
	descriptorSets[0].rangeNum = 1;
	descriptorSets[0].flags = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;
	descriptorSets[1].registerSpace = 1;
	descriptorSets[1].ranges = &outputRange;
	descriptorSets[1].rangeNum = 1;
	descriptorSets[1].flags = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;

	nri::RootConstantDesc rootConstant = {};
	rootConstant.registerIndex = 0;
	rootConstant.size = sizeof(NRITemporalConstants);
	rootConstant.shaderStages = PipelineComputeStage();

	nri::PipelineLayoutDesc desc = {};
	desc.rootRegisterSpace = 2;
	desc.rootConstants = &rootConstant;
	desc.rootConstantNum = 1;
	desc.descriptorSets = descriptorSets;
	desc.descriptorSetNum = (uint32_t)std::size(descriptorSets);
	desc.shaderStages = PipelineComputeStage();

	return renderer.mFrameBuffer->mCore.CreatePipelineLayout(*renderer.mFrameBuffer->mDevice, desc, renderer.mTaaPipelineLayout) == nri::Result::SUCCESS;
}

bool NRIPipelineStateManager::CreatePresentPipelineLayout(NRIRenderer& renderer)
{
	nri::DescriptorRangeDesc inputRange = {};
	inputRange.baseRegisterIndex = 0;
	inputRange.descriptorNum = 3;
	inputRange.descriptorType = nri::DescriptorType::TEXTURE;
	inputRange.shaderStages = PipelineComputeStage();
	inputRange.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

	nri::DescriptorRangeDesc outputRange = {};
	outputRange.baseRegisterIndex = 0;
	outputRange.descriptorNum = 1;
	outputRange.descriptorType = nri::DescriptorType::STORAGE_TEXTURE;
	outputRange.shaderStages = PipelineComputeStage();
	outputRange.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

	nri::DescriptorSetDesc descriptorSets[2] = {};
	descriptorSets[0].registerSpace = 0;
	descriptorSets[0].ranges = &inputRange;
	descriptorSets[0].rangeNum = 1;
	descriptorSets[0].flags = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;
	descriptorSets[1].registerSpace = 1;
	descriptorSets[1].ranges = &outputRange;
	descriptorSets[1].rangeNum = 1;
	descriptorSets[1].flags = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;

	nri::RootConstantDesc rootConstant = {};
	rootConstant.registerIndex = 0;
	rootConstant.size = sizeof(NRIPresentConstants);
	rootConstant.shaderStages = PipelineComputeStage();

	nri::PipelineLayoutDesc desc = {};
	desc.rootRegisterSpace = 2;
	desc.rootConstants = &rootConstant;
	desc.rootConstantNum = 1;
	desc.descriptorSets = descriptorSets;
	desc.descriptorSetNum = (uint32_t)std::size(descriptorSets);
	desc.shaderStages = PipelineComputeStage();

	return renderer.mFrameBuffer->mCore.CreatePipelineLayout(*renderer.mFrameBuffer->mDevice, desc, renderer.mPresentPipelineLayout) == nri::Result::SUCCESS;
}

bool NRIPipelineStateManager::CreateExposurePipelineLayout(NRIRenderer& renderer)
{
	nri::DescriptorRangeDesc inputRange = {};
	inputRange.baseRegisterIndex = 0;
	inputRange.descriptorNum = NRI_EXPOSURE_INPUT_DESCRIPTOR_NUM;
	inputRange.descriptorType = nri::DescriptorType::TEXTURE;
	inputRange.shaderStages = PipelineComputeStage();
	inputRange.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

	nri::DescriptorRangeDesc outputTextureRange = {};
	outputTextureRange.baseRegisterIndex = NRI_EXPOSURE_OUTPUT_TEXTURE_BASE_REGISTER;
	outputTextureRange.descriptorNum = NRI_EXPOSURE_OUTPUT_TEXTURE_DESCRIPTOR_NUM;
	outputTextureRange.descriptorType = nri::DescriptorType::STORAGE_TEXTURE;
	outputTextureRange.shaderStages = PipelineComputeStage();
	outputTextureRange.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

	nri::DescriptorRangeDesc outputBufferRange = {};
	outputBufferRange.baseRegisterIndex = NRI_EXPOSURE_OUTPUT_BUFFER_BASE_REGISTER;
	outputBufferRange.descriptorNum = NRI_EXPOSURE_OUTPUT_BUFFER_DESCRIPTOR_NUM;
	outputBufferRange.descriptorType = nri::DescriptorType::STORAGE_STRUCTURED_BUFFER;
	outputBufferRange.shaderStages = PipelineComputeStage();
	outputBufferRange.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

	nri::DescriptorRangeDesc outputRanges[2] = { outputTextureRange, outputBufferRange };

	nri::DescriptorSetDesc descriptorSets[2] = {};
	descriptorSets[0].registerSpace = NRI_EXPOSURE_SET_INPUTS;
	descriptorSets[0].ranges = &inputRange;
	descriptorSets[0].rangeNum = 1;
	descriptorSets[0].flags = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;
	descriptorSets[1].registerSpace = NRI_EXPOSURE_SET_OUTPUTS;
	descriptorSets[1].ranges = outputRanges;
	descriptorSets[1].rangeNum = (uint32_t)std::size(outputRanges);
	descriptorSets[1].flags = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;

	nri::RootConstantDesc rootConstant = {};
	rootConstant.registerIndex = NRI_EXPOSURE_ROOT_REGISTER;
	rootConstant.size = sizeof(NRIExposureConstants);
	rootConstant.shaderStages = PipelineComputeStage();

	nri::PipelineLayoutDesc desc = {};
	desc.rootRegisterSpace = NRI_EXPOSURE_SET_ROOT;
	desc.rootConstants = &rootConstant;
	desc.rootConstantNum = 1;
	desc.descriptorSets = descriptorSets;
	desc.descriptorSetNum = (uint32_t)std::size(descriptorSets);
	desc.shaderStages = PipelineComputeStage();

	return renderer.mFrameBuffer->mCore.CreatePipelineLayout(*renderer.mFrameBuffer->mDevice, desc, renderer.mExposurePipelineLayout) == nri::Result::SUCCESS;
}

bool NRIPipelineStateManager::CreateBloomPipelineLayout(NRIRenderer& renderer)
{
	nri::DescriptorRangeDesc inputRange = {};
	inputRange.baseRegisterIndex = 0;
	inputRange.descriptorNum = NRI_BLOOM_INPUT_DESCRIPTOR_NUM;
	inputRange.descriptorType = nri::DescriptorType::TEXTURE;
	inputRange.shaderStages = PipelineComputeStage();
	inputRange.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

	nri::DescriptorRangeDesc outputRange = {};
	outputRange.baseRegisterIndex = 0;
	outputRange.descriptorNum = NRI_BLOOM_OUTPUT_DESCRIPTOR_NUM;
	outputRange.descriptorType = nri::DescriptorType::STORAGE_TEXTURE;
	outputRange.shaderStages = PipelineComputeStage();
	outputRange.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

	nri::DescriptorSetDesc descriptorSets[2] = {};
	descriptorSets[0].registerSpace = NRI_BLOOM_SET_INPUTS;
	descriptorSets[0].ranges = &inputRange;
	descriptorSets[0].rangeNum = 1;
	descriptorSets[0].flags = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;
	descriptorSets[1].registerSpace = NRI_BLOOM_SET_OUTPUTS;
	descriptorSets[1].ranges = &outputRange;
	descriptorSets[1].rangeNum = 1;
	descriptorSets[1].flags = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;

	nri::RootConstantDesc rootConstant = {};
	rootConstant.registerIndex = NRI_BLOOM_ROOT_REGISTER;
	rootConstant.size = sizeof(NRIBloomConstants);
	rootConstant.shaderStages = PipelineComputeStage();

	nri::PipelineLayoutDesc desc = {};
	desc.rootRegisterSpace = NRI_BLOOM_SET_ROOT;
	desc.rootConstants = &rootConstant;
	desc.rootConstantNum = 1;
	desc.descriptorSets = descriptorSets;
	desc.descriptorSetNum = (uint32_t)std::size(descriptorSets);
	desc.shaderStages = PipelineComputeStage();

	return renderer.mFrameBuffer->mCore.CreatePipelineLayout(*renderer.mFrameBuffer->mDevice, desc, renderer.mBloomPipelineLayout) == nri::Result::SUCCESS;
}

bool NRIPipelineStateManager::CreateVoxelComputePipelineLayout(NRIRenderer& renderer)
{
	nri::DescriptorRangeDesc inputRange = {};
	inputRange.baseRegisterIndex = 0;
	inputRange.descriptorNum = NRI_VOXEL_COMPUTE_INPUT_DESCRIPTOR_NUM;
	inputRange.descriptorType = nri::DescriptorType::STRUCTURED_BUFFER;
	inputRange.shaderStages = PipelineComputeStage();
	inputRange.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

	nri::DescriptorRangeDesc faceRange = {};
	faceRange.baseRegisterIndex = 2;
	faceRange.descriptorNum = NRI_VOXEL_COMPUTE_FACE_DESCRIPTOR_NUM;
	faceRange.descriptorType = nri::DescriptorType::STRUCTURED_BUFFER;
	faceRange.shaderStages = PipelineComputeStage();
	faceRange.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

	nri::DescriptorRangeDesc resultRange = {};
	resultRange.baseRegisterIndex = 0;
	resultRange.descriptorNum = NRI_VOXEL_COMPUTE_RESULT_DESCRIPTOR_NUM;
	resultRange.descriptorType = nri::DescriptorType::STORAGE_STRUCTURED_BUFFER;
	resultRange.shaderStages = PipelineComputeStage();
	resultRange.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

	nri::DescriptorRangeDesc emitOutputRange = {};
	emitOutputRange.baseRegisterIndex = 1;
	emitOutputRange.descriptorNum = NRI_VOXEL_COMPUTE_EMIT_OUTPUT_DESCRIPTOR_NUM;
	emitOutputRange.descriptorType = nri::DescriptorType::STORAGE_STRUCTURED_BUFFER;
	emitOutputRange.shaderStages = PipelineComputeStage();
	emitOutputRange.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

	nri::DescriptorRangeDesc scratchRange = {};
	scratchRange.baseRegisterIndex = 4;
	scratchRange.descriptorNum = NRI_VOXEL_COMPUTE_SCRATCH_DESCRIPTOR_NUM;
	scratchRange.descriptorType = nri::DescriptorType::STORAGE_STRUCTURED_BUFFER;
	scratchRange.shaderStages = PipelineComputeStage();
	scratchRange.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

	nri::DescriptorRangeDesc inputRanges[2] = { inputRange, faceRange };
	nri::DescriptorRangeDesc outputRanges[3] = { resultRange, emitOutputRange, scratchRange };

	nri::DescriptorSetDesc descriptorSets[2] = {};
	descriptorSets[0].registerSpace = NRI_VOXEL_COMPUTE_SET_INPUTS;
	descriptorSets[0].ranges = inputRanges;
	descriptorSets[0].rangeNum = (uint32_t)std::size(inputRanges);
	descriptorSets[0].flags = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;
	descriptorSets[1].registerSpace = NRI_VOXEL_COMPUTE_SET_OUTPUTS;
	descriptorSets[1].ranges = outputRanges;
	descriptorSets[1].rangeNum = (uint32_t)std::size(outputRanges);
	descriptorSets[1].flags = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;

	nri::RootConstantDesc rootConstant = {};
	rootConstant.registerIndex = NRI_VOXEL_COMPUTE_ROOT_REGISTER;
	rootConstant.size = sizeof(NRIVoxelComputeConstants);
	rootConstant.shaderStages = PipelineComputeStage();

	nri::PipelineLayoutDesc desc = {};
	desc.rootRegisterSpace = NRI_VOXEL_COMPUTE_SET_ROOT;
	desc.rootConstants = &rootConstant;
	desc.rootConstantNum = 1;
	desc.descriptorSets = descriptorSets;
	desc.descriptorSetNum = (uint32_t)std::size(descriptorSets);
	desc.shaderStages = PipelineComputeStage();

	return renderer.mFrameBuffer->mCore.CreatePipelineLayout(*renderer.mFrameBuffer->mDevice, desc, renderer.mVoxelComputePipelineLayout) == nri::Result::SUCCESS;
}

bool NRIPipelineStateManager::CreatePipelines(NRIRenderer& renderer)
{
	auto createPipeline = [&renderer](const char* fileName, NRIRenderer::PipelineSlot slot, nri::PipelineLayout* layout)
	{
		std::vector<uint8_t> shaderBlob;
		if (!renderer.mFrameBuffer->LoadShaderBlob(fileName, shaderBlob))
		{
			Printf("NRI PT pipeline create failed: shader=%s reason=load\n", fileName);
			return false;
		}

		nri::ShaderDesc shader = {};
		shader.stage = nri::StageBits::COMPUTE_SHADER;
		shader.bytecode = shaderBlob.data();
		shader.size = shaderBlob.size();
		shader.entryPointName = "main";

		nri::ComputePipelineDesc pipelineDesc = {};
		pipelineDesc.pipelineLayout = layout;
		pipelineDesc.shader = shader;
		if ((int)nri_ptvoxelcomputetrace > 0 && fileName != nullptr && std::strncmp(fileName, "VoxelCompute", 12) == 0)
		{
			Printf("NRI PT voxel pipeline create: shader=%s slot=%u event=begin\n", fileName, (unsigned)slot);
		}
		const nri::Result result = renderer.mFrameBuffer->mCore.CreateComputePipeline(
			*renderer.mFrameBuffer->mDevice,
			pipelineDesc,
			renderer.mPipelines[(size_t)slot]);
		if (result != nri::Result::SUCCESS)
		{
			Printf("NRI PT pipeline create failed: shader=%s slot=%u result=%d\n", fileName, (unsigned)slot, (int)result);
			return false;
		}
		return true;
	};

	const bool d3d12 = renderer.mFrameBuffer->GetSelectedAPI() == nri::GraphicsAPI::D3D12;
	const std::string suffix = d3d12 ? "dxil" : "spirv";

	const char* traceStem = renderer.mSpatialAbsenceFormat == 1u ? "TraceOpaqueTyped.cs." :
		(renderer.mSpatialAbsenceFormat == 2u ? "variants/diagnostic/TraceOpaqueCompare.cs." : "TraceOpaque.cs.");
	const std::string trace = traceStem + suffix;
	const std::string composition = "Composition.cs." + suffix;
	const std::string traceTransparent = "TraceTransparent.cs." + suffix;
	const std::string taa = "Taa.cs." + suffix;
	const std::string rawPresent = "RawPresent.cs." + suffix;
	const std::string finalPresent = "FinalPresent.cs." + suffix;
	const std::string dlssSrBefore = "DlssSrBefore.cs." + suffix;
	const std::string dlssBefore = "DlssBefore.cs." + suffix;
	const std::string dlssAfter = "DlssAfter.cs." + suffix;
	const std::string final = "Final.cs." + suffix;
	const std::string exposureHistogramClear = "ExposureHistogramClear.cs." + suffix;
	const std::string exposureHistogramBuild = "ExposureHistogramBuild.cs." + suffix;
	const std::string exposureResolve = "ExposureResolve.cs." + suffix;
	const std::string bloomCopy = "BloomCopy.cs." + suffix;
	const std::string voxelComputeCount = "VoxelComputeCount.cs." + suffix;
	const std::string voxelComputeEmit = "VoxelComputeEmit.cs." + suffix;
	const std::string voxelComputeClassify = "VoxelComputeClassify.cs." + suffix;
	const std::string voxelComputeScan = "VoxelComputeScan.cs." + suffix;
	const std::string voxelComputeEmitParallel = "VoxelComputeEmitParallel.cs." + suffix;
	const std::string voxelComputeFinalize = "VoxelComputeFinalize.cs." + suffix;
	const std::string bloomDownsample = "BloomDownsample.cs." + suffix;
	const std::string bloomUpsample = "BloomUpsample.cs." + suffix;
	const std::string bloomComposite = "BloomComposite.cs." + suffix;

	return
		createPipeline(trace.c_str(), NRIRenderer::PipelineSlot::TraceOpaque, renderer.mPipelineLayout) &&
		createPipeline(composition.c_str(), NRIRenderer::PipelineSlot::Composition, renderer.mPipelineLayout) &&
		createPipeline(traceTransparent.c_str(), NRIRenderer::PipelineSlot::TraceTransparent, renderer.mPipelineLayout) &&
		createPipeline(exposureHistogramClear.c_str(), NRIRenderer::PipelineSlot::ExposureHistogramClear, renderer.mExposurePipelineLayout) &&
		createPipeline(exposureHistogramBuild.c_str(), NRIRenderer::PipelineSlot::ExposureHistogramBuild, renderer.mExposurePipelineLayout) &&
		createPipeline(exposureResolve.c_str(), NRIRenderer::PipelineSlot::ExposureResolve, renderer.mExposurePipelineLayout) &&
		createPipeline(taa.c_str(), NRIRenderer::PipelineSlot::Taa, renderer.mTaaPipelineLayout) &&
		createPipeline(rawPresent.c_str(), NRIRenderer::PipelineSlot::RawPresent, renderer.mPresentPipelineLayout) &&
		createPipeline(finalPresent.c_str(), NRIRenderer::PipelineSlot::FinalPresent, renderer.mPresentPipelineLayout) &&
		createPipeline(dlssSrBefore.c_str(), NRIRenderer::PipelineSlot::DlssSrBefore, renderer.mPipelineLayout) &&
		createPipeline(dlssBefore.c_str(), NRIRenderer::PipelineSlot::DlssBefore, renderer.mPipelineLayout) &&
		createPipeline(dlssAfter.c_str(), NRIRenderer::PipelineSlot::DlssAfter, renderer.mPipelineLayout) &&
		createPipeline(final.c_str(), NRIRenderer::PipelineSlot::Final, renderer.mPipelineLayout) &&
		createPipeline(bloomCopy.c_str(), NRIRenderer::PipelineSlot::BloomCopy, renderer.mBloomPipelineLayout) &&
		createPipeline(voxelComputeCount.c_str(), NRIRenderer::PipelineSlot::VoxelComputeCount, renderer.mVoxelComputePipelineLayout) &&
		createPipeline(voxelComputeEmit.c_str(), NRIRenderer::PipelineSlot::VoxelComputeEmit, renderer.mVoxelComputePipelineLayout) &&
		createPipeline(voxelComputeClassify.c_str(), NRIRenderer::PipelineSlot::VoxelComputeClassify, renderer.mVoxelComputePipelineLayout) &&
		createPipeline(voxelComputeScan.c_str(), NRIRenderer::PipelineSlot::VoxelComputeScan, renderer.mVoxelComputePipelineLayout) &&
		createPipeline(voxelComputeEmitParallel.c_str(), NRIRenderer::PipelineSlot::VoxelComputeEmitParallel, renderer.mVoxelComputePipelineLayout) &&
		createPipeline(voxelComputeFinalize.c_str(), NRIRenderer::PipelineSlot::VoxelComputeFinalize, renderer.mVoxelComputePipelineLayout) &&
		createPipeline(bloomDownsample.c_str(), NRIRenderer::PipelineSlot::BloomDownsample, renderer.mBloomPipelineLayout) &&
		createPipeline(bloomUpsample.c_str(), NRIRenderer::PipelineSlot::BloomUpsample, renderer.mBloomPipelineLayout) &&
		createPipeline(bloomComposite.c_str(), NRIRenderer::PipelineSlot::BloomComposite, renderer.mBloomPipelineLayout);
}
