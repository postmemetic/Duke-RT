#pragma once

#include <cstdint>

struct FVoxelAdjacencySpan
{
	uint32_t zOffset = 0;
	uint32_t zLength = 0;
	uint32_t normalKey = 0;
};

inline uint32_t VoxelSurfaceNormalKey(uint32_t cullMask, uint32_t voxelOffset, uint32_t slabLength, uint32_t faceCullBit)
{
	int x = 0;
	int y = 0;
	int z = 0;
	if ((cullMask & 1u) != 0u) x -= 1;
	if ((cullMask & 2u) != 0u) x += 1;
	if ((cullMask & 4u) != 0u) z += 1;
	if ((cullMask & 8u) != 0u) z -= 1;
	if (voxelOffset == 0u && (cullMask & 16u) != 0u) y += 1;
	if (slabLength != 0u && voxelOffset + 1u == slabLength && (cullMask & 32u) != 0u) y -= 1;

	int faceX = 0;
	int faceY = 0;
	int faceZ = 0;
	if (faceCullBit == 1u) faceX = -1;
	else if (faceCullBit == 2u) faceX = 1;
	else if (faceCullBit == 4u) faceZ = 1;
	else if (faceCullBit == 8u) faceZ = -1;
	else if (faceCullBit == 16u) faceY = 1;
	else if (faceCullBit == 32u) faceY = -1;

	if ((x == 0 && y == 0 && z == 0) || x * faceX + y * faceY + z * faceZ <= 0)
	{
		x = faceX;
		y = faceY;
		z = faceZ;
	}
	return (uint32_t)(x + 1) | ((uint32_t)(y + 1) << 2u) | ((uint32_t)(z + 1) << 4u);
}

inline uint32_t BuildVoxelAdjacencyNormalSpans(
	uint32_t cullMask,
	uint32_t runOffset,
	uint32_t runLength,
	uint32_t slabLength,
	uint32_t faceCullBit,
	FVoxelAdjacencySpan outSpans[3])
{
	if (runLength == 0u || runOffset > slabLength || runLength > slabLength - runOffset)
	{
		return 0u;
	}

	uint32_t spanCount = 0u;
	uint32_t previousNormalKey = 0u;
	const auto append = [&](uint32_t zOffset, uint32_t zLength)
	{
		const uint32_t normalKey = VoxelSurfaceNormalKey(cullMask, zOffset, slabLength, faceCullBit);
		if (spanCount != 0u && previousNormalKey == normalKey)
		{
			if (outSpans != nullptr)
			{
				outSpans[spanCount - 1u].zLength += zLength;
			}
			return;
		}
		if (outSpans != nullptr)
		{
			outSpans[spanCount] = { zOffset, zLength, normalKey };
		}
		previousNormalKey = normalKey;
		++spanCount;
	};

	uint32_t cursor = runOffset;
	const uint32_t end = runOffset + runLength;
	if (cursor == 0u)
	{
		append(cursor, 1u);
		++cursor;
	}
	const uint32_t bottomOffset = slabLength - 1u;
	const uint32_t interiorEnd = end < bottomOffset ? end : bottomOffset;
	if (cursor < interiorEnd)
	{
		append(cursor, interiorEnd - cursor);
		cursor = interiorEnd;
	}
	if (cursor < end)
	{
		append(cursor, end - cursor);
	}
	return spanCount;
}

inline uint32_t CountVoxelAdjacencyNormalSpans(
	uint32_t cullMask,
	uint32_t runOffset,
	uint32_t runLength,
	uint32_t slabLength,
	uint32_t faceCullBit)
{
	return BuildVoxelAdjacencyNormalSpans(cullMask, runOffset, runLength, slabLength, faceCullBit, nullptr);
}

inline uint32_t CountVoxelUnitSurfaceFaces(uint32_t cullMask, uint32_t zLength)
{
	const uint32_t sideDirections =
		((cullMask & 1u) != 0u ? 1u : 0u) +
		((cullMask & 2u) != 0u ? 1u : 0u) +
		((cullMask & 4u) != 0u ? 1u : 0u) +
		((cullMask & 8u) != 0u ? 1u : 0u);
	const uint32_t caps =
		((cullMask & 16u) != 0u ? 1u : 0u) +
		((cullMask & 32u) != 0u ? 1u : 0u);
	return sideDirections * zLength + caps;
}
