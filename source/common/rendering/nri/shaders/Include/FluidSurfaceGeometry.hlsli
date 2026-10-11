#ifndef NRI_FLUID_SURFACE_GEOMETRY_HLSLI
#define NRI_FLUID_SURFACE_GEOMETRY_HLSLI

// Reuse the packed corner-normal row without sharing voxel blend policy.
// The CPU only tags normals joined through compatible fluid floor edges.
float3 ResolveFluidCarrierShadingNormal(HitData hit, float3 geometricNormal)
{
	const PrimitiveData primitive = GetPrimitiveData(hit.dataSource, hit.primitiveIndex);
	if ((primitive.smoothNormals.y & 0xc0000000u) != 0xc0000000u)
		return geometricNormal;
	const float3 weights = ResolveHitBarycentricWeights(hit);
	float3 normal =
		UnpackVoxelNormal(primitive.smoothNormals.x & 0xffffu) * weights.x +
		UnpackVoxelNormal(primitive.smoothNormals.x >> 16u) * weights.y +
		UnpackVoxelNormal(primitive.smoothNormals.y & 0xffffu) * weights.z;
	if (hit.instanceId != 0xffffffffu)
		normal = TransformSceneInstanceNormal(GetSceneInstanceData(hit.instanceId), normal, false);
	normal = FluidSafeNormalize(normal, geometricNormal);
	return dot(normal, geometricNormal) < 0.0 ? -normal : normal;
}

#endif
