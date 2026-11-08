#pragma once

#include "perf_audit.h"
#include <cstdint>
#include <vector>

struct NRITraceShaderDispatchMetadata;
struct NRITraceShaderInstanceAttribution;
class SceneLightSystem;
namespace nri { struct TopLevelInstance; }
struct SceneInstanceData;

// Trace-only use of bit 1; the present shader independently uses this bit for
// upscale selection. Diagnostic shaders alone compile the audit interpretation.
constexpr uint32_t NRI_TRACE_FLAG_PERF_AUDIT = 0x2u;
struct NRITraceAuditCapture
{
    PerfAuditToken token;
    uint64_t request = 0;
    explicit operator bool() const { return request != 0; }
};

bool NRITraceAuditRequested();
NRITraceAuditCapture NRIBeginTraceAudit(bool diagnostic, bool conflictingProbe, uint64_t rendererFrame, uint64_t fence);
void NRITraceAuditLoss(const NRITraceAuditCapture& capture, uint64_t rendererFrame, uint64_t fence, const char* reason, bool abandoned = false);
void NRIPublishTraceAudit(const NRITraceAuditCapture& capture, uint64_t rendererFrame, uint64_t fence,
    const NRITraceShaderDispatchMetadata& metadata, const uint32_t* counters,
    const std::vector<NRITraceShaderInstanceAttribution>& attribution, uint32_t totalInstances);
void NRIAuditEmissiveCensus(const SceneLightSystem& lights);
void NRIAuditTlasCensus(const std::vector<nri::TopLevelInstance>& instances,
    const std::vector<SceneInstanceData>& records, uint64_t rendererFrame, uint64_t mapEpoch);
// Sparse event at actual resident arena growth, independent of census cadence.
// CPU time includes allocation/view creation/copy recording, not GPU copy execution.
void NRIAuditResidentArenaGrowth(uint32_t rendererFrame, uint32_t stride,
    uint64_t oldSize, uint64_t oldMemory, uint64_t oldUsed, uint64_t required,
    uint64_t newSize, uint64_t newMemory, uint64_t copied, bool retiredToSlot,
    double allocationMs, double totalMs);
