#ifndef RAZE_NRI_PT_PRESENT_CONSTANTS_HLSLI
#define RAZE_NRI_PT_PRESENT_CONSTANTS_HLSLI

#define NRI_PT_OUTPUT_MODE_SDR 0u
#define NRI_PT_OUTPUT_MODE_HDR 1u
#define NRI_PT_OUTPUT_MODE_HDR_LINEAR16 2u
#define NRI_PT_OUTPUT_MODE_HDR10_PQ 3u

#define NRI_PT_TONEMAP_HABLE 0u
#define NRI_PT_TONEMAP_ACES_FITTED 1u
#define NRI_PT_TONEMAP_REINHARD 2u

#define NRI_PT_NIGHT_VISION_MODE_NONE 0u
#define NRI_PT_NIGHT_VISION_MODE_DUKE 1u

#define NRI_PRESENT_FLAG_ADD_SECONDARY 0x10u
#define NRI_PRESENT_FLAG_SPLIT_SHADOW_DENOISER 0x20u
#define NRI_PRESENT_OUTPUT_FLAG_DISPLAY_INFO_AVAILABLE 0x1u
#define NRI_PRESENT_OUTPUT_FLAG_DISPLAY_HDR_SUPPORTED 0x2u
#define NRI_PRESENT_OUTPUT_FLAG_HDR_SWAPCHAIN_ACTIVE 0x4u
#define NRI_PRESENT_OUTPUT_FLAG_OFFSCREEN_HDR_TARGET 0x8u
#define NRI_PRESENT_OUTPUT_FLAG_AUTO_EXPOSURE 0x10u
#define NRI_PRESENT_OUTPUT_FLAG_EXPOSURE_TEXTURE_VALID 0x20u
#define NRI_PRESENT_OUTPUT_FLAG_INPUT_PRE_EXPOSED 0x40u
#define NRI_PRESENT_OUTPUT_FLAG_EFFECT_SMOKE 0x80u

struct NRIPresentConstants
{
	uint InputWidth;
	uint InputHeight;
	uint DisplayWidth;
	uint DisplayHeight;
	uint PackedSceneOrigin;
	uint FrameIndex;
	uint DebugMode;
	uint Flags;
	uint DenoiserMode;
	uint OutputMode;
	uint TonemapMode;
	uint OutputFlags;
	float Exposure;
	float Contrast;
	float Saturation;
	float Shoulder;
	float Toe;
	float PaperWhiteNits;
	float DisplayMaxLuminance;
	float DisplaySdrLuminance;
	uint NightVisionPackedModeTint;
	float NightVisionStrength;
	float NightVisionExposure;
	uint NightVisionPackedControls;
	float2 EffectJitter;
};

#endif
