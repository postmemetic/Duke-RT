#include "perf_route_observer.h"

#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "c_cvars.h"
#include "files.h"
#include "perf_audit.h"
#include "printf.h"
#include "v_video.h"

EXTERN_CVAR(Bool, nri_ptvoxeloccurrencetrace)
EXTERN_CVAR(Int, nri_ptvoxeloccurrenceactor)
EXTERN_CVAR(Int, nri_pttraceframes)
EXTERN_CVAR(Int, perf_looptraceframes)

namespace
{
constexpr size_t MaxShots = 16, MaxPath = 1023;
constexpr uint64_t MaxOccurrenceFrames = 128;
struct Shot { uint64_t frame = 0; std::string path; bool queued = false; };
std::array<Shot, MaxShots> shots;
size_t shotCount = 0, queuedCount = 0;
bool windowSet = false, armed = false, failed = false, controlsOwned = false;
bool occurrenceThisDisplay = false;
uint64_t windowFirst = 0, windowLast = 0, windowSeen = 0, ownedFrame = 0;
int actorIndex = -1;

bool Number(const char* text, uint64_t& value)
{
    if (!text || !*text) return false;
    value = 0;
    for (const char* p = text; *p; ++p)
    {
        if (*p < '0' || *p > '9') return false;
        const uint64_t digit = uint64_t(*p - '0');
        if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10) return false;
        value = value * 10 + digit;
    }
    return true;
}

bool PathAllowed(const char* path)
{
    if (!path) return false;
    const size_t length = strlen(path);
    if (length < 5 || length > MaxPath || strcmp(path + length - 4, ".png")) return false;
#ifdef _WIN32
    if (length < 7 || !std::isalpha(static_cast<unsigned char>(path[0])) || path[1] != ':' || path[2] != '/') return false;
#else
    if (path[0] != '/') return false;
#endif
    // Local capture paths use forward slashes and a deliberately small alphabet.
    // They are safe inside both quoted text logs and JSON without escaping.
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(path); *p; ++p)
        if ((*p == ':' && p != reinterpret_cast<const unsigned char*>(path) + 1) ||
            !((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == ' ' || *p == '.' || *p == '_' ||
              *p == '-' || *p == ':' || *p == '/')) return false;
    return true;
}

bool BaselineInactive()
{
    return !nri_ptvoxeloccurrencetrace && (int)nri_ptvoxeloccurrenceactor == -1 &&
        (int)nri_pttraceframes == 0 && (int)perf_looptraceframes == 0;
}

void Failure(const char* reason)
{
    failed = true;
    Printf("PERF route observer: status=failed reason=%s\n", reason);
    char fields[256];
    snprintf(fields, sizeof(fields), "\"status\":\"failed\",\"reason\":\"%s\"", reason);
    PerfAuditWriteEvent("route_observer_failure", PerfAuditCurrentToken(), fields);
}

FILE* CreateExclusive(const char* path)
{
#ifdef _WIN32
    const int fd = _open(path, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
    if (fd < 0) return nullptr;
    FILE* output = _fdopen(fd, "wb");
    if (!output) _close(fd);
#else
    const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
    if (fd < 0) return nullptr;
    FILE* output = fdopen(fd, "wb");
    if (!output) close(fd);
#endif
    return output;
}

void TraceControls(const char* stage)
{
    const auto token = PerfAuditCurrentToken();
    char fields[512];
    snprintf(fields, sizeof(fields), "\"route_frame\":%llu,\"stage\":\"%s\",\"occurrence\":%s,\"actor\":%d,\"pt_traceframes\":%d,\"loop_traceframes\":%d",
        (unsigned long long)ownedFrame, stage, nri_ptvoxeloccurrencetrace ? "true" : "false",
        (int)nri_ptvoxeloccurrenceactor, (int)nri_pttraceframes, (int)perf_looptraceframes);
    PerfAuditWriteEvent("route_observer_controls", token, fields);
    Printf("PERF route observer controls: stage=%s route_frame=%llu epoch=%llu presentation=%llu occurrence=%u actor=%d pt_traceframes=%d loop_traceframes=%d\n",
        stage, (unsigned long long)ownedFrame, (unsigned long long)token.epoch,
        (unsigned long long)token.presentation, nri_ptvoxeloccurrencetrace ? 1u : 0u,
        (int)nri_ptvoxeloccurrenceactor, (int)nri_pttraceframes, (int)perf_looptraceframes);
}
}

