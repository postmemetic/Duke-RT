#include "nri_smoke_transient_clouds.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
constexpr float Pi = 3.14159265359f;

bool Finite3(const float value[3])
{
	return std::isfinite(value[0]) && std::isfinite(value[1]) &&
		std::isfinite(value[2]);
}

bool ValidClass(NRISmokeTransientClass value)
{
	return static_cast<uint32_t>(value) <=
		static_cast<uint32_t>(NRISmokeTransientClass::Diagnostic);
}

bool ValidRefresh(NRISmokeTransientLightRefresh value)
{
	return value == NRISmokeTransientLightRefresh::Frozen ||
		value == NRISmokeTransientLightRefresh::Slow;
}

uint32_t NextGeneration(uint32_t generation)
{
	++generation;
	return generation == 0u ? 1u : generation;
}

uint16_t NextRevision(uint16_t revision)
{
	++revision;
	return revision == 0u ? 1u : revision;
}

uint32_t Hash32(uint64_t seed, uint32_t index, uint32_t stream)
{
	uint64_t value = seed ^ (static_cast<uint64_t>(index) << 32u) ^
		static_cast<uint64_t>(stream) * 0x9e3779b97f4a7c15ull;
	value ^= value >> 30u;
	value *= 0xbf58476d1ce4e5b9ull;
	value ^= value >> 27u;
	value *= 0x94d049bb133111ebull;
	value ^= value >> 31u;
	return static_cast<uint32_t>(value ^ (value >> 32u));
}

float UnitFloat(uint64_t seed, uint32_t index, uint32_t stream)
{
	return static_cast<float>(Hash32(seed, index, stream) >> 8u) *
		(1.0f / 16777216.0f);
}

float Smooth01(float value)
{
	value = std::clamp(value, 0.0f, 1.0f);
	return value * value * (3.0f - 2.0f * value);
}

float Length3(const float value[3])
{
	return std::sqrt(value[0] * value[0] + value[1] * value[1] +
		value[2] * value[2]);
}

void Normalize3(const float value[3], const float fallback[3], float output[3])
{
	const float length = Length3(value);
	if (length > 1.0e-6f)
	{
		for (uint32_t axis = 0u; axis < 3u; ++axis) output[axis] = value[axis] / length;
	}
	else
	{
		for (uint32_t axis = 0u; axis < 3u; ++axis) output[axis] = fallback[axis];
	}
}

void Cross3(const float a[3], const float b[3], float output[3])
{
	output[0] = a[1] * b[2] - a[2] * b[1];
	output[1] = a[2] * b[0] - a[0] * b[2];
	output[2] = a[0] * b[1] - a[1] * b[0];
}

float SupportRadius(const NRISmokeTransientLobeRequest& request, double groupAgeSeconds)
{
	const float localAge = std::max(static_cast<float>(groupAgeSeconds) -
		request.lobeDelaySeconds, 0.0f);
	const float normalizedAge = std::clamp(localAge / request.lifetimeSeconds,
		0.0f, 1.0f);
	const float shapedAge = std::pow(normalizedAge, request.radiusExponent);
	return std::max(request.initialRadius + request.expansionVelocity *
		(request.lifetimeSeconds * shapedAge), 0.001f);
}

float DensityEnvelope(const NRISmokeTransientLobeRequest& request,
	double groupAgeSeconds)
{
	const float localAge = static_cast<float>(groupAgeSeconds) -
		request.lobeDelaySeconds;
	if (localAge < 0.0f || localAge >= request.lifetimeSeconds) return 0.0f;

	const float attack = request.densityAttackSeconds > 0.0f
		? Smooth01(localAge / request.densityAttackSeconds) : 1.0f;
	if (request.densityReleaseSeconds <= 0.0f)
		return attack * std::exp2(-localAge / request.densityHalfLife);

	const float releaseStart = std::clamp(request.densitySustainSeconds,
		request.densityAttackSeconds, request.lifetimeSeconds);
	if (localAge <= releaseStart) return attack;
	const float releaseDuration = std::min(request.densityReleaseSeconds,
		request.lifetimeSeconds - releaseStart);
	if (releaseDuration <= 0.0f) return 0.0f;
	return attack * (1.0f - Smooth01((localAge - releaseStart) / releaseDuration));
}

bool InitialLightMature(const NRISmokeTransientLobeRequest& request,
	double groupAgeSeconds)
{
	const float localAge = static_cast<float>(groupAgeSeconds) -
		request.lobeDelaySeconds;
	if (localAge < 0.0f) return false;
	if (request.densityAttackSeconds <= 0.0f) return true;
	return Smooth01(localAge / request.densityAttackSeconds) >= 0.9f;
}

float LobeSupportExtent(const NRISmokeTransientLobeRequest& request, float radius)
{
	if (request.shape == 0u) return radius;
	return radius + Length3(request.halfAxisU) + Length3(request.halfAxisV);
}

uint32_t ClassPriority(NRISmokeTransientClass transientClass)
{
	switch (transientClass)
	{
	case NRISmokeTransientClass::Explosion: return 0u;
	case NRISmokeTransientClass::TrailChunk: return 1u;
	case NRISmokeTransientClass::Muzzle: return 2u;
	case NRISmokeTransientClass::Impact: return 3u;
	case NRISmokeTransientClass::FirePacket: return 4u;
	case NRISmokeTransientClass::Diagnostic: return 5u;
	}
	return 5u;
}

bool ValidShapeInput(const NRISmokeTransientGroupShapeInput& input)
{
	return Finite3(input.position) && Finite3(input.velocity) && Finite3(input.up) &&
		Finite3(input.trailAxis) && std::isfinite(input.trailSpan) && input.trailSpan >= 0.0f &&
		Finite3(input.halfAxisU) && Finite3(input.halfAxisV) && input.shape <= 1u &&
		input.requestedLobeCount > 0u && std::isfinite(input.initialRadius) &&
		input.initialRadius > 0.0f && std::isfinite(input.initialDensity) &&
		input.initialDensity > 0.0f && std::isfinite(input.opticalAmount) &&
		input.opticalAmount > 0.0f && std::isfinite(input.expansionVelocity) &&
		std::isfinite(input.densityHalfLife) && input.densityHalfLife > 0.0f &&
		std::isfinite(input.lobeLifetimeSeconds) && input.lobeLifetimeSeconds > 0.0f &&
		std::isfinite(input.groupLifetimeSeconds) && input.groupLifetimeSeconds > 0.0f &&
		std::isfinite(input.maximumLatencySeconds) && input.maximumLatencySeconds >= 0.0f &&
		std::isfinite(input.authoredGameplaySeconds) &&
		std::isfinite(input.densityAttackSeconds) && input.densityAttackSeconds >= 0.0f &&
		std::isfinite(input.densitySustainSeconds) && input.densitySustainSeconds >= 0.0f &&
		std::isfinite(input.densityReleaseSeconds) && input.densityReleaseSeconds >= 0.0f &&
		std::isfinite(input.radiusExponent) && input.radiusExponent > 0.0f &&
		std::isfinite(input.intrinsicEmission) && input.intrinsicEmission >= 0.0f &&
		std::isfinite(input.emissionHalfLife) && input.emissionHalfLife > 0.0f &&
		std::isfinite(input.clusterSpread) && input.clusterSpread >= 0.0f &&
		std::isfinite(input.lobeRadiusMinScale) && input.lobeRadiusMinScale > 0.0f &&
		std::isfinite(input.lobeRadiusMaxScale) &&
		input.lobeRadiusMaxScale >= input.lobeRadiusMinScale &&
		std::isfinite(input.riseVelocity) && std::isfinite(input.curlVelocity) &&
		std::isfinite(input.lobeDelayStepSeconds) && input.lobeDelayStepSeconds >= 0.0f &&
		std::isfinite(input.corePlateau) && input.corePlateau >= 0.0f &&
		input.corePlateau <= 0.95f && std::isfinite(input.edgeErosion) &&
		input.edgeErosion >= 0.0f && input.edgeErosion <= 0.9f &&
		std::isfinite(input.noiseScale) && input.noiseScale > 0.0f &&
		std::isfinite(input.noiseStrength) && input.noiseStrength >= 0.0f &&
		input.noiseStrength <= 0.8f && ValidClass(input.transientClass) &&
		ValidRefresh(input.lightRefresh);
}
}

