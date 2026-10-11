#ifndef NRI_SMOKE_TRANSIENT_COVERAGE_HLSLI
#define NRI_SMOKE_TRANSIENT_COVERAGE_HLSLI

// Positive, spatial-only coverage filtering. The fixed lanes retain resolved
// shape variation; a radial area integral prevents narrow spheres falling
// between those lanes. No light cache or physical lobe geometry is changed.
#include "SmokeTransientLighting.hlsli"

float SmokeTransientCoverageOpacity(float tau)
{
	tau = max(tau, 0.0);
	return tau < 1e-4 ? tau * (1.0 - tau * (0.5 - tau / 6.0)) : 1.0 - exp(-tau);
}

float SmokeTransientCoverageTau(float opacity)
{
	opacity = saturate(opacity);
	return opacity < 1e-4 ? opacity * (1.0 + opacity * (0.5 + opacity / 3.0)) :
		-log(max(1.0 - opacity, 1e-7));
}

float SmokeTransientCoverageScatterIntegral(float sigma, float segmentLength)
{
	const float tau = max(sigma, 0.0) * segmentLength;
	return tau < 1e-4 ? segmentLength * (1.0 - tau * (0.5 - tau / 6.0)) :
		SmokeTransientCoverageOpacity(tau) / max(sigma, 1e-20);
}

float2 SmokeTransientCoverageLaneOffset(uint lane)
{
	return float2((lane & 1u) != 0u ? 0.25 : -0.25,
		(lane & 2u) != 0u ? 0.25 : -0.25);
}

float3 SmokeTransientCoverageRay(uint2 froxel, uint lane)
{
	return SmokeCameraRay(SmokeFroxelCenterUv(froxel) +
		SmokeTransientCoverageLaneOffset(lane) /
		float2(gSmokeConstants.FroxelWidth, gSmokeConstants.FroxelHeight));
}

float3 SmokeTransientCoverageHalfX(float viewDepth)
{
	return gSmokeConstants.CameraRight *
		(gSmokeConstants.TanHalfFovX * viewDepth / max((float)gSmokeConstants.FroxelWidth, 1.0));
}

float3 SmokeTransientCoverageHalfY(float viewDepth)
{
	return gSmokeConstants.CameraUp *
		(gSmokeConstants.TanHalfFovY * viewDepth / max((float)gSmokeConstants.FroxelHeight, 1.0));
}

bool SmokeTransientCoverageIntersectsAabb(float3 ray, float nearDepth, float farDepth,
	float3 lower, float3 upper)
{
	// The ray fan is affine in view depth. Its largest coordinate-wise deviation
	// is at the far face, so this conservatively includes every sub-froxel ray.
	const float3 padding = abs(SmokeTransientCoverageHalfX(farDepth)) +
		abs(SmokeTransientCoverageHalfY(farDepth));
	return SmokeTransientRaySegmentIntersectsAabb(ray, nearDepth, farDepth,
		lower - padding, upper + padding);
}

float SmokeTransientCoverageCross(float2 a, float2 b)
{
	return a.x * b.y - a.y * b.x;
}

float SmokeTransientCoverageEdgeArea(float2 a, float2 b, float radius)
{
	const float2 delta = b - a;
	const float lengthSquared = dot(delta, delta);
	if (lengthSquared < 1e-16) return 0.0;
	const float projection = -dot(a, delta) / lengthSquared;
	const float discriminant = projection * projection -
		(dot(a, a) - radius * radius) / lengthSquared;
	const float halfChord = sqrt(max(discriminant, 0.0));
	const float t1 = discriminant > 0.0 ? saturate(projection - halfChord) : 0.0;
	const float t2 = discriminant > 0.0 ? saturate(projection + halfChord) : 0.0;
	const float cuts[4] = { 0.0, t1, t2, 1.0 };
	float area = 0.0;
	[unroll]
	for (uint i = 0u; i < 3u; ++i)
	{
		const float2 p = a + delta * cuts[i];
		const float2 q = a + delta * cuts[i + 1u];
		const float2 midpoint = (p + q) * 0.5;
		const float cross = SmokeTransientCoverageCross(p, q);
		const float product = dot(p, q);
		if (dot(midpoint, midpoint) <= radius * radius)
			area += cross * 0.5;
		else if (abs(cross) + abs(product) > 1e-20)
			area += radius * radius * atan2(cross, product) * 0.5;
	}
	return area;
}

