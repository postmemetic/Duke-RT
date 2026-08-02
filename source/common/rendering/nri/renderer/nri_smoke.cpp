#include "nri_smoke.h"

#include "nri_pass_dispatch.h"
#include "nri_renderer.h"
#include "../system/nri_gpu_timing.h"
#include "../system/nri_renderdevice.h"
#include "gamecontrol.h"
#include "lightoverlay.h"
#include "printf.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace
{
	constexpr uint32_t kThreads = 64;
	constexpr uint32_t kMaxCommands = 256;
	constexpr uint32_t kWideCellCount = 16u * 9u;
	constexpr uint32_t kMaximumReferencesPerParticle = 512u;
	constexpr uint32_t kDirectionalProbesPerParticle = 8000u;
	constexpr uint32_t kDirectionalProbeThreadGroupWidth = 64u;
	constexpr uint32_t kDirectionalParticleThreadGroupHeight = 2u;
	constexpr uint32_t kSmokeCoreStorageBufferCount = 17u;
	constexpr uint32_t kSmokeDirectStorageBufferCount = 2u;
	constexpr uint32_t kSmokeGridLightingStorageBufferCount = NRISmokeGridLighting::StorageDescriptorCount;
	constexpr uint32_t kSmokeViewStorageBufferCount = 3u;
	constexpr uint32_t kSmokePromptStorageBufferCount = 1u;
	constexpr uint32_t kSmokeAnalyticStorageBufferCount = 5u;
	constexpr uint32_t kSmokeDormantStorageBufferCount =
		NRISmokeDormantGrid::EvaluationDescriptorCount;
	constexpr uint32_t kSmokeStorageDescriptorCount = kSmokeCoreStorageBufferCount + NRISmokeGrid::EvaluationDescriptorCount +
		kSmokeDirectStorageBufferCount + kSmokeGridLightingStorageBufferCount + kSmokeViewStorageBufferCount +
		kSmokePromptStorageBufferCount + kSmokeAnalyticStorageBufferCount +
		kSmokeDormantStorageBufferCount;
	constexpr uint32_t kSmokeViewStorageBase = kSmokeStorageDescriptorCount -
		kSmokeDormantStorageBufferCount - kSmokeAnalyticStorageBufferCount -
		kSmokePromptStorageBufferCount - kSmokeViewStorageBufferCount;
	constexpr uint32_t kSmokePromptStorageBase = kSmokeStorageDescriptorCount -
		kSmokeDormantStorageBufferCount - kSmokeAnalyticStorageBufferCount -
		kSmokePromptStorageBufferCount;
	constexpr uint32_t kSmokeAnalyticStorageBase = kSmokeStorageDescriptorCount -
		kSmokeDormantStorageBufferCount - kSmokeAnalyticStorageBufferCount;
	constexpr uint32_t kSmokeDormantStorageBase = kSmokeStorageDescriptorCount -
		kSmokeDormantStorageBufferCount;
	constexpr uint32_t kSmokeFilteredSceneBufferCount = 8u;
	constexpr uint32_t kSmokeEmissiveSceneBufferCount = 7u;
	constexpr uint32_t kSmokeExtendedSceneBufferCount = 10u;
	constexpr uint32_t kSmokeFlagDirectReuseShift = 14u;
	constexpr uint32_t kSmokeFlagCompareRepresentation = 0x10000u;
	constexpr uint32_t kSmokeFlagGridRepresentation = 0x20000u;
	constexpr uint32_t kSmokeFlagDirectHistoryValid = 0x40000u;
	constexpr uint32_t kSmokeFlagDirectReferenceShift = 19u;
	constexpr uint32_t kSmokeFlagGridLightingWorld = 0x200000u;
	constexpr uint32_t kSmokeFlagGridLightingCompare = 0x400000u;
	constexpr uint32_t kSmokeGridResidentState = 2u;
	constexpr uint32_t kSmokeGridBrickContent = 1u;
	constexpr uint32_t kSmokeLightSourceDormantGrid = 0x100u;

	bool SmokeChunkMarked(const std::vector<uint32_t>& words, uint32_t chunkIndex)
	{
		const size_t wordIndex = chunkIndex >> 5u;
		return wordIndex < words.size() &&
			(words[wordIndex] & (1u << (chunkIndex & 31u))) != 0u;
	}
	constexpr uint32_t kSmokeFlagGridLightingFilter = 0x800000u;
	constexpr uint32_t kSmokeFlagEmissiveLegacyGatherDisabled = 0x1000000u;
	constexpr uint32_t kSmokeFlagEmissiveQuarterKey = 0x2000000u;
	constexpr uint32_t kSmokeFlagGridLightingFieldPing = 0x4000000u;
	constexpr uint32_t kSmokeFlagGridLightingDebugShift = 27u;
	constexpr uint32_t kSmokeFlagViewMask = 0x40000000u;
	constexpr uint32_t kSmokeFlagGridLightingLocalProposals = 0x80000000u;
	const char* const kSmokePipelineNames[] = { "SmokeClear", "SmokeSimulate", "SmokeSpawn", "SmokeBin", "SmokeLightDirectionalCarriers", "SmokeEvaluateMedium", "SmokeEvaluateGrid", "SmokeLightPoint", "SmokeLightDirectional", "SmokeLightDirectTemporal", "SmokeLightDirectSpatial", "SmokeLightEmissive", "SmokeLightEmissiveTemporal", "SmokeLightEmissiveSpatial", "SmokeLightIndirectReference", "SmokeLightIndirectTemporal", "SmokeLightIndirectSpatial", "SmokeIntegrate", "SmokeResolveVolume", "SmokeTemporalVolume", "SmokeComposite", "SmokeEvaluateGridCompact", "SmokePromptFallback", "SmokeAnalyticClear", "SmokeAnalyticBuildTiles", "SmokeAnalyticMaterialize", "SmokeAnalyticEmissiveBuild", "SmokeAnalyticEmissiveResolve" };
	static_assert(std::size(kSmokePipelineNames) == (size_t)NRISmokePass::Count);

	uint32_t PackDirectionalLightColor24(const float color[3])
	{
		auto packChannel = [](float value) -> uint32_t
		{
			return (uint32_t)std::clamp((int)std::lround((double)(std::clamp(value, 0.0f, 8.0f) * (255.0f / 8.0f))), 0, 255);
		};
		return packChannel(color[0]) | (packChannel(color[1]) << 8u) | (packChannel(color[2]) << 16u);
	}

	uint32_t Groups(uint64_t count)
	{
		return (uint32_t)std::max<uint64_t>(1, (count + kThreads - 1) / kThreads);
	}

	uint32_t FloatBits(float value)
	{
		uint32_t bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		return bits;
	}

	uint64_t HashCombine64(uint64_t hash, uint64_t value)
	{
		hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u);
		return hash;
	}

}

bool NRISmokeSystem::LoadGridShaderBlob(void* user, const char* fileName, std::vector<uint8_t>& outBlob)
{
	NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
	return renderer != nullptr && renderer->mFrameBuffer->LoadShaderBlob(fileName, outBlob);
}

void NRISmokeSystem::WaitForGridCommands(void* user, const char* reason)
{
	(void)reason;
	NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
	if (renderer != nullptr)
		renderer->WaitForCommandsTracked();
}

NRISmokeGridServices NRISmokeSystem::BuildGridServices(NRIRenderer& renderer) const
{
	NRISmokeGridServices services = {};
	services.core = &renderer.mFrameBuffer->mCore;
	services.device = renderer.mFrameBuffer->mDevice;
	services.commandBuffer = renderer.mFrameBuffer->mCommandBuffer;
	services.gpuTimingDevice = renderer.mFrameBuffer;
	services.descriptorPool = renderer.mFrameBuffer->mDescriptorPool;
	services.graphicsAPI = renderer.mFrameBuffer->GetSelectedAPI();
	services.queuedFrameCount = std::max(1u, (uint32_t)renderer.mFrameBuffer->mQueuedFrames.size());
	services.queuedFrameIndex = renderer.mFrameBuffer->mCurrentQueuedFrameIndex;
	services.rendererFrame = renderer.mFrameBuffer->mFrameIndex;
	services.user = &renderer;
	services.loadShaderBlob = &NRISmokeSystem::LoadGridShaderBlob;
	services.waitForCommands = &NRISmokeSystem::WaitForGridCommands;
	return services;
}

bool NRISmokeSystem::CreateBuffer(NRIRenderer& renderer, NRIBufferResource& out, uint64_t size, uint32_t stride,
		nri::BufferUsageBits usage, nri::MemoryLocation location, bool srv, bool uav)
	{
		renderer.DestroyBufferResource(out);
		nri::BufferDesc desc = {};
		desc.size = std::max<uint64_t>(size, stride);
		desc.structureStride = stride;
		desc.usage = usage;
		if (renderer.mFrameBuffer->mCore.CreateCommittedBuffer(*renderer.mFrameBuffer->mDevice, location, 0.0f, desc, out.buffer) != nri::Result::SUCCESS)
			return false;
		nri::MemoryDesc memory = {};
		renderer.mFrameBuffer->mCore.GetBufferMemoryDesc(*out.buffer, location, memory);
		out.size = desc.size;
		out.usedSize = desc.size;
		out.memorySize = memory.size;
		out.stride = stride;
		out.usage = usage;
		out.memoryLocation = location;

		auto createView = [&](nri::BufferView type, nri::Descriptor*& descriptor)
		{
			nri::BufferViewDesc view = {};
			view.buffer = out.buffer;
			view.type = type;
			view.offset = 0;
			view.size = nri::WHOLE_SIZE;
			view.structureStride = stride;
			return renderer.mFrameBuffer->mCore.CreateBufferView(view, descriptor) == nri::Result::SUCCESS;
		};
		if ((srv && !createView(nri::BufferView::STRUCTURED_BUFFER, out.shaderView)) ||
			(uav && !createView(nri::BufferView::STORAGE_STRUCTURED_BUFFER, out.storageView)))
		{
			renderer.DestroyBufferResource(out);
			return false;
		}
		return true;
	}

bool NRISmokeSystem::CreateCompatibilityDescriptors(NRIRenderer& renderer)
{
	return CreateCompatibilityDescriptors(renderer, mCompatibilityStorage,
		mCompatibilityParticleView, mCompatibilityCellView);
}

bool NRISmokeSystem::CreateCompatibilityDescriptors(NRIRenderer& renderer, NRIBufferResource& storage,
	nri::Descriptor*& particleView, nri::Descriptor*& cellView)
{
	if (storage.buffer != nullptr && storage.storageView != nullptr && particleView != nullptr && cellView != nullptr)
		return true;
	DestroyCompatibilityDescriptors(renderer, storage, particleView, cellView);
	const nri::BufferUsageBits storageUsage = NRIResourceFlags(nri::BufferUsageBits::SHADER_RESOURCE_STORAGE,
		nri::BufferUsageBits::SHADER_RESOURCE);
	if (!CreateBuffer(renderer, storage, sizeof(NRISmokeParticleGpu), sizeof(uint32_t),
		storageUsage, nri::MemoryLocation::DEVICE, false, true))
		return false;

	auto createView = [&](uint32_t stride, nri::Descriptor*& descriptor)
	{
		nri::BufferViewDesc view = {};
		view.buffer = storage.buffer;
		view.type = nri::BufferView::STORAGE_STRUCTURED_BUFFER;
		view.offset = 0;
		view.size = nri::WHOLE_SIZE;
		view.structureStride = stride;
		return renderer.mFrameBuffer->mCore.CreateBufferView(view, descriptor) == nri::Result::SUCCESS;
	};
	if (!createView(sizeof(NRISmokeParticleGpu), particleView) ||
		!createView(sizeof(uint32_t) * 2u, cellView))
	{
		DestroyCompatibilityDescriptors(renderer, storage, particleView, cellView);
		return false;
	}
	return true;
}

void NRISmokeSystem::DestroyCompatibilityDescriptors(NRIRenderer& renderer)
{
	DestroyCompatibilityDescriptors(renderer, mCompatibilityStorage,
		mCompatibilityParticleView, mCompatibilityCellView);
}

void NRISmokeSystem::DestroyCompatibilityDescriptors(NRIRenderer& renderer, NRIBufferResource& storage,
	nri::Descriptor*& particleView, nri::Descriptor*& cellView)
{
	if (particleView != nullptr)
		renderer.mFrameBuffer->mCore.DestroyDescriptor(particleView);
	if (cellView != nullptr)
		renderer.mFrameBuffer->mCore.DestroyDescriptor(cellView);
	particleView = nullptr;
	cellView = nullptr;
	renderer.DestroyBufferResource(storage);
}

bool NRISmokeSystem::UploadBytes(NRIRenderer& renderer, NRIBufferResource& upload, const void* data, uint64_t size)
	{
		void* mapped = renderer.mFrameBuffer->mCore.MapBuffer(*upload.buffer, 0, size);
		if (mapped == nullptr)
			return false;
		std::memcpy(mapped, data, (size_t)size);
		renderer.mFrameBuffer->mCore.UnmapBuffer(*upload.buffer);
		return true;
	}

