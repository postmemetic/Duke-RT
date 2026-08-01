#ifndef RAZE_NRI_VOXEL_NORMALS_HLSLI
#define RAZE_NRI_VOXEL_NORMALS_HLSLI

uint PackVoxelNormal(float3 normal)
{
	const float lengthSq = dot(normal, normal);
	if (lengthSq <= 1.0e-12f)
	{
		return 0u;
	}
	normal *= rsqrt(lengthSq);
	float2 oct = normal.xy / (abs(normal.x) + abs(normal.y) + abs(normal.z));
	if (normal.z < 0.0f)
	{
		oct = (1.0f - abs(oct.yx)) * float2(oct.x < 0.0f ? -1.0f : 1.0f, oct.y < 0.0f ? -1.0f : 1.0f);
	}
	const uint2 packed = (uint2)round(saturate(oct * 0.5f + 0.5f) * 255.0f);
	return packed.x | (packed.y << 8u);
}

uint BuildVoxelSurfaceNormal(
	int voxelZ,
	float3 faceNormal,
	NRIVoxelComputeSlabRecord slab)
{
	float3 occupancyNormal = 0.0f;
	if ((slab.CullMask & 1u) != 0u)
	{
		occupancyNormal += float3(-1.0f, 0.0f, 0.0f);
	}
	if ((slab.CullMask & 2u) != 0u)
	{
		occupancyNormal += float3(1.0f, 0.0f, 0.0f);
	}
	if ((slab.CullMask & 4u) != 0u)
	{
		occupancyNormal += float3(0.0f, 0.0f, 1.0f);
	}
	if ((slab.CullMask & 8u) != 0u)
	{
		occupancyNormal += float3(0.0f, 0.0f, -1.0f);
	}
	if (voxelZ == (int)slab.ZTop && (slab.CullMask & 16u) != 0u)
	{
		occupancyNormal += float3(0.0f, 1.0f, 0.0f);
	}
	if (voxelZ + 1 == (int)(slab.ZTop + slab.ZLength) && (slab.CullMask & 32u) != 0u)
	{
		occupancyNormal += float3(0.0f, -1.0f, 0.0f);
	}

	const float lengthSq = dot(occupancyNormal, occupancyNormal);
	if (lengthSq <= 1.0e-12f)
	{
		return PackVoxelNormal(faceNormal);
	}
	const float3 smoothNormal = occupancyNormal * rsqrt(lengthSq);
	return PackVoxelNormal(dot(smoothNormal, faceNormal) > 1.0e-4f ? smoothNormal : faceNormal);
}

#endif
