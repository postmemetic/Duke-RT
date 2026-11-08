#include "perf_audit.h"
#include "c_cvars.h"
#include "c_dispatch.h"
#include "i_time.h"
#include "printf.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <cinttypes>
#include <cstdarg>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

std::atomic<bool> gPerfAuditActive{false};
CUSTOM_CVAR(Int, perf_audit_interval, 30, 0)
{
    if (self < 1) self = 1;
    else if (self > 4096) self = 4096;
}

namespace
{
    constexpr size_t MetricCount = static_cast<size_t>(PerfAuditMetric::Count);
    constexpr uint64_t MaxFileBytes = 512ull * 1024 * 1024;
    constexpr size_t MaxEventBytes = 65536;
    const char* const Names[] = {
#define PERF_AUDIT_METRIC(name, kind) #name,
#include "perf_audit_metrics.inc"
#undef PERF_AUDIT_METRIC
    };
    const char* const Kinds[] = {
#define PERF_AUDIT_METRIC(name, kind) #kind,
#include "perf_audit_metrics.inc"
#undef PERF_AUDIT_METRIC
    };
    std::array<std::atomic<uint64_t>, MetricCount> Values{};
    std::array<std::atomic<bool>, MetricCount> Observed{};
    FILE* Output = nullptr;
    char OutputBuffer[65536];
    PerfAuditToken Token;
    uint64_t Epoch = 0, FrameCount = 0, FileBytes = 0, FrameStart = 0, CensusOrigin = 0, PendingCheckpoints = 0;
    bool Census = false, PendingCensus = false, Failed = false;

    void Write(const char* format, ...)
    {
        if (!Output) return;
        char line[4096];
        va_list args;
        va_start(args, format);
        const int length = vsnprintf(line, sizeof(line), format, args);
        va_end(args);
        if (length < 0 || static_cast<size_t>(length) >= sizeof(line) ||
            FileBytes + static_cast<uint64_t>(length) > MaxFileBytes ||
            fwrite(line, 1, static_cast<size_t>(length), Output) != static_cast<size_t>(length))
        {
            Failed = true;
            gPerfAuditActive.store(false, std::memory_order_relaxed);
            Printf("PERF audit failed: output-full-or-write-error; capture invalid\n");
            fclose(Output);
            Output = nullptr;
            return;
        }
        FileBytes += length;
    }

    void Prefix(const char* kind, const PerfAuditToken& token)
    {
        Write("{\"kind\":\"%s\",\"epoch\":%" PRIu64 ",\"checkpoint\":%" PRIu64
            ",\"presentation\":%" PRIu64 ",\"tic\":%" PRId64, kind,
            token.epoch, token.checkpoint, token.presentation, token.tic);
    }

    void Sample()
    {
#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS_EX counters{};
        counters.cb = sizeof(counters);
        if (GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)))
        {
            PerfAuditSet(PerfAuditMetric::CpuPrivateBytes, counters.PrivateUsage);
            PerfAuditSet(PerfAuditMetric::CpuWorkingSetBytes, counters.WorkingSetSize);
        }
#endif
        PerfAuditSet(PerfAuditMetric::AuditStorageBytes, sizeof(Values) + sizeof(Observed) + sizeof(OutputBuffer));
        Prefix("sample", Token);
        Write(",\"values\":{");
        for (size_t i = 0; i < MetricCount; ++i)
            Write("%s\"%s\":%" PRIu64, i ? "," : "", Names[i], Values[i].load(std::memory_order_relaxed));
        Write("},\"observed\":[");
        bool first = true;
        for (size_t i = 0; i < MetricCount; ++i)
        {
            if (!Observed[i].load(std::memory_order_relaxed)) continue;
            Write("%s\"%s\"", first ? "" : ",", Names[i]);
            first = false;
        }
        Write("]}\n");
    }
}

uint64_t PerfAuditClockNs() { return I_nsTime(); }
bool PerfAuditCensusRequested() { return PerfAuditActive() && Census; }
PerfAuditToken PerfAuditCurrentToken() { return PerfAuditActive() ? Token : PerfAuditToken{}; }

