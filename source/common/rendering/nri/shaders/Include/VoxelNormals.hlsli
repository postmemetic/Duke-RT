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

int3 ResolveVoxelSurfaceNormalVector(uint exposureMask, uint faceCullBit)
{
	int3 occupancyNormal = int3(0, 0, 0);
	if ((exposureMask & 1u) != 0u) occupancyNormal.x -= 1;
	if ((exposureMask & 2u) != 0u) occupancyNormal.x += 1;
	if ((exposureMask & 4u) != 0u) occupancyNormal.z += 1;
	if ((exposureMask & 8u) != 0u) occupancyNormal.z -= 1;
	if ((exposureMask & 16u) != 0u) occupancyNormal.y += 1;
	if ((exposureMask & 32u) != 0u) occupancyNormal.y -= 1;

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

uint BuildVoxelSurfaceNormalKey(uint exposureMask, uint faceCullBit)
{
	return EncodeVoxelSurfaceNormalKey(ResolveVoxelSurfaceNormalVector(exposureMask, faceCullBit));
}

uint BuildVoxelSurfaceNormal(uint exposureMask, uint faceCullBit)
{
	return PackVoxelNormal((float3)ResolveVoxelSurfaceNormalVector(exposureMask, faceCullBit));
}

uint BuildVoxelSurfaceExposureMask(
	uint lateralExposureMask,
	uint capExposureMask,
	uint voxelOffset,
	uint slabLength)
{
	uint exposureMask = lateralExposureMask & 15u;
	if (voxelOffset == 0u)
	{
		exposureMask |= capExposureMask & 16u;
	}
	if (slabLength != 0u && voxelOffset + 1u == slabLength)
	{
		exposureMask |= capExposureMask & 32u;
	}
	return exposureMask;
}

void AppendVoxelAdjacencySpan(
	NRIVoxelComputeSlabRecord slab,
	NRIVoxelComputeColorRunRecord run,
	uint faceCullBit,
	uint zOffset,
	uint zLength,
	inout NRIVoxelAdjacencySpan spans[3],
	inout uint spanCount)
{
	const uint exposureMask = BuildVoxelSurfaceExposureMask(
		run.LateralExposureMask, slab.CapExposureMask, zOffset, slab.ZLength);
	const uint normalKey = BuildVoxelSurfaceNormalKey(exposureMask, faceCullBit);
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
		AppendVoxelAdjacencySpan(slab, run, faceCullBit, cursor, 1u, spans, spanCount);
		++cursor;
	}
	const uint bottomOffset = slab.ZLength - 1u;
	const uint interiorEnd = min(end, bottomOffset);
	if (cursor < interiorEnd)
	{
		AppendVoxelAdjacencySpan(slab, run, faceCullBit, cursor, interiorEnd - cursor, spans, spanCount);
		cursor = interiorEnd;
	}
	if (cursor < end)
	{
		AppendVoxelAdjacencySpan(slab, run, faceCullBit, cursor, end - cursor, spans, spanCount);
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