namespace
{
	nri::AccessStage StorageAccess()
	{
		return { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
	}
}

bool NRISmokeSystem::Initialize(NRIRenderer& renderer)
{
	if (mPipelineLayout != nullptr)
		return true;

	nri::DescriptorRangeDesc input = {};
	input.baseRegisterIndex = 0;
	input.descriptorNum = 3;
	input.descriptorType = nri::DescriptorType::STRUCTURED_BUFFER;
	input.shaderStages = nri::StageBits::COMPUTE_SHADER;
	input.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;
	nri::DescriptorRangeDesc buffers = {};
	buffers.baseRegisterIndex = 0;
	buffers.descriptorNum = kSmokeStorageDescriptorCount;
	buffers.descriptorType = nri::DescriptorType::STORAGE_STRUCTURED_BUFFER;
	buffers.shaderStages = nri::StageBits::COMPUTE_SHADER;
	buffers.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;
	nri::DescriptorRangeDesc textures = {};
	textures.baseRegisterIndex = 0;
	textures.descriptorNum = 8;
	textures.descriptorType = nri::DescriptorType::TEXTURE;
	textures.shaderStages = nri::StageBits::COMPUTE_SHADER;
	textures.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;
	nri::DescriptorRangeDesc output = {};
	output.baseRegisterIndex = 0;
	output.descriptorNum = 5;
	output.descriptorType = nri::DescriptorType::STORAGE_TEXTURE;
	output.shaderStages = nri::StageBits::COMPUTE_SHADER;
	output.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;
	nri::DescriptorRangeDesc lights = {};
	lights.baseRegisterIndex = 0;
	lights.descriptorNum = 3;
	lights.descriptorType = nri::DescriptorType::STRUCTURED_BUFFER;
	lights.shaderStages = nri::StageBits::COMPUTE_SHADER;
	lights.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;
	nri::DescriptorRangeDesc filteredSceneRanges[5] = {};
	filteredSceneRanges[0].baseRegisterIndex = 0;
	filteredSceneRanges[0].descriptorNum = kSmokeFilteredSceneBufferCount;
	filteredSceneRanges[0].descriptorType = nri::DescriptorType::STRUCTURED_BUFFER;
	filteredSceneRanges[0].shaderStages = nri::StageBits::COMPUTE_SHADER;
	filteredSceneRanges[0].flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;
	filteredSceneRanges[1].baseRegisterIndex = 8;
	filteredSceneRanges[1].descriptorNum = kSmokeExtendedSceneBufferCount;
	filteredSceneRanges[1].descriptorType = nri::DescriptorType::STRUCTURED_BUFFER;
	filteredSceneRanges[1].shaderStages = nri::StageBits::COMPUTE_SHADER;
	filteredSceneRanges[1].flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;
	filteredSceneRanges[2].baseRegisterIndex = 18;
	filteredSceneRanges[2].descriptorNum = 514;
	filteredSceneRanges[2].descriptorType = nri::DescriptorType::TEXTURE;
	filteredSceneRanges[2].shaderStages = nri::StageBits::COMPUTE_SHADER;
	filteredSceneRanges[2].flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;
	filteredSceneRanges[3].baseRegisterIndex = 0;
	filteredSceneRanges[3].descriptorNum = 3;
	filteredSceneRanges[3].descriptorType = nri::DescriptorType::SAMPLER;
	filteredSceneRanges[3].shaderStages = nri::StageBits::COMPUTE_SHADER;
	filteredSceneRanges[3].flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;
	filteredSceneRanges[4].baseRegisterIndex = 532;
	filteredSceneRanges[4].descriptorNum = 1;
	filteredSceneRanges[4].descriptorType = nri::DescriptorType::ACCELERATION_STRUCTURE;
	filteredSceneRanges[4].shaderStages = nri::StageBits::COMPUTE_SHADER;
	filteredSceneRanges[4].flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;
	nri::DescriptorSetDesc sets[6] = {};
	nri::DescriptorRangeDesc* ranges[] = { &input, &buffers, &textures, &output, &lights };
	for (uint32_t i = 0; i < 5; ++i)
	{
		sets[i].registerSpace = i;
		sets[i].ranges = ranges[i];
		sets[i].rangeNum = 1;
		sets[i].flags = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;
	}
	sets[5].registerSpace = 6;
	sets[5].ranges = filteredSceneRanges;
	sets[5].rangeNum = 5;
	sets[5].flags = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;
	nri::RootConstantDesc root = {};
	root.registerIndex = 0;
	root.size = sizeof(NRISmokeConstants);
	root.shaderStages = nri::StageBits::COMPUTE_SHADER;
	nri::PipelineLayoutDesc layout = {};
	layout.rootRegisterSpace = 5;
	layout.rootConstants = &root;
	layout.rootConstantNum = 1;
	layout.descriptorSets = sets;
	layout.descriptorSetNum = 6;
	layout.shaderStages = nri::StageBits::COMPUTE_SHADER;
	if (renderer.mFrameBuffer->mCore.CreatePipelineLayout(*renderer.mFrameBuffer->mDevice, layout, mPipelineLayout) != nri::Result::SUCCESS)
		return false;

	const bool d3d12 = renderer.mFrameBuffer->GetSelectedAPI() == nri::GraphicsAPI::D3D12;
	for (uint32_t i = 0; i < (uint32_t)mPipelines.size(); ++i)
	{
		std::vector<uint8_t> blob;
		const std::string file = std::string(kSmokePipelineNames[i]) + ".cs." + (d3d12 ? "dxil" : "spirv");
		if (!renderer.mFrameBuffer->LoadShaderBlob(file.c_str(), blob))
			return false;
		nri::ShaderDesc shader = {};
		shader.stage = nri::StageBits::COMPUTE_SHADER;
		shader.bytecode = blob.data();
		shader.size = blob.size();
		shader.entryPointName = "main";
		nri::ComputePipelineDesc pipeline = {};
		pipeline.pipelineLayout = mPipelineLayout;
		pipeline.shader = shader;
		if (renderer.mFrameBuffer->mCore.CreateComputePipeline(*renderer.mFrameBuffer->mDevice, pipeline, mPipelines[i]) != nri::Result::SUCCESS)
			return false;
	}
	// The grid-lighting owner shares this layout so it can consume the already
	// resident smoke grid and scene tables without becoming another scene owner.
	// Failure remains local: particle and legacy-grid emissive lighting stay usable.
	mGridLighting.Initialize(BuildGridServices(renderer), mPipelineLayout);

	const uint32_t queued = std::max(1u, (uint32_t)renderer.mFrameBuffer->mQueuedFrames.size());
	mCommandSlots.resize(queued);
	for (CommandSlot& slot : mCommandSlots)
	{
		if (renderer.mFrameBuffer->mCore.AllocateDescriptorSets(*renderer.mFrameBuffer->mDescriptorPool, *mPipelineLayout, 0, &slot.inputSet, 1, 0) != nri::Result::SUCCESS ||
			renderer.mFrameBuffer->mCore.AllocateDescriptorSets(*renderer.mFrameBuffer->mDescriptorPool, *mPipelineLayout, 1, &slot.bufferSet, 1, 0) != nri::Result::SUCCESS ||
			renderer.mFrameBuffer->mCore.AllocateDescriptorSets(*renderer.mFrameBuffer->mDescriptorPool, *mPipelineLayout, 2, &slot.textureSet, 1, 0) != nri::Result::SUCCESS ||
			renderer.mFrameBuffer->mCore.AllocateDescriptorSets(*renderer.mFrameBuffer->mDescriptorPool, *mPipelineLayout, 3, &slot.outputSet, 1, 0) != nri::Result::SUCCESS ||
			renderer.mFrameBuffer->mCore.AllocateDescriptorSets(*renderer.mFrameBuffer->mDescriptorPool, *mPipelineLayout, 4, &slot.lightSet, 1, 0) != nri::Result::SUCCESS)
			return false;
		if (renderer.mFrameBuffer->mCore.AllocateDescriptorSets(*renderer.mFrameBuffer->mDescriptorPool, *mPipelineLayout, 5, &slot.filteredSceneSet, 1, 0) != nri::Result::SUCCESS)
			return false;
	}
	return true;
}

bool NRISmokeSystem::EnsureResources(NRIRenderer& renderer, uint32_t representation)
{
	if (mStyles.empty())
		mStyles.emplace_back();
	const uint32_t fw = (renderer.mRenderWidth + mSettings.froxelPixelSize - 1) / mSettings.froxelPixelSize;
	const uint32_t fh = (renderer.mRenderHeight + mSettings.froxelPixelSize - 1) / mSettings.froxelPixelSize;
	const bool particlePayload = representation != 1u;
	// Analytic carriers use the profile-selected receiver emissive path even
	// while fine-grid smoke uses world-field emissive lighting. Keep its fixed
	// reservoir storage resident so a one-shot event never triggers allocation.
	const bool legacyEmissiveFull = true;
	const bool commandSlotsReady = std::all_of(mCommandSlots.begin(), mCommandSlots.end(), [](const CommandSlot& slot)
	{
		return slot.upload.buffer != nullptr && slot.device.buffer != nullptr &&
			slot.analyticUpload.buffer != nullptr && slot.analyticDevice.buffer != nullptr && slot.styleUpload.buffer != nullptr &&
			slot.controlReadback.buffer != nullptr;
	});
	const bool selectedPayloadReady = particlePayload ?
		(mParticles.buffer != nullptr && mReferenceNext.buffer != nullptr && mParticleDirectionalVisibility.buffer != nullptr &&
			mResourceParticleCapacity == mSettings.particleCapacity) :
		(mParticles.buffer == nullptr && mReferenceNext.buffer == nullptr && mParticleDirectionalVisibility.buffer == nullptr &&
			mCompatibilityStorage.buffer != nullptr && mCompatibilityParticleView != nullptr && mCompatibilityCellView != nullptr &&
			mResourceParticleCapacity == 0u);
	const bool persistentReady = mControl.buffer != nullptr && mStyleBuffer.buffer != nullptr && commandSlotsReady &&
		mResourceParticlePayload == particlePayload && selectedPayloadReady && mResourceStyleCapacity == (uint32_t)mStyles.size();
	const bool selectedViewReady = particlePayload ?
		(mFineCells.buffer != nullptr && mWideCells.buffer != nullptr && mGlobalDepthCells.buffer != nullptr) :
		(mFineCells.buffer == nullptr && mWideCells.buffer == nullptr && mGlobalDepthCells.buffer == nullptr);
	const bool viewReady = selectedViewReady && mFroxelMedium.buffer != nullptr && mDirectCurrent.buffer != nullptr && mDirectHistory.buffer != nullptr &&
		mAnalyticTileHeaders.buffer != nullptr && mAnalyticTileIndices.buffer != nullptr && mAnalyticFroxelMedium.buffer != nullptr &&
		mAnalyticEmissiveA.buffer != nullptr && mAnalyticEmissiveB.buffer != nullptr && mResourceFroxelWidth == fw &&
		mResourceFroxelHeight == fh && mResourceFroxelDepth == mSettings.froxelDepth &&
		mResourceLegacyEmissiveFull == legacyEmissiveFull;
	if (persistentReady && viewReady)
	{
		UpdateResourceStatus();
		return true;
	}

	renderer.WaitForCommandsTracked();
	if (!persistentReady)
		DestroyResources(renderer);
	else
		DestroyViewResources(renderer);
	const nri::BufferUsageBits storage = NRIResourceFlags(nri::BufferUsageBits::SHADER_RESOURCE_STORAGE, nri::BufferUsageBits::SHADER_RESOURCE);
	const nri::BufferUsageBits copyDevice = nri::BufferUsageBits::SHADER_RESOURCE;
	const uint64_t columns = (uint64_t)fw * fh;
	const uint64_t froxels = columns * mSettings.froxelDepth;
	const uint64_t analyticTiles = ((uint64_t)fw + 3u) / 4u * (((uint64_t)fh + 3u) / 4u);
	const uint64_t legacyEmissiveRecords = legacyEmissiveFull ? froxels : 1u;
	const uint64_t wideCells = (uint64_t)kWideCellCount * mSettings.froxelDepth;
	if (!persistentReady)
	{
		if (!CreateBuffer(renderer, mControl, sizeof(NRISmokeControlGpu), sizeof(NRISmokeControlGpu), storage, nri::MemoryLocation::DEVICE, false, true) ||
			!CreateBuffer(renderer, mStyleBuffer, std::max<size_t>(1, mStyles.size()) * sizeof(NRISmokeStyleGpu), sizeof(NRISmokeStyleGpu), copyDevice, nri::MemoryLocation::DEVICE, true, false) ||
			(particlePayload &&
				(!CreateBuffer(renderer, mParticles, (uint64_t)mSettings.particleCapacity * sizeof(NRISmokeParticleGpu), sizeof(NRISmokeParticleGpu), storage, nri::MemoryLocation::DEVICE, false, true) ||
				 !CreateBuffer(renderer, mReferenceNext, (uint64_t)mSettings.particleCapacity * kMaximumReferencesPerParticle * sizeof(uint32_t), sizeof(uint32_t), storage, nri::MemoryLocation::DEVICE, false, true) ||
				 !CreateBuffer(renderer, mParticleDirectionalVisibility, (uint64_t)mSettings.particleCapacity * kDirectionalProbesPerParticle * sizeof(float), sizeof(float), storage, nri::MemoryLocation::DEVICE, false, true))) ||
			(!particlePayload && !CreateCompatibilityDescriptors(renderer)))
		{
			DestroyResources(renderer);
			return false;
		}
		for (CommandSlot& slot : mCommandSlots)
		{
			if (!CreateBuffer(renderer, slot.upload, kMaxCommands * sizeof(NRISmokeInjectionCommandGpu), sizeof(NRISmokeInjectionCommandGpu), nri::BufferUsageBits::NONE, nri::MemoryLocation::HOST_UPLOAD, false, false) ||
				!CreateBuffer(renderer, slot.device, kMaxCommands * sizeof(NRISmokeInjectionCommandGpu), sizeof(NRISmokeInjectionCommandGpu), copyDevice, nri::MemoryLocation::DEVICE, true, false) ||
				!CreateBuffer(renderer, slot.analyticUpload, NRISmokeAnalyticCarriers::FixedCarrierCapacity * sizeof(NRISmokeAnalyticCarrierGpu), sizeof(NRISmokeAnalyticCarrierGpu), nri::BufferUsageBits::NONE, nri::MemoryLocation::HOST_UPLOAD, false, false) ||
				!CreateBuffer(renderer, slot.analyticDevice, NRISmokeAnalyticCarriers::FixedCarrierCapacity * sizeof(NRISmokeAnalyticCarrierGpu), sizeof(NRISmokeAnalyticCarrierGpu), copyDevice, nri::MemoryLocation::DEVICE, true, false) ||
				!CreateBuffer(renderer, slot.styleUpload, std::max<size_t>(1, mStyles.size()) * sizeof(NRISmokeStyleGpu), sizeof(NRISmokeStyleGpu), nri::BufferUsageBits::NONE, nri::MemoryLocation::HOST_UPLOAD, false, false) ||
				!CreateBuffer(renderer, slot.controlReadback, sizeof(NRISmokeControlGpu), sizeof(NRISmokeControlGpu), nri::BufferUsageBits::NONE, nri::MemoryLocation::HOST_READBACK, false, false))
			{
				DestroyResources(renderer);
				return false;
			}
		}
		mResourceParticleCapacity = particlePayload ? mSettings.particleCapacity : 0u;
		mResourceStyleCapacity = (uint32_t)mStyles.size();
		mResourceParticlePayload = particlePayload;
		mNeedsClear = true;
	}
	if ((particlePayload &&
			(!CreateBuffer(renderer, mFineCells, froxels * sizeof(uint32_t) * 2u, sizeof(uint32_t) * 2u, storage, nri::MemoryLocation::DEVICE, false, true) ||
			 !CreateBuffer(renderer, mWideCells, wideCells * sizeof(uint32_t) * 2u, sizeof(uint32_t) * 2u, storage, nri::MemoryLocation::DEVICE, false, true) ||
			 !CreateBuffer(renderer, mGlobalDepthCells, (uint64_t)mSettings.froxelDepth * sizeof(uint32_t) * 2u, sizeof(uint32_t) * 2u, storage, nri::MemoryLocation::DEVICE, false, true))) ||
		!CreateBuffer(renderer, mFroxelMedium, froxels * 16, 16, storage, nri::MemoryLocation::DEVICE, false, true) ||
		!CreateBuffer(renderer, mFroxelIntegrated, froxels * 16, 16, storage, nri::MemoryLocation::DEVICE, false, true) ||
		!CreateBuffer(renderer, mFroxelPhase, froxels * 16, 16, storage, nri::MemoryLocation::DEVICE, false, true) ||
		!CreateBuffer(renderer, mFroxelSource, froxels * 16, 16, storage, nri::MemoryLocation::DEVICE, false, true) ||
		!CreateBuffer(renderer, mOccupiedFroxelIndices, froxels * sizeof(uint32_t), sizeof(uint32_t), storage, nri::MemoryLocation::DEVICE, false, true) ||
		!CreateBuffer(renderer, mIndirectHistory, froxels * sizeof(NRISmokeIndirectCacheGpu), sizeof(NRISmokeIndirectCacheGpu), storage, nri::MemoryLocation::DEVICE, false, true) ||
		!CreateBuffer(renderer, mIndirectScratch, froxels * sizeof(NRISmokeIndirectCacheGpu), sizeof(NRISmokeIndirectCacheGpu), storage, nri::MemoryLocation::DEVICE, false, true) ||
		!CreateBuffer(renderer, mEmissiveCurrent, legacyEmissiveRecords * sizeof(NRISmokeEmissiveStorageGpu), sizeof(NRISmokeEmissiveStorageGpu), storage, nri::MemoryLocation::DEVICE, false, true) ||
		!CreateBuffer(renderer, mEmissiveTemporal, legacyEmissiveRecords * sizeof(NRISmokeEmissiveStorageGpu), sizeof(NRISmokeEmissiveStorageGpu), storage, nri::MemoryLocation::DEVICE, false, true) ||
		!CreateBuffer(renderer, mEmissiveHistory, legacyEmissiveRecords * sizeof(NRISmokeEmissiveStorageGpu), sizeof(NRISmokeEmissiveStorageGpu), storage, nri::MemoryLocation::DEVICE, false, true) ||
		!CreateBuffer(renderer, mDirectCurrent, froxels * sizeof(NRISmokeDirectCacheGpu), sizeof(NRISmokeDirectCacheGpu), storage, nri::MemoryLocation::DEVICE, false, true) ||
		!CreateBuffer(renderer, mDirectHistory, froxels * sizeof(NRISmokeDirectCacheGpu), sizeof(NRISmokeDirectCacheGpu), storage, nri::MemoryLocation::DEVICE, false, true) ||
		!CreateBuffer(renderer, mAnalyticTileHeaders, analyticTiles * sizeof(uint32_t) * 2u, sizeof(uint32_t) * 2u, storage, nri::MemoryLocation::DEVICE, false, true) ||
		!CreateBuffer(renderer, mAnalyticTileIndices, analyticTiles * NRISmokeAnalyticCarriers::FixedCarrierCapacity * sizeof(uint32_t), sizeof(uint32_t), storage, nri::MemoryLocation::DEVICE, false, true) ||
		!CreateBuffer(renderer, mAnalyticFroxelMedium, froxels * sizeof(float) * 4u, sizeof(float) * 4u, storage, nri::MemoryLocation::DEVICE, false, true) ||
				!CreateBuffer(renderer, mAnalyticEmissiveA, NRISmokeAnalyticCarriers::FixedCarrierCapacity * 2u * sizeof(NRISmokeAnalyticEmissiveStorageGpu), sizeof(NRISmokeAnalyticEmissiveStorageGpu), storage, nri::MemoryLocation::DEVICE, false, true) ||
				!CreateBuffer(renderer, mAnalyticEmissiveB, NRISmokeAnalyticCarriers::FixedCarrierCapacity * 2u * sizeof(NRISmokeAnalyticEmissiveStorageGpu), sizeof(NRISmokeAnalyticEmissiveStorageGpu), storage, nri::MemoryLocation::DEVICE, false, true))
	{
		DestroyViewResources(renderer);
		return false;
	}
	mResourceFroxelWidth = fw;
	mResourceFroxelHeight = fh;
	mResourceFroxelDepth = mSettings.froxelDepth;
	mResourceLegacyEmissiveFull = legacyEmissiveFull;
	mViewResourcesInitialized = false;
	mIndirectHistoryValid = false;
	mEmissiveHistoryValid = false;
	mStatus.emissiveHistoryValid = false;
	mLastEmissiveFrame = UINT32_MAX;
	mLastEmissiveRepresentation = UINT32_MAX;
	mLastEmissiveLaneCount = 0;
	mLastEmissiveLightMode = 0;
	mLastEmissiveVisibilityBackend = 0;
	mDirectHistoryValid = false;
	mLastDirectFrame = UINT32_MAX;
	mStatus.directHistoryValid = false;
	mStatus.directHistoryResetReason = "view-resources";
	UpdateResourceStatus();
	return true;
}

void NRISmokeSystem::UpdateResourceStatus()
{
	mStatus.froxelWidth = mResourceFroxelWidth;
	mStatus.froxelHeight = mResourceFroxelHeight;
	mStatus.froxelDepth = mResourceFroxelDepth;
	mStatus.particleCapacity = mResourceParticleCapacity;
	mStatus.particlePayloadBytes = mParticles.memorySize + mReferenceNext.memorySize + mParticleDirectionalVisibility.memorySize +
		mFineCells.memorySize + mWideCells.memorySize + mGlobalDepthCells.memorySize;
	mStatus.descriptorSentinelBytes = mCompatibilityStorage.memorySize;
	mStatus.residentBytes = mParticles.memorySize + mControl.memorySize + mReferenceNext.memorySize + mParticleDirectionalVisibility.memorySize + mFineCells.memorySize +
		mWideCells.memorySize + mGlobalDepthCells.memorySize + mFroxelMedium.memorySize + mFroxelIntegrated.memorySize +
		mFroxelPhase.memorySize + mFroxelSource.memorySize + mOccupiedFroxelIndices.memorySize + mIndirectHistory.memorySize +
		mIndirectScratch.memorySize + mEmissiveCurrent.memorySize + mEmissiveTemporal.memorySize + mEmissiveHistory.memorySize +
		mDirectCurrent.memorySize + mDirectHistory.memorySize + mAnalyticTileHeaders.memorySize +
		mAnalyticTileIndices.memorySize + mAnalyticFroxelMedium.memorySize + mAnalyticEmissiveA.memorySize +
		mAnalyticEmissiveB.memorySize + mStyleBuffer.memorySize + mCompatibilityStorage.memorySize;
	mStatus.indirectCacheBytes = mIndirectHistory.memorySize + mIndirectScratch.memorySize;
	mStatus.emissiveReservoirBytes = mEmissiveCurrent.memorySize + mEmissiveTemporal.memorySize + mEmissiveHistory.memorySize;
	mStatus.directHistoryBytes = mDirectCurrent.memorySize + mDirectHistory.memorySize;
	for (const CommandSlot& slot : mCommandSlots)
		mStatus.residentBytes += slot.upload.memorySize + slot.device.memorySize + slot.analyticUpload.memorySize +
			slot.analyticDevice.memorySize + slot.styleUpload.memorySize + slot.controlReadback.memorySize;
}

void NRISmokeSystem::PublishAuthorityStatus(const char* preparation, const char* fallbackOverride)
{
	const NRISmokeAuthoritySnapshot& authority = mAuthority.GetSnapshot();
	mStatus.representationEffective = authority.effectiveRepresentation;
	mStatus.representationFallback = fallbackOverride != nullptr ? fallbackOverride : authority.fallback;
	mStatus.authority = NRISmokeAuthority::ModeName(authority.mode);
	mStatus.authorityReason = authority.reason;
	mStatus.authorityPreparation = preparation != nullptr ? preparation : "none";
	mStatus.authorityOperational = authority.operational;
	mStatus.authorityTransitionSerial = authority.transitionSerial;
	mStatus.authorityTransitionFrame = authority.transitionFrame;
	if (!authority.operational)
	{
		mStatus.representationEffective = 0u;
		mStatus.particleSimulationDispatches = 0u;
		mStatus.gridSimulationDispatches = 0u;
		mStatus.particleOpticalDispatches = 0u;
		mStatus.gridOpticalDispatches = 0u;
		mStatus.particleCommandsRouted = 0u;
		mStatus.gridCommandsRouted = 0u;
	}
}

bool NRISmokeSystem::RebuildAuthorityResourcesTransactional(NRIRenderer& renderer, uint32_t representation)
{
	struct Replacement
	{
		NRIBufferResource particles;
		NRIBufferResource referenceNext;
		NRIBufferResource directionalVisibility;
		NRIBufferResource compatibility;
		nri::Descriptor* compatibilityParticleView = nullptr;
		nri::Descriptor* compatibilityCellView = nullptr;
		NRIBufferResource fineCells;
		NRIBufferResource wideCells;
		NRIBufferResource globalDepthCells;
		NRIBufferResource froxelMedium;
		NRIBufferResource froxelIntegrated;
		NRIBufferResource froxelPhase;
		NRIBufferResource froxelSource;
		NRIBufferResource occupiedFroxelIndices;
		NRIBufferResource indirectHistory;
		NRIBufferResource indirectScratch;
		NRIBufferResource emissiveCurrent;
		NRIBufferResource emissiveTemporal;
		NRIBufferResource emissiveHistory;
		NRIBufferResource directCurrent;
		NRIBufferResource directHistory;
		NRIBufferResource analyticTileHeaders;
		NRIBufferResource analyticTileIndices;
		NRIBufferResource analyticFroxelMedium;
		NRIBufferResource analyticEmissiveA;
		NRIBufferResource analyticEmissiveB;
	};

	Replacement replacement = {};
	auto destroyReplacement = [&]()
	{
		DestroyCompatibilityDescriptors(renderer, replacement.compatibility,
			replacement.compatibilityParticleView, replacement.compatibilityCellView);
		NRIBufferResource* resources[] = { &replacement.particles, &replacement.referenceNext,
			&replacement.directionalVisibility, &replacement.fineCells, &replacement.wideCells,
			&replacement.globalDepthCells, &replacement.froxelMedium, &replacement.froxelIntegrated,
			&replacement.froxelPhase, &replacement.froxelSource, &replacement.occupiedFroxelIndices,
			&replacement.indirectHistory, &replacement.indirectScratch, &replacement.emissiveCurrent,
			&replacement.emissiveTemporal, &replacement.emissiveHistory, &replacement.directCurrent,
			&replacement.directHistory, &replacement.analyticTileHeaders,
			&replacement.analyticTileIndices, &replacement.analyticFroxelMedium,
			&replacement.analyticEmissiveA, &replacement.analyticEmissiveB };
		for (NRIBufferResource* resource : resources)
			renderer.DestroyBufferResource(*resource);
	};
	auto take = [](NRIBufferResource& destination, NRIBufferResource& source)
	{
		destination = source;
		source = {};
	};

	const bool particlePayload = representation != 1u;
	const uint32_t fw = (renderer.mRenderWidth + mSettings.froxelPixelSize - 1u) / mSettings.froxelPixelSize;
	const uint32_t fh = (renderer.mRenderHeight + mSettings.froxelPixelSize - 1u) / mSettings.froxelPixelSize;
	const uint64_t columns = (uint64_t)fw * fh;
	const uint64_t froxels = columns * mSettings.froxelDepth;
	const uint64_t analyticTiles = ((uint64_t)fw + 3u) / 4u * (((uint64_t)fh + 3u) / 4u);
	const uint64_t wideCells = (uint64_t)kWideCellCount * mSettings.froxelDepth;
	const uint64_t legacyEmissiveRecords = froxels;
	const nri::BufferUsageBits storage = NRIResourceFlags(nri::BufferUsageBits::SHADER_RESOURCE_STORAGE,
		nri::BufferUsageBits::SHADER_RESOURCE);
	bool created = true;
	if (particlePayload)
	{
		created = CreateBuffer(renderer, replacement.particles,
			(uint64_t)mSettings.particleCapacity * sizeof(NRISmokeParticleGpu), sizeof(NRISmokeParticleGpu),
			storage, nri::MemoryLocation::DEVICE, false, true) &&
			CreateBuffer(renderer, replacement.referenceNext,
				(uint64_t)mSettings.particleCapacity * kMaximumReferencesPerParticle * sizeof(uint32_t), sizeof(uint32_t),
				storage, nri::MemoryLocation::DEVICE, false, true) &&
			CreateBuffer(renderer, replacement.directionalVisibility,
				(uint64_t)mSettings.particleCapacity * kDirectionalProbesPerParticle * sizeof(float), sizeof(float),
				storage, nri::MemoryLocation::DEVICE, false, true);
	}
	else
	{
		created = CreateCompatibilityDescriptors(renderer, replacement.compatibility,
			replacement.compatibilityParticleView, replacement.compatibilityCellView);
	}
	if (created && particlePayload)
	{
		created = CreateBuffer(renderer, replacement.fineCells, froxels * sizeof(uint32_t) * 2u,
			sizeof(uint32_t) * 2u, storage, nri::MemoryLocation::DEVICE, false, true) &&
			CreateBuffer(renderer, replacement.wideCells, wideCells * sizeof(uint32_t) * 2u,
				sizeof(uint32_t) * 2u, storage, nri::MemoryLocation::DEVICE, false, true) &&
			CreateBuffer(renderer, replacement.globalDepthCells, (uint64_t)mSettings.froxelDepth * sizeof(uint32_t) * 2u,
				sizeof(uint32_t) * 2u, storage, nri::MemoryLocation::DEVICE, false, true);
	}
	if (created)
	{
		created = CreateBuffer(renderer, replacement.froxelMedium, froxels * 16u, 16u, storage, nri::MemoryLocation::DEVICE, false, true) &&
			CreateBuffer(renderer, replacement.froxelIntegrated, froxels * 16u, 16u, storage, nri::MemoryLocation::DEVICE, false, true) &&
			CreateBuffer(renderer, replacement.froxelPhase, froxels * 16u, 16u, storage, nri::MemoryLocation::DEVICE, false, true) &&
			CreateBuffer(renderer, replacement.froxelSource, froxels * 16u, 16u, storage, nri::MemoryLocation::DEVICE, false, true) &&
			CreateBuffer(renderer, replacement.occupiedFroxelIndices, froxels * sizeof(uint32_t), sizeof(uint32_t), storage, nri::MemoryLocation::DEVICE, false, true) &&
			CreateBuffer(renderer, replacement.indirectHistory, froxels * sizeof(NRISmokeIndirectCacheGpu), sizeof(NRISmokeIndirectCacheGpu), storage, nri::MemoryLocation::DEVICE, false, true) &&
			CreateBuffer(renderer, replacement.indirectScratch, froxels * sizeof(NRISmokeIndirectCacheGpu), sizeof(NRISmokeIndirectCacheGpu), storage, nri::MemoryLocation::DEVICE, false, true) &&
			CreateBuffer(renderer, replacement.emissiveCurrent, legacyEmissiveRecords * sizeof(NRISmokeEmissiveStorageGpu), sizeof(NRISmokeEmissiveStorageGpu), storage, nri::MemoryLocation::DEVICE, false, true) &&
			CreateBuffer(renderer, replacement.emissiveTemporal, legacyEmissiveRecords * sizeof(NRISmokeEmissiveStorageGpu), sizeof(NRISmokeEmissiveStorageGpu), storage, nri::MemoryLocation::DEVICE, false, true) &&
			CreateBuffer(renderer, replacement.emissiveHistory, legacyEmissiveRecords * sizeof(NRISmokeEmissiveStorageGpu), sizeof(NRISmokeEmissiveStorageGpu), storage, nri::MemoryLocation::DEVICE, false, true) &&
			CreateBuffer(renderer, replacement.directCurrent, froxels * sizeof(NRISmokeDirectCacheGpu), sizeof(NRISmokeDirectCacheGpu), storage, nri::MemoryLocation::DEVICE, false, true) &&
			CreateBuffer(renderer, replacement.directHistory, froxels * sizeof(NRISmokeDirectCacheGpu), sizeof(NRISmokeDirectCacheGpu), storage, nri::MemoryLocation::DEVICE, false, true) &&
			CreateBuffer(renderer, replacement.analyticTileHeaders, analyticTiles * sizeof(uint32_t) * 2u, sizeof(uint32_t) * 2u, storage, nri::MemoryLocation::DEVICE, false, true) &&
			CreateBuffer(renderer, replacement.analyticTileIndices, analyticTiles * NRISmokeAnalyticCarriers::FixedCarrierCapacity * sizeof(uint32_t), sizeof(uint32_t), storage, nri::MemoryLocation::DEVICE, false, true) &&
			CreateBuffer(renderer, replacement.analyticFroxelMedium, froxels * sizeof(float) * 4u, sizeof(float) * 4u, storage, nri::MemoryLocation::DEVICE, false, true) &&
			CreateBuffer(renderer, replacement.analyticEmissiveA, NRISmokeAnalyticCarriers::FixedCarrierCapacity * 2u * sizeof(NRISmokeAnalyticEmissiveStorageGpu), sizeof(NRISmokeAnalyticEmissiveStorageGpu), storage, nri::MemoryLocation::DEVICE, false, true) &&
			CreateBuffer(renderer, replacement.analyticEmissiveB, NRISmokeAnalyticCarriers::FixedCarrierCapacity * 2u * sizeof(NRISmokeAnalyticEmissiveStorageGpu), sizeof(NRISmokeAnalyticEmissiveStorageGpu), storage, nri::MemoryLocation::DEVICE, false, true);
	}
	if (!created)
	{
		destroyReplacement();
		return false;
	}

	// The replacement is complete before the committed authority is touched.
	// This is the transactional boundary: allocation failure above leaves the
	// old backend and its world field entirely intact.
	renderer.WaitForCommandsTracked();
	DestroyViewResources(renderer);
	renderer.DestroyBufferResource(mParticles);
	renderer.DestroyBufferResource(mReferenceNext);
	renderer.DestroyBufferResource(mParticleDirectionalVisibility);
	DestroyCompatibilityDescriptors(renderer);
	take(mParticles, replacement.particles);
	take(mReferenceNext, replacement.referenceNext);
	take(mParticleDirectionalVisibility, replacement.directionalVisibility);
	take(mCompatibilityStorage, replacement.compatibility);
	mCompatibilityParticleView = replacement.compatibilityParticleView;
	mCompatibilityCellView = replacement.compatibilityCellView;
	replacement.compatibilityParticleView = nullptr;
	replacement.compatibilityCellView = nullptr;
	take(mFineCells, replacement.fineCells);
	take(mWideCells, replacement.wideCells);
	take(mGlobalDepthCells, replacement.globalDepthCells);
	take(mFroxelMedium, replacement.froxelMedium);
	take(mFroxelIntegrated, replacement.froxelIntegrated);
	take(mFroxelPhase, replacement.froxelPhase);
	take(mFroxelSource, replacement.froxelSource);
	take(mOccupiedFroxelIndices, replacement.occupiedFroxelIndices);
	take(mIndirectHistory, replacement.indirectHistory);
	take(mIndirectScratch, replacement.indirectScratch);
	take(mEmissiveCurrent, replacement.emissiveCurrent);
	take(mEmissiveTemporal, replacement.emissiveTemporal);
	take(mEmissiveHistory, replacement.emissiveHistory);
	take(mDirectCurrent, replacement.directCurrent);
	take(mDirectHistory, replacement.directHistory);
	take(mAnalyticTileHeaders, replacement.analyticTileHeaders);
	take(mAnalyticTileIndices, replacement.analyticTileIndices);
	take(mAnalyticFroxelMedium, replacement.analyticFroxelMedium);
	take(mAnalyticEmissiveA, replacement.analyticEmissiveA);
	take(mAnalyticEmissiveB, replacement.analyticEmissiveB);
	destroyReplacement();

	mResourceParticleCapacity = particlePayload ? mSettings.particleCapacity : 0u;
	mResourceParticlePayload = particlePayload;
	mResourceFroxelWidth = fw;
	mResourceFroxelHeight = fh;
	mResourceFroxelDepth = mSettings.froxelDepth;
	mResourceLegacyEmissiveFull = true;
	mParticleResourcesInitialized = false;
	mViewResourcesInitialized = false;
	mNeedsClear = true;
	UpdateResourceStatus();
	return true;
}

void NRISmokeSystem::AppendSyntheticCommand(NRIRenderer& renderer)
{
	if (!mSyntheticRequested)
		return;
	mSyntheticRequested = false;
	NRISmokeInjectionCommandGpu command = {};
	for (uint32_t i = 0; i < 3; ++i)
		command.position[i] = renderer.mCurrentCameraPos[i] + renderer.mCurrentCameraForward[i] * 96.0f;
	// Keep the test conspicuous without manufacturing a large overlapping
	// particle workload when the command is invoked repeatedly.
	command.count = 64;
	command.spawnRadius = 10.0f;
	command.densityScale = 4.0f;
	command.radiusScale = 1.5f;
	command.serial = mNextCommandSerial++;
	command.epoch = mStatus.simulationEpoch;
	command.sourceId = NRIMakeSmokeSourceId("diagnostic", "runtime", "synthetic");
	command.sourceMetadata = NRIPackSmokeSourceMetadata(NRISmokeInjectionSourceClass::Diagnostic);
	mPendingCommands.push_back(command);
	mPendingPulseEnqueueInfo.push_back({});
}

bool NRISmokeSystem::PrepareFrame(NRIRenderer& renderer, bool mainViewEligible, const TArray<PathTracingWeaponLightEvent>& weaponEvents)
{
	mSettings = BuildNRISmokeSettingsFromCVars();
	const NRISmokeWorkSchedulerSnapshot& workSchedule =
		mWorkScheduler.Resolve(mSettings.workProfile, mSettings.maxSubsteps);
	if (workSchedule.table.froxelPixelSize != NRISmokeWorkTable::Unrestricted)
		mSettings.froxelPixelSize = workSchedule.table.froxelPixelSize;
	if (workSchedule.table.froxelDepth != NRISmokeWorkTable::Unrestricted)
		mSettings.froxelDepth = workSchedule.table.froxelDepth;
	if (workSchedule.table.emissiveLights != NRISmokeWorkTable::Unrestricted)
		mSettings.emissiveLights = workSchedule.table.emissiveLights != 0u;
	if (workSchedule.table.emissiveBackend != NRISmokeWorkTable::Unrestricted)
		mSettings.emissiveBackend = workSchedule.table.emissiveBackend;
	if (workSchedule.table.lightSamples != NRISmokeWorkTable::Unrestricted)
		mSettings.lightSamples = workSchedule.table.lightSamples;
	if (workSchedule.table.maximumLightCandidates != NRISmokeWorkTable::Unrestricted)
		mSettings.maxLightCandidates = workSchedule.table.maximumLightCandidates;
	mStatus.enabled = mSettings.enabled;
	mStatus.dlrrModeRequested = mSettings.dlrrMode;
	mStatus.mainViewEligible = mainViewEligible;
	mStatus.preparedFrame = renderer.mFrameIndex;
	// Volume layers are frame-local products.  Do not let a route that was
	// skipped this frame expose last frame's reactive mask to TAA or an
	// upscaler.
	mStatus.volumeResolvedSlot = UINT32_MAX;
	mStatus.volumeMetaSlot = UINT32_MAX;
	mStatus.dlrrModeEffective = 0u;
	mStatus.representationRequested = mSettings.representation;
	if (!mSettings.enabled)
	{
		mAuthority.Disable(mSettings.representation, renderer.mFrameIndex, "smoke-disabled");
		mStatus.gridReady = false;
		PublishAuthorityStatus("retained-disabled", "resources-retained");
		return true;
	}
	if (!mainViewEligible || mLastPreparedFrame == renderer.mFrameIndex)
		return true;
	mLastPreparedFrame = renderer.mFrameIndex;
	bool gridLayoutInvalidated = false;
	if (mSettings.representation != 0u)
	{
		const bool layoutChanged = mGridLayoutTracked &&
			(mLastGridBrickCapacity != mSettings.gridBrickCapacity || mLastGridCellSize != mSettings.gridCellSize);
		mLastGridBrickCapacity = mSettings.gridBrickCapacity;
		mLastGridCellSize = mSettings.gridCellSize;
		mGridLayoutTracked = true;
		if (layoutChanged)
		{
			gridLayoutInvalidated = true;
			Reset("grid-layout");
			mStatus.preparedFrame = renderer.mFrameIndex;
			mLastPreparedFrame = renderer.mFrameIndex;
		}
	}
	if (!mCommandSlots.empty())
	{
		CommandSlot& completedSlot = mCommandSlots[std::min(renderer.mFrameBuffer->mCurrentQueuedFrameIndex, (uint32_t)mCommandSlots.size() - 1)];
		if (completedSlot.readbackPending && completedSlot.controlReadback.buffer != nullptr)
		{
			mStatus.gpuStatsValid = false;
			const void* mapped = renderer.mFrameBuffer->mCore.MapBuffer(*completedSlot.controlReadback.buffer, 0, sizeof(NRISmokeControlGpu));
			if (mapped != nullptr && completedSlot.readbackEpoch == mStatus.simulationEpoch)
			{
				const NRISmokeControlGpu control = *static_cast<const NRISmokeControlGpu*>(mapped);
				renderer.mFrameBuffer->mCore.UnmapBuffer(*completedSlot.controlReadback.buffer);
				mStatus.gpuStatsValid = true;
				mStatus.gpuStatsFrame = completedSlot.readbackFrame;
				mStatus.gpuStatsEpoch = completedSlot.readbackEpoch;
				mStatus.activeParticles = control.activeApprox;
				mStatus.spawnedParticles = control.spawned;
				mStatus.expiredParticles = control.expired;
				mStatus.liveEvictions = control.liveEvictions;
				mStatus.columnOverflow = control.columnOverflow;
				mStatus.wideParticlesProjected = control.wideParticlesProjected;
				mStatus.wideGlobalDrops = control.wideGlobalDrops;
				mStatus.fineColumnReferences = control.fineColumnReferences;
				mStatus.wideCellReferences = control.wideCellReferences;
				mStatus.globalDepthReferences = control.globalDepthReferences;
				mStatus.referenceInvalidLinks = control.referenceInvalidLinks;
				mStatus.referenceTraversalLimitExits = control.referenceTraversalLimitExits;
				mStatus.fineTierParticles = control.fineTierParticles;
				mStatus.wideTierParticles = control.wideTierParticles;
				mStatus.globalTierParticles = control.globalTierParticles;
				mStatus.fineOccupiedCells = control.fineOccupiedCells;
				mStatus.wideOccupiedCells = control.wideOccupiedCells;
				mStatus.globalOccupiedSlices = control.globalOccupiedSlices;
				mStatus.fineMaximumCellReferences = control.fineMaximumCellReferences;
				mStatus.wideMaximumCellReferences = control.wideMaximumCellReferences;
				mStatus.globalMaximumCellReferences = control.globalMaximumCellReferences;
				mStatus.maximumDepthSpan = control.maximumDepthSpan;
				mStatus.maximumCandidatesPerFroxel = control.maximumCandidatesPerFroxel;
				mStatus.occupiedCount = control.occupiedCount;
				mStatus.occupiedOverflow = control.occupiedOverflow;
				mStatus.mediumCandidateTests = control.mediumCandidateTests;
				mStatus.pointFroxelsProcessed = control.pointFroxelsProcessed;
				mStatus.directionalFroxelsProcessed = control.directionalFroxelsProcessed;
				mStatus.directionalSamples = control.directionalSamples;
				mStatus.directionalShadowRays = control.directionalShadowRays;
				mStatus.directionalShadowVisible = control.directionalShadowVisible;
				mStatus.directionalShadowOccluded = control.directionalShadowOccluded;
				mStatus.directionalRadianceClamps = control.directionalRadianceClamps;
				mStatus.emissiveFroxelsProcessed = control.emissiveFroxelsProcessed;
				mStatus.emissiveSamples = control.emissiveSamples;
				mStatus.emissiveCandidateMisses = control.emissiveCandidateMisses;
				mStatus.emissiveDistanceRejected = control.emissiveDistanceRejected;
				mStatus.emissiveFacingRejected = control.emissiveFacingRejected;
				mStatus.emissiveShadowRays = control.emissiveShadowRays;
				mStatus.emissiveShadowVisible = control.emissiveShadowVisible;
				mStatus.emissiveShadowOccluded = control.emissiveShadowOccluded;
				mStatus.emissiveContributed = control.emissiveContributed;
				mStatus.emissiveRadianceClamps = control.emissiveRadianceClamps;
				mStatus.emissiveReservoirInitial = control.emissiveReservoirInitial;
				mStatus.emissiveReservoirInvalid = control.emissiveReservoirInvalid;
				mStatus.emissiveTemporalAccepted = control.emissiveTemporalAccepted;
				mStatus.emissiveTemporalRejected = control.emissiveTemporalRejected;
				mStatus.emissiveSpatialAccepted = control.emissiveSpatialAccepted;
				mStatus.emissiveSpatialRejected = control.emissiveSpatialRejected;
				mStatus.emissiveFinalEvaluations = control.emissiveFinalEvaluations;
				mStatus.emissiveSourceClamps = control.emissiveSourceClamps;
				mStatus.emissiveRemovedEnergy = control.emissiveRemovedEnergy;
				mStatus.emissiveMaximumAge = control.emissiveMaximumAge;
				mStatus.emissiveReferenceSamples = control.emissiveReferenceSamples;
				mStatus.emissiveReferenceRays = control.emissiveReferenceRays;
				mStatus.emissiveIdentityRejects = control.emissiveIdentityRejects;
				mStatus.emissiveInnerRisSets = control.emissiveInnerRisSets;
				mStatus.emissiveInnerPointProposals = control.emissiveInnerPointProposals;
				mStatus.emissiveInnerZeroProposals = control.emissiveInnerZeroProposals;
				mStatus.emissiveInnerRisRejects = control.emissiveInnerRisRejects;
				mStatus.emissiveInnerSelections = control.emissiveInnerSelections;
				mStatus.emissiveInnerVisibilityRays = control.emissiveInnerVisibilityRays;
				mStatus.emissiveInnerSourceVisibilityRays = control.emissiveInnerSourceVisibilityRays;
				mStatus.emissiveInnerVisibilityVisible = control.emissiveInnerVisibilityVisible;
				mStatus.emissiveInnerBlockerReceiverImmediate = control.emissiveInnerBlockerReceiverImmediate;
				mStatus.emissiveInnerBlockerReceiverCell = control.emissiveInnerBlockerReceiverCell;
				mStatus.emissiveInnerBlockerEmitterCell = control.emissiveInnerBlockerEmitterCell;
				mStatus.emissiveInnerBlockerInterior = control.emissiveInnerBlockerInterior;
				mStatus.emissiveInnerSourceSelections = control.emissiveInnerSourceSelections;
				mStatus.emissiveInnerSourceOverflow = control.emissiveInnerSourceOverflow;
				mStatus.emissiveTargetVisibilityRays = control.emissiveTargetVisibilityRays;
				mStatus.emissiveTargetVisibilityVisible = control.emissiveTargetVisibilityVisible;
				mStatus.emissiveTargetBlockerExact = control.emissiveTargetBlockerExact;
				mStatus.emissiveTargetBlockerRange = control.emissiveTargetBlockerRange;
				mStatus.emissiveTargetBlockerOther = control.emissiveTargetBlockerOther;
				mStatus.emissiveTargetWitnessClaim = control.emissiveTargetWitnessClaim;
				mStatus.emissiveTargetWitnessCandidate = control.emissiveTargetWitnessCandidate;
				mStatus.emissiveTargetWitnessRelation = control.emissiveTargetWitnessRelation;
				mStatus.emissiveTargetWitnessSamplePrimitive = control.emissiveTargetWitnessSamplePrimitive;
				mStatus.emissiveTargetWitnessSampleMaterial = control.emissiveTargetWitnessSampleMaterial;
				mStatus.emissiveTargetWitnessBlockerDataSource = control.emissiveTargetWitnessBlockerDataSource;
				mStatus.emissiveTargetWitnessBlockerInstance = control.emissiveTargetWitnessBlockerInstance;
				mStatus.emissiveTargetWitnessBlockerPrimitive = control.emissiveTargetWitnessBlockerPrimitive;
				mStatus.emissiveTargetWitnessBlockerMaterial = control.emissiveTargetWitnessBlockerMaterial;
				mStatus.emissiveTargetWitnessDistanceBits = control.emissiveTargetWitnessDistanceBits;
				mStatus.indirectFroxelsProcessed = control.indirectFroxelsProcessed;
				mStatus.indirectLocalityRays = control.indirectLocalityRays;
				mStatus.indirectLocalityAgreement = control.indirectLocalityAgreement;
				mStatus.indirectLocalityOneSided = control.indirectLocalityOneSided;
				mStatus.indirectLocalityMismatch = control.indirectLocalityMismatch;
				mStatus.indirectLocalityInvalid = control.indirectLocalityInvalid;
				mStatus.indirectReferenceRays = control.indirectReferenceRays;
				mStatus.indirectReferenceHits = control.indirectReferenceHits;
				mStatus.indirectReferenceMisses = control.indirectReferenceMisses;
				mStatus.indirectSectorContributions = control.indirectSectorContributions;
				mStatus.indirectSkyContributions = control.indirectSkyContributions;
				mStatus.indirectEmissionContributions = control.indirectEmissionContributions;
				mStatus.indirectRadianceClamps = control.indirectRadianceClamps;
				mStatus.indirectNanRejects = control.indirectNanRejects;
				mStatus.indirectTemporalAccepted = control.indirectTemporalAccepted;
				mStatus.indirectTemporalRejected = control.indirectTemporalRejected;
				mStatus.indirectSpatialAccepted = control.indirectSpatialAccepted;
				mStatus.indirectSpatialRejected = control.indirectSpatialRejected;
				mStatus.indirectCacheMaximumAge = control.indirectCacheMaximumAge;
				mStatus.indirectCacheClamps = control.indirectCacheClamps;
				mStatus.indirectCacheResolved = control.indirectCacheResolved;
				mStatus.directReceiverSamples = control.directReceiverSamples;
				mStatus.directFractionalVisibility = control.directFractionalVisibility;
				mStatus.directVisibilityZero = control.directVisibilityZero;
				mStatus.directVisibilityOne = control.directVisibilityOne;
				mStatus.directTemporalAccepted = control.directTemporalAccepted;
				mStatus.directTemporalRejected = control.directTemporalRejected;
				mStatus.directSpatialAccepted = control.directSpatialAccepted;
				mStatus.directSpatialRejected = control.directSpatialRejected;
				mStatus.directHistoryMaximumAge = control.directHistoryMaximumAge;
				mStatus.directHistoryResolved = control.directHistoryResolved;
				mStatus.directHistoryClamps = control.directHistoryClamps;
				mStatus.directNanRejects = control.directNanRejects;
				mStatus.lightCandidatesTested = control.lightCandidatesTested;
				mStatus.lightDistanceRejected = control.lightDistanceRejected;
				mStatus.lightShadowRays = control.lightShadowRays;
				mStatus.lightShadowVisible = control.lightShadowVisible;
				mStatus.lightShadowOccluded = control.lightShadowOccluded;
				mStatus.lightSoftSamples = control.lightSoftSamples;
				mStatus.lightRadianceClamps = control.lightRadianceClamps;
				mStatus.filterCandidateHits = control.filterCandidateHits;
				mStatus.filterAlphaRejects = control.filterAlphaRejects;
				mStatus.filterNoShadowRejects = control.filterNoShadowRejects;
				mStatus.filterOneWayRejects = control.filterOneWayRejects;
				mStatus.filterReflectionRejects = control.filterReflectionRejects;
				mStatus.filterPortalContinuations = control.filterPortalContinuations;
				mStatus.filterAcceptedBlockers = control.filterAcceptedBlockers;
				mStatus.filterMisses = control.filterMisses;
				mStatus.filterSkipLimitExits = control.filterSkipLimitExits;
				mStatus.filterContinuationLimitExits = control.filterContinuationLimitExits;
				mStatus.filterResourceDowngrades = control.filterResourceDowngrades;
				auto& analyticLight = mStatus.analyticLight;
				analyticLight.valid = true;
				analyticLight.sourceFrame = completedSlot.readbackFrame;
				analyticLight.epoch = completedSlot.readbackEpoch;
				analyticLight.profile = completedSlot.analyticProfile;
				analyticLight.profileRevision = completedSlot.analyticProfileRevision;
				analyticLight.buildDispatchGroups = completedSlot.analyticBuildDispatchGroups;
				analyticLight.applyDispatchGroups = completedSlot.analyticApplyDispatchGroups;
				analyticLight.cpu = completedSlot.analyticSnapshot;
				analyticLight.buildEvents = control.analyticLightBuildEvents;
				analyticLight.anchorsBuilt = control.analyticLightAnchorsBuilt;
				analyticLight.anchorsValid = control.analyticLightAnchorsValid;
				analyticLight.anchorsInvalid = control.analyticLightAnchorsInvalid;
				analyticLight.samplesRequested = control.analyticLightSamplesRequested;
				analyticLight.samplesExecuted = control.analyticLightSamplesExecuted;
				analyticLight.evaluations = control.analyticLightEvaluations;
				analyticLight.buildVisibilityRays = control.analyticLightBuildVisibilityRays;
				analyticLight.gridSeedHits = control.analyticLightGridSeedHits;
				analyticLight.gridSeedMisses = control.analyticLightGridSeedMisses;
				analyticLight.applyFroxelsTested = control.analyticLightApplyFroxelsTested;
				analyticLight.applyFroxelsApplied = control.analyticLightApplyFroxelsApplied;
				analyticLight.carrierContributions = control.analyticLightCarrierContributions;
				analyticLight.anchorBlendTaps = control.analyticLightAnchorBlendTaps;
				analyticLight.groupCacheHits = control.analyticLightGroupCacheHits;
				analyticLight.missingGroupRecords = control.analyticLightMissingGroupRecords;
				analyticLight.identityRejects = control.analyticLightIdentityRejects;
				analyticLight.applyVisibilityRays = control.analyticLightApplyVisibilityRays;
				mStatus.controlReadbackBytes += sizeof(NRISmokeControlGpu);
				if (mSettings.traceMode >= 2u)
				{
					const auto& cpu = analyticLight.cpu;
					Printf("PERF pt smoke analytic light NRI: source_frame=%llu epoch=%u readback=valid profile=%u revision=%u implementation=%u events_requested=%u events_admitted=%u events_rejected=%u events_ready=%u reject_not_prepared=%u reject_disabled=%u reject_invalid=%u reject_stale_epoch=%u reject_expired=%u reject_stale=%u reject_capacity=%u reject_light_budget=%u groups_active=%u groups_free=%u groups_high_water=%u shared_carriers=%u anchors_requested=%u anchors_reserved=%u samples_cpu_requested=%u samples_cpu_reserved=%u build_dispatch_groups=%u apply_dispatch_groups=%u build_events=%u anchors_built=%u anchors_valid=%u anchors_invalid=%u samples_gpu_requested=%u samples_executed=%u evaluations=%u build_visibility_rays=%u grid_seed_hits=%u grid_seed_misses=%u apply_froxels_tested=%u apply_froxels_applied=%u carrier_contributions=%u anchor_blend_taps=%u group_cache_hits=%u missing_group_records=%u identity_rejects=%u apply_visibility_rays=%u apply_rays_zero=%s analytic_grid_requests=0 analytic_grid_radiance=0 analytic_grid_links=0 analytic_prompt_claims=0 analytic_dormant_work=0 compact=1\n",
						(unsigned long long)analyticLight.sourceFrame, analyticLight.epoch,
						analyticLight.profile, analyticLight.profileRevision, analyticLight.implementation,
						cpu.lightEventsRequestedThisFrame, cpu.lightEventsAdmittedThisFrame,
						cpu.lightEventsRejectedThisFrame, cpu.lightEventsFirstFrameReady,
						cpu.lightRejectedNotPrepared, cpu.lightRejectedDisabled,
						cpu.lightRejectedInvalidRequest, cpu.lightRejectedStaleEpoch,
						cpu.lightRejectedExpiredOnArrival, cpu.lightRejectedStaleOnArrival,
						cpu.lightRejectedCapacity, cpu.lightRejectedLightingBudget,
						cpu.activeLightGroups, cpu.freeLightGroupSlots, cpu.lightGroupHighWater,
						cpu.sharedCarrierReferences, cpu.lightAnchorsRequested,
						cpu.lightAnchorsReserved, cpu.lightSamplesRequested, cpu.lightSamplesReserved,
						analyticLight.buildDispatchGroups, analyticLight.applyDispatchGroups,
						analyticLight.buildEvents, analyticLight.anchorsBuilt,
						analyticLight.anchorsValid, analyticLight.anchorsInvalid,
						analyticLight.samplesRequested, analyticLight.samplesExecuted,
						analyticLight.evaluations, analyticLight.buildVisibilityRays,
						analyticLight.gridSeedHits, analyticLight.gridSeedMisses,
						analyticLight.applyFroxelsTested, analyticLight.applyFroxelsApplied,
						analyticLight.carrierContributions, analyticLight.anchorBlendTaps,
						analyticLight.groupCacheHits, analyticLight.missingGroupRecords,
						analyticLight.identityRejects, analyticLight.applyVisibilityRays,
						analyticLight.applyVisibilityRays == 0u ? "yes" : "no");
				}
			}
			else if (mapped != nullptr)
				renderer.mFrameBuffer->mCore.UnmapBuffer(*completedSlot.controlReadback.buffer);
			completedSlot.readbackPending = false;
		}
	}
	if (!mSettings.readback)
	{
		mStatus.gpuStatsValid = false;
		mStatus.analyticLight.valid = false;
	}
	const bool worldLightingRequired = mSettings.representation != 0u &&
		(mSettings.emissiveBackend != (uint32_t)NRISmokeEmissiveBackend::Legacy ||
		 mSettings.multipleScatter || mSettings.selfShadow);
	NRISmokeDormantGridConfig dormantConfig = {};
	dormantConfig.enabled = mSettings.dormantGrid && mSettings.representation != 0u;
	dormantConfig.archiveCapacity = 256u;
	dormantConfig.maximumDemotionsPerFrame = workSchedule.table.dormantArchives;
	dormantConfig.maximumPromotionsPerFrame = workSchedule.table.dormantPromotions;
	dormantConfig.maximumEvolutionPerFrame = workSchedule.table.dormantEvolution;
	dormantConfig.maximumContinuousInjectionsPerFrame = workSchedule.table.dormantEvolution;
	dormantConfig.densityHalfLifeScale = mSettings.gridDensityHalfLifeScale;
	dormantConfig.coolingScale = mSettings.gridCoolingScale;
	bool worldLightingReady = !worldLightingRequired;
	auto prepareGridOwners = [&]()
	{
		const NRISmokeGridServices services = BuildGridServices(renderer);
		const bool gridPrepared = mGrid.PrepareFrame(services, mSettings,
			renderer.mFrameIndex, mStatus.simulationEpoch);
		mStatus.gridReady = mSettings.representation != 0u && gridPrepared && mGrid.GetStatusSnapshot().resourcesReady;
		if (mStatus.gridReady)
		{
			NRISmokeDormantGridConfig resourceConfig = dormantConfig;
			// Keep fixed descriptor-compatible archive resources available even
			// when policy is disabled; shader access is separately flag-gated.
			resourceConfig.enabled = true;
			mDormantGrid.PrepareFrame(services, resourceConfig, mStatus.simulationEpoch);
		}
		mStatus.dormantGrid = mDormantGrid.GetStatusSnapshot();
		worldLightingReady = !worldLightingRequired;
		if (mStatus.gridReady && worldLightingRequired)
		{
			const NRISmokeGridStatusSnapshot& gridStatus = mGrid.GetStatusSnapshot();
			worldLightingReady = mGridLighting.PrepareFrame(BuildGridServices(renderer), mSettings,
				workSchedule.table,
				gridStatus.cellCapacity, renderer.mFrameIndex, mStatus.simulationEpoch) && mGridLighting.IsWorldReady();
			if (worldLightingReady)
			{
				std::array<const nri::Descriptor*, NRISmokeGrid::EvaluationDescriptorCount> gridLightingSnapshot = {};
				if (mGrid.GetEvaluationStorageDescriptors(gridLightingSnapshot))
					mGridLighting.PublishGridSnapshot(gridLightingSnapshot, mGrid.GetFieldPing(), mSettings.gridCellSize);
			}
		}
	};
	prepareGridOwners();
	NRISmokeInterestFrameInput interestInput = {};
	interestInput.rendererFrame = renderer.mFrameIndex;
	interestInput.cameraPosition = renderer.mCurrentCameraPos;
	interestInput.previousCameraPosition = renderer.mPreviousCameraPos;
	interestInput.hasPreviousCamera = renderer.mHasPreviousCameraState;
	interestInput.mapWorld = &renderer.mMapWorld;
	interestInput.visibleChunkWords = &renderer.mCurrentVisibleChunkWords;
	interestInput.overlays = &GetResolvedLightOverlaySet();
	mInterest.Update(interestInput);
	const NRISmokeDormantGridStatusSnapshot& dormantStatus =
		mDormantGrid.GetStatusSnapshot();
	if (dormantStatus.gpuStatsValid &&
		dormantStatus.gpuRendererFrame != mLastDormantResultFrame)
	{
		for (const NRISmokeDormantGridResultGpu& result : dormantStatus.results)
		{
			NRISmokeSpatialCoordinate coordinate = {
				result.coordinate[0], result.coordinate[1], result.coordinate[2] };
			const bool pendingDemotion =
				mDormantPendingDemotions.erase(coordinate) != 0u;
			if (result.outcome == (uint32_t)NRISmokeDormantGridOutcome::Archived)
			{
				NRISmokeSpatialBrickObservation observation = {};
				observation.coordinate = coordinate;
				observation.generation = result.outputGeneration;
				observation.authority = NRISmokeSpatialAuthority::Coarse;
				observation.occupied = true;
				observation.lastSimulationFrame = (uint32_t)std::min<uint64_t>(
					dormantStatus.gpuRendererFrame, UINT32_MAX);
				mDormantAuthorities[coordinate] = observation;
			}
			else if (result.outcome == (uint32_t)NRISmokeDormantGridOutcome::Rehydrated)
				mDormantAuthorities.erase(coordinate);
			else if (pendingDemotion)
				mDormantAuthorities.erase(coordinate);
		}
		mLastDormantResultFrame = dormantStatus.gpuRendererFrame;
	}

	NRISmokeSpatialInterestFrameInput spatialInput = {};
	spatialInput.epoch = mStatus.simulationEpoch;
	spatialInput.rendererFrame = renderer.mFrameIndex;
	spatialInput.brickWorldSize = mSettings.gridCellSize *
		(float)NRI_SMOKE_GRID_BRICK_AXIS;
	std::copy(renderer.mCurrentCameraPos, renderer.mCurrentCameraPos + 3,
		spatialInput.cameraPosition);
	std::copy(renderer.mPreviousCameraPos, renderer.mPreviousCameraPos + 3,
		spatialInput.previousCameraPosition);
	spatialInput.hasPreviousCamera = renderer.mHasPreviousCameraState;
	const NRISmokeInterestSnapshot& sourceInterest = mInterest.GetSnapshot();
	spatialInput.conservativeInterestComplete =
		sourceInterest.conservativeInterestComplete;
	spatialInput.runtimePortalUncertain = sourceInterest.runtimePortalUncertain;
	spatialInput.demotionQuantity = dormantConfig.enabled ?
		dormantConfig.maximumDemotionsPerFrame : 0u;
	spatialInput.promotionQuantity = dormantConfig.enabled ?
		dormantConfig.maximumPromotionsPerFrame : 0u;
	for (uint32_t chunkIndex = 0u; chunkIndex < renderer.mMapWorld.chunks.size(); ++chunkIndex)
	{
		const auto& chunk = renderer.mMapWorld.chunks[chunkIndex];
		if (!chunk.bounds.valid ||
			!SmokeChunkMarked(sourceInterest.positiveChunkWords, chunkIndex)) continue;
		NRISmokeSpatialInterestRegion region = {};
		std::copy(chunk.bounds.min, chunk.bounds.min + 3, region.boundsMin);
		std::copy(chunk.bounds.max, chunk.bounds.max + 3, region.boundsMax);
		region.reasons = NRISmokeSpatialReason_MainView;
		spatialInput.positiveRegions.push_back(region);
	}
	const NRISmokeGridStatusSnapshot& gridSpatial = mGrid.GetStatusSnapshot();
	spatialInput.bricks.reserve(gridSpatial.spatialBricks.size() +
		mDormantAuthorities.size());
	for (const NRISmokeGridBrickGpu& brick : gridSpatial.spatialBricks)
	{
		NRISmokeSpatialCoordinate coordinate = {
			brick.coordinate[0], brick.coordinate[1], brick.coordinate[2] };
		if (brick.state != kSmokeGridResidentState || brick.generation == 0u ||
			(brick.flags & kSmokeGridBrickContent) == 0u ||
			mDormantAuthorities.find(coordinate) != mDormantAuthorities.end() ||
			mDormantPendingDemotions.find(coordinate) != mDormantPendingDemotions.end()) continue;
		NRISmokeSpatialBrickObservation observation = {};
		observation.coordinate = coordinate;
		observation.generation = brick.generation;
		observation.authority = NRISmokeSpatialAuthority::Fine;
		observation.occupied = true;
		observation.lastSimulationFrame = (uint32_t)std::min<uint64_t>(
			gridSpatial.spatialGpuRendererFrame, UINT32_MAX);
		spatialInput.bricks.push_back(observation);
	}
	for (const auto& authority : mDormantAuthorities)
		spatialInput.bricks.push_back(authority.second);
	const NRISmokeSpatialInterestSnapshot& spatial =
		mSpatialInterest.Update(spatialInput);
	mStatus.spatialResidency = spatial;
	mDormantDemotions.clear();
	mDormantPromotions.clear();
	auto appendDormantWork = [&](const NRISmokeSpatialCandidate& candidate,
		std::vector<NRISmokeDormantGridWorkGpu>& output)
	{
		NRISmokeDormantGridWorkGpu work = {};
		work.coordinate[0] = candidate.coordinate.x;
		work.coordinate[1] = candidate.coordinate.y;
		work.coordinate[2] = candidate.coordinate.z;
		work.generation = candidate.generation;
		work.epoch = mStatus.simulationEpoch;
		work.lastSimulationFrame = renderer.mFrameIndex - candidate.simulationAgeFrames;
		work.opticalMass = candidate.opticalMass;
		if (candidate.opticalMass > 0.0f)
			work.flags |= NRISmokeDormantGridWorkFlag_MassKnown;
		output.push_back(work);
	};
	for (const NRISmokeSpatialCandidate& candidate : spatial.demotions)
	{
		appendDormantWork(candidate, mDormantDemotions);
		mDormantPendingDemotions.insert(candidate.coordinate);
	}
	for (const NRISmokeSpatialCandidate& candidate : spatial.promotions)
		appendDormantWork(candidate, mDormantPromotions);
	const uint32_t previousGeneration = mEmitters.GetGeneration();
	const double gameplayTimeSeconds = PlayClock > 0 ? (double)PlayClock * (1.0 / 120.0) : 0.0;
	mEmitters.SetContinuousSourceWorkQuantity(dormantConfig.maximumEvolutionPerFrame);
	mEmitters.Gather(mStatus.simulationEpoch, gameplayTimeSeconds, weaponEvents, renderer.mSceneLights,
		mStyles, mPendingCommands, mPendingPulseEnqueueInfo, mPendingTrailObservations,
		mPendingAnalyticRequests,
		mNextCommandSerial, mSettings.traceMode, mInterest.GetSnapshot(),
		mSettings.gridCellSize, mSettings.gridBrickCapacity);
	std::set<NRISmokeSpatialCoordinate> promotionCoordinates;
	for (const NRISmokeDormantGridWorkGpu& work : mDormantPromotions)
		promotionCoordinates.insert({ work.coordinate[0], work.coordinate[1], work.coordinate[2] });
	NRISmokeDormantInjectionBuildInput dormantInjectionInput = {};
	dormantInjectionInput.epoch = mStatus.simulationEpoch;
	dormantInjectionInput.maximumInjections = dormantConfig.maximumContinuousInjectionsPerFrame;
	dormantInjectionInput.cellSize = mSettings.gridCellSize;
	dormantInjectionInput.requests = &mEmitters.GetContinuousSourceSnapshot().work;
	dormantInjectionInput.styles = &mStyles;
	dormantInjectionInput.authorities = &mDormantAuthorities;
	dormantInjectionInput.promotions = &promotionCoordinates;
	mDormantInjectionBuild = NRIBuildSmokeDormantInjections(dormantInjectionInput);
	std::set<uint32_t> dormantRoutedSourceIds = mDormantInjectionBuild.routedSourceIds;
	const float dormantBrickSize = mSettings.gridCellSize * (float)NRI_SMOKE_GRID_BRICK_AXIS;
	for (const NRISmokeContinuousSourceWorkRequest& request :
		mEmitters.GetContinuousSourceSnapshot().work)
	{
		if (dormantBrickSize <= 0.0f) break;
		const NRISmokeSpatialCoordinate coordinate = {
			(int32_t)std::floor(request.position[0] / dormantBrickSize),
			(int32_t)std::floor(request.position[1] / dormantBrickSize),
			(int32_t)std::floor(request.position[2] / dormantBrickSize) };
		if (mDormantPendingDemotions.find(coordinate) != mDormantPendingDemotions.end())
			dormantRoutedSourceIds.insert(request.sourceId);
	}
	if (!dormantRoutedSourceIds.empty())
	{
		for (size_t index = mPendingCommands.size(); index-- > 0u; )
		{
			if (dormantRoutedSourceIds.find(mPendingCommands[index].sourceId) ==
				dormantRoutedSourceIds.end()) continue;
			mPendingCommands.erase(mPendingCommands.begin() + index);
			if (index < mPendingPulseEnqueueInfo.size())
				mPendingPulseEnqueueInfo.erase(mPendingPulseEnqueueInfo.begin() + index);
		}
	}
	const bool styleLayoutInvalidated = previousGeneration != 0 && previousGeneration != mEmitters.GetGeneration();
	if (styleLayoutInvalidated)
	{
		mStatus.simulationEpoch = std::max(1u, mStatus.simulationEpoch + 1u);
		mNeedsClear = true;
		mStatus.resetReason = "style-layout";
		mStatus.gpuStatsValid = false;
		for (NRISmokeInjectionCommandGpu& command : mPendingCommands) command.epoch = mStatus.simulationEpoch;
		for (NRISmokeAnalyticCarrierRequest& request : mPendingAnalyticRequests) request.epoch = mStatus.simulationEpoch;
		mGrid.Reset(mStatus.simulationEpoch, mStatus.resetReason);
		mDormantGrid.Reset(mStatus.simulationEpoch, mStatus.resetReason);
		mSpatialInterest.Reset(mStatus.simulationEpoch);
		mDormantAuthorities.clear();
		mDormantPendingDemotions.clear();
		mDormantInjectionBuild = {};
		mLastDormantResultFrame = UINT64_MAX;
		mGridLighting.Reset(mStatus.simulationEpoch, mStatus.resetReason);
		prepareGridOwners();
	}

	auto buildAuthorityRequest = [&]()
	{
		NRISmokeAuthorityRequest request = {};
		request.enabled = true;
		request.requestedRepresentation = mSettings.representation;
		request.gridReady = mStatus.gridReady;
		request.worldLightingRequired = worldLightingRequired;
		request.worldLightingReady = worldLightingReady;
		return request;
	};
	NRISmokeAuthorityRequest authorityRequest = buildAuthorityRequest();
	NRISmokeAuthorityDecision authorityDecision = mAuthority.Resolve(authorityRequest);
	const NRISmokeAuthoritySnapshot previousAuthority = mAuthority.GetSnapshot();
	bool authorityTransition = !previousAuthority.operational || previousAuthority.mode != authorityDecision.mode;
	if (authorityTransition && previousAuthority.operational)
	{
		// Readiness can change while queued work drains. Resolve once more before
		// any state mutation so a transient lighting failure cannot clear a valid
		// committed grid merely by proposing a fallback.
		renderer.WaitForCommandsTracked();
		prepareGridOwners();
		authorityRequest = buildAuthorityRequest();
		authorityDecision = mAuthority.Resolve(authorityRequest);
		authorityTransition = previousAuthority.mode != authorityDecision.mode;
	}
	bool resourcesReady = true;
	const bool previousParticles = previousAuthority.mode == NRISmokeAuthorityMode::Particles ||
		previousAuthority.mode == NRISmokeAuthorityMode::Compare;
	const bool targetParticles = authorityDecision.mode == NRISmokeAuthorityMode::Particles ||
		authorityDecision.mode == NRISmokeAuthorityMode::Compare;
	const bool isolatedTargetAttempt = authorityTransition && previousAuthority.operational && previousParticles != targetParticles;
	if (previousAuthority.operational && mResourceStyleCapacity != (uint32_t)mStyles.size() &&
		!EnsureResources(renderer, previousAuthority.effectiveRepresentation))
	{
		mAuthority.Disable(mSettings.representation, renderer.mFrameIndex, "style-resource-rebuild-failed");
		PublishAuthorityStatus("failed", "style-resource-rebuild-failed-disabled");
		return true;
	}
	if (isolatedTargetAttempt)
		resourcesReady = RebuildAuthorityResourcesTransactional(renderer, authorityDecision.effectiveRepresentation);
	else
		resourcesReady = EnsureResources(renderer, authorityDecision.effectiveRepresentation);
	if (!resourcesReady)
	{
		const bool previousGrid = previousAuthority.mode == NRISmokeAuthorityMode::Grid ||
			previousAuthority.mode == NRISmokeAuthorityMode::Compare;
		const bool oldAuthorityInvalidated = gridLayoutInvalidated || styleLayoutInvalidated;
		const bool previousGridLightingReady = !previousAuthority.worldLightingRequired ||
			mGridLighting.IsWorldReady();
		const bool oldAuthorityStillSafe = !oldAuthorityInvalidated &&
			(!previousGrid || (mGrid.GetStatusSnapshot().resourcesReady && previousGridLightingReady));
		if (previousAuthority.operational && isolatedTargetAttempt && oldAuthorityStillSafe)
		{
			// The candidate allocation was isolated. Continue with the last safe
			// authority and its last validated state; target preparation did not
			// advance the authority-transition epoch. Do
			// not simulate with target settings or consume this frame's one-shot
			// commands until the requested backend can be prepared coherently.
			PublishAuthorityStatus("failed-retained", "target-allocation-failed-retained-current");
			AppendSyntheticCommand(renderer);
			mStatus.particleSimulationDispatches = 0u;
			mStatus.gridSimulationDispatches = 0u;
			mStatus.particleOpticalDispatches = 0u;
			mStatus.gridOpticalDispatches = 0u;
			mStatus.particleCommandsRouted = 0u;
			mStatus.gridCommandsRouted = 0u;
			return true;
		}
		mAuthority.Disable(mSettings.representation, renderer.mFrameIndex, "authority-resource-allocation-failed");
		PublishAuthorityStatus("failed", isolatedTargetAttempt ? "resource-allocation-failed" : "resource-rebuild-failed-disabled");
		return true;
	}
	if (authorityTransition)
	{
		std::vector<NRISmokeInjectionCommandGpu> transitionCommands = std::move(mPendingCommands);
		std::vector<NRISmokePulseEnqueueInfo> transitionEnqueueInfo =
			std::move(mPendingPulseEnqueueInfo);
		std::vector<NRISmokeAnalyticTrailObservationBatch> transitionTrailObservations =
			std::move(mPendingTrailObservations);
		std::vector<NRISmokeAnalyticCarrierRequest> transitionAnalytic = std::move(mPendingAnalyticRequests);
		Reset("authority-transition");
		mPendingCommands = std::move(transitionCommands);
		mPendingPulseEnqueueInfo = std::move(transitionEnqueueInfo);
		mPendingTrailObservations = std::move(transitionTrailObservations);
		mPendingAnalyticRequests = std::move(transitionAnalytic);
		mStatus.preparedFrame = renderer.mFrameIndex;
		mLastPreparedFrame = renderer.mFrameIndex;
		for (NRISmokeInjectionCommandGpu& command : mPendingCommands)
			command.epoch = mStatus.simulationEpoch;
		for (NRISmokeAnalyticCarrierRequest& request : mPendingAnalyticRequests)
			request.epoch = mStatus.simulationEpoch;
		if (authorityDecision.mode == NRISmokeAuthorityMode::Grid || authorityDecision.mode == NRISmokeAuthorityMode::Compare)
		{
			prepareGridOwners();
			authorityRequest = buildAuthorityRequest();
			const NRISmokeAuthorityDecision postResetDecision = mAuthority.Resolve(authorityRequest);
			if (postResetDecision.mode != authorityDecision.mode)
			{
				mAuthority.Disable(mSettings.representation, renderer.mFrameIndex, "authority-post-reset-readiness-failed");
				PublishAuthorityStatus("failed", "post-reset-readiness-failed");
				return true;
			}
			authorityDecision = postResetDecision;
		}
	}
	mAuthority.Commit(authorityRequest, authorityDecision, renderer.mFrameIndex);
	PublishAuthorityStatus(authorityTransition ? "completed" : "none");
	const NRISmokeAuthoritySnapshot& authority = mAuthority.GetSnapshot();
	if (authority.mode == NRISmokeAuthorityMode::Grid)
	{
		mMayHaveParticleSmoke = false;
		mLatestParticleDeathSeconds = 0.0;
		mStatus.activeParticles = 0u;
		mStatus.spawnedParticles = 0u;
		mStatus.expiredParticles = 0u;
		mStatus.liveEvictions = 0u;
		mStatus.columnOverflow = 0u;
		mStatus.wideParticlesProjected = 0u;
		mStatus.wideGlobalDrops = 0u;
		mStatus.fineColumnReferences = 0u;
		mStatus.wideCellReferences = 0u;
		mStatus.globalDepthReferences = 0u;
		mStatus.referenceInvalidLinks = 0u;
		mStatus.referenceTraversalLimitExits = 0u;
		mStatus.fineTierParticles = 0u;
		mStatus.wideTierParticles = 0u;
		mStatus.globalTierParticles = 0u;
		mStatus.fineOccupiedCells = 0u;
		mStatus.wideOccupiedCells = 0u;
		mStatus.globalOccupiedSlices = 0u;
		mStatus.fineMaximumCellReferences = 0u;
		mStatus.wideMaximumCellReferences = 0u;
		mStatus.globalMaximumCellReferences = 0u;
		mStatus.maximumDepthSpan = 0u;
		mStatus.maximumCandidatesPerFroxel = 0u;
	}
	if (mSettings.readback && (PerfCompactCaptureTimingActive() || PerfCompactCaptureReadbackDrainActive()))
	{
		const NRISmokeGridStatusSnapshot& gridWork = mGrid.GetStatusSnapshot();
		const NRISmokeGridLightingStatusSnapshot& worldWork = mGridLighting.GetStatusSnapshot();
		const NRISmokePulseSnapshot& pulseWork = mPulseOwner.GetSnapshot();
		const NRISmokePromptFallbackSnapshot& promptWork = mPromptFallback.GetSnapshot();
		const NRISmokeAnalyticTrailBridgeSnapshot& trailBridgeWork =
			mAnalyticTrailBridge.GetSnapshot();
		const bool joined = gridWork.gpuStatsValid && worldWork.gpuStatsValid && mStatus.gpuStatsValid &&
			gridWork.gpuRendererFrame == worldWork.gpuRendererFrame &&
			gridWork.gpuRendererFrame == mStatus.gpuStatsFrame &&
			gridWork.gpu.generation == worldWork.gpu.simulationEpoch &&
			gridWork.gpu.generation == mStatus.gpuStatsEpoch;
		Printf("PERF pt smoke work NRI: observe_renderer_frame=%llu observe_frame=%u joined=%u grid_valid=%u grid_renderer_frame=%llu grid_frame=%u grid_epoch=%u grid_delta_valid=%u grid_delta_interval=%u grid_resident=%u grid_free=%u grid_active_a=%u grid_active_b=%u grid_occupied=%u grid_empty=%u grid_allocated_total=%u grid_allocated_delta=%u grid_reclaimed_total=%u grid_reclaimed_delta=%u grid_allocation_failures_total=%u grid_allocation_failures_delta=%u grid_probe_failures_total=%u grid_probe_failures_delta=%u grid_commands_total=%u grid_commands_delta=%u grid_deposition_cells_total=%u grid_deposition_cells_delta=%u grid_deposition_rejected_total=%u grid_deposition_rejected_delta=%u grid_halo_total=%u grid_halo_delta=%u world_valid=%u world_renderer_frame=%llu world_frame=%u world_epoch=%u world_active=%u world_support_current=%u world_source_density=%u world_support_only=%u world_support_duplicates=%u world_support_overflow=%u world_scheduled=%u world_samples=%u world_visible=%u world_physical_zero=%u world_missing=%u world_overflow=%u world_temporal_accepted=%u world_temporal_rejected=%u world_links_open=%u world_links_blocked=%u world_link_rays=%u world_proposal_lists=%u world_proposal_tested=%u world_proposal_accepted=%u view_valid=%u view_renderer_frame=%llu view_epoch=%u view_occupied=%u view_occupied_overflow=%u view_point_froxels=%u view_point_shadow_rays=%u view_directional_froxels=%u view_directional_samples=%u view_directional_shadow_rays=%u view_direct_receiver_samples=%u view_direct_temporal_accepted=%u view_direct_temporal_rejected=%u view_direct_spatial_accepted=%u view_direct_spatial_rejected=%u compact=1\n",
			(unsigned long long)renderer.mFrameBuffer->mFrameIndex, renderer.mFrameIndex, joined ? 1u : 0u,
			gridWork.gpuStatsValid ? 1u : 0u, (unsigned long long)gridWork.gpuRendererFrame, gridWork.gpu.frameStamp,
			gridWork.gpu.generation, gridWork.gpuFrameDeltaValid ? 1u : 0u, gridWork.gpuFrameDeltaInterval,
			gridWork.gpu.residentCount, gridWork.gpu.freeCount, gridWork.gpu.activeCountA,
			gridWork.gpu.activeCountB, gridWork.gpu.occupiedBricks, gridWork.gpu.emptyBricks,
			gridWork.gpu.allocated, gridWork.gpuFrameDelta.allocated,
			gridWork.gpu.reclaimed, gridWork.gpuFrameDelta.reclaimed,
			gridWork.gpu.allocationFailures, gridWork.gpuFrameDelta.allocationFailures,
			gridWork.gpu.probeFailures, gridWork.gpuFrameDelta.probeFailures,
			gridWork.gpu.commandsProcessed, gridWork.gpuFrameDelta.commandsProcessed,
			gridWork.gpu.depositionCells, gridWork.gpuFrameDelta.depositionCells,
			gridWork.gpu.depositionRejected, gridWork.gpuFrameDelta.depositionRejected,
			gridWork.gpu.haloAllocations, gridWork.gpuFrameDelta.haloAllocations,
			worldWork.gpuStatsValid ? 1u : 0u, (unsigned long long)worldWork.gpuRendererFrame,
			worldWork.gpu.frameStamp, worldWork.gpu.simulationEpoch,
			worldWork.gpu.activeCount, worldWork.gpu.supportCount, worldWork.gpu.sourceCount,
			worldWork.gpu.supportOnlyCount, worldWork.gpu.duplicateCount, worldWork.gpu.supportOverflowCount,
			worldWork.gpu.scheduledCount, worldWork.gpu.samples, worldWork.gpu.visible,
			worldWork.gpu.physicalZero, worldWork.gpu.missing, worldWork.gpu.overflowRejects,
			worldWork.gpu.temporalAccepted, worldWork.gpu.temporalRejected,
			worldWork.gpu.linksOpen, worldWork.gpu.linksBlocked,
			worldWork.gpu.linksOpen + worldWork.gpu.linksBlocked, worldWork.gpu.proposalListsBuilt,
			worldWork.gpu.proposalCandidatesTested, worldWork.gpu.proposalCandidatesAccepted,
			mStatus.gpuStatsValid ? 1u : 0u, (unsigned long long)mStatus.gpuStatsFrame, mStatus.gpuStatsEpoch,
			mStatus.occupiedCount, mStatus.occupiedOverflow,
			mStatus.pointFroxelsProcessed, mStatus.lightShadowRays, mStatus.directionalFroxelsProcessed,
			mStatus.directionalSamples, mStatus.directionalShadowRays, mStatus.directReceiverSamples,
			mStatus.directTemporalAccepted, mStatus.directTemporalRejected,
			mStatus.directSpatialAccepted, mStatus.directSpatialRejected);
		Printf("PERF pt smoke radiance schedule NRI: observe_renderer_frame=%llu observe_frame=%u valid=%u renderer_frame=%llu frame=%u epoch=%u partitions=%u new_quantity=%u maintenance_quantity=%u maximum_age=%u new_requested=%u new_scheduled=%u new_deferred=%u new_tickets=%u maintenance_requested=%u maintenance_scheduled=%u maintenance_deferred=%u maintenance_tickets=%u history_retained=%u history_missing=%u age_overflows=%u compact=1\n",
			(unsigned long long)renderer.mFrameBuffer->mFrameIndex, renderer.mFrameIndex,
			worldWork.gpuStatsValid ? 1u : 0u, (unsigned long long)worldWork.gpuRendererFrame,
			worldWork.gpu.frameStamp, worldWork.gpu.simulationEpoch, worldWork.gpu.radiancePartitionCount,
			worldWork.gpu.radianceNewInvalidQuantity, worldWork.gpu.radianceMaintenanceQuantity,
			worldWork.gpu.radianceMaximumAge, worldWork.gpu.radianceNewInvalidRequested,
			worldWork.gpu.radianceNewInvalidScheduled, worldWork.gpu.radianceNewInvalidDeferred,
			worldWork.gpu.radianceNewInvalidTickets, worldWork.gpu.radianceMaintenanceRequested,
			worldWork.gpu.radianceMaintenanceScheduled, worldWork.gpu.radianceMaintenanceDeferred,
			worldWork.gpu.radianceMaintenanceTickets, worldWork.gpu.radianceHistoryRetained,
			worldWork.gpu.radianceHistoryMissing, worldWork.gpu.radianceAgeOverflows);
		Printf("PERF pt smoke transactions NRI: observe_renderer_frame=%llu observe_frame=%u pulse_enqueued_ranges=%llu pulse_enqueued_mass=%llu pulse_planned_ranges=%llu pulse_planned_mass=%llu pulse_committed_ranges=%llu pulse_committed_mass=%llu pulse_fallback_retired_ranges=%llu pulse_fallback_retired_mass=%llu pulse_deferred_expired_ranges=%llu pulse_deferred_expired_mass=%llu pulse_superseded_ranges=%llu pulse_superseded_mass=%llu pulse_stale_dropped_ranges=%llu pulse_stale_dropped_mass=%llu pulse_pending_ranges=%u pulse_pending_mass=%llu pulse_authored_clocks=%u pulse_rollbacks=%llu pulse_resets=%llu prompt_scheduled_current=%u prompt_active_slots=%u prompt_oldest_age_ms=%u prompt_scheduled_total=%llu prompt_fallback_executed=%llu prompt_fallback_requested_cells=%llu prompt_fallback_admitted_cells=%llu prompt_fallback_empty_closures=%llu prompt_fallback_partial_closures=%llu prompt_fallback_closed_closures=%llu prompt_grid_handoffs=%llu prompt_expired_ranges=%llu prompt_expired_mass=%llu prompt_deferred_expired_ranges=%llu prompt_deferred_expired_mass=%llu prompt_expiry_ack_failures=%llu prompt_pending_mass=%llu prompt_grid_mass=%llu prompt_fallback_mass=%llu prompt_deferred_ranges=%llu prompt_deferred_mass=%llu prompt_deferred_brick_work=%llu prompt_rollbacks=%llu prompt_internal_errors=%llu prompt_authored_renderer_frame=%llu trail_bridge_active=%u trail_bridge_high_water=%u trail_bridge_replacements=%llu trail_bridge_publications=%llu trail_bridge_grid_handoffs=%llu compact=1\n",
			(unsigned long long)renderer.mFrameBuffer->mFrameIndex, renderer.mFrameIndex,
			(unsigned long long)pulseWork.enqueuedPulses, (unsigned long long)pulseWork.enqueuedMass,
			(unsigned long long)pulseWork.plannedRanges, (unsigned long long)pulseWork.plannedMass,
			(unsigned long long)pulseWork.committedRanges, (unsigned long long)pulseWork.committedMass,
			(unsigned long long)pulseWork.fallbackRetiredRanges,
			(unsigned long long)pulseWork.fallbackRetiredMass,
			(unsigned long long)pulseWork.deferredExpiredRanges,
			(unsigned long long)pulseWork.deferredExpiredMass,
			(unsigned long long)pulseWork.supersededPulses,
			(unsigned long long)pulseWork.supersededMass,
			(unsigned long long)pulseWork.staleDroppedPulses,
			(unsigned long long)pulseWork.staleDroppedMass,
			pulseWork.pendingRanges, (unsigned long long)pulseWork.pendingMass,
			pulseWork.authoredClockCount,
			(unsigned long long)pulseWork.rollbackCount, (unsigned long long)pulseWork.resetPulses,
			promptWork.scheduledFallbackQuantity, promptWork.activeFallbackSlots,
			promptWork.oldestActiveAgeMilliseconds,
			(unsigned long long)promptWork.scheduledFallbackRanges,
			(unsigned long long)promptWork.executedFallbackRanges,
			(unsigned long long)promptWork.fallbackRequestedCells,
			(unsigned long long)promptWork.fallbackAdmittedCells,
			(unsigned long long)promptWork.fallbackEmptyClosures,
			(unsigned long long)promptWork.fallbackPartialClosures,
			(unsigned long long)promptWork.fallbackClosedClosures,
			(unsigned long long)promptWork.gridHandoffs,
			(unsigned long long)promptWork.expiredFallbackRanges,
			(unsigned long long)promptWork.expiredFallbackMass,
			(unsigned long long)promptWork.expiredDeferredRanges,
			(unsigned long long)promptWork.expiredDeferredMass,
			(unsigned long long)promptWork.expiryAcknowledgeFailures,
			(unsigned long long)promptWork.authoredPendingMass,
			(unsigned long long)promptWork.committedDepositedMass,
			(unsigned long long)promptWork.fallbackCarrierMass,
			(unsigned long long)promptWork.promptDeferredRanges,
			(unsigned long long)promptWork.promptDeferredMass,
			(unsigned long long)promptWork.promptDeferredBrickWork,
			(unsigned long long)promptWork.rollbackCount, (unsigned long long)promptWork.internalErrors,
			(unsigned long long)promptWork.authoredRendererFrame,
			trailBridgeWork.activeSources, trailBridgeWork.highWaterSources,
			(unsigned long long)trailBridgeWork.replacements,
			(unsigned long long)trailBridgeWork.fallbackPublications,
			(unsigned long long)trailBridgeWork.handoffsCommitted);
		Printf("PERF pt smoke hash health NRI: observe_renderer_frame=%llu observe_frame=%u joined=%u grid_valid=%u renderer_frame=%llu frame=%u epoch=%u delta_valid=%u hash_capacity=%u hash_empty=%u hash_claimed=%u hash_resident=%u hash_new=%u hash_tombstone=%u hash_invalid_state=%u hash_invalid_mapping=%u control_probe_total=%u control_probe_delta=%u control_probe_max=%u probe_bin_1_total=%u probe_bin_1_delta=%u probe_bin_2_4_total=%u probe_bin_2_4_delta=%u probe_bin_5_8_total=%u probe_bin_5_8_delta=%u probe_bin_9_16_total=%u probe_bin_9_16_delta=%u probe_bin_17_24_total=%u probe_bin_17_24_delta=%u lookup_probe_total=%u lookup_probe_delta=%u insertion_probe_total=%u insertion_probe_delta=%u lookup_probe_limit_failures_total=%u lookup_probe_limit_failures_delta=%u insertion_probe_limit_failures_total=%u insertion_probe_limit_failures_delta=%u insertion_capacity_failures_total=%u insertion_capacity_failures_delta=%u insertion_active_failures_total=%u insertion_active_failures_delta=%u reclaim_invalid_mapping_failures_total=%u reclaim_invalid_mapping_failures_delta=%u hash_rebuild_attempts_total=%u hash_rebuild_attempts_delta=%u hash_rebuild_successes_total=%u hash_rebuild_successes_delta=%u hash_rebuild_failures_total=%u hash_rebuild_failures_delta=%u compact=1\n",
			(unsigned long long)renderer.mFrameBuffer->mFrameIndex, renderer.mFrameIndex, joined ? 1u : 0u,
			gridWork.gpuStatsValid ? 1u : 0u, (unsigned long long)gridWork.gpuRendererFrame,
			gridWork.gpu.frameStamp, gridWork.gpu.generation, gridWork.gpuFrameDeltaValid ? 1u : 0u,
			gridWork.gpu.hashCapacity, gridWork.gpu.hashEmpty, gridWork.gpu.hashClaimed,
			gridWork.gpu.hashResident, gridWork.gpu.hashNew, gridWork.gpu.hashTombstone,
			gridWork.gpu.hashInvalidState, gridWork.gpu.hashInvalidMapping,
			gridWork.gpu.controlProbeTotal, gridWork.gpuFrameDelta.controlProbeTotal,
			gridWork.gpu.maximumProbe, gridWork.gpu.controlProbeBin1, gridWork.gpuFrameDelta.controlProbeBin1,
			gridWork.gpu.controlProbeBin2To4, gridWork.gpuFrameDelta.controlProbeBin2To4,
			gridWork.gpu.controlProbeBin5To8, gridWork.gpuFrameDelta.controlProbeBin5To8,
			gridWork.gpu.controlProbeBin9To16, gridWork.gpuFrameDelta.controlProbeBin9To16,
			gridWork.gpu.controlProbeBin17To24, gridWork.gpuFrameDelta.controlProbeBin17To24,
			gridWork.gpu.lookupProbeTotal, gridWork.gpuFrameDelta.lookupProbeTotal,
			gridWork.gpu.insertionProbeTotal, gridWork.gpuFrameDelta.insertionProbeTotal,
			gridWork.gpu.lookupProbeLimitFailures, gridWork.gpuFrameDelta.lookupProbeLimitFailures,
			gridWork.gpu.insertionProbeLimitFailures, gridWork.gpuFrameDelta.insertionProbeLimitFailures,
			gridWork.gpu.insertionCapacityFailures, gridWork.gpuFrameDelta.insertionCapacityFailures,
			gridWork.gpu.insertionActiveFailures, gridWork.gpuFrameDelta.insertionActiveFailures,
			gridWork.gpu.reclaimInvalidMappingFailures, gridWork.gpuFrameDelta.reclaimInvalidMappingFailures,
			gridWork.gpu.hashRebuildAttempts, gridWork.gpuFrameDelta.hashRebuildAttempts,
			gridWork.gpu.hashRebuildSuccesses, gridWork.gpuFrameDelta.hashRebuildSuccesses,
			gridWork.gpu.hashRebuildFailures, gridWork.gpuFrameDelta.hashRebuildFailures);
		Printf("PERF pt smoke first use NRI: observe_renderer_frame=%llu observe_frame=%u joined=%u grid_valid=%u renderer_frame=%llu frame=%u epoch=%u delta_valid=%u core_capacity=%u borrowed_resident=%u borrowed_allocations_total=%u borrowed_allocations_delta=%u borrowed_returns_total=%u borrowed_returns_delta=%u borrowed_promotions_total=%u borrowed_promotions_delta=%u borrowed_reclaims_total=%u borrowed_reclaims_delta=%u replacement_admissions_total=%u replacement_admissions_delta=%u blocked_no_borrowed_total=%u blocked_no_borrowed_delta=%u blocked_visible_total=%u blocked_visible_delta=%u blocked_probe_total=%u blocked_probe_delta=%u blocked_invalid_total=%u blocked_invalid_delta=%u capacity_failures_total=%u capacity_failures_delta=%u compact=1\n",
			(unsigned long long)renderer.mFrameBuffer->mFrameIndex, renderer.mFrameIndex, joined ? 1u : 0u,
			gridWork.gpuStatsValid ? 1u : 0u, (unsigned long long)gridWork.gpuRendererFrame,
			gridWork.gpu.frameStamp, gridWork.gpu.generation, gridWork.gpuFrameDeltaValid ? 1u : 0u,
			gridWork.gpu.firstUseCoreCapacity, gridWork.gpu.borrowedResident,
			gridWork.gpu.borrowedAllocations, gridWork.gpuFrameDelta.borrowedAllocations,
			gridWork.gpu.borrowedReturns, gridWork.gpuFrameDelta.borrowedReturns,
			gridWork.gpu.borrowedPromotions, gridWork.gpuFrameDelta.borrowedPromotions,
			gridWork.gpu.borrowedReclaims, gridWork.gpuFrameDelta.borrowedReclaims,
			gridWork.gpu.firstUseReplacementAdmissions, gridWork.gpuFrameDelta.firstUseReplacementAdmissions,
			gridWork.gpu.firstUseBlockedNoBorrowed, gridWork.gpuFrameDelta.firstUseBlockedNoBorrowed,
			gridWork.gpu.firstUseBlockedVisible, gridWork.gpuFrameDelta.firstUseBlockedVisible,
			gridWork.gpu.firstUseBlockedProbe, gridWork.gpuFrameDelta.firstUseBlockedProbe,
			gridWork.gpu.firstUseBlockedInvalid, gridWork.gpuFrameDelta.firstUseBlockedInvalid,
			gridWork.gpu.firstUseCapacityFailures, gridWork.gpuFrameDelta.firstUseCapacityFailures);
		Printf("PERF pt smoke admission NRI: observe_renderer_frame=%llu observe_frame=%u renderer_frame=%llu frame=%u gathered=%u uploaded=%u deferred=%u coalesced=%u expired=%u rejected=%u sources=%u interactive_gathered=%u interactive_uploaded=%u estimated_bricks_gathered=%llu estimated_bricks_uploaded=%llu closure=%u compact=1\n",
			(unsigned long long)renderer.mFrameBuffer->mFrameIndex, renderer.mFrameIndex,
			(unsigned long long)mStatus.admissionRendererFrame, mStatus.admissionFrame,
			mStatus.admission.gathered, mStatus.admission.uploaded,
			mStatus.admission.boundedDeferred, mStatus.admission.coalesced, mStatus.admission.expired,
			mStatus.admission.rejected, mStatus.admission.sourceCount,
			mStatus.admission.interactiveGathered, mStatus.admission.interactiveUploaded,
			(unsigned long long)mStatus.admission.estimatedBrickWorkGathered,
			(unsigned long long)mStatus.admission.estimatedBrickWorkUploaded,
			mStatus.admission.Closes() ? 1u : 0u);
		Printf("PERF pt smoke grid admission NRI: observe_renderer_frame=%llu observe_frame=%u valid=%u renderer_frame=%llu frame=%u epoch=%u sources=%u requested=%u existing=%u admitted=%u rejected=%u rejected_capacity=%u rejected_probe=%u rejected_invalid=%u footprint_culled=%u compact=1\n",
			(unsigned long long)renderer.mFrameBuffer->mFrameIndex, renderer.mFrameIndex,
			gridWork.gpuStatsValid ? 1u : 0u, (unsigned long long)gridWork.gpuRendererFrame, gridWork.gpu.frameStamp,
			gridWork.gpu.generation, gridWork.gpu.admissionSourceCount,
			gridWork.gpu.admissionRequested, gridWork.gpu.admissionExisting,
			gridWork.gpu.admissionAdmitted, gridWork.gpu.admissionRejected,
			gridWork.gpu.admissionCapacityRejected, gridWork.gpu.admissionProbeRejected,
			gridWork.gpu.admissionInvalidRejected, gridWork.gpu.admissionFootprintCulled);
		for (const NRISmokeGridSourceStatusSnapshot& source : gridWork.sources)
		{
			Printf("PERF pt smoke grid source NRI: renderer_frame=%llu frame=%u epoch=%u source_id=%08x source_class=%u priority=%u commands=%u requested=%u existing=%u admitted=%u rejected_capacity=%u rejected_probe=%u rejected_invalid=%u footprint_culled=%u deposition_cells=%u requested_mass_q=%u deposited_mass_q=%u rejected_mass_q=%u admitted_key_hash=%08x compact=1\n",
				(unsigned long long)gridWork.gpuRendererFrame, gridWork.gpu.frameStamp,
				gridWork.gpu.generation, source.sourceId,
				source.sourceClass, source.priority, source.commands, source.requestedBricks,
				source.existingHits, source.admittedNew, source.rejectedCapacity,
				source.rejectedProbe, source.rejectedInvalid, source.footprintCulled, source.depositionCells,
				source.requestedMassQ, source.depositedMassQ, source.rejectedMassQ,
				source.admittedKeyHash);
		}
	}
	AppendSyntheticCommand(renderer);
	return RecordSimulation(renderer);
}

bool NRISmokeSystem::RecordSimulation(NRIRenderer& renderer)
{
	if (mLastSimulatedFrame == renderer.mFrameIndex)
		return true;
	NRIScopedGpuTiming gpuTiming(renderer.mFrameBuffer, NRIGpuTimingScope::SmokeSimulation);
	mLastSimulatedFrame = renderer.mFrameIndex;
	const NRISmokeAuthorityMode authorityMode = mAuthority.GetSnapshot().mode;
	const bool particleAuthority = authorityMode == NRISmokeAuthorityMode::Particles || authorityMode == NRISmokeAuthorityMode::Compare;
	const bool gridAuthority = authorityMode == NRISmokeAuthorityMode::Grid || authorityMode == NRISmokeAuthorityMode::Compare;
	mStatus.particleSimulationDispatches = 0u;
	mStatus.gridSimulationDispatches = 0u;
	mStatus.particleOpticalDispatches = 0u;
	mStatus.gridOpticalDispatches = 0u;
	mStatus.particleCommandsRouted = 0u;
	mStatus.gridCommandsRouted = 0u;
	const double now = PlayClock > 0 ? (double)PlayClock / 120.0 : 0.0;
	const float elapsed = mLastGameplaySeconds < 0.0 ? 0.0f : (float)std::max(0.0, std::min(0.25, now - mLastGameplaySeconds));
	mLastGameplaySeconds = now;
	mAccumulator += elapsed * mSettings.timeScale;
	const float step = 1.0f / (float)mSettings.simulationRate;
	const uint32_t dueSubsteps = (uint32_t)std::min<double>(
		std::floor(mAccumulator / step), (double)UINT32_MAX);
	const NRISmokeWorkTable& workTable = mWorkScheduler.GetSnapshot().table;
	const uint32_t substeps = std::min(dueSubsteps, workTable.simulationSubsteps);
	mAccumulator -= (float)substeps * step;
	const uint32_t debtSubsteps = (uint32_t)std::min<double>(
		std::floor(mAccumulator / step), (double)UINT32_MAX);
	mWorkScheduler.RecordSimulation(dueSubsteps, substeps, debtSubsteps);
	mStatus.simulationSubsteps = substeps;
	mPromptSimulationSeconds += (double)substeps * (double)step * (double)mSettings.timeScale;
	if (particleAuthority)
		mParticleSimulationSeconds += (double)substeps * (double)step * (double)mSettings.timeScale;
	NRISmokeAnalyticLightPolicy analyticLightPolicy = {};
	analyticLightPolicy.enabled = mSettings.emissiveLights &&
		workTable.analyticLightEvents != 0u;
	analyticLightPolicy.maximumEventBuilds = workTable.analyticLightEvents;
	analyticLightPolicy.anchorsPerEvent = workTable.analyticLightAnchors;
	analyticLightPolicy.samplesPerAnchor =
		workTable.analyticLightSamples == NRISmokeWorkTable::Unrestricted
		? mSettings.lightSamples : workTable.analyticLightSamples;
	mAnalyticTrailBridge.BeginFrame(now, mStatus.simulationEpoch);
	std::vector<NRISmokePromptBridgeOutcome> bridgeOutcomes;
	mPromptFallback.CommitGridHandoffs(mPulseOwner, mGrid.ConsumePromptOutcomes(),
		&bridgeOutcomes);
	std::vector<uint64_t> bridgeRetirements;
	for (const NRISmokePromptBridgeOutcome& outcome : bridgeOutcomes)
	{
		if (outcome.kind == NRISmokePromptBridgeOutcomeKind::Fallback)
			mAnalyticTrailBridge.PublishFallback(outcome.sourceKey,
				outcome.segmentRevision, outcome.epoch);
		else if (mAnalyticTrailBridge.CommitExactGridRange(outcome.sourceKey,
			outcome.segmentRevision, outcome.epoch, outcome.rangeCount))
			bridgeRetirements.push_back(outcome.sourceKey);
	}

	// Authored pulses become persistent before selection. The immutable plan is
	// committed only after every authoritative consumer has recorded it.
	mPulseOwner.Enqueue(mPendingCommands, mPendingPulseEnqueueInfo,
		mPromptSimulationSeconds, now);
	mPendingCommands.clear();
	mPendingPulseEnqueueInfo.clear();
	mPromptFallback.RetireExpired(mPulseOwner, mPromptSimulationSeconds, mStyles);
	mPulseOwner.ExpireStale(now);
	for (const NRISmokeAnalyticTrailObservationBatch& batch : mPendingTrailObservations)
		mAnalyticTrailBridge.Observe(batch.observation, batch.points.data(),
			batch.points.size());
	mPendingTrailObservations.clear();

	mAnalyticCarriers.BeginFrame(now, workTable.analyticCarriers, analyticLightPolicy);
	for (const uint64_t sourceKey : bridgeRetirements)
		mAnalyticCarriers.RetireLatest(sourceKey);
	uint32_t analyticRequested = (uint32_t)mPendingAnalyticRequests.size();
	uint32_t analyticAdmitted = 0u;
	for (uint32_t requestIndex = 0u; requestIndex < mPendingAnalyticRequests.size();)
	{
		const NRISmokeAnalyticCarrierRequest& request = mPendingAnalyticRequests[requestIndex];
		const uint32_t batchCount = request.batchIndex == 0u ? std::min(request.batchCount,
			(uint32_t)mPendingAnalyticRequests.size() - requestIndex) : 1u;
		analyticAdmitted += mAnalyticCarriers.AdmitBatch(
			mPendingAnalyticRequests.data() + requestIndex, batchCount);
		requestIndex += batchCount;
	}
	mPendingAnalyticRequests.clear();
	for (const NRISmokeAnalyticTrailPresentation& presentation :
		mAnalyticTrailBridge.GetPresentations())
	{
		analyticRequested++;
		analyticAdmitted += mAnalyticCarriers.AdmitLatest(presentation.carrier).Accepted() ? 1u : 0u;
	}
	mWorkScheduler.RecordAnalytic(analyticRequested, analyticAdmitted);
	mStatus.analytic = mAnalyticCarriers.GetSnapshot();

	const auto& availableCommands = mPulseOwner.PendingCommands();
	const uint32_t maximumCommands = std::min(kMaxCommands, workTable.emissionCommands);
	if (gridAuthority && !particleAuthority)
	{
		mStatus.admission = mAdmissionScheduler.SelectGridCommands(availableCommands, maximumCommands,
			renderer.mFrameIndex, mSettings.gridCellSize, mSelectedGridCommands);
		mStatus.admission.boundedDeferred = mStatus.admission.rejected;
		mStatus.admission.rejected = 0u;
	}
	else
	{
		mStatus.admission = {};
		mStatus.admission.gathered = (uint32_t)availableCommands.size();
		mStatus.admission.uploaded = std::min(mStatus.admission.gathered, maximumCommands);
		mStatus.admission.boundedDeferred = mStatus.admission.gathered - mStatus.admission.uploaded;
		mSelectedGridCommands.assign(availableCommands.begin(),
			availableCommands.begin() + mStatus.admission.uploaded);
		for (uint32_t i = 0; i < mStatus.admission.gathered; ++i)
		{
			const auto& command = availableCommands[i];
			const uint32_t cost = NRIEstimateSmokeCommandBrickWork(command, mSettings.gridCellSize);
			mStatus.admission.estimatedBrickWorkGathered += cost;
			if (i < mStatus.admission.uploaded)
				mStatus.admission.estimatedBrickWorkUploaded += cost;
			if (NRIIsInteractiveSmokeSource(command.sourceMetadata))
			{
				mStatus.admission.interactiveGathered++;
				if (i < mStatus.admission.uploaded) mStatus.admission.interactiveUploaded++;
			}
		}
	}
	std::vector<NRISmokePromptRangeIdentity> retainedPromptIdentities;
	uint32_t promptRequested = 0u;
	for (const auto& command : mSelectedGridCommands)
		if (NRIIsInteractiveSmokeSource(command.sourceMetadata)) promptRequested++;
	if (gridAuthority && !particleAuthority)
	{
		const NRISmokePromptPrepareResult promptResult = mPromptFallback.Prepare(mSelectedGridCommands,
			mPulseOwner, renderer.mFrameBuffer->mFrameIndex, mPromptSimulationSeconds, mStyles,
			mSettings.gridCellSize,
			retainedPromptIdentities,
			workTable.firstUseSources);
		mStatus.admission.uploaded -= std::min(mStatus.admission.uploaded, promptResult.deferredRanges);
		mStatus.admission.boundedDeferred += promptResult.deferredRanges;
		mStatus.admission.interactiveUploaded -= std::min(mStatus.admission.interactiveUploaded,
			promptResult.deferredRanges);
		mStatus.admission.estimatedBrickWorkUploaded -=
			std::min(mStatus.admission.estimatedBrickWorkUploaded, promptResult.deferredBrickWork);
	}
	uint32_t promptScheduled = 0u;
	for (const auto& command : mSelectedGridCommands)
		if (NRIIsInteractiveSmokeSource(command.sourceMetadata)) promptScheduled++;
	mWorkScheduler.RecordPrompt(promptRequested, promptScheduled);
	mWorkScheduler.RecordEmission((uint32_t)availableCommands.size(), (uint32_t)mSelectedGridCommands.size());
	if (!mPulseOwner.Plan(mSelectedGridCommands, mPendingCommands, mPulsePlanToken))
		return false;
	std::vector<NRISmokeInjectionCommandGpu> retainedPromptCommands;
	for (const auto& command : mPendingCommands)
		if ((command.sourceMetadata & NRI_SMOKE_SOURCE_METADATA_PROMPT_ELIGIBLE) != 0u)
			retainedPromptCommands.push_back(command);
	const std::vector<NRISmokeInjectionCommandGpu>* frameCommands = &mPendingCommands;
	auto rollbackPulsePlan = [&]()
	{
		if (mPulsePlanToken != 0u) mPulseOwner.Rollback(mPulsePlanToken);
		mPulsePlanToken = 0u;
		mPendingCommands.clear();
		mSelectedGridCommands.clear();
		mPromptFallback.Rollback();
	};
	mStatus.admissionFrame = renderer.mFrameIndex;
	mStatus.admissionRendererFrame = renderer.mFrameBuffer->mFrameIndex;
	CommandSlot& slot = mCommandSlots[std::min(renderer.mFrameBuffer->mCurrentQueuedFrameIndex, (uint32_t)mCommandSlots.size() - 1)];
	const uint32_t commandCount = std::min((uint32_t)frameCommands->size(), maximumCommands);
	const std::vector<NRISmokeAnalyticCarrierGpu>& analyticGpu = mAnalyticCarriers.GetGpuCarriers();
	const uint32_t analyticCount = (uint32_t)analyticGpu.size();
	mStatus.commandsUploaded = commandCount;
	mStatus.commandsUploadedTotal += commandCount;
	mStatus.styleCount = (uint32_t)mStyles.size();
	mStatus.commandsDropped += mStatus.admission.rejected;
	if (commandCount > 0u && particleAuthority)
	{
		mMayHaveParticleSmoke = true;
		for (uint32_t i = 0; i < commandCount; ++i)
		{
			const uint32_t styleIndex = (*frameCommands)[i].styleIndex;
			if (styleIndex < mStyles.size())
				mLatestParticleDeathSeconds = std::max(mLatestParticleDeathSeconds,
					mParticleSimulationSeconds + (double)std::max(mStyles[styleIndex].lifetime, 0.0f));
		}
	}
	else if (particleAuthority && mMayHaveParticleSmoke && mParticleSimulationSeconds >= mLatestParticleDeathSeconds)
	{
		mMayHaveParticleSmoke = false;
	}
	else if (!particleAuthority)
	{
		mMayHaveParticleSmoke = false;
		mLatestParticleDeathSeconds = 0.0;
	}
	mStatus.particleCommandsRouted = particleAuthority ? commandCount : 0u;
	mStatus.gridCommandsRouted = gridAuthority ? commandCount : 0u;
	if (commandCount > 0 && !UploadBytes(renderer, slot.upload, frameCommands->data(), (uint64_t)commandCount * sizeof(NRISmokeInjectionCommandGpu)))
	{
		rollbackPulsePlan();
		return false;
	}
	if (analyticCount > 0u && !UploadBytes(renderer, slot.analyticUpload, analyticGpu.data(),
		(uint64_t)analyticCount * sizeof(NRISmokeAnalyticCarrierGpu)))
	{
		rollbackPulsePlan();
		return false;
	}
	if (!UploadBytes(renderer, slot.styleUpload, mStyles.data(), mStyles.size() * sizeof(NRISmokeStyleGpu)))
	{
		rollbackPulsePlan();
		return false;
	}

	const bool firstWorldUse = !mResourcesInitialized;
	const bool firstParticleResourceUse = !mParticleResourcesInitialized;
	const bool firstSlotUse = !slot.initialized;
	nri::BufferBarrierDesc uploadBarriers[6] = {};
	uploadBarriers[0].buffer = slot.styleUpload.buffer; uploadBarriers[0].after = NRIResourceCopySourceAccess();
	uploadBarriers[1].buffer = mStyleBuffer.buffer; uploadBarriers[1].before = firstWorldUse ? nri::AccessStage{} : NRIResourceComputeShaderResourceAccess(); uploadBarriers[1].after = NRIResourceCopyDestinationAccess();
	uploadBarriers[2].buffer = slot.upload.buffer; uploadBarriers[2].after = NRIResourceCopySourceAccess();
	uploadBarriers[3].buffer = slot.device.buffer; uploadBarriers[3].before = firstSlotUse ? nri::AccessStage{} : NRIResourceComputeShaderResourceAccess(); uploadBarriers[3].after = NRIResourceCopyDestinationAccess();
	uploadBarriers[4].buffer = slot.analyticUpload.buffer; uploadBarriers[4].after = NRIResourceCopySourceAccess();
	uploadBarriers[5].buffer = slot.analyticDevice.buffer; uploadBarriers[5].before = firstSlotUse ? nri::AccessStage{} : NRIResourceComputeShaderResourceAccess(); uploadBarriers[5].after = NRIResourceCopyDestinationAccess();
	nri::BarrierDesc uploadBarrier = {}; uploadBarrier.buffers = uploadBarriers; uploadBarrier.bufferNum = 6;
	renderer.mFrameBuffer->mCore.CmdBarrier(*renderer.mFrameBuffer->mCommandBuffer, uploadBarrier);
	renderer.mFrameBuffer->mCore.CmdCopyBuffer(*renderer.mFrameBuffer->mCommandBuffer, *mStyleBuffer.buffer, 0, *slot.styleUpload.buffer, 0, mStyles.size() * sizeof(NRISmokeStyleGpu));
	if (commandCount > 0)
		renderer.mFrameBuffer->mCore.CmdCopyBuffer(*renderer.mFrameBuffer->mCommandBuffer, *slot.device.buffer, 0, *slot.upload.buffer, 0, (uint64_t)commandCount * sizeof(NRISmokeInjectionCommandGpu));
	if (analyticCount > 0u)
		renderer.mFrameBuffer->mCore.CmdCopyBuffer(*renderer.mFrameBuffer->mCommandBuffer, *slot.analyticDevice.buffer, 0, *slot.analyticUpload.buffer, 0, (uint64_t)analyticCount * sizeof(NRISmokeAnalyticCarrierGpu));

	std::vector<NRIBufferResource*> persistentStorage = { &mParticles, &mControl, &mReferenceNext,
		&mParticleDirectionalVisibility, &mCompatibilityStorage };
	std::vector<NRIBufferResource*> viewStorage = { &mFineCells, &mFroxelMedium, &mFroxelIntegrated,
		&mWideCells, &mGlobalDepthCells, &mFroxelPhase, &mFroxelSource, &mOccupiedFroxelIndices,
		&mIndirectHistory, &mIndirectScratch, &mEmissiveCurrent, &mEmissiveTemporal, &mEmissiveHistory,
		&mDirectCurrent, &mDirectHistory, &mAnalyticTileHeaders, &mAnalyticTileIndices,
		&mAnalyticFroxelMedium, &mAnalyticEmissiveA, &mAnalyticEmissiveB };
	std::vector<nri::BufferBarrierDesc> compute;
	compute.reserve(2u + persistentStorage.size() + viewStorage.size());
	nri::BufferBarrierDesc styleBarrier = {};
	styleBarrier.buffer = mStyleBuffer.buffer;
	styleBarrier.before = NRIResourceCopyDestinationAccess();
	styleBarrier.after = NRIResourceComputeShaderResourceAccess();
	compute.push_back(styleBarrier);
	nri::BufferBarrierDesc commandBarrier = {};
	commandBarrier.buffer = slot.device.buffer;
	commandBarrier.before = NRIResourceCopyDestinationAccess();
	commandBarrier.after = NRIResourceComputeShaderResourceAccess();
	compute.push_back(commandBarrier);
	nri::BufferBarrierDesc analyticBarrier = {};
	analyticBarrier.buffer = slot.analyticDevice.buffer;
	analyticBarrier.before = NRIResourceCopyDestinationAccess();
	analyticBarrier.after = NRIResourceComputeShaderResourceAccess();
	compute.push_back(analyticBarrier);
	auto appendStorageTransition = [&](NRIBufferResource* resource, bool initialized)
	{
		if (resource == nullptr || resource->buffer == nullptr)
			return;
		nri::BufferBarrierDesc transition = {};
		transition.buffer = resource->buffer;
		transition.before = initialized ? StorageAccess() : nri::AccessStage{};
		if (resource == &mControl && mControlCopyPending)
			transition.before = NRIResourceCopySourceAccess();
		transition.after = StorageAccess();
		compute.push_back(transition);
	};
	appendStorageTransition(&mControl, !firstWorldUse);
	appendStorageTransition(&mParticles, !firstParticleResourceUse);
	appendStorageTransition(&mReferenceNext, !firstParticleResourceUse);
	appendStorageTransition(&mParticleDirectionalVisibility, !firstParticleResourceUse);
	appendStorageTransition(&mCompatibilityStorage, !firstParticleResourceUse);
	for (NRIBufferResource* resource : viewStorage) appendStorageTransition(resource, mViewResourcesInitialized);
	mControlCopyPending = false;
	slot.initialized = true;
	mResourcesInitialized = true;
	mParticleResourcesInitialized = true;
	mViewResourcesInitialized = true;
	nri::BarrierDesc computeBarrier = {}; computeBarrier.buffers = compute.data(); computeBarrier.bufferNum = (uint32_t)compute.size();
	renderer.mFrameBuffer->mCore.CmdBarrier(*renderer.mFrameBuffer->mCommandBuffer, computeBarrier);

	const nri::Descriptor* inputs[] = { mStyleBuffer.shaderView, slot.device.shaderView, slot.analyticDevice.shaderView };
	const nri::Descriptor* particleView = mParticles.storageView != nullptr ? mParticles.storageView : mCompatibilityParticleView;
	const nri::Descriptor* cellView = mFineCells.storageView != nullptr ? mFineCells.storageView : mCompatibilityCellView;
	const nri::Descriptor* wideCellView = mWideCells.storageView != nullptr ? mWideCells.storageView : mCompatibilityCellView;
	const nri::Descriptor* globalCellView = mGlobalDepthCells.storageView != nullptr ? mGlobalDepthCells.storageView : mCompatibilityCellView;
	const nri::Descriptor* scalarView = mCompatibilityStorage.storageView;
	const nri::Descriptor* referenceView = mReferenceNext.storageView != nullptr ? mReferenceNext.storageView : scalarView;
	const nri::Descriptor* directionalView = mParticleDirectionalVisibility.storageView != nullptr ? mParticleDirectionalVisibility.storageView : scalarView;
	std::array<const nri::Descriptor*, kSmokeStorageDescriptorCount> outputs = { particleView, mControl.storageView, cellView, referenceView,
		mFroxelMedium.storageView, mFroxelIntegrated.storageView, mWideCells.storageView, mGlobalDepthCells.storageView, mFroxelPhase.storageView,
		mFroxelSource.storageView, mOccupiedFroxelIndices.storageView, mIndirectHistory.storageView, mIndirectScratch.storageView,
		directionalView, mEmissiveCurrent.storageView, mEmissiveTemporal.storageView, mEmissiveHistory.storageView };
	outputs[6] = wideCellView;
	outputs[7] = globalCellView;
	std::array<const nri::Descriptor*, NRISmokeGrid::EvaluationDescriptorCount> gridDescriptors = {};
	if (!mGrid.GetEvaluationStorageDescriptors(gridDescriptors))
		gridDescriptors.fill(mControl.storageView);
	std::copy(gridDescriptors.begin(), gridDescriptors.end(), outputs.begin() + kSmokeCoreStorageBufferCount);
	outputs[kSmokeCoreStorageBufferCount + NRISmokeGrid::EvaluationDescriptorCount] = mDirectCurrent.storageView;
	outputs[kSmokeCoreStorageBufferCount + NRISmokeGrid::EvaluationDescriptorCount + 1u] = mDirectHistory.storageView;
	std::array<const nri::Descriptor*, NRISmokeGridLighting::StorageDescriptorCount> gridLightingDescriptors = {};
	if (!mGridLighting.GetStorageDescriptors(gridLightingDescriptors))
		gridLightingDescriptors.fill(mControl.storageView);
	std::copy(gridLightingDescriptors.begin(), gridLightingDescriptors.end(),
		outputs.begin() + kSmokeCoreStorageBufferCount + NRISmokeGrid::EvaluationDescriptorCount + kSmokeDirectStorageBufferCount);
	outputs[kSmokeViewStorageBase] = mControl.storageView;
	outputs[kSmokeViewStorageBase + 1u] = mControl.storageView;
	outputs[kSmokeViewStorageBase + 2u] = mControl.storageView;
	outputs[kSmokePromptStorageBase] = mGrid.GetPromptOutcomeDescriptor() != nullptr ?
		mGrid.GetPromptOutcomeDescriptor() : mControl.storageView;
	outputs[kSmokeAnalyticStorageBase] = mAnalyticTileHeaders.storageView;
	outputs[kSmokeAnalyticStorageBase + 1u] = mAnalyticTileIndices.storageView;
	outputs[kSmokeAnalyticStorageBase + 2u] = mAnalyticFroxelMedium.storageView;
	outputs[kSmokeAnalyticStorageBase + 3u] = mAnalyticEmissiveA.storageView;
	outputs[kSmokeAnalyticStorageBase + 4u] = mAnalyticEmissiveB.storageView;
	std::array<const nri::Descriptor*, NRISmokeDormantGrid::EvaluationDescriptorCount>
		dormantDescriptors = {};
	if (!mDormantGrid.GetEvaluationStorageDescriptors(dormantDescriptors))
		dormantDescriptors.fill(mControl.storageView);
	std::copy(dormantDescriptors.begin(), dormantDescriptors.end(),
		outputs.begin() + kSmokeDormantStorageBase);
	nri::UpdateDescriptorRangeDesc updates[2] = {};
	updates[0].descriptorSet = slot.inputSet; updates[0].rangeIndex = 0; updates[0].descriptors = inputs; updates[0].descriptorNum = 3;
	updates[1].descriptorSet = slot.bufferSet; updates[1].rangeIndex = 0; updates[1].descriptors = outputs.data(); updates[1].descriptorNum = kSmokeStorageDescriptorCount;
	renderer.mFrameBuffer->mCore.UpdateDescriptorRanges(updates, 2);

	NRISmokeConstants constants = {};
	constants.frameIndex = renderer.mFrameIndex;
	constants.simulationEpoch = mStatus.simulationEpoch;
	constants.particleCapacity = mResourceParticleCapacity;
	constants.commandCount = commandCount;
	constants.styleCount = (uint32_t)mStyles.size();
	constants.froxelWidth = mResourceFroxelWidth;
	constants.froxelHeight = mResourceFroxelHeight;
	constants.froxelDepth = mResourceFroxelDepth;
	constants.directionalColorPacked = 0u;
	constants.deltaTime = step;
	constants.timeScale = mSettings.timeScale;
	std::copy(mSettings.wind, mSettings.wind + 3, constants.wind);
	auto dispatch = [&](NRISmokePass pass, uint32_t groups)
	{
		renderer.mFrameBuffer->mCore.CmdBeginAnnotation(*renderer.mFrameBuffer->mCommandBuffer,
			kSmokePipelineNames[(uint32_t)pass], nri::BGRA_UNUSED);
		constants.pass = (uint32_t)pass;
		renderer.mFrameBuffer->mCore.CmdSetRootConstants(*renderer.mFrameBuffer->mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
		renderer.mFrameBuffer->mCore.CmdSetPipeline(*renderer.mFrameBuffer->mCommandBuffer, *mPipelines[(uint32_t)pass]);
		renderer.mFrameBuffer->mCore.CmdDispatch(*renderer.mFrameBuffer->mCommandBuffer, { groups, 1, 1 });
		renderer.mFrameBuffer->mCore.CmdEndAnnotation(*renderer.mFrameBuffer->mCommandBuffer);
	};
	auto storageBarrier = [&]()
	{
		std::vector<nri::BufferBarrierDesc> barriers;
		barriers.reserve(persistentStorage.size() + viewStorage.size());
		auto append = [&](NRIBufferResource* resource)
		{
			if (resource == nullptr || resource->buffer == nullptr)
				return;
			nri::BufferBarrierDesc item = {};
			item.buffer = resource->buffer;
			item.before = StorageAccess();
			item.after = StorageAccess();
			barriers.push_back(item);
		};
		for (NRIBufferResource* resource : persistentStorage) append(resource);
		for (NRIBufferResource* resource : viewStorage) append(resource);
		nri::BarrierDesc barrier = {}; barrier.buffers = barriers.data(); barrier.bufferNum = (uint32_t)barriers.size();
		renderer.mFrameBuffer->mCore.CmdBarrier(*renderer.mFrameBuffer->mCommandBuffer, barrier);
	};
	renderer.mFrameBuffer->mCore.CmdSetPipelineLayout(*renderer.mFrameBuffer->mCommandBuffer, nri::BindPoint::COMPUTE, *mPipelineLayout);
	renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 0, slot.inputSet, nri::BindPoint::COMPUTE });
	renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 1, slot.bufferSet, nri::BindPoint::COMPUTE });
	if (mNeedsClear)
	{
		constants.flags = 1u | (gridAuthority && !particleAuthority ? kSmokeFlagGridRepresentation : 0u);
		const uint64_t froxelCount = (uint64_t)mResourceFroxelWidth * mResourceFroxelHeight * mResourceFroxelDepth;
		const uint64_t particleCount = particleAuthority ? (uint64_t)mResourceParticleCapacity : 0u;
		const uint64_t wideCellCount = particleAuthority ? (uint64_t)kWideCellCount * mResourceFroxelDepth : 0u;
		dispatch(NRISmokePass::Clear, Groups(std::max({ particleCount, froxelCount, wideCellCount, (uint64_t)mResourceFroxelDepth })));
		storageBarrier();
		mNeedsClear = false;
	}
	constants.flags = 0;
	// Compare mode validates the grid prerequisite before mutating particles.
	// A failed grid record can therefore retain and retry the command batch
	// without spawning the particle side twice.
	if (gridAuthority)
	{
		const NRISmokeWorkTable& workTable = mWorkScheduler.GetSnapshot().table;
		NRISmokeDormantGridConfig dormantConfig = {};
		dormantConfig.enabled = mSettings.dormantGrid;
		dormantConfig.archiveCapacity = 256u;
		dormantConfig.maximumDemotionsPerFrame = workTable.dormantArchives;
		dormantConfig.maximumPromotionsPerFrame = workTable.dormantPromotions;
		dormantConfig.maximumEvolutionPerFrame = workTable.dormantEvolution;
		dormantConfig.maximumContinuousInjectionsPerFrame = workTable.dormantEvolution;
		dormantConfig.densityHalfLifeScale = mSettings.gridDensityHalfLifeScale;
		dormantConfig.coolingScale = mSettings.gridCoolingScale;
		const NRISmokeGridServices gridServices = BuildGridServices(renderer);
		auto buildDormantFrame = [&]()
		{
			NRISmokeDormantGridFrameDesc frame = {};
			frame.frameIndex = renderer.mFrameIndex;
			frame.simulationEpoch = mStatus.simulationEpoch;
			frame.deltaTime = step * (float)substeps * mSettings.timeScale;
			std::copy(renderer.mCurrentCameraPos, renderer.mCurrentCameraPos + 3,
				frame.cameraPosition);
			mGrid.GetDormantTransactionStorageDescriptors(frame.fine.storage);
			mGrid.GetDormantTransactionStorageBuffers(frame.fine.buffers);
			const NRISmokeGridStatusSnapshot& grid = mGrid.GetStatusSnapshot();
			frame.fine.brickCapacity = grid.brickCapacity;
			frame.fine.hashCapacity = grid.hashCapacity;
			frame.fine.cellCapacity = grid.cellCapacity;
			frame.fine.fieldPing = mGrid.GetFieldPing();
			frame.fine.activePing = mGrid.GetActivePing();
			frame.fine.cellSize = mSettings.gridCellSize;
			frame.demotions = mDormantDemotions.empty() ? nullptr : mDormantDemotions.data();
			frame.demotionCount = (uint32_t)mDormantDemotions.size();
			frame.promotions = mDormantPromotions.empty() ? nullptr : mDormantPromotions.data();
			frame.promotionCount = (uint32_t)mDormantPromotions.size();
			frame.injections = mDormantInjectionBuild.injections.empty() ? nullptr :
				mDormantInjectionBuild.injections.data();
			frame.injectionCount = (uint32_t)mDormantInjectionBuild.injections.size();
			return frame;
		};
		if (dormantConfig.enabled && !mDormantPromotions.empty())
		{
			const NRISmokeDormantGridFrameDesc dormantFrame = buildDormantFrame();
			if (!mDormantGrid.RecordPromotions(gridServices, dormantConfig, dormantFrame))
				dormantConfig.enabled = false;
		}
		NRISmokeGridFrameDesc gridFrame = {};
		gridFrame.frameIndex = renderer.mFrameIndex;
		gridFrame.simulationEpoch = mStatus.simulationEpoch;
		gridFrame.commandCount = commandCount;
		gridFrame.styleCount = (uint32_t)mStyles.size();
		gridFrame.simulationSubsteps = substeps;
		gridFrame.hashHealthDiagnostic = mSettings.readback;
		gridFrame.spatialObservationReadback = dormantConfig.enabled;
		gridFrame.simulationStep = step;
		gridFrame.styleView = mStyleBuffer.shaderView;
		gridFrame.commandView = slot.device.shaderView;
		if (!mGrid.RecordFrame(gridServices, mSettings, gridFrame))
		{
			rollbackPulsePlan();
			mStatus.gridReady = false;
			mStatus.representationFallback = "grid-record-retry";
			mStatus.authorityReason = "grid-record-retry";
			mStatus.authorityPreparation = "failed-retained";
			mStatus.authorityOperational = false;
			mStatus.gridCommandsRouted = 0u;
			return true;
		}
		if (dormantConfig.enabled)
		{
			const NRISmokeDormantGridFrameDesc dormantFrame = buildDormantFrame();
			if (!mDormantGrid.RecordDemotions(gridServices, dormantConfig, dormantFrame) ||
				!mDormantGrid.RecordEvolution(gridServices, dormantConfig, dormantFrame) ||
				!mDormantGrid.RecordReadback(gridServices, dormantFrame))
				dormantConfig.enabled = false;
		}
		mStatus.dormantGrid = mDormantGrid.GetStatusSnapshot();
		mStatus.gridSimulationDispatches++;
	}
	for (uint32_t i = 0; particleAuthority && i < substeps; ++i)
	{
		dispatch(NRISmokePass::Simulate, Groups(mResourceParticleCapacity));
		mStatus.particleSimulationDispatches++;
		storageBarrier();
	}
	if (particleAuthority && commandCount > 0)
	{
		dispatch(NRISmokePass::Spawn, Groups(commandCount));
		mStatus.particleSimulationDispatches++;
		storageBarrier();
	}
	const bool pulseCommitSucceeded = mPulsePlanToken == 0u ||
		(retainedPromptCommands.empty() ? mPulseOwner.Commit(mPulsePlanToken) :
			mPulseOwner.CommitRetaining(mPulsePlanToken, retainedPromptCommands));
	if (!pulseCommitSucceeded)
	{
		mPulseOwner.Reset();
		mPulsePlanToken = 0u;
		mPendingCommands.clear();
		mSelectedGridCommands.clear();
		return false;
	}
	mPulsePlanToken = 0u;
	mPromptFallback.Commit(renderer.mFrameBuffer->mFrameIndex, mPulseOwner);
	if (mSettings.traceMode >= 2u)
	{
		const NRISmokeWorkSchedulerSnapshot& work = mWorkScheduler.GetSnapshot();
		Printf("PERF pt smoke schedule NRI: renderer_frame=%llu frame=%u epoch=%u profile_requested=%u profile_effective=%u profile_name=%s revision=%u supported=%08x enforced=%08x froxel_pixels=%u froxel_depth=%u emissive_lights=%u emissive_backend=%u light_samples=%u light_candidates=%u emission_limit=%u emission_requested=%u emission_scheduled=%u emission_deferred=%u first_use_limit=%u first_use_requested=%u first_use_scheduled=%u first_use_deferred=%u analytic_limit=%u analytic_requested=%u analytic_scheduled=%u analytic_dropped=%u analytic_active=%u analytic_high_water=%u analytic_expired=%llu analytic_stale=%llu analytic_capacity_drop=%llu radiance_new_limit=%u radiance_maintenance_limit=%u dormant_archive_limit=%u dormant_promote_limit=%u dormant_evolve_limit=%u simulation_limit=%u simulation_due=%u simulation_scheduled=%u simulation_deferred=%u simulation_debt=%u simulation_debt_max=%u simulation_capped_consecutive=%u simulation_capped_total=%llu unsupported_unrestricted=%u compact=1\n",
			(unsigned long long)renderer.mFrameBuffer->mFrameIndex, renderer.mFrameIndex, mStatus.simulationEpoch,
			work.requestedProfile, (uint32_t)work.effectiveProfile,
			NRISmokeWorkScheduler::ProfileName(work.effectiveProfile), work.table.revision,
			work.table.supportedCapabilities, work.table.enforcedCapabilities,
			mSettings.froxelPixelSize, mSettings.froxelDepth,
			mSettings.emissiveLights ? 1u : 0u, mSettings.emissiveBackend,
			mSettings.lightSamples, mSettings.maxLightCandidates,
			work.table.emissionCommands, work.emissionRequested, work.emissionScheduled, work.emissionDeferred,
			work.table.firstUseSources, work.promptRequested, work.promptScheduled, work.promptDeferred,
			work.table.analyticCarriers, work.analyticRequested, work.analyticScheduled, work.analyticDropped,
			mStatus.analytic.activeQuantity, mStatus.analytic.highWaterQuantity,
			(unsigned long long)mStatus.analytic.expired,
			(unsigned long long)mStatus.analytic.droppedStaleOnArrival,
			(unsigned long long)mStatus.analytic.droppedCapacity,
			work.table.radianceNewInvalidCells, work.table.radianceMaintenanceCells,
			work.table.dormantArchives, work.table.dormantPromotions,
			work.table.dormantEvolution,
			work.table.simulationSubsteps, work.simulationDueSubsteps, work.simulationScheduledSubsteps,
			work.simulationDeferredSubsteps, work.simulationDebtSubsteps,
			work.simulationMaximumDebtSubsteps, work.simulationConsecutiveCappedFrames,
			(unsigned long long)work.simulationCappedFrames, NRISmokeWorkTable::Unrestricted);
		const NRISmokeDormantGridStatusSnapshot& dormant = mStatus.dormantGrid;
		const NRISmokeSpatialInterestSnapshot& spatial = mStatus.spatialResidency;
		Printf("PERF pt smoke dormant NRI: renderer_frame=%llu frame=%u epoch=%u enabled=%u resources=%u archive_capacity=%u archive_resident=%u archive_free=%u authority_cpu=%u pending_demotions=%u spatial_observed=%u spatial_hot=%u spatial_warm=%u spatial_dormant=%u demotion_eligible=%u demotion_selected=%u promotion_eligible=%u promotion_selected=%u projected_recovered=%u selected_recovered=%u archive_submitted=%u archive_clipped=%u archive_attempts=%u archive_published=%u archive_retained=%u archive_full=%u archive_hash_fail=%u archive_stale=%u archive_validation_fail=%u promote_submitted=%u promote_clipped=%u promote_attempts=%u promote_published=%u promote_retained=%u promote_capacity=%u promote_hash_fail=%u promote_stale=%u evolution_limit=%u evolution_attempts=%u evolution_executed=%u evolution_skipped=%u injection_requests=%u injection_sources=%u injection_records=%u injection_invalid=%u injection_missing_authority=%u injection_promotion_conflict=%u injection_capacity_reject=%u injection_submitted=%u injection_clipped=%u injection_attempts=%u injection_applied=%u injection_rejected=%u injection_missing=%u injection_stale=%u injection_cadence=%u injection_cells=%u gpu_valid=%u gpu_frame=%llu resident_bytes=%llu payload_bytes=%llu policy=fixed-profile-no-headroom compact=1\n",
			(unsigned long long)renderer.mFrameBuffer->mFrameIndex, renderer.mFrameIndex,
			mStatus.simulationEpoch, mSettings.dormantGrid ? 1u : 0u,
			dormant.resourcesReady ? 1u : 0u, dormant.archiveCapacity,
			dormant.gpu.residentCount, dormant.gpu.freeCount,
			(uint32_t)mDormantAuthorities.size(), (uint32_t)mDormantPendingDemotions.size(),
			spatial.observed, spatial.hot, spatial.warm, spatial.dormant,
			spatial.eligibleDemotions, (uint32_t)spatial.demotions.size(),
			spatial.eligiblePromotions, (uint32_t)spatial.promotions.size(),
			spatial.projectedRecoveredBricks, spatial.selectedRecoveredBricks,
			dormant.submittedDemotions, dormant.clippedDemotions,
			dormant.gpu.archiveAttempts, dormant.gpu.archivePublished,
			dormant.gpu.archiveRetainedFine, dormant.gpu.archiveFull,
			dormant.gpu.archiveHashFailures, dormant.gpu.archiveStale,
			dormant.gpu.archiveValidationFailures, dormant.submittedPromotions,
			dormant.clippedPromotions, dormant.gpu.rehydrateAttempts,
			dormant.gpu.rehydratePublished, dormant.gpu.rehydrateRetainedCoarse,
			dormant.gpu.rehydrateFineCapacity, dormant.gpu.rehydrateHashFailures,
			dormant.gpu.rehydrateStale, work.table.dormantEvolution,
			dormant.gpu.evolutionAttempts, dormant.gpu.evolutionWorkExecuted,
			dormant.gpu.evolutionSkipped, mDormantInjectionBuild.requests,
			mDormantInjectionBuild.routedSources,
			(uint32_t)mDormantInjectionBuild.injections.size(),
			mDormantInjectionBuild.invalidRequests,
			mDormantInjectionBuild.missingAuthorities,
			mDormantInjectionBuild.promotionConflicts,
			mDormantInjectionBuild.capacityRejected, dormant.submittedInjections,
			dormant.clippedInjections, dormant.gpu.injectionAttempts,
			dormant.gpu.injectionApplied, dormant.gpu.injectionRejected,
			dormant.gpu.injectionMissing, dormant.gpu.injectionStale,
			dormant.gpu.injectionCadenceSteps, dormant.gpu.injectionCells,
			dormant.gpuStatsValid ? 1u : 0u,
			(unsigned long long)dormant.gpuRendererFrame,
			(unsigned long long)dormant.residentBytes,
			(unsigned long long)dormant.payloadBytes);
	}
	mPendingCommands.clear();
	mPendingPulseEnqueueInfo.clear();
	mPendingTrailObservations.clear();
	mSelectedGridCommands.clear();
	mPendingAnalyticRequests.clear();
	return true;
}

