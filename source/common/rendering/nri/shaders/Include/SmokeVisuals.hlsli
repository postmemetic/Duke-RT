#ifndef NRI_SMOKE_VISUALS_HLSLI
#define NRI_SMOKE_VISUALS_HLSLI

#define NRI_SMOKE_FIELD_DEBUG_MASS 1u
#define NRI_SMOKE_FIELD_DEBUG_EXTINCTION 2u
#define NRI_SMOKE_FIELD_DEBUG_ALBEDO 3u
#define NRI_SMOKE_FIELD_DEBUG_THERMAL 4u
#define NRI_SMOKE_FIELD_DEBUG_GRADIENT_MAGNITUDE 5u
#define NRI_SMOKE_FIELD_DEBUG_GRADIENT_ORIENTATION 6u
#define NRI_SMOKE_FIELD_DEBUG_LOBE_SUM 7u
#define NRI_SMOKE_FIELD_DEBUG_LOBE_DIRECTION 8u
#define NRI_SMOKE_FIELD_DEBUG_LOBE_CONTRAST 9u
#define NRI_SMOKE_FIELD_DEBUG_LOBE_CONFIDENCE 10u

float SmokeVisualTone(float value)
{
	value = max(isfinite(value) ? value : 0.0, 0.0);
	return value / (1.0 + value);
}

float3 SmokeVisualUnpackColor(uint packed)
{
	const uint3 code = uint3(packed & 31u, (packed >> 5u) & 31u,
		(packed >> 10u) & 31u);
	return float3(
		code.x <= 16u ? (float)code.x / 16.0 : 1.0 + (float)(code.x - 16u) / 15.0,
		code.y <= 16u ? (float)code.y / 16.0 : 1.0 + (float)(code.y - 16u) / 15.0,
		code.z <= 16u ? (float)code.z / 16.0 : 1.0 + (float)(code.z - 16u) / 15.0);
}

float2 SmokeVisualUnpackHalf2(uint packed)
{
	return float2(f16tof32(packed & 0xffffu), f16tof32(packed >> 16u));
}

float SmokeVisualShapeExtinction(float baseExtinction)
{
	const float base = max(isfinite(baseExtinction) ? baseExtinction : 0.0, 0.0);
	if (base <= 0.0)
		return 0.0;

	const float2 toe = SmokeVisualUnpackHalf2(gSmokeConstants.Visuals.x);
	const float2 middle = SmokeVisualUnpackHalf2(gSmokeConstants.Visuals.y);
	const float2 shoulderControls = SmokeVisualUnpackHalf2(gSmokeConstants.Visuals.z);
	const float threshold = max(toe.x, 0.0);
	const float knee = max(toe.y, 0.0);
	float shaped = base;
	if (threshold > 0.0 || knee > 0.0)
	{
		const float low = max(threshold - knee, 0.0);
		const float high = max(threshold + knee, low + 1e-6);
		shaped *= smoothstep(low, high, base);
	}

	const float reference = max(middle.y, 1e-6);
	const float gamma = max(middle.x, 0.05);
	shaped = reference * pow(max(shaped / reference, 0.0), gamma);
	const float shoulder = max(shoulderControls.x, 0.0);
	if (shoulder > 1e-6)
		shaped = shoulder * (1.0 - exp(-shaped / shoulder));
	return max(isfinite(shaped) ? shaped : 0.0, 0.0);
}

void SmokeVisualShapeMedium(float baseExtinction, float3 baseScattering,
	float3 baseSource, out float extinction, out float3 scattering, out float3 source)
{
	extinction = SmokeVisualShapeExtinction(baseExtinction);
	const float extinctionRatio = baseExtinction > 1e-6 ? extinction / baseExtinction : 0.0;
	const float colorPivot = max(SmokeVisualUnpackHalf2(gSmokeConstants.Visuals.z).y, 0.0);
	const float colorWidth = colorPivot * 0.5;
	const float colorLow = max(colorPivot - colorWidth, 0.0);
	const float colorHigh = max(colorPivot + colorWidth, colorLow + 1e-6);
	const float colorWeight = smoothstep(colorLow, colorHigh, extinction);
	const float3 tint = lerp(SmokeVisualUnpackColor(gSmokeConstants.Visuals.w),
		SmokeVisualUnpackColor(gSmokeConstants.Visuals.w >> 15u), colorWeight);
	const float3 safeScattering = max(all(isfinite(baseScattering)) ? baseScattering : 0.0, 0.0);
	const float3 candidateScattering = safeScattering * extinctionRatio * max(tint, 0.0);
	scattering = min(candidateScattering, extinction.xxx);
	const float3 sourceRatio = float3(
		safeScattering.x > 1e-6 ? scattering.x / safeScattering.x : 0.0,
		safeScattering.y > 1e-6 ? scattering.y / safeScattering.y : 0.0,
		safeScattering.z > 1e-6 ? scattering.z / safeScattering.z : 0.0);
	source = max(all(isfinite(baseSource)) ? baseSource : 0.0, 0.0) * sourceRatio;
	source = max(all(isfinite(source)) ? source : 0.0, 0.0);
}