float SmokeTransientCoverageDiskArea(float2 center, float2 halfX, float2 halfY,
	float radius, float footprintArea)
{
	if (!(radius > 0.0) ||
		length(center) >= radius + length(halfX) + length(halfY)) return 0.0;
	const float2 corners[4] = { center - halfX - halfY, center + halfX - halfY,
		center + halfX + halfY, center - halfX + halfY };
	float area = 0.0;
	[unroll]
	for (uint edge = 0u; edge < 4u; ++edge)
		area += SmokeTransientCoverageEdgeArea(corners[edge], corners[(edge + 1u) & 3u], radius);
	return clamp(abs(area), 0.0, min(footprintArea, 3.14159265359 * radius * radius));
}

struct SmokeTransientCoverageFootprint
{
	float2 Center;
	float2 HalfX;
	float2 HalfY;
	float Area;
	float Span;
	float Closest;
	float RayLength;
	float3 Axis;
	float3 UnitRay;
	float NarrowWeight;
};

SmokeTransientCoverageFootprint SmokeTransientMakeCoverageFootprint(
	SmokeTransientLobe lobe, float3 centerRay)
{
	SmokeTransientCoverageFootprint footprint = (SmokeTransientCoverageFootprint)0;
	footprint.RayLength = max(length(centerRay), 1e-6);
	footprint.UnitRay = centerRay / footprint.RayLength;
	const float3 toCenter = lobe.Position - gSmokeConstants.CameraPosition;
	const float viewDepth = dot(toCenter, gSmokeConstants.CameraForward);
	footprint.Closest = dot(toCenter, footprint.UnitRay);
	const float3 projectedRight = gSmokeConstants.CameraRight - footprint.UnitRay *
		dot(gSmokeConstants.CameraRight, footprint.UnitRay);
	const float axisLength = length(projectedRight);
	// Near/behind-camera and nearly grazing projections use the bounded exact
	// lane path. The local orthographic small-sphere approximation is invalid here.
	if (viewDepth <= lobe.Radius * 2.0 || axisLength < 1e-4) return footprint;
	footprint.Axis = projectedRight / axisLength;
	const float3 axisY = cross(footprint.UnitRay, footprint.Axis);
	const float3 halfX = SmokeTransientCoverageHalfX(viewDepth);
	const float3 halfY = SmokeTransientCoverageHalfY(viewDepth);
	footprint.HalfX = float2(dot(halfX, footprint.Axis), dot(halfX, axisY));
	footprint.HalfY = float2(dot(halfY, footprint.Axis), dot(halfY, axisY));
	footprint.Center = -float2(dot(toCenter, footprint.Axis), dot(toCenter, axisY));
	footprint.Area = 4.0 * abs(SmokeTransientCoverageCross(footprint.HalfX, footprint.HalfY));
	footprint.Span = 2.0 * (length(footprint.HalfX) + length(footprint.HalfY));
	const float minimumHalfWidth = min(length(footprint.HalfX), length(footprint.HalfY));
	if (footprint.Area < 1e-8 || minimumHalfWidth < 1e-4) return footprint;
	// Full area filtering below a one-cell radius; smoothly return to four exact
	// rays by a two-cell radius. The transition blends transmittance, not tau.
	footprint.NarrowWeight = 1.0 - smoothstep(2.0, 4.0, lobe.Radius / minimumHalfWidth);
	return footprint;
}

