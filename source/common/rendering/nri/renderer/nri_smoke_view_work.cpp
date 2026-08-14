#include "nri_smoke_view_work.h"
#include "printf.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace
{
	constexpr uint32_t kThreads = 64u;
	const char* const kPipelineNames[] = {
		"SmokeViewWorkClear",
		"SmokeViewWorkProjectTiles",
		"SmokeViewWorkExpandColumns",
		"SmokeViewWorkCountColumns",
		"SmokeViewWorkPrefixColumns",
		"SmokeViewWorkScatterFroxels",
		"SmokeViewWorkFinalize",
		"SmokeViewWorkCompareDense",
	};
	static_assert(std::size(kPipelineNames) == (size_t)NRISmokeViewWorkPass::Count);

	uint32_t Groups(uint64_t count)
	{
		return (uint32_t)std::max<uint64_t>(1u, (count + kThreads - 1u) / kThreads);
	}

	nri::AccessStage StorageAccess()
	{
		return { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
	}

	nri::AccessStage ArgumentAccess()
	{
		return { nri::AccessBits::ARGUMENT_BUFFER, nri::StageBits::INDIRECT };
	}

	float FloatFromBits(uint32_t bits)
	{
		float value = 0.0f;
		std::memcpy(&value, &bits, sizeof(value));
		return value;
	}
}

NRISmokeViewWorkLayout NRISmokeViewWork::Describe(uint32_t froxelWidth, uint32_t froxelHeight,
	uint32_t froxelDepth, uint32_t brickCapacity)
{
	NRISmokeViewWorkLayout result = {};
	result.tileCountX = (froxelWidth + NRI_SMOKE_VIEW_TILE_AXIS - 1u) / NRI_SMOKE_VIEW_TILE_AXIS;
	result.tileCountY = (froxelHeight + NRI_SMOKE_VIEW_TILE_AXIS - 1u) / NRI_SMOKE_VIEW_TILE_AXIS;
	result.tileCount = result.tileCountX * result.tileCountY;
	result.columnCount = froxelWidth * froxelHeight;
	result.brickTilePairBound = (uint64_t)result.tileCount * brickCapacity;
	result.opticalCellTestBound = result.brickTilePairBound * NRI_SMOKE_GRID_CELLS_PER_BRICK;
	result.preparationUnitBound = result.brickTilePairBound + result.columnCount;
	(void)froxelDepth;
	return result;
}

void NRISmokeViewWork::SetFailure(const char* reason)
{
	mStatus.resourcesReady = false;
	mStatus.failureReason = reason != nullptr ? reason : "unspecified";
}

bool NRISmokeViewWork::CreateBuffer(const NRISmokeGridServices& services, NRIBufferResource& out,
	uint64_t size, uint32_t stride, nri::BufferUsageBits usage)
{
	DestroyBuffer(services, out);
	nri::BufferDesc desc = {};
	desc.size = std::max<uint64_t>(size, stride);
	desc.structureStride = stride;
	desc.usage = usage;
	if (services.core->CreateCommittedBuffer(*services.device, nri::MemoryLocation::DEVICE,
		0.0f, desc, out.buffer) != nri::Result::SUCCESS)
		return false;
	nri::MemoryDesc memory = {};
	services.core->GetBufferMemoryDesc(*out.buffer, nri::MemoryLocation::DEVICE, memory);
	out.size = out.usedSize = desc.size;
	out.memorySize = memory.size;
	out.stride = stride;
	out.usage = usage;
	out.memoryLocation = nri::MemoryLocation::DEVICE;
	nri::BufferViewDesc view = {};
	view.buffer = out.buffer;
	view.type = nri::BufferView::STORAGE_STRUCTURED_BUFFER;
	view.offset = 0;
	view.size = nri::WHOLE_SIZE;
	view.structureStride = stride;
	if (services.core->CreateBufferView(view, out.storageView) != nri::Result::SUCCESS)
	{
		DestroyBuffer(services, out);
		return false;
	}
	return true;
}

bool NRISmokeViewWork::CreateReadback(const NRISmokeGridServices& services, NRIBufferResource& out)
{
	DestroyBuffer(services, out);
	nri::BufferDesc desc = {};
	desc.size = sizeof(NRISmokeViewWorkControlGpu);
	desc.structureStride = sizeof(NRISmokeViewWorkControlGpu);
	desc.usage = nri::BufferUsageBits::NONE;
	if (services.core->CreateCommittedBuffer(*services.device, nri::MemoryLocation::HOST_READBACK,
		0.0f, desc, out.buffer) != nri::Result::SUCCESS)
		return false;
	nri::MemoryDesc memory = {};
	services.core->GetBufferMemoryDesc(*out.buffer, nri::MemoryLocation::HOST_READBACK, memory);
	out.size = out.usedSize = desc.size;
	out.memorySize = memory.size;
	out.stride = desc.structureStride;
	out.memoryLocation = nri::MemoryLocation::HOST_READBACK;
	return true;
}

void NRISmokeViewWork::DestroyBuffer(const NRISmokeGridServices& services, NRIBufferResource& resource)
{
	if (services.core != nullptr)
	{
		if (resource.storageView != nullptr) services.core->DestroyDescriptor(resource.storageView);
		if (resource.buffer != nullptr) services.core->DestroyBuffer(resource.buffer);
	}
	resource = {};
}

void NRISmokeViewWork::DestroyResources(const NRISmokeGridServices& services)
{
	DestroyBuffer(services, mTileMasks);
	DestroyBuffer(services, mColumnMasks);
	DestroyBuffer(services, mControl);
	DestroyBuffer(services, mCompactIndices);
	DestroyBuffer(services, mIndirectArgs);
	DestroyBuffer(services, mColumnOffsets);
	mResourceFroxelDepth = 0u;
	mResourcesInitialized = false;
	mCompactResourcesInitialized = false;
	mIndirectInitialized = false;
	mIndirectIsArgument = false;
	mStatus.layout = {};
	mStatus.residentBytes = 0u;
	mStatus.resourcesReady = false;
}

bool NRISmokeViewWork::Initialize(const NRISmokeGridServices& services)
{
	if (mPipelineLayout != nullptr)
		return true;
	if (!services.IsDeviceValid() || services.loadShaderBlob == nullptr)
	{
		SetFailure("invalid-services");
		return false;
	}
	nri::DescriptorRangeDesc range = {};
	range.baseRegisterIndex = 0u;
	range.descriptorNum = StorageDescriptorCount;
	range.descriptorType = nri::DescriptorType::STORAGE_STRUCTURED_BUFFER;
	range.shaderStages = nri::StageBits::COMPUTE_SHADER;
	range.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;
	nri::DescriptorSetDesc set = {};
	set.registerSpace = 0u;
	set.ranges = &range;
	set.rangeNum = 1u;
	set.flags = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;
	nri::RootConstantDesc root = {};
	root.registerIndex = 0u;
	root.size = sizeof(NRISmokeViewWorkConstants);
	root.shaderStages = nri::StageBits::COMPUTE_SHADER;
	nri::PipelineLayoutDesc layout = {};
	layout.rootRegisterSpace = 1u;
	layout.rootConstants = &root;
	layout.rootConstantNum = 1u;
	layout.descriptorSets = &set;
	layout.descriptorSetNum = 1u;
	layout.shaderStages = nri::StageBits::COMPUTE_SHADER;
	if (services.core->CreatePipelineLayout(*services.device, layout, mPipelineLayout) != nri::Result::SUCCESS)
	{
		SetFailure("pipeline-layout");
		return false;
	}
	const bool d3d12 = services.graphicsAPI == nri::GraphicsAPI::D3D12;
	for (uint32_t i = 0u; i < mPipelines.size(); ++i)
	{
		std::vector<uint8_t> blob;
		const std::string file = std::string(kPipelineNames[i]) + ".cs." + (d3d12 ? "dxil" : "spirv");
		if (!services.LoadShaderBlob(file.c_str(), blob))
		{
			SetFailure("shader-blob");
			Shutdown(services);
			return false;
		}
		nri::ShaderDesc shader = {};
		shader.stage = nri::StageBits::COMPUTE_SHADER;
		shader.bytecode = blob.data();
		shader.size = blob.size();
		shader.entryPointName = "main";
		nri::ComputePipelineDesc desc = {};
		desc.pipelineLayout = mPipelineLayout;
		desc.shader = shader;
		if (services.core->CreateComputePipeline(*services.device, desc, mPipelines[i]) != nri::Result::SUCCESS)
		{
			SetFailure("compute-pipeline");
			Shutdown(services);
			return false;
		}
	}
	if (services.core->AllocateDescriptorSets(*services.descriptorPool, *mPipelineLayout, 0,
		&mStorageSet, 1, 0) != nri::Result::SUCCESS)
	{
		SetFailure("descriptor-set");
		Shutdown(services);
		return false;
	}
	mStatus.initialized = true;
	mFrameSlots.resize(std::max(services.queuedFrameCount, 1u));
	for (FrameSlot& slot : mFrameSlots)
	{
		if (!CreateReadback(services, slot.controlReadback))
		{
			SetFailure("readback-buffer");
			Shutdown(services);
			return false;
		}
	}
	mStatus.failureReason = "none";
	return true;
}

bool NRISmokeViewWork::EnsureResources(const NRISmokeGridServices& services,
	const NRISmokeViewWorkLayout& layout, uint32_t froxelDepth)
{
	if (mTileMasks.buffer != nullptr && mStatus.layout.tileCount == layout.tileCount &&
		mStatus.layout.columnCount == layout.columnCount && mResourceFroxelDepth == froxelDepth)
		return true;
	services.WaitForCommands("smoke-view-work-resize");
	DestroyResources(services);
	const auto storage = nri::BufferUsageBits::SHADER_RESOURCE_STORAGE;
	const auto indirectStorage = NRIResourceFlags(storage, nri::BufferUsageBits::ARGUMENT_BUFFER);
	const uint64_t froxelCount = (uint64_t)layout.columnCount * froxelDepth;
	const bool created =
		CreateBuffer(services, mTileMasks, (uint64_t)layout.tileCount * sizeof(NRISmokeViewMaskGpu), sizeof(NRISmokeViewMaskGpu), storage) &&
		CreateBuffer(services, mColumnMasks, (uint64_t)layout.columnCount * sizeof(NRISmokeViewMaskGpu), sizeof(NRISmokeViewMaskGpu), storage) &&
		CreateBuffer(services, mControl, sizeof(NRISmokeViewWorkControlGpu), sizeof(NRISmokeViewWorkControlGpu), storage) &&
		CreateBuffer(services, mCompactIndices, froxelCount * sizeof(uint32_t), sizeof(uint32_t), storage) &&
		CreateBuffer(services, mIndirectArgs, 2u * sizeof(NRISmokeViewIndirectArgsGpu), sizeof(NRISmokeViewIndirectArgsGpu), indirectStorage) &&
		CreateBuffer(services, mColumnOffsets, (uint64_t)layout.columnCount * sizeof(uint32_t), sizeof(uint32_t), storage);
	if (!created)
	{
		DestroyResources(services);
		SetFailure("resource-allocation");
		return false;
	}
	mStatus.layout = layout;
	mResourceFroxelDepth = froxelDepth;
	mStatus.residentBytes = mTileMasks.memorySize + mColumnMasks.memorySize + mControl.memorySize +
		mCompactIndices.memorySize + mIndirectArgs.memorySize + mColumnOffsets.memorySize;
	mStatus.resourcesReady = true;
	mStatus.failureReason = "none";
	return true;
}

void NRISmokeViewWork::TransitionOutputsToStorage(const NRISmokeGridServices& services, bool compactRoute)
{
	std::array<nri::BufferBarrierDesc, 6> barriers = {};
	uint32_t count = 0u;
	if (!mResourcesInitialized)
	{
		NRIBufferResource* resources[] = { &mTileMasks, &mColumnMasks, &mControl };
		for (NRIBufferResource* resource : resources)
		{
			barriers[count].buffer = resource->buffer;
			barriers[count].before = {};
			barriers[count].after = StorageAccess();
			count++;
		}
		mResourcesInitialized = true;
	}
	if (compactRoute && !mCompactResourcesInitialized)
	{
		NRIBufferResource* resources[] = { &mCompactIndices, &mColumnOffsets };
		for (NRIBufferResource* resource : resources)
		{
			barriers[count].buffer = resource->buffer;
			barriers[count].before = {};
			barriers[count].after = StorageAccess();
			count++;
		}
		mCompactResourcesInitialized = true;
	}
	if (compactRoute && !mIndirectInitialized)
	{
		barriers[count].buffer = mIndirectArgs.buffer;
		barriers[count].before = {};
		barriers[count].after = StorageAccess();
		count++;
		mIndirectInitialized = true;
	}
	if (count == 0u)
		return;
	nri::BarrierDesc desc = {};
	desc.buffers = barriers.data();
	desc.bufferNum = count;
	services.core->CmdBarrier(*services.commandBuffer, desc);
}

void NRISmokeViewWork::StorageBarrier(const NRISmokeGridServices& services, bool compactRoute)
{
	NRIBufferResource* resources[] = { &mTileMasks, &mColumnMasks, &mControl };
	std::array<nri::BufferBarrierDesc, 6> barriers = {};
	uint32_t count = 0u;
	for (NRIBufferResource* resource : resources)
	{
		barriers[count].buffer = resource->buffer;
		barriers[count].before = StorageAccess();
		barriers[count].after = StorageAccess();
		count++;
	}
	if (compactRoute)
	{
		NRIBufferResource* compactResources[] = { &mCompactIndices, &mColumnOffsets };
		for (NRIBufferResource* resource : compactResources)
		{
			barriers[count].buffer = resource->buffer;
			barriers[count].before = StorageAccess();
			barriers[count].after = StorageAccess();
			count++;
		}
		barriers[count].buffer = mIndirectArgs.buffer;
		barriers[count].before = StorageAccess();
		barriers[count].after = StorageAccess();
		count++;
	}
	nri::BarrierDesc desc = {};
	desc.buffers = barriers.data();
	desc.bufferNum = count;
	services.core->CmdBarrier(*services.commandBuffer, desc);
}

void NRISmokeViewWork::Dispatch(const NRISmokeGridServices& services, NRISmokeViewWorkPass pass,
	const NRISmokeViewWorkConstants& constants, uint32_t groups)
{
	NRISmokeViewWorkConstants local = constants;
	local.pass = (uint32_t)pass;
	services.core->CmdSetPipeline(*services.commandBuffer, *mPipelines[(size_t)pass]);
	nri::SetRootConstantsDesc root = {};
	root.rootConstantIndex = 0u;
	root.data = &local;
	root.size = sizeof(local);
	root.offset = 0u;
	root.bindPoint = nri::BindPoint::COMPUTE;
	services.core->CmdSetRootConstants(*services.commandBuffer, root);
	services.core->CmdDispatch(*services.commandBuffer, { std::max(groups, 1u), 1u, 1u });
}

bool NRISmokeViewWork::Prepare(const NRISmokeGridServices& services, const NRISmokeViewWorkFrame& frame)
{
	const auto& c = frame.constants;
	const bool compactRoute = c.executionRoute == 2u;
	ConsumeReadback(services, c.simulationEpoch);
	if (!services.IsRecordingValid() || mPipelineLayout == nullptr || mStorageSet == nullptr ||
		c.froxelWidth == 0u || c.froxelHeight == 0u || c.froxelDepth == 0u ||
		c.froxelDepth > NRI_SMOKE_VIEW_MAX_DEPTH || c.brickCapacity == 0u || c.cellSize <= 0.0f ||
		std::any_of(frame.gridDescriptors.begin(), frame.gridDescriptors.end(),
			[](const nri::Descriptor* descriptor) { return descriptor == nullptr; }))
	{
		SetFailure("prepare-prerequisite");
		return false;
	}
	const NRISmokeViewWorkLayout layout = Describe(c.froxelWidth, c.froxelHeight, c.froxelDepth, c.brickCapacity);
	if (c.tileCountX != layout.tileCountX || c.tileCountY != layout.tileCountY ||
		!EnsureResources(services, layout, c.froxelDepth))
	{
		SetFailure(c.tileCountX != layout.tileCountX || c.tileCountY != layout.tileCountY ?
			"tile-layout-mismatch" : mStatus.failureReason);
		return false;
	}
	if (compactRoute && mIndirectIsArgument)
	{
		nri::BufferBarrierDesc buffer = {};
		buffer.buffer = mIndirectArgs.buffer;
		buffer.before = ArgumentAccess();
		buffer.after = StorageAccess();
		nri::BarrierDesc barrier = {};
		barrier.buffers = &buffer;
		barrier.bufferNum = 1u;
		services.core->CmdBarrier(*services.commandBuffer, barrier);
		mIndirectIsArgument = false;
	}
	const nri::Descriptor* descriptors[] = {
		frame.gridDescriptors[0], frame.gridDescriptors[1], frame.gridDescriptors[2], frame.gridDescriptors[3],
		mTileMasks.storageView, mColumnMasks.storageView, mControl.storageView,
		mCompactIndices.storageView, mIndirectArgs.storageView,
		mControl.storageView, mControl.storageView, mColumnOffsets.storageView,
	};
	nri::UpdateDescriptorRangeDesc update = {};
	update.descriptorSet = mStorageSet;
	update.rangeIndex = 0u;
	update.descriptors = descriptors;
	update.descriptorNum = (uint32_t)std::size(descriptors);
	services.core->UpdateDescriptorRanges(&update, 1u);
	services.core->CmdSetPipelineLayout(*services.commandBuffer, nri::BindPoint::COMPUTE, *mPipelineLayout);
	services.core->CmdSetDescriptorSet(*services.commandBuffer, { 0, mStorageSet, nri::BindPoint::COMPUTE });
	TransitionOutputsToStorage(services, compactRoute);
	Dispatch(services, NRISmokeViewWorkPass::Clear, c, Groups(std::max(layout.tileCount, layout.columnCount)));
	StorageBarrier(services, compactRoute);
	Dispatch(services, NRISmokeViewWorkPass::ProjectTiles, c, layout.tileCount);
	StorageBarrier(services, compactRoute);
	Dispatch(services, NRISmokeViewWorkPass::ExpandColumns, c, Groups(layout.columnCount));
	StorageBarrier(services, compactRoute);
	if (compactRoute)
	{
		Dispatch(services, NRISmokeViewWorkPass::CountColumns, c, Groups(layout.columnCount));
		StorageBarrier(services, true);
		Dispatch(services, NRISmokeViewWorkPass::PrefixColumns, c, 1u);
		StorageBarrier(services, true);
		Dispatch(services, NRISmokeViewWorkPass::ScatterFroxels, c, Groups(layout.columnCount));
		StorageBarrier(services, true);
	}
	Dispatch(services, NRISmokeViewWorkPass::Finalize, c, 1u);
	StorageBarrier(services, compactRoute);
	mLastConstants = c;
	return true;
}

bool NRISmokeViewWork::CompareDense(const NRISmokeGridServices& services,
	const nri::Descriptor* denseMedium, const nri::Descriptor* denseSource)
{
	if (!services.IsRecordingValid() || !mStatus.resourcesReady || denseMedium == nullptr ||
		denseSource == nullptr || mLastConstants.simulationEpoch == 0u)
	{
		SetFailure("compare-prerequisite");
		return false;
	}
	if (mLastConstants.executionRoute == 2u && mIndirectIsArgument)
	{
		nri::BufferBarrierDesc buffer = {};
		buffer.buffer = mIndirectArgs.buffer;
		buffer.before = ArgumentAccess();
		buffer.after = StorageAccess();
		nri::BarrierDesc barrier = {};
		barrier.buffers = &buffer;
		barrier.bufferNum = 1u;
		services.core->CmdBarrier(*services.commandBuffer, barrier);
		mIndirectIsArgument = false;
	}
	const nri::Descriptor* descriptors[] = { denseMedium, denseSource };
	nri::UpdateDescriptorRangeDesc update = {};
	update.descriptorSet = mStorageSet;
	update.rangeIndex = 0u;
	update.baseDescriptor = 9u;
	update.descriptors = descriptors;
	update.descriptorNum = (uint32_t)std::size(descriptors);
	services.core->UpdateDescriptorRanges(&update, 1u);
	services.core->CmdSetPipelineLayout(*services.commandBuffer, nri::BindPoint::COMPUTE, *mPipelineLayout);
	services.core->CmdSetDescriptorSet(*services.commandBuffer, { 0, mStorageSet, nri::BindPoint::COMPUTE });
	const uint64_t froxelCount = (uint64_t)mLastConstants.froxelWidth *
		mLastConstants.froxelHeight * mLastConstants.froxelDepth;
	Dispatch(services, NRISmokeViewWorkPass::CompareDense, mLastConstants, Groups(froxelCount));
	StorageBarrier(services, mLastConstants.executionRoute == 2u);
	RecordReadback(services);
	return true;
}

void NRISmokeViewWork::Finish(const NRISmokeGridServices& services)
{
	if (services.IsRecordingValid() && mStatus.resourcesReady)
		RecordReadback(services);
}

void NRISmokeViewWork::TransitionIndirectToArgument(const NRISmokeGridServices& services)
{
	if (mIndirectIsArgument || !services.IsRecordingValid() || mIndirectArgs.buffer == nullptr)
		return;
	nri::BufferBarrierDesc buffer = {};
	buffer.buffer = mIndirectArgs.buffer;
	buffer.before = StorageAccess();
	buffer.after = ArgumentAccess();
	nri::BarrierDesc barrier = {};
	barrier.buffers = &buffer;
	barrier.bufferNum = 1u;
	services.core->CmdBarrier(*services.commandBuffer, barrier);
	mIndirectIsArgument = true;
}

void NRISmokeViewWork::ConsumeReadback(const NRISmokeGridServices& services, uint32_t simulationEpoch)
{
	if (mFrameSlots.empty())
		return;
	FrameSlot& slot = mFrameSlots[std::min(services.queuedFrameIndex, (uint32_t)mFrameSlots.size() - 1u)];
	if (!slot.pending || slot.controlReadback.buffer == nullptr)
		return;
	mStatus.gpuStatsValid = false;
	const void* mapped = services.core->MapBuffer(*slot.controlReadback.buffer, 0,
		sizeof(NRISmokeViewWorkControlGpu));
	if (mapped != nullptr)
	{
		NRISmokeViewWorkControlGpu next = {};
		std::memcpy(&next, mapped, sizeof(next));
		services.core->UnmapBuffer(*slot.controlReadback.buffer);
		if (slot.simulationEpoch == simulationEpoch && next.simulationEpoch == simulationEpoch)
		{
			mStatus.gpu = next;
			mStatus.gpuRendererFrame = slot.rendererFrame;
			mStatus.gpuStatsValid = true;
			Printf("PERF pt smoke view work NRI: renderer_frame=%llu frame=%u epoch=%u route=%u dispatched=%u selected=%u skipped=%u brick_tile_tests=%u resident_brick_tile_tests=%u optical_cell_tests=%u contributing_brick_tile_pairs=%u empty_brick_tile_pairs=%u dense_contributing=%u unique_froxels=%u unique_columns=%u compact_count=%u prefix_columns=%u scatter_writes=%u compact_capacity=%u false_negatives=%u false_positives=%u output_hash=%08x%08x boundary_false_negatives=%u overflow=%u compact=1\n",
				(unsigned long long)mStatus.gpuRendererFrame, next.frameStamp, next.simulationEpoch,
				next.evaluationRoute, next.evaluationDispatched, next.evaluationSelected,
				next.evaluationSkipped, next.brickTileTests, next.residentBrickTileTests,
				next.opticalCellTests, next.contributingBrickTilePairs, next.emptyBrickTilePairs,
				next.denseContributing, next.uniqueFroxels,
				next.uniqueColumns, next.compactCount, next.prefixColumns, next.scatterWrites,
				next.compactCapacity, next.falseNegatives, next.falsePositives,
				next.outputHashHi, next.outputHashLo, next.boundaryFalseNegatives, next.overflow);
		}
	}
	slot.pending = false;
}

void NRISmokeViewWork::RecordReadback(const NRISmokeGridServices& services)
{
	if (mFrameSlots.empty())
		return;
	FrameSlot& slot = mFrameSlots[std::min(services.queuedFrameIndex, (uint32_t)mFrameSlots.size() - 1u)];
	nri::BufferBarrierDesc barriers[2] = {};
	barriers[0].buffer = mControl.buffer;
	barriers[0].before = StorageAccess();
	barriers[0].after = NRIResourceCopySourceAccess();
	barriers[1].buffer = slot.controlReadback.buffer;
	barriers[1].before = slot.initialized ? NRIResourceCopyDestinationAccess() : nri::AccessStage{};
	barriers[1].after = NRIResourceCopyDestinationAccess();
	nri::BarrierDesc before = {};
	before.buffers = barriers;
	before.bufferNum = 2u;
	services.core->CmdBarrier(*services.commandBuffer, before);
	services.core->CmdCopyBuffer(*services.commandBuffer, *slot.controlReadback.buffer, 0,
		*mControl.buffer, 0, sizeof(NRISmokeViewWorkControlGpu));
	nri::BufferBarrierDesc restore = {};
	restore.buffer = mControl.buffer;
	restore.before = NRIResourceCopySourceAccess();
	restore.after = StorageAccess();
	nri::BarrierDesc after = {};
	after.buffers = &restore;
	after.bufferNum = 1u;
	services.core->CmdBarrier(*services.commandBuffer, after);
	slot.pending = true;
	slot.initialized = true;
	slot.rendererFrame = services.rendererFrame;
	slot.simulationEpoch = mLastConstants.simulationEpoch;
}

void NRISmokeViewWork::PrintStatus(bool compareRequested, uint32_t routeRequested) const
{
	const auto& g = mStatus.gpu;
	Printf("NRI PT smoke view work: requested=%s compare=%s route_requested=%u route_effective=%u authority=smoke-evaluate-grid comparator_output_mutation=no initialized=%s resources=%s gpu_stats=%s renderer_frame=%llu frame=%u epoch=%u tiles=%u columns=%u brick_tile_bound=%llu optical_cell_bound=%llu dispatched=%u selected=%u skipped=%u brick_tile_tests=%u resident_brick_tile_tests=%u optical_cell_tests=%u contributing_brick_tile_pairs=%u empty_brick_tile_pairs=%u dense_contributing=%u unique_froxels=%u unique_columns=%u compact_count=%u prefix_columns=%u scatter_writes=%u compact_capacity=%u false_negatives=%u false_positives=%u output_hash=%08x%08x tau_error_max=%.9g opacity_error_max=%.9g radiance_error_max=%.9g boundary_false_negatives=%u near_plane_spans=%u camera_inside_spans=%u overflow=%u reason=%s\n",
		(compareRequested || routeRequested != 0u) ? "yes" : "no", compareRequested ? "yes" : "no",
		routeRequested, g.evaluationRoute, mStatus.initialized ? "yes" : "no",
		mStatus.resourcesReady ? "ready" : "unavailable", mStatus.gpuStatsValid ? "valid" : "disabled",
		(unsigned long long)mStatus.gpuRendererFrame, g.frameStamp, g.simulationEpoch,
		mStatus.layout.tileCount, mStatus.layout.columnCount,
		(unsigned long long)mStatus.layout.brickTilePairBound,
		(unsigned long long)mStatus.layout.opticalCellTestBound,
		g.evaluationDispatched, g.evaluationSelected, g.evaluationSkipped, g.brickTileTests,
		g.residentBrickTileTests, g.opticalCellTests, g.contributingBrickTilePairs,
		g.emptyBrickTilePairs, g.denseContributing,
		g.uniqueFroxels, g.uniqueColumns, g.compactCount, g.prefixColumns, g.scatterWrites,
		g.compactCapacity, g.falseNegatives, g.falsePositives,
		g.outputHashHi, g.outputHashLo,
		(double)FloatFromBits(g.tauErrorBits), (double)FloatFromBits(g.opacityErrorBits),
		(double)FloatFromBits(g.radianceErrorBits), g.boundaryFalseNegatives,
		g.nearPlaneSpans, g.cameraInsideSpans, g.overflow, mStatus.failureReason);
}

bool NRISmokeViewWork::GetOutputs(NRISmokeViewWorkOutputs& outputs) const
{
	outputs = {};
	if (!mStatus.resourcesReady)
		return false;
	outputs.columnMasks = mColumnMasks.storageView;
	outputs.compactIndices = mCompactIndices.storageView;
	outputs.control = mControl.storageView;
	outputs.indirectArgs = mIndirectArgs.storageView;
	outputs.indirectBuffer = mIndirectArgs.buffer;
	outputs.columnCount = mStatus.layout.columnCount;
	outputs.froxelCapacity = mStatus.layout.columnCount * mResourceFroxelDepth;
	return true;
}

void NRISmokeViewWork::Shutdown(const NRISmokeGridServices& services)
{
	DestroyResources(services);
	for (FrameSlot& slot : mFrameSlots)
		DestroyBuffer(services, slot.controlReadback);
	mFrameSlots.clear();
	if (services.core != nullptr)
	{
		for (nri::Pipeline*& pipeline : mPipelines)
		{
			if (pipeline != nullptr) services.core->DestroyPipeline(pipeline);
			pipeline = nullptr;
		}
		if (mPipelineLayout != nullptr) services.core->DestroyPipelineLayout(mPipelineLayout);
	}
	mPipelineLayout = nullptr;
	mStorageSet = nullptr;
	mStatus = {};
}
