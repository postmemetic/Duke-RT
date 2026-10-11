#ifndef RAZE_NRI_HDR_OUTPUT_MATH_HLSLI
#define RAZE_NRI_HDR_OUTPUT_MATH_HLSLI

// Scalar output math shared with nri_output.h and native regression tests.
// Keep this include valid in both C++ and HLSL; transport units are linear scRGB.
#ifdef __cplusplus
#include <cmath>
#endif

inline bool NriHdrIsFinite(float value)
{
#ifdef __cplusplus
	return std::isfinite(value);
#else
	return !isnan(value) && !isinf(value);
#endif
}

inline float NriHdrSafeSdrWhite(float sdrWhite)
{
	if (!NriHdrIsFinite(sdrWhite) || sdrWhite <= 0.0f)
		return 80.0f;
	return sdrWhite > 1.0f ? sdrWhite : 1.0f;
}

inline float NriHdrSafePeak(float sdrWhite, float peak)
{
	// Windows' SDR reference white is not a lower bound on a native HDR peak.
	// Some displays report a peak below that reference white; honor valid data.
	if (!NriHdrIsFinite(peak) || peak <= 0.0f)
		return NriHdrSafeSdrWhite(sdrWhite);
	return peak > 1.0f ? peak : 1.0f;
}

inline float NriHdrScenePaperWhite(float requested, float sdrWhite, float peak)
{
	const float safePeak = NriHdrSafePeak(sdrWhite, peak);
	const float finiteWhite = NriHdrIsFinite(requested) ? requested : 80.0f;
	const float positiveWhite = finiteWhite > 1.0f ? finiteWhite : 1.0f;
	return positiveWhite < safePeak ? positiveWhite : safePeak;
}

inline float NriHdrTonemapChannel(float value, unsigned int tonemapMode)
{
	if (!(value > 0.0f))
		return 0.0f;
	if (!NriHdrIsFinite(value))
		return 1.0f;

	// Mode values match NRIPTTonemapMode / PresentConstants.hlsli.
	if (tonemapMode == 2u)
		return value / (1.0f + value);

	// Normalize the raw curves to their infinite-input limits, not a finite
	// white point. The SDR ACES clamp and Hable 11.2 white would flatten HDR
	// highlights even if the later reference-white clamp were removed.
	const bool aces = tonemapMode == 1u;
	const float a = aces ? 2.43f : 0.15f;
	const float b = aces ? 0.03f * (2.43f / 2.51f) :
		0.50f * (0.10f - 0.02f / 0.30f) / (1.0f - 0.02f / 0.30f);
	const float c = aces ? 0.59f : 0.50f;
	const float d = aces ? 0.14f : 0.20f * 0.30f;
	if (value <= 1.0f)
		return value * (a * value + b) / (value * (a * value + c) + d);

	// Divide through by value^2 to avoid overflow in bright emissive pixels.
	const float inverseValue = 1.0f / value;
	return (a + b * inverseValue) / (a + c * inverseValue + d * inverseValue * inverseValue);
}

inline float NriHdrSaturationScale(float luma, float minChannel, float maxChannel, float requested)
{
	float scale = requested > 0.0f && NriHdrIsFinite(requested) ? requested : 0.0f;
	// Limit chroma as a whole instead of independently clipping RGB. Bright
	// colored gradients retain their luminance even with saturation above 1.
	if (minChannel < luma)
	{
		const float lowerBound = luma / (luma - minChannel);
		scale = scale < lowerBound ? scale : lowerBound;
	}
	if (maxChannel > luma)
	{
		const float upperBound = (1.0f - luma) / (maxChannel - luma);
		scale = scale < upperBound ? scale : upperBound;
	}
	return scale;
}

#endif
