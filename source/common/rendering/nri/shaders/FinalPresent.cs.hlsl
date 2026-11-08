#include "NRI.hlsl"
#include "Include/PresentConstants.hlsli"
#include "Include/DisplayMapping.hlsli"

NRI_ROOT_CONSTANTS(NRIPresentConstants, gPresentConstants, 0, 2);

Texture2D<float4> gInputTexture : register(t0, space0);
Texture2D<float4> gExposureStateTexture : register(t1, space0);
Texture2D<float4> gTranslucentEffects : register(t2, space0);
Texture2D<float4> gEffectSmoke : register(t3, space0);

#include "Include/TranslucentEffectsPresent.hlsli"

NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float3>, gOutputTexture, u, 0, 1);

float ResolvePresentExposure()
{
	const uint outputFlags = gPresentConstants.OutputFlags;
	const bool shouldUseAutoExposure =
		(outputFlags & NRI_PRESENT_OUTPUT_FLAG_AUTO_EXPOSURE) != 0u &&
		(outputFlags & NRI_PRESENT_OUTPUT_FLAG_EXPOSURE_TEXTURE_VALID) != 0u &&
		(outputFlags & NRI_PRESENT_OUTPUT_FLAG_INPUT_PRE_EXPOSED) == 0u;
	if (!shouldUseAutoExposure)
	{
		return gPresentConstants.Exposure;
	}

	const float exposure = gExposureStateTexture.Load(int3(0, 0, 0)).x;
	return exposure > 0.0 && !isnan(exposure) && !isinf(exposure) ? exposure : gPresentConstants.Exposure;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint2 targetSize;
	gOutputTexture.GetDimensions(targetSize.x, targetSize.y);
	const uint packedSceneOrigin = gPresentConstants.PackedSceneOrigin;
	const int2 sceneOrigin = int2((int)(packedSceneOrigin << 16) >> 16, (int)packedSceneOrigin >> 16);
	if (dispatchThreadId.x >= targetSize.x || dispatchThreadId.y >= targetSize.y)
	{
		return;
	}

	const uint2 targetPixelPos = dispatchThreadId.xy;
	const uint2 outputSize = uint2(max(gPresentConstants.DisplayWidth, 1u), max(gPresentConstants.DisplayHeight, 1u));
	const int2 pixelPos = int2(targetPixelPos) - sceneOrigin;
	if (pixelPos.x < 0 || pixelPos.y < 0 || pixelPos.x >= (int)outputSize.x || pixelPos.y >= (int)outputSize.y)
	{
		gOutputTexture[targetPixelPos] = 0.0;
		return;
	}

	const uint2 inputSize = uint2(max(gPresentConstants.InputWidth, 1u), max(gPresentConstants.InputHeight, 1u));
	const uint2 samplePos = min((uint2(pixelPos) * inputSize) / outputSize, inputSize - 1u);
	const float3 inputColor = gInputTexture.Load(int3(samplePos, 0)).rgb;
	const bool hdrSwapChainActive =
		gPresentConstants.OutputMode != NRI_PT_OUTPUT_MODE_SDR &&
		(gPresentConstants.OutputFlags & NRI_PRESENT_OUTPUT_FLAG_HDR_SWAPCHAIN_ACTIVE) != 0u;
	const float presentExposure = ResolvePresentExposure();
	if (gPresentConstants.DebugMode == 45u)
	{
		float3 debugColor = SanitizeFiniteColor(inputColor);
		debugColor = ApplyManualExposure(debugColor, presentExposure);

		gOutputTexture[targetPixelPos] = hdrSwapChainActive ?
			ApplyHdrOutputMapping(
				debugColor,
				gPresentConstants.TonemapMode,
				gPresentConstants.Saturation,
				gPresentConstants.Shoulder,
				gPresentConstants.Toe,
				gPresentConstants.PaperWhiteNits,
				gPresentConstants.DisplaySdrLuminance,
				gPresentConstants.DisplayMaxLuminance) :
			ApplyDebugRadianceDisplayMapping(debugColor, targetPixelPos, gPresentConstants.FrameIndex);
		return;
	}

	float3 displayColor = ApplyPresentDisplayMapping(
		inputColor,
		targetPixelPos,
		gPresentConstants.FrameIndex,
		gPresentConstants.OutputMode,
		gPresentConstants.TonemapMode,
		gPresentConstants.OutputFlags,
		presentExposure,
		gPresentConstants.Contrast,
		gPresentConstants.Saturation,
		gPresentConstants.Shoulder,
		gPresentConstants.Toe,
		gPresentConstants.NightVisionPackedModeTint,
		gPresentConstants.NightVisionStrength,
		gPresentConstants.NightVisionExposure,
		gPresentConstants.NightVisionPackedControls,
		gPresentConstants.PaperWhiteNits,
		gPresentConstants.DisplaySdrLuminance,
		gPresentConstants.DisplayMaxLuminance);
	if (gPresentConstants.DebugMode == 0u)
	{
		displayColor = BlendTranslucentEffectLayer(displayColor,
			SampleTranslucentEffectLayer(uint2(pixelPos), outputSize), hdrSwapChainActive);
	}
	gOutputTexture[targetPixelPos] = displayColor;
}
