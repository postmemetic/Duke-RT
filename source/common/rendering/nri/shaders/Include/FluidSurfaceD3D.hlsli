#ifndef NRI_FLUID_SURFACE_D3D_HLSLI
#define NRI_FLUID_SURFACE_D3D_HLSLI

#include "FluidSurfaceContracts.hlsli"

#define NRI_FLUID_FIELD_LAYER_CAP NRI_FLUID_MAX_LAYERS

struct FluidSurfaceEvaluation
{
	uint active;
	uint bindingIndex;
	uint activeLayerCount;
	uint underside;
	float3 samplePosition;
	float virtualOffset;
	float3 carrierNormal;
	float worldFootprint;
	float3 shadingNormal;
	float roughness;
	float3 baseColor;
	float reactive;
	float3 emission;
	float height;
};

float FluidHash01(uint value)
{
	value ^= value >> 16u;
	value *= 0x7feb352du;
	value ^= value >> 15u;
	value *= 0x846ca68bu;
	value ^= value >> 16u;
	return (float)(value & 0x00ffffffu) * (1.0 / 16777216.0);
}

float3 FluidSafeNormalize(float3 value, float3 fallback)
{
	const float lengthSq = dot(value, value);
	return lengthSq > 1.0e-10 && all(isfinite(value)) ?
		value * rsqrt(lengthSq) : fallback;
}

float3 FluidFaceCarrierNormalToDirection(float3 carrierNormal, float3 direction)
{
	const float3 normal = FluidSafeNormalize(
		carrierNormal, float3(0.0, 0.0, 1.0));
	return dot(normal, direction) >= 0.0 ? normal : -normal;
}

uint FluidHashUint(uint value)
{
	value ^= value >> 16u;
	value *= 0x7feb352du;
	value ^= value >> 15u;
	value *= 0x846ca68bu;
	value ^= value >> 16u;
	return value;
}

float FluidHashCellSigned(int2 cell, uint seed)
{
	uint value = seed ^ ((uint)cell.x * 0x8da6b343u) ^
		((uint)cell.y * 0xd8163841u);
	value = FluidHashUint(value);
	return (float)(value & 0x00ffffffu) * (2.0 / 16777216.0) - 1.0;
}

float FluidValueNoise(float2 p, uint seed)
{
	const int2 cell = (int2)floor(p);
	const float2 f = frac(p);
	const float2 interpolation = f * f * (3.0 - 2.0 * f);
	const float h00 = FluidHashCellSigned(cell + int2(0, 0), seed);
	const float h10 = FluidHashCellSigned(cell + int2(1, 0), seed);
	const float h01 = FluidHashCellSigned(cell + int2(0, 1), seed);
	const float h11 = FluidHashCellSigned(cell + int2(1, 1), seed);
	return lerp(
		lerp(h00, h10, interpolation.x),
		lerp(h01, h11, interpolation.x),
		interpolation.y);
}

float2 FluidRotate2(float2 value, float cosine, float sine)
{
	return float2(
		cosine * value.x - sine * value.y,
		sine * value.x + cosine * value.y);
}

// Returns a centered wave and its analytic derivative with respect to phase.
// Choppiness adds a second harmonic that narrows crests without the old
// abs(wave) ridge cusp and its repeating line-followed-by-trough artifact.
float2 FluidChoppyWaveDerivative(float phase, float choppiness)
{
	float sine;
	float cosine;
	sincos(phase, sine, cosine);
	const float choppyAmount = saturate((choppiness - 1.0) * (1.0 / 3.0));
	const float sineDouble = 2.0 * sine * cosine;
	const float cosineDouble = cosine * cosine - sine * sine;
	const float normalization = 1.0 + 0.5 * choppyAmount;
	return float2(
		(sine - 0.5 * choppyAmount * cosineDouble) / normalization,
		(cosine + choppyAmount * sineDouble) / normalization);
}

