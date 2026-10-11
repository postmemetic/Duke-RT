#include "nri_primary_temporal_geometry_diagnostics.h"

#include "nri_trace_stats.h"
#include "printf.h"

void LogNRIPrimaryTemporalGeometryOracle(uint64_t frameNumber, const NRITraceShaderStatsSnapshot& snapshot)
{
	if (!snapshot.valid)
	{
		return;
	}
	const auto& c = snapshot.counters;
	Printf("PERF pt shader primary temporal geometry oracle NRI: schema=1 frame=%llu stats_frame=%llu comparisons=%u mismatch_pixels=%u current_position=%u previous_position=%u current_normal=%u previous_normal=%u identity=%u\n",
		(unsigned long long)frameNumber,
		(unsigned long long)snapshot.frameNumber,
		c[NRI_TRACE_SHADER_PRIMARY_GEOMETRY_ORACLE_COMPARISONS],
		c[NRI_TRACE_SHADER_PRIMARY_GEOMETRY_ORACLE_MISMATCH_PIXELS],
		c[NRI_TRACE_SHADER_PRIMARY_GEOMETRY_ORACLE_CURRENT_POSITION],
		c[NRI_TRACE_SHADER_PRIMARY_GEOMETRY_ORACLE_PREVIOUS_POSITION],
		c[NRI_TRACE_SHADER_PRIMARY_GEOMETRY_ORACLE_CURRENT_NORMAL],
		c[NRI_TRACE_SHADER_PRIMARY_GEOMETRY_ORACLE_PREVIOUS_NORMAL],
		c[NRI_TRACE_SHADER_PRIMARY_GEOMETRY_ORACLE_IDENTITY]);
}
