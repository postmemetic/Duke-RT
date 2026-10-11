#include "nri_spatial_absence_diagnostics.h"

#include "nri_trace_stats.h"
#include "printf.h"

void LogNRISpatialAbsenceProfile(uint64_t frameNumber, const NRITraceShaderStatsSnapshot& snapshot)
{
	if (!snapshot.valid)
		return;
	const auto& c = snapshot.counters;
	// stats_frame identifies the dispatch producer; frame is only print cadence.
	// Visits describe instrumented source operations, not hardware transactions.
	Printf("PERF pt shader absence footprint NRI: schema=1 frame=%llu stats_frame=%llu trace_flags=0x%08x footprint_calls=%u footprint_eligible=%u footprint_probe_calls=%u certificate_tests=%u certificate_hits=%u reference_visits=%u reference_after_hit=%u triangle_visits=%u triangle_after_hit=%u triangle_first_hits=%u avoidable_references=%u avoidable_triangles=%u\n",
		(unsigned long long)frameNumber,
		(unsigned long long)snapshot.frameNumber,
		snapshot.dispatchMetadata.traceFlags,
		c[NRI_TRACE_SHADER_ABSENCE_FOOTPRINT_CALLS],
		c[NRI_TRACE_SHADER_ABSENCE_FOOTPRINT_ELIGIBLE],
		c[NRI_TRACE_SHADER_ABSENCE_FOOTPRINT_PROBE_CALLS],
		c[NRI_TRACE_SHADER_ABSENCE_CERTIFICATE_TESTS],
		c[NRI_TRACE_SHADER_ABSENCE_CERTIFICATE_HITS],
		c[NRI_TRACE_SHADER_ABSENCE_REFERENCE_VISITS],
		c[NRI_TRACE_SHADER_ABSENCE_REFERENCE_AFTER_HIT],
		c[NRI_TRACE_SHADER_ABSENCE_TRIANGLE_VISITS],
		c[NRI_TRACE_SHADER_ABSENCE_TRIANGLE_AFTER_HIT],
		c[NRI_TRACE_SHADER_ABSENCE_TRIANGLE_FIRST_HITS],
		c[NRI_TRACE_SHADER_ABSENCE_AVOIDABLE_REFERENCES],
		c[NRI_TRACE_SHADER_ABSENCE_AVOIDABLE_TRIANGLES]);
	Printf("PERF pt shader absence traversal NRI: schema=1 frame=%llu stats_frame=%llu trace_flags=0x%08x candidate_evaluations=%u candidate_rejects=%u fallback_evaluations=%u static_restarts=%u actor_restarts=%u\n",
		(unsigned long long)frameNumber,
		(unsigned long long)snapshot.frameNumber,
		snapshot.dispatchMetadata.traceFlags,
		c[NRI_TRACE_SHADER_ABSENCE_CANDIDATE_EVALUATIONS],
		c[NRI_TRACE_SHADER_ABSENCE_CANDIDATE_REJECTS],
		c[NRI_TRACE_SHADER_ABSENCE_FALLBACK_EVALUATIONS],
		c[NRI_TRACE_SHADER_ABSENCE_STATIC_RESTARTS],
		c[NRI_TRACE_SHADER_ABSENCE_ACTOR_RESTARTS]);
	Printf("PERF pt shader absence pairs NRI: schema=1 frame=%llu stats_frame=%llu trace_flags=0x%08x pair_visits=%u pair_bounds_hits=%u positive_evaluations=%u\n",
		(unsigned long long)frameNumber,
		(unsigned long long)snapshot.frameNumber,
		snapshot.dispatchMetadata.traceFlags,
		c[NRI_TRACE_SHADER_ABSENCE_PAIR_VISITS],
		c[NRI_TRACE_SHADER_ABSENCE_PAIR_BOUNDS_HITS],
		c[NRI_TRACE_SHADER_ABSENCE_POSITIVE_EVALUATIONS]);
	Printf("PERF pt shader absence oracle NRI: schema=1 frame=%llu stats_frame=%llu trace_flags=0x%08x comparisons=%u inside_mismatch=%u validity_mismatch=%u probe_mismatch=%u\n",
		(unsigned long long)frameNumber,
		(unsigned long long)snapshot.frameNumber,
		snapshot.dispatchMetadata.traceFlags,
		c[NRI_TRACE_SHADER_ABSENCE_ORACLE_COMPARISONS],
		c[NRI_TRACE_SHADER_ABSENCE_ORACLE_INSIDE_MISMATCH],
		c[NRI_TRACE_SHADER_ABSENCE_ORACLE_VALIDITY_MISMATCH],
		c[NRI_TRACE_SHADER_ABSENCE_ORACLE_PROBE_MISMATCH]);
}