float2 FluidInitialFieldDirection(FluidBindingData binding)
{
	const float2 flowVelocity = float2(
		binding.domainAxisUFlowU.w, binding.domainAxisVFlowV.w);
	if ((binding.styleFlags & NRI_FLUID_STYLE_FALL) != 0u &&
		dot(flowVelocity, flowVelocity) > 1.0e-6)
	{
		return normalize(float2(-flowVelocity.y, flowVelocity.x));
	}
	if ((binding.styleFlags & NRI_FLUID_STYLE_FLOW) != 0u &&
		dot(flowVelocity, flowVelocity) > 1.0e-6)
	{
		return normalize(flowVelocity);
	}
	const float initialAngle = 6.28318530718 * FluidHash01(binding.seed ^ 0x4f1bbcdcu);
	return float2(cos(initialAngle), sin(initialAngle));
}

// Evaluate a deterministic normalized field. Height and the phase-gradient
// approximation use the same filtered octave weights. The gradient is with
// respect to the dimensionless authored domain; the caller applies world scale
// and authored amplitude.
void FluidEvaluateField(
	FluidBindingData binding,
	float2 domain,
	float gameplaySeconds,
	float worldFootprint,
	uint layerCap,
	out float heightRatio,
	out float2 gradientRatio,
	out float crest)
{
	heightRatio = 0.0;
	gradientRatio = 0.0;
	crest = 0.0;
	const uint layerCount = min(
		min(max(binding.layerCount, 1u), max(layerCap, 1u)),
		NRI_FLUID_FIELD_LAYER_CAP);
	const float domainScale = max(abs(binding.domainOriginScale.w), 1.0e-5);
	float frequency = max(abs(binding.spectrum.y), 1.0e-4);
	const float lacunarity = clamp(abs(binding.spectrum.z), 1.01, 4.0);
	const float gain = saturate(abs(binding.spectrum.w));
	const float anisotropy = clamp(binding.motion.z, 0.2, 5.0);
	const float warp = clamp(binding.motion.w, 0.0, 4.0);
	const float noiseScale = clamp(binding.shaping.z, 0.05, 4.0);
	const float choppyRelaxation = saturate(binding.shaping.y);
	float layerChoppiness = clamp(binding.shaping.x, 0.05, 4.0);
	const float rotationCosine = cos(binding.shaping.w);
	const float rotationSine = sin(binding.shaping.w);
	float2 direction = FluidInitialFieldDirection(binding);
	float amplitudeWeight = 1.0;
	float filteredWeightSum = 0.0;

	// Keep final shading bounded without duplicating four octave bodies in the
	// already large TraceOpaque variants.
	[loop]
	for (uint layer = 0u; layer < NRI_FLUID_FIELD_LAYER_CAP; ++layer)
	{
		if (layer >= layerCount)
		{
			break;
		}
		const uint layerSeed = binding.seed + layer * 0x9e3779b9u;
		const float2 crossDirection = float2(-direction.y, direction.x);
		const float2 layerDomain = float2(
			dot(domain, direction) * anisotropy,
			dot(domain, crossDirection));
		const float layerTime = gameplaySeconds * binding.motion.x *
			(1.0 + 0.17 * (float)layer);
		const float2 noiseCoordinate = layerDomain * (frequency * noiseScale) +
			float2(layerTime * 0.13, -layerTime * 0.09);
		const float noise = FluidValueNoise(noiseCoordinate, layerSeed);
		const float phase = layerDomain.x * frequency + warp * noise + layerTime +
			6.28318530718 * FluidHash01(layerSeed ^ 0xa511e9b3u);
		const float2 wave = FluidChoppyWaveDerivative(phase, layerChoppiness);
		// The wave derivative remains analytic. Omitting the derivative of the
		// low-amplitude noise warp keeps the large TraceOpaque variants within a
		// practical compile budget while retaining warped crest placement.
		const float2 phaseGradientDomain = frequency * direction * anisotropy;
		const float gradientBound = frequency * max(anisotropy, 1.0) *
			(1.0 + 3.0 * warp * noiseScale);
		const float filterWidth = max(worldFootprint, 0.0) * domainScale * gradientBound;
		const float layerWeight = amplitudeWeight / (1.0 + filterWidth * filterWidth);
		heightRatio += layerWeight * wave.x;
		gradientRatio += layerWeight * wave.y * phaseGradientDomain;
		crest += layerWeight * saturate(wave.x * 0.5 + 0.5);
		filteredWeightSum += layerWeight;
		frequency *= lacunarity;
		amplitudeWeight *= gain;
		layerChoppiness = lerp(layerChoppiness, 1.0, choppyRelaxation);
		direction = FluidRotate2(direction, rotationCosine, rotationSine);
	}

	if (filteredWeightSum > 1.0e-5)
	{
		heightRatio /= filteredWeightSum;
		gradientRatio /= filteredWeightSum;
		crest /= filteredWeightSum;
	}
	if (!isfinite(heightRatio) || !all(isfinite(gradientRatio)) || !isfinite(crest))
	{
		heightRatio = 0.0;
		gradientRatio = 0.0;
		crest = 0.0;
	}
}

