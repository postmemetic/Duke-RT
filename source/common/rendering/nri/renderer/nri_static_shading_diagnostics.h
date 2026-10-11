#pragma once
#include <cstdint>
struct NRITraceShaderStatsSnapshot;
void LogNRIStaticShadingApplicability(uint64_t frameNumber, const NRITraceShaderStatsSnapshot& snapshot);
