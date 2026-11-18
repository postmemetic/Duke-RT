#pragma once

#include "perf_audit.h"
#include <array>
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

// Exclusive CPU spans for one geometry reservation. Record timings only while
// the native audit is active. The graphics fence samples are nonblocking.
struct NRIVoxelAllocationTiming
{
    struct Resource
    {
        uint64_t bytes = 0;
        uint64_t createNs = 0;
        uint64_t viewNs = 0;
        uint64_t discardNs = 0;
    };
    // Vertex/index/primitive arenas, private vertex/index, scratch, then BLAS.
    std::array<Resource, 7> resources{};
    struct InterimBudget
    {
        int mode = 0; // Populated only at an eligible checkpoint.
        bool liveAvailable = false, wouldDefer = false;
        uint64_t queryNs = 0, liveUsage = 0, liveBudget = 0, effectiveBudget = 0;
        uint64_t committed = 0, remaining = 0, projected = 0;
    } interim;
    uint64_t budgetBeforeNs = 0, preflightNs = 0, budgetAfterNs = 0;
    uint64_t copyRecordNs = 0, copyBytes = 0, retireNs = 0;
    uint64_t recordingFence = 0, submittedFence = 0;
    uint64_t completedBefore = UINT64_MAX, completedAfter = UINT64_MAX;
    const bool active = PerfAuditActive();
    bool attempted = false;

    template<class Operation>
    auto Measure(uint64_t& ns, Operation&& operation) const -> decltype(operation())
    {
        if (!active) return operation();
        struct Timer
        {
            uint64_t& total;
            uint64_t start;
            ~Timer() { total += PerfAuditClockNs() - start; }
        } timer{ns, PerfAuditClockNs()};
        return operation();
    }
};

void NRIAuditVoxelAllocationTiming(const NRIVoxelAllocationTiming& timing, uint32_t frame,
    uint64_t meshKey, const char* decision);
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
void NRIAuditResidentRetiredDestroy(uint32_t rendererFrame, uint32_t previousSlotFrame,
    uint32_t slot, bool explicitReset, uint64_t bufferCount, uint64_t bufferBytes,
    uint64_t accelerationCount, uint64_t accelerationBytes, double bufferMs, double accelerationMs);
