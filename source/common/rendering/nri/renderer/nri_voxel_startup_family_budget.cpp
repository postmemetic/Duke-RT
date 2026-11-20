#include "nri_voxel_startup_family_budget.h"
#include "nri_enemy_family_staging.h"
#include "nri_cvars.h"
#include "nri_persistent_voxel_geometry_arena_policy.h"
#include "nri_persistent_voxel_allocation.h"
#include "nri_persistent_voxels.h"
#include "../scene/nri_voxel_material_slots.h"
#include "../../../platform/loading_window.h"
#include <algorithm>

NRIVoxelStartupFamilyBudget PriceNRIVoxelStartupFamilies(nri::Device& device, const nri::RayTracingInterface& rt,
    uint32_t heapAlignment, const std::vector<nri_scene::PrecachedVoxelVariantView>& variants)
{
    NRIVoxelStartupFamilyBudget plan;
    plan.blasFlags = GetNRIPersistentVoxelAllocationBlasFlags();
    const auto measure = [&](const nri_scene::PrecachedVoxelVariantView& pose,
        NRIVoxelStartupFamilyBudget::PoseCost& cost, uint64_t& scratch)
    {
        if (!pose.primitiveCount || pose.primitiveCount > UINT32_MAX / 3) return false;
        LoadingWindow::Scope::ServiceIfActive();
        auto geometry = BuildNRIVoxelAllocationGeometry(2 * pose.primitiveCount, 3 * pose.primitiveCount);
        nri::AccelerationStructureDesc desc = {};
        desc.type = nri::AccelerationStructureType::BOTTOM_LEVEL;
        desc.flags = plan.blasFlags;
        desc.geometryOrInstanceNum = 1;
        desc.geometries = &geometry;
        nri::AccelerationStructure* metadata = nullptr;
        if (rt.CreateAccelerationStructure(device, desc, metadata) != nri::Result::SUCCESS) return false;
        nri::MemoryDesc memory = {};
        rt.GetAccelerationStructureMemoryDesc(*metadata, nri::MemoryLocation::DEVICE, memory);
        scratch = rt.GetAccelerationStructureBuildScratchBufferSize(*metadata);
        rt.DestroyAccelerationStructure(metadata);
        if (!memory.size || !scratch || !AccumulateNRIVoxelStartupHeapCharge(memory.size,
            std::max(heapAlignment, memory.alignment), cost.blas) ||
            !AccumulateNRIVoxelStartupHeapCharge(scratch, heapAlignment, cost.scratch)) return false;
        cost.geometry = uint64_t(pose.primitiveCount) *
            (2 * sizeof(nri_scene::SceneVertex) + 3 * sizeof(uint32_t) + sizeof(nri_scene::PrimitiveData));
        return true;
    };
    constexpr auto maxPrices = NRIEnemyFamilyStaging::MaximumPreparedFamilies * NRIEnemyFamilyStaging::MaximumFamilyPoses;
    for (const auto& pose : variants)
    {
        if (!pose.preloadGeometry || !(pose.sourceBits & nri_scene::PrecachedVoxelSourceBit_EnemyFamily)) continue;
        const auto key = BuildPersistentVoxelVariantMeshResourceKey(pose);
        if (plan.meshes.count(key)) continue;
        NRIVoxelStartupFamilyBudget::PoseCost cost = {};
        uint64_t scratch = 0;
        if (!key || plan.meshes.size() >= maxPrices || !measure(pose, cost, scratch) ||
            cost.blas > UINT64_MAX - plan.blasBytes) return {};
        plan.meshes.emplace(key, cost);
        plan.blasBytes += cost.blas;
        plan.geometryBytes += cost.geometry;
        // Reserve one complete material binding per distinct placed pose. At runtime each live owner is priced separately.
        plan.materialBytes += uint64_t(nri_scene::VoxelPaletteMaterialSlotCount) * sizeof(nri_scene::MaterialData);
        plan.largestScratchBytes = std::max(plan.largestScratchBytes, scratch);
    }
    if (plan.geometryBytes > UINT64_MAX - plan.blasBytes || plan.materialBytes > UINT64_MAX - plan.blasBytes ||
        plan.largestScratchBytes > UINT64_MAX - plan.blasBytes - plan.materialBytes) return {};
    if ((bool)nri_ptvoxellatefamilyrecovery && (bool)nri_ptvoxelfamilyrecovery)
        for (const auto& pose : variants)
        {
            if (!pose.preloadGeometry || !(pose.sourceBits & nri_scene::PrecachedVoxelSourceBit_SpawnableEnemyFamily)) continue;
            const auto key = BuildPersistentVoxelVariantMeshResourceKey(pose);
            if (!key || plan.RecoveryCost(key) || plan.spawnMeshes.size() >= maxPrices) continue;
            NRIVoxelStartupFamilyBudget::PoseCost cost = {};
            uint64_t scratch = 0;
            if (measure(pose, cost, scratch)) plan.spawnMeshes.emplace(key, cost);
        }
    return plan;
}