namespace PerfRouteObserver
{
bool Configured() { return shotCount != 0 || windowSet; }

bool AddShot(const char* frameText, const char* path)
{
    uint64_t frame = 0;
    if (armed || shotCount >= MaxShots || !Number(frameText, frame) || !PathAllowed(path)) return false;
    // One shot per requested presentation avoids a readback burst. Paths may not
    // be reused; exclusive creation at the actual request is authoritative.
    for (size_t i = 0; i < shotCount; ++i)
        if (shots[i].frame == frame || shots[i].path == path) return false;
    shots[shotCount++] = {frame, path, false};
    Printf("PERF route observer: status=declared action=shot route_frame=%llu path=\"%s\"\n",
        (unsigned long long)frame, path);
    return true;
}

bool SetOccurrence(const char* firstText, const char* lastText, const char* actorText)
{
    uint64_t first = 0, last = 0, actor = 0;
    if (armed || windowSet || !Number(firstText, first) || !Number(lastText, last) ||
        !Number(actorText, actor) || actor > uint64_t(std::numeric_limits<int>::max()) ||
        last < first || last - first >= MaxOccurrenceFrames) return false;
    windowFirst = first; windowLast = last; actorIndex = int(actor); windowSet = true;
    Printf("PERF route observer: status=declared action=occurrence first=%llu last=%llu actor=%d\n",
        (unsigned long long)first, (unsigned long long)last, actorIndex);
    return true;
}

void Clear()
{
    AfterDisplay();
    for (auto& shot : shots) shot = {};
    shotCount = queuedCount = 0;
    windowSet = armed = failed = false;
    windowFirst = windowLast = windowSeen = ownedFrame = 0;
    actorIndex = -1;
}

bool Arm(uint64_t routeFrames)
{
    if (!Configured()) return true;
    if (armed || !routeFrames || !PerfAuditActive() || !BaselineInactive())
    { Failure("arm-requires-audit-and-inactive-trace-baseline"); return false; }
    for (size_t i = 0; i < shotCount; ++i)
        if (shots[i].frame >= routeFrames) { Failure("shot-outside-route"); return false; }
    if (windowSet && windowLast >= routeFrames) { Failure("occurrence-window-outside-route"); return false; }
    armed = true;
    char fields[512];
    snprintf(fields, sizeof(fields), "\"shots\":%u,\"route_frames\":%llu,\"window_set\":%s,\"window_first\":%llu,\"window_last\":%llu,\"actor\":%d,\"baseline_occurrence\":false,\"baseline_actor\":-1,\"baseline_pt_traceframes\":0,\"baseline_loop_traceframes\":0",
        (unsigned)shotCount, (unsigned long long)routeFrames, windowSet ? "true" : "false",
        (unsigned long long)windowFirst, (unsigned long long)windowLast, actorIndex);
    PerfAuditWriteEvent("route_observer_begin", PerfAuditCurrentToken(), fields);
    Printf("PERF route observer: status=armed shots=%u route_frames=%llu window_set=%u first=%llu last=%llu actor=%d baseline=inactive\n",
        (unsigned)shotCount, (unsigned long long)routeFrames, windowSet ? 1u : 0u,
        (unsigned long long)windowFirst, (unsigned long long)windowLast, actorIndex);
    return true;
}

void BeforeDisplay(uint64_t routeFrame)
{
    if (!armed || failed) return;
    const bool occurrence = windowSet && routeFrame >= windowFirst && routeFrame <= windowLast;
    bool takeShot = false;
    for (size_t i = 0; i < shotCount; ++i) takeShot |= shots[i].frame == routeFrame;
    if (!occurrence && !takeShot) return;
    if (controlsOwned || !PerfAuditActive() || !BaselineInactive() || !screen)
    { Failure("display-requires-audit-screen-and-inactive-trace-baseline"); return; }
    ownedFrame = routeFrame; occurrenceThisDisplay = occurrence; controlsOwned = true;
    nri_ptvoxeloccurrencetrace = occurrence;
    nri_ptvoxeloccurrenceactor = occurrence ? actorIndex : -1;
    // One presentation at a time. The renderer may consume 1 to 0; after Display
    // only the verified inactive baseline is restored, never a positive count.
    nri_pttraceframes = 1;
    if (occurrence) ++windowSeen;
    TraceControls("before-display");
    for (size_t i = 0; i < shotCount; ++i)
    {
        auto& shot = shots[i];
        if (shot.frame != routeFrame) continue;
        if (shot.queued) { Failure("duplicate-shot-request"); return; }
        const auto token = PerfAuditCurrentToken();
        char fields[1400];
        snprintf(fields, sizeof(fields), "\"shot\":%u,\"requested_route_frame\":%llu,\"boundary\":\"before-display\",\"path\":\"%s\"",
            (unsigned)i, (unsigned long long)routeFrame, shot.path.c_str());
        PerfAuditWriteEvent("route_observer_shot", token, fields);
        Printf("PERF route observer shot: shot=%u requested_route_frame=%llu epoch=%llu presentation=%llu boundary=before-display path=\"%s\"\n",
            (unsigned)i, (unsigned long long)routeFrame, (unsigned long long)token.epoch,
            (unsigned long long)token.presentation, shot.path.c_str());
        std::unique_ptr<FILE, decltype(&fclose)> file(CreateExclusive(shot.path.c_str()), fclose);
        if (!file) { Failure("shot-exclusive-create-failed"); return; }
        std::unique_ptr<FileWriter> writer(new FileWriter(file.get()));
        file.release(); // The writer owns the exclusively-created file.
        if (!screen->QueueScreenshot(writer.get(), shot.path.c_str()))
        { Failure("shot-queue-failed-no-synchronous-fallback"); return; }
        writer.release(); // The screenshot owner now owns this writer.
        shot.queued = true; ++queuedCount;
    }
}

void AfterDisplay()
{
    if (!controlsOwned) return;
    const bool unexpected = (bool)nri_ptvoxeloccurrencetrace != occurrenceThisDisplay ||
        (int)nri_ptvoxeloccurrenceactor != (occurrenceThisDisplay ? actorIndex : -1) ||
        (int)nri_pttraceframes < 0 || (int)nri_pttraceframes > 1 || (int)perf_looptraceframes != 0;
    nri_ptvoxeloccurrencetrace = false;
    nri_ptvoxeloccurrenceactor = -1;
    nri_pttraceframes = 0;
    controlsOwned = false;
    TraceControls("after-display");
    if (unexpected) Failure("owned-trace-controls-changed");
}

void Finish(const char* routeStatus, const char* routeReason)
{
    AfterDisplay();
    if (!Configured()) return;
    const uint64_t expectedWindow = windowSet ? windowLast - windowFirst + 1 : 0;
    const bool complete = armed && !failed && queuedCount == shotCount && windowSeen == expectedWindow &&
        !strcmp(routeStatus, "played") && BaselineInactive();
    char fields[512];
    snprintf(fields, sizeof(fields), "\"complete\":%s,\"route_status\":\"%s\",\"route_reason\":\"%s\",\"shots_declared\":%u,\"shots_queued\":%u,\"window_expected\":%llu,\"window_observed\":%llu,\"baseline_restored\":%s,\"image_completion_requires_external_join\":true",
        complete ? "true" : "false", routeStatus, routeReason, (unsigned)shotCount, (unsigned)queuedCount,
        (unsigned long long)expectedWindow, (unsigned long long)windowSeen, BaselineInactive() ? "true" : "false");
    PerfAuditWriteEvent("route_observer_end", PerfAuditCurrentToken(), fields);
    Printf("PERF route observer: status=%s route_status=%s route_reason=%s shots=%u/%u occurrence_frames=%llu/%llu baseline_restored=%u image_completion=external-join\n",
        complete ? "requests-served" : "incomplete", routeStatus, routeReason,
        (unsigned)queuedCount, (unsigned)shotCount, (unsigned long long)windowSeen,
        (unsigned long long)expectedWindow, BaselineInactive() ? 1u : 0u);
    Clear();
}
}