float SmokeTransientCoverageFilteredOpacity(SmokeTransientLobe lobe,
	SmokeTransientCoverageFootprint footprint, float farDepth, float materialSigma,
	float shellFactor)
{
	const float segmentFar = farDepth * footprint.RayLength;
	if (segmentFar <= 0.0) return 0.0;
	const float nearest = clamp(footprint.Closest, 0.0, segmentFar) - footprint.Closest;
	const float supportSquared = lobe.Radius * lobe.Radius - nearest * nearest;
	if (!(supportSquared > 0.0)) return 0.0;
	const float supportRadius = sqrt(supportSquared);
	// Four Gauss-Legendre samples in squared radius. Band areas are the cumulative
	// quadrature weights: the full projected integral is a positive four-point
	// quadrature, while circle intersections make translations continuous.
	const float nodes[4] = { 0.0694318442, 0.3300094782, 0.6699905218, 0.9305681558 };
	const float edges[4] = { 0.1739274226, 0.5, 0.8260725774, 1.0 };
	float priorArea = 0.0;
	float opacity = 0.0;
	[unroll]
	for (uint band = 0u; band < 4u; ++band)
	{
		const float area = SmokeTransientCoverageDiskArea(footprint.Center,
			footprint.HalfX, footprint.HalfY, supportRadius * sqrt(edges[band]), footprint.Area);
		const float3 virtualOrigin = lobe.Position - footprint.UnitRay * footprint.Closest +
			footprint.Axis * (supportRadius * sqrt(nodes[band]));
		float coreIntegral, shellIntegral;
		SmokeTransientSpherePlateauIntegral(lobe.Position, lobe.Radius, lobe.CorePlateau,
			virtualOrigin, footprint.UnitRay, 0.0, segmentFar, coreIntegral, shellIntegral);
		opacity += max(area - priorArea, 0.0) * SmokeTransientCoverageOpacity(
			(coreIntegral + shellIntegral * shellFactor) * materialSigma);
		priorArea = max(area, priorArea);
	}
	return saturate(opacity / max(footprint.Area, 1e-8));
}

float SmokeTransientCoverageNarrowTau(SmokeTransientLobe lobe,
	SmokeTransientCoverageFootprint footprint, float nearDepth, float farDepth,
	float materialSigma)
{
	if (!(footprint.NarrowWeight > 0.0)) return 0.0;
	// This noise sample and footprint depend on the lobe, not the current slice.
	// Adjacent cumulative prefixes must agree, or a narrow cloud would acquire
	// extra optical mass whenever it crosses a depth-slice boundary.
	const float shellFactor = SmokeTransientFilteredShellFactor(lobe, lobe.Position,
		footprint.Span, lobe.Radius * 2.0);
	const float nearOpacity = SmokeTransientCoverageFilteredOpacity(lobe, footprint,
		nearDepth, materialSigma, shellFactor);
	const float farOpacity = SmokeTransientCoverageFilteredOpacity(lobe, footprint,
		farDepth, materialSigma, shellFactor);
	return max(SmokeTransientCoverageTau(farOpacity) - SmokeTransientCoverageTau(nearOpacity), 0.0);
}

float SmokeTransientCoverageBlendTau(float exactTau, float narrowTau, float narrowWeight)
{
	return SmokeTransientCoverageTau(lerp(SmokeTransientCoverageOpacity(exactTau),
		SmokeTransientCoverageOpacity(narrowTau), narrowWeight));
}

bool SmokeTransientCoverageReceiver(SmokeTransientLobe lobe, float3 unitRay,
	float segmentNear, float segmentFar, out float3 receiver)
{
	const float3 toCenter = lobe.Position - gSmokeConstants.CameraPosition;
	const float3 axisOrigin = lobe.Position - unitRay * dot(toCenter, unitRay);
	return SmokeTransientSphereSegmentReceiver(lobe.Position, lobe.Radius,
		axisOrigin, unitRay, segmentNear, segmentFar, receiver);
}

#endif