uint32_t NRIBuildSmokeTransientLobes(const NRISmokeTransientGroupShapeInput& input,
	NRISmokeTransientLobeRequest* output, uint32_t outputCapacity)
{
	if (output == nullptr || outputCapacity == 0u || !ValidShapeInput(input)) return 0u;
	const uint32_t count = std::min({ input.requestedLobeCount, outputCapacity,
		NRISmokeTransientClouds::FixedMaximumLobesPerGroup });
	if (count == 0u) return 0u;
	const bool continuousFire = input.transientClass == NRISmokeTransientClass::FirePacket &&
		input.lobeDelayStepSeconds > 0.0f;
	const float delayStep = continuousFire ? input.lobeDelayStepSeconds *
		static_cast<float>(std::min(input.requestedLobeCount,
			NRISmokeTransientClouds::FixedMaximumLobesPerGroup)) / static_cast<float>(count)
		: input.lobeDelayStepSeconds;
	if (continuousFire && (!std::isfinite(delayStep) ||
		delayStep * static_cast<float>(count - 1u) + input.densityAttackSeconds >=
			input.groupLifetimeSeconds)) return 0u;

	const bool explicitTrail = input.transientClass == NRISmokeTransientClass::TrailChunk &&
		input.trailSpan > 0.0f;
	const float* forwardSource = explicitTrail && Length3(input.trailAxis) > 1.0e-6f
		? input.trailAxis : input.velocity;
	const float forwardFallback[3] = { 0.0f, 0.0f, 1.0f };
	float forward[3] = {};
	Normalize3(forwardSource, forwardFallback, forward);
	const float upFallback[3] = { 0.0f, 1.0f, 0.0f };
	float authoredUp[3] = {};
	Normalize3(input.up, upFallback, authoredUp);
	float rightCandidate[3] = {};
	Cross3(forward, authoredUp, rightCandidate);
	if (Length3(rightCandidate) <= 1.0e-6f)
	{
		const float alternate[3] = { 1.0f, 0.0f, 0.0f };
		Cross3(forward, alternate, rightCandidate);
	}
	const float rightFallback[3] = { 1.0f, 0.0f, 0.0f };
	float right[3] = {};
	Normalize3(rightCandidate, rightFallback, right);
	float cloudUpCandidate[3] = {};
	Cross3(right, forward, cloudUpCandidate);
	float cloudUp[3] = {};
	Normalize3(cloudUpCandidate, upFallback, cloudUp);
	// Fire rings live in the plane perpendicular to gravity. Deriving that plane
	// from the full source velocity would tip curl vertically, and a vertical source
	// velocity would make the projected-up fallback point sideways.
	float fireForwardCandidate[3] = {};
	float forwardUpDot = 0.0f;
	for (uint32_t axis = 0u; axis < 3u; ++axis)
		forwardUpDot += forward[axis] * authoredUp[axis];
	for (uint32_t axis = 0u; axis < 3u; ++axis)
		fireForwardCandidate[axis] = forward[axis] - authoredUp[axis] * forwardUpDot;
	if (Length3(fireForwardCandidate) <= 1.0e-6f)
	{
		const float preferredForward[3] = { 0.0f, 0.0f, 1.0f };
		float preferredUpDot = 0.0f;
		for (uint32_t axis = 0u; axis < 3u; ++axis)
			preferredUpDot += preferredForward[axis] * authoredUp[axis];
		for (uint32_t axis = 0u; axis < 3u; ++axis)
			fireForwardCandidate[axis] = preferredForward[axis] -
				authoredUp[axis] * preferredUpDot;
	}
	const float fireForwardFallback[3] = { 1.0f, 0.0f, 0.0f };
	float fireForward[3] = {};
	Normalize3(fireForwardCandidate, fireForwardFallback, fireForward);
	float fireRightCandidate[3] = {};
	Cross3(fireForward, authoredUp, fireRightCandidate);
	float fireRight[3] = {};
	Normalize3(fireRightCandidate, rightFallback, fireRight);

	const uint64_t seed = static_cast<uint64_t>(input.deterministicSeed) ^
		input.sourceEventSerial ^ (static_cast<uint64_t>(input.sourceId) << 32u);
	const bool explosion = input.transientClass == NRISmokeTransientClass::Explosion;
	const uint32_t explosionBinderCount = explosion
		? std::clamp((count + 3u) / 4u, 1u, 3u) : 0u;
	auto explosionRadiusScale = [&](uint32_t index)
	{
		float radiusUnit = UnitFloat(seed, index, 3u);
		if (index < explosionBinderCount)
			radiusUnit = 0.72f + 0.28f * radiusUnit;
		return input.lobeRadiusMinScale +
			(input.lobeRadiusMaxScale - input.lobeRadiusMinScale) * radiusUnit;
	};
	const float primaryExplosionBinderRadius = explosion
		? input.initialRadius * explosionRadiusScale(0u) : 0.0f;
	const float trailJitter = explicitTrail ? input.initialRadius *
		input.clusterSpread * 0.08f : 0.0f;
	const float trailSpacing = explicitTrail ? (count > 1u
		? input.trailSpan / static_cast<float>(count - 1u) : input.trailSpan) : 0.0f;
	const float trailMinimumRadius = explicitTrail ? 0.5f * std::sqrt(
		trailSpacing * trailSpacing + 4.0f * trailJitter * trailJitter) / 0.9f : 0.0f;
	float explosionEmissionFactors[NRISmokeTransientClouds::FixedMaximumLobesPerGroup] = {};
	if (explosion)
	{
		float binderFactorSum = 0.0f;
		float satelliteFactorSum = 0.0f;
		for (uint32_t index = 0u; index < count; ++index)
		{
			if (index < explosionBinderCount)
			{
				explosionEmissionFactors[index] = 0.85f +
					0.3f * UnitFloat(seed, index, 5u);
				binderFactorSum += explosionEmissionFactors[index];
			}
			else
			{
				explosionEmissionFactors[index] =
					0.1f * UnitFloat(seed, index, 5u);
				satelliteFactorSum += explosionEmissionFactors[index];
			}
		}
		// Equal optical weights make the target factor sum exactly the lobe count.
		// Concentrating the remainder in the connected binders keeps the shell cool
		// while retaining the authored density-weighted source amount exactly.
		const float binderScale = (static_cast<float>(count) - satelliteFactorSum) /
			std::max(binderFactorSum, 1.0e-6f);
		for (uint32_t index = 0u; index < explosionBinderCount; ++index)
			explosionEmissionFactors[index] *= binderScale;
	}
	for (uint32_t index = 0u; index < count; ++index)
	{
		const float unit = (static_cast<float>(index) + 0.5f) /
			static_cast<float>(count);
		const float phase = UnitFloat(seed, index, 0u) * (2.0f * Pi);
		const float radiusUnit = UnitFloat(seed, index, 3u);
		const float radiusScale = explosion ? explosionRadiusScale(index) :
			input.lobeRadiusMinScale +
			(input.lobeRadiusMaxScale - input.lobeRadiusMinScale) * radiusUnit;
		const float lobeInitialRadius = input.initialRadius * radiusScale;
		const float ringX = std::cos(phase);
		const float ringY = std::sin(phase);
		float radial[3] = {};
		for (uint32_t axis = 0u; axis < 3u; ++axis)
			radial[axis] = right[axis] * ringX + cloudUp[axis] * ringY;
		float offset[3] = {};
		float velocityDelta[3] = {};
		switch (input.transientClass)
		{
		case NRISmokeTransientClass::Explosion:
		{
			const float buoyantVelocity = input.riseVelocity +
				std::max(input.expansionVelocity, 0.0f) * 0.08f;
			if (index < explosionBinderCount)
			{
				// A small set of coincident binders is the persistent connected/hot core.
				// Keeping their relative motion zero makes them safe topology anchors for
				// deterministic owner-side lobe reduction.
				for (uint32_t axis = 0u; axis < 3u; ++axis)
					velocityDelta[axis] = authoredUp[axis] * buoyantVelocity;
			}
			else
			{
				const uint32_t satelliteIndex = index - explosionBinderCount;
				const uint32_t satelliteCount = count - explosionBinderCount;
				const float satelliteUnit = (static_cast<float>(satelliteIndex) + 0.5f) /
					static_cast<float>(satelliteCount);
				const float satelliteZ = 1.0f - 2.0f * satelliteUnit;
				const float satellitePlanar = std::sqrt(std::max(
					1.0f - satelliteZ * satelliteZ, 0.0f));
				const float satellitePhase = UnitFloat(seed, index, 0u) * (2.0f * Pi);
				const float satelliteRingX = std::cos(satellitePhase);
				const float satelliteRingY = std::sin(satellitePhase);
				float direction[3] = {};
				for (uint32_t axis = 0u; axis < 3u; ++axis)
				{
					const float satelliteRadial = right[axis] * satelliteRingX +
						cloudUp[axis] * satelliteRingY;
					direction[axis] = satelliteRadial * satellitePlanar +
						forward[axis] * satelliteZ;
				}
				const float spreadActivation = std::clamp(input.clusterSpread / 0.55f,
					0.0f, 1.0f);
				const float ownRadiusOffset = lobeInitialRadius *
					(0.85f + 0.25f * UnitFloat(seed, index, 1u));
				const float authoredOffset = input.initialRadius * input.clusterSpread *
					(0.65f + 0.35f * UnitFloat(seed, index, 2u));
				const float connectedOffset = 0.88f *
					(primaryExplosionBinderRadius + lobeInitialRadius);
				const float offsetDistance = std::min(
					std::max(ownRadiusOffset, authoredOffset) * spreadActivation,
					connectedOffset);
				const float positiveGrowth = std::max(
					input.expansionVelocity * radiusScale, 0.0f);
				const float curlFactor = positiveGrowth > 1.0e-6f
					? 0.05f * input.curlVelocity / positiveGrowth : 0.0f;
				const float outwardVelocity = positiveGrowth * std::clamp(
					0.9f + 0.1f * UnitFloat(seed, index, 6u) + curlFactor,
					0.9f, 1.05f);
				for (uint32_t axis = 0u; axis < 3u; ++axis)
				{
					offset[axis] = direction[axis] * offsetDistance;
					velocityDelta[axis] = direction[axis] * outwardVelocity +
						authoredUp[axis] * buoyantVelocity;
				}
			}
			break;
		}
		case NRISmokeTransientClass::Diagnostic:
		{
			const float z = 1.0f - 2.0f * unit;
			const float planar = std::sqrt(std::max(1.0f - z * z, 0.0f));
			for (uint32_t axis = 0u; axis < 3u; ++axis)
			{
				const float direction = radial[axis] * planar + forward[axis] * z;
				offset[axis] = direction * input.initialRadius * input.clusterSpread *
					(0.45f + 0.55f * UnitFloat(seed, index, 1u));
				velocityDelta[axis] = direction * input.curlVelocity;
			}
			break;
		}
		case NRISmokeTransientClass::TrailChunk:
		{
			const float along = explicitTrail ? (count > 1u
				? -0.5f * input.trailSpan + input.trailSpan *
					static_cast<float>(index) / static_cast<float>(count - 1u) : 0.0f)
				: (unit - 0.5f) * 2.0f * input.initialRadius * input.clusterSpread;
			const float jitter = explicitTrail ? trailJitter :
				input.initialRadius * input.clusterSpread * 0.18f;
			for (uint32_t axis = 0u; axis < 3u; ++axis)
			{
				offset[axis] = forward[axis] * along + radial[axis] * jitter;
				velocityDelta[axis] = radial[axis] * input.curlVelocity * 0.25f;
			}
			break;
		}
		case NRISmokeTransientClass::FirePacket:
		{
			const float ringRadius = input.initialRadius * input.clusterSpread *
				(0.22f + 0.28f * unit);
			for (uint32_t axis = 0u; axis < 3u; ++axis)
			{
				const float fireRadial = fireRight[axis] * ringX +
					fireForward[axis] * ringY;
				const float fireTangent = fireRight[axis] * -ringY +
					fireForward[axis] * ringX;
				offset[axis] = fireRadial * ringRadius + authoredUp[axis] *
					(continuousFire ? 0.0f : input.initialRadius * input.clusterSpread * 1.6f * unit);
				velocityDelta[axis] = authoredUp[axis] * input.riseVelocity +
					fireTangent * input.curlVelocity;
			}
			break;
		}
		case NRISmokeTransientClass::Muzzle:
		{
			for (uint32_t axis = 0u; axis < 3u; ++axis)
			{
				offset[axis] = forward[axis] * input.initialRadius *
					input.clusterSpread * unit + radial[axis] * input.initialRadius *
					input.clusterSpread * 0.22f * unit;
				velocityDelta[axis] = radial[axis] * input.curlVelocity * unit;
			}
			break;
		}
		case NRISmokeTransientClass::Impact:
		{
			const float outward = 0.3f + 0.7f * UnitFloat(seed, index, 2u);
			for (uint32_t axis = 0u; axis < 3u; ++axis)
			{
				const float direction = forward[axis] * outward + radial[axis] *
					std::sqrt(std::max(1.0f - outward * outward, 0.0f));
				offset[axis] = direction * input.initialRadius * input.clusterSpread * unit;
				velocityDelta[axis] = direction * input.curlVelocity;
			}
			break;
		}
		}
		// Explosion and fire author their class-specific buoyant/axial motion above.
		// Every other transient class retains its shaped motion while applying the
		// common authored rise once in the normalized world-up direction.
		if (input.transientClass != NRISmokeTransientClass::Explosion &&
			input.transientClass != NRISmokeTransientClass::FirePacket)
		{
			for (uint32_t axis = 0u; axis < 3u; ++axis)
				velocityDelta[axis] += authoredUp[axis] * input.riseVelocity;
		}

		NRISmokeTransientLobeRequest request = {};
		for (uint32_t axis = 0u; axis < 3u; ++axis)
		{
			request.position[axis] = input.position[axis] + offset[axis];
			request.velocity[axis] = input.velocity[axis] + velocityDelta[axis];
			request.halfAxisU[axis] = input.halfAxisU[axis];
			request.halfAxisV[axis] = input.halfAxisV[axis];
		}
		request.initialRadius = lobeInitialRadius;
		if (explicitTrail)
			request.initialRadius = std::max(request.initialRadius, trailMinimumRadius);
		request.initialDensity = input.initialDensity;
		request.opticalWeight = input.opticalAmount / static_cast<float>(count);
		request.shape = input.shape;
		request.expansionVelocity = input.transientClass ==
			NRISmokeTransientClass::Explosion
			? input.expansionVelocity * radiusScale : input.expansionVelocity;
		request.densityHalfLife = input.densityHalfLife;
		request.lifetimeSeconds = input.lobeLifetimeSeconds;
		request.styleIndex = input.styleIndex;
		request.sourceId = input.sourceId;
		request.epoch = input.epoch;
		request.authoredGameplaySeconds = input.authoredGameplaySeconds;
		request.maximumLatencySeconds = input.maximumLatencySeconds;
		request.burstBorrow = input.burstBorrow;
		request.sourceEventSerial = input.sourceEventSerial;
		request.replacementKey = input.replacementKey;
		request.batchIndex = index;
		request.batchCount = count;
		request.groupLifetimeSeconds = input.groupLifetimeSeconds;
		request.lobeDelaySeconds = delayStep * static_cast<float>(index);
		if (continuousFire)
		{
			// Delayed births share the existing packet lifetime/capacity budget.
			// Shorten the local life, not the fade at group retirement. DensityEnvelope
			// fits the authored release into this remaining local interval.
			request.lifetimeSeconds = std::min(request.lifetimeSeconds,
				std::max(input.groupLifetimeSeconds - request.lobeDelaySeconds, 0.001f));
		}
		request.densityAttackSeconds = input.densityAttackSeconds;
		request.densitySustainSeconds = input.densitySustainSeconds;
		request.densityReleaseSeconds = input.densityReleaseSeconds;
		request.radiusExponent = input.radiusExponent;
		if (explosion)
		{
			request.intrinsicEmission = input.intrinsicEmission *
				explosionEmissionFactors[index];
			request.emissionHalfLife = input.emissionHalfLife *
				(0.8f + 0.4f * radiusUnit);
		}
		else
		{
			request.intrinsicEmission = input.intrinsicEmission;
			request.emissionHalfLife = input.emissionHalfLife;
		}
		request.corePlateau = explosion && index >= explosionBinderCount
			? std::min(input.corePlateau, 0.4f) : input.corePlateau;
		request.edgeErosion = input.edgeErosion;
		request.noiseScale = input.noiseScale;
		request.noiseStrength = input.noiseStrength;
		request.deterministicSeed = Hash32(seed, index, 4u);
		request.transientClass = input.transientClass;
		request.lightRefresh = input.lightRefresh;
		output[index] = request;
	}
	return count;
}

