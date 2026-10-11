#ifndef RAZE_NRI_PT_DISPLAY_MAPPING_HLSLI
#define RAZE_NRI_PT_DISPLAY_MAPPING_HLSLI

#include "PresentConstants.hlsli"
#include "HdrOutputMath.hlsli"

float SanitizeFiniteChannel(float value)
{
	if (isnan(value) || isinf(value))
	{
		return 0.0;
	}

	return max(value, 0.0);
}

float3 SanitizeFiniteColor(float3 color)
{
	return float3(
		SanitizeFiniteChannel(color.x),
		SanitizeFiniteChannel(color.y),
		SanitizeFiniteChannel(color.z));
}

float3 ApplyManualExposure(float3 color, float exposure)
{
	return color * max(exposure, 0.0);
}

float3 ApplyPresentSceneContrast(float3 color, float contrast)
{
	const float safeContrast = max(contrast, 0.0);
	const float pivot = 0.18;
	return max((color - pivot.xxx) * safeContrast + pivot.xxx, 0.0);
}

uint GetNightVisionMode(uint packedModeTint)
{
	return packedModeTint & 0xffu;
}

float3 GetNightVisionTint(uint packedModeTint)
{
	const float unpackScale = 2.0 / 255.0;
	return float3(
		(float)((packedModeTint >> 8) & 0xffu) * unpackScale,
		(float)((packedModeTint >> 16) & 0xffu) * unpackScale,
		(float)((packedModeTint >> 24) & 0xffu) * unpackScale);
}

float3 ApplyDukeNightVisionViewOperator(float3 color, float strength, float3 tint)
{
	const float safeStrength = saturate(strength);
	if (safeStrength <= 0.0)
	{
		return max(color, 0.0);
	}

	const float3 sanitized = max(color, 0.0);
	const float luma = max(dot(sanitized, float3(0.2126, 0.7152, 0.0722)), 0.0);
	const float liftedLuma = max(luma, sqrt(luma) * 0.55);
	const float3 detail = luma > 1e-4 ? lerp(1.0.xxx, saturate(sanitized / luma), 0.20) : 1.0.xxx;
	const float3 tinted = liftedLuma * max(tint, 0.0.xxx) * detail;
	return lerp(sanitized, tinted, safeStrength);
}

float3 ApplyNightVisionViewOperator(float3 color, uint packedModeTint, float strength)
{
	const uint mode = GetNightVisionMode(packedModeTint);
	if (mode == NRI_PT_NIGHT_VISION_MODE_DUKE)
	{
		return ApplyDukeNightVisionViewOperator(color, strength, GetNightVisionTint(packedModeTint));
	}

	return max(color, 0.0);
}

float UnpackNightVisionPackedControl(uint packedControls, uint shift)
{
	const uint bits = (packedControls >> shift) & 0xffffu;
	return (float)bits * (2.0 / 65535.0);
}

void ApplyNightVisionDisplayControlMultipliers(
	uint packedModeTint,
	float strength,
	float nightVisionExposure,
	uint nightVisionPackedControls,
	inout float exposure,
	inout float contrast,
	inout float saturation)
{
	const float safeStrength = saturate(strength);
	if (GetNightVisionMode(packedModeTint) == NRI_PT_NIGHT_VISION_MODE_NONE || safeStrength <= 0.0)
	{
		return;
	}

	const float nightVisionContrast = UnpackNightVisionPackedControl(nightVisionPackedControls, 0u);
	const float nightVisionSaturation = UnpackNightVisionPackedControl(nightVisionPackedControls, 16u);
	exposure *= lerp(1.0, max(nightVisionExposure, 0.0), safeStrength);
	contrast *= lerp(1.0, max(nightVisionContrast, 0.0), safeStrength);
	saturation *= lerp(1.0, max(nightVisionSaturation, 0.0), safeStrength);
}

float ApplyDisplayToe(float value, float toe)
{
	const float safeValue = saturate(value);
	const float safeToe = max(toe, 1e-3);
	const float curved = pow(safeValue, safeToe);
	const float weight = saturate(1.0 - safeValue * 2.0);
	return lerp(safeValue, curved, weight);
}

float ApplyDisplayShoulder(float value, float shoulder)
{
	const float safeValue = saturate(value);
	const float safeShoulder = max(shoulder, 1e-3);
	const float curved = 1.0 - pow(1.0 - safeValue, safeShoulder);
	const float weight = saturate(safeValue * 2.0 - 1.0);
	return lerp(safeValue, curved, weight);
}