void PerfAuditAdd(PerfAuditMetric metric, uint64_t value)
{
    if (!PerfAuditActive()) return;
    const auto i = static_cast<size_t>(metric);
    if (i >= MetricCount) return;
    Values[i].fetch_add(value, std::memory_order_relaxed);
    Observed[i].store(true, std::memory_order_relaxed);
}

void PerfAuditSet(PerfAuditMetric metric, uint64_t value)
{
    if (!PerfAuditActive()) return;
    const auto i = static_cast<size_t>(metric);
    if (i >= MetricCount) return;
    Values[i].store(value, std::memory_order_relaxed);
    Observed[i].store(true, std::memory_order_relaxed);
}

void PerfAuditBeginFrame(uint64_t presentation, int64_t tic)
{
    if (!PerfAuditActive()) return;
    Token.presentation = presentation;
    Token.tic = tic;
    Token.checkpoint += PendingCheckpoints;
    PendingCheckpoints = 0;
    Census = PendingCensus || (FrameCount - CensusOrigin) % static_cast<uint64_t>(static_cast<int>(perf_audit_interval)) == 0;
    PendingCensus = false;
    FrameStart = I_nsTime();
}

void PerfAuditAlignSampling()
{
    if (!PerfAuditActive()) return;
    CensusOrigin = FrameCount;
    Census = true;
    PendingCensus = false;
}

void PerfAuditEndFrame()
{
    if (!PerfAuditActive()) return;
    const uint64_t now = I_nsTime();
    Prefix("frame", Token);
    Write(",\"index\":%" PRIu64 ",\"wall_ns\":%" PRIu64 ",\"census\":%s}\n",
        FrameCount++, FrameStart ? now - FrameStart : 0, Census ? "true" : "false");
    if (Census) Sample();
    Census = false;
}

void PerfAuditMark(const char* label)
{
    if (!PerfAuditActive()) return;
    // Route markers arrive after Display. Keep the just-rendered frame and its
    // asynchronous producers in the old interval; the new interval starts next frame.
    PerfAuditToken markerToken = Token;
    markerToken.checkpoint += ++PendingCheckpoints;
    PendingCensus = true;
    char safe[257] = {};
    size_t n = 0;
    for (const unsigned char* c = reinterpret_cast<const unsigned char*>(label ? label : "");
        *c && n + 2 < sizeof(safe); ++c)
    {
        if (*c == '"' || *c == '\\') safe[n++] = '\\';
        safe[n++] = *c < 32 ? '_' : static_cast<char>(*c);
    }
    Prefix("marker", markerToken);
    Write(",\"label\":\"%s\"}\n", safe);
    Printf("PERF audit marker: checkpoint=%" PRIu64 " label=%s\n", markerToken.checkpoint, safe);
}

void PerfAuditWriteEvent(const char* kind, const PerfAuditToken& token, const char* fieldsJson)
{
    if (!PerfAuditActive() || !token || token.epoch != Token.epoch) return;
    const size_t size = fieldsJson ? strlen(fieldsJson) : 0;
    if (size > MaxEventBytes)
    {
        PerfAuditAdd(PerfAuditMetric::AuditEventsDropped);
        return;
    }
    Prefix(kind, token);
    if (size)
    {
        Write(",");
        // Bounded event data can exceed the small formatting buffer.
        if (Output && FileBytes + size <= MaxFileBytes && fwrite(fieldsJson, 1, size, Output) == size)
            FileBytes += size;
        else
        {
            Failed = true;
            gPerfAuditActive.store(false, std::memory_order_relaxed);
            if (Output) fclose(Output);
            Output = nullptr;
            Printf("PERF audit failed: event-write-error; capture invalid\n");
            return;
        }
    }
    Write("}\n");
}