bool NRISmokeSystem::RecordVolume(NRIRenderer& renderer, const NRISmokeRouteDesc& route)
{
	NRIScopedGpuTiming gpuTiming(renderer.mFrameBuffer, NRIGpuTimingScope::SmokeVolume);
	CommandSlot& slot = mCommandSlots[std::min(renderer.mFrameBuffer->mCurrentQueuedFrameIndex, (uint32_t)mCommandSlots.size() - 1)];
	NRITextureResource& input = renderer.GetFrameTexture(route.inputSlot);
	NRITextureResource& depth = renderer.GetFrameTexture(route.depthSlot);
	NRITextureResource& output = renderer.GetFrameTexture(route.outputSlot);
	NRITextureResource& volumeCurrent = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::SmokeVolumeCurrent);
	NRITextureResource& volumeCurrentMeta = renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::SmokeVolumeCurrentMeta);
	const bool writePing = (renderer.mFrameIndex & 1u) == 0u;
	NRITextureResource& volumeHistoryRead = renderer.GetFrameTexture(writePing ? NRIRenderer::FrameTextureSlot::SmokeVolumeHistoryPong : NRIRenderer::FrameTextureSlot::SmokeVolumeHistoryPing);
	NRITextureResource& volumeHistoryWrite = renderer.GetFrameTexture(writePing ? NRIRenderer::FrameTextureSlot::SmokeVolumeHistoryPing : NRIRenderer::FrameTextureSlot::SmokeVolumeHistoryPong);
	NRITextureResource& volumeMetaRead = renderer.GetFrameTexture(writePing ? NRIRenderer::FrameTextureSlot::SmokeVolumeMetaPong : NRIRenderer::FrameTextureSlot::SmokeVolumeMetaPing);
	NRITextureResource& volumeMetaWrite = renderer.GetFrameTexture(writePing ? NRIRenderer::FrameTextureSlot::SmokeVolumeMetaPing : NRIRenderer::FrameTextureSlot::SmokeVolumeMetaPong);
	if (input.shaderView == nullptr || depth.shaderView == nullptr || output.storageView == nullptr ||
		volumeCurrent.shaderView == nullptr || volumeCurrent.storageView == nullptr ||
		volumeCurrentMeta.shaderView == nullptr || volumeCurrentMeta.storageView == nullptr ||
		volumeHistoryRead.shaderView == nullptr || volumeHistoryWrite.shaderView == nullptr || volumeHistoryWrite.storageView == nullptr ||
		volumeMetaRead.shaderView == nullptr || volumeMetaWrite.shaderView == nullptr || volumeMetaWrite.storageView == nullptr)
		return false;
	const bool standardExtent = (route.placement == NRISmokeRoutePlacement::StandardPreUpscale ||
		route.placement == NRISmokeRoutePlacement::DlrrPreUpscaleMainInput) &&
		route.width == renderer.mRenderWidth && route.height == renderer.mRenderHeight &&
		input.width == route.width && input.height == route.height &&
		depth.width == route.width && depth.height == route.height &&
		output.width == route.width && output.height == route.height;
	if (!standardExtent)
	{
		mStatus.routeSupported = false;
		if (mSettings.traceMode > 0u)
		{
			Printf("NRI PT smoke route rejected: placement=%u route=%ux%u render=%ux%u input=%ux%u depth=%ux%u output=%ux%u\n",
				(uint32_t)route.placement, route.width, route.height, renderer.mRenderWidth, renderer.mRenderHeight,
				input.width, input.height, depth.width, depth.height, output.width, output.height);
		}
		if (input.width == output.width && input.height == output.height)
		{
			renderer.CopyTexture(input, output);
			return true;
		}
		return false;
	}
	renderer.mFrameBuffer->TransitionTexture(input, { nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE, nri::StageBits::COMPUTE_SHADER });
	renderer.mFrameBuffer->TransitionTexture(depth, { nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE, nri::StageBits::COMPUTE_SHADER });
	renderer.mFrameBuffer->TransitionTexture(output, { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::Layout::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER });
	renderer.mFrameBuffer->TransitionTexture(volumeHistoryRead, { nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE, nri::StageBits::COMPUTE_SHADER });
	renderer.mFrameBuffer->TransitionTexture(volumeMetaRead, { nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE, nri::StageBits::COMPUTE_SHADER });
	renderer.mFrameBuffer->TransitionTexture(volumeCurrent, { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::Layout::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER });
	renderer.mFrameBuffer->TransitionTexture(volumeCurrentMeta, { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::Layout::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER });
	renderer.mFrameBuffer->TransitionTexture(volumeHistoryWrite, { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::Layout::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER });
	renderer.mFrameBuffer->TransitionTexture(volumeMetaWrite, { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::Layout::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER });
	const nri::Descriptor* textures[] = { input.shaderView, depth.shaderView, volumeHistoryRead.shaderView, volumeMetaRead.shaderView,
		volumeHistoryWrite.shaderView, volumeMetaWrite.shaderView, volumeCurrent.shaderView, volumeCurrentMeta.shaderView };
	const nri::Descriptor* outputTextures[] = { output.storageView, volumeCurrent.storageView, volumeCurrentMeta.storageView,
		volumeHistoryWrite.storageView, volumeMetaWrite.storageView };
	const nri::Descriptor* lightBuffers[] = {
		renderer.mSceneDataDescriptors[10],
		renderer.mSceneDataDescriptors[11],
		renderer.mSceneDataDescriptors[12],
	};
	const bool lightBuffersReady = lightBuffers[0] != nullptr && lightBuffers[1] != nullptr && lightBuffers[2] != nullptr;
	const nri::Descriptor* filteredSceneBuffers[] = {
		renderer.mSceneDataDescriptors[2], renderer.mSceneDataDescriptors[3],
		renderer.mSceneDataDescriptors[6], renderer.mSceneDataDescriptors[7],
		renderer.mSceneDataDescriptors[8], renderer.mSceneDataDescriptors[9],
		renderer.mSceneDataDescriptors[23], renderer.mSceneDataDescriptors[24],
	};
	const bool filteredBuffersReady = std::all_of(std::begin(filteredSceneBuffers), std::end(filteredSceneBuffers), [](const nri::Descriptor* descriptor) { return descriptor != nullptr; });
	const nri::Descriptor* emissiveSceneBuffers[] = {
		renderer.mSceneDataDescriptors[0], renderer.mSceneDataDescriptors[4], renderer.mSceneDataDescriptors[21],
		renderer.mSceneDataDescriptors[13], renderer.mSceneDataDescriptors[14], renderer.mSceneDataDescriptors[15],
		renderer.mSceneDataDescriptors[25],
	};
	const bool emissiveBuffersReady = std::all_of(std::begin(emissiveSceneBuffers), std::end(emissiveSceneBuffers), [](const nri::Descriptor* descriptor) { return descriptor != nullptr; });
	const bool filteredTexturesReady = renderer.mCurrentSceneTextureDescriptors.size() >= 514u && renderer.mCurrentSceneTextureDescriptors[0] != nullptr;
	const NRIWorldTlasFrameSlot& worldTlasFrameSlot = renderer.GetCurrentWorldTlasFrameSlot();
	const nri::Descriptor* worldTlasDescriptor = worldTlasFrameSlot.accelerationStructure.descriptor;
	const bool shadowReady = worldTlasDescriptor != nullptr;
	const nri::Descriptor* indirectSceneBuffers[] = {
		renderer.mSceneDataDescriptors[16], renderer.mSceneDataDescriptors[17], renderer.mSceneDataDescriptors[18]
	};
	const bool sectorLightResourcesReady = indirectSceneBuffers[0] != nullptr && indirectSceneBuffers[1] != nullptr && renderer.mBoundSectorLightSectorCount > 0u;
	const bool reprojectionResourcesReady = indirectSceneBuffers[2] != nullptr;
	const nri::Descriptor* smokeSky[] = {
		renderer.mCurrentSceneTextureDescriptors.size() > 1u ? renderer.mCurrentSceneTextureDescriptors[1] : nullptr
	};
	const bool skyResourceReady = smokeSky[0] != nullptr;
	std::array<const nri::Descriptor*, kSmokeExtendedSceneBufferCount> extendedSceneBuffers = {};
	std::copy(std::begin(emissiveSceneBuffers), std::end(emissiveSceneBuffers), extendedSceneBuffers.begin());
	for (uint32_t i = 0; i < 3u; ++i)
		extendedSceneBuffers[kSmokeEmissiveSceneBufferCount + i] = indirectSceneBuffers[i] != nullptr ? indirectSceneBuffers[i] : emissiveSceneBuffers[0];
	const bool extendedSceneBuffersReady = emissiveBuffersReady;
	const bool filteredResourcesReady = filteredBuffersReady && filteredTexturesReady && skyResourceReady && extendedSceneBuffersReady;
	const bool emissiveResourcesReady = filteredResourcesReady && emissiveBuffersReady && renderer.mBoundEmissivePrimitiveCount > 0u;
	const bool indirectResourcesReady = filteredResourcesReady && shadowReady && sectorLightResourcesReady && skyResourceReady;
	std::array<const nri::Descriptor*, 514> smokeSceneTextures = {};
	if (filteredTexturesReady && skyResourceReady)
	{
		smokeSceneTextures[0] = renderer.mCurrentSceneTextureDescriptors[0];
		smokeSceneTextures[1] = smokeSky[0];
		std::copy(renderer.mCurrentSceneTextureDescriptors.begin() + 2, renderer.mCurrentSceneTextureDescriptors.begin() + 514, smokeSceneTextures.begin() + 2);
	}
	const nri::Descriptor* filteredSamplers[] = {
		renderer.mFrameBuffer->mSamplers[(size_t)NRISamplerMode::WrapPoint],
		renderer.mFrameBuffer->mSamplers[(size_t)NRISamplerMode::WrapLinear],
		renderer.mFrameBuffer->mSamplers[(size_t)NRISamplerMode::ClampPoint],
	};
	nri::UpdateDescriptorRangeDesc updates[11] = {};
	updates[0].descriptorSet = slot.textureSet; updates[0].rangeIndex = 0; updates[0].descriptors = textures; updates[0].descriptorNum = 8;
	updates[1].descriptorSet = slot.outputSet; updates[1].rangeIndex = 0; updates[1].descriptors = outputTextures; updates[1].descriptorNum = 5;
	uint32_t updateCount = 2;
	if (lightBuffersReady)
	{
		updates[2].descriptorSet = slot.lightSet; updates[2].rangeIndex = 0; updates[2].descriptors = lightBuffers; updates[2].descriptorNum = 3;
		updateCount++;
	}
	if (filteredResourcesReady)
	{
		updates[updateCount].descriptorSet = slot.filteredSceneSet; updates[updateCount].rangeIndex = 0; updates[updateCount].descriptors = filteredSceneBuffers; updates[updateCount].descriptorNum = kSmokeFilteredSceneBufferCount; updateCount++;
		if (extendedSceneBuffersReady)
		{
			updates[updateCount].descriptorSet = slot.filteredSceneSet; updates[updateCount].rangeIndex = 1; updates[updateCount].descriptors = extendedSceneBuffers.data(); updates[updateCount].descriptorNum = kSmokeExtendedSceneBufferCount; updateCount++;
		}
		updates[updateCount].descriptorSet = slot.filteredSceneSet; updates[updateCount].rangeIndex = 2; updates[updateCount].descriptors = smokeSceneTextures.data(); updates[updateCount].descriptorNum = (uint32_t)smokeSceneTextures.size(); updateCount++;
		updates[updateCount].descriptorSet = slot.filteredSceneSet; updates[updateCount].rangeIndex = 3; updates[updateCount].descriptors = filteredSamplers; updates[updateCount].descriptorNum = 3; updateCount++;
	}
	const nri::Descriptor* worldTlas[] = { worldTlasDescriptor };
	if (shadowReady)
	{
		updates[updateCount].descriptorSet = slot.filteredSceneSet; updates[updateCount].rangeIndex = 4; updates[updateCount].descriptors = worldTlas; updates[updateCount].descriptorNum = 1; updateCount++;
	}
	renderer.mFrameBuffer->mCore.UpdateDescriptorRanges(updates, updateCount);

	NRISmokeConstants constants = {};
	constants.frameIndex = renderer.mFrameIndex;
	constants.simulationEpoch = mStatus.simulationEpoch;
	constants.particleCapacity = mResourceParticleCapacity;
	constants.styleCount = (uint32_t)mStyles.size();
	constants.froxelWidth = mResourceFroxelWidth;
	constants.froxelHeight = mResourceFroxelHeight;
	constants.froxelDepth = mResourceFroxelDepth;
	uint64_t emissivePayloadHash = renderer.mEmissiveSamplingPayloadHash;
	if (renderer.ShouldUseSceneDataFrameRing())
	{
		const NRIRenderer::SceneDataFrameSlot& frameSlot = renderer.GetCurrentSceneDataFrameSlot();
		if (frameSlot.emissiveSamplingPayloadValid)
			emissivePayloadHash = frameSlot.emissiveSamplingPayloadHash;
	}
	// Candidate payload values and dynamic geometry legitimately change every
	// frame. Stable distribution keys guard candidate identity while this
	// epoch rejects records across a smoke/world reset; using the full payload
	// hash here would disable temporal reuse during ordinary gameplay.
	const uint32_t emissiveGeneration = std::max(1u, mStatus.simulationEpoch);
	constants.commandCount = emissiveGeneration;
	constants.directionalColorPacked = PackDirectionalLightColor24(renderer.mDirectionalLightState.color);
	constants.renderWidth = renderer.mRenderWidth;
	constants.renderHeight = renderer.mRenderHeight;
	constants.outputWidth = route.width;
	constants.outputHeight = route.height;
	constants.froxelMaxDistance = mSettings.froxelMaxDistance;
	constants.depthExponent = 2.0f;
	constants.densityScale = mSettings.densityScale;
	constants.radianceScale = mSettings.radianceScale;
	constants.deltaTime = mSettings.emissiveSourceClamp;
	constants.indirectScale = mSettings.indirectScale;
	constants.tanHalfFovX = renderer.mCurrentTanHalfFovX;
	constants.tanHalfFovY = renderer.mCurrentTanHalfFovY;
	std::copy(renderer.mCurrentCameraPos, renderer.mCurrentCameraPos + 3, constants.cameraPosition);
	std::copy(renderer.mCurrentCameraForward, renderer.mCurrentCameraForward + 3, constants.cameraForward);
	std::copy(renderer.mCurrentCameraRight, renderer.mCurrentCameraRight + 3, constants.cameraRight);
	std::copy(renderer.mCurrentCameraUp, renderer.mCurrentCameraUp + 3, constants.cameraUp);
	constants.directionalDirectionX = renderer.mDirectionalLightState.direction[0];
	constants.directionalDirectionY = renderer.mDirectionalLightState.direction[1];
	constants.directionalDirectionZ = renderer.mDirectionalLightState.direction[2];
	constants.directionalAngularSize = std::clamp(renderer.mDirectionalLightState.angularSize, 0.001f, 1.2f);
	std::copy(renderer.mCurrentJitter, renderer.mCurrentJitter + 2, constants.currentJitter);
	NRIPopulateSmokeVisualConstants(mSettings.visuals, constants);
	const bool fieldDiagnostics = mSettings.debugMode >= 12u;
	uint64_t visualHistoryHash = NRIHashSmokeVisualSettings(mSettings.visuals);
	visualHistoryHash = HashCombine64(visualHistoryHash, mSettings.debugMode);
	const float scatterScale = mSettings.multipleScatter ? mSettings.multipleScatterScale : 0.0f;
	const uint32_t packedScatterScale = (uint32_t)std::lround((double)std::clamp(scatterScale, 0.0f, 16.0f) * (65535.0 / 16.0));
	const bool selfShadowEffective = !fieldDiagnostics && mSettings.selfShadow &&
		(mSettings.emissiveBackend == (uint32_t)NRISmokeEmissiveBackend::Legacy ||
		 mGridLighting.GetStatusSnapshot().selfShadowEffective);
	constants.debugMode = (mSettings.debugMode & 0xffu) |
		((mSettings.multipleScatter ? mSettings.multipleScatterDebug : 0u) << 8u) |
		((selfShadowEffective ? 1u : 0u) << 10u) |
		((selfShadowEffective ? mSettings.selfShadowDebug : 0u) << 11u) |
		(packedScatterScale << 16u);
	const bool pointLightsReady = !fieldDiagnostics && mSettings.pointLights && lightBuffersReady && renderer.mBoundRuntimeLightCount > 0;
	const bool directionalLightReady = !fieldDiagnostics && mSettings.directionalLight && renderer.mDirectionalLightState.enabled;
	const bool emissiveLightsReady = mSettings.emissiveLights && emissiveResourcesReady;
	constants.lightMode = !fieldDiagnostics && (pointLightsReady || directionalLightReady || emissiveLightsReady) ? mSettings.lightMode : 0u;
	constants.lightSamples = mSettings.lightSamples;
	constants.maxLightCandidates = mSettings.maxLightCandidates;
	constants.runtimeLightCount = pointLightsReady ? renderer.mBoundRuntimeLightCount : 0u;
	constants.runtimeLightTileCountX = pointLightsReady ? renderer.mBoundRuntimeLightTileCountX : 0u;
	constants.runtimeLightTileCountY = pointLightsReady ? renderer.mBoundRuntimeLightTileCountY : 0u;
	constants.lightSourceFlags =
		(pointLightsReady ? 0x1u : 0u) |
		(directionalLightReady ? 0x2u : 0u) |
		(directionalLightReady && renderer.mDirectionalLightState.shadow ? 0x4u : 0u) |
		(emissiveLightsReady ? 0x8u : 0u);
	if (mSettings.indirect && indirectResourcesReady)
		constants.lightSourceFlags |= 0x10u;
	const bool filteredVisibilityEffective = constants.lightMode >= 2u && mSettings.filteredVisibility && filteredResourcesReady && shadowReady;
	const uint32_t requestedEmissivePointCandidates = std::clamp(mSettings.emissivePointCandidates, 1u, 8u);
	const uint32_t effectiveEmissivePointCandidates = mSettings.emissiveReference ? 1u : requestedEmissivePointCandidates;
	const uint32_t emissiveCandidateTargetCode = mSettings.emissiveCandidateTarget >= 0 ?
		(uint32_t)mSettings.emissiveCandidateTarget + 1u : 0u;
	const uint32_t effectiveEmissiveEstimatorKey = effectiveEmissivePointCandidates |
		(mSettings.emissiveReference ? 0x100u : 0u) | (emissiveCandidateTargetCode << 16u);
	constants.filteredVisibilityEnabled =
		(filteredVisibilityEffective ? 1u : 0u) |
		(filteredResourcesReady ? 2u : 0u) |
		(shadowReady ? 4u : 0u) |
		(mSettings.filteredVisibility ? 8u : 0u) |
		(effectiveEmissivePointCandidates << 4u) |
		(std::min(BuildNRITraceSettingsFromCVars().portalDepth, 8u) << 8u) |
		(emissiveCandidateTargetCode << 16u);
	mStatus.requestedLightMode = mSettings.lightMode;
	mStatus.effectiveLightMode = constants.lightMode;
	mStatus.filteredVisibilityRequested = mSettings.filteredVisibility;
	mStatus.filteredVisibilityEffective = filteredVisibilityEffective;
	mStatus.forceOpaqueVisibility = constants.lightMode >= 2u && shadowReady && !filteredVisibilityEffective;
	mStatus.shadowTlasReady = shadowReady;
	uint32_t effectiveIndirectCacheMode = mSettings.indirectCacheMode;
	if (effectiveIndirectCacheMode >= 2u && !reprojectionResourcesReady)
		effectiveIndirectCacheMode = 1u;
	mStatus.indirectCacheModeRequested = mSettings.indirectCacheMode;
	mStatus.indirectCacheModeEffective = effectiveIndirectCacheMode;
	const uint64_t indirectSectorHash = renderer.mSectorLightingPayloadHash;
	const uint64_t indirectSkyKey = renderer.mSkyEnvironment.ActiveKey();
	const uint64_t indirectEmissiveHash = renderer.mEmissiveSamplingPayloadHash;
	const bool indirectHistoryCompatible = !fieldDiagnostics && mIndirectHistoryValid && !renderer.mResetHistory &&
		mLastSmokeVisualHash == visualHistoryHash &&
		mLastIndirectCacheMode == effectiveIndirectCacheMode &&
		mLastIndirectSectorHash == indirectSectorHash && mLastIndirectSkyKey == indirectSkyKey &&
		mLastIndirectEmissiveHash == indirectEmissiveHash;
	uint64_t directDirectionalHash = 1469598103934665603ull;
	directDirectionalHash = HashCombine64(directDirectionalHash, constants.directionalColorPacked);
	directDirectionalHash = HashCombine64(directDirectionalHash, FloatBits(constants.directionalDirectionX));
	directDirectionalHash = HashCombine64(directDirectionalHash, FloatBits(constants.directionalDirectionY));
	directDirectionalHash = HashCombine64(directDirectionalHash, FloatBits(constants.directionalDirectionZ));
	directDirectionalHash = HashCombine64(directDirectionalHash, FloatBits(constants.directionalAngularSize));
	directDirectionalHash = HashCombine64(directDirectionalHash, constants.lightSourceFlags & 0x7u);
	const bool gridRepresentationActive = mStatus.representationEffective != 0u;
	if (gridRepresentationActive && mSettings.dormantGrid &&
		mDormantGrid.GetStatusSnapshot().resourcesReady)
		constants.lightSourceFlags |= kSmokeLightSourceDormantGrid;
	const bool worldEmissiveRequested = gridRepresentationActive &&
		mSettings.emissiveBackend != (uint32_t)NRISmokeEmissiveBackend::Legacy;
	const bool worldEmissiveReady = worldEmissiveRequested && mGridLighting.IsWorldReady() && emissiveLightsReady;
	const bool worldEmissiveCompare = worldEmissiveReady &&
		mSettings.emissiveBackend == (uint32_t)NRISmokeEmissiveBackend::Compare;
	const uint32_t effectiveDirectReuseMode = gridRepresentationActive ? mSettings.directReuseMode : 0u;
	const uint32_t directVisibilityBackend = constants.filteredVisibilityEnabled & 0xfu;
	const uint32_t emissiveLaneCount = gridRepresentationActive ? (1u << std::min(mSettings.quality, 2u)) : 1u;
	const uint32_t emissiveVisibilityBackend = constants.filteredVisibilityEnabled & 0xfu;
	const bool directHistoryCompatible = !fieldDiagnostics && gridRepresentationActive && mDirectHistoryValid &&
		!renderer.mResetHistory && mLastDirectFrame + 1u == renderer.mFrameIndex &&
		mLastSmokeVisualHash == visualHistoryHash &&
		mLastDirectReuseMode == effectiveDirectReuseMode &&
		mLastDirectReferenceMode == mSettings.directReferenceMode &&
		mLastDirectQuality == std::min(mSettings.quality, 2u) &&
		mLastDirectLightMode == constants.lightMode &&
		mLastDirectLightSamples == constants.lightSamples && mLastDirectSimulationEpoch == mStatus.simulationEpoch &&
		mLastDirectVisibilityBackend == directVisibilityBackend && mLastDirectDirectionalHash == directDirectionalHash;
	constants.flags = (mSettings.readback || mSettings.traceMode > 0u) ? 2u : 0u;
	if (mStatus.representationEffective == 2u)
		constants.flags |= kSmokeFlagCompareRepresentation;
	if (gridRepresentationActive)
		constants.flags |= kSmokeFlagGridRepresentation;
	constants.flags |= (effectiveDirectReuseMode & 3u) << kSmokeFlagDirectReuseShift;
	constants.flags |= (mSettings.directReferenceMode & 3u) << kSmokeFlagDirectReferenceShift;
	if (directHistoryCompatible)
		constants.flags |= kSmokeFlagDirectHistoryValid;
	if (worldEmissiveReady)
		constants.flags |= kSmokeFlagGridLightingWorld;
	if (worldEmissiveCompare)
		constants.flags |= kSmokeFlagGridLightingCompare;
	if (worldEmissiveReady && mSettings.emissiveWorldFilter && mGridLighting.GetStatusSnapshot().filterAllocated)
		constants.flags |= kSmokeFlagGridLightingFilter;
	if (mSettings.emissiveLegacyGatherDisabled)
		constants.flags |= kSmokeFlagEmissiveLegacyGatherDisabled;
	if (mSettings.emissiveQuarterKey)
		constants.flags |= kSmokeFlagEmissiveQuarterKey;
	if (worldEmissiveReady && mGridLighting.GetFieldPing() != 0u)
		constants.flags |= kSmokeFlagGridLightingFieldPing;
	if (worldEmissiveReady)
		constants.flags |= (mSettings.emissiveWorldDebug & 7u) << kSmokeFlagGridLightingDebugShift;
	if (worldEmissiveReady && mSettings.emissiveLocalProposals)
		constants.flags |= kSmokeFlagGridLightingLocalProposals;
	constants.flags |= (effectiveIndirectCacheMode & 3u) << 2u;
	constants.flags |= (std::min(mSettings.quality, 2u) & 3u) << 5u;
	if (indirectHistoryCompatible)
		constants.flags |= 0x10u;
	else if (effectiveIndirectCacheMode > 0u)
		constants.flags |= 0x80u;
	const bool emissiveHistoryCompatible = !fieldDiagnostics && mEmissiveHistoryValid && !renderer.mResetHistory &&
		mLastSmokeVisualHash == visualHistoryHash &&
		mLastEmissiveFrame + 1u == renderer.mFrameIndex &&
		mLastEmissiveReuseMode == mSettings.emissiveReuseMode &&
		mLastEmissiveGeneration == emissiveGeneration &&
		mLastEmissiveRepresentation == mStatus.representationEffective &&
		mLastEmissiveLaneCount == emissiveLaneCount &&
		mLastEmissiveLightMode == constants.lightMode &&
		mLastEmissiveVisibilityBackend == emissiveVisibilityBackend;
	if (emissiveHistoryCompatible)
		constants.flags |= 0x100u;
	constants.flags |= (mSettings.emissiveReuseMode & 3u) << 9u;
	if (mSettings.emissiveReference)
		constants.flags |= 0x800u;
	mStatus.emissiveReuseModeRequested = mSettings.emissiveReuseMode;
	mStatus.emissiveReuseModeEffective = emissiveLightsReady ? mSettings.emissiveReuseMode : 0u;
	mStatus.emissiveLaneCount = emissiveLaneCount;
	mStatus.emissiveReference = emissiveLightsReady && mSettings.emissiveReference;
	mStatus.emissiveHistoryValid = emissiveHistoryCompatible;
	mStatus.directReuseModeRequested = mSettings.directReuseMode;
	mStatus.directReuseModeEffective = effectiveDirectReuseMode;
	mStatus.directReferenceMode = gridRepresentationActive ? mSettings.directReferenceMode : 0u;
	mStatus.directHistoryValid = directHistoryCompatible;
	if (directHistoryCompatible)
		mStatus.directHistoryResetReason = "none";
	else if (!gridRepresentationActive)
		mStatus.directHistoryResetReason = "particle-backend";
	else if (renderer.mResetHistory)
		mStatus.directHistoryResetReason = "renderer-reset";
	else if (!mDirectHistoryValid)
		mStatus.directHistoryResetReason = "uninitialized";
	else if (mLastDirectFrame + 1u != renderer.mFrameIndex)
		mStatus.directHistoryResetReason = "frame-gap";
	else if (mLastDirectSimulationEpoch != mStatus.simulationEpoch)
		mStatus.directHistoryResetReason = "smoke-reset";
	else if (mLastDirectDirectionalHash != directDirectionalHash)
		mStatus.directHistoryResetReason = "directional-change";
	else
		mStatus.directHistoryResetReason = "mode-change";
	// Ordinary emissive payload churn is handled by current-frame reservoir
	// evaluation and the volume neighborhood clamp. Resetting the final
	// volume layer for every animated/dynamic emissive would prevent history
	// from ever accumulating in a live scene.
	uint64_t volumeLightingHash = renderer.mSectorLightingPayloadHash ^
		(renderer.mSkyEnvironment.ActiveKey() * 0x9e3779b97f4a7c15ull);
	volumeLightingHash = HashCombine64(volumeLightingHash,
		worldEmissiveRequested ? effectiveEmissiveEstimatorKey : 0u);
	volumeLightingHash = HashCombine64(volumeLightingHash, visualHistoryHash);
	const bool volumeHistoryAllowed = mSettings.volumeHistory && !fieldDiagnostics;
	const bool volumeHistoryCompatible = volumeHistoryAllowed && mVolumeHistoryValid && mLastVolumeHistoryEnabled &&
		!renderer.mResetHistory && mLastVolumeFrame + 1u == renderer.mFrameIndex &&
		mLastVolumeWidth == route.width && mLastVolumeHeight == route.height &&
		mLastVolumePlacement == (uint32_t)route.placement && mLastVolumeSimulationEpoch == mStatus.simulationEpoch &&
		mLastVolumeLightingHash == volumeLightingHash;
	if (volumeHistoryAllowed)
		constants.flags |= 0x2000u;
	if (volumeHistoryCompatible)
		constants.flags |= 0x1000u;
	mStatus.volumeHistoryRequested = mSettings.volumeHistory;
	mStatus.volumeHistoryEffective = volumeHistoryAllowed && reprojectionResourcesReady;
	mStatus.volumeHistoryValid = volumeHistoryCompatible;
	mStatus.volumeHistoryAge = volumeHistoryCompatible ? std::min(mStatus.volumeHistoryAge + 1u, 255u) : 0u;
	mStatus.volumeResolvedSlot = (uint32_t)(writePing ? NRIRenderer::FrameTextureSlot::SmokeVolumeHistoryPing : NRIRenderer::FrameTextureSlot::SmokeVolumeHistoryPong);
	mStatus.volumeMetaSlot = (uint32_t)(writePing ? NRIRenderer::FrameTextureSlot::SmokeVolumeMetaPing : NRIRenderer::FrameTextureSlot::SmokeVolumeMetaPong);
	mStatus.volumeHistoryBytes = volumeCurrent.memorySize + volumeCurrentMeta.memorySize + volumeHistoryRead.memorySize +
		volumeHistoryWrite.memorySize + volumeMetaRead.memorySize + volumeMetaWrite.memorySize;
	if (volumeHistoryCompatible)
		mStatus.volumeHistoryResetReason = "none";
	else if (fieldDiagnostics)
		mStatus.volumeHistoryResetReason = "field-debug";
	else if (!mSettings.volumeHistory)
		mStatus.volumeHistoryResetReason = "disabled";
	else if (!reprojectionResourcesReady)
		mStatus.volumeHistoryResetReason = "missing-reprojection";
	else if (renderer.mResetHistory)
		mStatus.volumeHistoryResetReason = "renderer-reset";
	else if (!mVolumeHistoryValid)
		mStatus.volumeHistoryResetReason = "uninitialized";
	else if (!mLastVolumeHistoryEnabled)
		mStatus.volumeHistoryResetReason = "mode-change";
	else if (mLastVolumeFrame + 1u != renderer.mFrameIndex)
		mStatus.volumeHistoryResetReason = "frame-gap";
	else if (mLastVolumeWidth != route.width || mLastVolumeHeight != route.height)
		mStatus.volumeHistoryResetReason = "extent-change";
	else if (mLastVolumePlacement != (uint32_t)route.placement)
		mStatus.volumeHistoryResetReason = "route-change";
	else if (mLastVolumeSimulationEpoch != mStatus.simulationEpoch)
		mStatus.volumeHistoryResetReason = "smoke-reset";
	else if (mLastVolumeLightingHash != volumeLightingHash)
		mStatus.volumeHistoryResetReason = "lighting-change";
	else
		mStatus.volumeHistoryResetReason = "incompatible";
	auto dispatch = [&](NRISmokePass pass, uint32_t x, uint32_t y, uint32_t z)
	{
		renderer.mFrameBuffer->mCore.CmdBeginAnnotation(*renderer.mFrameBuffer->mCommandBuffer,
			kSmokePipelineNames[(uint32_t)pass], nri::BGRA_UNUSED);
		constants.pass = (uint32_t)pass;
		renderer.mFrameBuffer->mCore.CmdSetRootConstants(*renderer.mFrameBuffer->mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
		renderer.mFrameBuffer->mCore.CmdSetPipeline(*renderer.mFrameBuffer->mCommandBuffer, *mPipelines[(uint32_t)pass]);
		renderer.mFrameBuffer->mCore.CmdDispatch(*renderer.mFrameBuffer->mCommandBuffer, { x, y, z });
		renderer.mFrameBuffer->mCore.CmdEndAnnotation(*renderer.mFrameBuffer->mCommandBuffer);
	};
	auto dispatchIndirect = [&](NRISmokePass pass, const nri::Buffer& arguments, uint64_t byteOffset)
	{
		renderer.mFrameBuffer->mCore.CmdBeginAnnotation(*renderer.mFrameBuffer->mCommandBuffer,
			kSmokePipelineNames[(uint32_t)pass], nri::BGRA_UNUSED);
		constants.pass = (uint32_t)pass;
		renderer.mFrameBuffer->mCore.CmdSetRootConstants(*renderer.mFrameBuffer->mCommandBuffer,
			{ 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
		renderer.mFrameBuffer->mCore.CmdSetPipeline(*renderer.mFrameBuffer->mCommandBuffer,
			*mPipelines[(uint32_t)pass]);
		renderer.mFrameBuffer->mCore.CmdDispatchIndirect(*renderer.mFrameBuffer->mCommandBuffer,
			arguments, byteOffset);
		renderer.mFrameBuffer->mCore.CmdEndAnnotation(*renderer.mFrameBuffer->mCommandBuffer);
	};
	auto storageBarrier = [&]()
	{
		NRIBufferResource* resources[] = { &mParticles, &mControl, &mFineCells, &mReferenceNext,
			&mFroxelMedium, &mFroxelIntegrated, &mWideCells, &mGlobalDepthCells, &mFroxelPhase,
			&mFroxelSource, &mOccupiedFroxelIndices, &mIndirectHistory, &mIndirectScratch,
			&mParticleDirectionalVisibility, &mEmissiveCurrent, &mEmissiveTemporal, &mEmissiveHistory,
			&mDirectCurrent, &mDirectHistory, &mCompatibilityStorage, &mAnalyticTileHeaders,
			&mAnalyticTileIndices, &mAnalyticFroxelMedium, &mAnalyticEmissiveA,
			&mAnalyticEmissiveB };
		std::vector<nri::BufferBarrierDesc> barriers;
		barriers.reserve(std::size(resources));
		for (NRIBufferResource* resource : resources)
		{
			if (resource->buffer == nullptr)
				continue;
			nri::BufferBarrierDesc item = {};
			item.buffer = resource->buffer;
			item.before = StorageAccess();
			item.after = StorageAccess();
			barriers.push_back(item);
		}
		nri::BarrierDesc barrier = {}; barrier.buffers = barriers.data(); barrier.bufferNum = (uint32_t)barriers.size();
		renderer.mFrameBuffer->mCore.CmdBarrier(*renderer.mFrameBuffer->mCommandBuffer, barrier);
	};
	auto bindSmokePipeline = [&]()
	{
		renderer.mFrameBuffer->mCore.CmdSetPipelineLayout(*renderer.mFrameBuffer->mCommandBuffer, nri::BindPoint::COMPUTE, *mPipelineLayout);
		renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 0, slot.inputSet, nri::BindPoint::COMPUTE });
		renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 1, slot.bufferSet, nri::BindPoint::COMPUTE });
		renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 2, slot.textureSet, nri::BindPoint::COMPUTE });
		renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 3, slot.outputSet, nri::BindPoint::COMPUTE });
		if (lightBuffersReady)
			renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 4, slot.lightSet, nri::BindPoint::COMPUTE });
		if (filteredResourcesReady || shadowReady || sectorLightResourcesReady || skyResourceReady || reprojectionResourcesReady)
			renderer.mFrameBuffer->mCore.CmdSetDescriptorSet(*renderer.mFrameBuffer->mCommandBuffer, { 5, slot.filteredSceneSet, nri::BindPoint::COMPUTE });
	};
	bindSmokePipeline();
	const bool multipleScatterReady = gridRepresentationActive && mSettings.multipleScatter &&
		mGridLighting.GetStatusSnapshot().multipleScatterEffective;
	if ((worldEmissiveReady || multipleScatterReady) &&
		!mGridLighting.Record(BuildGridServices(renderer), mSettings,
			mWorkScheduler.GetSnapshot().table, constants, emissiveResourcesReady))
	{
		mStatus.representationFallback = "grid-lighting-record-retry";
		mStatus.authorityReason = "grid-lighting-record-retry";
		mStatus.authorityPreparation = "failed-retained";
		mStatus.authorityOperational = false;
		return false;
	}
	if ((worldEmissiveReady || multipleScatterReady) && mControl.buffer != nullptr)
	{
		// Grid-lighting diagnostics reuse six words in the established smoke
		// control buffer. Order those GPU writes before SmokeClear preserves the
		// current-frame values; no additional readback resource is introduced.
		nri::BufferBarrierDesc controlBarrier = {};
		controlBarrier.buffer = mControl.buffer;
		controlBarrier.before = StorageAccess();
		controlBarrier.after = StorageAccess();
		nri::BarrierDesc barrier = {};
		barrier.buffers = &controlBarrier;
		barrier.bufferNum = 1u;
		renderer.mFrameBuffer->mCore.CmdBarrier(*renderer.mFrameBuffer->mCommandBuffer, barrier);
	}
	const uint64_t froxelCount = (uint64_t)mResourceFroxelWidth * mResourceFroxelHeight * mResourceFroxelDepth;
	const NRISmokeAuthorityMode authorityMode = mAuthority.GetSnapshot().mode;
	const bool renderParticles = authorityMode == NRISmokeAuthorityMode::Particles || authorityMode == NRISmokeAuthorityMode::Compare;
	const bool renderGrid = authorityMode == NRISmokeAuthorityMode::Grid || authorityMode == NRISmokeAuthorityMode::Compare;
	const uint32_t analyticCount = (uint32_t)mAnalyticCarriers.GetGpuCarriers().size();
	const uint64_t analyticTileCount = ((uint64_t)mResourceFroxelWidth + 3u) / 4u *
		(((uint64_t)mResourceFroxelHeight + 3u) / 4u);
	const uint64_t wideCellCount = renderParticles ? (uint64_t)kWideCellCount * mResourceFroxelDepth : 0u;
	{
		NRIScopedGpuTiming timing(renderer.mFrameBuffer, NRIGpuTimingScope::SmokeViewPrepare);
		dispatch(NRISmokePass::Clear, Groups(std::max({ froxelCount, wideCellCount, (uint64_t)mResourceFroxelDepth })), 1, 1);
		storageBarrier();
	}
	if (analyticCount > 0u)
	{
		NRIScopedGpuTiming timing(renderer.mFrameBuffer, NRIGpuTimingScope::SmokeAnalyticMaterialize);
		dispatch(NRISmokePass::AnalyticClear, Groups(std::max(froxelCount, analyticTileCount)), 1, 1);
		storageBarrier();
	}
	bool viewComparatorPrepared = false;
	NRISmokeViewWorkOutputs viewOutputs = {};
	if (renderGrid && (mSettings.viewCompare || mSettings.viewRoute != 0u))
	{
		NRIScopedGpuTiming timing(renderer.mFrameBuffer, NRIGpuTimingScope::SmokeViewPrepare);
		std::array<const nri::Descriptor*, NRISmokeGrid::EvaluationDescriptorCount> grid = {};
		if (mGrid.GetEvaluationStorageDescriptors(grid) && mViewWork.Initialize(BuildGridServices(renderer)))
		{
			NRISmokeViewWorkFrame frame = {};
			frame.constants.frameIndex = renderer.mFrameIndex;
			frame.constants.simulationEpoch = mStatus.simulationEpoch;
			frame.constants.brickCapacity = mGrid.GetStatusSnapshot().brickCapacity;
			frame.constants.froxelWidth = mResourceFroxelWidth;
			frame.constants.froxelHeight = mResourceFroxelHeight;
			frame.constants.froxelDepth = mResourceFroxelDepth;
			const NRISmokeViewWorkLayout layout = NRISmokeViewWork::Describe(
				mResourceFroxelWidth, mResourceFroxelHeight, mResourceFroxelDepth,
				frame.constants.brickCapacity);
			frame.constants.tileCountX = layout.tileCountX;
			frame.constants.tileCountY = layout.tileCountY;
			frame.constants.fieldPing = mGrid.GetFieldPing();
			frame.constants.cellSize = mSettings.gridCellSize;
			// Discovery is an exact output-safety gate, not a lifecycle/quality
			// threshold. Any represented optical coefficient must remain eligible.
			frame.constants.opticalThreshold = 0.0f;
			frame.constants.executionRoute = mSettings.viewRoute;
			frame.constants.froxelCapacity = (uint32_t)froxelCount;
			frame.constants.froxelMaxDistance = mSettings.froxelMaxDistance;
			frame.constants.depthExponent = constants.depthExponent;
			frame.constants.tanHalfFovX = constants.tanHalfFovX;
			frame.constants.tanHalfFovY = constants.tanHalfFovY;
			std::copy(constants.cameraPosition, constants.cameraPosition + 3, frame.constants.cameraPosition);
			std::copy(constants.cameraForward, constants.cameraForward + 3, frame.constants.cameraForward);
			std::copy(constants.cameraRight, constants.cameraRight + 3, frame.constants.cameraRight);
			std::copy(constants.cameraUp, constants.cameraUp + 3, frame.constants.cameraUp);
			frame.gridDescriptors = { grid[0], grid[2], grid[7], grid[8] };
			viewComparatorPrepared = mViewWork.Prepare(BuildGridServices(renderer), frame);
			if (viewComparatorPrepared && mViewWork.GetOutputs(viewOutputs) && viewOutputs.columnMasks != nullptr &&
				viewOutputs.compactIndices != nullptr && viewOutputs.control != nullptr)
			{
				const nri::Descriptor* viewResources[] = {
					viewOutputs.columnMasks, viewOutputs.compactIndices, viewOutputs.control };
				nri::UpdateDescriptorRangeDesc update = {};
				update.descriptorSet = slot.bufferSet;
				update.rangeIndex = 0u;
				update.baseDescriptor = kSmokeViewStorageBase;
				update.descriptors = viewResources;
				update.descriptorNum = (uint32_t)std::size(viewResources);
				renderer.mFrameBuffer->mCore.UpdateDescriptorRanges(&update, 1u);
				if (mSettings.viewRoute == 1u)
					constants.flags |= kSmokeFlagViewMask;
			}
			bindSmokePipeline();
		}
	}
	if (renderParticles && !fieldDiagnostics)
	{
		NRIScopedGpuTiming timing(renderer.mFrameBuffer, NRIGpuTimingScope::SmokeCarrier);
		dispatch(NRISmokePass::Bin, Groups(mResourceParticleCapacity), 1, 1);
		mStatus.particleOpticalDispatches++;
		storageBarrier();
		if (directionalLightReady && constants.lightMode > 0u)
		{
			dispatch(NRISmokePass::LightDirectionalCarriers,
				(kDirectionalProbesPerParticle + kDirectionalProbeThreadGroupWidth - 1u) / kDirectionalProbeThreadGroupWidth,
				(mResourceParticleCapacity + kDirectionalParticleThreadGroupHeight - 1u) / kDirectionalParticleThreadGroupHeight, 1);
			mStatus.particleOpticalDispatches++;
			storageBarrier();
		}
		dispatch(NRISmokePass::EvaluateMedium, (mResourceFroxelWidth + 3) / 4, (mResourceFroxelHeight + 3) / 4, (mResourceFroxelDepth + 3) / 4);
		mStatus.particleOpticalDispatches++;
		storageBarrier();
	}
	if (renderGrid)
	{
		NRIScopedGpuTiming timing(renderer.mFrameBuffer, NRIGpuTimingScope::SmokeMaterialize);
		if (mSettings.viewRoute == 2u)
		{
			// Compact is an explicit static route. Missing preparation and overflow
			// both fail closed: the latter publishes zero indirect arguments.
			if (viewComparatorPrepared && viewOutputs.indirectBuffer != nullptr)
			{
				mViewWork.TransitionIndirectToArgument(BuildGridServices(renderer));
				dispatchIndirect(NRISmokePass::EvaluateGridCompact, *viewOutputs.indirectBuffer, 0u);
				mStatus.gridOpticalDispatches++;
			}
		}
		else
		{
			dispatch(NRISmokePass::EvaluateGrid, (mResourceFroxelWidth + 3) / 4,
				(mResourceFroxelHeight + 3) / 4, (mResourceFroxelDepth + 3) / 4);
			mStatus.gridOpticalDispatches++;
		}
		storageBarrier();
		if (!fieldDiagnostics && mPromptFallback.GetSnapshot().scheduledFallbackQuantity > 0u)
		{
			dispatch(NRISmokePass::PromptFallback, (mResourceFroxelWidth + 3) / 4,
				(mResourceFroxelHeight + 3) / 4, (mResourceFroxelDepth + 3) / 4);
			mStatus.gridOpticalDispatches++;
			storageBarrier();
		}
	}
	if (analyticCount > 0u && !fieldDiagnostics)
	{
		NRIScopedGpuTiming timing(renderer.mFrameBuffer, NRIGpuTimingScope::SmokeAnalyticMaterialize);
		const uint32_t savedParticleCapacity = constants.particleCapacity;
		constants.particleCapacity = analyticCount;
		dispatch(NRISmokePass::AnalyticBuildTiles, Groups(analyticCount), 1, 1);
		storageBarrier();
		dispatch(NRISmokePass::AnalyticMaterialize, (mResourceFroxelWidth + 3u) / 4u,
			(mResourceFroxelHeight + 3u) / 4u, (mResourceFroxelDepth + 3u) / 4u);
		storageBarrier();
		constants.particleCapacity = savedParticleCapacity;
	}
	if (viewComparatorPrepared && mSettings.viewCompare)
	{
		NRIScopedGpuTiming timing(renderer.mFrameBuffer, NRIGpuTimingScope::SmokeViewPrepare);
		mViewWork.CompareDense(BuildGridServices(renderer), mFroxelMedium.storageView, mFroxelSource.storageView);
		bindSmokePipeline();
	}
	else if (viewComparatorPrepared)
	{
		mViewWork.Finish(BuildGridServices(renderer));
	}
	if (!fieldDiagnostics)
	{
		NRIScopedGpuTiming timing(renderer.mFrameBuffer, NRIGpuTimingScope::SmokeViewPoint);
		dispatch(NRISmokePass::LightPoint, Groups(froxelCount), 1, 1);
		storageBarrier();
	}
	if (!fieldDiagnostics)
	{
		NRIScopedGpuTiming timing(renderer.mFrameBuffer, NRIGpuTimingScope::SmokeViewDirectional);
		dispatch(NRISmokePass::LightDirectional, Groups(froxelCount), 1, 1);
		storageBarrier();
	}
	if (renderGrid && !fieldDiagnostics)
	{
		{
			NRIScopedGpuTiming timing(renderer.mFrameBuffer, NRIGpuTimingScope::SmokeViewDirectReuse);
			if (effectiveDirectReuseMode >= 1u)
			{
				dispatch(NRISmokePass::LightDirectTemporal, Groups(froxelCount), 1, 1);
				storageBarrier();
			}
			dispatch(NRISmokePass::LightDirectSpatial, Groups(froxelCount), 1, 1);
			storageBarrier();
		}
		mDirectHistoryValid = true;
		mLastDirectFrame = renderer.mFrameIndex;
		mLastDirectReuseMode = effectiveDirectReuseMode;
		mLastDirectReferenceMode = mSettings.directReferenceMode;
		mLastDirectQuality = std::min(mSettings.quality, 2u);
		mLastDirectLightMode = constants.lightMode;
		mLastDirectLightSamples = constants.lightSamples;
		mLastDirectSimulationEpoch = mStatus.simulationEpoch;
		mLastDirectVisibilityBackend = directVisibilityBackend;
		mLastDirectDirectionalHash = directDirectionalHash;
	}
	else
	{
		mDirectHistoryValid = false;
		mLastDirectFrame = UINT32_MAX;
	}
	slot.analyticBuildDispatchGroups = 0u;
	slot.analyticApplyDispatchGroups = 0u;
	const bool runCarrierEmissive = !fieldDiagnostics && analyticCount > 0u && worldEmissiveReady &&
		emissiveLightsReady && !renderParticles && !mSettings.emissiveReference;
	mStatus.analyticEmissiveCarrierOwned = runCarrierEmissive;
	if (runCarrierEmissive)
	{
		slot.analyticBuildDispatchGroups = Groups(analyticCount);
		slot.analyticApplyDispatchGroups = Groups(froxelCount);
		const uint32_t savedParticleCapacity = constants.particleCapacity;
		constants.particleCapacity = analyticCount;
		{
			NRIScopedGpuTiming timing(renderer.mFrameBuffer,
				NRIGpuTimingScope::SmokeAnalyticEmissiveBuild);
			dispatch(NRISmokePass::AnalyticEmissiveBuild, Groups(analyticCount), 1, 1);
			storageBarrier();
		}
		mAnalyticCarriers.CommitLightBuilds();
		{
			NRIScopedGpuTiming timing(renderer.mFrameBuffer,
				NRIGpuTimingScope::SmokeAnalyticEmissiveApply);
			dispatch(NRISmokePass::AnalyticEmissiveResolve, Groups(froxelCount), 1, 1);
			storageBarrier();
		}
		constants.particleCapacity = savedParticleCapacity;
	}
	const NRISmokeDormantGridStatusSnapshot& dormantLighting =
		mDormantGrid.GetStatusSnapshot();
	const bool dormantReceiverLighting = gridRepresentationActive && mSettings.dormantGrid &&
		dormantLighting.resourcesReady &&
		(dormantLighting.gpu.residentCount != 0u || !mDormantPendingDemotions.empty());
	const bool runLegacyEmissive = !fieldDiagnostics && (renderParticles || dormantReceiverLighting || !worldEmissiveReady ||
		(analyticCount > 0u && !runCarrierEmissive));
	if (runLegacyEmissive)
	{
		NRIScopedGpuTiming timing(renderer.mFrameBuffer, NRIGpuTimingScope::SmokeViewEmissive);
		dispatch(NRISmokePass::LightEmissiveInitial, Groups(froxelCount), 1, 1);
		storageBarrier();
		dispatch(NRISmokePass::LightEmissiveTemporal, Groups(froxelCount), 1, 1);
		storageBarrier();
		dispatch(NRISmokePass::LightEmissiveSpatial, Groups(froxelCount), 1, 1);
		storageBarrier();
	}
	if (emissiveLightsReady && (runLegacyEmissive || runCarrierEmissive))
	{
		mEmissiveHistoryValid = true;
		mLastEmissiveReuseMode = mSettings.emissiveReuseMode;
		mLastEmissiveGeneration = emissiveGeneration;
		mLastEmissiveFrame = renderer.mFrameIndex;
		mLastEmissiveRepresentation = mStatus.representationEffective;
		mLastEmissiveLaneCount = emissiveLaneCount;
		mLastEmissiveLightMode = constants.lightMode;
		mLastEmissiveVisibilityBackend = emissiveVisibilityBackend;
	}
	else
	{
		mEmissiveHistoryValid = false;
		mStatus.emissiveHistoryValid = false;
		mLastEmissiveFrame = UINT32_MAX;
		mLastEmissiveRepresentation = UINT32_MAX;
		mLastEmissiveLaneCount = 0;
		mLastEmissiveLightMode = 0;
		mLastEmissiveVisibilityBackend = 0;
	}
	if (!fieldDiagnostics && mSettings.indirect && indirectResourcesReady && mSettings.indirectScale > 0.0f)
	{
		NRIScopedGpuTiming timing(renderer.mFrameBuffer, NRIGpuTimingScope::SmokeViewIndirect);
		dispatch(NRISmokePass::LightIndirectReference, Groups(froxelCount), 1, 1);
		storageBarrier();
		if (effectiveIndirectCacheMode > 0u)
		{
			dispatch(NRISmokePass::LightIndirectTemporal, Groups(froxelCount), 1, 1);
			storageBarrier();
			dispatch(NRISmokePass::LightIndirectSpatial, Groups(froxelCount), 1, 1);
			storageBarrier();
			mIndirectHistoryValid = true;
			mLastIndirectCacheMode = effectiveIndirectCacheMode;
			mLastIndirectSectorHash = indirectSectorHash;
			mLastIndirectSkyKey = indirectSkyKey;
			mLastIndirectEmissiveHash = indirectEmissiveHash;
		}
		else
		{
			mIndirectHistoryValid = false;
		}
	}
	else
	{
		mIndirectHistoryValid = false;
	}
	{
		NRIScopedGpuTiming timing(renderer.mFrameBuffer, NRIGpuTimingScope::SmokeIntegrate);
		dispatch(NRISmokePass::Integrate, (mResourceFroxelWidth + 7) / 8, (mResourceFroxelHeight + 7) / 8, 1);
		storageBarrier();
	}
	{
		NRIScopedGpuTiming timing(renderer.mFrameBuffer, NRIGpuTimingScope::SmokeReconstruction);
		dispatch(NRISmokePass::ResolveVolume, (route.width + 7) / 8, (route.height + 7) / 8, 1);
		renderer.mFrameBuffer->TransitionTexture(volumeCurrent, { nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE, nri::StageBits::COMPUTE_SHADER });
		renderer.mFrameBuffer->TransitionTexture(volumeCurrentMeta, { nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE, nri::StageBits::COMPUTE_SHADER });
		dispatch(NRISmokePass::TemporalVolume, (route.width + 7) / 8, (route.height + 7) / 8, 1);
		renderer.mFrameBuffer->TransitionTexture(volumeHistoryWrite, { nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE, nri::StageBits::COMPUTE_SHADER });
		renderer.mFrameBuffer->TransitionTexture(volumeMetaWrite, { nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE, nri::StageBits::COMPUTE_SHADER });
		dispatch(NRISmokePass::Composite, (route.width + 7) / 8, (route.height + 7) / 8, 1);
	}
	mVolumeHistoryValid = true;
	mLastVolumeFrame = renderer.mFrameIndex;
	mLastVolumeWidth = route.width;
	mLastVolumeHeight = route.height;
	mLastVolumePlacement = (uint32_t)route.placement;
	mLastVolumeSimulationEpoch = mStatus.simulationEpoch;
	mLastVolumeHistoryEnabled = volumeHistoryAllowed;
	mLastVolumeLightingHash = volumeLightingHash;
	mLastSmokeVisualHash = visualHistoryHash;
	if (mSettings.readback)
	{
		nri::BufferBarrierDesc copyBarriers[2] = {};
		copyBarriers[0].buffer = mControl.buffer;
		copyBarriers[0].before = StorageAccess();
		copyBarriers[0].after = NRIResourceCopySourceAccess();
		copyBarriers[1].buffer = slot.controlReadback.buffer;
		copyBarriers[1].before = slot.readbackInitialized ? NRIResourceCopyDestinationAccess() : nri::AccessStage{};
		copyBarriers[1].after = NRIResourceCopyDestinationAccess();
		nri::BarrierDesc barrier = {};
		barrier.buffers = copyBarriers;
		barrier.bufferNum = 2;
		renderer.mFrameBuffer->mCore.CmdBarrier(*renderer.mFrameBuffer->mCommandBuffer, barrier);
		renderer.mFrameBuffer->mCore.CmdCopyBuffer(*renderer.mFrameBuffer->mCommandBuffer, *slot.controlReadback.buffer, 0, *mControl.buffer, 0, sizeof(NRISmokeControlGpu));
		slot.readbackPending = true;
		slot.readbackInitialized = true;
		slot.readbackFrame = renderer.mFrameBuffer->mFrameIndex;
		slot.readbackEpoch = mStatus.simulationEpoch;
		const NRISmokeWorkSchedulerSnapshot& analyticWork = mWorkScheduler.GetSnapshot();
		slot.analyticProfile = (uint32_t)analyticWork.effectiveProfile;
		slot.analyticProfileRevision = analyticWork.table.revision;
		slot.analyticSnapshot = mAnalyticCarriers.GetSnapshot();
		mControlCopyPending = true;
	}
	return true;
}

