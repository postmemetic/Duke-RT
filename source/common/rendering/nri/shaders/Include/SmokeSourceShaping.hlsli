#ifndef NRI_SMOKE_SOURCE_SHAPING_HLSLI
#define NRI_SMOKE_SOURCE_SHAPING_HLSLI

uint SmokeHash(uint value)
{
	value ^= value >> 16u;
	value *= 0x7feb352du;
	value ^= value >> 15u;
	value *= 0x846ca68bu;
	return value ^ (value >> 16u);
}

float SmokeRandom01(inout uint state)
{
	state = SmokeHash(state);
	return (float)(state & 0x00ffffffu) / 16777216.0;
}

float SmokeSourceFinite(float value, float fallback)
{
	return isfinite(value) ? value : fallback;
}

float3 SmokeSourceFinite3(float3 value, float3 fallback)
{
	return all(isfinite(value)) ? value : fallback;
}

float3 SmokeSourceRandomDirection(inout uint randomState)
{
	const float z = SmokeRandom01(randomState) * 2.0 - 1.0;
	const float phi = SmokeRandom01(randomState) * 6.28318530718;
	const float radius = sqrt(max(0.0, 1.0 - z * z));
	return float3(radius * cos(phi), radius * sin(phi), z);
}

// Samples the same uniform solid-angle cone contract for particle carriers and
// authoritative-grid deposits. A command without a usable axis retains the
// caller's deterministic spherical direction and consumes no extra samples.
float3 SmokeSourceVelocityDirection(float3 commandVelocity, float velocityCone,
	float3 sphericalDirection, inout uint randomState)
{
	commandVelocity = SmokeSourceFinite3(commandVelocity, 0.0);
	sphericalDirection = SmokeSourceFinite3(sphericalDirection, float3(0.0, -1.0, 0.0));
	const float velocityLengthSquared = dot(commandVelocity, commandVelocity);
	if (velocityLengthSquared <= 1e-8)
		return sphericalDirection;

	const float3 coneAxis = commandVelocity * rsqrt(velocityLengthSquared);
	const float coneDegrees = clamp(SmokeSourceFinite(velocityCone, 0.0), 0.0, 180.0);
	const float coneCosine = cos(radians(coneDegrees));
	const float cosTheta = lerp(1.0, coneCosine, SmokeRandom01(randomState));
	const float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
	const float phi = SmokeRandom01(randomState) * 6.28318530718;
	const float3 referenceAxis = abs(coneAxis.z) < 0.999 ?
		float3(0.0, 0.0, 1.0) : float3(0.0, 1.0, 0.0);
	const float3 tangent = normalize(cross(referenceAxis, coneAxis));
	const float3 bitangent = cross(coneAxis, tangent);
	return coneAxis * cosTheta + (tangent * cos(phi) + bitangent * sin(phi)) * sinTheta;
}

uint SmokeSourceCellSeed(uint commandSerial, int3 cell)
{
	uint seed = SmokeHash(commandSerial ^ 0x9e3779b9u);
	seed ^= SmokeHash(asuint(cell.x) + 0x85ebca6bu);
	seed ^= SmokeHash(asuint(cell.y) + 0xc2b2ae35u);
	seed ^= SmokeHash(asuint(cell.z) + 0x27d4eb2fu);
	return SmokeHash(seed);
}

// A bounded analytic divergence-free field: every component varies only along
// the other two world axes, so its divergence is exactly zero. TurbulenceScale
// is a world-space wavelength. Simulation time changes only the phase, so the
// field remains camera independent, divergence free, and bounded.
float3 SmokeSourceWorldCurl(float3 worldPosition, float turbulenceScale,
	float simulationTime, float evolutionRate)
{
	worldPosition = SmokeSourceFinite3(worldPosition, 0.0);
	const float scale = max(abs(SmokeSourceFinite(turbulenceScale, 1.0)), 0.0001);
	const float3 q = worldPosition / scale;
	const float phase = SmokeSourceFinite(simulationTime, 0.0) *
		max(SmokeSourceFinite(evolutionRate, 0.0), 0.0);
	const float3 curl = float3(
		cos(q.y + 1.17 + phase * 0.73) - sin(q.z + 2.03 - phase * 1.11),
		cos(q.z + 2.71 + phase * 0.91) - sin(q.x + 0.43 + phase * 0.67),
		cos(q.x + 4.11 - phase * 0.79) - sin(q.y + 5.37 + phase * 1.03));
	return curl * 0.28867513459;
}

float3 SmokeSourceLimitVelocity(float3 velocity, float maximumVelocity)
{
	velocity = SmokeSourceFinite3(velocity, 0.0);
	maximumVelocity = max(SmokeSourceFinite(maximumVelocity, 0.0), 0.0);
	const float speedSquared = dot(velocity, velocity);
	const float maximumSquared = maximumVelocity * maximumVelocity;
	if (speedSquared > maximumSquared && speedSquared > 1e-8)
		velocity *= maximumVelocity * rsqrt(speedSquared);
	return velocity;
}

#endif