// Phase 2C uses the low-frequency prefix of the same authored field. Keeping
// this prefix explicit avoids a nested variable-bound field loop in the large
// radiance-cache TraceOpaque shader variants.
void FluidEvaluateVirtualField(
	FluidBindingData binding,
	float2 domain,
	float gameplaySeconds,
	out float heightRatio)
{
	const float frequency = max(abs(binding.spectrum.y), 1.0e-4);
	const float anisotropy = clamp(binding.motion.z, 0.2, 5.0);
	const float warp = clamp(binding.motion.w, 0.0, 4.0);
	const float noiseScale = clamp(binding.shaping.z, 0.05, 4.0);
	const float layerChoppiness = clamp(binding.shaping.x, 0.05, 4.0);
	const float2 direction = FluidInitialFieldDirection(binding);
	const float2 crossDirection = float2(-direction.y, direction.x);
	const float2 layerDomain = float2(
		dot(domain, direction) * anisotropy,
		dot(domain, crossDirection));
	const float layerTime = gameplaySeconds * binding.motion.x;
	const float2 noiseCoordinate = layerDomain * (frequency * noiseScale) +
		float2(layerTime * 0.13, -layerTime * 0.09);
	const float noise = FluidValueNoise(noiseCoordinate, binding.seed);
	const float phase = layerDomain.x * frequency + warp * noise + layerTime +
		6.28318530718 * FluidHash01(binding.seed ^ 0xa511e9b3u);
	heightRatio = FluidChoppyWaveDerivative(phase, layerChoppiness).x;
	if (!isfinite(heightRatio))
	{
		heightRatio = 0.0;
	}
}

