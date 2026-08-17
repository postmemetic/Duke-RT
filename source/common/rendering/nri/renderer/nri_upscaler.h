#pragma once

#include "../system/nri_local.h"

class NRIRenderDevice;

enum class NRIMainUpscalerKind : uint32_t
{
	Off = 0,
	DLSR = 2,
	DLRR = 3,
	FSR = 4,
};

enum class NRIPostSharpenKind : uint32_t
{
	Off = 0,
	NIS = 1,
};

void NRISyncLegacyUpscalerConfig(bool logMigration);
const char* NRIGetMainUpscalerName(NRIMainUpscalerKind kind);
const char* NRIGetPostSharpenName(NRIPostSharpenKind kind);
const char* NRIGetRenderResolutionPolicyName(NRIMainUpscalerKind kind);
const char* NRIGetUpscalerModeName(nri::UpscalerMode mode);
nri::UpscalerType NRIToMainUpscalerType(NRIMainUpscalerKind kind);
nri::UpscalerType NRIToPostSharpenType(NRIPostSharpenKind kind);
float NRIGetUpscalerRenderScale(nri::UpscalerMode mode);
uint32_t NRIGetUpscalerJitterPhaseCount(nri::UpscalerMode mode);
nri::UpscalerMode NRIResolveUpscalerModeForMain(NRIMainUpscalerKind kind, nri::UpscalerMode requestedMode);
float NRIResolveRenderScaleForMain(NRIMainUpscalerKind kind, nri::UpscalerMode requestedMode, float manualRenderScale);
bool NRIIsTemporalMain(NRIMainUpscalerKind kind);
bool NRIIsStandardSuperResolutionMain(NRIMainUpscalerKind kind);
bool NRIIsRayReconstructionMain(NRIMainUpscalerKind kind);
bool NRIUsesNriUpscalerProvider(NRIMainUpscalerKind kind);
bool NRIIsAppTaaEligibleUpscaler(NRIMainUpscalerKind kind);
bool NRIShouldRunAppTaa(NRIMainUpscalerKind kind);
bool NRIShouldUseTemporalJitter(NRIMainUpscalerKind kind);
const char* NRIGetTemporalJitterModeName(NRIMainUpscalerKind kind, bool guiCaptureActive);
uint32_t NRIGetTemporalJitterPhaseCount(NRIMainUpscalerKind kind, nri::UpscalerMode mode, bool guiCaptureActive);

struct NRIUpscalerDispatchDesc
{
	nri::CommandBuffer* commandBuffer = nullptr;
	NRITextureResource* input = nullptr;
	NRITextureResource* output = nullptr;
	NRITextureResource* motion = nullptr;
	NRITextureResource* depth = nullptr;
	NRITextureResource* exposure = nullptr;
	NRITextureResource* normalRoughness = nullptr;
	NRITextureResource* diffuseAlbedo = nullptr;
	NRITextureResource* specularAlbedo = nullptr;
	NRITextureResource* specularHitDistance = nullptr;
	NRITextureResource* reactive = nullptr;
	uint32_t currentWidth = 0;
	uint32_t currentHeight = 0;
	float cameraJitter[2] = {};
	float viewToClipMatrix[16] = {};
	float worldToViewMatrix[16] = {};
	float zNear = 0.0f;
	float zFar = 0.0f;
	float verticalFov = 0.0f;
	float frameTimeMs = 0.0f;
	float viewSpaceToMetersFactor = 1.0f;
	float sharpness = 0.2f;
	bool resetHistory = false;
};

class NRIUpscalerContext
{
public:
	bool EnsureMainUpscaler(NRIRenderDevice& frameBuffer, NRIMainUpscalerKind kind, nri::UpscalerMode mode, uint32_t upscaleWidth, uint32_t upscaleHeight, bool useExposure, bool useReactive);
	bool IsMainUpscalerReady(NRIMainUpscalerKind kind) const;
	bool DispatchMainUpscaler(NRIRenderDevice& frameBuffer, NRIMainUpscalerKind kind, const NRIUpscalerDispatchDesc& desc);
	bool EnsurePostSharpen(NRIRenderDevice& frameBuffer, NRIPostSharpenKind kind, uint32_t upscaleWidth, uint32_t upscaleHeight);
	bool DispatchPostSharpen(NRIRenderDevice& frameBuffer, NRIPostSharpenKind kind, const NRIUpscalerDispatchDesc& desc);
	void Shutdown(NRIRenderDevice& frameBuffer);

private:
	struct UpscalerSlotState
	{
		nri::Upscaler* instance = nullptr;
		nri::UpscalerMode mode = nri::UpscalerMode::QUALITY;
		uint32_t upscaleWidth = 0;
		uint32_t upscaleHeight = 0;
		nri::UpscalerBits flags = nri::UpscalerBits::NONE;
		bool creationFailed = false;
	};

	bool EnsureUpscaler(
		NRIRenderDevice& frameBuffer,
		UpscalerSlotState& slot,
		nri::UpscalerType type,
		nri::UpscalerMode mode,
		uint32_t upscaleWidth,
		uint32_t upscaleHeight,
		nri::UpscalerBits flags);
	void DestroyUpscaler(NRIRenderDevice& frameBuffer, nri::Upscaler*& upscaler);

	UpscalerSlotState mNis = {};
	UpscalerSlotState mDlsr = {};
	UpscalerSlotState mDlrr = {};
	UpscalerSlotState mFsr = {};
};
