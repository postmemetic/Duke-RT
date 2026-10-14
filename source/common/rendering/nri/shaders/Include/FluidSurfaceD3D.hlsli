#ifndef NRI_FLUID_SURFACE_D3D_HLSLI
#define NRI_FLUID_SURFACE_D3D_HLSLI

#include "FluidSurfaceContracts.hlsli"
#include "FluidSurfacePan.hlsli"

#define NRI_FLUID_FIELD_LAYER_CAP NRI_FLUID_MAX_LAYERS

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

float FluidChoppyWaveSecondDerivative(float phase, float choppiness)
{
	float sine;
	float cosine;
	sincos(phase, sine, cosine);
	const float choppyAmount = saturate((choppiness - 1.0) * (1.0 / 3.0));
	const float cosineDouble = cosine * cosine - sine * sine;
	return (-sine + 2.0 * choppyAmount * cosineDouble) /
		(1.0 + 0.5 * choppyAmount);
}

float2 FluidDomainVelocity(FluidBindingData binding)
{
	float2 velocity = 0.0;
	if ((binding.styleFlags & NRI_FLUID_STYLE_FLOW) != 0u)
	{
		velocity -= float2(
			binding.domainAxisUFlowU.w, binding.domainAxisVFlowV.w);
	}
	if ((binding.styleFlags & NRI_FLUID_STYLE_FALL) != 0u)
	{
		// The small authored fall wobble is excluded from nominal foam age.
		velocity.y += binding.domainAxisVFlowV.w;
	}
	return velocity;
}

float2 FluidApplyDomainMotion(
	FluidBindingData binding,
	float2 baseDomain,
	float gameplaySeconds)
{
	float2 domain = baseDomain;
	if ((binding.styleFlags & NRI_FLUID_STYLE_FLOW) != 0u)
	{
		domain -= float2(
			binding.domainAxisUFlowU.w,
			binding.domainAxisVFlowV.w) * gameplaySeconds;
	}
	if ((binding.styleFlags & NRI_FLUID_STYLE_FALL) != 0u)
	{
		domain.y += binding.domainAxisVFlowV.w *
			(gameplaySeconds + 0.2 * sin(gameplaySeconds * 0.73 +
			(float)(binding.seed & 31u)));
	}
	return domain;
}

float2 FluidInitialFieldDirection(FluidBindingData binding);

void FluidEvaluateMacroFoamCarrier(
	FluidBindingData binding,
	float2 domain,
	float gameplaySeconds,
	float phaseSeconds,
	float globalAmplitudeScale,
	out float phase,
	out float angularVelocity,
	out float breakingPotential,
	out float crestBreakingPotential)
{
	const float domainScale = max(abs(binding.domainOriginScale.w), 1.0e-5);
	const float frequency = max(abs(binding.spectrum.y), 1.0e-4);
	const float anisotropy = clamp(binding.motion.z, 0.2, 5.0);
	const float warp = clamp(binding.motion.w, 0.0, 4.0);
	const float noiseScale = clamp(binding.shaping.z, 0.05, 4.0);
	const float choppiness = clamp(binding.shaping.x, 0.05, 4.0);
	const float2 direction = FluidInitialFieldDirection(binding);
	const float2 crossDirection = float2(-direction.y, direction.x);
	const float2 layerDomain = float2(
		dot(domain, direction) * anisotropy,
		dot(domain, crossDirection));
	const float layerTime = gameplaySeconds * binding.motion.x;
	const float2 noiseCoordinate = layerDomain * (frequency * noiseScale) +
		float2(layerTime * 0.13, -layerTime * 0.09);
	const float noise = FluidValueNoise(noiseCoordinate, binding.seed);
	phase = layerDomain.x * frequency + warp * noise + phaseSeconds * binding.motion.x +
		6.28318530718 * FluidHash01(binding.seed ^ 0xa511e9b3u);
	const float2 wave = FluidChoppyWaveDerivative(phase, choppiness);
	const float secondDerivative = FluidChoppyWaveSecondDerivative(phase, choppiness);
	const float2 phaseGradient = frequency * direction * anisotropy;
	angularVelocity = binding.motion.x +
		dot(FluidDomainVelocity(binding), phaseGradient);
	const float steepness = max(binding.spectrum.x, 0.0) *
		max(globalAmplitudeScale, 0.0) * frequency * domainScale *
		max(anisotropy, 1.0);
	const float upperCrest = saturate(wave.x * 0.5 + 0.5);
	const float choppyAmount = saturate((choppiness - 1.0) * (1.0 / 3.0));
	const float crestCurvature = (1.0 + 2.0 * choppyAmount) /
		(1.0 + 0.5 * choppyAmount);
	const float rawBreaking = 1.0 - exp(
		-steepness * max(-secondDerivative, 0.0));
	const float rawCrestBreaking = 1.0 - exp(-steepness * crestCurvature);
	// Normalize curvature response so authored thresholds retain a useful 0..1
	// range, while the steepness gate still rejects genuinely flat carriers.
	const float breakingStrength = saturate(steepness * 4.0);
	breakingPotential = saturate(upperCrest * breakingStrength *
		rawBreaking / max(rawCrestBreaking, 1.0e-5));
	crestBreakingPotential = breakingStrength;
	if (!isfinite(phase) || !isfinite(angularVelocity) ||
		!isfinite(breakingPotential) || !isfinite(crestBreakingPotential))
	{
		phase = 0.0;
		angularVelocity = 0.0;
		breakingPotential = 0.0;
		crestBreakingPotential = 0.0;
	}
}