float3 ApplyDisplayToeAndShoulder(float3 color, float toe, float shoulder)
{
	return float3(
		ApplyDisplayShoulder(ApplyDisplayToe(color.x, toe), shoulder),
		ApplyDisplayShoulder(ApplyDisplayToe(color.y, toe), shoulder),
		ApplyDisplayShoulder(ApplyDisplayToe(color.z, toe), shoulder));
}

float3 ApplyDisplaySaturation(float3 color, float saturation)
{
	const float safeSaturation = max(saturation, 0.0);
	const float luma = dot(color, float3(0.2126, 0.7152, 0.0722));
	return max(lerp(luma.xxx, color, safeSaturation), 0.0);
}

float3 ApplyDisplayCalibration(float3 color, float saturation, float toe, float shoulder)
{
	const float3 shaped = ApplyDisplayToeAndShoulder(saturate(color), toe, shoulder);
	return saturate(ApplyDisplaySaturation(shaped, saturation));
}

float3 TonemapHable(float3 color)
{
	const float A = 0.15;
	const float B = 0.50;
	const float C = 0.10;
	const float D = 0.20;
	const float E = 0.02;
	const float F = 0.30;
	const float whitePoint = 11.2;
	const float3 numerator = ((color * (A * color + C * B) + D * E) / (color * (A * color + B) + D * F)) - E / F;
	const float white = (((whitePoint * (A * whitePoint + C * B) + D * E) / (whitePoint * (A * whitePoint + B) + D * F)) - E / F);
	return numerator / max(white, 1e-5);
}

float3 TonemapAcesFitted(float3 color)
{
	return saturate((color * (2.51 * color + 0.03)) / (color * (2.43 * color + 0.59) + 0.14));
}

float3 TonemapReinhard(float3 color)
{
	return color / (1.0 + color);
}

float3 ApplySdrTonemap(float3 color, uint tonemapMode)
{
	if (tonemapMode == NRI_PT_TONEMAP_ACES_FITTED)
	{
		return TonemapAcesFitted(color);
	}
	if (tonemapMode == NRI_PT_TONEMAP_REINHARD)
	{
		return TonemapReinhard(color);
	}

	return max(TonemapHable(color), 0.0);
}

float GetHdrSafePaperWhiteNits(float paperWhiteNits, float displaySdrLuminance, float displayMaxLuminance)
{
	return NriHdrScenePaperWhite(paperWhiteNits, displaySdrLuminance, displayMaxLuminance);
}

float GetHdrPaperWhiteScale(float paperWhiteNits, float displaySdrLuminance, float displayMaxLuminance)
{
	return GetHdrSafePaperWhiteNits(paperWhiteNits, displaySdrLuminance, displayMaxLuminance) / 80.0;
}

float GetHdrHeadroomInPaperWhites(float paperWhiteNits, float displaySdrLuminance, float displayMaxLuminance)
{
	const float safeDisplayMax = NriHdrSafePeak(displaySdrLuminance, displayMaxLuminance);
	const float safePaperWhite = GetHdrSafePaperWhiteNits(paperWhiteNits, displaySdrLuminance, displayMaxLuminance);
	return max(safeDisplayMax / safePaperWhite, 1.0);
}

float GetHdrMaxOutputScale(float paperWhiteNits, float displaySdrLuminance, float displayMaxLuminance)
{
	return NriHdrSafePeak(displaySdrLuminance, displayMaxLuminance) / 80.0;
}

float3 ApplyHdrOutputMapping(
	float3 color,
	uint tonemapMode,
	float saturation,
	float shoulder,
	float toe,
	float paperWhiteNits,
	float displaySdrLuminance,
	float displayMaxLuminance)
{
	const float headroomInPaperWhites = GetHdrHeadroomInPaperWhites(paperWhiteNits, displaySdrLuminance, displayMaxLuminance);
	const float3 peakRelativeInput = color / headroomInPaperWhites;
	// Scene paper white is a brightness scale, not a finite input forced to
	// display peak. Even at headroom == 1, brighter inputs keep rolling off.
	const float3 normalized = float3(
		NriHdrTonemapChannel(peakRelativeInput.x, tonemapMode),
		NriHdrTonemapChannel(peakRelativeInput.y, tonemapMode),
		NriHdrTonemapChannel(peakRelativeInput.z, tonemapMode));
	const float3 shaped = ApplyDisplayToeAndShoulder(normalized, toe, shoulder);
	const float luma = dot(shaped, float3(0.2126, 0.7152, 0.0722));
	const float chromaScale = NriHdrSaturationScale(luma,
		min(shaped.x, min(shaped.y, shaped.z)), max(shaped.x, max(shaped.y, shaped.z)), saturation);
	const float3 calibrated = luma.xxx + (shaped - luma.xxx) * chromaScale;
	// The final clamp handles roundoff only; the curve and chroma limiter
	// already fit the display range, with no early highlight flattening.
	return saturate(calibrated) * GetHdrMaxOutputScale(paperWhiteNits, displaySdrLuminance, displayMaxLuminance);
}

