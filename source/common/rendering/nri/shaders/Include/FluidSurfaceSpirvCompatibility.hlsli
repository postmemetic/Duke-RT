#ifndef NRI_FLUID_SURFACE_HLSLI
#define NRI_FLUID_SURFACE_HLSLI

#include "FluidSurfaceContracts.hlsli"

struct FluidSurfaceEvaluation
{
	uint active;
	uint bindingIndex;
	uint activeLayerCount;
	uint underside;
	uint ownsPrimaryEmission;
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
	float crest;
	float breaking;
	float foamBirth;
	float foamTrail;
	float foam;
	float previousFoam;
	float foamPhase;
	float foamAngularVelocity;
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

#include "FluidSurfaceFrame.hlsli"

void FluidEvaluateOctaves(
	FluidBindingData binding,
	float2 domain,
	float gameplaySeconds,
	float worldFootprint,
	uint layerCap,
	out float height,
	out float2 gradient,
	out float signal,
	out float crest)
{
	height = 0.0;
	gradient = 0.0;
	signal = 0.0;
	crest = 0.0;
	const uint layerCount = min(
		min(max(binding.layerCount, 1u), max(layerCap, 1u)),
		NRI_FLUID_MAX_LAYERS);
	const float domainScale = max(abs(binding.domainOriginScale.w), 1.0e-5);
	float frequency = max(abs(binding.spectrum.y), 1.0e-4);
	float amplitude = binding.spectrum.x;
	const float lacunarity = max(abs(binding.spectrum.z), 1.01);
	const float gain = saturate(abs(binding.spectrum.w));
	const float anisotropy = clamp(binding.motion.z, 0.2, 5.0);
	const float viscosity = (binding.styleFlags & NRI_FLUID_STYLE_VISCOUS) != 0u ?
		saturate(binding.shaping.x) : 0.0;
	const float ridgeMix = saturate(binding.shaping.z);
	const float detailScale = max(binding.shaping.w, 0.05);
	float amplitudeSum = 0.0;

	[unroll]
	for (uint layer = 0u; layer < NRI_FLUID_MAX_LAYERS; ++layer)
	{
		if (layer >= layerCount)
		{
			break;
		}
		const uint layerSeed = binding.seed + layer * 0x9e3779b9u;
		const float angle = 6.28318530718 * FluidHash01(layerSeed);
		float2 direction = normalize(float2(cos(angle) * anisotropy, sin(angle)));
		const float layerFrequency = frequency * (layer == 0u ? 1.0 : detailScale);
		const float2 crossDirection = float2(-direction.y, direction.x);
		const float crossPhase = dot(domain, crossDirection) * layerFrequency * 0.41 +
			gameplaySeconds * binding.motion.x * 0.31;
		const float warp = clamp(binding.motion.w, -1.5, 1.5);
		const float phase = dot(domain, direction) * layerFrequency +
			warp * sin(crossPhase) +
			gameplaySeconds * binding.motion.x * (1.0 + 0.17 * (float)layer) +
			6.28318530718 * FluidHash01(layerSeed ^ 0xa511e9b3u);
		const float rawWave = sin(phase);
		const float rawDerivative = cos(phase);
		const float viscousWave = rawWave * rawWave * rawWave;
		const float viscousDerivative = 3.0 * rawWave * rawWave * rawDerivative;
		float wave = lerp(rawWave, viscousWave, viscosity);
		float derivative = lerp(rawDerivative, viscousDerivative, viscosity);
		const float ridgedWave = 1.0 - 2.0 * abs(wave);
		const float ridgedDerivative = -2.0 * sign(wave) * derivative;
		wave = lerp(wave, ridgedWave, ridgeMix);
		derivative = lerp(derivative, ridgedDerivative, ridgeMix);
		const float filterWidth = max(worldFootprint, 0.0) * domainScale * layerFrequency;
		const float filteredAmplitude = amplitude / (1.0 + filterWidth * filterWidth);
		const float2 phaseGradient = direction * layerFrequency +
			warp * cos(crossPhase) * crossDirection * layerFrequency * 0.41;
		height += filteredAmplitude * wave;
		gradient += filteredAmplitude * derivative * phaseGradient * domainScale;
		signal += abs(filteredAmplitude) * wave;
		crest += abs(filteredAmplitude) * saturate(wave * 0.5 + 0.5);
		amplitudeSum += abs(filteredAmplitude);
		frequency *= lacunarity;
		amplitude *= gain;
	}

	const float maxHeight = max(abs(binding.spectrum.x), 0.0);
	if (maxHeight > 0.0)
	{
		height = clamp(height, -maxHeight, maxHeight);
	}
	const float maxSlope = max(binding.surfaceResponse.w, 0.0);
	const float slope = length(gradient);
	if (maxSlope > 0.0 && slope > maxSlope)
	{
		gradient *= maxSlope / slope;
	}
	signal = amplitudeSum > 1.0e-5 ? signal / amplitudeSum : 0.0;
	crest = amplitudeSum > 1.0e-5 ? crest / amplitudeSum : 0.0;
}

// Keep the optional ray-offset solve intentionally cheaper than the final
// four-octave shading evaluation. It follows the same authored domain, seed,
// time, warp, viscosity, and ridge controls, but uses a normalized two-wave
// carrier so increasing the iteration count does not multiply the full fluid
// evaluator inside TraceOpaque.
float FluidEvaluateVirtualHeightRatio(
	FluidBindingData binding,
	float2 domain,
	float gameplaySeconds)
{
	const float frequency = max(abs(binding.spectrum.y), 1.0e-4);
	const float anisotropy = clamp(binding.motion.z, 0.2, 5.0);
	const float angle = 6.28318530718 * FluidHash01(binding.seed);
	const float2 direction = normalize(float2(cos(angle) * anisotropy, sin(angle)));
	const float2 crossDirection = float2(-direction.y, direction.x);
	const float warp = clamp(binding.motion.w, -1.5, 1.5);
	const float crossPhase = dot(domain, crossDirection) * frequency * 0.41 +
		gameplaySeconds * binding.motion.x * 0.31;
	const float primaryPhase = dot(domain, direction) * frequency +
		warp * sin(crossPhase) + gameplaySeconds * binding.motion.x +
		6.28318530718 * FluidHash01(binding.seed ^ 0xa511e9b3u);
	const float secondaryPhase =
		dot(domain, normalize(crossDirection + 0.31 * direction)) *
		frequency * max(abs(binding.spectrum.z), 1.01) +
		gameplaySeconds * binding.motion.x * 1.17 +
		6.28318530718 * FluidHash01(binding.seed ^ 0x63d83595u);
	const float secondaryWeight = saturate(abs(binding.spectrum.w));
	float wave = (sin(primaryPhase) + secondaryWeight * sin(secondaryPhase)) /
		max(1.0 + secondaryWeight, 1.0e-4);
	const float viscosity = (binding.styleFlags & NRI_FLUID_STYLE_VISCOUS) != 0u ?
		saturate(binding.shaping.x) : 0.0;
	wave = lerp(wave, wave * wave * wave, viscosity);
	const float ridgedWave = 1.0 - 2.0 * abs(wave);
	return clamp(lerp(wave, ridgedWave, saturate(binding.shaping.z)), -1.0, 1.0);
}

bool TryEvaluateFluidSurface(
	MaterialData material,
	float3 carrierPosition,
	float3 geometricNormal,
	float3 carrierShadingNormal,
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

	float3 axisU, axisV;
	FluidBuildDomainAxes(material, binding, carrierNormal, axisU, axisV);

	const float domainScale = max(abs(binding.domainOriginScale.w), 1.0e-5);
	const float viscosity = (binding.styleFlags & NRI_FLUID_STYLE_VISCOUS) != 0u ?
		saturate(binding.shaping.x) : 0.0;
	const float timeScale = frame.globalTimeScale * binding.motion.y *
		lerp(1.0, 0.18, viscosity);
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
	const float signedViewCosine = dot(
		FluidSafeNormalize(viewRayDirection, -viewDir), carrierNormal);
	const float minimumCosine = max(abs(binding.virtualParams.w), 0.08);
	const bool useVirtualHeight = allowVirtualHeight &&
		(frame.flags & NRI_FLUID_FRAME_FLAG_VIRTUAL_HEIGHT) != 0u &&
		(binding.flags & NRI_FLUID_BINDING_FLAG_DISABLE_VIRTUAL_HEIGHT) == 0u &&
		abs(signedViewCosine) >= minimumCosine && binding.virtualParams.x > 0.0;
	if (useVirtualHeight)
	{
		// Iterate only inside the thin shading slab. This refines the field's
		// sample position; the accepted triangle hit remains authoritative.
		const float virtualHalfHeight = max(binding.virtualParams.x, 0.0);
		const float travelBound = max(binding.virtualParams.y, 0.0);
		const float maximumTravel = min(
			travelBound,
			virtualHalfHeight / max(abs(signedViewCosine), minimumCosine));
		const uint virtualStepCount = clamp(frame.virtualStepCount, 2u, 12u);
		[loop]
		for (uint virtualStep = 0u; virtualStep < 12u; ++virtualStep)
		{
			if (virtualStep >= virtualStepCount)
			{
				break;
			}
			const float3 virtualPosition = carrierPosition + viewRayDirection * virtualOffset;
			const float3 virtualLocal = virtualPosition - binding.domainOriginScale.xyz;
			float2 virtualDomain = float2(
				dot(virtualLocal, axisU), dot(virtualLocal, axisV)) * domainScale;
			virtualDomain += domain - float2(
				dot(localPosition, axisU), dot(localPosition, axisV)) * domainScale;
			const float targetHeight = FluidEvaluateVirtualHeightRatio(
				binding, virtualDomain, gameplaySeconds) * virtualHalfHeight;
			const float targetOffset = clamp(
				targetHeight / signedViewCosine,
				-maximumTravel, maximumTravel);
			virtualOffset = lerp(virtualOffset, targetOffset,
				virtualStep + 1u == virtualStepCount ? 1.0 : 0.65);
		}
	}

	const float3 samplePosition = carrierPosition + viewRayDirection * virtualOffset;
	const float3 sampleLocal = samplePosition - binding.domainOriginScale.xyz;
	float2 sampleDomain = float2(dot(sampleLocal, axisU), dot(sampleLocal, axisV)) * domainScale;
	sampleDomain += domain - float2(dot(localPosition, axisU), dot(localPosition, axisV)) * domainScale;
	float height = 0.0;
	float2 gradient = 0.0;
	float signal = 0.0;
	float crest = 0.0;
	FluidEvaluateOctaves(
		binding, sampleDomain, gameplaySeconds, worldFootprint, layerCap,
		height, gradient, signal, crest);
	height *= frame.globalAmplitudeScale;
	gradient *= frame.globalAmplitudeScale;

	const float normalStrength = max(binding.surfaceResponse.z, 0.0);
	float3 shadingNormal = FluidComposeShadingNormal(carrierNormal, carrierShadingNormal,
		axisU, axisV, gradient, normalStrength);
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
		min(max(binding.layerCount, 1u), layerCap), NRI_FLUID_MAX_LAYERS);
	evaluation.underside = underside ? 1u : 0u;
	// Match primary visible ownership without importing the D3D foam evaluator.
	evaluation.ownsPrimaryEmission =
		(binding.styleFlags & NRI_FLUID_STYLE_CRUST_EMISSION) != 0u ? 1u : 0u;
	evaluation.samplePosition = samplePosition;
	evaluation.virtualOffset = virtualOffset;
	evaluation.carrierNormal = carrierNormal;
	evaluation.worldFootprint = worldFootprint;
	evaluation.shadingNormal = shadingNormal;
	evaluation.roughness = clamp(
		underside ? binding.surfaceResponse.y : binding.surfaceResponse.x,
		0.02, 1.0);
	evaluation.baseColor = max(baseColor, 0.0);
	evaluation.reactive = FluidAnimationReactive(frame, binding, timeScale);
	evaluation.emission = max(binding.emissionColorIntensity.rgb, 0.0) *
		max(binding.emissionColorIntensity.w, 0.0) * hotCrack;
	evaluation.height = height;
	evaluation.crest = crest;
	// The compatibility field predates the D3D Seascape-style field and gives
	// the shaping rows different meanings. Keep Vulkan foam capability-disabled
	// instead of producing a detached, separately tuned mask.
	evaluation.breaking = 0.0;
	evaluation.foamBirth = 0.0;
	evaluation.foamTrail = 0.0;
	evaluation.foam = 0.0;
	evaluation.previousFoam = 0.0;
	evaluation.foamPhase = 0.0;
	evaluation.foamAngularVelocity = 0.0;
	return true;
}

#endif