bool FluidUsesSmoothFoamBlend(FluidBindingData binding)
{
	return binding.foamPresentationParams.y == (float)NRI_FLUID_FOAM_BLEND_SMOOTH;
}

float FluidThresholdFoamBirth(
	FluidBindingData binding,
	float potential,
	float worldFootprint)
{
	const float domainScale = max(abs(binding.domainOriginScale.w), 1.0e-5);
	const float frequency = max(abs(binding.spectrum.y), 1.0e-4);
	const float anisotropy = clamp(binding.motion.z, 0.2, 5.0);
	const float filterWidth = max(worldFootprint, 0.0) * domainScale * frequency *
		max(anisotropy, 1.0);
	float softness = max(max(binding.foamBirthParams.z, 0.0),
		0.5 * saturate(filterWidth));
	const float threshold = saturate(binding.foamBirthParams.y);
	if (FluidUsesSmoothFoamBlend(binding))
	{
		// Smooth scum/crust fades at birth instead of only blurring an already
		// thresholded stripe. Preserve full coverage at the peak potential.
		softness = max(softness, clamp(binding.foamPresentationParams.z, 0.0, 0.5));
		const float upper = min(1.0, threshold + softness + 1.0e-4);
		const float lower = min(threshold - softness, upper - 1.0e-4);
		return smoothstep(lower, upper, saturate(potential));
	}
	// Keep the legacy path exact for water and profiles which do not opt in.
	return smoothstep(threshold - softness, threshold + softness + 1.0e-4,
		saturate(potential));
}

float FluidEvaluateFoamBreakup(
	FluidBindingData binding,
	float2 domain,
	float worldFootprint)
{
	const float breakupScale = max(binding.foamTrailParams.x, 1.0e-4);
	const float breakupStrength = saturate(binding.foamTrailParams.y);
	if (breakupStrength <= 0.0)
	{
		return 1.0;
	}
	const float domainScale = max(abs(binding.domainOriginScale.w), 1.0e-5);
	const float footprint = max(worldFootprint, 0.0) * domainScale * breakupScale;
	const float detailWeight = rcp(1.0 + footprint * footprint);
	const float noise = 0.5 + 0.5 * FluidValueNoise(
		domain * breakupScale + float2(17.19, -31.73),
		binding.seed ^ 0x6c8e9cf5u);
	float transitionWidth = 0.12 + 0.35 * saturate(footprint);
	if (FluidUsesSmoothFoamBlend(binding))
		transitionWidth = max(transitionWidth, clamp(binding.foamPresentationParams.z, 0.0, 0.5));
	const float clump = smoothstep(
		0.5 - transitionWidth, 0.5 + transitionWidth, noise);
	return lerp(1.0, 0.25 + 0.75 * clump,
		breakupStrength * detailWeight);
}

float FluidPositiveModulo(float value, float period)
{
	return value - period * floor(value / period);
}

