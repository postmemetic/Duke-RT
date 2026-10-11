#ifndef NRI_SMOKE_FROXEL_HLSLI
#define NRI_SMOKE_FROXEL_HLSLI

// Shared by materialization and volume resolve; independent of light identity.
#define NRI_SMOKE_TRANSIENT_COVERAGE_FILTER 0x400u

bool SmokeTransientCoverageEnabled()
{
	return (gSmokeConstants.LightSourceFlags & NRI_SMOKE_TRANSIENT_COVERAGE_FILTER) != 0u;
}

// Canonical camera/froxel reconstruction for every smoke pass. The lattice is
// stable and unjittered; only the primary-sample lookup applies current jitter.

uint SmokeFroxelIndex(uint x, uint y, uint z)
{
	return (z * gSmokeConstants.FroxelHeight + y) * gSmokeConstants.FroxelWidth + x;
}

uint SmokeFroxelCount()
{
	return gSmokeConstants.FroxelWidth * gSmokeConstants.FroxelHeight * gSmokeConstants.FroxelDepth;
}

uint3 SmokeFroxelCoordinates(uint froxelIndex)
{
	const uint planeSize = gSmokeConstants.FroxelWidth * gSmokeConstants.FroxelHeight;
	const uint z = froxelIndex / max(planeSize, 1u);
	const uint planeIndex = froxelIndex - z * planeSize;
	const uint y = planeIndex / max(gSmokeConstants.FroxelWidth, 1u);
	return uint3(planeIndex - y * gSmokeConstants.FroxelWidth, y, z);
}

uint SmokeWideCellIndex(uint x, uint y, uint z)
{
	return (z * NRI_SMOKE_WIDE_GRID_Y + y) * NRI_SMOKE_WIDE_GRID_X + x;
}

float SmokeSliceFarDepth(uint z)
{
	const float normalizedDepth = (float)(z + 1u) / max((float)gSmokeConstants.FroxelDepth, 1.0);
	return gSmokeConstants.FroxelMaxDistance * pow(normalizedDepth, max(gSmokeConstants.DepthExponent, 0.001));
}

float SmokeSliceNearDepth(uint z)
{
	return z == 0u ? 0.0 : SmokeSliceFarDepth(z - 1u);
}

uint SmokeDepthSlice(float viewDepth)
{
	const float normalizedDepth = saturate(viewDepth / max(gSmokeConstants.FroxelMaxDistance, 0.001));
	const float slice = pow(normalizedDepth, 1.0 / max(gSmokeConstants.DepthExponent, 0.001));
	return min((uint)(slice * gSmokeConstants.FroxelDepth), gSmokeConstants.FroxelDepth - 1u);
}

float2 SmokeFroxelCenterUv(uint2 froxelPosition)
{
	return (float2(froxelPosition) + 0.5) /
		float2(max(gSmokeConstants.FroxelWidth, 1u), max(gSmokeConstants.FroxelHeight, 1u));
}

float2 SmokePrimarySampleUv(uint2 renderPixel)
{
	return (float2(renderPixel) + 0.5 + gSmokeConstants.CurrentJitter) /
		float2(max(gSmokeConstants.RenderWidth, 1u), max(gSmokeConstants.RenderHeight, 1u));
}

float2 SmokeFroxelCoordinate(float2 stableUv)
{
	return stableUv * float2(gSmokeConstants.FroxelWidth, gSmokeConstants.FroxelHeight) - 0.5;
}

float3 SmokeCameraRay(float2 stableUv)
{
	const float2 ndc = float2(stableUv.x * 2.0 - 1.0, 1.0 - stableUv.y * 2.0);
	return gSmokeConstants.CameraForward +
		gSmokeConstants.CameraRight * (ndc.x * gSmokeConstants.TanHalfFovX) +
		gSmokeConstants.CameraUp * (ndc.y * gSmokeConstants.TanHalfFovY);
}

float3 SmokeFroxelRay(uint2 froxelPosition)
{
	return SmokeCameraRay(SmokeFroxelCenterUv(froxelPosition));
}

float3 SmokeWorldPosition(float2 stableUv, float viewDepth)
{
	return gSmokeConstants.CameraPosition + SmokeCameraRay(stableUv) * viewDepth;
}

float SmokeWorldSegmentLength(float3 ray, float nearViewDepth, float farViewDepth)
{
	return max(farViewDepth - nearViewDepth, 0.0) * length(ray);
}

