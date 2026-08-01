#pragma once

#include <cstdint>

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
