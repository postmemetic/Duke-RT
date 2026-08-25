#include "NRI.hlsl"
#include "Include/TemporalConstants.hlsli"
#include "Include/DisplayMapping.hlsli"

#define NRI_FLAG_RESET_HISTORY 0x1u
#define NRI_TEMPORAL_FLAG_AUTO_EXPOSURE 0x1000u
#define NRI_TEMPORAL_FLAG_EXPOSURE_TEXTURE_VALID 0x2000u
#define NRI_TEMPORAL_FLAG_VOLUME_REACTIVE 0x4000u
#define TAA_HISTORY_FRAME_CAP 12.0
#define TAA_BASE_BLEND (1.0 / TAA_HISTORY_FRAME_CAP)
#define TAA_SIGMA_SCALE 2.0
#define TAA_REJECTION_SCALE 2.0
#define TAA_HISTORY_EPSILON 1e-4
#define TAA_MOTION_BLEND_SCALE 1.5
#define TAA_MOTION_REJECT_PIXELS 2.0
#define TAA_MOTION_MIN_WEIGHT 0.2

NRI_ROOT_CONSTANTS(NRITemporalConstants, gTemporalConstants, 0, 2);

Texture2D<float4> gHistoryInput : register(t0, space0);
Texture2D<float4> gMotionInput : register(t1, space0);
Texture2D<float4> gComposedInput : register(t2, space0);
Texture2D<float4> gExposureStateInput : register(t3, space0);
Texture2D<float4> gVolumeMetaInput : register(t4, space0);
Texture2D<float4> gTemporalValidityInput : register(t5, space0);

NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float4>, gHistoryOutput, u, 0, 1);

struct TemporalExposureState
{
	float currentExposure;
	float historyScale;
};

float SanitizeExposureValue(float value, float fallback)
{
	return value > 0.0 && !isnan(value) && !isinf(value) ? value : fallback;
}

TemporalExposureState LoadTemporalExposureState()
{
	TemporalExposureState state;
	state.currentExposure = gTemporalConstants.Exposure;
	state.historyScale = 1.0;

	const bool shouldUseAutoExposure =
		(gTemporalConstants.Flags & NRI_TEMPORAL_FLAG_AUTO_EXPOSURE) != 0u &&
		(gTemporalConstants.Flags & NRI_TEMPORAL_FLAG_EXPOSURE_TEXTURE_VALID) != 0u;
	if (!shouldUseAutoExposure)
	{
		return state;
	}

	const float4 exposureState = gExposureStateInput.Load(int3(0, 0, 0));
	state.currentExposure = SanitizeExposureValue(exposureState.x, gTemporalConstants.Exposure);
	const float previousExposure = SanitizeExposureValue(exposureState.z, state.currentExposure);
	state.historyScale = state.currentExposure / previousExposure;
	return state;
}

float4 LoadHistory(int2 pixelPos, uint2 size)
{
	const int2 clampedPos = clamp(pixelPos, int2(0, 0), int2(size) - 1);
	return gHistoryInput.Load(int3(clampedPos, 0));
}

float4 SampleHistoryBilinear(float2 uv, uint2 size)
{
	const float2 texelPos = uv * float2(size) - 0.5;
	const int2 basePos = int2(floor(texelPos));
	const float2 blend = frac(texelPos);
	const float4 h00 = LoadHistory(basePos, size);
	const float4 h10 = LoadHistory(basePos + int2(1, 0), size);
	const float4 h01 = LoadHistory(basePos + int2(0, 1), size);
	const float4 h11 = LoadHistory(basePos + int2(1, 1), size);
	const float4 hx0 = lerp(h00, h10, blend.x);
	const float4 hx1 = lerp(h01, h11, blend.x);
	return lerp(hx0, hx1, blend.y);
}

float MaxComponent(float3 value)
{
	return max(value.x, max(value.y, value.z));
}

