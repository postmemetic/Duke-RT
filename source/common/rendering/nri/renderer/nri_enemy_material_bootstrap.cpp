#include "nri_enemy_material_bootstrap_policy.h"
#include "nri_renderer.h"
#include "nri_cvars.h"
#include "nri_persistent_voxel_allocation_policy.h"
#include "../scene/nri_actor_sprite_fallback.h"
#include "../scene/nri_enemy_material_key.h"
#include "../scene/nri_voxel_material_slots.h"
#include "../system/nri_renderdevice.h"
#include "actor_presentation_snapshot.h"
#include "perf_audit.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <unordered_set>

EXTERN_CVAR(Bool, nri_ptenemystablerepresentation)

namespace
{
// The ordinary material uploader backs the allocator cursor when it has a
// dirty active binding. Only a visible owner with that exact live range may
// advance this required allocation to before its first representation choice.
uint64_t CurrentMaterialRowEnd(const NRIPersistentVoxelResidency& voxels, uint64_t world, uint64_t lifetime, uint64_t identity)
{
    const auto slot = voxels.batch.actorIndex.Find(identity);
    if (!voxels.batch.valid || slot >= voxels.batch.actors.size()) return 0;
    const auto& actor = voxels.batch.actors[slot];
    if (!actor.active || actor.identityKey != identity || actor.ownerWorldEpoch != world ||
        actor.ownerLifetimeGeneration != lifetime || !actor.materialCount) return 0;
    const auto found = voxels.materialVariantResources.find(actor.materialKeyHash);
    if (found == voxels.materialVariantResources.end()) return 0;
    const auto& material = found->second;
    if (!material.enemyStableMaterialBinding || material.enemyMaterialReservationOnly ||
        material.materialOffset != actor.materialOffset || material.materialSlotGeneration != actor.materialSlotGeneration ||
        !voxels.materialRangeAllocator.Owns({material.materialOffset, material.materialCapacity, material.materialSlotGeneration})) return 0;
    if (voxels.materialBuffer.buffer && voxels.uploadedMaterialResourceGeneration == voxels.materialResourceGeneration &&
        voxels.uploadedMaterialPublicationGeneration == voxels.batchMaterialPublicationGeneration &&
        !voxels.dirtyMaterialResourceKeys.count(actor.materialKeyHash)) return 0;
    return uint64_t(material.materialOffset) + material.materialCapacity;
}
}

