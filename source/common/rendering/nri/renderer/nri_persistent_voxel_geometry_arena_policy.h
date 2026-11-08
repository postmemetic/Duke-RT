#pragma once

#include <cstdint>

struct NRIPersistentVoxelGeometryArenaPlan
{
	uint64_t plannedGeometryBytes = 0;
	uint64_t plannedRuntimeTailBytes = 0;
	uint64_t lateAliasReserveBytes = 0;
	uint64_t totalReserveBytes = 0;
	uint64_t targetGeometryBytes = 0;
	bool overflow = false;
};

NRIPersistentVoxelGeometryArenaPlan BuildNRIPersistentVoxelGeometryArenaPlan(
	uint64_t plannedGeometryBytes,
	uint64_t plannedRuntimeTailBytes,
	uint64_t largestKnownGeometryBytes);

struct NRIPersistentVoxelGeometryArenaBudget
{
	uint64_t availableBytes = 0;
	uint64_t futureBytes = 0;
	uint64_t requiredBytes = 0;
	bool overflow = false;
	bool fits = false;
};

// allocationBytes is the sum of NRI's actual memory requirements, not logical
// buffer sizes. Keep minimumReserveBytes free after future AS/staging estimates.
NRIPersistentVoxelGeometryArenaBudget BuildNRIPersistentVoxelGeometryArenaBudget(
	uint64_t currentUsageBytes, uint64_t localBudgetBytes,
	uint64_t minimumReserveBytes, uint64_t allocationBytes,
	uint64_t knownGeometryBytes, uint64_t sourceBytes, uint32_t peakPercent);

struct NRIPersistentVoxelGeometryArenaSelection
{
	uint64_t primitives = 0;
	uint32_t candidates = 0;
};

// fits must be monotonic and inspect requirements only, never allocate resources.
// The caller bounds maximum by full manifest demand and the index/device limits.
template<class Fits>
NRIPersistentVoxelGeometryArenaSelection SelectNRIPersistentVoxelGeometryArenaCapacity(
	uint64_t maximum, uint64_t minimum, Fits&& fits)
{
	NRIPersistentVoxelGeometryArenaSelection selection = {};
	if (maximum == 0 || minimum == 0 || minimum > maximum) return selection;
	++selection.candidates;
	if (fits(maximum)) { selection.primitives = maximum; return selection; }
	uint64_t lower = 0, upper = maximum - 1;
	while (lower < upper)
	{
		const uint64_t distance = upper - lower;
		const uint64_t candidate = lower + distance / 2 + distance % 2;
		++selection.candidates;
		if (fits(candidate)) lower = candidate;
		else upper = candidate - 1;
	}
	if (lower >= minimum) selection.primitives = lower;
	return selection;
}
