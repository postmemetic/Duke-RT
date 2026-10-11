#ifndef NRI_SMOKE_TRANSIENT_HISTORY_HLSLI
#define NRI_SMOKE_TRANSIENT_HISTORY_HLSLI

#include "SmokeFroxel.hlsli"

#define NRI_SMOKE_TRANSIENT_HISTORY_ENABLED 0x800u

bool SmokeTransientHistoryEnabled()
{
	return (gSmokeConstants.LightSourceFlags & NRI_SMOKE_TRANSIENT_HISTORY_ENABLED) != 0u;
}

float SmokeTransientHistoryOpacity(float tau)
{
	tau = max(tau, 0.0);
	return tau < 1e-4 ? tau * (1.0 - tau * (0.5 - tau / 6.0)) : 1.0 - exp(-tau);
}

float SmokeTransientHistoryTau(float opacity)
{
	opacity = saturate(opacity);
	return opacity < 1e-4 ? opacity * (1.0 + opacity * (0.5 + opacity / 3.0)) :
		min(-log(max(1.0 - opacity, 1e-7)), 16.0);
}

float SmokeTransientPackMotionGuide(float depth, float confidence)
{
	const uint depthBits = (uint)round(saturate(depth / max(gSmokeConstants.FroxelMaxDistance, 0.001)) * 65535.0);
	const uint confidenceBits = (uint)round(saturate(confidence) * 255.0);
	// Numeric 24-bit integer, exactly representable as float32. No NaN-prone
	// bitcast and no extra full-resolution guide texture are needed.
	return (float)((depthBits << 8u) | confidenceBits);
}

float2 SmokeTransientUnpackMotionGuide(float packed)
{
	const uint bits = (uint)round(clamp(isfinite(packed) ? packed : 0.0, 0.0, 16777215.0));
	return float2((float)(bits >> 8u) * (gSmokeConstants.FroxelMaxDistance / 65535.0),
		(float)(bits & 255u) / 255.0);
}

struct SmokeTransientHistoryMotion
{
	float3 Displacement;
	float Depth;
	float Confidence;
};

SmokeTransientHistoryMotion SmokeTransientLobeHistoryMotion(SmokeTransientLobe lobe,
	SmokeTransientGroup group, uint lobeIndex, float3 unitRay, float nearWorld, float farWorld)
{
	SmokeTransientHistoryMotion motion = (SmokeTransientHistoryMotion)0;
	if (!SmokeTransientHistoryEnabled() || lobe.Shape == NRI_SMOKE_INJECTION_SHAPE_RECTANGLE)
		return motion;
	uint count, stride;
	gSmokeTransientPreviousLobes.GetDimensions(count, stride);
	if (lobeIndex >= count) return motion;
	const float4 previous = gSmokeTransientPreviousLobes[lobeIndex];
	if (!all(isfinite(previous)) || previous.w <= 0.0 || lobe.Radius <= 0.0) return motion;
	const float growthRatio = previous.w / lobe.Radius;
	if (growthRatio < 0.75 || growthRatio > 1.25) return motion;
	const float3 translation = previous.xyz - lobe.Position;
	if (length(translation) > max(lobe.Radius * 0.5, 16.0)) return motion;
	const float3 toCenter = lobe.Position - gSmokeConstants.CameraPosition;
	const float closest = dot(toCenter, unitRay);
	const float3 perpendicular = toCenter - unitRay * closest;
	const float chordSquared = lobe.Radius * lobe.Radius - dot(perpendicular, perpendicular);
	float3 origin = gSmokeConstants.CameraPosition;
	float halfChord;
	if (chordSquared > 0.0)
		halfChord = sqrt(chordSquared);
	else
	{
		// Area-prefiltered support may miss the center ray. Use the occupied
		// sphere axis, never the potentially empty coarse froxel midpoint.
		origin = lobe.Position - unitRay * closest;
		halfChord = lobe.Radius;
	}
	const float entry = max(nearWorld, closest - halfChord);
	const float exit = min(farWorld, closest + halfChord);
	if (exit <= entry) return motion;
	const float3 receiver = origin + unitRay * ((entry + exit) * 0.5);
	const float3 previousReceiver = previous.xyz + (receiver - lobe.Position) * growthRatio;
	motion.Displacement = previousReceiver - receiver;
	motion.Depth = dot(receiver - gSmokeConstants.CameraPosition, gSmokeConstants.CameraForward);
	const bool fire = group.TransientClass == NRI_SMOKE_TRANSIENT_CLASS_FIRE;
	const float birthConfidence = smoothstep(fire ? 0.05 : 0.10, fire ? 0.20 : 0.35, group.AgeSeconds);
	const float growthConfidence = 1.0 - smoothstep(0.02, 0.20, abs(growthRatio - 1.0));
	motion.Confidence = birthConfidence * growthConfidence;
	if (!all(isfinite(motion.Displacement)) || !isfinite(motion.Depth))
		motion = (SmokeTransientHistoryMotion)0;
	return motion;
}