void FluidShapeFoam(
	FluidBindingData binding,
	uint foamMode,
	bool underside,
	float phase,
	float angularVelocity,
	float breakingPotential,
	float crestBreakingPotential,
	float2 domain,
	float worldFootprint,
	out float birth,
	out float trail,
	out float foam)
{
	birth = 0.0;
	trail = 0.0;
	foam = 0.0;
	const float sideStrength = underside ?
		saturate(binding.foamBirthParams.w) : 1.0;
	const float amount = saturate(binding.foamColorAmount.w) * sideStrength;
	if (foamMode == 0u || amount <= 0.0)
	{
		return;
	}
	const float breakup = FluidEvaluateFoamBreakup(
		binding, domain, worldFootprint);
	birth = saturate(amount * FluidThresholdFoamBirth(
		binding, breakingPotential, worldFootprint) * breakup);
	const float lifetime = max(binding.foamTrailParams.z, 0.0);
	const float speed = abs(angularVelocity);
	if (foamMode >= 2u && lifetime > 0.0 && speed > 1.0e-4)
	{
		const float directionSign = angularVelocity >= 0.0 ? 1.0 : -1.0;
		const float agePhase = FluidPositiveModulo(
			(phase - 1.57079632679) * directionSign, 6.28318530718);
		const float age = agePhase / speed;
		const float trailSource = FluidThresholdFoamBirth(
			binding, crestBreakingPotential, worldFootprint);
		trail = saturate(amount * max(binding.foamTrailParams.w, 0.0) *
			trailSource * exp(-age / lifetime) * breakup);
	}
	foam = max(birth, trail);
}

float FluidPresentFoam(
	FluidBindingData binding,
	uint styleMode,
	float2 domain,
	float worldFootprint,
	float maximumFoam,
	float rawFoam)
{
	// Natural presentation is an exact bypass of the landed foam response.
	if (styleMode == 0u)
	{
		return rawFoam;
	}
	// A broad birth fade must not be sharpened back into a stripe by graphic
	// presentation. Smooth mode retains the authored amount and cutouts.
	const float authoredContrast = FluidUsesSmoothFoamBlend(binding) ?
		1.0 : max(binding.foamStyleParams.x, 0.0);
	const bool neutralCutouts = binding.foamStyleParams.w <= 0.0;
	if (authoredContrast <= 1.0 && neutralCutouts)
	{
		return rawFoam;
	}

	const float domainScale = max(abs(binding.domainOriginScale.w), 1.0e-5);
	const float frequency = max(abs(binding.spectrum.y), 1.0e-4);
	const float anisotropy = clamp(binding.motion.z, 0.2, 5.0);
	const float carrierFootprint = max(worldFootprint, 0.0) * domainScale *
		frequency * max(anisotropy, 1.0);
	const float authoredEdgeWidth = clamp(binding.foamStyleParams.y, 0.001, 0.5);
	const float edgeWidth = min(0.5, max(
		authoredEdgeWidth / max(authoredContrast, 1.0),
		0.25 * saturate(carrierFootprint)));
	const float normalizedFoam = saturate(
		rawFoam / max(maximumFoam, 1.0e-5));
	const float crispFoam = smoothstep(
		0.5 - edgeWidth, 0.5 + edgeWidth, normalizedFoam);
	float presentedFoam = lerp(
		rawFoam, crispFoam, saturate(authoredContrast - 1.0));

	// Broad value-noise cutouts break long graphic ribbons. As the pattern
	// becomes subpixel its influence tends to one, never to gray coverage.
	[branch]
	if (binding.foamStyleParams.w > 0.0)
	{
		const float cutoutScale = max(binding.foamStyleParams.z, 1.0e-4);
		const float cutoutFootprint = max(worldFootprint, 0.0) * domainScale *
			cutoutScale;
		const float cutoutDetail = rcp(
			1.0 + cutoutFootprint * cutoutFootprint);
		const float cutoutNoise = 0.5 + 0.5 * FluidValueNoise(
			domain * cutoutScale + float2(-43.17, 11.29),
			binding.seed ^ 0x51ed270bu);
		float cutoutTransition = 0.10 +
			0.35 * saturate(cutoutFootprint);
		if (FluidUsesSmoothFoamBlend(binding))
			cutoutTransition = max(cutoutTransition, clamp(binding.foamPresentationParams.z, 0.0, 0.5));
		const float cutoutMask = smoothstep(
			0.5 - cutoutTransition, 0.5 + cutoutTransition, cutoutNoise);
		const float cutoutInfluence = saturate(binding.foamStyleParams.w) *
			cutoutDetail;
		presentedFoam *= lerp(1.0, cutoutMask, cutoutInfluence);
	}

	return saturate(presentedFoam);
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
	float phaseSeconds,
	float worldFootprint,
	float globalAmplitudeScale,
	uint foamMode,
	uint layerCap,
	out float heightRatio,
	out float2 gradientRatio,
	out float crest,
	out float foamPhase,
	out float foamAngularVelocity,
	out float foamBreakingPotential,
	out float foamCrestBreakingPotential)
{
	heightRatio = 0.0;
	gradientRatio = 0.0;
	crest = 0.0;
	foamPhase = 0.0;
	foamAngularVelocity = 0.0;
	foamBreakingPotential = 0.0;
	foamCrestBreakingPotential = 0.0;
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
		const float phaseTime = phaseSeconds * binding.motion.x * (1.0 + 0.17 * (float)layer);
		const float phase = layerDomain.x * frequency + warp * noise + phaseTime +
			6.28318530718 * FluidHash01(layerSeed ^ 0xa511e9b3u);
		const float2 wave = FluidChoppyWaveDerivative(phase, layerChoppiness);
		[branch]
		if (layer == 0u && foamMode != 0u)
		{
			const float secondDerivative = FluidChoppyWaveSecondDerivative(
				phase, layerChoppiness);
			const float steepness = max(binding.spectrum.x, 0.0) *
				max(globalAmplitudeScale, 0.0) * frequency *
				domainScale * max(anisotropy, 1.0);
			const float upperCrest = saturate(wave.x * 0.5 + 0.5);
			foamPhase = phase;
			foamAngularVelocity = binding.motion.x + dot(
				FluidDomainVelocity(binding), frequency * direction * anisotropy);
			const float choppyAmount = saturate(
				(layerChoppiness - 1.0) * (1.0 / 3.0));
			const float crestCurvature = (1.0 + 2.0 * choppyAmount) /
				(1.0 + 0.5 * choppyAmount);
			const float rawBreaking = 1.0 - exp(
				-steepness * max(-secondDerivative, 0.0));
			const float rawCrestBreaking =
				1.0 - exp(-steepness * crestCurvature);
			const float breakingStrength = saturate(steepness * 4.0);
			foamBreakingPotential = saturate(upperCrest * breakingStrength *
				rawBreaking / max(rawCrestBreaking, 1.0e-5));
			foamCrestBreakingPotential = breakingStrength;
		}
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
	if (!isfinite(heightRatio) || !all(isfinite(gradientRatio)) || !isfinite(crest) ||
		!isfinite(foamPhase) || !isfinite(foamAngularVelocity) ||
		!isfinite(foamBreakingPotential) || !isfinite(foamCrestBreakingPotential))
	{
		heightRatio = 0.0;
		gradientRatio = 0.0;
		crest = 0.0;
		foamPhase = 0.0;
		foamAngularVelocity = 0.0;
		foamBreakingPotential = 0.0;
		foamCrestBreakingPotential = 0.0;
	}
}

