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
uint SmokeVisualGradientPacked0() { return asuint(gSmokeConstants.IndirectScale); }
uint SmokeVisualGradientPacked1() { return gSmokeConstants.OutputHeight; }
uint SmokeVisualGradientPacked2() { return gSmokeConstants.DirectionalColorPacked; }
uint SmokeVisualRadiancePacked0() { return asuint(gSmokeConstants.DirectionalDirectionX); }
uint SmokeVisualRadiancePacked1() { return asuint(gSmokeConstants.DirectionalDirectionY); }
uint SmokeVisualRadiancePacked2() { return asuint(gSmokeConstants.DirectionalDirectionZ); }
uint SmokeVisualRadiancePacked3() { return asuint(gSmokeConstants.DirectionalAngularSize); }
uint SmokeVisualThicknessPacked0() { return gSmokeConstants.RuntimeLightTileCountX; }
uint SmokeVisualThicknessPacked1() { return gSmokeConstants.RuntimeLightTileCountY; }
uint SmokeVisualFlowPacked0() { return gSmokeConstants.ParticleCapacity; }
uint SmokeVisualFlowPacked1() { return gSmokeConstants.CommandCount; }
uint SmokeVisualFlowPacked2() { return gSmokeConstants.StyleCount; }
uint SmokeVisualIllustrationPacked0() { return gSmokeConstants.RuntimeLightCount; }
uint SmokeVisualIllustrationPacked1() { return gSmokeConstants.LightSamples; }

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

bool SmokeVisualRadianceEnabled()
{
	const float2 rim = SmokeVisualUnpackHalf2(SmokeVisualRadiancePacked0());
	const float2 shaping = SmokeVisualUnpackHalf2(SmokeVisualRadiancePacked1());
	const float desaturation = SmokeVisualUnpackHalf2(SmokeVisualRadiancePacked2()).x;
	return any(abs(rim) > 1e-6) || any(abs(shaping) > 1e-6) || desaturation > 1e-6;
}

bool SmokeVisualThicknessEnabled()
{
	return abs(SmokeVisualUnpackHalf2(SmokeVisualThicknessPacked0()).x) > 1e-6;
}

bool SmokeVisualFlowEnabled()
{
	const float2 flow = SmokeVisualUnpackHalf2(SmokeVisualFlowPacked0());
	const float2 curl = SmokeVisualUnpackHalf2(SmokeVisualFlowPacked1());
	const float compression = SmokeVisualUnpackHalf2(SmokeVisualFlowPacked2()).x;
	return flow.x > 1e-6 || curl.x > 1e-6 || abs(compression) > 1e-6;
}

bool SmokeVisualFlowBoundaryRequired()
{
	return SmokeVisualUnpackHalf2(SmokeVisualFlowPacked1()).x > 1e-6;
}

bool SmokeVisualIllustrationEnabled()
{
	const float band = SmokeVisualUnpackHalf2(SmokeVisualIllustrationPacked0()).x;
	const float contour = SmokeVisualUnpackHalf2(SmokeVisualIllustrationPacked1()).y;
	return band > 1e-6 || contour > 1e-6;
}

bool SmokeVisualBoundaryRequired()
{
	return SmokeVisualGradientEnabled() || SmokeVisualRadianceEnabled() ||
		SmokeVisualFlowBoundaryRequired();
}

float3 SmokeVisualExtinctionGradient(float4 scalarCorners[8], float3 blend,
	float cellSize);

void SmokeVisualBoundarySample(float4 scalarCorners[8], float3 blend, float cellSize,
	float extinction, float3 viewRay, out float edge, out float facing,
	out float3 outward, out float interior, out float densityWeight)
{
	edge = 0.0;
	facing = 0.0;
	outward = 0.0;
	interior = 0.0;
	densityWeight = 0.0;
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
	if (delta > 1e-6)
	{
		const float3 gradientDirection = normalize(gradient);
		facing = edge * saturate(dot(gradientDirection, viewRay));
		outward = -gradientDirection;
	}
	float cornerAverage = 0.0;
	[unroll]
	for (uint corner = 0u; corner < 8u; ++corner)
		cornerAverage += max(scalarCorners[corner].z * gSmokeConstants.DensityScale, 0.0);
	cornerAverage *= 0.125;
	const float relativeCavity = cornerAverage > 1e-6 ?
		saturate((cornerAverage - extinction) / cornerAverage) : 0.0;
	const float cavity = smoothstep(0.10, 0.50, relativeCavity);
	densityWeight = SmokeVisualDensityColorWeight(extinction);
	const float middleBand = 4.0 * densityWeight * (1.0 - densityWeight);
	const float crack = max(cavity, edge * middleBand);
	interior = saturate(densityWeight * (1.0 - crack));
}

float SmokeVisualLuminance(float3 color)
{
	return dot(color, float3(0.2126, 0.7152, 0.0722));
}

