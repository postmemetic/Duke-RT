#include "nri_blue_noise.h"

#include "nri_blue_noise_data.h"
#include "../system/nri_renderdevice.h"

bool NRIBlueNoiseResources::Initialize(NRIRenderDevice& device)
{
	if (IsReady())
	{
		return true;
	}

	Destroy(device);
	const bool scramblingReady =
		device.CreateOwnedTexture(
			mScramblingRanking,
			nri_blue_noise_data::kSpatialDimension,
			nri_blue_noise_data::kSpatialDimension,
			nri::Format::RGBA8_UINT,
			nri::TextureUsageBits::SHADER_RESOURCE) &&
		device.UploadTextureData(
			mScramblingRanking,
			nri_blue_noise_data::kScramblingRanking.data(),
			nri_blue_noise_data::kSpatialDimension,
			nri_blue_noise_data::kSpatialDimension,
			nri_blue_noise_data::kSpatialDimension * sizeof(uint32_t));
	const bool sobolReady =
		scramblingReady &&
		device.CreateOwnedTexture(
			mSobol,
			nri_blue_noise_data::kSobolSampleCount,
			1u,
			nri::Format::RGBA8_UINT,
			nri::TextureUsageBits::SHADER_RESOURCE) &&
		device.UploadTextureData(
			mSobol,
			nri_blue_noise_data::kSobol.data(),
			nri_blue_noise_data::kSobolSampleCount,
			1u,
			nri_blue_noise_data::kSobolSampleCount * sizeof(uint32_t));

	if (!sobolReady)
	{
		Destroy(device);
		return false;
	}
	return true;
}

void NRIBlueNoiseResources::Destroy(NRIRenderDevice& device)
{
	device.DestroyTextureResource(mSobol);
	device.DestroyTextureResource(mScramblingRanking);
}

bool NRIBlueNoiseResources::IsReady() const
{
	return mScramblingRanking.texture != nullptr &&
		mScramblingRanking.shaderView != nullptr &&
		mSobol.texture != nullptr &&
		mSobol.shaderView != nullptr;
}

nri::Descriptor* NRIBlueNoiseResources::GetScramblingRankingDescriptor() const
{
	return mScramblingRanking.shaderView;
}

nri::Descriptor* NRIBlueNoiseResources::GetSobolDescriptor() const
{
	return mSobol.shaderView;
}