float3 SmokeFroxelCenter(uint3 froxelPosition, float3 ray)
{
	const float nearDepth = SmokeSliceNearDepth(froxelPosition.z);
	const float farDepth = SmokeSliceFarDepth(froxelPosition.z);
	return gSmokeConstants.CameraPosition + ray * ((nearDepth + farDepth) * 0.5);
}

uint2 SmokeNativeRenderPixel(uint2 froxelPosition)
{
	const float2 stableUv = SmokeFroxelCenterUv(froxelPosition);
	return min((uint2)(stableUv * float2(gSmokeConstants.RenderWidth, gSmokeConstants.RenderHeight)),
		uint2(gSmokeConstants.RenderWidth - 1u, gSmokeConstants.RenderHeight - 1u));
}

bool SmokeDecodeViewDepth(float rawViewDepth, out float viewDepth)
{
	if (isnan(rawViewDepth) || rawViewDepth < 0.0)
	{
		viewDepth = 0.0;
		return false;
	}
	viewDepth = isinf(rawViewDepth) ? gSmokeConstants.FroxelMaxDistance :
		min(rawViewDepth, gSmokeConstants.FroxelMaxDistance);
	return true;
}

bool SmokeProjectSphereToFroxelBounds(float3 sphereCenter, float radius, out int2 minimumColumn, out int2 maximumColumn)
{
	minimumColumn = int2(0, 0);
	maximumColumn = int2(gSmokeConstants.FroxelWidth - 1u, gSmokeConstants.FroxelHeight - 1u);
	const float3 relativePosition = sphereCenter - gSmokeConstants.CameraPosition;
	const float viewDepth = dot(relativePosition, gSmokeConstants.CameraForward);
	if (viewDepth <= radius)
		return true;

	const float cameraX = dot(relativePosition, gSmokeConstants.CameraRight);
	const float cameraY = dot(relativePosition, gSmokeConstants.CameraUp);
	const float nearProjectionDepth = max(viewDepth - radius, 0.001);
	const float farProjectionDepth = max(viewDepth + radius, nearProjectionDepth);
	const float inverseTanX = rcp(max(gSmokeConstants.TanHalfFovX, 0.001));
	const float inverseTanY = rcp(max(gSmokeConstants.TanHalfFovY, 0.001));
	const float2 cameraMinimum = float2(cameraX - radius, cameraY - radius);
	const float2 cameraMaximum = float2(cameraX + radius, cameraY + radius);
	const float4 projectedX = float4(cameraMinimum.x, cameraMaximum.x, cameraMinimum.x, cameraMaximum.x) /
		float4(nearProjectionDepth, nearProjectionDepth, farProjectionDepth, farProjectionDepth) * inverseTanX;
	const float4 projectedY = -float4(cameraMinimum.y, cameraMaximum.y, cameraMinimum.y, cameraMaximum.y) /
		float4(nearProjectionDepth, nearProjectionDepth, farProjectionDepth, farProjectionDepth) * inverseTanY;
	const float2 ndcMinimum = float2(
		min(min(projectedX.x, projectedX.y), min(projectedX.z, projectedX.w)),
		min(min(projectedY.x, projectedY.y), min(projectedY.z, projectedY.w)));
	const float2 ndcMaximum = float2(
		max(max(projectedX.x, projectedX.y), max(projectedX.z, projectedX.w)),
		max(max(projectedY.x, projectedY.y), max(projectedY.z, projectedY.w)));
	const int2 unclampedMinimum = int2(floor((ndcMinimum * 0.5 + 0.5) * float2(gSmokeConstants.FroxelWidth, gSmokeConstants.FroxelHeight)));
	const int2 unclampedMaximum = int2(floor((ndcMaximum * 0.5 + 0.5) * float2(gSmokeConstants.FroxelWidth, gSmokeConstants.FroxelHeight)));
	if (unclampedMaximum.x < 0 || unclampedMaximum.y < 0 ||
		unclampedMinimum.x >= (int)gSmokeConstants.FroxelWidth || unclampedMinimum.y >= (int)gSmokeConstants.FroxelHeight)
		return false;
	minimumColumn = max(unclampedMinimum, int2(0, 0));
	maximumColumn = min(unclampedMaximum, int2(gSmokeConstants.FroxelWidth - 1u, gSmokeConstants.FroxelHeight - 1u));
	return true;
}

