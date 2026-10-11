#ifndef RAZE_NRI_PRIMARY_TEMPORAL_GEOMETRY_HLSLI
#define RAZE_NRI_PRIMARY_TEMPORAL_GEOMETRY_HLSLI

// Call only for the accepted primary hit, then consume the results before lighting.
// Keep geometry records inside this helper instead of extending HitData's live state.
void ResolvePrimaryTemporalGeometry(
	HitData hit,
	out float3 currentPosition,
	out float3 previousPosition,
	out float3 currentGeometricNormal,
	out float3 previousGeometricNormal,
	out uint4 temporalIdentity)
{
	const PrimitiveData primitive = GetPrimitiveData(hit.dataSource, hit.primitiveIndex);
	const SceneVertex v0 = GetVertexData(hit.dataSource, primitive.indices.x);
	const SceneVertex v1 = GetVertexData(hit.dataSource, primitive.indices.y);
	const SceneVertex v2 = GetVertexData(hit.dataSource, primitive.indices.z);
	const float3 weights = ResolveHitBarycentricWeights(hit);
	float3 currentP0 = v0.position;
	float3 currentP1 = v1.position;
	float3 currentP2 = v2.position;
	float3 previousP0 = v0.prevPosition;
	float3 previousP1 = v1.prevPosition;
	float3 previousP2 = v2.prevPosition;

	// Preserve the position resolver's barycentric-before-transform arithmetic.
	currentPosition = currentP0 * weights.x + currentP1 * weights.y + currentP2 * weights.z;
	previousPosition = previousP0 * weights.x + previousP1 * weights.y + previousP2 * weights.z;
	temporalIdentity = uint4(primitive.temporalSurfaceId, primitive.temporalGeneration, primitive.temporalFlags);
	if (hit.instanceId != 0xffffffffu)
	{
		const SceneInstanceData instanceData = GetSceneInstanceData(hit.instanceId);
		currentPosition = TransformSceneInstancePoint(instanceData, currentPosition, false);
		previousPosition = TransformSceneInstancePoint(instanceData, previousPosition, true);
		// Normals instead require transforming each vertex before the cross product.
		currentP0 = TransformSceneInstancePoint(instanceData, currentP0, false);
		currentP1 = TransformSceneInstancePoint(instanceData, currentP1, false);
		currentP2 = TransformSceneInstancePoint(instanceData, currentP2, false);
		previousP0 = TransformSceneInstancePoint(instanceData, previousP0, true);
		previousP1 = TransformSceneInstancePoint(instanceData, previousP1, true);
		previousP2 = TransformSceneInstancePoint(instanceData, previousP2, true);
	}
	const float3 currentUnnormalized = cross(currentP1 - currentP0, currentP2 - currentP0);
	const float currentLengthSquared = dot(currentUnnormalized, currentUnnormalized);
	currentGeometricNormal = currentLengthSquared > 1e-12 ? currentUnnormalized * rsqrt(currentLengthSquared) : float3(0.0, 0.0, 1.0);
	const float3 previousUnnormalized = cross(previousP1 - previousP0, previousP2 - previousP0);
	const float previousLengthSquared = dot(previousUnnormalized, previousUnnormalized);
	previousGeometricNormal = previousLengthSquared > 1e-12 ? previousUnnormalized * rsqrt(previousLengthSquared) : float3(0.0, 0.0, 1.0);
}

#endif