float3 LoadCurrentColor(int2 pixelPos, uint2 size, float exposure)
{
	const int2 clampedPos = clamp(pixelPos, int2(0, 0), int2(size) - 1);
	// Native TAA operates on a pre-exposed HDR signal so history stays in a stable FP16-friendly domain.
	const float3 sceneColor = SanitizeFiniteColor(gComposedInput.Load(int3(clampedPos, 0)).rgb);
	return ApplyManualExposure(sceneColor, exposure);
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint width = 0;
	uint height = 0;
	gComposedInput.GetDimensions(width, height);
	if (dispatchThreadId.x >= width || dispatchThreadId.y >= height)
	{
		return;
	}

	const uint2 pixelPos = dispatchThreadId.xy;
	const uint2 size = uint2(width, height);
	const float2 resolution = float2(size);
	const float2 uv = ((float2)pixelPos + 0.5) / resolution;
	const bool resetHistory = (gTemporalConstants.Flags & NRI_FLAG_RESET_HISTORY) != 0u;
	const TemporalExposureState exposureState = LoadTemporalExposureState();
	const float3 currentColor = LoadCurrentColor(int2(pixelPos), size, exposureState.currentExposure);
	const float volumeReactive = (gTemporalConstants.Flags & NRI_TEMPORAL_FLAG_VOLUME_REACTIVE) != 0u ?
		saturate(gVolumeMetaInput.Load(int3(pixelPos, 0)).x) : 0.0;

	const float4 centerMotion = gMotionInput[pixelPos];
	// TAA consumes the shared PT motion contract from Shared.hlsli:
	// - xy is pixel-space old-minus-new reprojection, excluding temporal jitter
	// - w is a Raze-local validity/history signal, not an NRD requirement
	const bool unreliableHistory = gTemporalValidityInput.Load(int3(pixelPos, 0)).x < 0.5;
	const int radius = 1;
	float sum = 0.0;
	float3 mean = 0.0;
	float3 meanSquares = 0.0;
	float3 neighborhoodMin = 1e6;
	float3 neighborhoodMax = -1e6;
	float2 selectedMotion = centerMotion.xy;

	[loop]
	for (int y = -radius; y <= radius; ++y)
	{
		[loop]
		for (int x = -radius; x <= radius; ++x)
		{
			const int2 samplePos = clamp(int2(pixelPos) + int2(x, y), int2(0, 0), int2(size) - 1);
			const float3 sampleColor = LoadCurrentColor(samplePos, size, exposureState.currentExposure);
			const float weight = exp(-0.5 * float(x * x + y * y));

			mean += sampleColor * weight;
			meanSquares += sampleColor * sampleColor * weight;
			sum += weight;
			neighborhoodMin = min(neighborhoodMin, sampleColor);
			neighborhoodMax = max(neighborhoodMax, sampleColor);
		}
	}

	mean /= max(sum, TAA_HISTORY_EPSILON);
	meanSquares /= max(sum, TAA_HISTORY_EPSILON);
	const float3 sigma = sqrt(max(meanSquares - mean * mean, 0.0)) * TAA_SIGMA_SCALE;
	const float3 clampMin = max(neighborhoodMin, mean - sigma);
	const float3 clampMax = min(neighborhoodMax, mean + sigma);
	// TAA history is a resolved pixel-grid image, so reproject with raw screen motion and keep jitter out of the history sample position.
	const float2 prevUv = uv + selectedMotion / resolution;
	const bool historyInScreen = !unreliableHistory && all(prevUv >= 0.0) && all(prevUv <= 1.0);

	float4 historySample = float4(currentColor, 0.0);
	if (!resetHistory && historyInScreen)
	{
		historySample = SampleHistoryBilinear(prevUv, size);
		historySample.rgb *= exposureState.historyScale;
	}

	const float3 historyColor = max(historySample.rgb, 0.0);
	const float3 clampedHistory = clamp(historyColor, clampMin, clampMax);
	const float divergence = MaxComponent(abs(historyColor - clampedHistory));
	const float historyScale = max(MaxComponent(currentColor), MaxComponent(clampedHistory));
	const float motionPixels = length(selectedMotion);
	const float motionRejection = saturate(motionPixels / TAA_MOTION_BLEND_SCALE);
	const float clampRejection = saturate(divergence / max(historyScale, 1.0) * TAA_REJECTION_SCALE);
	const float rejection = max(clampRejection, motionRejection);
	const bool rejectHistory = resetHistory || !historyInScreen || motionPixels >= TAA_MOTION_REJECT_PIXELS;

	float effectiveHistoryFrames = 1.0 + saturate(historySample.w) * (TAA_HISTORY_FRAME_CAP - 1.0);
	effectiveHistoryFrames = lerp(effectiveHistoryFrames, 1.0, rejection);

	float currentWeight = max(1.0 / (effectiveHistoryFrames + 1.0), TAA_BASE_BLEND);
	currentWeight = max(currentWeight, rejection);
	currentWeight = max(currentWeight, volumeReactive);
	if (motionPixels > 0.0)
	{
		currentWeight = max(currentWeight, TAA_MOTION_MIN_WEIGHT);
	}
	if (rejectHistory || volumeReactive >= 0.95)
	{
		currentWeight = 1.0;
		effectiveHistoryFrames = 1.0;
	}

	const float3 resultColor = lerp(clampedHistory, currentColor, currentWeight);
	const float nextHistoryFrames = rejectHistory ? 1.0 : min(effectiveHistoryFrames + 1.0, TAA_HISTORY_FRAME_CAP);
	const float nextHistoryAlpha = (nextHistoryFrames - 1.0) / (TAA_HISTORY_FRAME_CAP - 1.0);
	gHistoryOutput[pixelPos] = float4(resultColor, nextHistoryAlpha);
}
