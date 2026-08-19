#pragma once

#include "../system/nri_local.h"

class NRIRenderDevice;

// Owns the immutable ranked Sobol tables used by directional-shadow sampling.
// Their lifetime and descriptor contract stay independent of map content.
class NRIBlueNoiseResources
{
public:
	bool Initialize(NRIRenderDevice& device);
	void Destroy(NRIRenderDevice& device);

	bool IsReady() const;
	nri::Descriptor* GetScramblingRankingDescriptor() const;
	nri::Descriptor* GetSobolDescriptor() const;

private:
	NRITextureResource mScramblingRanking;
	NRITextureResource mSobol;
};
