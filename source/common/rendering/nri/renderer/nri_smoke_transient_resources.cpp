#include "nri_smoke_transient_resources.h"

#include <algorithm>
#include <cstring>

namespace
{
	nri::AccessStage StorageAccess() { return { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER }; }
	nri::AccessStage ReadAccess() { return { nri::AccessBits::SHADER_RESOURCE, nri::StageBits::COMPUTE_SHADER }; }
	nri::AccessStage CopySource() { return { nri::AccessBits::COPY_SOURCE, nri::StageBits::COPY }; }
	nri::AccessStage CopyDestination() { return { nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY }; }
}

bool NRISmokeTransientResources::Create(const NRISmokeGridServices& s, NRIBufferResource& out,
	uint64_t bytes, uint32_t stride, nri::MemoryLocation location, bool storage)
{
	nri::BufferDesc desc = {};
	desc.size = std::max<uint64_t>(bytes, stride);
	desc.structureStride = stride;
	desc.usage = location == nri::MemoryLocation::HOST_UPLOAD ? nri::BufferUsageBits::NONE :
		(storage ? nri::BufferUsageBits::SHADER_RESOURCE_STORAGE : nri::BufferUsageBits::SHADER_RESOURCE);
	if (s.core->CreateCommittedBuffer(*s.device, location, 0.0f, desc, out.buffer) != nri::Result::SUCCESS)
		return false;
	out.size = out.usedSize = desc.size;
	out.stride = stride;
	out.usage = desc.usage;
	out.memoryLocation = location;
	nri::MemoryDesc memory = {};
	s.core->GetBufferMemoryDesc(*out.buffer, location, memory);
	out.memorySize = memory.size;
	if (location == nri::MemoryLocation::HOST_UPLOAD)
		return true;
	nri::BufferViewDesc view = {};
	view.buffer = out.buffer;
	view.type = storage ? nri::BufferView::STORAGE_STRUCTURED_BUFFER : nri::BufferView::STRUCTURED_BUFFER;
	view.size = nri::WHOLE_SIZE;
	view.structureStride = stride;
	if (s.core->CreateBufferView(view, storage ? out.storageView : out.shaderView) == nri::Result::SUCCESS)
		return true;
	Destroy(s, out);
	return false;
}

void NRISmokeTransientResources::Destroy(const NRISmokeGridServices& s, NRIBufferResource& buffer)
{
	if (buffer.shaderView) s.core->DestroyDescriptor(buffer.shaderView);
	if (buffer.storageView) s.core->DestroyDescriptor(buffer.storageView);
	if (buffer.buffer) s.core->DestroyBuffer(buffer.buffer);
	buffer = {};
}

bool NRISmokeTransientResources::Prepare(const NRISmokeGridServices& s, uint32_t width, uint32_t height, uint32_t depth)
{
	if (!s.IsDeviceValid() || !width || !height || !depth || !s.queuedFrameCount)
		return false;
	if (mFrames.size() != s.queuedFrameCount)
	{
		if (!mFrames.empty()) s.WaitForCommands("smoke transient input resize");
		Shutdown(s);
		mFrames.resize(s.queuedFrameCount);
	}
	const uint32_t strides[] = { sizeof(NRISmokeTransientGroupGpu), sizeof(NRISmokeTransientLobeGpu),
		sizeof(NRISmokeTransientPreviousLobeGpu) };
	const uint32_t capacities[] = { 64, 256, 256 };
	for (auto& frame : mFrames)
		for (uint32_t i = 0; i != InputDescriptorCount; ++i)
		{
			if (!frame.upload[i].buffer && !Create(s, frame.upload[i], uint64_t(capacities[i]) * strides[i], strides[i], nri::MemoryLocation::HOST_UPLOAD, false)) return false;
			if (!frame.device[i].buffer && !Create(s, frame.device[i], uint64_t(capacities[i]) * strides[i], strides[i], nri::MemoryLocation::DEVICE, false)) return false;
		}
	const uint64_t froxels = uint64_t(width) * height * depth;
	const uint64_t bins = uint64_t((width + 3) / 4) * ((height + 3) / 4) * ((depth + 3) / 4);
	const uint64_t sizes[] = { bins * 8, bins * 64 * 4, froxels * 16, 64 * 4 * 64, 64 * 4 * 64, 64 * 64, froxels * 16 };
	const uint32_t storageStrides[] = { 8, 4, 16, 64, 64, 64, 16 };
	// Stage replacements before releasing live storage; cache banks survive view resizing.
	std::array<NRIBufferResource, StorageDescriptorCount> replacements = {};
	bool replacingLive = false;
	for (uint32_t i = 0; i != StorageDescriptorCount; ++i)
	{
		if (mStorage[i].buffer && mStorage[i].size == sizes[i]) continue;
		replacingLive |= mStorage[i].buffer != nullptr;
		if (!Create(s, replacements[i], sizes[i], storageStrides[i], nri::MemoryLocation::DEVICE, true))
		{
			for (auto& buffer : replacements) Destroy(s, buffer);
			return false;
		}
	}
	if (replacingLive) s.WaitForCommands("smoke transient view resize");
	for (uint32_t i = 0; i != StorageDescriptorCount; ++i)
	{
		if (!replacements[i].buffer) continue;
		if (i >= 3u && i < 6u && mStorage[i].buffer != nullptr) mCacheRecreated = true;
		Destroy(s, mStorage[i]);
		mStorage[i] = replacements[i];
		mStorageInitialized[i] = false;
		if (i >= 3u && i < 6u) mResetPending = true;
	}
	mFroxelCount = froxels;
	mBinCount = bins;
	return true;
}

