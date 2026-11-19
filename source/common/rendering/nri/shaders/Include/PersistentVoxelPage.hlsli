#ifndef NRI_PERSISTENT_VOXEL_PAGE_HLSLI
#define NRI_PERSISTENT_VOXEL_PAGE_HLSLI

#include "SceneShadowContracts.hlsli"
#include "EmissiveLightContracts.hlsli"

// The BLAS uses physical, page-local vertex/index offsets. Published
// primitive addresses carry this bit. Scalar vertex readers also use
// the tag on returned indices. Grouped triangle readers keep indices page-local.
// Material rows and instance identity are shared with the arena.
static const uint VOXEL_PAGE_BIT = 0x80000000u;

bool VoxelPrimitiveRangeFits(StructuredBuffer<PrimitiveData> arena, StructuredBuffer<PrimitiveData> page,
	uint base, uint length)
{
	uint count, stride;
	if ((base & VOXEL_PAGE_BIT) != 0u)
		page.GetDimensions(count, stride);
	else
		arena.GetDimensions(count, stride);
	const uint first = base & ~VOXEL_PAGE_BIT;
	return first < count && length <= count - first;
}

PrimitiveData ReadVoxelPagePrimitive(StructuredBuffer<PrimitiveData> page, uint address)
{
	uint count, stride;
	page.GetDimensions(count, stride);
	const uint row = address & ~VOXEL_PAGE_BIT;
	if (row >= count)
		return (PrimitiveData)0;
	PrimitiveData primitive = page[row];
#if !defined(NRI_TRACE_PAGED_TRIANGLES)
	primitive.indices |= VOXEL_PAGE_BIT;
#endif
	return primitive;
}

SceneVertex ReadVoxelPageVertex(StructuredBuffer<SceneVertex> page, uint address)
{
	uint count, stride;
	page.GetDimensions(count, stride);
	const uint row = address & ~VOXEL_PAGE_BIT;
	if (row >= count)
		return (SceneVertex)0;
	return page[row];
}

#endif
