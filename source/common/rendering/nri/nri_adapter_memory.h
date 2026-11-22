#pragma once

#include <cstdint>

// Dedicated capacity selects a startup tier. The live budget separately
// determines which voxel allocations the adapter can afford during play.
constexpr bool NRIIsSmallDiscreteGpu(bool discrete, uint64_t dedicatedBytes)
{
	return discrete && dedicatedBytes > 0 && dedicatedBytes <= (4ull << 30);
}
