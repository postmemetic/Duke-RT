#include "Include/Shared.hlsli"

float3 SanitizeColor(float3 value)
{
	if (any(isnan(value)) || any(isinf(value)))
	{
		return 0.0;
	}

	return value;
}

bool UseRelaxDenoiser()
{
	return (gTraceConstants.ReservedTrace1 & 0xffu) == 1u;
}

bool UseSplitShadowDenoiser()
{
	return (gTraceConstants.Flags & 0x20u) != 0;
}

bool UseDirectionalPlaceholderLight()
{
	return (gTraceConstants.Flags & 0x80u) != 0;
}

bool UseDirectionalPlaceholderShadow()
{
	return (gTraceConstants.Flags & NRI_FLAG_DIRECTIONAL_LIGHT_SHADOW) != 0;
}

float3 UnpackDenoisedRadiance(float4 packed)
{
	return UseRelaxDenoiser() ? RELAX_BackEnd_UnpackRadiance(packed).rgb : REBLUR_BackEnd_UnpackRadianceAndNormHitDist(packed).rgb;
}

float3 GeneratePrimaryRay(uint2 pixelPos)
{
	float2 resolution = float2(gTraceConstants.RenderWidth, gTraceConstants.RenderHeight);
	float2 uv = ((float2)pixelPos + 0.5) / resolution;
	float2 ndc = uv * 2.0 - 1.0;
	ndc.y = -ndc.y;

	float3 ray =
		gTraceConstants.CameraForward +
		ndc.x * gTraceConstants.TanHalfFovX * gTraceConstants.CameraRight +
		ndc.y * gTraceConstants.TanHalfFovY * gTraceConstants.CameraUp;

	return normalize(ray);
}

float GetMaterialID(uint2 pixelPos)
{
	float materialID = 0.0;
	NRD_FrontEnd_UnpackNormalAndRoughness(gNormalRoughnessInput[pixelPos], materialID);
	return materialID;
}

bool IsSpecularSpecialMaterial(float materialID)
{
	return materialID >= 2.5;
}

float3 GetSurfaceDiffuseColor(float3 albedo, float metalness)
{
	return albedo * (1.0 - metalness);
}

float3 GetSurfaceSpecularColor(float3 albedo, float metalness)
{
	return lerp(float3(0.04, 0.04, 0.04), albedo, metalness);
}

void GetNrdPrimaryMaterialFactors(float3 normal, float3 viewDir, float3 baseColor, float metalness, float roughness, out float3 diffuseFactor, out float3 specularFactor)
{
	const float3 albedo = GetSurfaceDiffuseColor(baseColor, metalness);
	const float3 rf0 = GetSurfaceSpecularColor(baseColor, metalness);
	NRD_MaterialFactors(normalize(normal), normalize(viewDir), albedo, rf0, roughness, diffuseFactor, specularFactor);
}

float GetRawSunShadow(uint2 pixelPos)
{
	// A directional miss is encoded as NRD_FP16_MAX; every finite penumbra value is occluded.
	return gGuideSpecHitInput.Load(int3(pixelPos, 0)).x >= NRD_FP16_MAX ? 1.0 : 0.0;
}

float GetFilteredSunShadow(uint2 pixelPos)
{
	return saturate(SIGMA_BackEnd_UnpackShadow(gShadowInput.Load(int3(pixelPos, 0))).x);
}

void ApplyShadowAwareDirectLightingCorrection(uint2 pixelPos, float materialID, bool useFilteredShadow, inout float3 directLighting)
{
	if (!UseDirectionalPlaceholderLight() || !UseDirectionalPlaceholderShadow())
	{
		return;
	}

	const float rawShadow = GetRawSunShadow(pixelPos);
	// TraceOpaque owns the exact center-direction BRDF and material-normal policy. SIGMA consumes
	// only penumbra.x, leaving yzw available for that unshadowed RGB term. TraceOpaque deliberately
	// leaves this term out of the FP16 direct-light target while split-shadow denoising is active;
	// adding it here once avoids a quantized subtract-and-replace residual correlated with raw samples.
	float3 unshadowedDirectionalLighting = SanitizeColor(gGuideSpecHitInput.Load(int3(pixelPos, 0)).yzw);
	// Emission is carried independently in DirectEmission, so an emissive receiver's
	// ordinary directional diffuse/specular term can and should consume SIGMA just like
	// any other shadow receiver. Only special specular materials retain the raw fallback.
	const bool rawOnlyMaterial = IsSpecularSpecialMaterial(materialID);
	const bool useFilteredVisibility = UseSplitShadowDenoiser() && useFilteredShadow && !rawOnlyMaterial;
	const float targetShadow = useFilteredVisibility ? GetFilteredSunShadow(pixelPos) : rawShadow;
	directLighting += unshadowedDirectionalLighting * targetShadow;
}

