#pragma once

#include "nri_resources.h"
#include <unordered_set>
#include <vector>

namespace nri_scene { struct PrecachedVoxelVariantView; }

// Only a successfully backed, map-specific plan may extend prospective pins.
struct NRIVoxelStartupFamilyBudget
{
    uint64_t build = 0, geometryBytes = 0, blasBytes = 0, largestScratchBytes = 0, materialBytes = 0;
    std::unordered_set<uint64_t> meshes;
    uint64_t StageCharge() const { return geometryBytes + blasBytes; }
    uint64_t FutureBytes() const { return blasBytes + largestScratchBytes + materialBytes; }
};

NRIVoxelStartupFamilyBudget PriceNRIVoxelStartupFamilies(nri::Device& device, const nri::RayTracingInterface& rt,
    uint32_t heapAlignment, const std::vector<nri_scene::PrecachedVoxelVariantView>& variants);
