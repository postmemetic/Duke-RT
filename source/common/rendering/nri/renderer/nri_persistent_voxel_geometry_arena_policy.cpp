#include "nri_persistent_voxel_geometry_arena_policy.h"

#include <limits>

namespace
{
	bool TryAdd(uint64_t left, uint64_t right, uint64_t& result)
	{
		if (right > std::numeric_limits<uint64_t>::max() - left)
		{
			result = std::numeric_limits<uint64_t>::max();
			return false;
		}
		result = left + right;
		return true;
	}
}

NRIPersistentVoxelGeometryArenaPlan BuildNRIPersistentVoxelGeometryArenaPlan(
	uint64_t plannedGeometryBytes,
	uint64_t plannedRuntimeTailBytes,
	uint64_t largestKnownGeometryBytes)
{
	NRIPersistentVoxelGeometryArenaPlan plan = {};
	plan.plannedGeometryBytes = plannedGeometryBytes;
	plan.plannedRuntimeTailBytes = plannedRuntimeTailBytes;
	if (plannedGeometryBytes == 0 && plannedRuntimeTailBytes == 0)
	{
		return plan;
	}

	// Loading sees the raw manifest before actor-scoped aliases appear, even
	// when every optional mesh is withheld until first runtime use.
	// Reserve one measured maximum manifest geometry unit so the first late
	// alias does not cross an exact-fit multi-gigabyte arena boundary.
	plan.lateAliasReserveBytes = largestKnownGeometryBytes;
	if (!TryAdd(
			plan.plannedRuntimeTailBytes,
			plan.lateAliasReserveBytes,
			plan.totalReserveBytes) ||
		!TryAdd(
			plan.plannedGeometryBytes,
			plan.totalReserveBytes,
			plan.targetGeometryBytes))
	{
		plan.overflow = true;
	}
	return plan;
}

NRIPersistentVoxelGeometryArenaBudget BuildNRIPersistentVoxelGeometryArenaBudget(
	uint64_t currentUsageBytes, uint64_t localBudgetBytes,
	uint64_t minimumReserveBytes, uint64_t allocationBytes,
	uint64_t knownGeometryBytes, uint64_t sourceBytes, uint32_t peakPercent)
{
	NRIPersistentVoxelGeometryArenaBudget budget = {};
	if (localBudgetBytes > currentUsageBytes)
	{
		budget.availableBytes = localBudgetBytes - currentUsageBytes;
	}
	const uint64_t extraPercent = peakPercent > 100u ? uint64_t(peakPercent - 100u) : 0ull;
	// Quotient/remainder multiplication avoids overflowing bytes * percent.
	const uint64_t whole = knownGeometryBytes / 100ull;
	if (extraPercent != 0 && whole > std::numeric_limits<uint64_t>::max() / extraPercent)
	{
		budget.overflow = true;
		return budget;
	}
	const uint64_t fraction = ((knownGeometryBytes % 100ull) * extraPercent + 99ull) / 100ull;
	uint64_t geometryExtraBytes = 0;
	if (!TryAdd(whole * extraPercent, fraction, geometryExtraBytes) ||
		!TryAdd(geometryExtraBytes, sourceBytes, budget.futureBytes) ||
		!TryAdd(allocationBytes, budget.futureBytes, budget.requiredBytes) ||
		!TryAdd(budget.requiredBytes, minimumReserveBytes, budget.requiredBytes))
	{
		budget.overflow = true;
		return budget;
	}
	budget.fits = localBudgetBytes != 0 && allocationBytes != 0 && budget.requiredBytes <= budget.availableBytes;
	return budget;
}