bool NRISmokeSystem::DispatchRoute(NRIRenderer& renderer, const NRISmokeRouteDesc& route)
{
	mStatus.routeSupported = route.supported;
	mStatus.dispatchedFrame = renderer.mFrameIndex;
	mStatus.inputSlot = (uint32_t)route.inputSlot;
	mStatus.outputSlot = (uint32_t)route.outputSlot;
	mStatus.depthSlot = (uint32_t)route.depthSlot;
	mStatus.routeWidth = route.width;
	mStatus.routeHeight = route.height;
	mStatus.routePlacement = (uint32_t)route.placement;
	mStatus.dlrrModeEffective = route.placement == NRISmokeRoutePlacement::DlrrPreUpscaleMainInput && route.supported ? 1u : 0u;
	mStatus.exposureDomain = (uint32_t)route.exposureDomain;
	if (!mSettings.enabled || !mStatus.mainViewEligible || !route.supported || !mStatus.authorityOperational)
	{
		mStatus.volumeResolvedSlot = UINT32_MAX;
		mStatus.volumeMetaSlot = UINT32_MAX;
		mStatus.volumeHistoryValid = false;
		if (route.supported)
			renderer.CopyTexture(renderer.GetFrameTexture(route.inputSlot), renderer.GetFrameTexture(route.outputSlot));
		return true;
	}
	if (mStatus.representationEffective == 0u && !mMayHaveParticleSmoke)
	{
		mStatus.volumeResolvedSlot = UINT32_MAX;
		mStatus.volumeMetaSlot = UINT32_MAX;
		mStatus.volumeHistoryValid = false;
		mVolumeHistoryValid = false;
		mIndirectHistoryValid = false;
		mEmissiveHistoryValid = false;
		mStatus.emissiveHistoryValid = false;
		mLastEmissiveFrame = UINT32_MAX;
		mLastEmissiveRepresentation = UINT32_MAX;
		mLastEmissiveLaneCount = 0;
		mLastEmissiveLightMode = 0;
		mLastEmissiveVisibilityBackend = 0;
		mDirectHistoryValid = false;
		mLastDirectFrame = UINT32_MAX;
		renderer.CopyTexture(renderer.GetFrameTexture(route.inputSlot), renderer.GetFrameTexture(route.outputSlot));
		return true;
	}
	if (RecordVolume(renderer, route))
		return true;
	mStatus.volumeResolvedSlot = UINT32_MAX;
	mStatus.volumeMetaSlot = UINT32_MAX;
	mStatus.volumeHistoryValid = false;
	renderer.CopyTexture(renderer.GetFrameTexture(route.inputSlot), renderer.GetFrameTexture(route.outputSlot));
	return true;
}