// Analytic integral of the parabolic spherical carrier kernel over the exact
// ray/slice overlap. The 1.5 normalization preserves the prior center-line
// diameter optical mass while producing coefficients in inverse world units.
float SmokeSphereSegmentKernelAverage(float3 sphereCenter, float radius, float3 ray, float sliceNearDepth, float sliceFarDepth)
{
	const float safeRadius = max(radius, 0.001);
	const float rayLengthSquared = max(dot(ray, ray), 0.000001);
	const float rayLength = sqrt(rayLengthSquared);
	const float3 toCenter = sphereCenter - gSmokeConstants.CameraPosition;
	const float closestViewDepth = dot(toCenter, ray) / rayLengthSquared;
	const float3 closestOffset = gSmokeConstants.CameraPosition + ray * closestViewDepth - sphereCenter;
	const float perpendicularDistanceSquared = dot(closestOffset, closestOffset);
	const float radiusSquared = safeRadius * safeRadius;
	if (perpendicularDistanceSquared >= radiusSquared)
		return 0.0;

	const float halfChordWorld = sqrt(max(radiusSquared - perpendicularDistanceSquared, 0.0));
	const float halfChordViewDepth = halfChordWorld / rayLength;
	const float overlapNearDepth = max(sliceNearDepth, closestViewDepth - halfChordViewDepth);
	const float overlapFarDepth = min(sliceFarDepth, closestViewDepth + halfChordViewDepth);
	if (overlapFarDepth <= overlapNearDepth)
		return 0.0;

	const float q0 = (overlapNearDepth - closestViewDepth) * rayLength;
	const float q1 = (overlapFarDepth - closestViewDepth) * rayLength;
	const float radialBase = 1.0 - perpendicularDistanceSquared / radiusSquared;
	const float kernelIntegral = radialBase * (q1 - q0) - (q1 * q1 * q1 - q0 * q0 * q0) / (3.0 * radiusSquared);
	const float segmentWorldLength = SmokeWorldSegmentLength(ray, sliceNearDepth, sliceFarDepth);
	return segmentWorldLength > 0.0 ? max(kernelIntegral * 1.5 / segmentWorldLength, 0.0) : 0.0;
}

float3 SmokeSphereSegmentKernelCentroid(float3 sphereCenter, float radius, float3 ray, float sliceNearDepth, float sliceFarDepth)
{
	const float safeRadius = max(radius, 0.001);
	const float radiusSquared = safeRadius * safeRadius;
	const float rayLengthSquared = max(dot(ray, ray), 0.000001);
	const float rayLength = sqrt(rayLengthSquared);
	const float3 toCenter = sphereCenter - gSmokeConstants.CameraPosition;
	const float closestViewDepth = dot(toCenter, ray) / rayLengthSquared;
	const float3 closestOffset = gSmokeConstants.CameraPosition + ray * closestViewDepth - sphereCenter;
	const float perpendicularDistanceSquared = dot(closestOffset, closestOffset);
	if (perpendicularDistanceSquared >= radiusSquared)
		return gSmokeConstants.CameraPosition + ray * clamp(closestViewDepth, sliceNearDepth, sliceFarDepth);

	const float halfChordWorld = sqrt(max(radiusSquared - perpendicularDistanceSquared, 0.0));
	const float halfChordViewDepth = halfChordWorld / rayLength;
	const float overlapNearDepth = max(sliceNearDepth, closestViewDepth - halfChordViewDepth);
	const float overlapFarDepth = min(sliceFarDepth, closestViewDepth + halfChordViewDepth);
	if (overlapFarDepth <= overlapNearDepth)
		return gSmokeConstants.CameraPosition + ray * clamp(closestViewDepth, sliceNearDepth, sliceFarDepth);

	const float q0 = (overlapNearDepth - closestViewDepth) * rayLength;
	const float q1 = (overlapFarDepth - closestViewDepth) * rayLength;
	const float radialBase = 1.0 - perpendicularDistanceSquared / radiusSquared;
	const float kernelIntegral = radialBase * (q1 - q0) - (q1 * q1 * q1 - q0 * q0 * q0) / (3.0 * radiusSquared);
	if (kernelIntegral <= 1e-8)
		return gSmokeConstants.CameraPosition + ray * ((overlapNearDepth + overlapFarDepth) * 0.5);
	const float kernelFirstMoment = radialBase * (q1 * q1 - q0 * q0) * 0.5 -
		(q1 * q1 * q1 * q1 - q0 * q0 * q0 * q0) / (4.0 * radiusSquared);
	const float centroidViewDepth = closestViewDepth + (kernelFirstMoment / kernelIntegral) / rayLength;
	return gSmokeConstants.CameraPosition + ray * centroidViewDepth;
}

#endif
