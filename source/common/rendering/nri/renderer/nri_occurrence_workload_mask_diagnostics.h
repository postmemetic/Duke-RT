#pragma once

#include <cstdint>

struct NRIOccurrenceWorkloadMaskStats;

void LogNRIOccurrenceWorkloadMaskStats(uint64_t frameNumber, const NRIOccurrenceWorkloadMaskStats& stats);