NRISmokeTransientProfile NRISmokeTransientClouds::ProfileForQuality(
	uint32_t workProfile)
{
	NRISmokeTransientProfile profile = {};
	switch (static_cast<NRISmokeTransientQuality>(workProfile))
	{
	case NRISmokeTransientQuality::Reference:
		profile.maximumLobesPerGroup = 16u;
		profile.minimumReducedLobes = 16u;
		profile.maximumFullLightBuilds = 64u;
		profile.anchorsPerGroup = 4u;
		profile.samplesPerAnchor = 8u;
		profile.fireRefreshSeconds = 0.0f;
		profile.allowSlowFireRefresh = false;
		break;
	case NRISmokeTransientQuality::High:
		profile.maximumLobesPerGroup = 16u;
		profile.minimumReducedLobes = 6u;
		profile.maximumFullLightBuilds = 16u;
		profile.anchorsPerGroup = 4u;
		profile.samplesPerAnchor = 4u;
		profile.fireRefreshSeconds = 0.25f;
		break;
	case NRISmokeTransientQuality::Medium:
		break;
	case NRISmokeTransientQuality::Low:
	default:
		profile.maximumActiveGroups = 24u;
		profile.maximumActiveLobes = 96u;
		profile.maximumLobesPerGroup = 4u;
		profile.minimumReducedLobes = 2u;
		profile.maximumFullLightBuilds = 2u;
		profile.anchorsPerGroup = 2u;
		profile.samplesPerAnchor = 1u;
		profile.fireRefreshSeconds = 0.0f;
		profile.allowSlowFireRefresh = false;
		break;
	}
	return profile;
}

