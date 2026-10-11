#include "nri_dynamic_overlay_blas_diagnostics.h"

#include "printf.h"

void LogNRIDynamicOverlayBlasPolicyStats(uint64_t frameNumber, const NRIDynamicOverlayBlasPolicyStats& stats)
{
	Printf("PERF pt dynamic overlay blas policy NRI: frame=%llu policy_requested=%d policy_effective=%u build_flags=0x%x build_requested=%u route_requested=%u filter_partition=%u build_effective=%u route_effective=%u cpu_total_ms=%.3f cpu_cold_ms=%.3f cache_limit=%u cached_assets=%u touched_assets=%u cached_as_bytes=%llu cached_geometry_bytes=%llu touched_as_bytes=%llu built_as_bytes=%llu build_scratch_max_bytes=%llu shared_scratch_bytes=%llu cache_hit_age_sum=%llu cache_hit_age_max=%u\n",
		(unsigned long long)frameNumber, stats.requestedPolicy, stats.effectivePolicy, stats.buildFlags,
		stats.requestedBuild ? 1u : 0u, stats.requestedRoute ? 1u : 0u, stats.filterPartition ? 1u : 0u,
		stats.effectiveBuild ? 1u : 0u, stats.effectiveRoute ? 1u : 0u, stats.totalCpuMs, stats.coldCpuMs,
		stats.cacheLimit, stats.cachedAssets, stats.touchedAssets,
		(unsigned long long)stats.cachedAsBytes, (unsigned long long)stats.cachedGeometryBytes,
		(unsigned long long)stats.touchedAsBytes, (unsigned long long)stats.builtAsBytes,
		(unsigned long long)stats.buildScratchMaxBytes, (unsigned long long)stats.sharedScratchBytes,
		(unsigned long long)stats.cacheHitAgeSumFrames, stats.cacheHitAgeMaxFrames);
}
