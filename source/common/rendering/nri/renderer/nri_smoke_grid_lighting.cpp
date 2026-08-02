#include "nri_smoke_grid_lighting.h"

#include "../system/nri_gpu_timing.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace
{
	constexpr uint32_t kThreads = 64u;
	const char* const kPipelineNames[] = {
		"SmokeGridLightPrepare", "SmokeGridLightClearSupport", "SmokeGridLightBuildActive", "SmokeGridLightBuildProposals", "SmokeGridLightSeed",
		"SmokeGridLightTemporal", "SmokeGridLightBuildLinks", "SmokeGridLightFilter",
		"SmokeGridLightSeedScattering", "SmokeGridLightPropagateScattering"
	};
	static_assert(std::size(kPipelineNames) == (size_t)NRISmokeGridLightingPass::Count);

	uint32_t Groups(uint64_t count)
	{
		return (uint32_t)std::max<uint64_t>(1u, (count + kThreads - 1u) / kThreads);
	}

	nri::AccessStage StorageAccess()
	{
		return { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
	}
}

bool NRISmokeGridLighting::Initialize(const NRISmokeGridServices& services, nri::PipelineLayout* sharedLayout)
{
	if (mSharedLayout != nullptr)
		return true;
	if (!services.IsDeviceValid() || sharedLayout == nullptr)
		return false;
	mSharedLayout = sharedLayout;
	const bool d3d12 = services.graphicsAPI == nri::GraphicsAPI::D3D12;
	for (uint32_t i = 0u; i < (uint32_t)mPipelines.size(); ++i)
	{
		std::vector<uint8_t> blob;
		const std::string file = std::string(kPipelineNames[i]) + ".cs." + (d3d12 ? "dxil" : "spirv");
		if (!services.LoadShaderBlob(file.c_str(), blob))
		{
			for (nri::Pipeline*& pipeline : mPipelines) { if (pipeline != nullptr) services.core->DestroyPipeline(pipeline); pipeline = nullptr; }
			mSharedLayout = nullptr;
			mStatus.failureReason = "shader-load-failed";
			return false;
		}
		nri::ShaderDesc shader = {};
		shader.stage = nri::StageBits::COMPUTE_SHADER;
		shader.bytecode = blob.data();
		shader.size = blob.size();
		shader.entryPointName = "main";
		nri::ComputePipelineDesc desc = {};
		desc.pipelineLayout = sharedLayout;
		desc.shader = shader;
		if (services.core->CreateComputePipeline(*services.device, desc, mPipelines[i]) != nri::Result::SUCCESS)
		{
			for (nri::Pipeline*& pipeline : mPipelines) { if (pipeline != nullptr) services.core->DestroyPipeline(pipeline); pipeline = nullptr; }
			mSharedLayout = nullptr;
			mStatus.failureReason = "pipeline-create-failed";
			return false;
		}
	}
	mStatus.initialized = true;
	mStatus.failureReason = "none";
	return true;
}

bool NRISmokeGridLighting::CreateBuffer(const NRISmokeGridServices& services, NRIBufferResource& out,
	uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::MemoryLocation location, bool storageView)
{
	DestroyBuffer(services, out);
	nri::BufferDesc desc = {};
	desc.size = std::max<uint64_t>(size, stride);
	desc.structureStride = stride;
	desc.usage = usage;
	if (services.core->CreateCommittedBuffer(*services.device, location, 0.0f, desc, out.buffer) != nri::Result::SUCCESS)
		return false;
	nri::MemoryDesc memory = {};
	services.core->GetBufferMemoryDesc(*out.buffer, location, memory);
	out.size = out.usedSize = desc.size;
	out.memorySize = memory.size;
	out.stride = stride;
	out.usage = usage;
	out.memoryLocation = location;
	if (!storageView)
		return true;
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

void NRISmokeGridLighting::DestroyBuffer(const NRISmokeGridServices& services, NRIBufferResource& resource)
{
	if (resource.storageView != nullptr) services.core->DestroyDescriptor(resource.storageView);
	if (resource.shaderView != nullptr) services.core->DestroyDescriptor(resource.shaderView);
	if (resource.buffer != nullptr) services.core->DestroyBuffer(resource.buffer);
	resource = {};
}

void NRISmokeGridLighting::DestroyResources(const NRISmokeGridServices& services)
{
	DestroyBuffer(services, mCurrent);
	DestroyBuffer(services, mHistory);
	DestroyBuffer(services, mActive);
	DestroyBuffer(services, mControl);
	DestroyBuffer(services, mLinks);
	DestroyBuffer(services, mFiltered);
	DestroyBuffer(services, mProposals);
	DestroyBuffer(services, mScatterSeed);
	DestroyBuffer(services, mScatterBounceA);
	DestroyBuffer(services, mScatterBounceB);
	DestroyBuffer(services, mScatterMetadata);
	DestroyBuffer(services, mScatterActive);
	DestroyBuffer(services, mSelfShadowCurrent);
	DestroyBuffer(services, mSelfShadowHistory);
	DestroyBuffer(services, mSupportStamps);
	for (FrameSlot& slot : mFrameSlots)
		DestroyBuffer(services, slot.controlReadback);
	mFrameSlots.clear();
	mResourceCellCapacity = 0u;
	mResourceBrickCapacity = 0u;
	mResourceScatterProbeCapacity = 0u;
	mResourceScatterRequested = false;
	mResourceSelfShadowRequested = false;
	mStatus.resourcesReady = false;
	mResourcesInitialized = false;
	mStatus.fieldBytes = mStatus.workBytes = mStatus.linkBytes = mStatus.proposalBytes = mStatus.filterBytes = mStatus.totalBytes = 0u;
	mStatus.scatterSeedBytes = mStatus.scatterBounceBytes = mStatus.scatterMetadataBytes = mStatus.scatterActiveBytes = mStatus.scatterBytes = 0u;
	mStatus.multipleScatterAllocated = false;
	mStatus.multipleScatterEffective = false;
	mStatus.selfShadowAllocated = false;
	mStatus.selfShadowEffective = false;
	mStatus.selfShadowFieldBytes = 0u;
	mStatus.gpuStatsValid = false;
}

bool NRISmokeGridLighting::EnsureResources(const NRISmokeGridServices& services, uint32_t cellCapacity,
	bool filterRequested, bool multipleScatterRequested, bool selfShadowRequested)
{
	const bool ready = mCurrent.buffer != nullptr && mHistory.buffer != nullptr && mActive.buffer != nullptr && mSupportStamps.buffer != nullptr &&
		mControl.buffer != nullptr && mLinks.buffer != nullptr && mProposals.buffer != nullptr && mResourceCellCapacity == cellCapacity &&
		((mFiltered.buffer != nullptr) == filterRequested) && mResourceScatterRequested == multipleScatterRequested &&
		mResourceSelfShadowRequested == selfShadowRequested && mFrameSlots.size() == services.queuedFrameCount &&
		std::all_of(mFrameSlots.begin(), mFrameSlots.end(), [](const FrameSlot& slot) { return slot.controlReadback.buffer != nullptr; });
	if (ready)
		return true;
	services.WaitForCommands("smoke-grid-lighting-resize");
	DestroyResources(services);
	const nri::BufferUsageBits storage = nri::BufferUsageBits::SHADER_RESOURCE_STORAGE;
	const uint64_t cells = std::max(cellCapacity, 1u);
	const uint64_t bricks = std::max<uint64_t>((cells + NRI_SMOKE_GRID_CELLS_PER_BRICK - 1u) / NRI_SMOKE_GRID_CELLS_PER_BRICK, 1u);
	const uint64_t scatterProbes = bricks * NRI_SMOKE_GRID_SCATTER_PROBES_PER_BRICK;
	if (!CreateBuffer(services, mCurrent, cells * sizeof(NRISmokeGridLightRecordGpu), sizeof(NRISmokeGridLightRecordGpu), storage) ||
		!CreateBuffer(services, mHistory, cells * sizeof(NRISmokeGridLightRecordGpu), sizeof(NRISmokeGridLightRecordGpu), storage) ||
		!CreateBuffer(services, mActive, cells * sizeof(uint32_t), sizeof(uint32_t), storage) ||
		!CreateBuffer(services, mControl, sizeof(NRISmokeGridLightControlGpu), sizeof(NRISmokeGridLightControlGpu), storage) ||
		!CreateBuffer(services, mLinks, cells * sizeof(uint32_t) * 4u, sizeof(uint32_t) * 4u, storage) ||
		!CreateBuffer(services, mSupportStamps, cells * sizeof(NRISmokeGridLightSupportStampGpu), sizeof(NRISmokeGridLightSupportStampGpu), storage) ||
		!CreateBuffer(services, mProposals, bricks * sizeof(NRISmokeGridLightProposalGpu), sizeof(NRISmokeGridLightProposalGpu), storage) ||
		(filterRequested && !CreateBuffer(services, mFiltered, cells * sizeof(NRISmokeGridLightRecordGpu), sizeof(NRISmokeGridLightRecordGpu), storage)) ||
		(selfShadowRequested &&
			(!CreateBuffer(services, mSelfShadowCurrent, cells * sizeof(NRISmokeGridLightRecordGpu), sizeof(NRISmokeGridLightRecordGpu), storage) ||
			 !CreateBuffer(services, mSelfShadowHistory, cells * sizeof(NRISmokeGridLightRecordGpu), sizeof(NRISmokeGridLightRecordGpu), storage))))
	{
		DestroyResources(services);
		mStatus.failureReason = "allocation-failed";
		return false;
	}
	mFrameSlots.resize(std::max(services.queuedFrameCount, 1u));
	for (FrameSlot& slot : mFrameSlots)
	{
		if (!CreateBuffer(services, slot.controlReadback, sizeof(NRISmokeGridLightControlGpu),
			sizeof(NRISmokeGridLightControlGpu), nri::BufferUsageBits::NONE, nri::MemoryLocation::HOST_READBACK, false))
		{
			DestroyResources(services);
			mStatus.failureReason = "readback-allocation-failed";
			return false;
		}
	}
	bool scatterAllocated = false;
	if (multipleScatterRequested)
	{
		scatterAllocated = CreateBuffer(services, mScatterSeed, scatterProbes * sizeof(float) * 4u, sizeof(float) * 4u, storage) &&
			CreateBuffer(services, mScatterBounceA, scatterProbes * sizeof(float) * 4u, sizeof(float) * 4u, storage) &&
			CreateBuffer(services, mScatterBounceB, scatterProbes * sizeof(float) * 4u, sizeof(float) * 4u, storage) &&
			CreateBuffer(services, mScatterMetadata, scatterProbes * sizeof(NRISmokeGridScatterMetadataGpu), sizeof(NRISmokeGridScatterMetadataGpu), storage) &&
			CreateBuffer(services, mScatterActive, scatterProbes * sizeof(uint32_t), sizeof(uint32_t), storage);
		if (!scatterAllocated)
		{
			DestroyBuffer(services, mScatterSeed);
			DestroyBuffer(services, mScatterBounceA);
			DestroyBuffer(services, mScatterBounceB);
			DestroyBuffer(services, mScatterMetadata);
			DestroyBuffer(services, mScatterActive);
		}
	}
	// The optional descriptor remains valid while filtering is disabled without
	// allocating a third field: the accepted current field is a safe alias.
	mResourceCellCapacity = cellCapacity;
	mResourceBrickCapacity = (uint32_t)bricks;
	mResourceScatterProbeCapacity = (uint32_t)scatterProbes;
	mResourceScatterRequested = multipleScatterRequested;
	mResourceSelfShadowRequested = selfShadowRequested;
	mStatus.cellCapacity = cellCapacity;
	mStatus.filterAllocated = mFiltered.buffer != nullptr;
	mStatus.fieldBytes = mCurrent.memorySize + mHistory.memorySize;
	mStatus.workBytes = mActive.memorySize + mControl.memorySize + mSupportStamps.memorySize;
	mStatus.linkBytes = mLinks.memorySize;
	mStatus.proposalBytes = mProposals.memorySize;
	mStatus.scatterSeedBytes = mScatterSeed.memorySize;
	mStatus.scatterBounceBytes = mScatterBounceA.memorySize + mScatterBounceB.memorySize;
	mStatus.scatterMetadataBytes = mScatterMetadata.memorySize;
	mStatus.scatterActiveBytes = mScatterActive.memorySize;
	mStatus.scatterBytes = mStatus.scatterSeedBytes + mStatus.scatterBounceBytes + mStatus.scatterMetadataBytes + mStatus.scatterActiveBytes;
	mStatus.selfShadowFieldBytes = mSelfShadowCurrent.memorySize + mSelfShadowHistory.memorySize;
	mStatus.filterBytes = mFiltered.memorySize;
	mStatus.totalBytes = mStatus.fieldBytes + mStatus.workBytes + mStatus.linkBytes + mStatus.proposalBytes +
		mStatus.scatterBytes + mStatus.filterBytes + mStatus.selfShadowFieldBytes;
	mStatus.multipleScatterAllocated = scatterAllocated;
	mStatus.selfShadowAllocated = selfShadowRequested && mSelfShadowCurrent.buffer != nullptr && mSelfShadowHistory.buffer != nullptr;
	mStatus.scatterProbeCapacity = scatterAllocated ? mResourceScatterProbeCapacity : 0u;
	mStatus.resourcesReady = true;
	mStatus.failureReason = "none";
	mNeedsClear = true;
	return true;
}

void NRISmokeGridLighting::ConsumeReadback(const NRISmokeGridServices& services, uint32_t simulationEpoch)
{
	if (mFrameSlots.empty() || services.core == nullptr)
		return;
	FrameSlot& slot = mFrameSlots[std::min(services.queuedFrameIndex, (uint32_t)mFrameSlots.size() - 1u)];
	if (!slot.readbackPending || slot.controlReadback.buffer == nullptr)
		return;
	mStatus.gpuStatsValid = false;
	const void* mapped = services.core->MapBuffer(*slot.controlReadback.buffer, 0, sizeof(NRISmokeGridLightControlGpu));
	if (mapped != nullptr)
	{
		NRISmokeGridLightControlGpu gpu = {};
		std::memcpy(&gpu, mapped, sizeof(gpu));
		services.core->UnmapBuffer(*slot.controlReadback.buffer);
		if (slot.readbackEpoch == simulationEpoch && gpu.simulationEpoch == simulationEpoch)
		{
			mStatus.gpu = gpu;
			mStatus.gpuRendererFrame = slot.readbackRendererFrame;
			mStatus.gpuStatsValid = true;
			mStatus.controlReadbackBytes += sizeof(gpu);
		}
		else
		{
			mStatus.gpuStatsValid = false;
		}
	}
	slot.readbackPending = false;
}

bool NRISmokeGridLighting::RecordControlReadback(const NRISmokeGridServices& services, const NRISmokeSettings& settings)
{
	if (!settings.readback || mFrameSlots.empty())
		return true;
	FrameSlot& slot = mFrameSlots[std::min(services.queuedFrameIndex, (uint32_t)mFrameSlots.size() - 1u)];
	if (mControl.buffer == nullptr || slot.controlReadback.buffer == nullptr)
		return false;

	nri::BufferBarrierDesc before[2] = {};
	before[0].buffer = mControl.buffer;
	before[0].before = StorageAccess();
	before[0].after = NRIResourceCopySourceAccess();
	before[1].buffer = slot.controlReadback.buffer;
	before[1].before = slot.readbackInitialized ? NRIResourceCopyDestinationAccess() : nri::AccessStage{};
	before[1].after = NRIResourceCopyDestinationAccess();
	nri::BarrierDesc beforeCopy = {};
	beforeCopy.buffers = before;
	beforeCopy.bufferNum = 2u;
	services.core->CmdBarrier(*services.commandBuffer, beforeCopy);
	services.core->CmdCopyBuffer(*services.commandBuffer, *slot.controlReadback.buffer, 0,
		*mControl.buffer, 0, sizeof(NRISmokeGridLightControlGpu));

	nri::BufferBarrierDesc restore = {};
	restore.buffer = mControl.buffer;
	restore.before = NRIResourceCopySourceAccess();
	restore.after = StorageAccess();
	nri::BarrierDesc afterCopy = {};
	afterCopy.buffers = &restore;
	afterCopy.bufferNum = 1u;
	services.core->CmdBarrier(*services.commandBuffer, afterCopy);
	slot.readbackPending = true;
	slot.readbackInitialized = true;
	slot.readbackRendererFrame = services.rendererFrame;
	slot.readbackEpoch = mSimulationEpoch;
	return true;
}

bool NRISmokeGridLighting::PrepareFrame(const NRISmokeGridServices& services, const NRISmokeSettings& settings,
	const NRISmokeWorkTable& workTable, uint32_t cellCapacity, uint32_t frameIndex, uint32_t simulationEpoch)
{
	ConsumeReadback(services, simulationEpoch);
	if (!settings.readback)
		mStatus.gpuStatsValid = false;
	const uint32_t requestedPointCandidates = std::clamp(settings.emissivePointCandidates, 1u, 8u);
	const uint32_t effectivePointCandidates = settings.emissiveReference ? 1u : requestedPointCandidates;
	const uint32_t candidateTargetCode = settings.emissiveCandidateTarget >= 0 ?
		(uint32_t)settings.emissiveCandidateTarget + 1u : 0u;
	const uint32_t estimatorKey = effectivePointCandidates | (settings.emissiveReference ? 0x100u : 0u) |
		(candidateTargetCode << 16u);
	mStatus.requested = settings.emissiveBackend != (uint32_t)NRISmokeEmissiveBackend::Legacy ||
		settings.multipleScatter || settings.selfShadow;
	mStatus.requestedBackend = settings.emissiveBackend;
	mStatus.filterRequested = settings.emissiveWorldFilter;
	mStatus.filterDecision = settings.emissiveWorldFilter ? "requested" : "disabled/variance-gate-not-accepted";
	mStatus.proposalDecision = settings.emissiveCandidateTarget >= 0 ? "exact-candidate-diagnostic" :
		(settings.emissiveLocalProposals ? "brick-top16/uniform75+global25" : "global-cdf/manual-disable");
	mStatus.emissivePointCandidatesRequested = requestedPointCandidates;
	mStatus.emissivePointCandidatesEffective = effectivePointCandidates;
	mStatus.emissiveCandidateTarget = settings.emissiveCandidateTarget;
	mStatus.radiancePartitionCount = settings.worldRadiancePartitions;
	mStatus.radianceWorkLimited = NRISmokeWorkScheduler::Enforces(workTable,
		NRISmokeWorkCapability_RadianceNewInvalid) && NRISmokeWorkScheduler::Enforces(workTable,
		NRISmokeWorkCapability_RadianceMaintenance);
	mStatus.radianceNewInvalidQuantity = mStatus.radianceWorkLimited ?
		workTable.radianceNewInvalidCells : settings.worldRadianceNewCells;
	mStatus.radianceMaintenanceQuantity = mStatus.radianceWorkLimited ?
		workTable.radianceMaintenanceCells : settings.worldRadianceMaintenanceCells;
	mStatus.radianceMaximumAge = settings.worldRadianceMaximumAge;
	mStatus.multipleScatterRequested = settings.multipleScatter;
	mStatus.scatterDecision = !settings.multipleScatter ? "disabled" : "gpu-probe4x4x4/boundary-aware";
	mStatus.selfShadowRequested = settings.selfShadow;
	mStatus.selfShadowDecision = settings.selfShadow ? "gpu-w8/experimental" : "disabled";
	if (!mStatus.initialized)
	{
		mStatus.failureReason = "pipelines-unavailable";
		return false;
	}
	if (!mStatus.requested)
	{
		mStatus.effectiveBackend = (uint32_t)NRISmokeEmissiveBackend::Legacy;
		mStatus.authority = "legacy";
		return true;
	}
	if (simulationEpoch != mSimulationEpoch)
		Reset(simulationEpoch, "simulation-epoch");
	if (!EnsureResources(services, cellCapacity, settings.emissiveWorldFilter, settings.multipleScatter, settings.selfShadow))
		return false;
	if (mLastEmissiveEstimatorKey != 0u && mLastEmissiveEstimatorKey != estimatorKey)
	{
		// The point estimator changed, so discard only derived world-lighting
		// history. Density, temperature, and velocity remain authoritative.
		mFieldPing = 0u;
		mNeedsClear = true;
	}
	mLastEmissiveEstimatorKey = estimatorKey;
	mStatus.multipleScatterEffective = settings.multipleScatter && mStatus.multipleScatterAllocated;
	if (settings.multipleScatter && !mStatus.multipleScatterAllocated)
		mStatus.scatterDecision = "allocation-failed/direct-only";
	mStatus.selfShadowEffective = settings.selfShadow && mStatus.selfShadowAllocated;
	if (settings.selfShadow && !mStatus.selfShadowAllocated)
		mStatus.selfShadowDecision = "allocation-failed/unshadowed";
	mStatus.effectiveBackend = settings.emissiveBackend == (uint32_t)NRISmokeEmissiveBackend::Legacy ?
		(uint32_t)NRISmokeEmissiveBackend::Legacy :
		(settings.emissiveBackend == (uint32_t)NRISmokeEmissiveBackend::Compare ?
			(uint32_t)NRISmokeEmissiveBackend::Compare : (uint32_t)NRISmokeEmissiveBackend::World);
	mStatus.authority = mStatus.effectiveBackend == (uint32_t)NRISmokeEmissiveBackend::Compare ? "compare" :
		(mStatus.effectiveBackend == (uint32_t)NRISmokeEmissiveBackend::Legacy ? "legacy+scatter" : "world");
	mStatus.simulationEpoch = simulationEpoch;
	mStatus.lastUpdatedFrame = frameIndex;
	return true;
}

void NRISmokeGridLighting::Barrier(const NRISmokeGridServices& services)
{
	NRIBufferResource* resources[] = { &mCurrent, &mHistory, &mActive, &mControl, &mLinks, &mProposals, &mFiltered,
		&mScatterSeed, &mScatterBounceA, &mScatterBounceB, &mScatterMetadata, &mScatterActive,
		&mSelfShadowCurrent, &mSelfShadowHistory, &mSupportStamps };
	nri::BufferBarrierDesc barriers[15] = {};
	uint32_t resourceCount = 0u;
	for (NRIBufferResource* resource : resources)
	{
		if (resource->buffer == nullptr)
			continue;
		barriers[resourceCount].buffer = resource->buffer;
		barriers[resourceCount].before = StorageAccess();
		barriers[resourceCount].after = StorageAccess();
		resourceCount++;
	}
	nri::BarrierDesc desc = {};
	desc.buffers = barriers;
	desc.bufferNum = resourceCount;
	services.core->CmdBarrier(*services.commandBuffer, desc);
}

void NRISmokeGridLighting::Dispatch(const NRISmokeGridServices& services, NRISmokeGridLightingPass pass,
	NRISmokeConstants& constants, uint32_t groups, uint32_t iteration)
{
	const uint32_t index = (uint32_t)pass;
	services.core->CmdBeginAnnotation(*services.commandBuffer, kPipelineNames[index], nri::BGRA_UNUSED);
	constants.pass = index | (iteration << 16u);
	services.core->CmdSetRootConstants(*services.commandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	services.core->CmdSetPipeline(*services.commandBuffer, *mPipelines[index]);
	services.core->CmdDispatch(*services.commandBuffer, { groups, 1u, 1u });
	services.core->CmdEndAnnotation(*services.commandBuffer);
}

bool NRISmokeGridLighting::Record(const NRISmokeGridServices& services, const NRISmokeSettings& settings,
	const NRISmokeWorkTable& workTable, NRISmokeConstants constants, bool emissiveResourcesReady)
{
	if (!mStatus.initialized || !mStatus.resourcesReady || !services.IsRecordingValid())
		return false;
	if (mLastRecordedFrame == constants.frameIndex)
		return true;
	NRIPopulateSmokeVisualPhaseConstants(settings.visuals, constants);
	if (!mResourcesInitialized)
	{
		NRIBufferResource* resources[] = { &mCurrent, &mHistory, &mActive, &mControl, &mLinks, &mProposals, &mFiltered,
			&mScatterSeed, &mScatterBounceA, &mScatterBounceB, &mScatterMetadata, &mScatterActive,
			&mSelfShadowCurrent, &mSelfShadowHistory, &mSupportStamps };
		nri::BufferBarrierDesc barriers[15] = {};
		uint32_t resourceCount = 0u;
		for (NRIBufferResource* resource : resources)
		{
			if (resource->buffer == nullptr)
				continue;
			barriers[resourceCount].buffer = resource->buffer;
			barriers[resourceCount].after = StorageAccess();
			resourceCount++;
		}
		nri::BarrierDesc desc = {};
		desc.buffers = barriers;
		desc.bufferNum = resourceCount;
		services.core->CmdBarrier(*services.commandBuffer, desc);
		mResourcesInitialized = true;
	}
	if (mNeedsClear)
		constants.flags |= 1u;
	if (mFieldPing != 0u)
		constants.flags |= 0x4000000u;
	// These root lanes are unused by world-lighting passes. Keep the diagnostic
	// work table local to this focused owner instead of widening the shared ABI.
	constants.particleCapacity = settings.worldRadiancePartitions;
	const bool radianceWorkLimited = NRISmokeWorkScheduler::Enforces(workTable,
		NRISmokeWorkCapability_RadianceNewInvalid) && NRISmokeWorkScheduler::Enforces(workTable,
		NRISmokeWorkCapability_RadianceMaintenance);
	constants.styleCount = radianceWorkLimited ? workTable.radianceNewInvalidCells : settings.worldRadianceNewCells;
	constants.froxelWidth = radianceWorkLimited ? workTable.radianceMaintenanceCells : settings.worldRadianceMaintenanceCells;
	constants.froxelHeight = settings.worldRadianceMaximumAge;
	if (radianceWorkLimited)
		constants.flags |= 0x40000000u;
	{
		NRIScopedGpuTiming timing(services.gpuTimingDevice, NRIGpuTimingScope::SmokeWorldActive);
		Dispatch(services, NRISmokeGridLightingPass::Prepare, constants, 1u);
		Barrier(services);
		Dispatch(services, NRISmokeGridLightingPass::ClearSupport, constants, Groups(mResourceCellCapacity));
		Barrier(services);
		Dispatch(services, NRISmokeGridLightingPass::BuildActive, constants, Groups(mResourceCellCapacity));
		Barrier(services);
	}
	{
		NRIScopedGpuTiming timing(services.gpuTimingDevice, NRIGpuTimingScope::SmokeWorldLink);
		Dispatch(services, NRISmokeGridLightingPass::BuildLinks, constants, Groups(mResourceCellCapacity));
		Barrier(services);
	}
	const bool directEnabled = settings.emissiveBackend != (uint32_t)NRISmokeEmissiveBackend::Legacy && emissiveResourcesReady;
	if (directEnabled && settings.emissiveLocalProposals)
	{
		NRIScopedGpuTiming timing(services.gpuTimingDevice, NRIGpuTimingScope::SmokeWorldProposal);
		Dispatch(services, NRISmokeGridLightingPass::BuildProposals, constants, Groups(mResourceBrickCapacity));
		Barrier(services);
	}
	if (directEnabled)
	{
		{
			NRIScopedGpuTiming timing(services.gpuTimingDevice, NRIGpuTimingScope::SmokeWorldSeed);
			Dispatch(services, NRISmokeGridLightingPass::Seed, constants, Groups(mResourceCellCapacity));
			Barrier(services);
		}
		if (settings.emissiveReuseMode >= 1u)
		{
			NRIScopedGpuTiming timing(services.gpuTimingDevice, NRIGpuTimingScope::SmokeWorldTemporal);
			Dispatch(services, NRISmokeGridLightingPass::Temporal, constants, Groups(mResourceCellCapacity));
			Barrier(services);
		}
	}
	if (directEnabled && settings.emissiveWorldFilter && mFiltered.buffer != nullptr)
	{
		NRIScopedGpuTiming timing(services.gpuTimingDevice, NRIGpuTimingScope::SmokeWorldFilter);
		Dispatch(services, NRISmokeGridLightingPass::Filter, constants, Groups(mResourceCellCapacity));
		Barrier(services);
	}
	if (settings.multipleScatter && mStatus.multipleScatterAllocated)
	{
		NRIScopedGpuTiming timing(services.gpuTimingDevice, NRIGpuTimingScope::SmokeWorldScatter);
		const uint32_t qualityIterations = 1u << std::min(settings.quality, 2u);
		const uint32_t iterations = settings.multipleScatterIterations != 0u ? settings.multipleScatterIterations : qualityIterations;
		Dispatch(services, NRISmokeGridLightingPass::SeedScattering, constants, Groups(mResourceScatterProbeCapacity));
		Barrier(services);
		for (uint32_t iteration = 0u; iteration < iterations; ++iteration)
		{
			Dispatch(services, NRISmokeGridLightingPass::PropagateScattering, constants,
				Groups(mResourceScatterProbeCapacity), iteration);
			Barrier(services);
		}
		mStatus.scatterIterations = iterations;
		mStatus.scatterFinalPing = iterations & 1u;
	}
	else
	{
		mStatus.scatterIterations = 0u;
		mStatus.scatterFinalPing = 0u;
	}
	mNeedsClear = false;
	mLastRecordedFrame = constants.frameIndex;
	mStatus.lastUpdatedFrame = constants.frameIndex;
	mStatus.fieldPing = mFieldPing;
	if (directEnabled)
		mFieldPing = 1u - mFieldPing;
	if (!RecordControlReadback(services, settings))
		mStatus.gpuStatsValid = false;
	return true;
}

bool NRISmokeGridLighting::GetStorageDescriptors(std::array<const nri::Descriptor*, StorageDescriptorCount>& descriptors) const
{
	if (!mStatus.resourcesReady)
		return false;
	descriptors = { mCurrent.storageView, mHistory.storageView, mActive.storageView, mControl.storageView,
		mLinks.storageView, mFiltered.storageView != nullptr ? mFiltered.storageView : mCurrent.storageView,
		mProposals.storageView,
		mScatterSeed.storageView != nullptr ? mScatterSeed.storageView : mLinks.storageView,
		mScatterBounceA.storageView != nullptr ? mScatterBounceA.storageView : mLinks.storageView,
		mScatterBounceB.storageView != nullptr ? mScatterBounceB.storageView : mLinks.storageView,
		mScatterMetadata.storageView != nullptr ? mScatterMetadata.storageView : mLinks.storageView,
		mScatterActive.storageView != nullptr ? mScatterActive.storageView : mActive.storageView,
		mSelfShadowCurrent.storageView != nullptr ? mSelfShadowCurrent.storageView : mCurrent.storageView,
		mSelfShadowHistory.storageView != nullptr ? mSelfShadowHistory.storageView : mHistory.storageView,
		mSupportStamps.storageView };
	return true;
}

void NRISmokeGridLighting::PublishGridSnapshot(
	const std::array<const nri::Descriptor*, NRISmokeGrid::EvaluationDescriptorCount>& descriptors,
	uint32_t fieldPing, float cellSize)
{
	mGridDescriptors = descriptors;
	mGridFieldPing = fieldPing;
	mGridCellSize = cellSize;
}

NRISmokeGridLightingDirectSeedSnapshot NRISmokeGridLighting::GetDirectSeedSnapshot() const
{
	NRISmokeGridLightingDirectSeedSnapshot snapshot = {};
	if (!mStatus.resourcesReady)
		return snapshot;
	snapshot.current = mCurrent.storageView;
	snapshot.history = mHistory.storageView;
	snapshot.activeCells = mActive.storageView;
	snapshot.links = mLinks.storageView;
	snapshot.control = mControl.storageView;
	snapshot.filtered = mFiltered.storageView;
	snapshot.gridControl = mGridDescriptors[0];
	snapshot.gridHash = mGridDescriptors[1];
	snapshot.gridBricks = mGridDescriptors[2];
	snapshot.scalarA = mGridDescriptors[3];
	snapshot.scalarB = mGridDescriptors[4];
	snapshot.opticalA = mGridDescriptors[7];
	snapshot.opticalB = mGridDescriptors[8];
	snapshot.cellCapacity = mResourceCellCapacity;
	snapshot.fieldPing = mGridFieldPing;
	snapshot.simulationEpoch = mSimulationEpoch;
	snapshot.cellSize = mGridCellSize;
	return snapshot;
}

void NRISmokeGridLighting::Reset(uint32_t simulationEpoch, const char* reason)
{
	(void)reason;
	mSimulationEpoch = simulationEpoch;
	mFieldPing = 0u;
	mLastRecordedFrame = UINT32_MAX;
	mLastEmissiveEstimatorKey = 0u;
	mNeedsClear = true;
	mStatus.simulationEpoch = simulationEpoch;
	mStatus.gpuStatsValid = false;
	mStatus.gpuRendererFrame = UINT64_MAX;
	mStatus.gpu = {};
	mGridDescriptors.fill(nullptr);
	mGridFieldPing = 0u;
	mGridCellSize = 0.0f;
}

void NRISmokeGridLighting::Shutdown(const NRISmokeGridServices& services)
{
	services.WaitForCommands("smoke-grid-lighting-shutdown");
	DestroyResources(services);
	for (nri::Pipeline*& pipeline : mPipelines)
	{
		if (pipeline != nullptr) services.core->DestroyPipeline(pipeline);
		pipeline = nullptr;
	}
	mSharedLayout = nullptr;
	mStatus = {};
}
