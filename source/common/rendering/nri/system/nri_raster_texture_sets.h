#pragma once

#include "nri_local.h"

// Every set here uses set 1 of the device's raster pipeline layout. NRI pools
// allocate sets linearly, so retain and rebind those no longer visible to the GPU.
class NRIRasterTextureSets
{
public:
	nri::Result Acquire(const nri::CoreInterface& core, nri::DescriptorPool& pool, nri::PipelineLayout& layout,
		nri::Descriptor& shaderView, nri::DescriptorSet*& out)
	{
		nri::DescriptorSet* set = nullptr;
		if (mAvailable.empty())
		{
			const auto result = core.AllocateDescriptorSets(pool, layout, 1, &set, 1, 0);
			if (result != nri::Result::SUCCESS)
				return result;
			++mAllocated;
		}
		else
		{
			set = mAvailable.back();
			mAvailable.pop_back();
			++mReuses;
		}
		const nri::Descriptor* view = &shaderView;
		nri::UpdateDescriptorRangeDesc update = {};
		update.descriptorSet = set;
		update.descriptors = &view;
		update.descriptorNum = 1;
		core.UpdateDescriptorRanges(&update, 1);
		out = set;
		return nri::Result::SUCCESS;
	}

	// Called at resource destruction, after the owning texture's retirement fence.
	void Recycle(NRITextureResource& resource)
	{
		if (resource.textureSet != nullptr)
		{
			mAvailable.push_back(resource.textureSet);
			resource.textureSet = nullptr;
		}
	}

	uint32_t Allocated() const { return mAllocated; }
	uint32_t Available() const { return (uint32_t)mAvailable.size(); }
	uint64_t Reuses() const { return mReuses; }

private:
	std::vector<nri::DescriptorSet*> mAvailable;
	uint32_t mAllocated = 0;
	uint64_t mReuses = 0;
};
