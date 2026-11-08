#pragma once

// Main-thread, sampled audit only. Enumerates linked actors, not GC allocations.
// Does not depend on the local route recorder or change game/renderer state.
void PerfAuditCaptureActors();