float3 SmokeVisualExtinctionGradient(float4 scalarCorners[8], float3 blend,
	float cellSize)
{
	float values[8];
	[unroll]
	for (uint corner = 0u; corner < 8u; ++corner)
		values[corner] = max(scalarCorners[corner].z * gSmokeConstants.DensityScale, 0.0);
	const float dx0 = lerp(values[1] - values[0], values[3] - values[2], blend.y);
	const float dx1 = lerp(values[5] - values[4], values[7] - values[6], blend.y);
	const float dy0 = lerp(values[2] - values[0], values[3] - values[1], blend.x);
	const float dy1 = lerp(values[6] - values[4], values[7] - values[5], blend.x);
	const float dz0 = lerp(values[4] - values[0], values[5] - values[1], blend.x);
	const float dz1 = lerp(values[6] - values[2], values[7] - values[3], blend.x);
	return float3(lerp(dx0, dx1, blend.z), lerp(dy0, dy1, blend.z),
		lerp(dz0, dz1, blend.y)) / max(cellSize, 1e-6);
}

float3 SmokeVisualLocalDiagnostic(uint mode, float4 scalar, float4 optical,
	float4 scalarCorners[8], float3 blend, float cellSize)
{
	if (mode == NRI_SMOKE_FIELD_DEBUG_MASS)
		return SmokeVisualTone(max(scalar.x, 0.0)).xxx;
	const float extinction = max(scalar.z * gSmokeConstants.DensityScale, 0.0);
	if (mode == NRI_SMOKE_FIELD_DEBUG_EXTINCTION)
		return (1.0 - exp(-extinction * cellSize)).xxx;
	if (mode == NRI_SMOKE_FIELD_DEBUG_ALBEDO)
		return extinction > 1e-6 ? saturate(optical.rgb * gSmokeConstants.DensityScale / extinction) : 0.0;
	if (mode == NRI_SMOKE_FIELD_DEBUG_THERMAL)
		return SmokeVisualTone(scalar.x > 1e-6 ? max(scalar.y / scalar.x, 0.0) : 0.0).xxx;
	if (mode == NRI_SMOKE_FIELD_DEBUG_GRADIENT_MAGNITUDE ||
		mode == NRI_SMOKE_FIELD_DEBUG_GRADIENT_ORIENTATION)
	{
		const float3 gradient = SmokeVisualExtinctionGradient(scalarCorners, blend, cellSize);
		const float magnitude = length(gradient);
		if (mode == NRI_SMOKE_FIELD_DEBUG_GRADIENT_MAGNITUDE)
			return SmokeVisualTone(magnitude * cellSize / max(extinction, 1e-6)).xxx;
		return magnitude > 1e-6 ? (normalize(gradient) * 0.5 + 0.5) *
			(1.0 - exp(-extinction * cellSize)) : 0.0;
	}
	return 0.0;
}

float3 SmokeVisualLobeDiagnostic(uint mode, float3 lobes[6], float confidence)
{
	if (mode == NRI_SMOKE_FIELD_DEBUG_LOBE_SUM)
	{
		float3 sum = 0.0;
		[unroll] for (uint lobe = 0u; lobe < 6u; ++lobe) sum += lobes[lobe];
		return min(sum * 0.05, 4.0);
	}
	if (mode == NRI_SMOKE_FIELD_DEBUG_LOBE_CONTRAST)
		return min(abs(lobes[0] - lobes[1]) + abs(lobes[2] - lobes[3]) +
			abs(lobes[4] - lobes[5]), 4.0);
	if (mode == NRI_SMOKE_FIELD_DEBUG_LOBE_CONFIDENCE)
		return saturate(confidence).xxx;
	if (mode == NRI_SMOKE_FIELD_DEBUG_LOBE_DIRECTION)
	{
		float luminance[6];
		[unroll] for (uint lobe = 0u; lobe < 6u; ++lobe)
			luminance[lobe] = dot(max(lobes[lobe], 0.0), float3(0.2126, 0.7152, 0.0722));
		const float3 directional = float3(luminance[0] - luminance[1],
			luminance[2] - luminance[3], luminance[4] - luminance[5]);
		const float strength = length(directional);
		return strength > 1e-6 ? (normalize(directional) * 0.5 + 0.5) *
			saturate(strength * 0.05) : 0.0;
	}
	return 0.0;
}

#endif
