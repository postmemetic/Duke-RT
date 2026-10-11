#pragma once
#include <cstdint>
struct NRITraceShaderStatsSnapshot;
void LogNRIStaticTangentTrial(uint64_t frameNumber, const NRITraceShaderStatsSnapshot& snapshot);
