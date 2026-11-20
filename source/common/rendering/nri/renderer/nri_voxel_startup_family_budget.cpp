#include "nri_voxel_startup_family_budget.h"
#include "nri_enemy_family_staging.h"
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
    for (const auto& pose : variants)
    {
        if (!pose.preloadGeometry || !(pose.sourceBits & nri_scene::PrecachedVoxelSourceBit_EnemyFamily)) continue;
        const auto key = BuildPersistentVoxelVariantMeshResourceKey(pose);
        if (!plan.meshes.insert(key).second) continue;
        // This plan is an optional credit. Unknown or excessive manifests get ordinary preflight and staging.
        if (!key || !pose.primitiveCount || pose.primitiveCount > UINT32_MAX / 3 ||
            plan.meshes.size() > NRIEnemyFamilyStaging::MaximumPreparedFamilies * NRIEnemyFamilyStaging::MaximumFamilyPoses)
            return {};
        LoadingWindow::Scope::ServiceIfActive();
        auto geometry = BuildNRIVoxelAllocationGeometry(2 * pose.primitiveCount, 3 * pose.primitiveCount);
        nri::AccelerationStructureDesc desc = {};
        desc.type = nri::AccelerationStructureType::BOTTOM_LEVEL;
        desc.flags = GetNRIPersistentVoxelAllocationBlasFlags();
        desc.geometryOrInstanceNum = 1;
        desc.geometries = &geometry;
        nri::AccelerationStructure* metadata = nullptr;
        if (rt.CreateAccelerationStructure(device, desc, metadata) != nri::Result::SUCCESS) return {};
        nri::MemoryDesc memory = {};
        rt.GetAccelerationStructureMemoryDesc(*metadata, nri::MemoryLocation::DEVICE, memory);
        const uint64_t scratch = rt.GetAccelerationStructureBuildScratchBufferSize(*metadata);
        rt.DestroyAccelerationStructure(metadata);
        if (!memory.size || !scratch || !AccumulateNRIVoxelStartupHeapCharge(memory.size,
            std::max(heapAlignment, memory.alignment), plan.blasBytes)) return {};
        const uint64_t geometryBytes = uint64_t(pose.primitiveCount) *
            (2 * sizeof(nri_scene::SceneVertex) + 3 * sizeof(uint32_t) + sizeof(nri_scene::PrimitiveData));
        plan.geometryBytes += geometryBytes;
        // Reserve one complete material binding per distinct placed pose. At runtime each live owner is priced separately.
        plan.materialBytes += uint64_t(nri_scene::VoxelPaletteMaterialSlotCount) * sizeof(nri_scene::MaterialData);
        plan.largestScratchBytes = std::max(plan.largestScratchBytes, scratch);
    }
    if (plan.geometryBytes > UINT64_MAX - plan.blasBytes || plan.materialBytes > UINT64_MAX - plan.blasBytes ||
        plan.largestScratchBytes > UINT64_MAX - plan.blasBytes - plan.materialBytes) return {};
    return plan;
}