uint32_t NRISmokeTransientClouds::DefaultLobeCountForQuality(uint32_t workProfile,
	NRISmokeTransientClass transientClass)
{
	static constexpr uint32_t Counts[4][6] = {
		{ 16u, 12u, 16u, 8u, 8u, 16u },
		{ 16u, 8u, 12u, 6u, 6u, 12u },
		{ 12u, 6u, 8u, 4u, 4u, 8u },
		{ 4u, 3u, 4u, 2u, 2u, 3u },
	};
	const uint32_t profileIndex = std::min(workProfile, 3u);
	const uint32_t classIndex = ValidClass(transientClass)
		? static_cast<uint32_t>(transientClass)
		: static_cast<uint32_t>(NRISmokeTransientClass::Diagnostic);
	return Counts[profileIndex][classIndex];
}

bool NRISmokeTransientClouds::Valid(const NRISmokeTransientLobeRequest& request)
{
	return Finite3(request.position) && Finite3(request.velocity) &&
		Finite3(request.halfAxisU) && Finite3(request.halfAxisV) &&
		request.shape <= 1u && std::isfinite(request.initialRadius) &&
		request.initialRadius > 0.0f && std::isfinite(request.initialDensity) &&
		request.initialDensity > 0.0f && std::isfinite(request.opticalWeight) &&
		request.opticalWeight > 0.0f && std::isfinite(request.expansionVelocity) &&
		std::isfinite(request.densityHalfLife) && request.densityHalfLife > 0.0f &&
		std::isfinite(request.lifetimeSeconds) && request.lifetimeSeconds > 0.0f &&
		std::isfinite(request.authoredGameplaySeconds) &&
		std::isfinite(request.maximumLatencySeconds) && request.maximumLatencySeconds >= 0.0f &&
		request.batchCount > 0u && request.batchIndex < request.batchCount &&
		std::isfinite(request.groupLifetimeSeconds) && request.groupLifetimeSeconds > 0.0f &&
		std::isfinite(request.lobeDelaySeconds) && request.lobeDelaySeconds >= 0.0f &&
		std::isfinite(request.densityAttackSeconds) && request.densityAttackSeconds >= 0.0f &&
		std::isfinite(request.densitySustainSeconds) && request.densitySustainSeconds >= 0.0f &&
		std::isfinite(request.densityReleaseSeconds) && request.densityReleaseSeconds >= 0.0f &&
		std::isfinite(request.radiusExponent) && request.radiusExponent > 0.0f &&
		std::isfinite(request.intrinsicEmission) && request.intrinsicEmission >= 0.0f &&
		std::isfinite(request.emissionHalfLife) && request.emissionHalfLife > 0.0f &&
		std::isfinite(request.corePlateau) && request.corePlateau >= 0.0f &&
		request.corePlateau <= 0.95f && std::isfinite(request.edgeErosion) &&
		request.edgeErosion >= 0.0f && request.edgeErosion <= 0.9f &&
		std::isfinite(request.noiseScale) && request.noiseScale > 0.0f &&
		std::isfinite(request.noiseStrength) && request.noiseStrength >= 0.0f &&
		request.noiseStrength <= 0.8f && ValidClass(request.transientClass) &&
		ValidRefresh(request.lightRefresh);
}

bool NRISmokeTransientClouds::ValidBatchIdentity(
	const NRISmokeTransientLobeRequest* requests, uint32_t count)
{
	if (requests == nullptr || count == 0u) return false;
	const NRISmokeTransientLobeRequest& first = requests[0];
	for (uint32_t index = 0u; index < count; ++index)
	{
		const NRISmokeTransientLobeRequest& request = requests[index];
		if (request.batchIndex != index || request.batchCount != count ||
			request.sourceId != first.sourceId || request.epoch != first.epoch ||
			request.sourceEventSerial != first.sourceEventSerial ||
			request.replacementKey != first.replacementKey ||
			request.authoredGameplaySeconds != first.authoredGameplaySeconds ||
			request.maximumLatencySeconds != first.maximumLatencySeconds ||
			request.groupLifetimeSeconds != first.groupLifetimeSeconds ||
			request.transientClass != first.transientClass ||
			request.lightRefresh != first.lightRefresh)
			return false;
		if (request.burstBorrow != first.burstBorrow)
			return false;
	}
	return true;
}

void NRISmokeTransientClouds::BeginFrame(double gameplayTimeSeconds,
	uint32_t maximumActiveLobes, const NRISmokeTransientProfile& profile)
{
	++mFrameSerial;
	if (mFrameSerial == 0u) ++mFrameSerial;
	mPrepared = std::isfinite(gameplayTimeSeconds);
	if (mPrepared) mGameplayTimeSeconds = gameplayTimeSeconds;
	const uint32_t previousAnchors = mProfile.anchorsPerGroup;
	const uint32_t previousSamples = mProfile.samplesPerAnchor;
	const bool previousEnabled = mProfile.enabled;
	mProfile = profile;
	mProfile.maximumActiveGroups = std::min(mProfile.maximumActiveGroups,
		FixedGroupCapacity);
	mProfile.maximumActiveLobes = std::min({ mProfile.maximumActiveLobes,
		maximumActiveLobes, FixedLobeCapacity });
	mProfile.maximumLobesPerGroup = std::clamp(mProfile.maximumLobesPerGroup,
		1u, FixedMaximumLobesPerGroup);
	mProfile.minimumReducedLobes = std::clamp(mProfile.minimumReducedLobes,
		1u, mProfile.maximumLobesPerGroup);
	mProfile.maximumFullLightBuilds = std::min(mProfile.maximumFullLightBuilds,
		FixedGroupCapacity);
	mProfile.anchorsPerGroup = std::clamp(mProfile.anchorsPerGroup, 1u, 4u);
	mProfile.samplesPerAnchor = std::clamp(mProfile.samplesPerAnchor, 1u, 8u);
	if (!std::isfinite(mProfile.fireRefreshSeconds) || mProfile.fireRefreshSeconds < 0.0f)
		mProfile.fireRefreshSeconds = 0.0f;
	const bool lightingPolicyChanged = mHasProfile &&
		(previousAnchors != mProfile.anchorsPerGroup ||
			previousSamples != mProfile.samplesPerAnchor ||
			previousEnabled != mProfile.enabled);
	if (lightingPolicyChanged) InvalidateLightingState();
	mHasProfile = true;
	mSnapshot.maximumActiveGroups = mProfile.maximumActiveGroups;
	mSnapshot.maximumActiveLobes = mProfile.maximumActiveLobes;
	mLightBudget.maximumFullBuildsPerFrame = mProfile.maximumFullLightBuilds;
	mLightBudget.anchorsPerGroup = mProfile.anchorsPerGroup;
	mLightBudget.samplesPerAnchor = mProfile.samplesPerAnchor;
	mLightBudget.maximumVisibilityQueriesPerGroup = mProfile.anchorsPerGroup *
		(mProfile.samplesPerAnchor + mLightBudget.maximumPointLightsPerAnchor +
			mLightBudget.maximumDirectionalLightsPerAnchor);
	mLightBudget.maximumVisibilityQueriesPerFrame = mProfile.maximumFullLightBuilds *
		mLightBudget.maximumVisibilityQueriesPerGroup;
	mLightBudget.fireRefreshSeconds = mProfile.fireRefreshSeconds;
	mLightBudget.slowFireRefreshEnabled = mProfile.allowSlowFireRefresh &&
		mProfile.fireRefreshSeconds > 0.0f;
	mLastDropReason = NRISmokeTransientDropReason::None;
	if (!mPrepared)
	{
		mGpuLobes.clear();
		mGpuGroups.clear();
		mSnapshot.visibleGroups = 0u;
		mSnapshot.visibleLobes = 0u;
		return;
	}
	Refresh();
}