void NRISmokeSystem::QueueSyntheticInjection()
{
	mSyntheticRequested = true;
}

void NRISmokeSystem::Reset(const char* reason)
{
	mStatus.simulationEpoch = std::max(1u, mStatus.simulationEpoch + 1u);
	mStatus.preparedFrame = UINT32_MAX;
	mStatus.dispatchedFrame = UINT32_MAX;
	mStatus.resetReason = reason != nullptr ? reason : "unspecified";
	mStatus.gpuStatsValid = false;
	mStatus.gpuStatsFrame = UINT64_MAX;
	mStatus.analyticLight = {};
	mStatus.activeParticles = 0;
	mStatus.spawnedParticles = 0;
	mStatus.expiredParticles = 0;
	mStatus.liveEvictions = 0;
	mStatus.columnOverflow = 0;
	mStatus.wideParticlesProjected = 0;
	mStatus.wideGlobalDrops = 0;
	mStatus.fineColumnReferences = 0;
	mStatus.wideCellReferences = 0;
	mStatus.globalDepthReferences = 0;
	mStatus.referenceInvalidLinks = 0;
	mStatus.referenceTraversalLimitExits = 0;
	mStatus.fineTierParticles = 0;
	mStatus.wideTierParticles = 0;
	mStatus.globalTierParticles = 0;
	mStatus.fineOccupiedCells = 0;
	mStatus.wideOccupiedCells = 0;
	mStatus.globalOccupiedSlices = 0;
	mStatus.fineMaximumCellReferences = 0;
	mStatus.wideMaximumCellReferences = 0;
	mStatus.globalMaximumCellReferences = 0;
	mStatus.maximumDepthSpan = 0;
	mStatus.maximumCandidatesPerFroxel = 0;
	mStatus.occupiedCount = 0;
	mStatus.occupiedOverflow = 0;
	mStatus.mediumCandidateTests = 0;
	mStatus.pointFroxelsProcessed = 0;
	mStatus.directionalFroxelsProcessed = 0;
	mStatus.directionalSamples = 0;
	mStatus.directionalShadowRays = 0;
	mStatus.directionalShadowVisible = 0;
	mStatus.directionalShadowOccluded = 0;
	mStatus.directionalRadianceClamps = 0;
	mStatus.emissiveFroxelsProcessed = 0;
	mStatus.emissiveSamples = 0;
	mStatus.emissiveCandidateMisses = 0;
	mStatus.emissiveDistanceRejected = 0;
	mStatus.emissiveFacingRejected = 0;
	mStatus.emissiveShadowRays = 0;
	mStatus.emissiveShadowVisible = 0;
	mStatus.emissiveShadowOccluded = 0;
	mStatus.emissiveContributed = 0;
	mStatus.emissiveRadianceClamps = 0;
	mStatus.emissiveReservoirInitial = 0;
	mStatus.emissiveReservoirInvalid = 0;
	mStatus.emissiveTemporalAccepted = 0;
	mStatus.emissiveTemporalRejected = 0;
	mStatus.emissiveSpatialAccepted = 0;
	mStatus.emissiveSpatialRejected = 0;
	mStatus.emissiveFinalEvaluations = 0;
	mStatus.emissiveSourceClamps = 0;
	mStatus.emissiveRemovedEnergy = 0;
	mStatus.emissiveMaximumAge = 0;
	mStatus.emissiveReferenceSamples = 0;
	mStatus.emissiveReferenceRays = 0;
	mStatus.emissiveIdentityRejects = 0;
	mStatus.emissiveInnerRisSets = 0;
	mStatus.emissiveInnerPointProposals = 0;
	mStatus.emissiveInnerZeroProposals = 0;
	mStatus.emissiveInnerRisRejects = 0;
	mStatus.emissiveInnerSelections = 0;
	mStatus.emissiveInnerVisibilityRays = 0;
	mStatus.emissiveInnerSourceVisibilityRays = 0;
	mStatus.emissiveInnerVisibilityVisible = 0;
	mStatus.emissiveInnerBlockerReceiverImmediate = 0;
	mStatus.emissiveInnerBlockerReceiverCell = 0;
	mStatus.emissiveInnerBlockerEmitterCell = 0;
	mStatus.emissiveInnerBlockerInterior = 0;
	mStatus.emissiveInnerSourceSelections = 0;
	mStatus.emissiveInnerSourceOverflow = 0;
	mStatus.emissiveTargetVisibilityRays = 0;
	mStatus.emissiveTargetVisibilityVisible = 0;
	mStatus.emissiveTargetBlockerExact = 0;
	mStatus.emissiveTargetBlockerRange = 0;
	mStatus.emissiveTargetBlockerOther = 0;
	mStatus.emissiveTargetWitnessClaim = 0;
	mStatus.emissiveTargetWitnessCandidate = 0xffffffffu;
	mStatus.emissiveTargetWitnessRelation = 0;
	mStatus.emissiveTargetWitnessSamplePrimitive = 0xffffffffu;
	mStatus.emissiveTargetWitnessSampleMaterial = 0xffffffffu;
	mStatus.emissiveTargetWitnessBlockerDataSource = 0xffffffffu;
	mStatus.emissiveTargetWitnessBlockerInstance = 0xffffffffu;
	mStatus.emissiveTargetWitnessBlockerPrimitive = 0xffffffffu;
	mStatus.emissiveTargetWitnessBlockerMaterial = 0xffffffffu;
	mStatus.emissiveTargetWitnessDistanceBits = 0;
	mStatus.indirectFroxelsProcessed = 0;
	mStatus.indirectLocalityRays = 0;
	mStatus.indirectLocalityAgreement = 0;
	mStatus.indirectLocalityOneSided = 0;
	mStatus.indirectLocalityMismatch = 0;
	mStatus.indirectLocalityInvalid = 0;
	mStatus.indirectReferenceRays = 0;
	mStatus.indirectReferenceHits = 0;
	mStatus.indirectReferenceMisses = 0;
	mStatus.indirectSectorContributions = 0;
	mStatus.indirectSkyContributions = 0;
	mStatus.indirectEmissionContributions = 0;
	mStatus.indirectRadianceClamps = 0;
	mStatus.indirectNanRejects = 0;
	mStatus.indirectTemporalAccepted = 0;
	mStatus.indirectTemporalRejected = 0;
	mStatus.indirectSpatialAccepted = 0;
	mStatus.indirectSpatialRejected = 0;
	mStatus.indirectCacheMaximumAge = 0;
	mStatus.indirectCacheClamps = 0;
	mStatus.indirectCacheResolved = 0;
	mStatus.directReceiverSamples = 0;
	mStatus.directFractionalVisibility = 0;
	mStatus.directVisibilityZero = 0;
	mStatus.directVisibilityOne = 0;
	mStatus.directTemporalAccepted = 0;
	mStatus.directTemporalRejected = 0;
	mStatus.directSpatialAccepted = 0;
	mStatus.directSpatialRejected = 0;
	mStatus.directHistoryMaximumAge = 0;
	mStatus.directHistoryResolved = 0;
	mStatus.directHistoryClamps = 0;
	mStatus.directNanRejects = 0;
	mStatus.lightCandidatesTested = 0;
	mStatus.lightDistanceRejected = 0;
	mStatus.lightShadowRays = 0;
	mStatus.lightShadowVisible = 0;
	mStatus.lightShadowOccluded = 0;
	mStatus.lightSoftSamples = 0;
	mStatus.lightRadianceClamps = 0;
	mStatus.filterCandidateHits = 0;
	mStatus.filterAlphaRejects = 0;
	mStatus.filterNoShadowRejects = 0;
	mStatus.filterOneWayRejects = 0;
	mStatus.filterReflectionRejects = 0;
	mStatus.filterPortalContinuations = 0;
	mStatus.filterAcceptedBlockers = 0;
	mStatus.filterMisses = 0;
	mStatus.filterSkipLimitExits = 0;
	mStatus.filterContinuationLimitExits = 0;
	mStatus.filterResourceDowngrades = 0;
	mPendingCommands.clear();
	mPendingPulseEnqueueInfo.clear();
	mPendingTrailObservations.clear();
	mSelectedGridCommands.clear();
	mAdmissionScheduler.Reset();
	mWorkScheduler.ResetTelemetry();
	mPulsePlanToken = 0u;
	mPromptFallback.Reset();
	mAnalyticCarriers.Reset(mStatus.simulationEpoch);
	mAnalyticTrailBridge.Reset(mStatus.simulationEpoch);
	mStatus.analytic = mAnalyticCarriers.GetSnapshot();
	if (std::strcmp(mStatus.resetReason, "authority-transition") == 0)
	{
		mPulseOwner.RebaseEpoch(mStatus.simulationEpoch);
		mPulseOwner.RebaseSimulationClock(mPromptSimulationSeconds, 0.0);
	}
	else
		mPulseOwner.Reset();
	mStatus.admission = {};
	mStatus.admissionFrame = UINT32_MAX;
	mStatus.admissionRendererFrame = UINT64_MAX;
	mAccumulator = 0.0f;
	mLastGameplaySeconds = -1.0;
	mParticleSimulationSeconds = 0.0;
	mPromptSimulationSeconds = 0.0;
	mLatestParticleDeathSeconds = 0.0;
	mMayHaveParticleSmoke = false;
	mLastPreparedFrame = UINT32_MAX;
	mLastSimulatedFrame = UINT32_MAX;
	mNeedsClear = true;
	mIndirectHistoryValid = false;
	mEmissiveHistoryValid = false;
	mStatus.emissiveHistoryValid = false;
	mLastEmissiveFrame = UINT32_MAX;
	mLastEmissiveRepresentation = UINT32_MAX;
	mLastEmissiveLaneCount = 0;
	mLastEmissiveLightMode = 0;
	mLastEmissiveVisibilityBackend = 0;
	mDirectHistoryValid = false;
	mLastDirectFrame = UINT32_MAX;
	mStatus.directHistoryValid = false;
	mStatus.directHistoryResetReason = mStatus.resetReason;
	mVolumeHistoryValid = false;
	mLastVolumeFrame = UINT32_MAX;
	mStatus.volumeHistoryValid = false;
	mStatus.volumeResolvedSlot = UINT32_MAX;
	mStatus.volumeMetaSlot = UINT32_MAX;
	mStatus.volumeHistoryAge = 0;
	mStatus.volumeHistoryResetReason = mStatus.resetReason;
	if (std::strcmp(mStatus.resetReason, "authority-transition") != 0)
	{
		mEmitters.Reset();
		mInterest.Reset();
	}
	mGrid.Reset(mStatus.simulationEpoch, mStatus.resetReason);
	mDormantGrid.Reset(mStatus.simulationEpoch, mStatus.resetReason);
	mSpatialInterest.Reset(mStatus.simulationEpoch);
	mDormantAuthorities.clear();
	mDormantPendingDemotions.clear();
	mDormantDemotions.clear();
	mDormantPromotions.clear();
	mDormantInjectionBuild = {};
	mLastDormantResultFrame = UINT64_MAX;
	mGridLighting.Reset(mStatus.simulationEpoch, mStatus.resetReason);
}