void NRIRenderer::PrepareEnemyMaterialBacking(const NRIPersistentVoxelSettings& settings)
{
    if (!(bool)nri_ptactorspritefallback || !(bool)nri_ptenemystablerepresentation ||
        !mFrameBuffer || !mFrameBuffer->mDevice || !mFrameBuffer->HasCurrentCommandBuffer() ||
        !mFrameBuffer->GetRecordingCommandFenceValue() || mResidentUploadScratchFrames.empty()) return;
    auto& voxels = mPersistentVoxels;
    if (voxels.retentionQuarantined || voxels.enemyMaterialTexturesReadyFrame != mFrameIndex ||
        voxels.enemyMaterialTexturesReadyActors.empty()) return;

    const auto begin = std::chrono::steady_clock::now();
    constexpr uint32_t stride = sizeof(nri_scene::MaterialData);
    NRIEnemyMaterialBootstrapPlan plan;
    plan.cursorRows = voxels.MaterialRangeStats().cursorRows;
    std::unordered_set<uint64_t> owners;
    uint64_t currentRowEnd = 0;
    uint32_t refused = 0, shortBindings = 0, invalidBindings = 0, capRefusals = 0;
    if (voxels.enemyPreparesProspectiveMaterials)
    {
        plan.ownerLimit = NRIEnemyFamilyStaging::MaximumOwners;
        plan.extraByteLimit = std::max(plan.extraByteLimit, voxels.enemyPreparationHeadroom / 4);
    }
    const auto addOwner = [&](uint64_t world, uint64_t lifetime, uint64_t identity)
    {
        if (!voxels.enemyMaterialTexturesReadyActors.count(identity) || !owners.insert(identity).second ||
            !voxels.CanPrepareEnemyVoxel({world, lifetime})) return;
        const auto* authority = nri_scene::LookupActorSpriteFallbackAuthority(world, lifetime);
        if (!authority || !authority->authorityCurrent || !authority->publicationEligible ||
            !authority->spriteAnimation.enemy || !authority->spriteAnimation.recipeCertified ||
            authority->spriteAnimation.recipeHidden || !authority->spriteAnimation.enemyVoxelFamily ||
            !voxels.InspectEnemyFamily(*authority, mFrameIndex, settings).complete) return;
        if (plan.owners >= plan.ownerLimit)
        { ++refused; ++capRefusals; return; }
        uint32_t missing = 0;
        bool valid = true;
        for (int32_t texture : authority->spriteAnimation.enemyVoxelFamily->textureIds)
        {
            const uint64_t key = nri_scene::BuildEnemyStableMaterialKey(authority->owner.ownerWorldEpoch,
                uint64_t(authority->owner.ownerLifetimeGeneration), texture);
            const auto found = voxels.materialVariantResources.find(key);
            if (!key) { valid = false; ++invalidBindings; break; }
            if (found == voxels.materialVariantResources.end()) { ++missing; continue; }
            const auto& material = found->second;
            if (!material.enemyStableMaterialBinding ||
                !voxels.materialRangeAllocator.Owns({material.materialOffset, material.materialCapacity, material.materialSlotGeneration}))
            { valid = false; ++invalidBindings; break; }
            if (material.materialCapacity < nri_scene::VoxelPaletteMaterialSlotCount)
            { valid = false; ++shortBindings; break; }
        }
        if (!valid) ++refused;
        else if (!plan.AddFamily(missing, nri_scene::VoxelPaletteMaterialSlotCount, stride))
        { ++refused; ++capRefusals; }
    };
    for (const auto& candidate : nri_scene::GetActorSpriteFallbackCandidates())
    {
        if (candidate.descriptorCurrent && candidate.capturedThisFrame && !candidate.primaryHidden &&
            !candidate.presentationHidden && candidate.recipeCertified && !candidate.transientGeometryCaptured)
        {
            const auto previousOwners = plan.owners;
            addOwner(candidate.ownerWorldEpoch, candidate.ownerLifetimeGeneration, candidate.identityKey);
            if (plan.owners != previousOwners)
                currentRowEnd = std::max(currentRowEnd, CurrentMaterialRowEnd(voxels, candidate.ownerWorldEpoch,
                    candidate.ownerLifetimeGeneration, candidate.identityKey));
        }
    }
    for (const auto& owner : voxels.prospectiveMaterialOwners)
        addOwner(owner.world, owner.lifetime,
            nri_scene::BuildVoxelActorIdentityKey({owner.world, int64_t(owner.lifetime)}));

    auto& resource = voxels.materialBuffer;
    const uint64_t requiredBytes = plan.RequiredBytes(stride);
    const uint64_t oldBytes = resource.memorySize;
    const uint64_t currentBytes = currentRowEnd > resource.size / stride && currentRowEnd <= plan.cursorRows ?
        plan.cursorRows * stride : 0;
    const char* speculativeRefusal = "none";
    uint64_t predictedBytes = 0, allocationBytes = 0, usageBytes = 0, budgetBytes = 0;
    const uint64_t reserve = uint64_t(std::max(0, (int)nri_ptvoxelallocationreservemb)) * 1024 * 1024;
    const auto report = [&](const char* decision)
    {
        if (!PerfAuditActive()) return;
        char fields[1024];
        std::snprintf(fields, sizeof(fields), "\"renderer_frame\":%u,\"decision\":\"%s\",\"owners\":%u,"
            "\"refused_owners\":%u,\"cursor_rows\":%llu,\"extra_rows\":%llu,\"required_bytes\":%llu,"
            "\"old_backing_bytes\":%llu,\"predicted_allocation_bytes\":%llu,\"actual_allocation_bytes\":%llu,"
            "\"backing_bytes\":%llu,\"usage_bytes\":%llu,\"budget_bytes\":%llu,\"reserve_bytes\":%llu,"
            "\"prospective_owners\":%zu,\"short_bindings\":%u,\"invalid_bindings\":%u,\"cap_refusals\":%u,"
            "\"current_required_bytes\":%llu,\"speculative_refusal\":\"%s\",\"cpu_ms\":%.6f",
            mFrameIndex, decision, plan.owners, refused, (unsigned long long)plan.cursorRows,
            (unsigned long long)plan.extraRows, (unsigned long long)requiredBytes, (unsigned long long)oldBytes,
            (unsigned long long)predictedBytes, (unsigned long long)allocationBytes,
            (unsigned long long)resource.memorySize, (unsigned long long)usageBytes,
            (unsigned long long)budgetBytes, (unsigned long long)reserve, voxels.prospectiveMaterialOwners.size(), shortBindings, invalidBindings, capRefusals,
            (unsigned long long)currentBytes, speculativeRefusal,
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count());
        PerfAuditWriteEvent("enemy_material_bootstrap", PerfAuditCurrentToken(), fields);
    };
    if (!plan.owners) { report("no-eligible-binding"); return; }
    if (!requiredBytes) { report("invalid-size"); return; }
    if (resource.buffer && resource.shaderView && resource.memoryLocation == nri::MemoryLocation::DEVICE &&
        resource.stride == stride && NRIResourceUsageIncludes(resource.usage, nri::BufferUsageBits::SHADER_RESOURCE) &&
        resource.size >= requiredBytes) { report("existing-backing"); return; }

    const auto allocate = [&](uint64_t bytes, const char* decision)
    {
        if (!EnsureResidentArenaBuffer(resource, bytes, stride, nri::BufferUsageBits::SHADER_RESOURCE,
            NRIResourceComputeShaderResourceAccess())) { report("allocation-failed"); return; }
        allocationBytes = resource.memorySize;
        report(decision);
    };
    if (currentBytes && GetNRIGrownBufferSize(resource.size, currentBytes, stride) >= requiredBytes)
    {
        allocate(currentBytes, "allocated-current");
        return;
    }
    const auto refuseExtra = [&](const char* reason)
    {
        if (!currentBytes) { report(reason); return; }
        speculativeRefusal = reason;
        allocate(currentBytes, "allocated-current");
    };
    auto& core = mFrameBuffer->mCore;
    const auto& deviceDesc = core.GetDeviceDesc(*mFrameBuffer->mDevice);
    if (!core.GetBufferMemoryDesc2 || !deviceDesc.features.getMemoryDesc2)
    { refuseExtra("requirements-unavailable"); return; }
    nri::BufferDesc desc = {};
    desc.size = GetNRIGrownBufferSize(resource.size, requiredBytes, stride);
    desc.structureStride = stride;
    desc.usage = nri::BufferUsageBits::SHADER_RESOURCE;
    if (deviceDesc.memory.bufferMaxSize && desc.size > deviceDesc.memory.bufferMaxSize)
    { refuseExtra("size-limit"); return; }
    nri::MemoryDesc requirements = {};
    core.GetBufferMemoryDesc2(*mFrameBuffer->mDevice, desc, nri::MemoryLocation::DEVICE, requirements);
    predictedBytes = requirements.size;
    if (predictedBytes < desc.size || (deviceDesc.memory.allocationMaxSize &&
        predictedBytes > deviceDesc.memory.allocationMaxSize)) { refuseExtra("requirements-invalid"); return; }
    const auto budget = GetPersistentVoxelBudgetSnapshot(true);
    usageBytes = budget.usageBytes;
    budgetBytes = budget.budgetBytes;
    // Fresh usage includes old backing, deferred retirements and compute
    // promises. Charge the full new physical allocation without crediting any
    // old bytes; the ordinary arena owner preserves copies and fence lifetime.
    if (!BuildNRIPersistentVoxelAllocationBudget(usageBytes, budgetBytes, predictedBytes, reserve).fits)
    { refuseExtra("budget-denied"); return; }
    // Only backing exists now. The normal combined material pass resolves
    // texture slots/policy and records current row uploads before any TLAS
    // consumer. PrepareEnemyMaterialCommit still reserves actual capacity.
    allocate(requiredBytes, "allocated");
}
