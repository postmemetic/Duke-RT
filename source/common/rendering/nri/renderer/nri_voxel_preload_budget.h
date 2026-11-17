#pragma once

#include <cstdint>
#include <vector>

struct NRIVoxelPreloadBudgetLimits
{
    uint64_t usage = 0, budget = 0, reserve = 0;
    uint32_t peakPercent = 175;
    bool strict = false;
};
struct NRIVoxelPreloadBudgetResource
{
    uint64_t mesh = 0, geometryBytes = 0, source = 0, sourceBytes = 0;
    bool preload = true;
};
struct NRIVoxelPreloadBudgetGroup
{
    bool family = false, eligible = true;
    int32_t order = 0;
    std::vector<NRIVoxelPreloadBudgetResource> resources;
};
struct NRIVoxelPreloadBudgetSelection
{
    bool applied = false;
    uint64_t geometryBytes = 0, tailBytes = 0, sourceBytes = 0, largestBytes = 0, peakAdditional = 0;
    std::vector<uint32_t> selected;
};

uint64_t EstimateNRIVoxelPreloadPeak(uint64_t geometryBytes, uint64_t tailBytes,
    uint64_t sourceBytes, uint64_t largestBytes, uint32_t peakPercent, bool strict);
NRIVoxelPreloadBudgetSelection SelectNRIVoxelPreloadBudgetGroups(
    const std::vector<NRIVoxelPreloadBudgetGroup>& groups, const NRIVoxelPreloadBudgetLimits& limits);