void NRISmokeSystem::DestroyViewResources(NRIRenderer& renderer)
{
	auto destroy = [&](NRIBufferResource& resource) { renderer.DestroyBufferResource(resource); };
	destroy(mFineCells); destroy(mWideCells); destroy(mGlobalDepthCells); destroy(mFroxelMedium); destroy(mFroxelIntegrated);
	destroy(mFroxelPhase); destroy(mFroxelSource); destroy(mOccupiedFroxelIndices);
	destroy(mIndirectHistory); destroy(mIndirectScratch);
	destroy(mEmissiveCurrent); destroy(mEmissiveTemporal); destroy(mEmissiveHistory);
	destroy(mDirectCurrent); destroy(mDirectHistory);
	destroy(mAnalyticTileHeaders); destroy(mAnalyticTileIndices); destroy(mAnalyticFroxelMedium);
	destroy(mAnalyticEmissiveA); destroy(mAnalyticEmissiveB);
	mViewResourcesInitialized = false;
	mIndirectHistoryValid = false;
	mEmissiveHistoryValid = false;
	mStatus.emissiveHistoryValid = false;
	mLastEmissiveFrame = UINT32_MAX;
	mLastEmissiveRepresentation = UINT32_MAX;
	mLastEmissiveLaneCount = 0;
	mLastEmissiveLightMode = 0;
	mLastEmissiveVisibilityBackend = 0;
	mDirectHistoryValid = false;
	mLastDirectFrame = UINT32_MAX;
	mStatus.indirectCacheBytes = 0;
	mStatus.emissiveReservoirBytes = 0;
	mStatus.directHistoryBytes = 0;
	mStatus.directHistoryValid = false;
	mVolumeHistoryValid = false;
	mLastVolumeFrame = UINT32_MAX;
	mStatus.volumeHistoryValid = false;
	mResourceFroxelWidth = mResourceFroxelHeight = mResourceFroxelDepth = 0;
	mResourceLegacyEmissiveFull = true;
}

