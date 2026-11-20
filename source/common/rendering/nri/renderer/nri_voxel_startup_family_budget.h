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
    // Spawn-only costs can validate later recovery. They confer no loading budget or staging credit.
    std::unordered_map<uint64_t, PoseCost> spawnMeshes;
    const PoseCost* RecoveryCost(uint64_t key) const
    {
        const auto placed = meshes.find(key);
        if (placed != meshes.end()) return &placed->second;
        const auto spawn = spawnMeshes.find(key);
        return spawn == spawnMeshes.end() ? nullptr : &spawn->second;
    }
    uint64_t StageCharge() const { return geometryBytes + blasBytes; }
    uint64_t FutureBytes() const { return blasBytes + largestScratchBytes + materialBytes; }
};

NRIVoxelStartupFamilyBudget PriceNRIVoxelStartupFamilies(nri::Device& device, const nri::RayTracingInterface& rt,
    uint32_t heapAlignment, const std::vector<nri_scene::PrecachedVoxelVariantView>& variants);