bool PerfAuditStart(const char* path)
{
    if (PerfAuditActive() || Output || !path || !*path)
    {
        Printf("PERF audit start rejected: already-active-or-empty-path\n");
        return false;
    }
    FILE* exists = fopen(path, "rb");
    if (exists)
    {
        fclose(exists);
        Printf("PERF audit start rejected: output exists (%s)\n", path);
        return false;
    }
    Output = fopen(path, "wb");
    if (!Output)
    {
        Printf("PERF audit start rejected: cannot-create (%s)\n", path);
        return false;
    }
    setvbuf(Output, OutputBuffer, _IOFBF, sizeof(OutputBuffer));
    for (size_t i = 0; i < MetricCount; ++i)
    {
        Values[i].store(0, std::memory_order_relaxed);
        Observed[i].store(false, std::memory_order_relaxed);
    }
    Token = {};
    Token.epoch = ++Epoch;
    FrameCount = FileBytes = CensusOrigin = PendingCheckpoints = 0;
    FrameStart = I_nsTime();
    Census = PendingCensus = true;
    Failed = false;
    gPerfAuditActive.store(true, std::memory_order_relaxed);
    Prefix("schema", Token);
    Write(",\"version\":1,\"limit_bytes\":%" PRIu64 ",\"interval\":%d,\"metrics\":{",
        MaxFileBytes, static_cast<int>(perf_audit_interval));
    for (size_t i = 0; i < MetricCount; ++i)
        Write("%s\"%s\":\"%s\"", i ? "," : "", Names[i], Kinds[i]);
    Write("}}\n");
    Printf("PERF audit started: %s interval=%d\n", path, static_cast<int>(perf_audit_interval));
    return PerfAuditActive();
}

void PerfAuditStop()
{
    if (!Output) return;
    Sample();
    auto value = [](PerfAuditMetric m) { return Values[static_cast<size_t>(m)].load(std::memory_order_relaxed); };
    const uint64_t requested = value(PerfAuditMetric::AuditReadbacksRequested);
    const uint64_t terminal = value(PerfAuditMetric::AuditReadbacksPublished) +
        value(PerfAuditMetric::AuditReadbacksDropped) + value(PerfAuditMetric::AuditReadbacksAbandoned);
    const uint64_t outstanding = requested > terminal ? requested - terminal : 0;
    const uint64_t timingRequested = value(PerfAuditMetric::GpuTimingRequests);
    const uint64_t timingTerminal = value(PerfAuditMetric::GpuTimingPublished) + value(PerfAuditMetric::GpuTimingLost);
    const uint64_t timingOutstanding = timingRequested > timingTerminal ? timingRequested - timingTerminal : 0;
    const bool valid = !Failed && outstanding == 0 && value(PerfAuditMetric::AuditReadbacksDropped) == 0 &&
        value(PerfAuditMetric::AuditReadbacksAbandoned) == 0 && value(PerfAuditMetric::AuditReadbacksOverflow) == 0 &&
        value(PerfAuditMetric::AuditEventsDropped) == 0 && timingOutstanding == 0 && value(PerfAuditMetric::GpuTimingLost) == 0;
    Prefix("end", Token);
    Write(",\"complete\":true,\"valid\":%s,\"frames\":%" PRIu64
        ",\"shader_outstanding\":%" PRIu64 ",\"timing_outstanding\":%" PRIu64 ",\"bytes\":%" PRIu64 "}\n",
        valid ? "true" : "false", FrameCount, outstanding, timingOutstanding, FileBytes);
    gPerfAuditActive.store(false, std::memory_order_relaxed);
    if (Output)
    {
        const bool ok = fflush(Output) == 0;
        const int closeResult = fclose(Output);
        Output = nullptr;
        Printf("PERF audit stopped: frames=%" PRIu64 " valid=%d outstanding=%" PRIu64 "\n",
            FrameCount, valid && ok && closeResult == 0 ? 1 : 0, outstanding);
    }
    Census = PendingCensus = false;
}

UNSAFE_CCMD(perf_audit_start)
{
    if (argv.argc() != 2) { Printf("usage: perf_audit_start <new-jsonl-path>\n"); return; }
    PerfAuditStart(argv[1]);
}
CCMD(perf_audit_stop) { PerfAuditStop(); }
CCMD(perf_audit_mark) { PerfAuditMark(argv.argc() > 1 ? argv[1] : "checkpoint"); }
