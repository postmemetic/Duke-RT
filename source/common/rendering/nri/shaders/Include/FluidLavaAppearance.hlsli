#ifndef NRI_FLUID_LAVA_APPEARANCE_HLSLI
#define NRI_FLUID_LAVA_APPEARANCE_HLSLI

// Appearance only for the D3D convection-lava opt-in. The signed field used by
// the normals and virtual-height predictor also orders the thermal palette:
// bodycolor is cooled rock, emissioncolor is molten orange, crestcolor is hot.
// No independent noise mask, geometry, sampled light or secondary-ray policy.
struct FluidLavaAppearance
{
	float3 bodyColor;
	float3 emission;
};

FluidLavaAppearance FluidEvaluateLavaAppearance(
	FluidBindingData binding, float heightRatio, float3 cooledBodyColor)
{
	const float height = clamp(heightRatio, -1.0, 1.0);
	const float3 cooled = max(cooledBodyColor, 0.0);
	const float3 molten = max(binding.emissionColorIntensity.rgb, 0.0);
	const float3 hottest = max(binding.crestColorOpacity.rgb, 0.0);
	const float crestGain = max(binding.virtualParams.z, 0.0);
	// Compress thermal extremes without changing geometric relief. Let some
	// cooling enter the lower shoulders while retaining compact dark troughs.
	// q(h)=.35h+.65h^5 preserves -1/0/1 and q'(h)>=.35, keeping visible color
	// variation through the middle rather than a near-uniform orange plateau.
	const float heightSq = height * height;
	const float coolingHeight = height * (0.35 + 0.65 * heightSq * heightSq);
	// Softness still controls the cool-to-orange shoulder.
	const float moltenWeight = pow(smoothstep(-1.0, 0.2, coolingHeight),
		rcp(clamp(binding.crust.y, 0.05, 4.0)));
	float3 heatColor = lerp(cooled, molten, moltenWeight);
	// Yellow begins lower on the shoulders, narrowing the orange band. Admit
	// blue only near the highest tips to preserve the accepted small white caps.
	// Each channel and its boundary derivatives remain continuous.
	const float3 crestWeight = float3(
		smoothstep(0.0, 0.9, height),
		smoothstep(-0.20, 0.55, height),
		smoothstep(0.78, 0.98, height));
	heatColor = lerp(heatColor, hottest, crestWeight);
	const float crustShade = lerp(
		saturate(1.0 - binding.crust.w), 1.0, moltenWeight);
	const float heatGain = lerp(0.04, 1.0, moltenWeight) *
		(1.0 + crestGain * smoothstep(0.1, 0.9, height));
	FluidLavaAppearance result;
	result.bodyColor = lerp(cooled, heatColor, saturate(crestGain)) * crustShade;
	// Preserve both established no-glow opt-outs even though body/crest now
	// provide extra palette anchors. Foam still covers this emission downstream.
	result.emission = any(molten > 0.0) ? heatColor *
		(max(binding.emissionColorIntensity.w, 0.0) * heatGain) : 0.0.xxx;
	return result;
}

#endif