NRISmokeTransientAdmission NRISmokeTransientClouds::Admit(
	const NRISmokeTransientLobeRequest& request)
{
	return AdmitBatch(&request, 1u);
}

NRISmokeTransientAdmission NRISmokeTransientClouds::AdmitLatest(
	const NRISmokeTransientLobeRequest& request)
{
	if (request.replacementKey == 0u) return Admit(request);
	for (uint32_t groupIndex = 0u; groupIndex < mGroups.size(); ++groupIndex)
	{
		GroupSlot& previousGroup = mGroups[groupIndex];
		if (!previousGroup.active || previousGroup.replacementKey !=
			request.replacementKey) continue;

		mSnapshot.groupsRequested++;
		mSnapshot.lobesRequested++;
		if (!mPrepared) return Drop(NRISmokeTransientDropReason::NotPrepared, 1u);
		if (!mProfile.enabled || mProfile.maximumActiveGroups == 0u ||
			mProfile.maximumActiveLobes == 0u)
			return Drop(NRISmokeTransientDropReason::Disabled, 1u);
		if (request.epoch != mSnapshot.epoch)
			return Drop(NRISmokeTransientDropReason::StaleEpoch, 1u);
		if (!Valid(request) || request.batchIndex != 0u || request.batchCount != 1u ||
			previousGroup.lobeCount != 1u)
			return Drop(NRISmokeTransientDropReason::InvalidRequest, 1u);
		const double age = mGameplayTimeSeconds - request.authoredGameplaySeconds;
		if (age >= request.groupLifetimeSeconds ||
			age >= static_cast<double>(request.lobeDelaySeconds + request.lifetimeSeconds))
			return Drop(NRISmokeTransientDropReason::ExpiredOnArrival, 1u);
		if (request.maximumLatencySeconds > 0.0f &&
			age > request.maximumLatencySeconds)
			return Drop(NRISmokeTransientDropReason::StaleOnArrival, 1u);

		const uint32_t lobeIndex = previousGroup.lobes[0];
		LobeSlot& previousLobe = mLobes[lobeIndex];
		const uint32_t groupGeneration = NextGeneration(previousGroup.generation);
		const uint32_t lobeGeneration = NextGeneration(previousLobe.generation);
		GroupSlot replacement = {};
		replacement.lobes[0] = lobeIndex;
		replacement.lobeCount = 1u;
		replacement.generation = groupGeneration;
		replacement.epoch = request.epoch;
		replacement.sourceId = request.sourceId;
		replacement.sourceEventSerial = request.sourceEventSerial;
		replacement.replacementKey = request.replacementKey;
		replacement.authoredGameplaySeconds = request.authoredGameplaySeconds;
		replacement.admissionOrdinal = ++mAdmissionOrdinal;
		replacement.admittedFrame = mFrameSerial;
		replacement.lifetimeSeconds = request.groupLifetimeSeconds;
		replacement.lightingRevision = mLightingRevision;
		replacement.transientClass = request.transientClass;
		replacement.lightRefresh = request.lightRefresh;
		replacement.needsInitialLight = true;
		replacement.active = true;
		previousGroup = replacement;
		previousLobe = {};
		previousLobe.request = request;
		previousLobe.generation = lobeGeneration;
		previousLobe.groupSlot = groupIndex;
		previousLobe.active = true;
		mSnapshot.groupsAdmitted++;
		mSnapshot.lobesAdmitted++;
		mSnapshot.replacements++;
		Refresh();
		return { { groupIndex, groupGeneration, request.epoch },
			NRISmokeTransientDropReason::None, 1u, 1u };
	}
	return Admit(request);
}

NRISmokeTransientAdmission NRISmokeTransientClouds::AdmitBatch(
	const NRISmokeTransientLobeRequest* requests, uint32_t count)
{
	return AdmitBatchWithLimits(requests, count, mProfile.maximumLobesPerGroup,
		mProfile.minimumReducedLobes, false);
}

NRISmokeTransientAdmission NRISmokeTransientClouds::AdmitRetainedBatch(
	const NRISmokeTransientLobeRequest* requests, uint32_t count, uint32_t maximumLobes)
{
	return AdmitBatchWithLimits(requests, count, maximumLobes, 1u, true);
}

bool NRISmokeTransientClouds::ValidateBatch(
	const NRISmokeTransientLobeRequest* requests, uint32_t count)
{
	if (!ValidBatchIdentity(requests, count)) return false;
	for (uint32_t index = 0u; index < count; ++index)
		if (!Valid(requests[index])) return false;
	return true;
}