float LinearToSrgbChannel(float value)
{
	value = max(value, 0.0);
	if (value <= 0.0031308)
	{
		return value * 12.92;
	}

	return 1.055 * pow(value, 1.0 / 2.4) - 0.055;
}

float3 LinearToSrgb(float3 color)
{
	return float3(
		LinearToSrgbChannel(color.x),
		LinearToSrgbChannel(color.y),
		LinearToSrgbChannel(color.z));
}

float InterleavedGradientNoise(uint2 pixelPos, uint frameIndex)
{
	uint state = pixelPos.x * 0x1f123bb5u + pixelPos.y * 0x5f356495u + frameIndex * 0x6c8e9cf5u + 0x9e3779b9u;
	state ^= state >> 16u;
	state *= 0x7feb352du;
	state ^= state >> 15u;
	state *= 0x846ca68bu;
	state ^= state >> 16u;
	return (float)(state & 0x00ffffffu) * (1.0 / 16777215.0);
}

float3 ApplySdrTransferAndDither(float3 color, uint2 pixelPos, uint frameIndex)
{
	const float dither = InterleavedGradientNoise(pixelPos, frameIndex) - 0.5;
	const float3 srgb = LinearToSrgb(color);
	return saturate(srgb + dither / 255.0);
}

float3 ApplyLegacyClampOutputMapping(float3 color)
{
	return saturate(SanitizeFiniteColor(color));
}

float3 ApplyDebugRadianceDisplayMapping(float3 color, uint2 pixelPos, uint frameIndex)
{
	const float3 toneMapped = ApplySdrTonemap(SanitizeFiniteColor(color), NRI_PT_TONEMAP_HABLE);
	return ApplySdrTransferAndDither(toneMapped, pixelPos, frameIndex);
}

float3 ApplyDebugNormalizedDisplayMapping(float3 color, uint2 pixelPos, uint frameIndex)
{
	const float3 normalized = saturate(SanitizeFiniteColor(color));
	return ApplySdrTransferAndDither(normalized, pixelPos, frameIndex);
}

float3 ApplyPresentDisplayMapping(
	float3 color,
	uint2 pixelPos,
	uint frameIndex,
	uint outputMode,
	uint tonemapMode,
	uint outputFlags,
	float exposure,
	float contrast,
	float saturation,
	float shoulder,
	float toe,
	uint nightVisionPackedModeTint,
	float nightVisionStrength,
	float nightVisionExposure,
	uint nightVisionPackedControls,
	float paperWhiteNits,
	float displaySdrLuminance,
	float displayMaxLuminance)
{
	const bool hdrSwapChainActive =
		outputMode != NRI_PT_OUTPUT_MODE_SDR &&
		(outputFlags & NRI_PRESENT_OUTPUT_FLAG_HDR_SWAPCHAIN_ACTIVE) != 0u;
	float tunedExposure = exposure;
	float tunedContrast = contrast;
	float tunedSaturation = saturation;
	ApplyNightVisionDisplayControlMultipliers(
		nightVisionPackedModeTint,
		nightVisionStrength,
		nightVisionExposure,
		nightVisionPackedControls,
		tunedExposure,
		tunedContrast,
		tunedSaturation);
	const float3 sanitized = SanitizeFiniteColor(color);
	const float3 exposed = ApplyManualExposure(sanitized, tunedExposure);
	const float3 calibratedScene = ApplyPresentSceneContrast(exposed, tunedContrast);
	const float3 sceneWithViewEffects = ApplyNightVisionViewOperator(calibratedScene, nightVisionPackedModeTint, nightVisionStrength);
	if (hdrSwapChainActive)
	{
		return ApplyHdrOutputMapping(sceneWithViewEffects, tonemapMode, tunedSaturation, shoulder, toe, paperWhiteNits, displaySdrLuminance, displayMaxLuminance);
	}

	const float3 toneMapped = ApplySdrTonemap(sceneWithViewEffects, tonemapMode);
	const float3 calibratedDisplay = ApplyDisplayCalibration(toneMapped, tunedSaturation, toe, shoulder);
	return ApplySdrTransferAndDither(calibratedDisplay, pixelPos, frameIndex);
}

#endif
