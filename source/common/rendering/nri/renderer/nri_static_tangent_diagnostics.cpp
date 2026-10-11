#include "nri_static_tangent_diagnostics.h"
#include "nri_trace_stats.h"
#include "printf.h"

void LogNRIStaticTangentTrial(uint64_t frameNumber, const NRITraceShaderStatsSnapshot& snapshot)
{
    if (!snapshot.valid) return;
    const auto& d = snapshot.dispatchMetadata.staticTangents;
    const auto& c = snapshot.counters;
    Printf("PERF pt shader static tangent NRI: schema=1 frame=%llu stats_frame=%llu trace_flags=0x%08x requested=%u active=%u route_reason=%u owner_reason=%u prims=%u probe_dispatches_remaining=%u output_allocated_bytes=%llu fallback_allocated_bytes=%llu total_owner_allocated_bytes=%llu builds=%llu comparisons=%u raw_mismatch=%u success_mismatch=%u frame_mismatch=%u mapped_mismatch=%u valid=%u degenerate=%u unsupported=%u\n",
        (unsigned long long)frameNumber, (unsigned long long)snapshot.frameNumber,
        snapshot.dispatchMetadata.traceFlags, d.requestedMode, d.activeMode,
        (unsigned)d.reason, (unsigned)d.ownerReason, d.primitiveCount,
        d.probeDispatchesRemaining, (unsigned long long)d.outputAllocatedBytes,
        (unsigned long long)d.fallbackAllocatedBytes, (unsigned long long)d.totalOwnerAllocatedBytes,
        (unsigned long long)d.builds,
        c[NRI_TRACE_SHADER_STATIC_TANGENT_COMPARISONS],
        c[NRI_TRACE_SHADER_STATIC_TANGENT_RAW_MISMATCH],
        c[NRI_TRACE_SHADER_STATIC_TANGENT_SUCCESS_MISMATCH],
        c[NRI_TRACE_SHADER_STATIC_TANGENT_FRAME_MISMATCH],
        c[NRI_TRACE_SHADER_STATIC_TANGENT_MAPPED_MISMATCH],
        c[NRI_TRACE_SHADER_STATIC_TANGENT_VALID],
        c[NRI_TRACE_SHADER_STATIC_TANGENT_DEGENERATE],
        c[NRI_TRACE_SHADER_STATIC_TANGENT_UNSUPPORTED]);
}
