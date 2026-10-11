#pragma once

#include "nri_smoke_grid.h"
#include "nri_smoke_transient_clouds.h"
#include "nri_smoke_transient_motion.h"
#include "nri_smoke_descriptor_budget.h"

// Owns transient inputs and view/cache storage without borrowing grid lifetime.
class NRISmokeTransientResources
{
public:
	static constexpr uint32_t StorageDescriptorCount = nri_smoke_descriptors::TransientStorageCount;
	static constexpr uint32_t InputDescriptorCount = 3;
	bool Prepare(const NRISmokeGridServices& services, uint32_t width, uint32_t height, uint32_t depth);
	bool Upload(const NRISmokeGridServices& services,
		const std::vector<NRISmokeTransientGroupGpu>& groups,
		const std::vector<NRISmokeTransientLobeGpu>& lobes, bool resetMotion = false);
	std::array<const nri::Descriptor*, InputDescriptorCount> Inputs(uint32_t queuedFrameIndex) const;
	std::array<const nri::Descriptor*, StorageDescriptorCount> Storage() const;
	void StorageBarrier(const NRISmokeGridServices& services);
	void Shutdown(const NRISmokeGridServices& services);
	void InvalidateCache() { mResetPending = true; mMotion.Reset(); }
	bool ResetPending() const { return mResetPending; }
	// Resource recreation discards cache identity independently of an ordinary
	// GPU clear. The CPU group owner consumes this edge and reschedules full
	// lighting for surviving groups before their replacement snapshot is uploaded.
	bool ConsumeCacheRecreated()
	{
		const bool recreated = mCacheRecreated;
		mCacheRecreated = false;
		return recreated;
	}
	void DidClear() { mResetPending = false; }
	uint64_t ResidentBytes() const;
	uint64_t BinCount() const { return mBinCount; }

private:
	struct FrameInputs
	{
		std::array<NRIBufferResource, InputDescriptorCount> upload;
		std::array<NRIBufferResource, InputDescriptorCount> device;
		bool initialized = false;
	};
	bool Create(const NRISmokeGridServices& services, NRIBufferResource& buffer,
		uint64_t bytes, uint32_t stride, nri::MemoryLocation location, bool storage);
	void Destroy(const NRISmokeGridServices& services, NRIBufferResource& buffer);
	std::vector<FrameInputs> mFrames;
	NRISmokeTransientMotion mMotion;
	std::array<NRIBufferResource, StorageDescriptorCount> mStorage;
	std::array<bool, StorageDescriptorCount> mStorageInitialized = {};
	uint64_t mFroxelCount = 0;
	uint64_t mBinCount = 0;
	bool mResetPending = true;
	bool mCacheRecreated = false;
};
