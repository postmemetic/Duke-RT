#pragma once
#include <atomic>
#include <cstdint>

// Session diagnostic facility: disabled unless explicitly started. Never archives settings.
enum class PerfAuditMetric : uint16_t
{
#define PERF_AUDIT_METRIC(name, kind) name,
#include "perf_audit_metrics.inc"
#undef PERF_AUDIT_METRIC
    Count
};

struct PerfAuditToken
{
    uint64_t epoch = 0;
    uint64_t checkpoint = 0;
    uint64_t presentation = 0;
    int64_t tic = 0;
    explicit operator bool() const { return epoch != 0; }
};

extern std::atomic<bool> gPerfAuditActive;
inline bool PerfAuditActive() { return gPerfAuditActive.load(std::memory_order_relaxed); }
bool PerfAuditCensusRequested();
void PerfAuditAdd(PerfAuditMetric metric, uint64_t value = 1);
void PerfAuditSet(PerfAuditMetric metric, uint64_t value);
uint64_t PerfAuditClockNs();
PerfAuditToken PerfAuditCurrentToken();
void PerfAuditBeginFrame(uint64_t presentation, int64_t tic);
// Align periodic census to a route's first presentation, independent of load time.
void PerfAuditAlignSampling();
void PerfAuditEndFrame();
// Real-clock loop timings; no simulation-clock override. Called only while active.
void PerfAuditNoteLoop(double tryMs, double displayMs, double startTicMs, double musicMs,
    double frameMs, uint32_t ticks, bool levelRendered);
void PerfAuditMark(const char* label);
// Main thread only. fieldsJson contains JSON object members, without outer braces.
// The caller must escape strings. Fixed diagnostic data only, no user commands.
void PerfAuditWriteEvent(const char* kind, const PerfAuditToken& token, const char* fieldsJson);
bool PerfAuditStart(const char* path);
void PerfAuditStop();

class PerfAuditScope
{
    PerfAuditMetric mMetric;
    uint64_t mStart = 0;
public:
    explicit PerfAuditScope(PerfAuditMetric metric) : mMetric(metric)
    {
        if (PerfAuditActive()) mStart = PerfAuditClockNs();
    }
    ~PerfAuditScope()
    {
        if (mStart) PerfAuditAdd(mMetric, PerfAuditClockNs() - mStart);
    }
    PerfAuditScope(const PerfAuditScope&) = delete;
    PerfAuditScope& operator=(const PerfAuditScope&) = delete;
};
