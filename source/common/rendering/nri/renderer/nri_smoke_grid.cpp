#include "nri_smoke_grid.h"
#include "nri_smoke_grid_reserve_policy.h"

#include "../system/nri_gpu_timing.h"
#include "printf.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace
{
	constexpr uint32_t kThreadGroupWidth = 64u;
	constexpr uint32_t kDispatchWordCount = 6u;

	nri::AccessStage StorageAccess()
	{
		return { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
	}

	nri::AccessStage ArgumentAccess()
	{
		return { nri::AccessBits::ARGUMENT_BUFFER, nri::StageBits::INDIRECT };
	}

	uint32_t Groups(uint64_t count)
	{
		return (uint32_t)std::max<uint64_t>(1u, (count + kThreadGroupWidth - 1u) / kThreadGroupWidth);
	}

	uint32_t NextPowerOfTwo(uint32_t value)
	{
		value = std::max(value, 2u);
		value--;
		value |= value >> 1u;
		value |= value >> 2u;
		value |= value >> 4u;
		value |= value >> 8u;
		value |= value >> 16u;
		return value + 1u;
	}

	const char* const kPipelineNames[] = {
		"SmokeGridClear",
		"SmokeGridAllocateCommands",
		"SmokeGridBuildDispatch",
		"SmokeGridPrepareBricks",
		"SmokeGridDeposit",
		"SmokeGridResolveDeposit",
		"SmokeGridAllocateHalo",
		"SmokeGridBeginRebuild",
		"SmokeGridAdvectVelocity",
		"SmokeGridAdvectFields",
		"SmokeGridRebuild",
		"SmokeGridValidatePrompt",
		"SmokeGridAuthorizePrompt",
		"SmokeGridFinalizePrompt",
	};

	static_assert(std::size(kPipelineNames) == 14u);

	const char* SmokeSourceClassName(uint32_t sourceClass)
	{
		switch (sourceClass)
		{
		case 0u: return "ambient-map";
		case 1u: return "interactive-actor";
		case 2u: return "interactive-event";
		case 3u: return "diagnostic";
		default: return "unknown";
		}
	}
}

std::array<NRIBufferResource*, NRISmokeGrid::StorageDescriptorCount> NRISmokeGrid::StorageResources()
{
	return { &mControl, &mHash, &mBricks, &mFreeList, &mActiveA, &mActiveB, &mDispatchArgs,
		&mScalarA, &mScalarB, &mVelocityA, &mVelocityB, &mOpticalA, &mOpticalB,
		&mDynamicsA, &mDynamicsB, &mDeposit0, &mDeposit1, &mDeposit2, &mDeposit3,
		&mSourceStats, &mPromptOutcomes, &mPromptLedger, &mVorticity };
}

std::array<const NRIBufferResource*, NRISmokeGrid::StorageDescriptorCount> NRISmokeGrid::StorageResources() const
{
	return { &mControl, &mHash, &mBricks, &mFreeList, &mActiveA, &mActiveB, &mDispatchArgs,
		&mScalarA, &mScalarB, &mVelocityA, &mVelocityB, &mOpticalA, &mOpticalB,
		&mDynamicsA, &mDynamicsB, &mDeposit0, &mDeposit1, &mDeposit2, &mDeposit3,
		&mSourceStats, &mPromptOutcomes, &mPromptLedger, &mVorticity };
}

void NRISmokeGrid::SetFailure(const char* reason)
{
	mStatus.resourcesReady = false;
	mStatus.failureReason = reason != nullptr ? reason : "unspecified";
}

bool NRISmokeGrid::CreateBuffer(const NRISmokeGridServices& services, NRIBufferResource& out,
	uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::MemoryLocation location, bool storageView)
{
	DestroyBuffer(services, out);
	if (!services.IsDeviceValid() || stride == 0u)
		return false;

	nri::BufferDesc desc = {};
	desc.size = std::max<uint64_t>(size, stride);
	desc.structureStride = stride;
	desc.usage = usage;
	if (services.core->CreateCommittedBuffer(*services.device, location, 0.0f, desc, out.buffer) != nri::Result::SUCCESS)
		return false;

	nri::MemoryDesc memory = {};
	services.core->GetBufferMemoryDesc(*out.buffer, location, memory);
	out.size = desc.size;
	out.usedSize = desc.size;
	out.memorySize = memory.size;
	out.stride = stride;
	out.usage = usage;
	out.memoryLocation = location;

	if (storageView)
	{
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
	}
	return true;
}

void NRISmokeGrid::DestroyBuffer(const NRISmokeGridServices& services, NRIBufferResource& resource)
{
	if (services.core != nullptr)
	{
		if (resource.shaderView != nullptr)
			services.core->DestroyDescriptor(resource.shaderView);
		if (resource.storageView != nullptr)
			services.core->DestroyDescriptor(resource.storageView);
		if (resource.buffer != nullptr)
			services.core->DestroyBuffer(resource.buffer);
	}
	resource = {};
}

bool NRISmokeGrid::Initialize(const NRISmokeGridServices& services)
{
	if (mInitialized)
		return true;
	if (!services.IsDeviceValid() || services.loadShaderBlob == nullptr || services.queuedFrameCount == 0u)
	{
		SetFailure("invalid-services");
		return false;
	}

	nri::DescriptorRangeDesc inputRange = {};
	inputRange.baseRegisterIndex = 0;
	inputRange.descriptorNum = 2;
	inputRange.descriptorType = nri::DescriptorType::STRUCTURED_BUFFER;
	inputRange.shaderStages = nri::StageBits::COMPUTE_SHADER;
	inputRange.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

	nri::DescriptorRangeDesc storageRange = {};
	storageRange.baseRegisterIndex = 0;
	storageRange.descriptorNum = StorageDescriptorCount;
	storageRange.descriptorType = nri::DescriptorType::STORAGE_STRUCTURED_BUFFER;
	storageRange.shaderStages = nri::StageBits::COMPUTE_SHADER;
	storageRange.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

	nri::DescriptorSetDesc sets[2] = {};
	sets[0].registerSpace = 0;
	sets[0].ranges = &inputRange;
	sets[0].rangeNum = 1;
	sets[0].flags = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;
	sets[1].registerSpace = 1;
	sets[1].ranges = &storageRange;
	sets[1].rangeNum = 1;
	sets[1].flags = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;

	nri::RootConstantDesc root = {};
	root.registerIndex = 0;
	root.size = sizeof(NRISmokeGridConstants);
	root.shaderStages = nri::StageBits::COMPUTE_SHADER;
	nri::PipelineLayoutDesc layout = {};
	layout.rootRegisterSpace = 2;
	layout.rootConstants = &root;
	layout.rootConstantNum = 1;
	layout.descriptorSets = sets;
	layout.descriptorSetNum = 2;
	layout.shaderStages = nri::StageBits::COMPUTE_SHADER;
	if (services.core->CreatePipelineLayout(*services.device, layout, mPipelineLayout) != nri::Result::SUCCESS)
	{
		SetFailure("pipeline-layout");
		return false;
	}

	const bool d3d12 = services.graphicsAPI == nri::GraphicsAPI::D3D12;
	for (uint32_t i = 0; i < mPipelines.size(); ++i)
	{
		std::vector<uint8_t> blob;
		const std::string fileName = std::string(kPipelineNames[i]) + ".cs." + (d3d12 ? "dxil" : "spirv");
		if (!services.LoadShaderBlob(fileName.c_str(), blob))
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
		nri::ComputePipelineDesc pipeline = {};
		pipeline.pipelineLayout = mPipelineLayout;
		pipeline.shader = shader;
		if (services.core->CreateComputePipeline(*services.device, pipeline, mPipelines[i]) != nri::Result::SUCCESS)
		{
			SetFailure("compute-pipeline");
			Shutdown(services);
			return false;
		}
	}

	if (services.core->AllocateDescriptorSets(*services.descriptorPool, *mPipelineLayout, 1,
		&mStorageSet, 1, 0) != nri::Result::SUCCESS)
	{
		SetFailure("storage-descriptor-set");
		Shutdown(services);
		return false;
	}
	mFrameSlots.resize(services.queuedFrameCount);
	for (FrameSlot& slot : mFrameSlots)
	{
		if (services.core->AllocateDescriptorSets(*services.descriptorPool, *mPipelineLayout, 0,
			&slot.inputSet, 1, 0) != nri::Result::SUCCESS)
		{
			SetFailure("input-descriptor-set");
			Shutdown(services);
			return false;
		}
	}

	mInitialized = true;
	mStatus.initialized = true;
	mStatus.failureReason = "none";
	return true;
}

bool NRISmokeGrid::EnsureResources(const NRISmokeGridServices& services, const NRISmokeSettings& settings)
{
	const uint32_t brickCapacity = std::clamp(settings.gridBrickCapacity, 64u, 4096u);
	const uint32_t hashCapacity = NextPowerOfTwo(brickCapacity * 2u);
	const uint32_t cellCapacity = brickCapacity * NRI_SMOKE_GRID_CELLS_PER_BRICK;
	if (mControl.buffer != nullptr && mResourceBrickCapacity == brickCapacity &&
		mResourceHashCapacity == hashCapacity && mResourceCellCapacity == cellCapacity)
	{
		if (mResourceCellSize != settings.gridCellSize)
		{
			mResourceCellSize = settings.gridCellSize;
			mNeedsClear = true;
			mActivePing = 0;
			mFieldPing = 0;
			mStatus.resetReason = "grid-cell-size";
		}
		return true;
	}

	services.WaitForCommands("smoke-grid-resource-layout");
	DestroyResources(services);
	const nri::BufferUsageBits storage = nri::BufferUsageBits::SHADER_RESOURCE_STORAGE;
	const nri::BufferUsageBits indirectStorage = NRIResourceFlags(storage, nri::BufferUsageBits::ARGUMENT_BUFFER);
	const uint64_t cells = cellCapacity;
	bool created =
		CreateBuffer(services, mControl, sizeof(NRISmokeGridControlGpu), sizeof(NRISmokeGridControlGpu), storage, nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mHash, (uint64_t)hashCapacity * sizeof(NRISmokeGridHashEntryGpu), sizeof(NRISmokeGridHashEntryGpu), storage, nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mBricks, (uint64_t)brickCapacity * sizeof(NRISmokeGridBrickGpu), sizeof(NRISmokeGridBrickGpu), storage, nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mFreeList, (uint64_t)brickCapacity * sizeof(uint32_t), sizeof(uint32_t), storage, nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mActiveA, (uint64_t)brickCapacity * sizeof(uint32_t), sizeof(uint32_t), storage, nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mActiveB, (uint64_t)brickCapacity * sizeof(uint32_t), sizeof(uint32_t), storage, nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mDispatchArgs, kDispatchWordCount * sizeof(uint32_t), sizeof(uint32_t), indirectStorage, nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mScalarA, cells * 16u, 16u, storage, nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mScalarB, cells * 16u, 16u, storage, nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mVelocityA, cells * 16u, 16u, storage, nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mVelocityB, cells * 16u, 16u, storage, nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mOpticalA, cells * 16u, 16u, storage, nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mOpticalB, cells * 16u, 16u, storage, nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mDynamicsA, cells * 16u, 16u, storage, nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mDynamicsB, cells * 16u, 16u, storage, nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mDeposit0, cells * 16u, 16u, storage, nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mDeposit1, cells * 16u, 16u, storage, nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mDeposit2, cells * 16u, 16u, storage, nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mDeposit3, cells * 16u, 16u, storage, nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mSourceStats, (uint64_t)SourceCapacity * sizeof(NRISmokeGridSourceStatsGpu),
			sizeof(NRISmokeGridSourceStatsGpu), storage, nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mPromptOutcomes, NRI_SMOKE_PROMPT_FALLBACK_QUANTITY * sizeof(NRISmokePromptOutcomeGpu),
			sizeof(NRISmokePromptOutcomeGpu), storage, nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mPromptLedger, NRI_SMOKE_PROMPT_LEDGER_CAPACITY * sizeof(NRISmokePromptLedgerGpu),
			sizeof(NRISmokePromptLedgerGpu), storage, nri::MemoryLocation::DEVICE, true) &&
		CreateBuffer(services, mVorticity, cells * 16u, 16u, storage, nri::MemoryLocation::DEVICE, true);

	for (FrameSlot& slot : mFrameSlots)
	{
		created = created && CreateBuffer(services, slot.controlReadback, sizeof(NRISmokeGridControlGpu),
			sizeof(NRISmokeGridControlGpu), nri::BufferUsageBits::NONE, nri::MemoryLocation::HOST_READBACK, false);
		created = created && CreateBuffer(services, slot.sourceReadback,
			(uint64_t)SourceCapacity * sizeof(NRISmokeGridSourceStatsGpu), sizeof(NRISmokeGridSourceStatsGpu),
			nri::BufferUsageBits::NONE, nri::MemoryLocation::HOST_READBACK, false);
		created = created && CreateBuffer(services, slot.promptReadback,
			NRI_SMOKE_PROMPT_FALLBACK_QUANTITY * sizeof(NRISmokePromptOutcomeGpu), sizeof(NRISmokePromptOutcomeGpu),
			nri::BufferUsageBits::NONE, nri::MemoryLocation::HOST_READBACK, false);
		created = created && CreateBuffer(services, slot.spatialBrickReadback,
			(uint64_t)brickCapacity * sizeof(NRISmokeGridBrickGpu), sizeof(NRISmokeGridBrickGpu),
			nri::BufferUsageBits::NONE, nri::MemoryLocation::HOST_READBACK, false);
		slot.readbackPending = false;
		slot.diagnosticReadbackPending = false;
		slot.promptReadbackInitialized = false;
		slot.diagnosticReadbackInitialized = false;
		slot.spatialReadbackPending = false;
		slot.spatialReadbackInitialized = false;
	}
	if (!created)
	{
		DestroyResources(services);
		SetFailure("resource-allocation");
		return false;
	}

	const auto resources = StorageResources();
	std::array<const nri::Descriptor*, StorageDescriptorCount> descriptors = {};
	for (uint32_t i = 0; i < descriptors.size(); ++i)
		descriptors[i] = resources[i]->storageView;
	nri::UpdateDescriptorRangeDesc update = {};
	update.descriptorSet = mStorageSet;
	update.rangeIndex = 0;
	update.descriptors = descriptors.data();
	update.descriptorNum = (uint32_t)descriptors.size();
	services.core->UpdateDescriptorRanges(&update, 1);

	mResourceBrickCapacity = brickCapacity;
	mResourceHashCapacity = hashCapacity;
	mResourceCellCapacity = cellCapacity;
	mResourceCellSize = settings.gridCellSize;
	mResourceEpoch = 0;
	mActivePing = 0;
	mFieldPing = 0;
	mNeedsClear = true;
	mResourcesInitialized = false;
	mDispatchIsArgument = false;
	mStatus.brickCapacity = brickCapacity;
	mStatus.hashCapacity = hashCapacity;
	mStatus.cellCapacity = cellCapacity;
	mStatus.residentBytes = 0;
	for (const NRIBufferResource* resource : StorageResources())
		mStatus.residentBytes += resource->memorySize;
	for (const FrameSlot& slot : mFrameSlots)
		mStatus.residentBytes += slot.controlReadback.memorySize + slot.sourceReadback.memorySize +
			slot.promptReadback.memorySize + slot.spatialBrickReadback.memorySize;
	mStatus.resourcesReady = true;
	mStatus.failureReason = "none";
	return true;
}

void NRISmokeGrid::ConsumeReadback(const NRISmokeGridServices& services, uint32_t simulationEpoch)
{
	if (mFrameSlots.empty() || services.core == nullptr)
		return;
	FrameSlot& slot = mFrameSlots[std::min(services.queuedFrameIndex, (uint32_t)mFrameSlots.size() - 1u)];
	if (slot.readbackPending && slot.promptReadback.buffer != nullptr && slot.readbackEpoch == simulationEpoch)
	{
		const uint64_t promptBytes = NRI_SMOKE_PROMPT_FALLBACK_QUANTITY * sizeof(NRISmokePromptOutcomeGpu);
		const void* promptMapped = services.core->MapBuffer(*slot.promptReadback.buffer, 0, promptBytes);
		if (promptMapped != nullptr)
		{
			const auto* outcomes = static_cast<const NRISmokePromptOutcomeGpu*>(promptMapped);
			for (uint32_t i = 0u; i < NRI_SMOKE_PROMPT_FALLBACK_QUANTITY; ++i)
				if (outcomes[i].outcome != (uint32_t)NRISmokePromptOutcome::None && outcomes[i].rangeCount != 0u)
					mPromptCommits.push_back(outcomes[i]);
			services.core->UnmapBuffer(*slot.promptReadback.buffer);
		}
	}
	slot.readbackPending = false;
	if (slot.spatialReadbackPending && slot.spatialBrickReadback.buffer != nullptr &&
		slot.readbackEpoch == simulationEpoch)
	{
		const uint64_t brickBytes = (uint64_t)mResourceBrickCapacity * sizeof(NRISmokeGridBrickGpu);
		const void* brickMapped = services.core->MapBuffer(*slot.spatialBrickReadback.buffer, 0, brickBytes);
		if (brickMapped != nullptr)
		{
			const auto* bricks = static_cast<const NRISmokeGridBrickGpu*>(brickMapped);
			mStatus.spatialBricks.assign(bricks, bricks + mResourceBrickCapacity);
			mStatus.spatialGpuRendererFrame = slot.readbackRendererFrame;
			services.core->UnmapBuffer(*slot.spatialBrickReadback.buffer);
		}
	}
	slot.spatialReadbackPending = false;
	if (!slot.diagnosticReadbackPending || slot.controlReadback.buffer == nullptr || slot.sourceReadback.buffer == nullptr)
		return;
	mStatus.gpuStatsValid = false;
	mStatus.gpuFrameDeltaValid = false;
	const void* mapped = services.core->MapBuffer(*slot.controlReadback.buffer, 0, sizeof(NRISmokeGridControlGpu));
	if (mapped != nullptr)
	{
		NRISmokeGridControlGpu next = {};
		std::memcpy(&next, mapped, sizeof(next));
		services.core->UnmapBuffer(*slot.controlReadback.buffer);
		if (slot.readbackEpoch == simulationEpoch && next.generation == simulationEpoch)
		{
			const uint64_t sourceBytes = (uint64_t)SourceCapacity * sizeof(NRISmokeGridSourceStatsGpu);
			const void* sourceMapped = services.core->MapBuffer(*slot.sourceReadback.buffer, 0, sourceBytes);
			if (sourceMapped == nullptr)
			{
				slot.diagnosticReadbackPending = false;
				return;
			}
			const auto* rows = static_cast<const NRISmokeGridSourceStatsGpu*>(sourceMapped);
			mStatus.sources.clear();
			for (uint32_t sourceSlot = 0u; sourceSlot < std::min(next.admissionSourceCount, SourceCapacity); ++sourceSlot)
			{
				const auto& row = rows[sourceSlot];
				if (row.sourceId == 0u || row.commands == 0u) continue;
				mStatus.sources.push_back({ row.sourceId, row.sourceClass, row.priority, row.commands,
					row.requestedBricks, row.existingHits, row.admittedNew, row.rejectedCapacity,
					row.rejectedProbe, row.rejectedInvalid, row.footprintCulled, row.depositionCells, row.requestedMassQ,
					row.depositedMassQ, row.rejectedMassQ, row.admittedKeyHash });
			}
			services.core->UnmapBuffer(*slot.sourceReadback.buffer);
			mStatus.sourceReadbackBytes += sourceBytes;
			const NRISmokeGridControlGpu previous = mStatus.gpu;
			const bool consecutive = previous.generation == next.generation && previous.frameStamp + 1u == next.frameStamp;
			mStatus.gpu = next;
			mStatus.gpuRendererFrame = slot.readbackRendererFrame;
			mStatus.gpuStatsValid = true;
			mStatus.gpuFrameDeltaInterval = previous.generation == next.generation ? next.frameStamp - previous.frameStamp : 0u;
			if (consecutive)
			{
				auto& delta = mStatus.gpuFrameDelta;
				delta = {};
				delta.allocated = next.allocated - previous.allocated;
				delta.reclaimed = next.reclaimed - previous.reclaimed;
				delta.allocationFailures = next.allocationFailures - previous.allocationFailures;
				delta.probeFailures = next.probeFailures - previous.probeFailures;
				delta.commandsProcessed = next.commandsProcessed - previous.commandsProcessed;
				delta.requestedMassQ = next.requestedMassQ - previous.requestedMassQ;
				delta.depositedMassQ = next.depositedMassQ - previous.depositedMassQ;
				delta.rejectedMassQ = next.rejectedMassQ - previous.rejectedMassQ;
				delta.saturatedDeposits = next.saturatedDeposits - previous.saturatedDeposits;
				delta.haloAllocations = next.haloAllocations - previous.haloAllocations;
				delta.cflClamps = next.cflClamps - previous.cflClamps;
				delta.backtraceClamps = next.backtraceClamps - previous.backtraceClamps;
				delta.vorticityClamps = next.vorticityClamps - previous.vorticityClamps;
				delta.nanRejects = next.nanRejects - previous.nanRejects;
				delta.depositionCells = next.depositionCells - previous.depositionCells;
				delta.depositionRejected = next.depositionRejected - previous.depositionRejected;
				delta.controlProbeTotal = next.controlProbeTotal - previous.controlProbeTotal;
				delta.controlProbeBin1 = next.controlProbeBin1 - previous.controlProbeBin1;
				delta.controlProbeBin2To4 = next.controlProbeBin2To4 - previous.controlProbeBin2To4;
				delta.controlProbeBin5To8 = next.controlProbeBin5To8 - previous.controlProbeBin5To8;
				delta.controlProbeBin9To16 = next.controlProbeBin9To16 - previous.controlProbeBin9To16;
				delta.controlProbeBin17To24 = next.controlProbeBin17To24 - previous.controlProbeBin17To24;
				delta.lookupProbeTotal = next.lookupProbeTotal - previous.lookupProbeTotal;
				delta.insertionProbeTotal = next.insertionProbeTotal - previous.insertionProbeTotal;
				delta.lookupProbeLimitFailures = next.lookupProbeLimitFailures - previous.lookupProbeLimitFailures;
				delta.insertionProbeLimitFailures = next.insertionProbeLimitFailures - previous.insertionProbeLimitFailures;
				delta.insertionCapacityFailures = next.insertionCapacityFailures - previous.insertionCapacityFailures;
				delta.insertionActiveFailures = next.insertionActiveFailures - previous.insertionActiveFailures;
				delta.reclaimInvalidMappingFailures = next.reclaimInvalidMappingFailures - previous.reclaimInvalidMappingFailures;
				delta.hashRebuildAttempts = next.hashRebuildAttempts - previous.hashRebuildAttempts;
				delta.hashRebuildSuccesses = next.hashRebuildSuccesses - previous.hashRebuildSuccesses;
				delta.hashRebuildFailures = next.hashRebuildFailures - previous.hashRebuildFailures;
				delta.borrowedAllocations = next.borrowedAllocations - previous.borrowedAllocations;
				delta.borrowedReturns = next.borrowedReturns - previous.borrowedReturns;
				delta.borrowedPromotions = next.borrowedPromotions - previous.borrowedPromotions;
				delta.borrowedReclaims = next.borrowedReclaims - previous.borrowedReclaims;
				delta.firstUseReplacementAdmissions = next.firstUseReplacementAdmissions - previous.firstUseReplacementAdmissions;
				delta.firstUseBlockedNoBorrowed = next.firstUseBlockedNoBorrowed - previous.firstUseBlockedNoBorrowed;
				delta.firstUseBlockedVisible = next.firstUseBlockedVisible - previous.firstUseBlockedVisible;
				delta.firstUseBlockedProbe = next.firstUseBlockedProbe - previous.firstUseBlockedProbe;
				delta.firstUseBlockedInvalid = next.firstUseBlockedInvalid - previous.firstUseBlockedInvalid;
				delta.firstUseCapacityFailures = next.firstUseCapacityFailures - previous.firstUseCapacityFailures;
				mStatus.gpuFrameDeltaValid = true;
			}
			mStatus.controlReadbackBytes += sizeof(NRISmokeGridControlGpu);
		}
	}
	slot.diagnosticReadbackPending = false;
}

bool NRISmokeGrid::PrepareFrame(const NRISmokeGridServices& services, const NRISmokeSettings& settings,
	uint32_t frameIndex, uint32_t simulationEpoch)
{
	(void)frameIndex;
	ConsumeReadback(services, simulationEpoch);
	if (!settings.readback)
	{
		mStatus.gpuStatsValid = false;
		mStatus.gpuFrameDeltaValid = false;
	}
	mStatus.requested = settings.enabled && settings.representation != 0u;
	mStatus.representation = settings.representation;
	if (!mStatus.requested)
	{
		mStatus.failureReason = "not-requested";
		return true;
	}
	if (!Initialize(services) || !EnsureResources(services, settings))
		return false;
	if (mResourceEpoch != 0u && mResourceEpoch != simulationEpoch)
		Reset(simulationEpoch, "simulation-epoch");
	mStatus.activePing = mActivePing;
	mStatus.fieldPing = mFieldPing;
	return true;
}

void NRISmokeGrid::TransitionResourcesToStorage(const NRISmokeGridServices& services)
{
	const auto resources = StorageResources();
	std::array<nri::BufferBarrierDesc, StorageDescriptorCount> barriers = {};
	for (uint32_t i = 0; i < barriers.size(); ++i)
	{
		barriers[i].buffer = resources[i]->buffer;
		barriers[i].before = mResourcesInitialized ? StorageAccess() : nri::AccessStage{};
		barriers[i].after = StorageAccess();
	}
	nri::BarrierDesc barrier = {};
	barrier.buffers = barriers.data();
	barrier.bufferNum = (uint32_t)barriers.size();
	services.core->CmdBarrier(*services.commandBuffer, barrier);
	mResourcesInitialized = true;
	mDispatchIsArgument = false;
}

void NRISmokeGrid::StorageBarrier(const NRISmokeGridServices& services)
{
	const auto resources = StorageResources();
	std::array<nri::BufferBarrierDesc, StorageDescriptorCount - 1u> barriers = {};
	uint32_t count = 0;
	for (const NRIBufferResource* resource : resources)
	{
		if (resource == &mDispatchArgs)
			continue;
		barriers[count].buffer = resource->buffer;
		barriers[count].before = StorageAccess();
		barriers[count].after = StorageAccess();
		count++;
	}
	nri::BarrierDesc barrier = {};
	barrier.buffers = barriers.data();
	barrier.bufferNum = count;
	services.core->CmdBarrier(*services.commandBuffer, barrier);
}

void NRISmokeGrid::TransitionDispatchToArgument(const NRISmokeGridServices& services)
{
	if (mDispatchIsArgument)
		return;
	nri::BufferBarrierDesc buffer = {};
	buffer.buffer = mDispatchArgs.buffer;
	buffer.before = StorageAccess();
	buffer.after = ArgumentAccess();
	nri::BarrierDesc barrier = {};
	barrier.buffers = &buffer;
	barrier.bufferNum = 1;
	services.core->CmdBarrier(*services.commandBuffer, barrier);
	mDispatchIsArgument = true;
}

void NRISmokeGrid::TransitionDispatchToStorage(const NRISmokeGridServices& services)
{
	if (!mDispatchIsArgument)
		return;
	nri::BufferBarrierDesc buffer = {};
	buffer.buffer = mDispatchArgs.buffer;
	buffer.before = ArgumentAccess();
	buffer.after = StorageAccess();
	nri::BarrierDesc barrier = {};
	barrier.buffers = &buffer;
	barrier.bufferNum = 1;
	services.core->CmdBarrier(*services.commandBuffer, barrier);
	mDispatchIsArgument = false;
}

void NRISmokeGrid::Dispatch(const NRISmokeGridServices& services, NRISmokeGridConstants& constants,
	NRISmokeGridPass pass, uint32_t x, uint32_t y, uint32_t z)
{
	services.core->CmdBeginAnnotation(*services.commandBuffer, kPipelineNames[(uint32_t)pass], nri::BGRA_UNUSED);
	constants.pass = (uint32_t)pass;
	services.core->CmdSetRootConstants(*services.commandBuffer,
		{ 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	services.core->CmdSetPipeline(*services.commandBuffer, *mPipelines[(uint32_t)pass]);
	services.core->CmdDispatch(*services.commandBuffer, { x, y, z });
	services.core->CmdEndAnnotation(*services.commandBuffer);
}

void NRISmokeGrid::DispatchIndirect(const NRISmokeGridServices& services, NRISmokeGridConstants& constants,
	NRISmokeGridPass pass, uint64_t byteOffset)
{
	TransitionDispatchToArgument(services);
	services.core->CmdBeginAnnotation(*services.commandBuffer, kPipelineNames[(uint32_t)pass], nri::BGRA_UNUSED);
	constants.pass = (uint32_t)pass;
	services.core->CmdSetRootConstants(*services.commandBuffer,
		{ 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	services.core->CmdSetPipeline(*services.commandBuffer, *mPipelines[(uint32_t)pass]);
	services.core->CmdDispatchIndirect(*services.commandBuffer, *mDispatchArgs.buffer, byteOffset);
	services.core->CmdEndAnnotation(*services.commandBuffer);
}

bool NRISmokeGrid::RecordControlReadback(const NRISmokeGridServices& services,
	const NRISmokeSettings& settings, bool spatialObservationReadback)
{
	if (mFrameSlots.empty())
		return true;
	FrameSlot& slot = mFrameSlots[std::min(services.queuedFrameIndex, (uint32_t)mFrameSlots.size() - 1u)];
	if (slot.controlReadback.buffer == nullptr || slot.sourceReadback.buffer == nullptr || slot.promptReadback.buffer == nullptr)
		return false;

	nri::BufferBarrierDesc promptBefore[2] = {};
	promptBefore[0].buffer = mPromptOutcomes.buffer;
	promptBefore[0].before = StorageAccess();
	promptBefore[0].after = NRIResourceCopySourceAccess();
	promptBefore[1].buffer = slot.promptReadback.buffer;
	promptBefore[1].before = slot.promptReadbackInitialized ? NRIResourceCopyDestinationAccess() : nri::AccessStage{};
	promptBefore[1].after = NRIResourceCopyDestinationAccess();
	nri::BarrierDesc promptBeforeCopy = {};
	promptBeforeCopy.buffers = promptBefore;
	promptBeforeCopy.bufferNum = 2;
	services.core->CmdBarrier(*services.commandBuffer, promptBeforeCopy);
	services.core->CmdCopyBuffer(*services.commandBuffer, *slot.promptReadback.buffer, 0,
		*mPromptOutcomes.buffer, 0, NRI_SMOKE_PROMPT_FALLBACK_QUANTITY * sizeof(NRISmokePromptOutcomeGpu));
	nri::BufferBarrierDesc promptRestore = {};
	promptRestore.buffer = mPromptOutcomes.buffer;
	promptRestore.before = NRIResourceCopySourceAccess();
	promptRestore.after = StorageAccess();
	nri::BarrierDesc promptAfterCopy = {};
	promptAfterCopy.buffers = &promptRestore;
	promptAfterCopy.bufferNum = 1;
	services.core->CmdBarrier(*services.commandBuffer, promptAfterCopy);

	if (settings.readback)
	{
		nri::BufferBarrierDesc before[4] = {};
		before[0].buffer = mControl.buffer;
	before[0].before = StorageAccess();
	before[0].after = NRIResourceCopySourceAccess();
	before[1].buffer = mSourceStats.buffer;
	before[1].before = StorageAccess();
	before[1].after = NRIResourceCopySourceAccess();
	before[2].buffer = slot.controlReadback.buffer;
	before[2].before = slot.diagnosticReadbackInitialized ? NRIResourceCopyDestinationAccess() : nri::AccessStage{};
	before[2].after = NRIResourceCopyDestinationAccess();
	before[3].buffer = slot.sourceReadback.buffer;
	before[3].before = slot.diagnosticReadbackInitialized ? NRIResourceCopyDestinationAccess() : nri::AccessStage{};
	before[3].after = NRIResourceCopyDestinationAccess();
		nri::BarrierDesc beforeCopy = {};
		beforeCopy.buffers = before;
		beforeCopy.bufferNum = 4;
		services.core->CmdBarrier(*services.commandBuffer, beforeCopy);
		services.core->CmdCopyBuffer(*services.commandBuffer, *slot.controlReadback.buffer, 0,
		*mControl.buffer, 0, sizeof(NRISmokeGridControlGpu));
		services.core->CmdCopyBuffer(*services.commandBuffer, *slot.sourceReadback.buffer, 0,
		*mSourceStats.buffer, 0, (uint64_t)SourceCapacity * sizeof(NRISmokeGridSourceStatsGpu));

		nri::BufferBarrierDesc restore[2] = {};
	restore[0].buffer = mControl.buffer;
	restore[0].before = NRIResourceCopySourceAccess();
	restore[0].after = StorageAccess();
	restore[1].buffer = mSourceStats.buffer;
	restore[1].before = NRIResourceCopySourceAccess();
	restore[1].after = StorageAccess();
		nri::BarrierDesc afterCopy = {};
		afterCopy.buffers = restore;
		afterCopy.bufferNum = 2;
		services.core->CmdBarrier(*services.commandBuffer, afterCopy);
		slot.diagnosticReadbackPending = true;
		slot.diagnosticReadbackInitialized = true;
	}
	if (spatialObservationReadback)
	{
		nri::BufferBarrierDesc before[2] = {};
		before[0].buffer = mBricks.buffer;
		before[0].before = StorageAccess();
		before[0].after = NRIResourceCopySourceAccess();
		before[1].buffer = slot.spatialBrickReadback.buffer;
		before[1].before = slot.spatialReadbackInitialized ?
			NRIResourceCopyDestinationAccess() : nri::AccessStage{};
		before[1].after = NRIResourceCopyDestinationAccess();
		nri::BarrierDesc beforeCopy = {};
		beforeCopy.buffers = before;
		beforeCopy.bufferNum = 2u;
		services.core->CmdBarrier(*services.commandBuffer, beforeCopy);
		services.core->CmdCopyBuffer(*services.commandBuffer, *slot.spatialBrickReadback.buffer, 0,
			*mBricks.buffer, 0, (uint64_t)mResourceBrickCapacity * sizeof(NRISmokeGridBrickGpu));
		nri::BufferBarrierDesc restore = {};
		restore.buffer = mBricks.buffer;
		restore.before = NRIResourceCopySourceAccess();
		restore.after = StorageAccess();
		nri::BarrierDesc afterCopy = {};
		afterCopy.buffers = &restore;
		afterCopy.bufferNum = 1u;
		services.core->CmdBarrier(*services.commandBuffer, afterCopy);
		slot.spatialReadbackPending = true;
		slot.spatialReadbackInitialized = true;
	}
	slot.readbackPending = true;
	slot.promptReadbackInitialized = true;
	slot.readbackRendererFrame = services.rendererFrame;
	slot.readbackEpoch = mResourceEpoch;
	return true;
}

bool NRISmokeGrid::RecordFrame(const NRISmokeGridServices& services, const NRISmokeSettings& settings,
	const NRISmokeGridFrameDesc& frame)
{
	if (!mStatus.requested)
		return true;
	if (!services.IsRecordingValid() || !mInitialized || !mStatus.resourcesReady ||
		mFrameSlots.empty() || frame.styleView == nullptr || frame.commandView == nullptr)
	{
		SetFailure("record-prerequisite");
		return false;
	}

	FrameSlot& slot = mFrameSlots[std::min(services.queuedFrameIndex, (uint32_t)mFrameSlots.size() - 1u)];
	const nri::Descriptor* inputs[] = { frame.styleView, frame.commandView };
	nri::UpdateDescriptorRangeDesc inputUpdate = {};
	inputUpdate.descriptorSet = slot.inputSet;
	inputUpdate.rangeIndex = 0;
	inputUpdate.descriptors = inputs;
	inputUpdate.descriptorNum = 2;
	services.core->UpdateDescriptorRanges(&inputUpdate, 1);

	TransitionResourcesToStorage(services);
	services.core->CmdSetPipelineLayout(*services.commandBuffer, nri::BindPoint::COMPUTE, *mPipelineLayout);
	services.core->CmdSetDescriptorSet(*services.commandBuffer, { 0, slot.inputSet, nri::BindPoint::COMPUTE });
	services.core->CmdSetDescriptorSet(*services.commandBuffer, { 1, mStorageSet, nri::BindPoint::COMPUTE });

	NRISmokeGridConstants constants = {};
	constants.frameIndex = frame.frameIndex;
	constants.simulationEpoch = frame.simulationEpoch;
	constants.commandCount = frame.commandCount;
	constants.styleCount = frame.styleCount;
	constants.brickCapacity = mResourceBrickCapacity;
	constants.hashCapacity = mResourceHashCapacity;
	constants.cellCapacity = mResourceCellCapacity;
	constants.activePing = mActivePing;
	constants.fieldPing = mFieldPing;
	constants.representation = settings.representation;
	constants.cellSize = settings.gridCellSize;
	constants.deltaTime = std::max(frame.simulationStep, 0.0f);
	constants.timeScale = settings.timeScale;
	// A backtrace outside the source brick cannot be sampled coherently until
	// a wider halo contract exists. Keep the initial solver inside one brick.
	constants.maxBacktrace = std::min(settings.gridMaxBacktrace,
		settings.gridCellSize * (float)NRI_SMOKE_GRID_BRICK_AXIS);
	std::copy(settings.wind, settings.wind + 3, constants.wind);
	constants.buoyancy = settings.gridBuoyancy;
	constants.velocityDamping = settings.gridVelocityDamping;
	constants.windCoupling = settings.gridWindCoupling;
	constants.densityHalfLifeScale = settings.gridDensityHalfLifeScale;
	constants.coolingScale = settings.gridCoolingScale;
	constants.maxVelocity = settings.gridMaxVelocity;
	constants.activeThreshold = settings.gridActiveThreshold;
	constants.reclaimGrace = settings.gridReclaimGrace;
	constants.massQuantization = 4096.0f;
	constants.momentumQuantization = 256.0f;
	constants.curlEvolution = settings.gridCurlEvolution;
	constants.vorticityConfinement = settings.gridVorticity;

	if (mNeedsClear || mResourceEpoch != frame.simulationEpoch)
	{
		NRIScopedGpuTiming timing(services.gpuTimingDevice, NRIGpuTimingScope::SmokeGridInitialize);
		mActivePing = 0;
		mFieldPing = 0;
		mSimulationSeconds = 0.0;
		constants.activePing = 0;
		constants.fieldPing = 0;
		Dispatch(services, constants, NRISmokeGridPass::Clear,
			Groups(std::max({ mResourceHashCapacity, mResourceBrickCapacity, SourceCapacity })));
		StorageBarrier(services);
		mNeedsClear = false;
		mResourceEpoch = frame.simulationEpoch;
	}

	{
		NRIScopedGpuTiming timing(services.gpuTimingDevice, NRIGpuTimingScope::SmokeGridAllocate);
		// This serial control-plane pass also clears frame-local source rows.
		Dispatch(services, constants, NRISmokeGridPass::AllocateCommands, 1u);
		StorageBarrier(services);
	}
	{
		NRIScopedGpuTiming timing(services.gpuTimingDevice, NRIGpuTimingScope::SmokeGridInitialize);
		Dispatch(services, constants, NRISmokeGridPass::BuildDispatch, 1u);
		StorageBarrier(services);
		DispatchIndirect(services, constants, NRISmokeGridPass::PrepareBricks);
		StorageBarrier(services);
	}
	if (frame.commandCount > 0u)
	{
		NRIScopedGpuTiming timing(services.gpuTimingDevice, NRIGpuTimingScope::SmokeGridDeposit);
		Dispatch(services, constants, NRISmokeGridPass::ValidatePrompt, frame.commandCount);
		StorageBarrier(services);
		Dispatch(services, constants, NRISmokeGridPass::AuthorizePrompt, 1u);
		StorageBarrier(services);
		Dispatch(services, constants, NRISmokeGridPass::Deposit, frame.commandCount);
		StorageBarrier(services);
		Dispatch(services, constants, NRISmokeGridPass::FinalizePrompt, 1u);
		StorageBarrier(services);
		DispatchIndirect(services, constants, NRISmokeGridPass::ResolveDeposit);
		StorageBarrier(services);
	}

	for (uint32_t step = 0; step < frame.simulationSubsteps; ++step)
	{
		constants.curlTime = (float)mSimulationSeconds;
		{
			NRIScopedGpuTiming timing(services.gpuTimingDevice, NRIGpuTimingScope::SmokeGridHalo);
			// Allocation is deliberately one serial GPU control-plane dispatch. It
			// snapshots and walks the current active list without CPU involvement.
			Dispatch(services, constants, NRISmokeGridPass::AllocateHalo, 1u);
			StorageBarrier(services);
			TransitionDispatchToStorage(services);
			Dispatch(services, constants, NRISmokeGridPass::BuildDispatch, 1u);
			StorageBarrier(services);
			DispatchIndirect(services, constants, NRISmokeGridPass::PrepareBricks);
			StorageBarrier(services);
		}
		{
			NRIScopedGpuTiming timing(services.gpuTimingDevice, NRIGpuTimingScope::SmokeGridSimulate);
			Dispatch(services, constants, NRISmokeGridPass::BeginRebuild, 1u);
			StorageBarrier(services);
			DispatchIndirect(services, constants, NRISmokeGridPass::AdvectVelocity);
			StorageBarrier(services);
			DispatchIndirect(services, constants, NRISmokeGridPass::AdvectFields);
			StorageBarrier(services);
		}
		{
			NRIScopedGpuTiming timing(services.gpuTimingDevice, NRIGpuTimingScope::SmokeGridRebuild);
			DispatchIndirect(services, constants, NRISmokeGridPass::Rebuild);
			StorageBarrier(services);
		}

		mActivePing ^= 1u;
		mFieldPing ^= 1u;
		constants.activePing = mActivePing;
		constants.fieldPing = mFieldPing;
		mSimulationSeconds += (double)constants.deltaTime * (double)constants.timeScale;
	}

	TransitionDispatchToStorage(services);
	if (frame.simulationSubsteps > 0u || frame.hashHealthDiagnostic)
	{
		NRIScopedGpuTiming timing(services.gpuTimingDevice, NRIGpuTimingScope::SmokeGridRebuild);
		// Publish final ping/count after simulation. This is the only dispatch
		// authorized to reset a fully drained hash; earlier dispatches can contain
		// NEW mappings that have not reached preparation yet. Exact gauges remain
		// diagnostic-only outside that zero-resident cleanup case.
		constants.flags = frame.hashHealthDiagnostic ? NRI_SMOKE_GRID_FLAG_HASH_HEALTH : 0u;
		if (frame.simulationSubsteps > 0u)
			constants.flags |= NRI_SMOKE_GRID_FLAG_COMPACT_DRAINED_HASH;
		Dispatch(services, constants, NRISmokeGridPass::BuildDispatch, 1u);
		StorageBarrier(services);
	}
	const bool controlReadbackReady = RecordControlReadback(services, settings,
		frame.spatialObservationReadback);
	if (!controlReadbackReady)
	{
		// Readback is diagnostic-only and occurs after the authoritative field
		// has already been updated. Never misreport a completed deposition as a
		// simulation failure or replay its one-shot commands.
		mStatus.gpuStatsValid = false;
		mStatus.failureReason = "control-readback-unavailable";
	}
	mStatus.activePing = mActivePing;
	mStatus.fieldPing = mFieldPing;
	mStatus.resourcesReady = true;
	mStatus.failureReason = controlReadbackReady ? "none" : "control-readback-unavailable";
	return true;
}

bool NRISmokeGrid::GetEvaluationStorageDescriptors(
	std::array<const nri::Descriptor*, EvaluationDescriptorCount>& descriptors) const
{
	descriptors = { mControl.storageView, mHash.storageView, mBricks.storageView,
		mScalarA.storageView, mScalarB.storageView, mVelocityA.storageView, mVelocityB.storageView,
		mOpticalA.storageView, mOpticalB.storageView, mDynamicsA.storageView, mDynamicsB.storageView };
	return mStatus.resourcesReady && std::all_of(descriptors.begin(), descriptors.end(),
		[](const nri::Descriptor* descriptor) { return descriptor != nullptr; });
}

bool NRISmokeGrid::GetDormantTransactionStorageDescriptors(
	std::array<const nri::Descriptor*, DormantTransactionDescriptorCount>& descriptors) const
{
	descriptors = { mControl.storageView, mHash.storageView, mBricks.storageView,
		mFreeList.storageView, mActiveA.storageView, mActiveB.storageView,
		mScalarA.storageView, mScalarB.storageView,
		mVelocityA.storageView, mVelocityB.storageView,
		mOpticalA.storageView, mOpticalB.storageView,
		mDynamicsA.storageView, mDynamicsB.storageView,
		mDeposit0.storageView, mDeposit1.storageView,
		mDeposit2.storageView, mDeposit3.storageView };
	return mStatus.resourcesReady && std::all_of(descriptors.begin(), descriptors.end(),
		[](const nri::Descriptor* descriptor) { return descriptor != nullptr; });
}

bool NRISmokeGrid::GetDormantTransactionStorageBuffers(
	std::array<nri::Buffer*, DormantTransactionDescriptorCount>& buffers) const
{
	buffers = { mControl.buffer, mHash.buffer, mBricks.buffer,
		mFreeList.buffer, mActiveA.buffer, mActiveB.buffer,
		mScalarA.buffer, mScalarB.buffer,
		mVelocityA.buffer, mVelocityB.buffer,
		mOpticalA.buffer, mOpticalB.buffer,
		mDynamicsA.buffer, mDynamicsB.buffer,
		mDeposit0.buffer, mDeposit1.buffer,
		mDeposit2.buffer, mDeposit3.buffer };
	return mStatus.resourcesReady && std::all_of(buffers.begin(), buffers.end(),
		[](const nri::Buffer* buffer) { return buffer != nullptr; });
}

void NRISmokeGrid::Reset(uint32_t simulationEpoch, const char* reason)
{
	mResourceEpoch = simulationEpoch;
	mActivePing = 0;
	mFieldPing = 0;
	mSimulationSeconds = 0.0;
	mNeedsClear = true;
	mStatus.activePing = 0;
	mStatus.fieldPing = 0;
	mStatus.gpuStatsValid = false;
	mStatus.gpuRendererFrame = UINT64_MAX;
	mStatus.gpu = {};
	mStatus.sources.clear();
	mStatus.spatialBricks.clear();
	mStatus.spatialGpuRendererFrame = UINT64_MAX;
	mPromptCommits.clear();
	mStatus.resetReason = reason != nullptr ? reason : "unspecified";
}

void NRISmokeGrid::DestroyResources(const NRISmokeGridServices& services)
{
	for (NRIBufferResource* resource : StorageResources())
		DestroyBuffer(services, *resource);
	for (FrameSlot& slot : mFrameSlots)
	{
		DestroyBuffer(services, slot.controlReadback);
		DestroyBuffer(services, slot.sourceReadback);
		DestroyBuffer(services, slot.promptReadback);
		DestroyBuffer(services, slot.spatialBrickReadback);
		slot.readbackPending = false;
		slot.diagnosticReadbackPending = false;
		slot.promptReadbackInitialized = false;
		slot.diagnosticReadbackInitialized = false;
		slot.spatialReadbackPending = false;
		slot.spatialReadbackInitialized = false;
		slot.readbackRendererFrame = UINT64_MAX;
		slot.readbackEpoch = 0;
	}
	mResourceBrickCapacity = 0;
	mResourceHashCapacity = 0;
	mResourceCellCapacity = 0;
	mResourceCellSize = 0.0f;
	mResourceEpoch = 0;
	mResourcesInitialized = false;
	mDispatchIsArgument = false;
	mStatus.resourcesReady = false;
	mStatus.residentBytes = 0;
	mStatus.sources.clear();
	mStatus.spatialBricks.clear();
	mStatus.spatialGpuRendererFrame = UINT64_MAX;
	mPromptCommits.clear();
}

std::vector<NRISmokePromptOutcomeGpu> NRISmokeGrid::ConsumePromptOutcomes()
{
	std::vector<NRISmokePromptOutcomeGpu> committed;
	committed.swap(mPromptCommits);
	return committed;
}

void NRISmokeGrid::Shutdown(const NRISmokeGridServices& services)
{
	if (mInitialized || mControl.buffer != nullptr)
		services.WaitForCommands("smoke-grid-shutdown");
	DestroyResources(services);
	if (services.core != nullptr)
	{
		for (nri::Pipeline*& pipeline : mPipelines)
		{
			if (pipeline != nullptr)
				services.core->DestroyPipeline(pipeline);
			pipeline = nullptr;
		}
		if (mPipelineLayout != nullptr)
			services.core->DestroyPipelineLayout(mPipelineLayout);
	}
	mPipelineLayout = nullptr;
	mStorageSet = nullptr;
	mFrameSlots.clear();
	mInitialized = false;
	mNeedsClear = true;
	mStatus.initialized = false;
}

void NRISmokeGrid::PrintStatus() const
{
	Printf("NRI PT smoke grid status: requested=%s representation=%u initialized=%s resources=%s "
		"bricks=%u hash=%u cells=%u active_ping=%u field_ping=%u gpu_stats=%s "
		"resident=%u resident_bytes=%llu active=%u/%u free=%u allocated=%u reclaimed=%u allocation_failures=%u "
		"probe_failures=%u max_probe=%u commands=%u deposition_cells=%u deposition_rejected=%u requested_mass_q=%u deposited_mass_q=%u "
		"rejected_mass_q=%u saturated=%u halo=%u occupied=%u empty=%u cfl_clamps=%u "
		"backtrace_clamps=%u vorticity_clamps=%u nan=%u field_hash=%08x%08x resident_mib=%.2f "
		"admission_sources=%u admission_requested=%u admission_existing=%u admission_admitted=%u "
		"admission_rejected=%u admission_capacity_rejected=%u admission_probe_rejected=%u admission_invalid_rejected=%u "
		"admission_footprint_culled=%u "
		"hash_empty=%u hash_claimed=%u hash_resident=%u hash_new=%u hash_tombstone=%u hash_invalid_state=%u hash_invalid_mapping=%u "
		"control_probe_total=%u control_probe_max=%u control_probe_bins=%u/%u/%u/%u/%u lookup_probe_total=%u insertion_probe_total=%u "
		"lookup_probe_limit_failures=%u insertion_probe_limit_failures=%u insertion_capacity_failures=%u insertion_active_failures=%u reclaim_invalid_mapping_failures=%u "
		"hash_rebuild_attempts=%u hash_rebuild_successes=%u hash_rebuild_failures=%u "
		"first_use_core=%u first_use_core_expected=%u borrowed_resident=%u borrowed_allocations=%u borrowed_returns=%u "
		"borrowed_promotions=%u borrowed_reclaims=%u first_use_replacement_admissions=%u "
		"first_use_blocked_no_borrowed=%u first_use_blocked_visible=%u first_use_blocked_probe=%u first_use_blocked_invalid=%u first_use_capacity_failures=%u "
		"field_readback=0 control_readback=%llu source_readback=%llu fallback=%s reset=%s\n",
		mStatus.requested ? "yes" : "no", mStatus.representation,
		mStatus.initialized ? "yes" : "no", mStatus.resourcesReady ? "ready" : "unavailable",
		mStatus.brickCapacity, mStatus.hashCapacity, mStatus.cellCapacity,
		mStatus.activePing, mStatus.fieldPing, mStatus.gpuStatsValid ? "valid" : "disabled",
		mStatus.gpu.residentCount, (unsigned long long)mStatus.residentBytes,
		mStatus.gpu.activeCountA, mStatus.gpu.activeCountB,
		mStatus.gpu.freeCount, mStatus.gpu.allocated, mStatus.gpu.reclaimed,
		mStatus.gpu.allocationFailures, mStatus.gpu.probeFailures, mStatus.gpu.maximumProbe,
		mStatus.gpu.commandsProcessed, mStatus.gpu.depositionCells, mStatus.gpu.depositionRejected,
		mStatus.gpu.requestedMassQ, mStatus.gpu.depositedMassQ,
		mStatus.gpu.rejectedMassQ, mStatus.gpu.saturatedDeposits, mStatus.gpu.haloAllocations,
		mStatus.gpu.occupiedBricks, mStatus.gpu.emptyBricks, mStatus.gpu.cflClamps,
		mStatus.gpu.backtraceClamps, mStatus.gpu.vorticityClamps, mStatus.gpu.nanRejects,
		mStatus.gpu.fieldHashHi, mStatus.gpu.fieldHashLo,
		(double)mStatus.residentBytes / (1024.0 * 1024.0),
		mStatus.gpu.admissionSourceCount, mStatus.gpu.admissionRequested,
		mStatus.gpu.admissionExisting, mStatus.gpu.admissionAdmitted,
		mStatus.gpu.admissionRejected, mStatus.gpu.admissionCapacityRejected,
		mStatus.gpu.admissionProbeRejected, mStatus.gpu.admissionInvalidRejected,
		mStatus.gpu.admissionFootprintCulled,
		mStatus.gpu.hashEmpty, mStatus.gpu.hashClaimed, mStatus.gpu.hashResident,
		mStatus.gpu.hashNew, mStatus.gpu.hashTombstone, mStatus.gpu.hashInvalidState,
		mStatus.gpu.hashInvalidMapping, mStatus.gpu.controlProbeTotal, mStatus.gpu.maximumProbe,
		mStatus.gpu.controlProbeBin1, mStatus.gpu.controlProbeBin2To4,
		mStatus.gpu.controlProbeBin5To8, mStatus.gpu.controlProbeBin9To16,
		mStatus.gpu.controlProbeBin17To24, mStatus.gpu.lookupProbeTotal,
		mStatus.gpu.insertionProbeTotal, mStatus.gpu.lookupProbeLimitFailures,
		mStatus.gpu.insertionProbeLimitFailures, mStatus.gpu.insertionCapacityFailures,
		mStatus.gpu.insertionActiveFailures, mStatus.gpu.reclaimInvalidMappingFailures,
		mStatus.gpu.hashRebuildAttempts, mStatus.gpu.hashRebuildSuccesses,
		mStatus.gpu.hashRebuildFailures,
		mStatus.gpu.firstUseCoreCapacity, NRISmokeGridFirstUseCoreCapacity(mStatus.brickCapacity),
		mStatus.gpu.borrowedResident, mStatus.gpu.borrowedAllocations,
		mStatus.gpu.borrowedReturns, mStatus.gpu.borrowedPromotions,
		mStatus.gpu.borrowedReclaims, mStatus.gpu.firstUseReplacementAdmissions,
		mStatus.gpu.firstUseBlockedNoBorrowed, mStatus.gpu.firstUseBlockedVisible,
		mStatus.gpu.firstUseBlockedProbe, mStatus.gpu.firstUseBlockedInvalid,
		mStatus.gpu.firstUseCapacityFailures,
		(unsigned long long)mStatus.controlReadbackBytes,
		(unsigned long long)mStatus.sourceReadbackBytes,
		mStatus.failureReason.c_str(), mStatus.resetReason.c_str());
	for (const NRISmokeGridSourceStatusSnapshot& source : mStatus.sources)
	{
		Printf("NRI PT smoke grid source: source_id=%08x source_class=%u class=%s priority=%u commands=%u requested_bricks=%u existing_hits=%u admitted_new=%u rejected_capacity=%u rejected_probe=%u rejected_invalid=%u footprint_culled=%u deposition_cells=%u requested_mass_q=%u deposited_mass_q=%u rejected_mass_q=%u admitted_key_hash=%08x frame=%u epoch=%u\n",
			source.sourceId, source.sourceClass, SmokeSourceClassName(source.sourceClass), source.priority,
			source.commands, source.requestedBricks, source.existingHits, source.admittedNew,
			source.rejectedCapacity, source.rejectedProbe, source.rejectedInvalid, source.footprintCulled,
			source.depositionCells, source.requestedMassQ, source.depositedMassQ,
			source.rejectedMassQ, source.admittedKeyHash, mStatus.gpu.frameStamp, mStatus.gpu.generation);
	}
}