NRISmokeTransientAdmission NRISmokeTransientClouds::AdmitBatchWithLimits(
	const NRISmokeTransientLobeRequest* requests, uint32_t count,
	uint32_t maximumLobes, uint32_t minimumLobes, bool retained)
{
	mLastDropReason = NRISmokeTransientDropReason::None;
	mSnapshot.groupsRequested++;
	mSnapshot.lobesRequested += count;
	if (!mPrepared) return Drop(NRISmokeTransientDropReason::NotPrepared, count);
	if (!mProfile.enabled || mProfile.maximumActiveGroups == 0u ||
		mProfile.maximumActiveLobes == 0u)
		return Drop(NRISmokeTransientDropReason::Disabled, count);
	if (requests == nullptr || count == 0u)
		return Drop(NRISmokeTransientDropReason::InvalidRequest, count);
	for (uint32_t index = 0u; index < count; ++index)
		if (requests[index].epoch != mSnapshot.epoch)
			return Drop(NRISmokeTransientDropReason::StaleEpoch, count);
	for (uint32_t index = 0u; index < count; ++index)
		if (!Valid(requests[index]))
			return Drop(NRISmokeTransientDropReason::InvalidRequest, count);
	if (!ValidBatchIdentity(requests, count))
		return Drop(NRISmokeTransientDropReason::InvalidRequest, count);

	const double groupAge = mGameplayTimeSeconds - requests[0].authoredGameplaySeconds;
	if (groupAge >= requests[0].groupLifetimeSeconds)
		return Drop(NRISmokeTransientDropReason::ExpiredOnArrival, count);
	if (!retained && requests[0].maximumLatencySeconds > 0.0f &&
		groupAge > requests[0].maximumLatencySeconds)
		return Drop(NRISmokeTransientDropReason::StaleOnArrival, count);
	for (uint32_t index = 0u; index < count; ++index)
		if (!retained && groupAge >= static_cast<double>(requests[index].lobeDelaySeconds +
			requests[index].lifetimeSeconds))
			return Drop(NRISmokeTransientDropReason::ExpiredOnArrival, count);

	uint32_t groupIndex = UINT32_MAX;
	for (uint32_t index = 0u; index < mGroups.size(); ++index)
		if (!mGroups[index].active) { groupIndex = index; break; }
	if (groupIndex == UINT32_MAX || mSnapshot.activeGroups >=
		mProfile.maximumActiveGroups)
		return Drop(NRISmokeTransientDropReason::GroupCapacity, count);

	std::array<uint32_t, FixedLobeCapacity> freeLobes = {};
	uint32_t freeCount = 0u;
	for (uint32_t index = 0u; index < mLobes.size(); ++index)
		if (!mLobes[index].active) freeLobes[freeCount++] = index;
	const uint32_t profileFree = mProfile.maximumActiveLobes > mSnapshot.activeLobes
		? mProfile.maximumActiveLobes - mSnapshot.activeLobes : 0u;
	const uint32_t admitCount = std::min({ count, maximumLobes, mProfile.maximumLobesPerGroup,
		freeCount, profileFree });
	if (admitCount == 0u || (admitCount < count &&
		admitCount < minimumLobes))
		return Drop(NRISmokeTransientDropReason::LobeCapacity, count);

	bool reduceContinuousFire = admitCount < count && count > 1u &&
		requests[0].transientClass == NRISmokeTransientClass::FirePacket &&
		requests[0].lobeDelaySeconds == 0.0f && requests[count - 1u].lobeDelaySeconds > 0.0f;
	const float birthStep = count > 1u ? requests[count - 1u].lobeDelaySeconds /
		static_cast<float>(count - 1u) : 0.0f;
	double fireOpticalQuantity = 0.0;
	double fireIntrinsicSource = 0.0;
	for (uint32_t index = 0u; reduceContinuousFire && index < count; ++index)
	{
		const auto& request = requests[index];
		// Only re-time the builder's uniform, common-end stream. Arbitrarily
		// delayed diagnostic requests retain the generic selection behavior.
		reduceContinuousFire = std::abs(request.lobeDelaySeconds -
			birthStep * static_cast<float>(index)) < 1.0e-5f &&
			std::abs(request.lobeDelaySeconds + request.lifetimeSeconds -
				request.groupLifetimeSeconds) < 1.0e-5f;
		const double optical = static_cast<double>(request.initialDensity) * request.opticalWeight;
		fireOpticalQuantity += optical;
		fireIntrinsicSource += optical * request.intrinsicEmission;
	}
	std::array<NRISmokeTransientLobeRequest, FixedMaximumLobesPerGroup> admitted = {};
	for (uint32_t outputIndex = 0u; outputIndex < admitCount; ++outputIndex)
	{
		const uint32_t begin = static_cast<uint32_t>(
			(static_cast<uint64_t>(outputIndex) * count) / admitCount);
		const uint32_t end = static_cast<uint32_t>(
			(static_cast<uint64_t>(outputIndex + 1u) * count) / admitCount);
		// Explosion batch index zero is the primary connected binder. Retaining it
		// in the first reduction bucket makes every reduced satellite keep its
		// authored admission-time overlap path to the core. In the authored production
		// domain, positive growth and radius exponent at most one retain that path;
		// other classes preserve midpoint selection.
		const uint32_t representative = outputIndex == 0u &&
			requests[0].transientClass == NRISmokeTransientClass::Explosion
			? begin : begin + (end - begin - 1u) / 2u;
		admitted[outputIndex] = requests[representative];
		if (reduceContinuousFire)
		{
			// Re-space continuous births after quality reduction: dropping the last
			// birth would otherwise leave a conspicuous gap at each packet boundary.
			const float span = birthStep * static_cast<float>(count);
			const float localEnd = admitted[outputIndex].lobeDelaySeconds +
				admitted[outputIndex].lifetimeSeconds;
			admitted[outputIndex].lobeDelaySeconds = span *
				static_cast<float>(outputIndex) / static_cast<float>(admitCount);
			admitted[outputIndex].lifetimeSeconds = std::max(
				localEnd - admitted[outputIndex].lobeDelaySeconds, 0.001f);
		}
		double opticalQuantity = 0.0;
		double intrinsicSource = 0.0;
		for (uint32_t inputIndex = begin; inputIndex < end; ++inputIndex)
		{
			opticalQuantity += static_cast<double>(requests[inputIndex].initialDensity) *
				static_cast<double>(requests[inputIndex].opticalWeight);
			intrinsicSource += static_cast<double>(requests[inputIndex].initialDensity) *
				static_cast<double>(requests[inputIndex].opticalWeight) *
				static_cast<double>(requests[inputIndex].intrinsicEmission);
		}
		if (reduceContinuousFire)
		{
			// Equal temporal spacing also requires equal optical/source weight.
			// Bucket merging alone made every fourth Low-profile birth twice as dense.
			opticalQuantity = fireOpticalQuantity / static_cast<double>(admitCount);
			intrinsicSource = fireIntrinsicSource / static_cast<double>(admitCount);
		}
		const double reducedWeight = opticalQuantity /
			static_cast<double>(admitted[outputIndex].initialDensity);
		if (!std::isfinite(reducedWeight) || reducedWeight <= 0.0 ||
			reducedWeight > static_cast<double>(std::numeric_limits<float>::max()))
			return Drop(NRISmokeTransientDropReason::InvalidRequest, count);
		admitted[outputIndex].opticalWeight = static_cast<float>(reducedWeight);
		admitted[outputIndex].intrinsicEmission = static_cast<float>(
			intrinsicSource / opticalQuantity);
		admitted[outputIndex].batchIndex = outputIndex;
		admitted[outputIndex].batchCount = admitCount;
	}

	const uint32_t groupGeneration = NextGeneration(mGroups[groupIndex].generation);
	GroupSlot group = {};
	group.lobeCount = admitCount;
	group.generation = groupGeneration;
	group.epoch = requests[0].epoch;
	group.sourceId = requests[0].sourceId;
	group.sourceEventSerial = requests[0].sourceEventSerial;
	group.replacementKey = requests[0].replacementKey;
	group.authoredGameplaySeconds = requests[0].authoredGameplaySeconds;
	group.admissionOrdinal = ++mAdmissionOrdinal;
	group.admittedFrame = mFrameSerial;
	group.lifetimeSeconds = requests[0].groupLifetimeSeconds;
	group.lightingRevision = mLightingRevision;
	group.transientClass = requests[0].transientClass;
	group.lightRefresh = requests[0].lightRefresh;
	group.needsInitialLight = true;
	group.active = true;
	for (uint32_t outputIndex = 0u; outputIndex < admitCount; ++outputIndex)
	{
		const uint32_t lobeIndex = freeLobes[outputIndex];
		const uint32_t generation = NextGeneration(mLobes[lobeIndex].generation);
		LobeSlot lobe = {};
		lobe.request = admitted[outputIndex];
		lobe.generation = generation;
		lobe.groupSlot = groupIndex;
		lobe.active = true;
		mLobes[lobeIndex] = lobe;
		group.lobes[outputIndex] = lobeIndex;
	}
	mGroups[groupIndex] = group;
	mSnapshot.groupsAdmitted++;
	mSnapshot.lobesAdmitted += admitCount;
	if (admitCount < count)
	{
		mSnapshot.deterministicallyReducedGroups++;
		mSnapshot.deterministicallyReducedLobes += count - admitCount;
	}
	Refresh();
	return { { groupIndex, groupGeneration, requests[0].epoch },
		NRISmokeTransientDropReason::None, admitCount, count };
}

void NRISmokeTransientClouds::CommitLightDispatchSchedule()
{
	if (!mPrepared) return;
	for (GroupSlot& group : mGroups)
	{
		if (!group.active || !group.fullLightAllowed ||
			group.fullLightScheduledFrame != mFrameSerial) continue;
		group.needsInitialLight = false;
		group.lastFullLightScheduleSeconds = mGameplayTimeSeconds;
	}
}

void NRISmokeTransientClouds::InvalidateLightingState()
{
	mLightingRevision = NextRevision(mLightingRevision);
	for (GroupSlot& group : mGroups)
	{
		if (!group.active) continue;
		group.lightingRevision = mLightingRevision;
		group.needsInitialLight = true;
	}
}

void NRISmokeTransientClouds::InvalidateLighting()
{
	InvalidateLightingState();
	if (mPrepared) Refresh();
}

bool NRISmokeTransientClouds::RetireLatest(uint64_t replacementKey)
{
	if (replacementKey == 0u) return false;
	for (uint32_t index = 0u; index < mGroups.size(); ++index)
		if (mGroups[index].active && mGroups[index].replacementKey == replacementKey)
		{
			RetireGroup(index, false);
			mSnapshot.replacementRetirements++;
			Refresh();
			return true;
		}
	return false;
}

bool NRISmokeTransientClouds::IsLive(const NRISmokeTransientHandle& handle) const
{
	if (handle.slot >= mGroups.size()) return false;
	const GroupSlot& group = mGroups[handle.slot];
	return group.active && group.epoch == handle.epoch &&
		group.generation == handle.generation;
}

bool NRISmokeTransientClouds::Release(const NRISmokeTransientHandle& handle)
{
	if (!IsLive(handle)) return false;
	RetireGroup(handle.slot, false);
	mGroups[handle.slot].generation = NextGeneration(mGroups[handle.slot].generation);
	Refresh();
	return true;
}

bool NRISmokeTransientClouds::SetInterest(const NRISmokeTransientHandle& handle,
	NRISmokeTransientInterest interest)
{
	if (!IsLive(handle)) return false;
	GroupSlot& group = mGroups[handle.slot];
	if (group.interest == interest) return true;
	// Existing coherent cache can still be used while first-visible preparation
	// waits for its bounded slot. No camera-derived cache identity is introduced.
	// Frozen bursts deliberately reuse their valid camera-independent cache;
	// the GPU build shader also early-outs for an already-full frozen field.
	// Never-full groups keep their existing pending first-use state untouched.
	if (interest == NRISmokeTransientInterest::Hot &&
		group.lightRefresh == NRISmokeTransientLightRefresh::Slow)
		group.needsInitialLight = true;
	group.interest = interest;
	Refresh();
	return true;
}