float3 ComposeLighting(uint2 pixelPos, float3 diffuseSignal, float3 specularSignal, float3 directLighting, float3 directEmission, float3 normal, float3 albedo, float metalness, float roughness, float3 viewDir)
{
	const float viewZ = abs(gViewZInput.Load(int3(pixelPos, 0)).x);
	if (viewZ >= NRD_INF * 0.5)
	{
		return directEmission;
	}

	// Phase-2 composition contract:
	// - diffuse/specular: primary-hit demodulated radiance from NRD
	// - directLighting: direct-composition bucket (ambient + runtime point lights, plus sun when split shadow is inactive)
	// - split-shadow sun: producer-owned unshadowed RGB sideband multiplied once by raw or filtered visibility above
	// - directEmission: actual emissive-hit / fullbright surface output
	float3 diffuseFactor = 1.0;
	float3 specularFactor = 1.0;
	GetNrdPrimaryMaterialFactors(normal, viewDir, albedo, metalness, roughness, diffuseFactor, specularFactor);
	const float3 shadedDiffuse = diffuseSignal * diffuseFactor;
	const float3 shadedSpecular = specularSignal * specularFactor;

	// This remodulates the NRD-facing transport back into the beauty signal using the same
	// primary material factors that TraceOpaque used for de-modulation.
	return directEmission + directLighting + shadedDiffuse + shadedSpecular;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if (dispatchThreadId.x >= gTraceConstants.RenderWidth || dispatchThreadId.y >= gTraceConstants.RenderHeight)
	{
		return;
	}

	const uint2 pixelPos = dispatchThreadId.xy;
	const float3 rawDiffuseSignal = SanitizeColor(UnpackDenoisedRadiance(gComposedInput.Load(int3(pixelPos, 0))));
	const float3 rawSpecular = SanitizeColor(UnpackDenoisedRadiance(gUpscaledInput.Load(int3(pixelPos, 0))));
	const float3 filteredDiffuseSignal = SanitizeColor(UnpackDenoisedRadiance(gGuideDiffuseInput.Load(int3(pixelPos, 0))));
	const float3 filteredSpecular = SanitizeColor(UnpackDenoisedRadiance(gGuideSpecularInput.Load(int3(pixelPos, 0))));
	const float3 composedDirectLighting = SanitizeColor(gDirectLightingInput.Load(int3(pixelPos, 0)).rgb);
	const float3 surfaceDirectEmission = SanitizeColor(gDirectEmissionInput.Load(int3(pixelPos, 0)).rgb);
	const float4 baseColorMetalness = gBaseColorInput.Load(int3(pixelPos, 0));
	float materialID = 0.0;
	const float4 unpackedNormalRoughness = NRD_FrontEnd_UnpackNormalAndRoughness(gNormalRoughnessInput[pixelPos], materialID);
	const float3 normal = unpackedNormalRoughness.xyz;
	const float roughness = saturate(unpackedNormalRoughness.w);
	const float metalness = saturate(baseColorMetalness.a);
	const float3 albedo = SanitizeColor(baseColorMetalness.rgb);
	const float3 viewDir = normalize(-GeneratePrimaryRay(pixelPos));

	float3 adjustedRawDirectLighting = composedDirectLighting;
	float3 adjustedFilteredDirectLighting = composedDirectLighting;
	ApplyShadowAwareDirectLightingCorrection(pixelPos, materialID, false, adjustedRawDirectLighting);
	ApplyShadowAwareDirectLightingCorrection(pixelPos, materialID, true, adjustedFilteredDirectLighting);

	const float3 filteredComposed = ComposeLighting(pixelPos, filteredDiffuseSignal, filteredSpecular, adjustedFilteredDirectLighting, surfaceDirectEmission, normal, albedo, metalness, roughness, viewDir);
	const float3 rawComposed = ComposeLighting(pixelPos, rawDiffuseSignal, rawSpecular, adjustedRawDirectLighting, surfaceDirectEmission, normal, albedo, metalness, roughness, viewDir);
	const bool specialMaterialRawFallback = UseSplitShadowDenoiser() && IsSpecularSpecialMaterial(materialID);
	float3 composed = specialMaterialRawFallback ? rawComposed : filteredComposed;
	const uint splitMode = gTraceConstants.ReservedTrace0;
	if (splitMode != 0u)
	{
		const bool leftSide = pixelPos.x * 2u < gTraceConstants.RenderWidth;
		const bool rawOnLeft = splitMode == 1u;
		composed = (leftSide == rawOnLeft) ? rawComposed : filteredComposed;
		if (specialMaterialRawFallback)
		{
			composed = rawComposed;
		}

		const int dividerX = int(gTraceConstants.RenderWidth / 2u);
		if (abs((int)pixelPos.x - dividerX) <= 1)
		{
			composed = 1.0;
		}
	}
	gComposedOutput[pixelPos] = float4(SanitizeColor(composed), 1.0);
}