void NRISmokeSystem::DestroyResources(NRIRenderer& renderer)
{
	auto destroy = [&](NRIBufferResource& resource) { renderer.DestroyBufferResource(resource); };
	DestroyViewResources(renderer);
	destroy(mStyleBuffer); destroy(mParticles); destroy(mControl); destroy(mReferenceNext); destroy(mParticleDirectionalVisibility);
	DestroyCompatibilityDescriptors(renderer);
	for (CommandSlot& slot : mCommandSlots)
	{
		destroy(slot.upload); destroy(slot.device); destroy(slot.analyticUpload); destroy(slot.analyticDevice);
		destroy(slot.styleUpload); destroy(slot.controlReadback);
		slot.readbackPending = false;
		slot.initialized = false;
		slot.readbackInitialized = false;
	}
	mControlCopyPending = false;
	mResourcesInitialized = false;
	mParticleResourcesInitialized = false;
	mResourceParticleCapacity = mResourceStyleCapacity = 0;
	mResourceParticlePayload = false;
	mStatus.particlePayloadBytes = 0;
	mStatus.descriptorSentinelBytes = 0;
}

void NRISmokeSystem::Shutdown(NRIRenderer& renderer)
{
	mViewWork.Shutdown(BuildGridServices(renderer));
	mGridLighting.Shutdown(BuildGridServices(renderer));
	mDormantGrid.Shutdown(BuildGridServices(renderer));
	mGrid.Shutdown(BuildGridServices(renderer));
	DestroyResources(renderer);
	for (nri::Pipeline*& pipeline : mPipelines)
	{
		if (pipeline != nullptr) renderer.mFrameBuffer->mCore.DestroyPipeline(pipeline);
		pipeline = nullptr;
	}
	if (mPipelineLayout != nullptr) renderer.mFrameBuffer->mCore.DestroyPipelineLayout(mPipelineLayout);
	mPipelineLayout = nullptr;
	Reset("renderer-shutdown");
}

