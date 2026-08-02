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

float3 SmokeVisualUnpackLinearColor(uint packed, float maximum)
{
	return float3(packed & 31u, (packed >> 5u) & 31u,
		(packed >> 10u) & 31u) * (maximum / 31.0);
}

float2 SmokeVisualUnpackHalf2(uint packed)
{
	return float2(f16tof32(packed & 0xffffu), f16tof32(packed >> 16u));
}

uint SmokeVisualPacked0() { return asuint(gSmokeConstants.TimeScale); }
uint SmokeVisualPacked1() { return asuint(gSmokeConstants.Wind.x); }
uint SmokeVisualPacked2() { return asuint(gSmokeConstants.Wind.y); }
uint SmokeVisualPacked3() { return asuint(gSmokeConstants.Wind.z); }
uint SmokeVisualThermalPacked0() { return asuint(gSmokeConstants.CurrentJitter.x); }
uint SmokeVisualThermalPacked1() { return asuint(gSmokeConstants.CurrentJitter.y); }
uint SmokeVisualGradientPacked0() { return gSmokeConstants.OutputWidth; }
uint SmokeVisualGradientPacked1() { return gSmokeConstants.OutputHeight; }
uint SmokeVisualGradientPacked2() { return gSmokeConstants.DirectionalColorPacked; }

float SmokeVisualDensityColorWeight(float extinction)
{
	const float colorPivot = max(SmokeVisualUnpackHalf2(SmokeVisualPacked2()).y, 0.0);
	const float colorWidth = colorPivot * 0.5;
	const float colorLow = max(colorPivot - colorWidth, 0.0);
	const float colorHigh = max(colorPivot + colorWidth, colorLow + 1e-6);
	return smoothstep(colorLow, colorHigh, extinction);
}

