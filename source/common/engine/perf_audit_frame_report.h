#pragma once

// Main thread only. Register a static reporter after recording this frame's
// first observation. Pending reporters run once before the frame or audit ends.
using PerfAuditFrameReporter = void (*)();
void PerfAuditDeferFrameReport(PerfAuditFrameReporter report);
