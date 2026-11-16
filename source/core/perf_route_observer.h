#pragma once
#include <cstdint>

// Local diagnostic actions only. Configuration is accepted by perf_route's
// idle-only commands; an armed schedule is consumed by one playback.
namespace PerfRouteObserver
{
bool Configured();
bool AddShot(const char* frame, const char* absoluteNewPng);
bool SetOccurrence(const char* first, const char* last, const char* actor);
void Clear();
bool Arm(uint64_t routeFrames);
void BeforeDisplay(uint64_t routeFrame);
void AfterDisplay();
void Finish(const char* routeStatus, const char* routeReason);
}

// Display can throw before PerfRouteEndPresentation. Restore temporary
// diagnostic controls during unwinding as well as ordinary presentation.
class PerfRouteObserverDisplayScope
{
public:
    PerfRouteObserverDisplayScope() = default;
    ~PerfRouteObserverDisplayScope() { PerfRouteObserver::AfterDisplay(); }
    PerfRouteObserverDisplayScope(const PerfRouteObserverDisplayScope&) = delete;
    PerfRouteObserverDisplayScope& operator=(const PerfRouteObserverDisplayScope&) = delete;
};