bool TryEvaluateFluidSurface(
	MaterialData material,
	float3 carrierPosition,
	float3 geometricNormal,
	float3 viewRayDirection,
	float3 sampledBaseColor,
	float worldFootprint,
	bool allowVirtualHeight,
	out FluidSurfaceEvaluation evaluation)
{
	evaluation = (FluidSurfaceEvaluation)0;
	evaluation.samplePosition = carrierPosition;
	evaluation.carrierNormal = FluidSafeNormalize(
		geometricNormal, float3(0.0, 0.0, 1.0));
	evaluation.shadingNormal = evaluation.carrierNormal;
	evaluation.baseColor = sampledBaseColor;
	evaluation.roughness = material.roughnessHint;
	evaluation.bindingIndex = material.fluidBindingIndex;
	evaluation.worldFootprint = worldFootprint;
	if (material.fluidBindingIndex == NRI_FLUID_INVALID_BINDING)
	{
		return false;
	}

	uint frameCount = 0u;
	uint frameStride = 0u;
	uint bindingCount = 0u;
	uint bindingStride = 0u;
	gFluidFrames.GetDimensions(frameCount, frameStride);
	gFluidBindings.GetDimensions(bindingCount, bindingStride);
	if (frameCount == 0u || material.fluidBindingIndex >= bindingCount)
	{
		return false;
	}
	const FluidFrameData frame = gFluidFrames[0u];
	const FluidBindingData binding = gFluidBindings[material.fluidBindingIndex];
	if ((frame.flags & NRI_FLUID_FRAME_FLAG_ENABLED) == 0u ||
		(binding.flags & NRI_FLUID_BINDING_FLAG_ENABLED) == 0u)
	{
		return false;
	}

	float3 carrierNormal = FluidSafeNormalize(
		geometricNormal, float3(0.0, 0.0, 1.0));
	const float3 absoluteNormal = abs(carrierNormal);
	const float dominant = absoluteNormal.x >= absoluteNormal.y && absoluteNormal.x >= absoluteNormal.z ? carrierNormal.x :
		(absoluteNormal.y >= absoluteNormal.z ? carrierNormal.y : carrierNormal.z);
	if (dominant < 0.0)
	{
		carrierNormal = -carrierNormal;
	}
	if ((binding.flags & NRI_FLUID_BINDING_FLAG_FLIP_SIDE) != 0u)
	{
		carrierNormal = -carrierNormal;
	}
	const float3 viewDir = FluidSafeNormalize(-viewRayDirection, carrierNormal);
	const bool underside = dot(carrierNormal, viewDir) < 0.0;
	if (underside && (binding.flags & NRI_FLUID_BINDING_FLAG_TWO_SIDED) == 0u)
	{
		return false;
	}

	float3 requestedU = binding.domainAxisUFlowU.xyz;
	requestedU -= carrierNormal * dot(requestedU, carrierNormal);
	if (dot(requestedU, requestedU) <= 1.0e-8)
	{
		const float3 fallbackUp = abs(carrierNormal.z) < 0.95 ?
			float3(0.0, 0.0, 1.0) : float3(0.0, 1.0, 0.0);
		requestedU = cross(fallbackUp, carrierNormal);
	}
	const float3 axisU = normalize(requestedU);
	float3 requestedV = binding.domainAxisVFlowV.xyz;
	requestedV -= carrierNormal * dot(requestedV, carrierNormal) +
		axisU * dot(requestedV, axisU);
	float3 axisV = dot(requestedV, requestedV) > 1.0e-8 ?
		normalize(requestedV) : normalize(cross(carrierNormal, axisU));
	if (dot(cross(axisU, axisV), carrierNormal) < 0.0)
	{
		axisV = -axisV;
	}

	const float domainScale = max(abs(binding.domainOriginScale.w), 1.0e-5);
	const float timeScale = frame.globalTimeScale * binding.motion.y;
	const float gameplaySeconds = frame.currentGameplaySeconds * timeScale;
	const float3 localPosition = carrierPosition - binding.domainOriginScale.xyz;
	float2 domain = float2(dot(localPosition, axisU), dot(localPosition, axisV)) * domainScale;
	if ((binding.styleFlags & NRI_FLUID_STYLE_FLOW) != 0u)
	{
		domain -= float2(
			binding.domainAxisUFlowU.w,
			binding.domainAxisVFlowV.w) * gameplaySeconds;
	}
	if ((binding.styleFlags & NRI_FLUID_STYLE_FALL) != 0u)
	{
		domain.y += binding.domainAxisVFlowV.w *
			(gameplaySeconds + 0.2 * sin(gameplaySeconds * 0.73 + (float)(binding.seed & 31u)));
	}

	const uint layerCap = clamp(frame.primaryLayerCap, 1u, NRI_FLUID_MAX_LAYERS);
	float virtualOffset = 0.0;
	const float3 normalizedViewRayDirection = FluidSafeNormalize(viewRayDirection, -viewDir);
	const float signedViewCosine = dot(normalizedViewRayDirection, carrierNormal);
	const float minimumCosine = max(abs(binding.virtualParams.w), 0.08);
	const bool useVirtualHeight = allowVirtualHeight &&
		(frame.flags & NRI_FLUID_FRAME_FLAG_VIRTUAL_HEIGHT) != 0u &&
		(binding.flags & NRI_FLUID_BINDING_FLAG_DISABLE_VIRTUAL_HEIGHT) == 0u &&
		abs(signedViewCosine) >= minimumCosine && binding.virtualParams.x > 0.0;
	if (useVirtualHeight)
	{
		// Solve against the low-frequency prefix of the same deterministic field
		// used for final shading. The accepted triangle hit remains authoritative.
		const float virtualHalfHeight = max(binding.virtualParams.x, 0.0);
		const float travelBound = max(binding.virtualParams.y, 0.0);
		const float maximumTravel = min(
			travelBound,
			virtualHalfHeight / max(abs(signedViewCosine), minimumCosine));
		float virtualHeightRatio = 0.0;
		FluidEvaluateVirtualField(
			binding, domain, gameplaySeconds, virtualHeightRatio);
		const float targetHeight = virtualHeightRatio * virtualHalfHeight;
		const float targetOffset = clamp(
			targetHeight / signedViewCosine, -maximumTravel, maximumTravel);
		virtualOffset = isfinite(targetOffset) ? targetOffset : 0.0;
	}

	const float3 samplePosition = carrierPosition + normalizedViewRayDirection * virtualOffset;
	const float3 sampleLocal = samplePosition - binding.domainOriginScale.xyz;
	float2 sampleDomain = float2(dot(sampleLocal, axisU), dot(sampleLocal, axisV)) * domainScale;
	sampleDomain += domain - float2(dot(localPosition, axisU), dot(localPosition, axisV)) * domainScale;
	float heightRatio = 0.0;
	float2 gradientRatio = 0.0;
	float crest = 0.0;
	FluidEvaluateField(
		binding, sampleDomain, gameplaySeconds, worldFootprint, layerCap,
		heightRatio, gradientRatio, crest);
	const float amplitude = max(binding.spectrum.x, 0.0) * frame.globalAmplitudeScale;
	const float height = heightRatio * amplitude;
	float2 gradient = gradientRatio * (amplitude * domainScale);
	const float maxSlope = max(binding.surfaceResponse.w, 0.0);
	const float slope = length(gradient);
	if (maxSlope > 0.0 && slope > maxSlope)
	{
		gradient *= maxSlope / slope;
	}
	const float signal = heightRatio;

	const float normalStrength = max(binding.surfaceResponse.z, 0.0);
	float3 shadingNormal = normalize(carrierNormal - normalStrength *
		(gradient.x * axisU + gradient.y * axisV));
	if (dot(shadingNormal, viewDir) < 0.0)
	{
		shadingNormal = -shadingNormal;
	}
	const float4 sideColor = underside ?
		binding.undersideColorBaseMix : binding.topColorBaseMix;
	const float3 terminalBody = lerp(
		sideColor.rgb, sampledBaseColor, saturate(sideColor.a));
	float3 baseColor = lerp(
		sampledBaseColor, terminalBody, saturate(binding.crestColorOpacity.a));
	baseColor = lerp(baseColor, binding.crestColorOpacity.rgb,
		saturate(crest * max(binding.virtualParams.z, 0.0)));

	float hotCrack = 0.0;
	if ((binding.styleFlags & NRI_FLUID_STYLE_CRUST_EMISSION) != 0u)
	{
		const float softness = max(binding.crust.y, 1.0e-3);
		hotCrack = 1.0 - smoothstep(
			saturate(binding.crust.x) - softness,
			saturate(binding.crust.x) + softness,
			abs(signal));
		hotCrack = pow(saturate(hotCrack), max(binding.crust.z, 0.05));
		baseColor *= lerp(
			saturate(1.0 - binding.crust.w), 1.0, hotCrack);
	}

	evaluation.active = 1u;
	evaluation.bindingIndex = material.fluidBindingIndex;
	evaluation.activeLayerCount = min(
		min(max(binding.layerCount, 1u), layerCap), NRI_FLUID_FIELD_LAYER_CAP);
	evaluation.underside = underside ? 1u : 0u;
	evaluation.samplePosition = samplePosition;
	evaluation.virtualOffset = virtualOffset;
	evaluation.carrierNormal = carrierNormal;
	evaluation.worldFootprint = worldFootprint;
	evaluation.shadingNormal = shadingNormal;
	evaluation.roughness = clamp(
		underside ? binding.surfaceResponse.y : binding.surfaceResponse.x,
		0.02, 1.0);
	evaluation.baseColor = max(baseColor, 0.0);
	evaluation.reactive = saturate(abs(frame.currentGameplaySeconds - frame.previousGameplaySeconds) *
		(abs(binding.motion.x) + abs(binding.domainAxisUFlowU.w) + abs(binding.domainAxisVFlowV.w)));
	evaluation.emission = max(binding.emissionColorIntensity.rgb, 0.0) *
		max(binding.emissionColorIntensity.w, 0.0) * hotCrack;
	evaluation.height = height;
	return true;
}

#endif
