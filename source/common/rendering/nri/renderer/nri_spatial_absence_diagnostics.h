#pragma once

#include <cstdint>

struct NRITraceShaderStatsSnapshot;

void LogNRISpatialAbsenceProfile(uint64_t frameNumber, const NRITraceShaderStatsSnapshot& snapshot);