float SmokeVisualShapeExtinction(float baseExtinction)
{
	const float base = max(isfinite(baseExtinction) ? baseExtinction : 0.0, 0.0);
	if (base <= 0.0)
		return 0.0;

	const float2 toe = SmokeVisualUnpackHalf2(SmokeVisualPacked0());
	const float2 middle = SmokeVisualUnpackHalf2(SmokeVisualPacked1());
	const float2 shoulderControls = SmokeVisualUnpackHalf2(SmokeVisualPacked2());
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

float SmokeVisualShapeScatteringChannel(float baseExtinction, float extinction,
	float baseScattering, float tint)
{
	if (baseExtinction <= 0.0 || extinction <= 0.0 || baseScattering <= 0.0 || tint <= 0.0)
		return 0.0;
	const float candidate = extinction * (baseScattering / baseExtinction) * tint;
	return isfinite(candidate) ? clamp(candidate, 0.0, extinction) : extinction;
}

float SmokeVisualIncidentChannel(float source, float scattering)
{
	if (source <= 0.0 || scattering <= 0.0)
		return 0.0;
	const float incident = source / scattering;
	return isfinite(incident) ? max(incident, 0.0) : 0.0;
}

void SmokeVisualShapeMedium(float baseExtinction, float shapingExtinction, float3 baseScattering,
	float3 baseSource, out float extinction, out float3 scattering, out float3 source)
{
	extinction = SmokeVisualShapeExtinction(shapingExtinction);
	const float colorWeight = SmokeVisualDensityColorWeight(extinction);
	const uint packedColors = SmokeVisualPacked3();
	const float3 tint = lerp(SmokeVisualUnpackColor(packedColors),
		SmokeVisualUnpackColor(packedColors >> 15u), colorWeight);
	const float3 safeScattering = max(all(isfinite(baseScattering)) ? baseScattering : 0.0, 0.0);
	const float3 safeTint = max(tint, 0.0);
	scattering = float3(
		SmokeVisualShapeScatteringChannel(baseExtinction, extinction, safeScattering.x, safeTint.x),
		SmokeVisualShapeScatteringChannel(baseExtinction, extinction, safeScattering.y, safeTint.y),
		SmokeVisualShapeScatteringChannel(baseExtinction, extinction, safeScattering.z, safeTint.z));
	const float3 safeSource = max(all(isfinite(baseSource)) ? baseSource : 0.0, 0.0);
	const float3 incident = float3(
		SmokeVisualIncidentChannel(safeSource.x, safeScattering.x),
		SmokeVisualIncidentChannel(safeSource.y, safeScattering.y),
		SmokeVisualIncidentChannel(safeSource.z, safeScattering.z));
	// This is algebraically baseSource * (shapedScattering/baseScattering),
	// but avoids forming a huge ratio for tiny positive carrier coefficients.
	source = incident * scattering;
	source = max(all(isfinite(source)) ? source : 0.0, 0.0);
}

void SmokeVisualApplyScatteringTint(float extinction, float3 tint,
	inout float3 scattering, inout float3 source)
{
	const float3 incident = float3(
		SmokeVisualIncidentChannel(source.x, scattering.x),
		SmokeVisualIncidentChannel(source.y, scattering.y),
		SmokeVisualIncidentChannel(source.z, scattering.z));
	const float3 safeTint = max(all(isfinite(tint)) ? tint : 1.0, 0.0);
	scattering = min(max(scattering * safeTint, 0.0), extinction.xxx);
	source = max(all(isfinite(incident * scattering)) ? incident * scattering : 0.0, 0.0);
}

bool SmokeVisualGradientEnabled()
{
	const float2 sculpt = SmokeVisualUnpackHalf2(SmokeVisualGradientPacked1());
	const float tintStrength = f16tof32(SmokeVisualGradientPacked2() & 0xffffu);
	return any(abs(sculpt) > 1e-6) || tintStrength > 1e-6;
}

float3 SmokeVisualExtinctionGradient(float4 scalarCorners[8], float3 blend,
	float cellSize);

void SmokeVisualGradientSample(float4 scalarCorners[8], float3 blend, float cellSize,
	float extinction, float3 viewRay, out float edge, out float facing)
{
	edge = 0.0;
	facing = 0.0;
	if (extinction <= 1e-6)
		return;
	const float3 gradient = SmokeVisualExtinctionGradient(scalarCorners, blend, cellSize);
	const float delta = length(gradient) * cellSize;
	const float denominator = max(max(extinction, delta * 0.25), 1e-6);
	const float relativeGradient = delta / denominator;
	const float2 selection = SmokeVisualUnpackHalf2(SmokeVisualGradientPacked0());
	const float low = max(selection.x - selection.y, 0.0);
	const float high = max(selection.x + selection.y, low + 1e-6);
	edge = smoothstep(low, high, relativeGradient);
	facing = delta > 1e-6 ? edge * saturate(dot(normalize(gradient), viewRay)) : 0.0;
}

float SmokeVisualSculptExtinction(float baseExtinction, float edge)
{
	if (!SmokeVisualGradientEnabled() || baseExtinction <= 0.0)
		return baseExtinction;
	const float2 sculpt = SmokeVisualUnpackHalf2(SmokeVisualGradientPacked1());
	const float densityWeight = SmokeVisualDensityColorWeight(baseExtinction);
	const float middleBand = 4.0 * densityWeight * (1.0 - densityWeight);
	const float stops = edge * (sculpt.x + sculpt.y * middleBand);
	const float factor = exp2(clamp(stops, -2.0, 2.0));
	const float sculpted = baseExtinction * factor;
	return isfinite(sculpted) ? max(sculpted, 0.0) : 0.0;
}

void SmokeVisualApplyGradientTint(float facing, float extinction,
	inout float3 scattering, inout float3 source)
{
	if (!SmokeVisualGradientEnabled() || extinction <= 0.0)
		return;
	const uint packedTint = SmokeVisualGradientPacked2();
	const float tintStrength = max(f16tof32(packedTint & 0xffffu), 0.0);
	const float3 tintColor = SmokeVisualUnpackColor(packedTint >> 16u);
	SmokeVisualApplyScatteringTint(extinction,
		lerp(1.0, tintColor, saturate(facing * tintStrength)), scattering, source);
}

void SmokeVisualApplyThermal(float thermal, float extinction,
	inout float3 scattering, inout float3 source, out bool emitted)
{
	emitted = false;
	if (extinction <= 0.0)
		return;
	const uint packedColors = SmokeVisualThermalPacked1();
	const float3 tintColor = SmokeVisualUnpackColor(packedColors);
	const float3 glow = SmokeVisualUnpackLinearColor(packedColors >> 15u, 4.0);
	if (all(abs(tintColor - 1.0) <= 1e-6) && !any(glow > 0.0))
		return;
	const float2 knots = SmokeVisualUnpackHalf2(SmokeVisualThermalPacked0());
	const float low = max(knots.x, 0.0);
	const float high = max(knots.y, low + 1e-4);
	const float heat = smoothstep(low, high, max(isfinite(thermal) ? thermal : 0.0, 0.0));
	if (heat <= 0.0)
		return;
	SmokeVisualApplyScatteringTint(extinction, lerp(1.0, tintColor, heat), scattering, source);
	const float3 glowSource = extinction * heat * min(max(glow, 0.0), 4.0);
	emitted = any(glowSource > 0.0);
	source = max(all(isfinite(source + glowSource)) ? source + glowSource : 0.0, 0.0);
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
