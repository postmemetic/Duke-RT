#pragma once

#include "nri_resources.h"

#include "NRD.h"
#include "NRDSettings.h"
#include "NRDIntegration.h"

enum class NRINrdDenoiserMode : uint32_t
{
	Reblur = 0,
	Relax = 1,
};

struct NRINrdDispatchDesc
{
	nri::CommandBuffer* commandBuffer = nullptr;
	NRITextureResource* motion = nullptr;
	NRITextureResource* viewZ = nullptr;
	NRITextureResource* normalRoughness = nullptr;
	NRITextureResource* baseColorMetalness = nullptr;
	NRITextureResource* unfilteredDiffuse = nullptr;
	NRITextureResource* unfilteredSpecular = nullptr;
	NRITextureResource* unfilteredPenumbra = nullptr;
	NRITextureResource* diffuse = nullptr;
	NRITextureResource* specular = nullptr;
	NRITextureResource* shadow = nullptr;
	NRITextureResource* validation = nullptr;
	uint32_t resourceWidth = 0;
	uint32_t resourceHeight = 0;
	uint32_t frameIndex = 0;
	uint8_t queuedFrameNum = 1;
	float observedFrameTimeMs = 0.0f;
	float cameraJitter[2] = {};
	float cameraJitterPrev[2] = {};
	float lightDirection[3] = {};
	float viewToClipMatrix[16] = {};
	float viewToClipMatrixPrev[16] = {};
	float worldToViewMatrix[16] = {};
	float worldToViewMatrixPrev[16] = {};
	NRINrdDenoiserMode denoiserMode = NRINrdDenoiserMode::Reblur;
	float fastHistoryClampingSigmaScale = 1.25f;
	float diffusePrepassBlurRadius = 0.0f;
	float specularPrepassBlurRadius = 4.0f;
	float minBlurRadius = 0.0f;
	float maxBlurRadius = 4.0f;
	float sigmaPlaneDistanceSensitivity = 0.01f;
	uint32_t maxAccumulatedFrameNum = 31;
	uint32_t maxFastAccumulatedFrameNum = 7;
	uint32_t maxStabilizedFrameNum = 31;
	uint32_t sigmaMaxStabilizedFrameNum = 2;
	uint32_t hitDistanceReconstructionMode = 0;
	uint32_t indirectSamplingMode = 0;
	bool resetHistory = false;
	bool enableAntiFirefly = true;
	bool enableValidation = false;
	bool enableSigmaShadow = false;
	bool traceTemporalInput = false;
};

class NRINrdContext
{
public:
	bool EnsureReady(nri::Device& device, uint32_t width, uint32_t height, uint8_t queuedFrameNum);
	void NewFrame();
	bool Denoise(const NRINrdDispatchDesc& desc);
	void Shutdown();
	bool IsReady() const { return mInitialized; }
	// The integration sums device memory requirements for its own texture pools.
	uint64_t GetPermanentTexturePoolBytes() const { return uint64_t(mIntegration.GetPersistentMemoryUsageInMb() * 1048576.0); }
	uint64_t GetTransientTexturePoolBytes() const { return uint64_t(mIntegration.GetAliasableMemoryUsageInMb() * 1048576.0); }

private:
	static nrd::Resource MakeResource(NRITextureResource& texture);

	nrd::Integration mIntegration;
	nrd::Identifier mReblurDenoiser = 0;
	nrd::Identifier mRelaxDenoiser = 0;
	nrd::Identifier mSigmaDenoiser = 0;
	nrd::ReblurSettings mReblurSettings = {};
	nrd::RelaxSettings mRelaxSettings = {};
	nrd::SigmaSettings mSigmaSettings = {};
	uint32_t mWidth = 0;
	uint32_t mHeight = 0;
	uint8_t mQueuedFrameNum = 0;
	bool mInitialized = false;
	bool mHasReblurSettings = false;
	bool mHasRelaxSettings = false;
	bool mHasSigmaSettings = false;
	bool mHasIndirectSamplingMode = false;
	NRINrdDenoiserMode mLastDenoiserMode = NRINrdDenoiserMode::Reblur;
	uint32_t mLastIndirectSamplingMode = 0;
};
