#include "NRI.hlsl"
#include "NRD.hlsli"
#include "Include/PresentConstants.hlsli"

NRI_ROOT_CONSTANTS(NRIPresentConstants, gPresentConstants, 0, 2);

Texture2D<float4> gInputTexture : register(t0, space0);
Texture2D<float4> gUnused1 : register(t1, space0);
Texture2D<float4> gUnused2 : register(t2, space0);

NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float3>, gOutputTexture, u, 0, 1);

float3 ToneMapDebugRadiance(float3 value)
{
	value = max(value, 0.0);
	value *= 4.0;
	return value / (1.0 + value);
}

bool AnyNonFinite(float3 value)
{
	return any(isnan(value)) || any(isinf(value));
}

float3 VisualizeHdrProbe(float3 value)
{
	if (AnyNonFinite(value))
	{
		return float3(1.0, 0.0, 1.0);
	}

	const float3 positive = ToneMapDebugRadiance(max(value, 0.0));
	const float negativeMagnitude = max(max(-value.x, -value.y), -value.z);
	if (negativeMagnitude <= 0.0)
	{
		return positive;
	}

	const float negativeMarker = saturate(log2(1.0 + negativeMagnitude) / 8.0);
	return max(positive, float3(negativeMarker, 0.0, 0.0));
}

bool UseRelaxDenoiser()
{
	return gPresentConstants.DenoiserMode == 1u;
}

bool UseSplitShadowDenoiser()
{
	return (gPresentConstants.Flags & NRI_PRESENT_FLAG_SPLIT_SHADOW_DENOISER) != 0u;
}

float3 UnpackDebugRadiance(float4 packed)
{
	return UseRelaxDenoiser() ? RELAX_BackEnd_UnpackRadiance(packed).rgb : REBLUR_BackEnd_UnpackRadianceAndNormHitDist(packed).rgb;
}

static const float4 kReblurHitDistanceParams = float4(3.0, 0.1, 20.0, -25.0);

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
	float3 color = gInputTexture.Load(int3(samplePos, 0)).rgb;
	if ((gPresentConstants.Flags & NRI_PRESENT_FLAG_ADD_SECONDARY) != 0u)
	{
		color += gUnused1.Load(int3(samplePos, 0)).rgb;
	}
	if (gPresentConstants.DebugMode == 10u || gPresentConstants.DebugMode == 11u || gPresentConstants.DebugMode == 16u || gPresentConstants.DebugMode == 17u)
	{
		color = ToneMapDebugRadiance(UnpackDebugRadiance(gInputTexture.Load(int3(samplePos, 0))));
	}
	else
	if (gPresentConstants.DebugMode == 12u)
	{
		const float viewZ = abs(gUnused1.Load(int3(samplePos, 0)).x);
		if (viewZ >= NRD_INF * 0.5)
		{
			color = float3(1.0, 0.0, 0.0);
		}
		else if (UseRelaxDenoiser())
		{
			const float hitDistance = max(gInputTexture.Load(int3(samplePos, 0)).a, 0.0);
			const float mapped = saturate(log2(1.0 + hitDistance) / 12.0);
			color = mapped.xxx;
		}
		else
		{
			const float normalizedHitDistance = saturate(gInputTexture.Load(int3(samplePos, 0)).a);
			float materialID = 0.0;
			const float roughness = NRD_FrontEnd_UnpackNormalAndRoughness(gUnused2.Load(int3(samplePos, 0)), materialID).w;
			const float hitDistance = REBLUR_GetHitDist(normalizedHitDistance, viewZ, kReblurHitDistanceParams, roughness);
			const float mapped = saturate(log2(1.0 + max(hitDistance, 0.0)) / 12.0);
			color = mapped.xxx;
		}
	}
	else
	if (gPresentConstants.DebugMode == 18u)
	{
		const float metalness = saturate(gInputTexture.Load(int3(samplePos, 0)).a);
		color = metalness.xxx;
	}
	else
	if (gPresentConstants.DebugMode == 19u)
	{
		float materialID = 0.0;
		const float roughness = NRD_FrontEnd_UnpackNormalAndRoughness(gInputTexture.Load(int3(samplePos, 0)), materialID).w;
		color = saturate(roughness).xxx;
	}
	else
	if (gPresentConstants.DebugMode == 21u)
	{
		if (!UseSplitShadowDenoiser())
		{
			color = float3(1.0, 0.0, 1.0);
		}
		else
		{
			const float penumbra = max(gInputTexture.Load(int3(samplePos, 0)).x, 0.0);
			const float mapped = saturate(log2(1.0 + penumbra) / 8.0);
			color = mapped.xxx;
		}
	}
	else
	if (gPresentConstants.DebugMode == 22u)
	{
		if (!UseSplitShadowDenoiser())
		{
			color = float3(1.0, 0.0, 1.0);
		}
		else
		{
			const float rawShadow = gInputTexture.Load(int3(samplePos, 0)).x >= NRD_FP16_MAX ? 1.0 : 0.0;
			color = rawShadow.xxx;
		}
	}
	else
	if (gPresentConstants.DebugMode == 24u || gPresentConstants.DebugMode == 25u)
	{
		color = VisualizeHdrProbe(gInputTexture.Load(int3(samplePos, 0)).rgb);
	}
	else
	if (gPresentConstants.DebugMode == 34u)
	{
		color = VisualizeHdrProbe(gInputTexture.Load(int3(samplePos, 0)).rgb);
	}
	else
	{
		color = saturate(color);
	}
	gOutputTexture[targetPixelPos] = color;
}
