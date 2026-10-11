#pragma once

#include <cstdint>

struct NRITraceShaderStatsSnapshot;

void LogNRIPrimaryTemporalGeometryOracle(uint64_t frameNumber, const NRITraceShaderStatsSnapshot& snapshot);