float SmokeVisualEvidenceGate(float value, float pivot)
{
	const float low = saturate(pivot);
	const float high = min(low + 0.25, 1.0);
	return high > low + 1e-6 ? smoothstep(low, high, saturate(value)) :
		(value >= low ? 1.0 : 0.0);
}

float3 SmokeVisualShapeWorldIncident(float3 rawLobes[6], float confidence,
	float3 viewRay, float3 outward, float edge, float interior, float densityWeight,
	float3 baseIncident)
{
	const float3 safeBase = max(all(isfinite(baseIncident)) ? baseIncident : 0.0, 0.0);
	if (!SmokeVisualRadianceEnabled() || !any(safeBase > 0.0))
		return safeBase;

	float3 lobeMeans[6];
	float luminance[6];
	float totalLuminance = 0.0;
	[unroll]
	for (uint lobe = 0u; lobe < 6u; ++lobe)
	{
		lobeMeans[lobe] = max(all(isfinite(rawLobes[lobe])) ? rawLobes[lobe] : 0.0, 0.0);
		luminance[lobe] = max(SmokeVisualLuminance(lobeMeans[lobe]), 0.0);
		totalLuminance += luminance[lobe];
	}
	const float3 directional = float3(luminance[0] - luminance[1],
		luminance[2] - luminance[3], luminance[4] - luminance[5]);
	const float directionalLength = length(directional);
	const float directionality = totalLuminance > 1e-6 ?
		saturate(directionalLength / totalLuminance) : 0.0;
	const float3 dominantDirection = directionalLength > 1e-6 ?
		directional / directionalLength : 0.0;

	const float2 denseAndConfidence = SmokeVisualUnpackHalf2(SmokeVisualRadiancePacked2());
	const float2 directionalitySettings = SmokeVisualUnpackHalf2(SmokeVisualRadiancePacked3());
	const float confidenceGate = SmokeVisualEvidenceGate(confidence, denseAndConfidence.y);
	const float directionGate = SmokeVisualEvidenceGate(directionality, directionalitySettings.x);
	const float evidenceGate = confidenceGate * directionGate;
	const float lightFacing = saturate(dot(outward, dominantDirection));
	const float backlit = saturate(dot(dominantDirection, viewRay));
	const float rimMask = saturate(edge * evidenceGate * sqrt(lightFacing * backlit));

	const float2 rim = saturate(SmokeVisualUnpackHalf2(SmokeVisualRadiancePacked0()));
	const float2 shaping = saturate(SmokeVisualUnpackHalf2(SmokeVisualRadiancePacked1()));
	float3 incident = safeBase * saturate(1.0 - shaping.y * confidenceGate * interior);
	const float incidentLuminance = max(SmokeVisualLuminance(incident), 0.0);
	incident = lerp(incident, incidentLuminance.xxx,
		saturate(denseAndConfidence.x) * confidenceGate * saturate(densityWeight));

	float3 dominantColor = 0.0;
	[unroll]
	for (uint axis = 0u; axis < 6u; ++axis)
		dominantColor += lobeMeans[axis] * max(dot(
			(float3)NRI_SMOKE_GRID_LIGHT_LOBE_AXES[axis], dominantDirection), 0.0);
	const float currentLuminance = max(SmokeVisualLuminance(incident), 0.0);
	const float dominantLuminance = max(SmokeVisualLuminance(dominantColor), 0.0);
	const float3 redistributed = dominantLuminance > 1e-6 ?
		dominantColor * (currentLuminance / dominantLuminance) : incident;
	const float rimBlend = saturate(rim.x * rimMask);
	const float edgeBlend = saturate(shaping.x * edge * evidenceGate);
	const float combinedBlend = 1.0 - (1.0 - rimBlend) * (1.0 - edgeBlend);
	incident = lerp(incident, redistributed, saturate(combinedBlend));
	incident *= 1.0 + rim.y * rimMask;
	return max(all(isfinite(incident)) ? incident : 0.0, 0.0);
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

void SmokeVisualApplyIllustration(inout float extinction,
	inout float3 scattering, inout float3 source)
{
	if (!SmokeVisualIllustrationEnabled() || extinction <= 1e-6)
		return;
	const float2 bandSettings = SmokeVisualUnpackHalf2(SmokeVisualIllustrationPacked0());
	const float2 contourSettings = SmokeVisualUnpackHalf2(SmokeVisualIllustrationPacked1());
	const float bandStrength = saturate(bandSettings.x);
	const float bandCount = clamp(round(bandSettings.y), 2.0, 9.0);
	const float softness = clamp(contourSettings.x, 0.02, 0.49);
	const float contourStrength = saturate(contourSettings.y);
	const float scaled = log2(max(extinction, 1e-6)) * bandCount;
	const float level = floor(scaled);
	const float fraction = frac(scaled);
	const float softLevel = level + smoothstep(0.5 - softness, 0.5 + softness, fraction);
	const float bandedExtinction = exp2(softLevel / bandCount);
	const float baseExtinction = extinction;
	extinction = max(lerp(baseExtinction, bandedExtinction, bandStrength), 0.0);
	float3 remappedScattering;
	remappedScattering.x = SmokeVisualShapeScatteringChannel(baseExtinction,
		extinction, scattering.x, 1.0);
	remappedScattering.y = SmokeVisualShapeScatteringChannel(baseExtinction,
		extinction, scattering.y, 1.0);
	remappedScattering.z = SmokeVisualShapeScatteringChannel(baseExtinction,
		extinction, scattering.z, 1.0);
	const float3 incident = float3(
		SmokeVisualIncidentChannel(source.x, scattering.x),
		SmokeVisualIncidentChannel(source.y, scattering.y),
		SmokeVisualIncidentChannel(source.z, scattering.z));
	scattering = remappedScattering;
	source = max(all(isfinite(incident * scattering)) ? incident * scattering : 0.0, 0.0);
	const float contour = 1.0 - smoothstep(0.0, softness, abs(fraction - 0.5));
	SmokeVisualApplyScatteringTint(extinction,
		(1.0 - contourStrength * contour).xxx, scattering, source);
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

void SmokeVisualFlowSample(float4 scalarCorners[8], float4 velocityCorners[8],
	float4 scalar, float4 velocity, float3 blend, float cellSize, float3 viewRay,
	float edge, uint dormantMask, out float sourceGain, out float compressionStops)
{
	sourceGain = 1.0;
	compressionStops = 0.0;
	if (!SmokeVisualFlowEnabled() || scalar.x <= 1e-6)
		return;
	const float2 flowSettings = SmokeVisualUnpackHalf2(SmokeVisualFlowPacked0());
	const float2 curlSettings = SmokeVisualUnpackHalf2(SmokeVisualFlowPacked1());
	const float2 compressionSettings = SmokeVisualUnpackHalf2(SmokeVisualFlowPacked2());
	const float speed = length(velocity.xyz);
	const float3 flowDirection = speed > 1e-6 ? velocity.xyz / speed : 0.0;
	const float speedGate = saturate(speed / max(flowSettings.y, 1.0));
	const float sideFacing = 1.0 - abs(dot(flowDirection, viewRay));
	float highlight = saturate(flowSettings.x) * speedGate * sideFacing * sideFacing;

	float3 values[8];
	float supportCount = 0.0;
	[unroll]
	for (uint corner = 0u; corner < 8u; ++corner)
	{
		const bool supported = scalarCorners[corner].x > 1e-6;
		values[corner] = supported ? velocityCorners[corner].xyz : velocity.xyz;
		supportCount += supported ? 1.0 : 0.0;
	}
	const float3 dx0 = lerp(values[1] - values[0], values[3] - values[2], blend.y);
	const float3 dx1 = lerp(values[5] - values[4], values[7] - values[6], blend.y);
	const float3 dy0 = lerp(values[2] - values[0], values[3] - values[1], blend.x);
	const float3 dy1 = lerp(values[6] - values[4], values[7] - values[5], blend.x);
	const float3 dz0 = lerp(values[4] - values[0], values[5] - values[1], blend.x);
	const float3 dz1 = lerp(values[6] - values[2], values[7] - values[3], blend.x);
	const float3 dvDx = lerp(dx0, dx1, blend.z) / max(cellSize, 1e-6);
	const float3 dvDy = lerp(dy0, dy1, blend.z) / max(cellSize, 1e-6);
	const float3 dvDz = lerp(dz0, dz1, blend.y) / max(cellSize, 1e-6);
	const bool mixedAuthority = dormantMask != 0u && dormantMask != 0xffu;
	const float derivativeGate = mixedAuthority ? 0.0 : smoothstep(2.0, 6.0, supportCount);
	const float3 curl = float3(dvDy.z - dvDz.y, dvDz.x - dvDx.z,
		dvDx.y - dvDy.x) * derivativeGate;
	const float divergence = (dvDx.x + dvDy.y + dvDz.z) * derivativeGate;
	const float turbulenceScale = max(velocity.w / max(scalar.x, 1e-6), 0.0);
	const float turbulenceEvidence = saturate(turbulenceScale / max(cellSize, 1e-6));
	const float curlGate = saturate(length(curl) / max(curlSettings.y, 0.01));
	highlight += saturate(curlSettings.x) * curlGate * lerp(0.5, 1.0,
		turbulenceEvidence) * lerp(0.25, 1.0, saturate(edge));
	sourceGain = exp2(clamp(isfinite(highlight) ? highlight : 0.0, 0.0, 1.0));
	const float normalizedCompression = clamp(-divergence /
		max(compressionSettings.y, 0.01), -1.0, 1.0);
	compressionStops = clamp(isfinite(normalizedCompression) ?
		normalizedCompression * compressionSettings.x : 0.0, -1.0, 1.0);
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