bool NRISmokeTransientResources::Upload(const NRISmokeGridServices& s,
	const std::vector<NRISmokeTransientGroupGpu>& groups, const std::vector<NRISmokeTransientLobeGpu>& lobes,
	bool resetMotion)
{
	if (!s.IsRecordingValid() || s.queuedFrameIndex >= mFrames.size() || groups.size() > 64 || lobes.size() > 256) return false;
	auto& frame = mFrames[s.queuedFrameIndex];
	const auto& previousLobes = mMotion.Prepare(s.rendererFrame, lobes, resetMotion);
	if (lobes.empty() && frame.initialized &&
		std::all_of(mStorageInitialized.begin(), mStorageInitialized.end(), [](bool initialized) { return initialized; }))
	{
		mMotion.Commit(); // An empty rendered frame breaks lobe history on reentry.
		return true; // Counts are zero; retained inputs are unreachable until the next upload.
	}
	const void* data[] = { groups.data(), lobes.data(), previousLobes.data() };
	const uint64_t sizes[] = { groups.size() * sizeof(NRISmokeTransientGroupGpu), lobes.size() * sizeof(NRISmokeTransientLobeGpu),
		previousLobes.size() * sizeof(NRISmokeTransientPreviousLobeGpu) };
	for (uint32_t i = 0; i != InputDescriptorCount; ++i)
	{
		if (!sizes[i]) continue;
		void* mapped = s.core->MapBuffer(*frame.upload[i].buffer, 0, sizes[i]);
		if (!mapped) return false;
		std::memcpy(mapped, data[i], size_t(sizes[i]));
		s.core->UnmapBuffer(*frame.upload[i].buffer);
	}
	std::array<nri::BufferBarrierDesc, InputDescriptorCount * 2u> barriers = {};
	for (uint32_t i = 0; i != InputDescriptorCount; ++i)
	{
		barriers[i * 2].buffer = frame.upload[i].buffer;
		barriers[i * 2].after = CopySource();
		barriers[i * 2 + 1].buffer = frame.device[i].buffer;
		barriers[i * 2 + 1].before = frame.initialized ? ReadAccess() : nri::AccessStage{};
		barriers[i * 2 + 1].after = CopyDestination();
	}
	nri::BarrierDesc barrier = {};
	barrier.buffers = barriers.data(); barrier.bufferNum = uint32_t(barriers.size());
	s.core->CmdBarrier(*s.commandBuffer, barrier);
	for (uint32_t i = 0; i != InputDescriptorCount; ++i)
	{
		if (sizes[i]) s.core->CmdCopyBuffer(*s.commandBuffer, *frame.device[i].buffer, 0, *frame.upload[i].buffer, 0, sizes[i]);
		barriers[i].buffer = frame.device[i].buffer;
		barriers[i].before = CopyDestination(); barriers[i].after = ReadAccess();
	}
	barrier.bufferNum = InputDescriptorCount;
	s.core->CmdBarrier(*s.commandBuffer, barrier);
	frame.initialized = true;
	StorageBarrier(s);
	mMotion.Commit();
	return true;
}

std::array<const nri::Descriptor*, NRISmokeTransientResources::InputDescriptorCount> NRISmokeTransientResources::Inputs(uint32_t index) const
{
	if (index >= mFrames.size()) return {};
	return { mFrames[index].device[0].shaderView, mFrames[index].device[1].shaderView, mFrames[index].device[2].shaderView };
}

std::array<const nri::Descriptor*, NRISmokeTransientResources::StorageDescriptorCount> NRISmokeTransientResources::Storage() const
{
	std::array<const nri::Descriptor*, StorageDescriptorCount> descriptors = {};
	for (uint32_t i = 0; i != StorageDescriptorCount; ++i) descriptors[i] = mStorage[i].storageView;
	return descriptors;
}

void NRISmokeTransientResources::StorageBarrier(const NRISmokeGridServices& s)
{
	std::array<nri::BufferBarrierDesc, StorageDescriptorCount> barriers = {};
	for (uint32_t i = 0; i != StorageDescriptorCount; ++i)
	{
		barriers[i].buffer = mStorage[i].buffer;
		barriers[i].before = mStorageInitialized[i] ? StorageAccess() : nri::AccessStage{};
		barriers[i].after = StorageAccess();
		mStorageInitialized[i] = true;
	}
	nri::BarrierDesc barrier = {};
	barrier.buffers = barriers.data(); barrier.bufferNum = uint32_t(barriers.size());
	s.core->CmdBarrier(*s.commandBuffer, barrier);
}

uint64_t NRISmokeTransientResources::ResidentBytes() const
{
	uint64_t bytes = 0;
	for (const auto& buffer : mStorage) bytes += buffer.memorySize;
	for (const auto& frame : mFrames)
	{
		for (const auto& buffer : frame.upload) bytes += buffer.memorySize;
		for (const auto& buffer : frame.device) bytes += buffer.memorySize;
	}
	return bytes;
}

void NRISmokeTransientResources::Shutdown(const NRISmokeGridServices& s)
{
	if (!s.core) return;
	for (uint32_t i = 3u; i != 6u; ++i)
		mCacheRecreated |= mStorage[i].buffer != nullptr;
	for (auto& buffer : mStorage) Destroy(s, buffer);
	for (auto& frame : mFrames)
	{
		for (auto& buffer : frame.upload) Destroy(s, buffer);
		for (auto& buffer : frame.device) Destroy(s, buffer);
	}
	mFrames.clear();
	mMotion.Reset();
	mStorageInitialized = {};
	mFroxelCount = mBinCount = 0;
	mResetPending = true;
}
