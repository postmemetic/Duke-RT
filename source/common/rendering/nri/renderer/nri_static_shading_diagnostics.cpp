#include "nri_static_shading_diagnostics.h"
#include "nri_trace_stats.h"
#include "printf.h"

void LogNRIStaticShadingApplicability(uint64_t frameNumber, const NRITraceShaderStatsSnapshot& snapshot)
{
	if (!snapshot.valid)
		return;
	const auto& c = snapshot.counters;
	// Producer-joined source operation coverage, not a hardware cost estimate.
	Printf("PERF pt shader data2 applicability NRI: schema=1 frame=%llu stats_frame=%llu trace_flags=0x%08x static_tangent_calls=%u primary_lod_calls=%u lod_expensive=%u lod_static_identity=%u lod_selected_positive=%u lod_static_identity_selected_positive=%u lod_geometry_fallback=%u lod_footprint_fallback=%u\n",
		(unsigned long long)frameNumber,
		(unsigned long long)snapshot.frameNumber,
		snapshot.dispatchMetadata.traceFlags,
		c[NRI_TRACE_SHADER_DATA2_STATIC_TANGENT_CALLS],
		c[NRI_TRACE_SHADER_DATA2_PRIMARY_LOD_CALLS],
		c[NRI_TRACE_SHADER_DATA2_LOD_EXPENSIVE],
		c[NRI_TRACE_SHADER_DATA2_LOD_STATIC_IDENTITY],
		c[NRI_TRACE_SHADER_DATA2_LOD_SELECTED_POSITIVE],
		c[NRI_TRACE_SHADER_DATA2_LOD_STATIC_IDENTITY_SELECTED_POSITIVE],
		c[NRI_TRACE_SHADER_DATA2_LOD_GEOMETRY_FALLBACK],
		c[NRI_TRACE_SHADER_DATA2_LOD_FOOTPRINT_FALLBACK]);
}
