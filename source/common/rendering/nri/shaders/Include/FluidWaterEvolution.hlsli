#ifndef NRI_FLUID_WATER_EVOLUTION_HLSLI
#define NRI_FLUID_WATER_EVOLUTION_HLSLI

// Authored pan owns unbounded travel. Wave water alone also evolves locally
// in that moving frame; other styles and never-panned planes bypass exactly.
// Included after FluidHash01/FluidValueNoise in the D3D evaluator.
float FluidWaterEvolutionClock(FluidBindingData binding, uint panFlags,
	uint activeFlag, float gameplaySeconds, float phaseSeconds)
{
	[branch]
	if ((binding.flags & NRI_FLUID_BINDING_FLAG_WATER_EVOLUTION) == 0u ||
		(panFlags & activeFlag) == 0u)
		return 0.0;
	const float clock = (gameplaySeconds - phaseSeconds) * binding.motion.x;
	return isfinite(clock) ? clock : 0.0;
}

float FluidEvolveWaterPhase(float phase, float clock, uint layerSeed, uint layer)
{
	[branch]
	if (clock == 0.0)
		return phase;
	// Time-only variation preserves the carrier's spatial phase derivative.
	// Distinct seeded octave phases/rates create rolling interference, not a
	// second unbounded current. Smooth onset preserves the first-motion anchor.
	const float envelope = smoothstep(0.0, 1.0, abs(clock));
	const float rate = 0.55 + 0.12 * (float)layer;
	const float offset = 0.9 * envelope * sin(clock * rate +
		6.28318530718 * FluidHash01(layerSeed ^ 0x68bc21ebu));
	return phase + offset;
}

float FluidWaterEvolvingNoise(float2 coordinate, uint seed, float clock)
{
	[branch]
	if (clock == 0.0)
		return FluidValueNoise(coordinate, seed);
	// Morph the noise values, not their coordinates: foam reshapes at a fixed
	// material point instead of adding another scrolling decal. Different mask
	// seeds stagger their rates; cubic interpolation has zero endpoint velocity.
	const float rate = 0.25 + 0.10 * FluidHash01(seed ^ 0x02e5be93u);
	const float time = fmod(abs(clock) * rate, 4096.0);
	const uint epoch = (uint)floor(time);
	const uint nextEpoch = (epoch + 1u) & 4095u;
	const float fraction = frac(time);
	const float blend = fraction * fraction * (3.0 - 2.0 * fraction);
	const float currentNoise = FluidValueNoise(coordinate, seed ^ (epoch * 0x9e3779b9u));
	const float nextNoise = FluidValueNoise(coordinate, seed ^ (nextEpoch * 0x9e3779b9u));
	return lerp(currentNoise, nextNoise, blend);
}

#endif