// Phase 2C uses the low-frequency prefix of the same authored field. Keeping
// this prefix explicit avoids a nested variable-bound field loop in the large
// radiance-cache TraceOpaque shader variants.
void FluidEvaluateVirtualField(
	FluidBindingData binding,
	float2 domain,
	float gameplaySeconds,
	float phaseSeconds,
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
	const float phase = layerDomain.x * frequency + warp * noise + phaseSeconds * binding.motion.x +
		6.28318530718 * FluidHash01(binding.seed ^ 0xa511e9b3u);
	heightRatio = FluidChoppyWaveDerivative(phase, layerChoppiness).x;
	if (!isfinite(heightRatio))
	{
		heightRatio = 0.0;
	}
}

bool TryEvaluateFluidSurface(
	MaterialData material,
	HitData hit,
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
	const uint foamMode = min(frame.foamMode, 2u);
	const uint foamStyleMode = min(frame.foamStyleMode, 1u);
	const FluidPanEvaluation pan = FluidResolvePan(hit, material, binding, frame);

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
	const float timeScale = frame.globalTimeScale * binding.motion.y;
	const float gameplaySeconds = frame.currentGameplaySeconds * timeScale;
	// Freeze intrinsic linear travel when authored pan takes over, but keep
	// slow noise evolution. The first-moving phase anchor avoids a phase reset.
	const float phaseSeconds = pan.currentPhaseSeconds * timeScale;
	const float3 localPosition = carrierPosition - binding.domainOriginScale.xyz;
	const float2 carrierBaseDomain = float2(
		dot(localPosition, axisU), dot(localPosition, axisV)) * domainScale;
	float2 domain = FluidApplyDomainMotion(binding, carrierBaseDomain, phaseSeconds) + pan.currentOffset;

	const uint layerCap = clamp(frame.primaryLayerCap, 1u, NRI_FLUID_MAX_LAYERS);
	float virtualOffset = 0.0;
	const float3 normalizedViewRayDirection = FluidSafeNormalize(viewRayDirection, -viewDir);
	const float3 virtualCarrierNormal = FluidResolveSmoothCarrierNormal(carrierNormal, carrierShadingNormal);
	const float signedViewCosine = dot(normalizedViewRayDirection, virtualCarrierNormal);
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
			binding, domain, gameplaySeconds, phaseSeconds, virtualHeightRatio);
		const float targetHeight = virtualHeightRatio * virtualHalfHeight;
		const float targetOffset = clamp(
			targetHeight / signedViewCosine, -maximumTravel, maximumTravel);
		virtualOffset = isfinite(targetOffset) ? targetOffset : 0.0;
	}

	const float3 samplePosition = carrierPosition + normalizedViewRayDirection * virtualOffset;
	const float3 sampleLocal = samplePosition - binding.domainOriginScale.xyz;
	const float2 sampleBaseDomain = float2(
		dot(sampleLocal, axisU), dot(sampleLocal, axisV)) * domainScale;
	float2 sampleDomain = sampleBaseDomain;
	sampleDomain += domain - float2(dot(localPosition, axisU), dot(localPosition, axisV)) * domainScale;
	float heightRatio = 0.0;
	float2 gradientRatio = 0.0;
	float crest = 0.0;
	float foamPhase = 0.0;
	float foamAngularVelocity = 0.0;
	float foamBreakingPotential = 0.0;
	float foamCrestBreakingPotential = 0.0;
	FluidEvaluateField(
		binding, sampleDomain, gameplaySeconds, phaseSeconds, worldFootprint,
		frame.globalAmplitudeScale, foamMode, layerCap,
		heightRatio, gradientRatio, crest, foamPhase, foamAngularVelocity,
		foamBreakingPotential, foamCrestBreakingPotential);
	// The carrier helper differentiates with respect to its scaled time input;
	// authored foam lifetime is in gameplay seconds.
	foamAngularVelocity *= timeScale;
	// Nominal authored rate is held between ticks/while paused. Differencing
	// interpolated displacement against the integer game clock would blink
	// analytic trails on sub-tick frames. This is a trail-shape scale, not a
	// simulated foam lifetime or a replacement for exact domain displacement.
	const float2 panPhaseGradient = max(abs(binding.spectrum.y), 1.0e-4) *
		FluidInitialFieldDirection(binding) * clamp(binding.motion.z, 0.2, 5.0);
	if ((pan.flags & NRI_FLUID_PAN_CURRENT_ACTIVE) != 0u)
		foamAngularVelocity = dot(pan.currentDomainRate, panPhaseGradient);
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

	float foamBirth = 0.0;
	float foamTrail = 0.0;
	float foam = 0.0;
	float previousFoam = 0.0;
	const float foamSideStrength = underside ?
		saturate(binding.foamBirthParams.w) : 1.0;
	[branch]
	if (foamMode != 0u && binding.foamColorAmount.w > 0.0 &&
		foamSideStrength > 0.0)
	{
		FluidShapeFoam(
			binding, foamMode, underside, foamPhase, foamAngularVelocity,
			foamBreakingPotential, foamCrestBreakingPotential, sampleDomain,
			worldFootprint, foamBirth, foamTrail, foam);

		const float previousGameplaySeconds =
			frame.previousGameplaySeconds * timeScale;
		const float previousPhaseSeconds = pan.previousPhaseSeconds * timeScale;
		const float2 previousSampleDomain = FluidApplyDomainMotion(
			binding, sampleBaseDomain, previousPhaseSeconds) + pan.previousOffset;
		const float2 previousCarrierPatternDomain = FluidApplyDomainMotion(
			binding, carrierBaseDomain, previousPhaseSeconds) + pan.previousOffset;
		float previousPhase = 0.0;
		float previousAngularVelocity = 0.0;
		float previousBreakingPotential = 0.0;
		float previousCrestBreakingPotential = 0.0;
		FluidEvaluateMacroFoamCarrier(
			binding, previousSampleDomain, previousGameplaySeconds,
			previousPhaseSeconds,
			frame.globalAmplitudeScale, previousPhase, previousAngularVelocity,
			previousBreakingPotential, previousCrestBreakingPotential);
		previousAngularVelocity *= timeScale;
		if ((pan.flags & NRI_FLUID_PAN_PREVIOUS_ACTIVE) != 0u)
			previousAngularVelocity = dot(pan.previousDomainRate, panPhaseGradient);
		float previousBirth = 0.0;
		float previousTrail = 0.0;
		FluidShapeFoam(
			binding, foamMode, underside, previousPhase,
			previousAngularVelocity, previousBreakingPotential,
			previousCrestBreakingPotential, previousSampleDomain, worldFootprint,
			previousBirth, previousTrail, previousFoam);

		// Presentation is deliberately downstream of birth/trail construction:
		// diagnostics retain the raw physical signals while material response and
		// reactivity consume the identically styled current/previous coverage.
		[branch]
		if (foamStyleMode != 0u)
		{
			const float maximumFoam = saturate(
				binding.foamColorAmount.w) * foamSideStrength;
			foam = FluidPresentFoam(
				binding, foamStyleMode, domain, worldFootprint,
				maximumFoam, foam);
			previousFoam = FluidPresentFoam(
				binding, foamStyleMode, previousCarrierPatternDomain,
				worldFootprint, maximumFoam, previousFoam);
		}
	}

	// Ordinary fluids use uniform authored self-glow; lava alone modulates it
	// with the existing crust mask. Zero emission remains an exact opt-out.
	float hotCrack = 1.0;
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
	const bool neutralFoamPresentation =
		binding.foamStyleParams.x <= 1.0 &&
		binding.foamStyleParams.w <= 0.0 &&
		abs(binding.foamPresentationParams.x - 1.0) <= 1.0e-5;
	const bool useStylizedFoamColor =
		foamStyleMode != 0u && !neutralFoamPresentation;
	const float foamBrightness = useStylizedFoamColor ?
		max(binding.foamPresentationParams.x, 0.0) : 1.0;
	const float3 presentedFoamColor = !useStylizedFoamColor ?
		max(binding.foamColorAmount.rgb, 0.0) :
		saturate(binding.foamColorAmount.rgb * foamBrightness);
	baseColor = lerp(baseColor, presentedFoamColor, foam);
	const float fluidRoughness = clamp(
		underside ? binding.surfaceResponse.y : binding.surfaceResponse.x,
		0.02, 1.0);
	const float foamRoughness = clamp(binding.foamBirthParams.x, 0.02, 1.0);

	evaluation.active = 1u;
	evaluation.bindingIndex = material.fluidBindingIndex;
	evaluation.activeLayerCount = min(
		min(max(binding.layerCount, 1u), layerCap), NRI_FLUID_FIELD_LAYER_CAP);
	evaluation.underside = underside ? 1u : 0u;
	// This is visible primary-hit ownership only. The retained material remains
	// authoritative for sampled lights and secondary hits until emission parity.
	evaluation.ownsPrimaryEmission =
		(binding.styleFlags & NRI_FLUID_STYLE_CRUST_EMISSION) != 0u ? 1u : 0u;
	evaluation.samplePosition = samplePosition;
	evaluation.virtualOffset = virtualOffset;
	evaluation.carrierNormal = carrierNormal;
	evaluation.worldFootprint = worldFootprint;
	evaluation.shadingNormal = shadingNormal;
	evaluation.roughness = lerp(fluidRoughness, foamRoughness, foam);
	evaluation.baseColor = max(baseColor, 0.0);
	const float carrierReactive = FluidAnimationReactive(frame, binding, timeScale);
	evaluation.reactive = max(max(carrierReactive, FluidPanReactive(pan, binding)), abs(foam - previousFoam));
	// The normally lit scum layer covers hot lava instead of glowing through it.
	// Zero coverage preserves the original procedural emission exactly.
	evaluation.emission = max(binding.emissionColorIntensity.rgb, 0.0) *
		max(binding.emissionColorIntensity.w, 0.0) * hotCrack * (1.0 - saturate(foam));
	evaluation.height = height;
	evaluation.crest = crest;
	evaluation.breaking = foamBreakingPotential;
	evaluation.foamBirth = foamBirth;
	evaluation.foamTrail = foamTrail;
	evaluation.foam = foam;
	evaluation.previousFoam = previousFoam;
	evaluation.foamPhase = foamPhase;
	evaluation.foamAngularVelocity = foamAngularVelocity;
	return true;
}

#endif