void NRISmokeTransientClouds::RetireGroup(uint32_t groupSlot, bool expired)
{
	GroupSlot& group = mGroups[groupSlot];
	if (!group.active) return;
	for (uint32_t index = 0u; index < group.lobeCount; ++index)
	{
		LobeSlot& lobe = mLobes[group.lobes[index]];
		if (lobe.active && expired) mSnapshot.lobesExpired++;
		lobe.active = false;
		lobe.groupSlot = UINT32_MAX;
	}
	if (expired) mSnapshot.groupsExpired++;
	group.active = false;
	group.lobeCount = 0u;
	group.fullLightAllowed = false;
}

void NRISmokeTransientClouds::Reset(uint32_t epoch)
{
	for (GroupSlot& group : mGroups)
	{
		const uint32_t generation = NextGeneration(group.generation);
		group = {};
		group.generation = generation;
	}
	for (LobeSlot& lobe : mLobes)
	{
		const uint32_t generation = NextGeneration(lobe.generation);
		lobe = {};
		lobe.generation = generation;
	}
	mGpuLobes.clear();
	mGpuGroups.clear();
	mSnapshot = {};
	mSnapshot.epoch = epoch;
	mSnapshot.allocatedGroupBytes = sizeof(mGroups);
	mSnapshot.allocatedLobeBytes = sizeof(mLobes);
	mProfile = {};
	mLightBudget = {};
	mLastDropReason = NRISmokeTransientDropReason::None;
	mGameplayTimeSeconds = 0.0;
	mFrameSerial = 0u;
	mAdmissionOrdinal = 0u;
	mLightingRevision = 1u;
	mHasProfile = false;
	mPrepared = false;
}

bool NRISmokeTransientClouds::Visible(const LobeSlot& lobe) const
{
	if (!lobe.active) return false;
	const double age = mGameplayTimeSeconds - lobe.request.authoredGameplaySeconds;
	return DensityEnvelope(lobe.request, age) > 0.0f;
}

void NRISmokeTransientClouds::RebuildLightSchedule()
{
	mSnapshot.fullLightFreshRequestedThisFrame = 0u;
	mSnapshot.fullLightFreshScheduledThisFrame = 0u;
	mSnapshot.fullLightFreshDeferredThisFrame = 0u;
	mSnapshot.fullLightRefreshRequestedThisFrame = 0u;
	mSnapshot.fullLightRefreshScheduledThisFrame = 0u;
	mSnapshot.fullLightRefreshDeferredThisFrame = 0u;
	mSnapshot.fullLightAllowedGroups = 0u;
	mSnapshot.fallbackLightGroups = 0u;
	mSnapshot.lightAnchorsScheduledThisFrame = 0u;
	mSnapshot.lightSamplesScheduledThisFrame = 0u;
	mSnapshot.lightVisibilityQueriesScheduledThisFrame = 0u;
	mSnapshot.lightInterestDeferredGroups = 0u;
	std::vector<uint32_t> fresh;
	std::vector<uint32_t> refresh;
	for (uint32_t groupIndex = 0u; groupIndex < mGroups.size(); ++groupIndex)
	{
		GroupSlot& group = mGroups[groupIndex];
		group.fullLightAllowed = false;
		group.fullLightScheduledFrame = 0u;
		if (!group.active) continue;
		if (group.interest != NRISmokeTransientInterest::Hot)
		{
			++mSnapshot.lightInterestDeferredGroups;
			continue;
		}
		bool visible = false;
		bool initialLightMature = false;
		bool allFireMembersMature = true;
		const double groupAge = mGameplayTimeSeconds - group.authoredGameplaySeconds;
		for (uint32_t index = 0u; index < group.lobeCount; ++index)
		{
			const LobeSlot& lobe = mLobes[group.lobes[index]];
			allFireMembersMature = allFireMembersMature &&
				InitialLightMature(lobe.request, groupAge);
			if (!Visible(lobe)) continue;
			visible = true;
			initialLightMature = initialLightMature ||
				InitialLightMature(lobe.request, groupAge);
		}
		if (!visible) continue;
		// Materialization can use the coherent fallback during density attack. Do
		// not spend the bounded full-build budget or freeze a near-empty self-shadow
		// result until at least one visible member reaches 90% of its attack curve.
		// A continuous fire packet's initial field must cover its complete birth
		// span, not freeze samples from only its first tiny member's bounds.
		if (group.needsInitialLight && initialLightMature &&
			(group.transientClass != NRISmokeTransientClass::FirePacket || allFireMembersMature))
			fresh.push_back(groupIndex);
		else if (!group.needsInitialLight && mProfile.allowSlowFireRefresh &&
			group.lightRefresh == NRISmokeTransientLightRefresh::Slow &&
			mProfile.fireRefreshSeconds > 0.0f &&
			mGameplayTimeSeconds - group.lastFullLightScheduleSeconds >=
				mProfile.fireRefreshSeconds)
			refresh.push_back(groupIndex);
	}
	std::sort(fresh.begin(), fresh.end(), [this](uint32_t a, uint32_t b)
	{
		const GroupSlot& ga = mGroups[a];
		const GroupSlot& gb = mGroups[b];
		const uint32_t pa = ClassPriority(ga.transientClass);
		const uint32_t pb = ClassPriority(gb.transientClass);
		// Old first-use work cannot be perpetually leapfrogged by fresh bursts.
		return ga.admittedFrame != gb.admittedFrame ? ga.admittedFrame < gb.admittedFrame :
			(pa != pb ? pa < pb : ga.admissionOrdinal < gb.admissionOrdinal);
	});
	std::sort(refresh.begin(), refresh.end(), [this](uint32_t a, uint32_t b)
	{
		const GroupSlot& ga = mGroups[a];
		const GroupSlot& gb = mGroups[b];
		return ga.lastFullLightScheduleSeconds != gb.lastFullLightScheduleSeconds
			? ga.lastFullLightScheduleSeconds < gb.lastFullLightScheduleSeconds
			: ga.admissionOrdinal < gb.admissionOrdinal;
	});
	mSnapshot.fullLightFreshRequestedThisFrame = static_cast<uint32_t>(fresh.size());
	mSnapshot.fullLightRefreshRequestedThisFrame = static_cast<uint32_t>(refresh.size());
	uint32_t budget = mProfile.maximumFullLightBuilds;
	// Keep visible maintenance progressing under a continuous burst stream.
	uint32_t refreshReserve = !refresh.empty() && budget >= 2u ?
		std::max(1u, budget / 4u) : 0u;
	if (!refresh.empty() && budget == 1u && mGameplayTimeSeconds -
		mGroups[refresh.front()].lastFullLightScheduleSeconds >=
		4.0 * mProfile.fireRefreshSeconds) refreshReserve = 1u;
	refreshReserve = std::min(refreshReserve, static_cast<uint32_t>(refresh.size()));
	for (uint32_t groupIndex : fresh)
	{
		if (budget <= refreshReserve) break;
		mGroups[groupIndex].fullLightAllowed = true;
		mGroups[groupIndex].fullLightScheduledFrame = mFrameSerial;
		--budget;
		mSnapshot.fullLightFreshScheduledThisFrame++;
	}
	for (uint32_t groupIndex : refresh)
	{
		if (budget == 0u) break;
		mGroups[groupIndex].fullLightAllowed = true;
		mGroups[groupIndex].fullLightScheduledFrame = mFrameSerial;
		--budget;
		mSnapshot.fullLightRefreshScheduledThisFrame++;
	}
	mSnapshot.fullLightFreshDeferredThisFrame =
		mSnapshot.fullLightFreshRequestedThisFrame -
		mSnapshot.fullLightFreshScheduledThisFrame;
	mSnapshot.fullLightRefreshDeferredThisFrame =
		mSnapshot.fullLightRefreshRequestedThisFrame -
		mSnapshot.fullLightRefreshScheduledThisFrame;
	mSnapshot.fullLightAllowedGroups = mSnapshot.fullLightFreshScheduledThisFrame +
		mSnapshot.fullLightRefreshScheduledThisFrame;
	for (const GroupSlot& group : mGroups)
		if (group.active && !group.fullLightAllowed) mSnapshot.fallbackLightGroups++;
	mSnapshot.lightAnchorsScheduledThisFrame = mSnapshot.fullLightAllowedGroups *
		mProfile.anchorsPerGroup;
	mSnapshot.lightSamplesScheduledThisFrame =
		mSnapshot.lightAnchorsScheduledThisFrame * mProfile.samplesPerAnchor;
	mSnapshot.lightVisibilityQueriesScheduledThisFrame =
		mSnapshot.lightSamplesScheduledThisFrame;
}

