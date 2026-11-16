#pragma once

#include <cstdint>

struct InputPacket;
struct event_t;

// Local single-player performance fixtures, not the game's demo/network format.
bool PerfRoutePlaying();
bool PerfRouteActive();
bool PerfRouteAwaitingBoundary();
// Actual save execution, after old-world unload and before reading the save.
// False means a diagnostic failure already ended the route; loading must abort.
bool PerfRouteBeforeLoad();
void PerfRouteLoaded();
bool PerfRouteHoldAtBoundary();
void PerfRouteBeginPresentation();
void PerfRouteBeforeDisplay();
void PerfRouteEndPresentation();
int PerfRoutePlaybackTicks();
void PerfRouteBeforePlaybackTick();
void PerfRouteCommand(InputPacket& input, bool& synchronous);
bool PerfRouteAllowSpecials(int player, int buffer);
uint64_t PerfRouteFrameTime(uint64_t realFrameTime);
bool PerfRouteFilterEvent(event_t* event);

// Duke-only fixture setup, called only after the route's E2L6/E2L8 boundary
// check. Noclip is session state and is absent from ordinary save serialization.
bool PerfRouteApplyDukeFixture();
bool PerfRoutePrepareDukeFixture();
