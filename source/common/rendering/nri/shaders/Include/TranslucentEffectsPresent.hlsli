#ifndef RAZE_NRI_TRANSLUCENT_EFFECTS_PRESENT_HLSLI
#define RAZE_NRI_TRANSLUCENT_EFFECTS_PRESENT_HLSLI

float4 LoadTranslucentEffectLayer(int2 pixelPos)
{
	float4 effect = gTranslucentEffects.Load(int3(pixelPos, 0));
	if ((gPresentConstants.OutputFlags & NRI_PRESENT_OUTPUT_FLAG_EFFECT_SMOKE) != 0u)
	{
		// Preserve the previous whole-ray smoke attenuation approximation.
		// The resolved volume stores optical depth, not transmittance, in alpha.
		const float opticalDepth = gEffectSmoke.Load(int3(pixelPos, 0)).a;
		effect *= exp(-clamp(opticalDepth, 0.0, 16.0));
	}
	return effect;
}

float4 SampleTranslucentEffectLayer(uint2 pixelPos, uint2 outputSize)
{
	uint2 size;
	gTranslucentEffects.GetDimensions(size.x, size.y);
	// The ray queries use the opaque trace's jitter. Undo it when placing the
	// current-frame layer over the reconstructed, unjittered output image.
	const float2 position = (float2(pixelPos) + 0.5) * float2(size) / float2(outputSize)
		- 0.5 - gPresentConstants.EffectJitter;
	const int2 base = int2(floor(position));
	const float2 weight = frac(position);
	const int2 last = int2(size) - 1;
	const float4 a = LoadTranslucentEffectLayer(clamp(base, 0, last));
	const float4 b = LoadTranslucentEffectLayer(clamp(base + int2(1, 0), 0, last));
	const float4 c = LoadTranslucentEffectLayer(clamp(base + int2(0, 1), 0, last));
	const float4 d = LoadTranslucentEffectLayer(clamp(base + 1, 0, last));
	return lerp(lerp(a, b, weight.x), lerp(c, d, weight.x), weight.y);
}

float EffectSrgbToLinear(float value)
{
	return value <= 0.04045 ? value / 12.92 : pow((value + 0.055) / 1.055, 2.4);
}

float3 EffectSrgbToLinear(float3 color)
{
	return float3(EffectSrgbToLinear(color.x), EffectSrgbToLinear(color.y), EffectSrgbToLinear(color.z));
}

float3 BlendTranslucentEffectLayer(float3 background, float4 effect, bool hdrSwapChainActive)
{
	if (effect.a <= 0.0) return background;
	// Build's palette art and alpha describe a blend in display color, not a
	// source of scene radiance. In HDR, use paper white as the art's white and
	// retain the background's extended range across the transfer conversion.
	const float paperWhiteScale = hdrSwapChainActive ? GetHdrPaperWhiteScale(
		gPresentConstants.PaperWhiteNits, gPresentConstants.DisplaySdrLuminance,
		gPresentConstants.DisplayMaxLuminance) : 1.0;
	float3 displayBackground = hdrSwapChainActive ? LinearToSrgb(background / paperWhiteScale) : background;
	float3 effectColor = effect.rgb / effect.a;
	if (gPresentConstants.NightVisionStrength > 0.0)
	{
		effectColor = LinearToSrgb(ApplyNightVisionViewOperator(EffectSrgbToLinear(effectColor),
			gPresentConstants.NightVisionPackedModeTint, gPresentConstants.NightVisionStrength));
	}
	const float3 blended = effectColor * effect.a + (1.0 - effect.a) * displayBackground;
	// Night-vision tint can exceed one; respect the presenter's display peak.
	return hdrSwapChainActive ? min(EffectSrgbToLinear(blended) * paperWhiteScale,
		GetHdrMaxOutputScale(gPresentConstants.PaperWhiteNits, gPresentConstants.DisplaySdrLuminance,
			gPresentConstants.DisplayMaxLuminance)) : saturate(blended);
}

#endif