void NRISmokeTransientClouds::Refresh()
{
	for (uint32_t groupIndex = 0u; groupIndex < mGroups.size(); ++groupIndex)
	{
		GroupSlot& group = mGroups[groupIndex];
		if (!group.active) continue;
		const double groupAge = mGameplayTimeSeconds - group.authoredGameplaySeconds;
		if (mPrepared && groupAge >= group.lifetimeSeconds)
			RetireGroup(groupIndex, true);
	}
	RebuildLightSchedule();

	mGpuLobes.clear();
	// Group records preserve physical pool indices, including inactive holes, so a
	// lobe's groupSlot is always an exact buffer index rather than a compact index.
	mGpuGroups.assign(FixedGroupCapacity, {});
	mSnapshot.activeGroups = 0u;
	mSnapshot.activeLobes = 0u;
	mSnapshot.visibleGroups = 0u;
	mSnapshot.visibleLobes = 0u;
	double oldestAge = 0.0;
	for (uint32_t groupIndex = 0u; groupIndex < mGroups.size(); ++groupIndex)
	{
		GroupSlot& group = mGroups[groupIndex];
		if (!group.active) continue;
		const double groupAge = std::max(0.0,
			mGameplayTimeSeconds - group.authoredGameplaySeconds);
		oldestAge = std::max(oldestAge, groupAge);
		mSnapshot.activeGroups++;
		mSnapshot.activeLobes += group.lobeCount;
		NRISmokeTransientGroupGpu gpuGroup = {};
		gpuGroup.firstLobe = static_cast<uint32_t>(mGpuLobes.size());
		gpuGroup.slot = groupIndex;
		gpuGroup.generation = group.generation;
		gpuGroup.epoch = group.epoch;
		gpuGroup.flags = NRISmokeTransientGroupFlagActive |
			(group.fullLightAllowed ? NRISmokeTransientGroupFlagFullLightAllowed :
				NRISmokeTransientGroupFlagFallbackLight) |
			(group.lightRefresh == NRISmokeTransientLightRefresh::Slow ?
				NRISmokeTransientGroupFlagSlowRefresh : 0u);
		gpuGroup.anchorCount = mProfile.anchorsPerGroup;
		gpuGroup.samplesPerAnchor = mProfile.samplesPerAnchor;
		gpuGroup.requiredAnchorMask = (1u << mProfile.anchorsPerGroup) - 1u;
		gpuGroup.sourceId = group.sourceId;
		gpuGroup.transientClass = static_cast<uint32_t>(group.transientClass);
		gpuGroup.ageSeconds = static_cast<float>(groupAge);
		gpuGroup.groupLifetimeSeconds = group.lifetimeSeconds;
		gpuGroup.refreshIntervalSeconds = group.lightRefresh ==
			NRISmokeTransientLightRefresh::Slow ? mProfile.fireRefreshSeconds : 0.0f;
		// Low 16 bits are shape revision, high 16 bits are lighting-bounds revision.
		gpuGroup.reserved = static_cast<uint32_t>(group.shapeRevision) |
			(static_cast<uint32_t>(group.lightingRevision) << 16u);
		for (uint32_t axis = 0u; axis < 3u; ++axis)
		{
			gpuGroup.boundsMin[axis] = std::numeric_limits<float>::max();
			gpuGroup.boundsMax[axis] = -std::numeric_limits<float>::max();
		}
		for (uint32_t localIndex = 0u; localIndex < group.lobeCount; ++localIndex)
		{
			const uint32_t lobeIndex = group.lobes[localIndex];
			LobeSlot& slot = mLobes[lobeIndex];
			if (!Visible(slot)) continue;
			const double age = std::max(0.0,
				mGameplayTimeSeconds - slot.request.authoredGameplaySeconds);
			const float envelope = DensityEnvelope(slot.request, age);
			NRISmokeTransientLobeGpu gpu = {};
			const float localAge = std::max(static_cast<float>(age) -
				slot.request.lobeDelaySeconds, 0.0f);
			for (uint32_t axis = 0u; axis < 3u; ++axis)
			{
				gpu.position[axis] = slot.request.position[axis] +
					slot.request.velocity[axis] * localAge;
				gpu.halfAxisU[axis] = slot.request.halfAxisU[axis];
				gpu.halfAxisV[axis] = slot.request.halfAxisV[axis];
			}
			gpu.radius = SupportRadius(slot.request, age);
			gpu.shape = slot.request.shape;
			gpu.styleIndex = slot.request.styleIndex;
			gpu.densityScale = slot.request.initialDensity *
				slot.request.opticalWeight * envelope;
			gpu.emissionScale = slot.request.intrinsicEmission *
				std::exp2(-localAge /
					std::max(slot.request.emissionHalfLife, 0.001f));
			gpu.groupSlot = groupIndex;
			gpu.groupGeneration = group.generation;
			gpu.epoch = group.epoch;
			gpu.flags = 1u;
			gpu.deterministicSeed = slot.request.deterministicSeed;
			gpu.transientClass = static_cast<uint32_t>(slot.request.transientClass);
			gpu.corePlateau = slot.request.corePlateau;
			gpu.edgeErosion = slot.request.edgeErosion;
			gpu.noiseScale = slot.request.noiseScale;
			gpu.noiseStrength = slot.request.noiseStrength;
			const float extent = LobeSupportExtent(slot.request, gpu.radius);
			for (uint32_t axis = 0u; axis < 3u; ++axis)
			{
				gpuGroup.boundsMin[axis] = std::min(gpuGroup.boundsMin[axis],
					gpu.position[axis] - extent);
				gpuGroup.boundsMax[axis] = std::max(gpuGroup.boundsMax[axis],
					gpu.position[axis] + extent);
			}
			mGpuLobes.push_back(gpu);
		}
		gpuGroup.lobeCount = static_cast<uint32_t>(mGpuLobes.size()) -
			gpuGroup.firstLobe;
		if (gpuGroup.lobeCount > 0u)
		{
			mSnapshot.visibleGroups++;
			for (uint32_t axis = 0u; axis < 3u; ++axis)
				gpuGroup.center[axis] = (gpuGroup.boundsMin[axis] +
					gpuGroup.boundsMax[axis]) * 0.5f;
		}
		else
		{
			// Keep inactive-envelope groups finite without inventing visible support.
			const LobeSlot& first = mLobes[group.lobes[0]];
			for (uint32_t axis = 0u; axis < 3u; ++axis)
				gpuGroup.boundsMin[axis] = gpuGroup.boundsMax[axis] =
					gpuGroup.center[axis] = first.request.position[axis];
		}
		mGpuGroups[groupIndex] = gpuGroup;
	}
	mSnapshot.visibleLobes = static_cast<uint32_t>(mGpuLobes.size());
	mSnapshot.groupHighWater = std::max(mSnapshot.groupHighWater,
		mSnapshot.activeGroups);
	mSnapshot.lobeHighWater = std::max(mSnapshot.lobeHighWater,
		mSnapshot.activeLobes);
	mSnapshot.visibleLobeHighWater = std::max(mSnapshot.visibleLobeHighWater,
		mSnapshot.visibleLobes);
	mSnapshot.oldestActiveAgeMilliseconds = static_cast<uint32_t>(std::min(
		oldestAge * 1000.0, static_cast<double>(UINT32_MAX)));
}

NRISmokeTransientAdmission NRISmokeTransientClouds::Drop(
	NRISmokeTransientDropReason reason, uint32_t requestedLobes)
{
	mLastDropReason = reason;
	mSnapshot.groupsRejected++;
	switch (reason)
	{
	case NRISmokeTransientDropReason::NotPrepared:
		mSnapshot.droppedNotPrepared += requestedLobes; break;
	case NRISmokeTransientDropReason::Disabled:
		mSnapshot.droppedDisabled += requestedLobes; break;
	case NRISmokeTransientDropReason::InvalidRequest:
		mSnapshot.droppedInvalidRequest += requestedLobes; break;
	case NRISmokeTransientDropReason::StaleEpoch:
		mSnapshot.droppedStaleEpoch += requestedLobes; break;
	case NRISmokeTransientDropReason::ExpiredOnArrival:
		mSnapshot.droppedExpiredOnArrival += requestedLobes; break;
	case NRISmokeTransientDropReason::StaleOnArrival:
		mSnapshot.droppedStaleOnArrival += requestedLobes; break;
	case NRISmokeTransientDropReason::GroupCapacity:
		mSnapshot.droppedGroupCapacity += requestedLobes; break;
	case NRISmokeTransientDropReason::LobeCapacity:
		mSnapshot.droppedLobeCapacity += requestedLobes; break;
	case NRISmokeTransientDropReason::None: break;
	}
	return { {}, reason, 0u, requestedLobes };
}
