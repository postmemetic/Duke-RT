#ifndef RAZE_NRI_VOXEL_NORMALS_HLSLI
#define RAZE_NRI_VOXEL_NORMALS_HLSLI

struct NRIVoxelAdjacencySpan
{
	uint ZOffset;
	uint ZLength;
	uint NormalKey;
};

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

int3 VoxelFaceAxis(uint faceCullBit)
{
	if (faceCullBit == 1u) return int3(-1, 0, 0);
	if (faceCullBit == 2u) return int3(1, 0, 0);
	if (faceCullBit == 4u) return int3(0, 0, 1);
	if (faceCullBit == 8u) return int3(0, 0, -1);
	if (faceCullBit == 16u) return int3(0, 1, 0);
	return int3(0, -1, 0);
}

int3 ResolveVoxelSurfaceNormalVector(int voxelZ, uint faceCullBit, NRIVoxelComputeSlabRecord slab)
{
	int3 occupancyNormal = int3(0, 0, 0);
	if ((slab.CullMask & 1u) != 0u) occupancyNormal.x -= 1;
	if ((slab.CullMask & 2u) != 0u) occupancyNormal.x += 1;
	if ((slab.CullMask & 4u) != 0u) occupancyNormal.z += 1;
	if ((slab.CullMask & 8u) != 0u) occupancyNormal.z -= 1;
	if (voxelZ == (int)slab.ZTop && (slab.CullMask & 16u) != 0u) occupancyNormal.y += 1;
	if (voxelZ + 1 == (int)(slab.ZTop + slab.ZLength) && (slab.CullMask & 32u) != 0u) occupancyNormal.y -= 1;

	const int3 faceAxis = VoxelFaceAxis(faceCullBit);
	if (all(occupancyNormal == int3(0, 0, 0)) || dot(occupancyNormal, faceAxis) <= 0)
	{
		return faceAxis;
	}
	return occupancyNormal;
}

uint EncodeVoxelSurfaceNormalKey(int3 normal)
{
	return uint(normal.x + 1) | (uint(normal.y + 1) << 2u) | (uint(normal.z + 1) << 4u);
}

uint BuildVoxelSurfaceNormalKey(int voxelZ, uint faceCullBit, NRIVoxelComputeSlabRecord slab)
{
	return EncodeVoxelSurfaceNormalKey(ResolveVoxelSurfaceNormalVector(voxelZ, faceCullBit, slab));
}

uint BuildVoxelSurfaceNormal(int voxelZ, uint faceCullBit, NRIVoxelComputeSlabRecord slab)
{
	return PackVoxelNormal((float3)ResolveVoxelSurfaceNormalVector(voxelZ, faceCullBit, slab));
}

void AppendVoxelAdjacencySpan(
	NRIVoxelComputeSlabRecord slab,
	uint faceCullBit,
	uint zOffset,
	uint zLength,
	inout NRIVoxelAdjacencySpan spans[3],
	inout uint spanCount)
{
	const uint normalKey = BuildVoxelSurfaceNormalKey(int(slab.ZTop + zOffset), faceCullBit, slab);
	if (spanCount != 0u && spans[spanCount - 1u].NormalKey == normalKey)
	{
		spans[spanCount - 1u].ZLength += zLength;
		return;
	}
	spans[spanCount].ZOffset = zOffset;
	spans[spanCount].ZLength = zLength;
	spans[spanCount].NormalKey = normalKey;
	++spanCount;
}

uint BuildVoxelAdjacencyNormalSpans(
	NRIVoxelComputeSlabRecord slab,
	NRIVoxelComputeColorRunRecord run,
	uint faceCullBit,
	out NRIVoxelAdjacencySpan spans[3])
{
	[unroll]
	for (uint i = 0u; i < 3u; ++i)
	{
		spans[i].ZOffset = 0u;
		spans[i].ZLength = 0u;
		spans[i].NormalKey = 0u;
	}
	if (run.ZLength == 0u || run.ZOffset > slab.ZLength || run.ZLength > slab.ZLength - run.ZOffset)
	{
		return 0u;
	}

	uint spanCount = 0u;
	uint cursor = run.ZOffset;
	const uint end = run.ZOffset + run.ZLength;
	if (cursor == 0u)
	{
		AppendVoxelAdjacencySpan(slab, faceCullBit, cursor, 1u, spans, spanCount);
		++cursor;
	}
	const uint bottomOffset = slab.ZLength - 1u;
	const uint interiorEnd = min(end, bottomOffset);
	if (cursor < interiorEnd)
	{
		AppendVoxelAdjacencySpan(slab, faceCullBit, cursor, interiorEnd - cursor, spans, spanCount);
		cursor = interiorEnd;
	}
	if (cursor < end)
	{
		AppendVoxelAdjacencySpan(slab, faceCullBit, cursor, end - cursor, spans, spanCount);
	}
	return spanCount;
}

uint CountVoxelAdjacencyNormalSpans(
	NRIVoxelComputeSlabRecord slab,
	NRIVoxelComputeColorRunRecord run,
	uint faceCullBit)
{
	NRIVoxelAdjacencySpan spans[3];
	return BuildVoxelAdjacencyNormalSpans(slab, run, faceCullBit, spans);
}

#endif
