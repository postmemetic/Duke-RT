#pragma once
#include <cstdint>

class NRIRenderer;
// Consumes existing per-frame owner snapshots. No scene traversal or readback.
void NRICaptureCpuFrame(const NRIRenderer& renderer, uint64_t boundaryFrame, bool rendered);
