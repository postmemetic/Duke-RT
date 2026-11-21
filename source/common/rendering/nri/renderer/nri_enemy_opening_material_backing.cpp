#include "nri_renderer.h"
#include "nri_cvars.h"
#include "nri_enemy_material_bootstrap_policy.h"
#include "nri_persistent_voxel_allocation_policy.h"
#include "nri_persistent_voxel_geometry_arena_policy.h"
#include "../scene/nri_enemy_material_key.h"
#include "../scene/nri_voxel_material_slots.h"
#include "../system/nri_renderdevice.h"
#include "actor_presentation_snapshot.h"
#include "perf_audit.h"

#include <algorithm>
#include <cstdio>

CVAR(Bool, nri_ptvoxelopeningmaterials, false, 0)
EXTERN_CVAR(Bool, nri_ptenemystablerepresentation)

void NRIRenderer::PreloadOpeningEnemyMaterialBacking()
{
    auto& voxels = mPersistentVoxels;
    const uint64_t build = mMapWorld.buildSerial;
    if (!(bool)nri_ptvoxelopeningmaterials || !(bool)nri_ptenemystablerepresentation || !(bool)nri_ptactorspritefallback ||
        !build || mOpeningEnemyMaterialBuild == build || voxels.retentionQuarantined ||
        !mFrameBuffer || !mFrameBuffer->mDevice || voxels.materialBuffer.buffer) return;
    mOpeningEnemyMaterialBuild = build;
    constexpr uint32_t stride = sizeof(nri_scene::MaterialData);
    constexpr uint32_t slots = nri_scene::VoxelPaletteMaterialSlotCount;
    const auto settings = BuildNRIPersistentVoxelSettingsFromCVars();
    if (settings.transformKeyed) return;

    NRIEnemyMaterialBootstrapPlan plan;
    plan.cursorRows = voxels.MaterialRangeStats().cursorRows;
    uint32_t currentPoseOwners = 0;
    const auto candidates = nri_scene::CollectOpeningEnemyMaterialCandidates(currentPoseOwners);
    for (const auto& owner : candidates)
    {
        if (plan.owners == plan.ownerLimit) break;
        if (!voxels.InspectEnemyFamily(owner, mFrameIndex, settings).complete) continue;
        const auto& family = *owner.spriteAnimation.enemyVoxelFamily;
        uint32_t missing = 0;
        bool valid = true;
        for (int32_t texture : family.textureIds)
        {
            const auto key = nri_scene::BuildEnemyStableMaterialKey(owner.owner.ownerWorldEpoch,
                uint64_t(owner.owner.ownerLifetimeGeneration), texture);
            const auto existing = voxels.materialVariantResources.find(key);
            if (!key || (existing != voxels.materialVariantResources.end() && !existing->second.enemyStableMaterialBinding))
            { valid = false; break; }
            if (!voxels.materialPayloads.FindBinding(key)) ++missing;
            else if (!voxels.materialPayloads.CanReusePrivateBinding(key, owner.actorIndex, slots,
                UINT32_MAX, voxels.materialRangeAllocator))
            { valid = false; break; }
        }
        if (!valid || !plan.AddFamily(missing, slots, stride)) continue;
        if (PerfAuditActive())
        {
            char fields[320];
            std::snprintf(fields, sizeof(fields), "\"build\":%llu,\"world\":%llu,\"lifetime\":%lld,\"family\":%u,"
                "\"poses\":%zu,\"missing\":%u", (unsigned long long)build, (unsigned long long)owner.owner.ownerWorldEpoch,
                (long long)owner.owner.ownerLifetimeGeneration, family.id, family.textureIds.size(), missing);
            PerfAuditWriteEvent("enemy_opening_material_owner", PerfAuditCurrentToken(), fields);
        }
    }
    // Only backing is prefunded. Runtime still proves actual owner, lifetime, mesh, material and current visibility.
    // The first capture may allocate current-pose bindings for other placed
    // voxels before it reserves a certified family. Bound each by one palette;
    // this is physical space only, with no speculative texture/key ownership.
    const uint64_t currentPoseRows = uint64_t(currentPoseOwners) * slots;
    const uint64_t required = plan.RequiredBytes(stride, currentPoseRows);
    const auto before = GetPersistentVoxelBudgetSnapshot(true);
    const uint64_t reserve = uint64_t(std::max(0, (int)nri_ptvoxelallocationreservemb)) * 1024 * 1024;
    NRIPersistentVoxelBudgetSnapshot after = {};
    uint64_t predicted = 0, heapCharge = 0, actual = 0;
    const auto report = [&](const char* reason)
    {
        if (!PerfAuditActive()) return;
        char fields[768];
        std::snprintf(fields, sizeof(fields), "\"build\":%llu,\"decision\":\"%s\",\"candidates\":%zu,\"owners\":%u,"
            "\"current_pose_owners\":%u,\"current_pose_rows\":%llu,\"cursor_rows\":%llu,\"extra_rows\":%llu,\"requested_bytes\":%llu,\"predicted_bytes\":%llu,\"heap_charge_bytes\":%llu,\"actual_bytes\":%llu,"
            "\"usage\":%llu,\"budget\":%llu,\"reserve\":%llu,\"post_usage\":%llu,\"post_budget\":%llu",
            (unsigned long long)build, reason, candidates.size(), plan.owners, currentPoseOwners, (unsigned long long)currentPoseRows,
            (unsigned long long)plan.cursorRows,
            (unsigned long long)plan.extraRows, (unsigned long long)required, (unsigned long long)predicted,
            (unsigned long long)heapCharge, (unsigned long long)actual, (unsigned long long)before.usageBytes, (unsigned long long)before.budgetBytes,
            (unsigned long long)reserve, (unsigned long long)after.usageBytes, (unsigned long long)after.budgetBytes);
        PerfAuditWriteEvent("enemy_opening_material_backing", PerfAuditCurrentToken(), fields);
    };
    if (!plan.extraRows) { report("no-opening-demand"); return; }
    if (!required) { report("capacity-limit"); return; }
    auto& core = mFrameBuffer->mCore;
    const auto& device = core.GetDeviceDesc(*mFrameBuffer->mDevice);
    if (!before.liveAvailable || !core.GetBufferMemoryDesc2 || !device.features.getMemoryDesc2 ||
        (device.memory.bufferMaxSize && required > device.memory.bufferMaxSize)) { report("requirements-unavailable"); return; }
    nri::BufferDesc desc = {};
    desc.size = required;
    desc.structureStride = stride;
    desc.usage = nri::BufferUsageBits::SHADER_RESOURCE;
    nri::MemoryDesc requirements = {};
    core.GetBufferMemoryDesc2(*mFrameBuffer->mDevice, desc, nri::MemoryLocation::DEVICE, requirements);
    predicted = requirements.size;
    // Dedicated heaps may consume a whole placement granule even when the
    // native resource footprint is smaller and the adapter counter is stale.
    if (!AccumulateNRIVoxelStartupHeapCharge(predicted, std::max(device.memory.alignmentDefault, requirements.alignment), heapCharge) ||
        predicted < required || (device.memory.allocationMaxSize && predicted > device.memory.allocationMaxSize) ||
        !BuildNRIPersistentVoxelAllocationBudget(before.usageBytes, before.budgetBytes, heapCharge, reserve).fits)
    { report("budget-denied"); return; }
    NRIBufferResource staging = {};
    if (!EnsureResidentArenaBuffer(staging, required, stride, desc.usage, NRIResourceComputeShaderResourceAccess()))
    { report("allocation-failed"); return; }
    actual = staging.memorySize;
    after = GetPersistentVoxelBudgetSnapshot(true);
    if (actual != predicted || !after.liveAvailable ||
        !BuildNRIPersistentVoxelAllocationBudget(before.usageBytes, after.budgetBytes, heapCharge, reserve).fits ||
        !BuildNRIPersistentVoxelAllocationBudget(after.usageBytes, after.budgetBytes, 0, reserve).fits)
    {
        DestroyBufferResource(staging);
        report("budget-changed");
        return;
    }
    staging.usedSize = 0;
    voxels.materialBuffer = staging;
    voxels.MarkMaintenanceMutation();
    report("funded");
}