SmokeTransientHistoryMotion SmokeTransientResolveHistoryMotion(float2 stableUv, uint slice)
{
	SmokeTransientHistoryMotion result = (SmokeTransientHistoryMotion)0;
	if (!SmokeTransientHistoryEnabled()) return result;
	uint motionCount, motionStride, mediumCount, mediumStride;
	gSmokeTransientFroxelMotion.GetDimensions(motionCount, motionStride);
	gSmokeFroxelMedium.GetDimensions(mediumCount, mediumStride);
	const float2 coordinate = SmokeFroxelCoordinate(stableUv);
	const int2 base = int2(floor(coordinate));
	const int2 maximum = int2(gSmokeConstants.FroxelWidth - 1u, gSmokeConstants.FroxelHeight - 1u);
	const float2 blend = frac(coordinate);
	float totalWeight = 0.0;
	float validWeight = 0.0;
	float displacementMoment = 0.0;
	[unroll]
	for (uint tap = 0u; tap < 4u; ++tap)
	{
		const uint2 column = (uint2)clamp(base + int2(tap & 1u, tap >> 1u), int2(0, 0), maximum);
		const uint index = SmokeFroxelIndex(column.x, column.y, slice);
		if (index >= min(motionCount, mediumCount)) continue;
		const float weight = ((tap & 1u) != 0u ? blend.x : 1.0 - blend.x) *
			((tap & 2u) != 0u ? blend.y : 1.0 - blend.y) * max(gSmokeFroxelMedium[index].w, 0.0);
		totalWeight += weight;
		const float4 motion = gSmokeTransientFroxelMotion[index];
		if (!all(isfinite(motion))) continue;
		const float2 guide = SmokeTransientUnpackMotionGuide(motion.w);
		const float valid = weight * guide.y;
		validWeight += valid;
		result.Displacement += motion.xyz * valid;
		result.Depth += guide.x * valid;
		displacementMoment += dot(motion.xyz, motion.xyz) * valid;
	}
	if (validWeight <= 1e-12 || totalWeight <= 1e-12) return (SmokeTransientHistoryMotion)0;
	result.Displacement /= validWeight;
	result.Depth /= validWeight;
	const float variance = max(displacementMoment / validWeight - dot(result.Displacement, result.Displacement), 0.0);
	const float width = max(result.Depth * min(gSmokeConstants.TanHalfFovX / max((float)gSmokeConstants.FroxelWidth, 1.0),
		gSmokeConstants.TanHalfFovY / max((float)gSmokeConstants.FroxelHeight, 1.0)), 0.25);
	result.Confidence = saturate(validWeight / totalWeight) * exp(-variance / (width * width));
	const float nearDepth = SmokeSliceNearDepth(slice);
	const float farDepth = SmokeSliceFarDepth(slice);
	const float inset = max((farDepth - nearDepth) * 1e-4, 0.001);
	// Packing can round an occupied point onto a slice edge. Keep the guide in
	// its authoring slice so Resolve and Temporal fetch the same motion record.
	result.Depth = clamp(result.Depth, nearDepth + inset, farDepth - inset);
	return result;
}

#endif
