#pragma once

#include "nri_resources.h"
#include <unordered_map>
#include <vector>

namespace nri_scene { struct PrecachedVoxelVariantView; }

// Only a successfully backed, map-specific plan may extend prospective pins.
struct NRIVoxelStartupFamilyBudget
{
    struct PoseCost { uint64_t geometry, blas, scratch; };
    uint64_t build = 0, geometryBytes = 0, blasBytes = 0, largestScratchBytes = 0, materialBytes = 0;
    nri::AccelerationStructureBits blasFlags = {};
    std::unordered_map<uint64_t, PoseCost> meshes;
    uint64_t StageCharge() const { return geometryBytes + blasBytes; }
    uint64_t FutureBytes() const { return blasBytes + largestScratchBytes + materialBytes; }
};

NRIVoxelStartupFamilyBudget PriceNRIVoxelStartupFamilies(nri::Device& device, const nri::RayTracingInterface& rt,
    uint32_t heapAlignment, const std::vector<nri_scene::PrecachedVoxelVariantView>& variants);