void NRISmokeSystem::PrintStatus(const NRIRenderer& renderer) const
{
	const NRISmokeWorkSchedulerSnapshot& work = mWorkScheduler.GetSnapshot();
	Printf("NRI PT smoke visuals: debug=%u name=%s threshold=%.6f knee=%.6f gamma=%.3f reference=%.6f shoulder=%.6f color_pivot=%.6f thin=%.3f/%.3f/%.3f core=%.3f/%.3f/%.3f coefficient_space=yes\n",
		mSettings.debugMode, NRIGetSmokeFieldDebugName(
			mSettings.debugMode >= 12u ? mSettings.debugMode - 11u : 0u),
		mSettings.visuals.extinctionThreshold, mSettings.visuals.extinctionKnee,
		mSettings.visuals.extinctionGamma, mSettings.visuals.extinctionReference,
		mSettings.visuals.extinctionShoulder, mSettings.visuals.colorPivot,
		mSettings.visuals.thinColor[0], mSettings.visuals.thinColor[1],
		mSettings.visuals.thinColor[2], mSettings.visuals.coreColor[0],
		mSettings.visuals.coreColor[1], mSettings.visuals.coreColor[2]);
	Printf("NRI PT smoke work profile: requested=%u effective=%u name=%s revision=%u change_serial=%u supported=%08x enforced=%08x unrestricted=%u froxel_pixels=%u froxel_depth=%u emissive_lights=%u emissive_backend=%u light_samples=%u light_candidates=%u emission_commands=%u first_use_sources=%u analytic_carriers=%u admission_brick_requests=%u deposition_cell_visits=%u projection_work_units=%u materialized_froxels=%u radiance_new_invalid=%u radiance_maintenance=%u world_link_rays=%u direct_receiver_samples=%u dormant_archives=%u dormant_promotions=%u dormant_evolution=%u simulation_substeps=%u emission=%u/%u/%u first_use=%u/%u/%u analytic=%u/%u/%u simulation=%u/%u/%u debt=%u debt_max=%u capped_consecutive=%u capped_total=%llu policy=static-no-timing-input\n",
		work.requestedProfile, (uint32_t)work.effectiveProfile,
		NRISmokeWorkScheduler::ProfileName(work.effectiveProfile), work.table.revision,
		work.profileChangeSerial, work.table.supportedCapabilities, work.table.enforcedCapabilities,
		NRISmokeWorkTable::Unrestricted, mSettings.froxelPixelSize, mSettings.froxelDepth,
		mSettings.emissiveLights ? 1u : 0u, mSettings.emissiveBackend,
		mSettings.lightSamples, mSettings.maxLightCandidates,
		work.table.emissionCommands, work.table.firstUseSources, work.table.analyticCarriers,
		work.table.admissionBrickRequests, work.table.depositionCellVisits, work.table.projectionWorkUnits,
		work.table.materializedFroxels, work.table.radianceNewInvalidCells,
		work.table.radianceMaintenanceCells, work.table.worldLinkRays,
		work.table.directReceiverSamples, work.table.dormantArchives,
		work.table.dormantPromotions, work.table.dormantEvolution,
		work.table.simulationSubsteps,
		work.emissionRequested, work.emissionScheduled, work.emissionDeferred,
		work.promptRequested, work.promptScheduled, work.promptDeferred,
		work.analyticRequested, work.analyticScheduled, work.analyticDropped,
		work.simulationDueSubsteps, work.simulationScheduledSubsteps,
		work.simulationDeferredSubsteps, work.simulationDebtSubsteps,
		work.simulationMaximumDebtSubsteps, work.simulationConsecutiveCappedFrames,
		(unsigned long long)work.simulationCappedFrames);
	const NRISmokeAnalyticCarrierSnapshot& analytic = mAnalyticCarriers.GetSnapshot();
	Printf("NRI PT smoke analytic: epoch=%u active=%u limit=%u high_water=%u oldest_ms=%u requested=%llu admitted=%llu expired=%llu drop_not_prepared=%llu drop_disabled=%llu drop_invalid=%llu drop_epoch=%llu drop_expired=%llu drop_stale=%llu drop_capacity=%llu lighting=%s policy=immediate-or-drop\n",
		analytic.epoch, analytic.activeQuantity, analytic.maximumActiveQuantity,
		analytic.highWaterQuantity, analytic.oldestActiveAgeMilliseconds,
		(unsigned long long)analytic.requested, (unsigned long long)analytic.admitted,
		(unsigned long long)analytic.expired, (unsigned long long)analytic.droppedNotPrepared,
		(unsigned long long)analytic.droppedDisabled, (unsigned long long)analytic.droppedInvalidRequest,
		(unsigned long long)analytic.droppedStaleEpoch, (unsigned long long)analytic.droppedExpiredOnArrival,
		(unsigned long long)analytic.droppedStaleOnArrival, (unsigned long long)analytic.droppedCapacity,
		mStatus.analyticEmissiveCarrierOwned ? "event-anchor-field" : "shared-receiver");
	Printf("NRI PT smoke representation: requested=%u effective=%u authority=%s operational=%s reason=%s "
		"preparation=%s transition=%u@%u grid=%s fallback=%s particle_payload_bytes=%llu descriptor_sentinel_bytes=%llu "
		"particle_payload_mib=%.3f descriptor_sentinel_mib=%.3f particle_lifetime_active=%s "
		"particle_sim_dispatches=%u grid_sim_dispatches=%u particle_optical_dispatches=%u grid_optical_dispatches=%u "
		"particle_commands=%u grid_commands=%u\n",
		mStatus.representationRequested, mStatus.representationEffective, mStatus.authority,
		mStatus.authorityOperational ? "yes" : "no", mStatus.authorityReason, mStatus.authorityPreparation,
		mStatus.authorityTransitionSerial, mStatus.authorityTransitionFrame,
		mStatus.gridReady ? "ready" : "unavailable", mStatus.representationFallback,
		(unsigned long long)mStatus.particlePayloadBytes, (unsigned long long)mStatus.descriptorSentinelBytes,
		(double)mStatus.particlePayloadBytes / (1024.0 * 1024.0),
		(double)mStatus.descriptorSentinelBytes / (1024.0 * 1024.0),
		mMayHaveParticleSmoke ? "yes" : "no", mStatus.particleSimulationDispatches,
		mStatus.gridSimulationDispatches, mStatus.particleOpticalDispatches, mStatus.gridOpticalDispatches,
		mStatus.particleCommandsRouted, mStatus.gridCommandsRouted);
	mGrid.PrintStatus();
	mViewWork.PrintStatus(mSettings.viewCompare, mSettings.viewRoute);
	Printf("NRI PT smoke admission: gathered=%u uploaded=%u deferred=%u coalesced=%u expired=%u rejected=%u sources=%u interactive=%u/%u estimated_bricks=%llu/%llu closure=%s policy=cost-aware-source-round terminal_rejection=yes\n",
		mStatus.admission.gathered, mStatus.admission.uploaded, mStatus.admission.boundedDeferred,
		mStatus.admission.coalesced, mStatus.admission.expired, mStatus.admission.rejected,
		mStatus.admission.sourceCount, mStatus.admission.interactiveUploaded,
		mStatus.admission.interactiveGathered,
		(unsigned long long)mStatus.admission.estimatedBrickWorkUploaded,
		(unsigned long long)mStatus.admission.estimatedBrickWorkGathered,
		mStatus.admission.Closes() ? "yes" : "no");
	const NRISmokeInterestSnapshot& interest = mInterest.GetSnapshot();
	Printf("NRI PT smoke interest: frame=%u hot=%u warm=%u dormant=%u positive=%u portal_promoted=%u runtime_portal_uncertain=%u camera_jump=%u policy=positive-only-hysteretic\n",
		interest.rendererFrame, interest.hotCount, interest.warmCount, interest.dormantCount,
		interest.positiveCount, interest.portalPromotedChunks,
		interest.runtimePortalUncertain ? 1u : 0u, interest.cameraJump ? 1u : 0u);
	const NRISmokeDormantGridStatusSnapshot& dormant = mDormantGrid.GetStatusSnapshot();
	const NRISmokeSpatialInterestSnapshot& spatial = mSpatialInterest.GetSnapshot();
	Printf("NRI PT smoke dormant grid: enabled=%s ready=%s reason=%s capacity=%u resident=%u free=%u cpu_authority=%u pending=%u bytes=%llu payload=%llu gpu_stats=%s gpu_frame=%llu spatial=%u/%u/%u observed=%u archive=%u/%u retained=%u promote=%u/%u retained=%u evolution=%u/%u/%u injection=%u/%u rejected=%u missing=%u stale=%u cadence=%u cells=%u build=%u/%u/%u fixed_work=%u/%u/%u policy=transactional-single-authority\n",
		mSettings.dormantGrid ? "yes" : "no", dormant.resourcesReady ? "yes" : "no",
		dormant.failureReason.c_str(), dormant.archiveCapacity, dormant.gpu.residentCount,
		dormant.gpu.freeCount, (uint32_t)mDormantAuthorities.size(),
		(uint32_t)mDormantPendingDemotions.size(), (unsigned long long)dormant.residentBytes,
		(unsigned long long)dormant.payloadBytes, dormant.gpuStatsValid ? "valid" : "pending",
		(unsigned long long)dormant.gpuRendererFrame, spatial.hot, spatial.warm,
		spatial.dormant, spatial.observed, dormant.gpu.archivePublished,
		dormant.gpu.archiveAttempts, dormant.gpu.archiveRetainedFine,
		dormant.gpu.rehydratePublished, dormant.gpu.rehydrateAttempts,
		dormant.gpu.rehydrateRetainedCoarse, dormant.gpu.evolutionWorkExecuted,
		dormant.gpu.evolutionAttempts, dormant.gpu.evolutionSkipped,
		dormant.gpu.injectionApplied, dormant.gpu.injectionAttempts,
		dormant.gpu.injectionRejected, dormant.gpu.injectionMissing,
		dormant.gpu.injectionStale, dormant.gpu.injectionCadenceSteps,
		dormant.gpu.injectionCells, mDormantInjectionBuild.requests,
		mDormantInjectionBuild.routedSources,
		(uint32_t)mDormantInjectionBuild.injections.size(), work.table.dormantArchives,
		work.table.dormantPromotions, work.table.dormantEvolution);
	const NRISmokeGridLightingStatusSnapshot& world = mGridLighting.GetStatusSnapshot();
	Printf("NRI PT smoke grid emissive: requested_backend=%u effective_backend=%u authority=%s ready=%s cells=%u ping=%u inner_ris_points=%u/%u target=%d radiance_work_limited=%s radiance_new=%u radiance_maintenance=%u field_mib=%.2f work_mib=%.2f links_mib=%.2f proposal_mib=%.3f filter=%s filter_mib=%.2f total_mib=%.2f proposal=%s field_readback=0\n",
		world.requestedBackend, world.effectiveBackend, world.authority, world.resourcesReady ? "yes" : "no",
		world.cellCapacity, world.fieldPing, world.emissivePointCandidatesRequested, world.emissivePointCandidatesEffective,
		world.emissiveCandidateTarget,
		world.radianceWorkLimited ? "yes" : "no", world.radianceNewInvalidQuantity,
		world.radianceMaintenanceQuantity,
		(double)world.fieldBytes / (1024.0 * 1024.0),
		(double)world.workBytes / (1024.0 * 1024.0), (double)world.linkBytes / (1024.0 * 1024.0),
		(double)world.proposalBytes / (1024.0 * 1024.0),
		world.filterDecision, (double)world.filterBytes / (1024.0 * 1024.0),
		(double)world.totalBytes / (1024.0 * 1024.0), world.proposalDecision);
	Printf("NRI PT smoke grid lighting work: gpu_stats=%s frame=%u epoch=%u active=%u support_current=%u source_density=%u support_only=%u support_duplicates=%u support_overflow=%u support_closure=%s scheduled=%u samples=%u visible=%u physical_zero=%u missing=%u structural_errors=%u overflow=%u temporal_accepted=%u temporal_rejected=%u maximum_age=%u links_open=%u links_blocked=%u topology_missing_tlas=%u topology_asymmetric=%u proposal_lists=%u proposal_tested=%u proposal_accepted=%u proposal_local=%u proposal_global=%u proposal_fallbacks=%u proposal_truncations=%u proposal_maximum=%u filter_accepted=%u filter_rejected=%u scatter_active=%u scatter_seeded=%u scatter_neighbor_tests=%u scatter_neighbors_accepted=%u scatter_neighbors_blocked=%u scatter_overflow=%u self_shadow_samples=%u self_shadow_steps=%u control_readback=%llu\n",
		world.gpuStatsValid ? "valid" : "disabled", world.gpu.frameStamp, world.gpu.simulationEpoch,
		world.gpu.activeCount, world.gpu.supportCount, world.gpu.sourceCount, world.gpu.supportOnlyCount,
		world.gpu.duplicateCount, world.gpu.supportOverflowCount,
		world.gpu.sourceCount + world.gpu.supportOnlyCount == world.gpu.activeCount ? "yes" : "no",
		world.gpu.scheduledCount,
		world.gpu.samples, world.gpu.visible, world.gpu.physicalZero, world.gpu.missing,
		world.gpu.structuralErrors, world.gpu.overflowRejects, world.gpu.temporalAccepted,
		world.gpu.temporalRejected, world.gpu.maximumAge, world.gpu.linksOpen, world.gpu.linksBlocked,
		world.gpu.topologyMissingTlas, world.gpu.topologyAsymmetric, world.gpu.proposalListsBuilt,
		world.gpu.proposalCandidatesTested, world.gpu.proposalCandidatesAccepted,
		world.gpu.proposalLocalSamples, world.gpu.proposalGlobalSamples, world.gpu.proposalFallbacks,
		world.gpu.proposalTruncations, world.gpu.proposalMaximumCount, world.gpu.filterAccepted,
		world.gpu.filterRejected, world.gpu.scatterActiveCount, world.gpu.scatterSeededCount,
		world.gpu.scatterNeighborTests, world.gpu.scatterNeighborsAccepted, world.gpu.scatterNeighborsBlocked,
		world.gpu.scatterActiveOverflow, world.gpu.selfShadowSamples, world.gpu.selfShadowSteps,
		(unsigned long long)world.controlReadbackBytes);
	Printf("NRI PT smoke multiple scattering: requested=%s allocated=%s effective=%s probes=%u iterations=%u final_ping=%u scale=%.3f seed_mib=%.3f bounce_mib=%.3f metadata_mib=%.3f active_mib=%.3f total_mib=%.3f decision=%s field_readback=0\n",
		world.multipleScatterRequested ? "yes" : "no", world.multipleScatterAllocated ? "yes" : "no",
		world.multipleScatterEffective ? "yes" : "no", world.scatterProbeCapacity, world.scatterIterations,
		world.scatterFinalPing, mSettings.multipleScatterScale,
		(double)world.scatterSeedBytes / (1024.0 * 1024.0),
		(double)world.scatterBounceBytes / (1024.0 * 1024.0),
		(double)world.scatterMetadataBytes / (1024.0 * 1024.0),
		(double)world.scatterActiveBytes / (1024.0 * 1024.0),
		(double)world.scatterBytes / (1024.0 * 1024.0), world.scatterDecision);
	Printf("NRI PT smoke self shadow: requested=%s allocated=%s effective=%s debug=%u world_field_mib=%.3f decision=%s field_readback=0\n",
		world.selfShadowRequested ? "yes" : "no", world.selfShadowAllocated ? "yes" : "no",
		world.selfShadowEffective ? "yes" : "no", mSettings.selfShadowDebug,
		(double)world.selfShadowFieldBytes / (1024.0 * 1024.0), world.selfShadowDecision);
	const char* placement = mStatus.routePlacement == (uint32_t)NRISmokeRoutePlacement::DlrrPostUpscale ? "dlrr_post_upscale" :
		(mStatus.routePlacement == (uint32_t)NRISmokeRoutePlacement::DlrrPreUpscaleMainInput ? "dlrr_pre_upscale_main" : "standard_pre_upscale");
	const char* inputName = mStatus.inputSlot < (uint32_t)NRIRenderer::FrameTextureSlot::Count ? renderer.GetFrameTextureSlotName((NRIRenderer::FrameTextureSlot)mStatus.inputSlot) : "none";
	const char* outputName = mStatus.outputSlot < (uint32_t)NRIRenderer::FrameTextureSlot::Count ? renderer.GetFrameTextureSlotName((NRIRenderer::FrameTextureSlot)mStatus.outputSlot) : "none";
	Printf("NRI PT smoke status: enabled=%s epoch=%u main_view=%s route_supported=%s placement=%s input=%s output=%s extent=%ux%u froxels=%ux%ux%u particles=%u styles=%u commands=%u commands_total=%llu dropped=%u substeps=%u light_mode_requested=%u light_mode_effective=%u light_samples=%u light_candidates_max=%u point_lights=%s filtered_visibility_requested=%s filtered_visibility_effective=%s visibility_fallback=%s shadow_tlas=%s runtime_lights=%u gpu_stats=%s active=%u spawned=%u expired=%u evictions=%u column_overflow=%u reference_mode=complete reference_stride=512 invalid_links=%u traversal_limit=%u wide_projected=%u wide_global_drops=%u fine_refs=%u wide_refs=%u global_refs=%u tier_particles=%u/%u/%u occupied_cells=%u/%u/%u max_cell_refs=%u/%u/%u depth_span_max=%u carrier_candidates_max=%u medium_occupied=%u occupied_overflow=%u medium_candidate_tests=%u point_froxels=%u light_candidates=%u light_distance_rejected=%u light_shadow_rays=%u light_visible=%u light_occluded=%u light_soft_samples=%u light_clamps=%u filter_hits=%u filter_alpha=%u filter_no_shadow=%u filter_one_way=%u filter_reflection=%u filter_portals=%u filter_blockers=%u filter_misses=%u filter_skip_limit=%u filter_continuation_limit=%u filter_downgrades=%u resident_mib=%.2f particle_readback=0 control_readback=%llu reset=%s\n",
		mStatus.enabled ? "yes" : "no", mStatus.simulationEpoch, mStatus.mainViewEligible ? "yes" : "no", mStatus.routeSupported ? "yes" : "no", placement,
		inputName, outputName, mStatus.routeWidth, mStatus.routeHeight, mStatus.froxelWidth, mStatus.froxelHeight, mStatus.froxelDepth,
		mStatus.particleCapacity, mStatus.styleCount, mStatus.commandsUploaded, (unsigned long long)mStatus.commandsUploadedTotal, mStatus.commandsDropped, mStatus.simulationSubsteps,
		mStatus.requestedLightMode, mStatus.effectiveLightMode, mSettings.lightSamples, mSettings.maxLightCandidates, mSettings.pointLights ? "yes" : "no",
		mStatus.filteredVisibilityRequested ? "yes" : "no", mStatus.filteredVisibilityEffective ? "yes" : "no",
		mStatus.forceOpaqueVisibility ? "force_opaque" : "none", mStatus.shadowTlasReady ? "ready" : "missing", renderer.mBoundRuntimeLightCount,
		mStatus.gpuStatsValid ? "valid" : "disabled", mStatus.activeParticles, mStatus.spawnedParticles, mStatus.expiredParticles, mStatus.liveEvictions, mStatus.columnOverflow,
		mStatus.referenceInvalidLinks, mStatus.referenceTraversalLimitExits,
		mStatus.wideParticlesProjected, mStatus.wideGlobalDrops, mStatus.fineColumnReferences, mStatus.wideCellReferences, mStatus.globalDepthReferences,
		mStatus.fineTierParticles, mStatus.wideTierParticles, mStatus.globalTierParticles, mStatus.fineOccupiedCells, mStatus.wideOccupiedCells, mStatus.globalOccupiedSlices,
		mStatus.fineMaximumCellReferences, mStatus.wideMaximumCellReferences, mStatus.globalMaximumCellReferences, mStatus.maximumDepthSpan, mStatus.maximumCandidatesPerFroxel,
		mStatus.occupiedCount, mStatus.occupiedOverflow, mStatus.mediumCandidateTests, mStatus.pointFroxelsProcessed,
		mStatus.lightCandidatesTested, mStatus.lightDistanceRejected, mStatus.lightShadowRays, mStatus.lightShadowVisible, mStatus.lightShadowOccluded, mStatus.lightSoftSamples, mStatus.lightRadianceClamps,
		mStatus.filterCandidateHits, mStatus.filterAlphaRejects, mStatus.filterNoShadowRejects, mStatus.filterOneWayRejects, mStatus.filterReflectionRejects,
		mStatus.filterPortalContinuations, mStatus.filterAcceptedBlockers, mStatus.filterMisses, mStatus.filterSkipLimitExits, mStatus.filterContinuationLimitExits, mStatus.filterResourceDowngrades,
		(double)mStatus.residentBytes / (1024.0 * 1024.0), (unsigned long long)mStatus.controlReadbackBytes, mStatus.resetReason);
	Printf("NRI PT smoke lighting status: directional=%s resolved=%s shadow=%s directional_probe_grid=20x20x20 directional_probe_cell=2.0 directional_probe_mib=%.2f directional_froxels=%u directional_samples=%u directional_shadow_rays=%u directional_visible=%u directional_occluded=%u directional_clamps=%u emissive=%s emissive_primitives=%u emissive_froxels=%u emissive_samples=%u emissive_no_candidate=%u emissive_distance_rejected=%u emissive_facing_rejected=%u emissive_shadow_rays=%u emissive_visible=%u emissive_occluded=%u emissive_contributed=%u emissive_clamps=%u\n",
		mSettings.directionalLight ? "yes" : "no", renderer.mDirectionalLightState.enabled ? "yes" : "no", renderer.mDirectionalLightState.shadow ? "yes" : "no",
		(double)mParticleDirectionalVisibility.memorySize / (1024.0 * 1024.0),
		mStatus.directionalFroxelsProcessed, mStatus.directionalSamples, mStatus.directionalShadowRays, mStatus.directionalShadowVisible, mStatus.directionalShadowOccluded, mStatus.directionalRadianceClamps,
		mSettings.emissiveLights ? "yes" : "no", renderer.mBoundEmissivePrimitiveCount, mStatus.emissiveFroxelsProcessed, mStatus.emissiveSamples,
		mStatus.emissiveCandidateMisses, mStatus.emissiveDistanceRejected, mStatus.emissiveFacingRejected, mStatus.emissiveShadowRays,
		mStatus.emissiveShadowVisible, mStatus.emissiveShadowOccluded, mStatus.emissiveContributed, mStatus.emissiveRadianceClamps);
	Printf("NRI PT smoke direct reconstruction: grid_only=yes reuse_requested=%u reuse_effective=%u reference=%u history=%s reset=%s cache_mib=%.2f receiver_samples=%u visibility_fractional=%u visibility_zero=%u visibility_one=%u temporal=%u/%u spatial=%u/%u maximum_age=%u resolved=%u clamps=%u nan=%u field_readback=0\n",
		mStatus.directReuseModeRequested, mStatus.directReuseModeEffective, mStatus.directReferenceMode,
		mStatus.directHistoryValid ? "valid" : "invalid", mStatus.directHistoryResetReason,
		(double)mStatus.directHistoryBytes / (1024.0 * 1024.0), mStatus.directReceiverSamples,
		mStatus.directFractionalVisibility, mStatus.directVisibilityZero, mStatus.directVisibilityOne,
		mStatus.directTemporalAccepted, mStatus.directTemporalRejected,
		mStatus.directSpatialAccepted, mStatus.directSpatialRejected,
		mStatus.directHistoryMaximumAge, mStatus.directHistoryResolved,
		mStatus.directHistoryClamps, mStatus.directNanRejects);
	Printf("NRI PT smoke emissive reservoir: reuse_requested=%u reuse_effective=%u lanes=%u reference=%s history=%s reservoir_mib=%.2f initialized=%u invalid=%u temporal=%u/%u spatial=%u/%u final=%u source_clamps=%u removed_energy=%u maximum_age=%u identity_rejects=%u reference_samples=%u reference_rays=%u inner_sets=%u inner_points=%u inner_zeros=%u inner_rejects=%u inner_selections=%u inner_visibility_rays=%u inner_source_visibility_rays=%u inner_source_visible=%u inner_source_blocker_receiver_immediate=%u inner_source_blocker_receiver_cell=%u inner_source_blocker_emitter_cell=%u inner_source_blocker_interior=%u inner_source_selections=%u inner_source_overflow=%u field_readback=0\n",
		mStatus.emissiveReuseModeRequested, mStatus.emissiveReuseModeEffective, mStatus.emissiveLaneCount, mStatus.emissiveReference ? "yes" : "no",
		mStatus.emissiveHistoryValid ? "valid" : "invalid", (double)mStatus.emissiveReservoirBytes / (1024.0 * 1024.0),
		mStatus.emissiveReservoirInitial, mStatus.emissiveReservoirInvalid,
		mStatus.emissiveTemporalAccepted, mStatus.emissiveTemporalRejected,
		mStatus.emissiveSpatialAccepted, mStatus.emissiveSpatialRejected,
		mStatus.emissiveFinalEvaluations, mStatus.emissiveSourceClamps, mStatus.emissiveRemovedEnergy, mStatus.emissiveMaximumAge,
		mStatus.emissiveIdentityRejects, mStatus.emissiveReferenceSamples, mStatus.emissiveReferenceRays,
		mStatus.emissiveInnerRisSets, mStatus.emissiveInnerPointProposals, mStatus.emissiveInnerZeroProposals,
		mStatus.emissiveInnerRisRejects, mStatus.emissiveInnerSelections, mStatus.emissiveInnerVisibilityRays,
		mStatus.emissiveInnerSourceVisibilityRays,
		mStatus.emissiveInnerVisibilityVisible, mStatus.emissiveInnerBlockerReceiverImmediate,
		mStatus.emissiveInnerBlockerReceiverCell, mStatus.emissiveInnerBlockerEmitterCell,
		mStatus.emissiveInnerBlockerInterior, mStatus.emissiveInnerSourceSelections,
		mStatus.emissiveInnerSourceOverflow);
	const auto& analyticLight = mStatus.analyticLight;
	const auto& analyticCpu = analyticLight.cpu;
	Printf("NRI PT smoke analytic light: readback=%s source_frame=%llu epoch=%u profile=%u revision=%u implementation=%u events=%u/%u/%u ready=%u groups=%u free=%u shared=%u anchors=%u/%u built=%u valid=%u invalid=%u samples=%u/%u/%u evaluations=%u build_rays=%u apply_froxels=%u/%u contributions=%u taps=%u hits=%u missing=%u identity_rejects=%u apply_visibility_rays=%u apply_rays_zero=%s dispatch=%u/%u\n",
		analyticLight.valid ? "valid" : "invalid",
		(unsigned long long)analyticLight.sourceFrame, analyticLight.epoch,
		analyticLight.profile, analyticLight.profileRevision, analyticLight.implementation,
		analyticCpu.lightEventsRequestedThisFrame, analyticCpu.lightEventsAdmittedThisFrame,
		analyticCpu.lightEventsRejectedThisFrame, analyticCpu.lightEventsFirstFrameReady,
		analyticCpu.activeLightGroups, analyticCpu.freeLightGroupSlots,
		analyticCpu.sharedCarrierReferences, analyticCpu.lightAnchorsRequested,
		analyticCpu.lightAnchorsReserved, analyticLight.anchorsBuilt,
		analyticLight.anchorsValid, analyticLight.anchorsInvalid,
		analyticCpu.lightSamplesRequested, analyticLight.samplesRequested,
		analyticLight.samplesExecuted, analyticLight.evaluations,
		analyticLight.buildVisibilityRays, analyticLight.applyFroxelsTested,
		analyticLight.applyFroxelsApplied, analyticLight.carrierContributions,
		analyticLight.anchorBlendTaps, analyticLight.groupCacheHits,
		analyticLight.missingGroupRecords, analyticLight.identityRejects,
		analyticLight.applyVisibilityRays,
		analyticLight.applyVisibilityRays == 0u ? "yes" : "no",
		analyticLight.buildDispatchGroups, analyticLight.applyDispatchGroups);
	const uint64_t targetBlocked = (uint64_t)mStatus.emissiveTargetBlockerExact +
		(uint64_t)mStatus.emissiveTargetBlockerRange + (uint64_t)mStatus.emissiveTargetBlockerOther;
	const uint64_t targetAccounted = (uint64_t)mStatus.emissiveTargetVisibilityVisible + targetBlocked;
	float targetWitnessDistance = -1.0f;
	if (mStatus.emissiveTargetWitnessClaim != 0)
		std::memcpy(&targetWitnessDistance, &mStatus.emissiveTargetWitnessDistanceBits, sizeof(targetWitnessDistance));
	const char* targetWitnessRelation = mStatus.emissiveTargetWitnessRelation == 1u ? "exact" :
		mStatus.emissiveTargetWitnessRelation == 2u ? "range" :
		mStatus.emissiveTargetWitnessRelation == 3u ? "other" : "none";
	Printf("NRI PT smoke emissive target visibility: target=%d rays=%u visible=%u exact=%u range=%u other=%u closure=%s witness=%s witness_target=%u relation=%s sampled_primitive=%u sampled_material=%u blocker_source=%u blocker_instance=%u blocker_primitive=%u blocker_material=%u blocker_distance=%.4f\n",
		world.emissiveCandidateTarget, mStatus.emissiveTargetVisibilityRays, mStatus.emissiveTargetVisibilityVisible,
		mStatus.emissiveTargetBlockerExact, mStatus.emissiveTargetBlockerRange, mStatus.emissiveTargetBlockerOther,
		targetAccounted == (uint64_t)mStatus.emissiveTargetVisibilityRays ? "yes" : "no",
		mStatus.emissiveTargetWitnessClaim != 0 ? "yes" : "no", mStatus.emissiveTargetWitnessCandidate,
		targetWitnessRelation, mStatus.emissiveTargetWitnessSamplePrimitive, mStatus.emissiveTargetWitnessSampleMaterial,
		mStatus.emissiveTargetWitnessBlockerDataSource, mStatus.emissiveTargetWitnessBlockerInstance,
		mStatus.emissiveTargetWitnessBlockerPrimitive, mStatus.emissiveTargetWitnessBlockerMaterial,
		targetWitnessDistance);
	Printf("NRI PT smoke indirect status: enabled=%s scale=%.3f cache_mode_requested=%u cache_mode_effective=%u samples=%u history=%s cache_mib=%.2f froxels=%u locality_rays=%u agreement=%u one_sided=%u mismatch=%u invalid=%u reference_rays=%u hits=%u misses=%u sector=%u sky=%u emission=%u clamps=%u nan=%u temporal=%u/%u spatial=%u/%u cache_age=%u cache_clamps=%u resolved=%u field_readback=0\n",
		mSettings.indirect ? "yes" : "no", mSettings.indirectScale, mStatus.indirectCacheModeRequested, mStatus.indirectCacheModeEffective, 1u << std::min(mSettings.quality, 2u),
		mIndirectHistoryValid ? "valid" : "invalid", (double)mStatus.indirectCacheBytes / (1024.0 * 1024.0), mStatus.indirectFroxelsProcessed,
		mStatus.indirectLocalityRays, mStatus.indirectLocalityAgreement, mStatus.indirectLocalityOneSided,
		mStatus.indirectLocalityMismatch, mStatus.indirectLocalityInvalid, mStatus.indirectReferenceRays,
		mStatus.indirectReferenceHits, mStatus.indirectReferenceMisses, mStatus.indirectSectorContributions,
		mStatus.indirectSkyContributions, mStatus.indirectEmissionContributions, mStatus.indirectRadianceClamps,
		mStatus.indirectNanRejects, mStatus.indirectTemporalAccepted, mStatus.indirectTemporalRejected,
		mStatus.indirectSpatialAccepted, mStatus.indirectSpatialRejected, mStatus.indirectCacheMaximumAge,
		mStatus.indirectCacheClamps, mStatus.indirectCacheResolved);
	const char* volumeName = mStatus.volumeResolvedSlot < (uint32_t)NRIRenderer::FrameTextureSlot::Count ?
		renderer.GetFrameTextureSlotName((NRIRenderer::FrameTextureSlot)mStatus.volumeResolvedSlot) : "none";
	const char* volumeMetaName = mStatus.volumeMetaSlot < (uint32_t)NRIRenderer::FrameTextureSlot::Count ?
		renderer.GetFrameTextureSlotName((NRIRenderer::FrameTextureSlot)mStatus.volumeMetaSlot) : "none";
	Printf("NRI PT smoke volume status: history_requested=%s history_effective=%s history_valid=%s history_age=%u reset=%s layer_mib=%.2f resolved=%s metadata=%s dlrr_mode_requested=%u dlrr_mode_effective=%u field_readback=0\n",
		mStatus.volumeHistoryRequested ? "yes" : "no", mStatus.volumeHistoryEffective ? "yes" : "no",
		mStatus.volumeHistoryValid ? "yes" : "no", mStatus.volumeHistoryAge, mStatus.volumeHistoryResetReason,
		(double)mStatus.volumeHistoryBytes / (1024.0 * 1024.0), volumeName, volumeMetaName,
		mStatus.dlrrModeRequested, mStatus.dlrrModeEffective);
}
