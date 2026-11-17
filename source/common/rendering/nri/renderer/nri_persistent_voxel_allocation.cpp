#include "nri_persistent_voxel_allocation.h"
#include "nri_persistent_voxel_allocation_policy.h"
#include "nri_voxel_compute_allocation.h"
#include "nri_renderer.h"
#include "nri_cvars.h"
#include "../system/nri_renderdevice.h"
#include "../system/nri_gpu_timing.h"
#include "perf_audit.h"
#include "nri_perf_audit.h"
#include "gameupdate.h"
#include "../scene/nri_hash.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <limits>

uint64_t GetNRIVoxelAdmissionRetryClock()
{
    return GetGameUpdateSnapshot().presentationGeneration;
}

bool CanContinueNRIVoxelAllocationWait(const PersistentVoxelAdmissionEntry& entry)
{
    const auto& mesh = entry.uploadMeshResource;
    const auto ownsBuffer = [](const NRIBufferResource& buffer)
    { return buffer.buffer || buffer.shaderView || buffer.storageView || buffer.memorySize; };
    const auto ownsAs = [](const NRIAccelerationStructureResource& as)
    { return as.accelerationStructure || as.descriptor || as.memorySize || as.buildVertexBuffer || as.buildIndexBuffer; };
    const auto ownsRange = [](const NRIPersistentVoxelMaterialRangeHandle& range)
    { return range.offset || range.capacity || range.generation; };
    const bool hasOwnedWork = entry.schedulerTokenId || entry.directComputeRequested ||
        entry.directComputeGeneration || entry.directComputeJobId || entry.computeGeometryJobId ||
        entry.directBlasFenceValue || entry.directBlasRecordedFrame != UINT32_MAX ||
        entry.uploadPrepared || entry.uploadSubmittedBeforeBlas || entry.bytesUploaded ||
        entry.vertexBytesUploaded || entry.vertexArenaBytesUploaded || entry.indexBytesUploaded ||
        entry.indexArenaBytesUploaded || entry.primitiveBytesUploaded ||
        ownsBuffer(entry.directBlasScratchBuffer) || ownsBuffer(mesh.vertexBuffer) || ownsBuffer(mesh.indexBuffer) ||
        ownsAs(mesh.accelerationStructure) || ownsBuffer(mesh.shadowProxy.vertexBuffer) ||
        ownsBuffer(mesh.shadowProxy.indexBuffer) || ownsAs(mesh.shadowProxy.accelerationStructure) ||
        mesh.computeProviderReservation.Total() || mesh.geometryRange.transaction ||
        ownsRange(mesh.geometryRange.vertices) || ownsRange(mesh.geometryRange.indices) || ownsRange(mesh.geometryRange.primitives);
    return entry.state == PersistentVoxelAdmissionState::Deferred &&
        NRIPersistentVoxelAllocationWaitCanContinue(entry.lastReason, hasOwnedWork);
}

namespace
{
    nri::BottomLevelGeometryDesc Geometry(uint32_t vertices, uint32_t indices)
    {
        nri::BottomLevelGeometryDesc geometry = {};
        geometry.flags = nri::BottomLevelGeometryBits::OPAQUE_GEOMETRY;
        geometry.type = nri::BottomLevelGeometryType::TRIANGLES;
        geometry.triangles.vertexBuffer = nri::HAS_BUFFER;
        geometry.triangles.vertexNum = vertices;
        geometry.triangles.vertexStride = sizeof(nri_scene::SceneVertex);
        geometry.triangles.vertexFormat = nri::Format::RGB32_SFLOAT;
        geometry.triangles.indexBuffer = nri::HAS_BUFFER;
        geometry.triangles.indexNum = indices;
        geometry.triangles.indexType = nri::IndexType::UINT32;
        return geometry;
    }

    bool AddBytes(uint64_t& total, uint64_t bytes)
    {
        if (bytes > std::numeric_limits<uint64_t>::max() - total) return false;
        total += bytes;
        return true;
    }

    bool Compatible(const NRIBufferResource& resource, const nri::BufferDesc& desc, bool views)
    {
        return resource.buffer && (!views || resource.shaderView) &&
            (!NRIResourceUsageIncludes(desc.usage, nri::BufferUsageBits::SHADER_RESOURCE_STORAGE) || resource.storageView) &&
            resource.memoryLocation == nri::MemoryLocation::DEVICE && resource.stride == desc.structureStride &&
            NRIResourceUsageIncludes(resource.usage, desc.usage) && resource.size >= desc.size;
    }
}

nri::AccelerationStructureBits GetNRIPersistentVoxelAllocationBlasFlags()
{
    auto flags = nri::AccelerationStructureBits::PREFER_FAST_BUILD;
    switch (std::clamp((int)nri_ptvoxelblaspolicy, 0, 3))
    {
    case 1: flags = nri::AccelerationStructureBits::PREFER_FAST_TRACE; break;
    case 2: flags = nri::AccelerationStructureBits::MINIMIZE_MEMORY; break;
    case 3: flags = NRIResourceFlags(nri::AccelerationStructureBits::PREFER_FAST_TRACE,
        nri::AccelerationStructureBits::MINIMIZE_MEMORY); break;
    }
    return (bool)nri_ptvoxelblascompact && (bool)nri_ptvoxelcomputepreloadstrict ?
        NRIResourceFlags(flags, nri::AccelerationStructureBits::ALLOW_COMPACTION) : flags;
}

NRIPersistentVoxelAllocationResult NRIRenderer::PreparePersistentVoxelGeometryAllocation(
    PersistentVoxelMeshVariantResource& mesh, NRIBufferResource* buildScratch,
    uint32_t vertexCount, uint32_t indexCount, uint32_t primitiveCount, bool privateBuildInputs)
{
    using Result = NRIPersistentVoxelAllocationResult;
    if (!(bool)nri_ptvoxelallocationbudget) return Result::Disabled;
    if (!mFrameBuffer || !mFrameBuffer->mDevice || !mFrameBuffer->HasCurrentCommandBuffer() ||
        !mFrameBuffer->GetRecordingCommandFenceValue() || mResidentUploadScratchFrames.empty())
        return Result::DeferredFence;
    if (!vertexCount || !indexCount || !primitiveCount || !mFrameBuffer->mCore.GetBufferMemoryDesc2 ||
        !mFrameBuffer->mCore.GetDeviceDesc(*mFrameBuffer->mDevice).features.getMemoryDesc2)
        return Result::RequirementsUnavailable;

    const auto start = std::chrono::steady_clock::now();
    auto& core = mFrameBuffer->mCore;
    auto& rt = mFrameBuffer->mRayTracing;
    auto& scratch = buildScratch ? *buildScratch : mScratchBuffer;
    auto& arena = mPersistentVoxels;
    const uint64_t retryTick = GetNRIVoxelAdmissionRetryClock();
    arena.allocationMeshRetry.BeginTick(mMapWorld.buildSerial, retryTick);
    const bool reclaimEnabled = (bool)nri_ptvoxeladmissionreclaim;
    const std::array<uint32_t, 3> requestedRows = { vertexCount, indexCount, primitiveCount };
    const bool growsArena = uint64_t(arena.arenaVertexCursor) * sizeof(nri_scene::SceneVertex) > arena.vertexBuffer.size ||
        uint64_t(arena.arenaIndexCursor) * sizeof(uint32_t) > arena.indexBuffer.size ||
        uint64_t(arena.arenaPrimitiveCursor) * sizeof(nri_scene::PrimitiveData) > arena.primitiveBuffer.size;
    if (reclaimEnabled)
    {
        arena.admissionReclaim.BeginTick(arena.residencyMapGeneration, retryTick);
        if (!arena.admissionReclaim.PermitPreflight(growsArena))
        {
            mesh.allocationRetryTick = NRIVoxelComputeSaturatingAdd(retryTick, 1);
            return Result::DeferredCooldown;
        }
    }
    const auto flags = GetNRIPersistentVoxelAllocationBlasFlags();
    const uint64_t reserve = uint64_t(std::max(0, (int)nri_ptvoxelallocationreservemb)) * 1024 * 1024;
    uint64_t predictedBytes = 0, actualBytes = 0, oldArenaBytes = 0, newArenaBytes = 0;
    uint32_t candidatePercent = arena.allocationGrowthRetry.SelectPercent(
        uint32_t(std::clamp((int)nri_ptvoxelarenagrowthpercent, 100, 200)));
    uint64_t postUsageBytes = 0, postBudgetBytes = 0, postRawBudgetBytes = 0;
    uint32_t retryFrames = 0;
    auto budget = GetPersistentVoxelBudgetSnapshot(true);
    NRIPersistentVoxelAllocationBudget peak;
    NRIVoxelComputeProviderForecast provider;
    uint64_t reclaimSignature = 0;
    bool requirementsComplete = false;
    const auto report = [&](const char* decision, Result result)
    {
        if (result == Result::AllocationFailed || (result == Result::DeferredBudget && actualBytes != 0))
            arena.allocationMeshRetry.ObserveDenial(mMapWorld.buildSerial, retryTick, mesh.resourceKey, predictedBytes);
        if (result == Result::DeferredBudget)
            arena.preloadHandoff.ObserveBudgetDenial(mMapWorld.buildSerial);
        if (reclaimEnabled && requirementsComplete && result == Result::DeferredBudget && newArenaBytes &&
            arena.actorDemand.Upgrade(mesh.resourceKey, mFrameIndex).consumers)
        {
            NRIVoxelReclaimRequest request;
            request.mesh = mesh.resourceKey; request.epoch = arena.residencyMapGeneration;
            request.tick = retryTick; request.revision = arena.reclaimGeometryRevision;
            request.rows = requestedRows; request.usage = budget.usageBytes;
            request.budget = budget.budgetBytes; request.reserve = reserve;
            request.nonArenaBytes = NRIVoxelComputeSaturatingAdd(predictedBytes - std::min(predictedBytes, newArenaBytes), provider.bytes);
            request.signature = reclaimSignature; request.avoidedArenaBytes = newArenaBytes;
            // Notice only. The next pump processes it after this caller has
            // rolled back its unpublished lease; it never acts on this cursor.
            if (arena.admissionReclaim.Observe(request))
                arena.ReportAdmissionReclaim("request", "geometry-growth-budget-denied", request, mFrameIndex);
        }
        if (reclaimEnabled && result == Result::Ready)
        {
            const auto found = arena.admissionReclaim.Records().find(mesh.resourceKey);
            if (found != arena.admissionReclaim.Records().end())
            {
                arena.ReportAdmissionReclaim("outcome", newArenaBytes ? "allocated-with-growth" : "allocated-in-existing-arena",
                    found->second.request, mFrameIndex);
                arena.admissionReclaim.Success(mesh.resourceKey);
            }
        }
        if (PerfAuditActive())
        {
            char fields[2304];
            snprintf(fields, sizeof(fields), "\"renderer_frame\":%u,\"map_epoch\":%llu,\"decision\":\"%s\","
                "\"mesh_key\":%llu,\"budget_age_frames\":0,\"live_available\":%s,\"usage_bytes\":%llu,"
                "\"budget_bytes\":%llu,\"reserve_bytes\":%llu,\"combined_allocation_bytes\":%llu,\"geometry_allocation_bytes\":%llu,"
                "\"actual_allocation_bytes\":%llu,\"projected_peak_bytes\":%llu,\"growth_percent\":%u,"
                "\"old_arena_bytes\":%llu,\"new_arena_bytes\":%llu,\"concrete_as_scratch_reservation\":true,\"provider_forecast_bytes\":%llu,"
                "\"post_usage_bytes\":%llu,\"post_budget_bytes\":%llu,\"minimum_after_post_denial\":%s,\"shared_retry_frames\":%u,"
                "\"retry_tick\":%llu,\"retry_clock_unit\":\"presentation\",\"cpu_ms\":%.6f,"
                "\"raw_budget_bytes\":%llu,\"allocation_ceiling_bytes\":%llu,\"allocation_ceiling_remaining_ticks\":%llu,"
                "\"post_raw_budget_bytes\":%llu,\"observed_budget_ceiling_bytes\":%llu,"
                "\"observed_budget_hold_ticks\":%llu,\"observed_budget_contraction_streak\":%u,\"observed_budget_probe_reason\":\"%s\"",
                mFrameIndex, (unsigned long long)mMapWorld.buildSerial, decision,
                (unsigned long long)mesh.resourceKey, budget.liveAvailable ? "true" : "false",
                (unsigned long long)budget.usageBytes, (unsigned long long)budget.budgetBytes,
                (unsigned long long)reserve, (unsigned long long)NRIVoxelComputeSaturatingAdd(predictedBytes, provider.bytes),
                (unsigned long long)predictedBytes, (unsigned long long)actualBytes,
                (unsigned long long)peak.projectedBytes, candidatePercent,
                (unsigned long long)oldArenaBytes, (unsigned long long)newArenaBytes, (unsigned long long)provider.bytes,
                (unsigned long long)postUsageBytes, (unsigned long long)postBudgetBytes,
                arena.allocationGrowthRetry.minimumAfterPostDenial ? "true" : "false",
                retryFrames,
                (unsigned long long)retryTick,
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(),
                (unsigned long long)budget.rawBudgetBytes, (unsigned long long)budget.allocationCeilingBytes,
                (unsigned long long)budget.allocationCeilingRemainingTicks, (unsigned long long)postRawBudgetBytes,
                (unsigned long long)arena.allocationBudgetCeiling.ceilingBytes,
                (unsigned long long)arena.allocationBudgetCeiling.holdTicks,
                arena.allocationBudgetCeiling.contractionStreak, arena.allocationBudgetCeiling.Reason());
            PerfAuditWriteEvent("voxel_allocation_transaction", PerfAuditCurrentToken(), fields);
        }
        return result;
    };
    if (!privateBuildInputs)
    {
        const auto forecast = ForecastNRIVoxelComputeProviderAllocations(BuildResourceContext(), mesh.sourceModel, provider);
        if (forecast != Result::Ready) return report("defer-provider-forecast", forecast);
    }
    // Re-preparation replaces this owner's promise; other pending jobs remain reserved.
    budget.usageBytes = NRIVoxelComputeUsageAfterOwnerCredit(budget.usageBytes,
        budget.computeReservedBytes, mesh.computeProviderReservation.Total());
    auto geometry = Geometry(vertexCount, indexCount);
    nri::AccelerationStructureDesc asDesc = {};
    asDesc.type = nri::AccelerationStructureType::BOTTOM_LEVEL;
    asDesc.flags = flags;
    asDesc.geometryOrInstanceNum = 1;
    asDesc.geometries = &geometry;
    // A prepared AS has capacity metadata but no recorded build inputs yet.
    // Counts may contract after compute; its concrete allocation stays valid.
    const auto& oldAs = mesh.accelerationStructure;
    const bool reuseAs = oldAs.accelerationStructure && oldAs.buildTypeValid &&
        oldAs.buildType == nri::AccelerationStructureType::BOTTOM_LEVEL && oldAs.buildFlags == flags &&
        !oldAs.buildVertexBuffer && !oldAs.buildIndexBuffer &&
        oldAs.buildVertexCount >= vertexCount && oldAs.buildIndexCount >= indexCount;
    uint64_t asBytes = 0, scratchBytes = reuseAs ? oldAs.buildScratchSize : 0;
    if (!reuseAs)
    {
        nri::AccelerationStructure* metadata = nullptr;
        if (rt.CreateAccelerationStructure(*mFrameBuffer->mDevice, asDesc, metadata) != nri::Result::SUCCESS)
            return report("as-requirements-unavailable", Result::RequirementsUnavailable);
        nri::MemoryDesc requirements = {};
        rt.GetAccelerationStructureMemoryDesc(*metadata, nri::MemoryLocation::DEVICE, requirements);
        asBytes = requirements.size;
        scratchBytes = rt.GetAccelerationStructureBuildScratchBufferSize(*metadata);
        rt.DestroyAccelerationStructure(metadata);
        if (!asBytes || !scratchBytes) return report("as-requirements-invalid", Result::RequirementsUnavailable);
    }

    const auto geometryUsage = NRIResourceFlags(NRIResourceFlags(nri::BufferUsageBits::SHADER_RESOURCE,
        nri::BufferUsageBits::SHADER_RESOURCE_STORAGE), nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT);
    const auto privateUsage = NRIResourceFlags(nri::BufferUsageBits::SHADER_RESOURCE,
        nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT);
    const auto primitiveUsage = NRIResourceFlags(nri::BufferUsageBits::SHADER_RESOURCE,
        nri::BufferUsageBits::SHADER_RESOURCE_STORAGE);
    std::array<NRIBufferResource*, 6> targets = { &arena.vertexBuffer, &arena.indexBuffer, &arena.primitiveBuffer,
        privateBuildInputs ? &mesh.vertexBuffer : nullptr, privateBuildInputs ? &mesh.indexBuffer : nullptr, &scratch };
    const std::array<uint64_t, 6> required = {
        uint64_t(arena.arenaVertexCursor) * sizeof(nri_scene::SceneVertex),
        uint64_t(arena.arenaIndexCursor) * sizeof(uint32_t),
        uint64_t(arena.arenaPrimitiveCursor) * sizeof(nri_scene::PrimitiveData),
        uint64_t(vertexCount) * sizeof(nri_scene::SceneVertex), uint64_t(indexCount) * sizeof(uint32_t), scratchBytes };
    const std::array<uint32_t, 6> strides = { sizeof(nri_scene::SceneVertex), sizeof(uint32_t),
        sizeof(nri_scene::PrimitiveData), sizeof(nri_scene::SceneVertex), sizeof(uint32_t), 16 };
    const std::array<nri::BufferUsageBits, 6> usages = { geometryUsage, geometryUsage, primitiveUsage,
        privateUsage, privateUsage, nri::BufferUsageBits::SCRATCH_BUFFER };
    std::array<nri::BufferDesc, 6> descriptors = {};
    std::array<uint64_t, 6> predicted = {};
    std::array<bool, 6> replace = {};
    const auto& limits = core.GetDeviceDesc(*mFrameBuffer->mDevice).memory;
    const auto candidate = [&](uint32_t percent)
    {
        requirementsComplete = false;
        predictedBytes = asBytes;
        oldArenaBytes = newArenaBytes = 0;
        for (size_t i = 0; i != targets.size(); ++i)
        {
            if (!targets[i]) continue;
            auto& desc = descriptors[i];
            desc = {};
            desc.size = std::max<uint64_t>(required[i], strides[i]);
            desc.structureStride = strides[i];
            desc.usage = usages[i];
            replace[i] = !Compatible(*targets[i], desc, i != 5);
            predicted[i] = 0;
            if (!replace[i]) continue;
            if (i < 3) desc.size = GetNRIPersistentVoxelGrowthCapacity(targets[i]->size, desc.size, strides[i], percent);
            if (limits.bufferMaxSize && desc.size > limits.bufferMaxSize) return false;
            nri::MemoryDesc requirements = {};
            core.GetBufferMemoryDesc2(*mFrameBuffer->mDevice, desc, nri::MemoryLocation::DEVICE, requirements);
            if (requirements.size < desc.size || (limits.allocationMaxSize && requirements.size > limits.allocationMaxSize) ||
                !AddBytes(predictedBytes, requirements.size)) return false;
            predicted[i] = requirements.size;
            if (i < 3)
            {
                if (!AddBytes(oldArenaBytes, targets[i]->memorySize) || !AddBytes(newArenaBytes, requirements.size)) return false;
            }
        }
        uint64_t combined = predictedBytes;
        if (!AddBytes(combined, provider.bytes)) return false;
        requirementsComplete = true;
        peak = BuildNRIPersistentVoxelAllocationBudget(budget.usageBytes, budget.budgetBytes, combined, reserve);
        return combined == 0 || peak.fits;
    };
    // Preferred spare capacity is optional. Retry the exact footprint before
    // denying an otherwise affordable actor representation.
    bool candidateFits = candidate(candidatePercent);
    if (!candidateFits)
    {
        candidatePercent = 100;
        candidateFits = candidate(candidatePercent);
    }
    if (reclaimEnabled)
    {
        // Signature includes exact non-arena/private/AS/scratch/provider requirements,
        // independent of material-pair identity. Bound expensive probes per tick;
        // repeated identical denials then retain a longer per-owner retry deadline.
        reclaimSignature = nri_scene::HashCombine64(predictedBytes - std::min(predictedBytes, newArenaBytes), provider.bytes);
        reclaimSignature = nri_scene::HashCombine64(reclaimSignature, budget.budgetBytes);
        reclaimSignature = nri_scene::HashCombine64(reclaimSignature, reserve);
        reclaimSignature = nri_scene::HashCombine64(reclaimSignature, uint64_t(flags));
        reclaimSignature = nri_scene::HashCombine64(reclaimSignature, privateBuildInputs);
        const auto reuseFloor = BuildNRIPersistentVoxelAllocationBudget(budget.usageBytes, budget.budgetBytes,
            NRIVoxelComputeSaturatingAdd(predictedBytes - std::min(predictedBytes, newArenaBytes), provider.bytes), reserve);
        reclaimSignature = nri_scene::HashCombine64(reclaimSignature, reuseFloor.fits);
        if (reclaimEnabled && !candidateFits && arena.admissionReclaim.Suppress(mesh.resourceKey, requestedRows,
            arena.reclaimGeometryRevision, growsArena, reclaimSignature))
        {
            mesh.allocationRetryTick = NRIVoxelComputeSaturatingAdd(retryTick, NRIVoxelAdmissionReclaim::ProbeTicks);
            return Result::DeferredCooldown;
        }
    }
    if (!candidateFits) return report("defer-budget", Result::DeferredBudget);

    // Check after exact requirements so existing allocation-free reuse and a
    // smaller replacement can proceed. Material pairs share the original
    // denial deadline instead of each allocating the same mesh independently.
    retryFrames = arena.allocationMeshRetry.Remaining(mMapWorld.buildSerial,
        retryTick, mesh.resourceKey, predictedBytes);
    if (retryFrames)
    {
        mesh.allocationRetryTick = NRIVoxelComputeSaturatingAdd(retryTick, retryFrames);
        return report("defer-mesh-cooldown", Result::DeferredCooldown);
    }

    std::array<NRIBufferResource, 6> unpublished = {};
    std::array<double, 6> allocationMs = {};
    NRIAccelerationStructureResource unpublishedAs;
    const auto discard = [&]()
    {
        for (auto& resource : unpublished) DestroyBufferResource(resource);
        DestroyAccelerationStructureResource(unpublishedAs);
    };
    for (size_t i = 0; i != targets.size(); ++i)
    {
        if (!replace[i]) continue;
        auto& resource = unpublished[i];
        const auto& desc = descriptors[i];
        const auto allocationStart = std::chrono::steady_clock::now();
        if (!CreateBufferWithoutViewAtLocation(resource, desc.size, desc.structureStride, desc.usage, nri::MemoryLocation::DEVICE))
        { discard(); return report("buffer-allocation-failed", Result::AllocationFailed); }
        if (i != 5)
        {
            nri::BufferViewDesc view = {};
            view.buffer = resource.buffer;
            view.type = nri::BufferView::STRUCTURED_BUFFER;
            view.size = nri::WHOLE_SIZE;
            view.structureStride = desc.structureStride;
            if (core.CreateBufferView(view, resource.shaderView) != nri::Result::SUCCESS)
            { discard(); return report("view-allocation-failed", Result::AllocationFailed); }
            if (NRIResourceUsageIncludes(desc.usage, nri::BufferUsageBits::SHADER_RESOURCE_STORAGE))
            {
                view.type = nri::BufferView::STORAGE_STRUCTURED_BUFFER;
                if (core.CreateBufferView(view, resource.storageView) != nri::Result::SUCCESS)
                { discard(); return report("storage-view-allocation-failed", Result::AllocationFailed); }
            }
        }
        if (resource.memorySize != predicted[i] || !AddBytes(actualBytes, resource.memorySize))
        { discard(); return report("buffer-requirements-changed", Result::AllocationFailed); }
        resource.usedSize = required[i];
        allocationMs[i] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - allocationStart).count();
    }
    if (!reuseAs)
    {
        if (rt.CreateCommittedAccelerationStructure(*mFrameBuffer->mDevice, nri::MemoryLocation::DEVICE,
            0.0f, asDesc, unpublishedAs.accelerationStructure) != nri::Result::SUCCESS)
        { discard(); return report("as-allocation-failed", Result::AllocationFailed); }
        nri::MemoryDesc requirements = {};
        rt.GetAccelerationStructureMemoryDesc(*unpublishedAs.accelerationStructure, nri::MemoryLocation::DEVICE, requirements);
        unpublishedAs.memorySize = requirements.size;
        if (requirements.size != asBytes || !AddBytes(actualBytes, requirements.size))
        { discard(); return report("as-requirements-changed", Result::AllocationFailed); }
        unpublishedAs.buildScratchSize = scratchBytes;
        unpublishedAs.buildVertexCount = vertexCount;
        unpublishedAs.buildIndexCount = indexCount;
        unpublishedAs.buildPrimitiveCount = primitiveCount;
        unpublishedAs.buildFlags = flags;
        unpublishedAs.buildType = nri::AccelerationStructureType::BOTTOM_LEVEL;
        unpublishedAs.buildTypeValid = true;
        unpublishedAs.uncompactedMemorySize = requirements.size;
    }

    // Locals are not yet in renderer telemetry. Include their complete physical
    // footprint even if the driver's accounting lags the committed allocations.
    const auto postBudget = GetPersistentVoxelBudgetSnapshot(true);
    postUsageBytes = postBudget.usageBytes;
    postBudgetBytes = postBudget.budgetBytes;
    postRawBudgetBytes = postBudget.rawBudgetBytes;
    auto floor = BuildNRIPersistentVoxelAllocationBudget(budget.usageBytes, postBudget.budgetBytes, actualBytes, 0);
    const uint64_t postUsage = NRIVoxelComputeUsageAfterOwnerCredit(postBudget.usageBytes,
        postBudget.computeReservedBytes, mesh.computeProviderReservation.Total());
    peak = BuildNRIPersistentVoxelAllocationBudget(std::max(floor.projectedBytes, postUsage),
        postBudget.budgetBytes, provider.bytes, reserve);
    if ((actualBytes != 0 || provider.bytes != 0) && (floor.overflow || !peak.fits))
    {
        // Unpublished resources have no command references. The caller rolls
        // back its provisional range and retains the usual 30-frame cooldown.
        // A later retry must not allocate the same rejected speculative slack.
        arena.allocationGrowthRetry.ObservePostDenial(candidatePercent, oldArenaBytes, newArenaBytes);
        if (actualBytes)
            arena.allocationBudgetCeiling.ObservePostDenial(mMapWorld.buildSerial, retryTick,
                budget.rawBudgetBytes, postBudget.rawBudgetBytes);
        discard();
        return report("defer-post-budget", Result::DeferredBudget);
    }

    NRIScopedGpuTiming admissionTiming(mFrameBuffer, NRIGpuTimingScope::VoxelAdmission);
    arena.allocationBudgetCeiling.ObserveSuccess(mMapWorld.buildSerial, retryTick,
        actualBytes, postBudget.liveAvailable ? postBudget.liveUsageBytes : 0, postBudget.rawBudgetBytes);
    NRIScopedGpuTiming copyTiming(mFrameBuffer, NRIGpuTimingScope::VoxelArenaCopy);
    for (size_t i = 0; i != targets.size(); ++i)
    {
        if (!targets[i]) continue;
        if (!replace[i])
        {
            targets[i]->usedSize = std::max(targets[i]->usedSize, required[i]);
            continue;
        }
        auto& old = *targets[i];
        auto& next = unpublished[i];
        const uint64_t copied = i < 3 && old.buffer ? std::min(old.usedSize, next.size) : 0;
        if (copied)
        {
            nri::BufferBarrierDesc before[2] = {};
            before[0].buffer = old.buffer;
            before[0].before = NRIResourceComputeShaderResourceAccess();
            before[0].after = NRIResourceCopySourceAccess();
            before[1].buffer = next.buffer;
            before[1].after = NRIResourceCopyDestinationAccess();
            nri::BarrierDesc barrier = {};
            barrier.buffers = before;
            barrier.bufferNum = 2;
            core.CmdBarrier(*mFrameBuffer->mCommandBuffer, barrier);
            core.CmdCopyBuffer(*mFrameBuffer->mCommandBuffer, *next.buffer, 0, *old.buffer, 0, copied);
            nri::BufferBarrierDesc after = {};
            after.buffer = next.buffer;
            after.before = NRIResourceCopyDestinationAccess();
            after.after = NRIResourceComputeShaderResourceAccess();
            barrier.buffers = &after;
            barrier.bufferNum = 1;
            core.CmdBarrier(*mFrameBuffer->mCommandBuffer, barrier);
        }
        if (i < 3)
        {
            PerfAuditAdd(PerfAuditMetric::VoxelGeometryArenaGrowths);
            PerfAuditAdd(PerfAuditMetric::VoxelGeometryArenaOldBytes, old.memorySize);
            PerfAuditAdd(PerfAuditMetric::VoxelGeometryArenaNewBytes, next.memorySize);
            PerfAuditAdd(PerfAuditMetric::VoxelArenaCopyBytes, copied);
            NRIAuditResidentArenaGrowth(mFrameIndex, strides[i], old.size, old.memorySize, old.usedSize,
                required[i], next.size, next.memorySize, copied, old.buffer != nullptr, allocationMs[i], allocationMs[i]);
        }
        RetireResidentBufferResource(old);
        old = next;
        next = {};
    }
    PerfAuditSet(PerfAuditMetric::VoxelGeometryArenaOverlapBytes, oldArenaBytes + newArenaBytes);
    if (!reuseAs)
    {
        RetireResidentAccelerationStructure(mesh.accelerationStructure);
        mesh.accelerationStructure = unpublishedAs;
        unpublishedAs = {};
    }
    mesh.computeProviderReservation = provider.reservation;
    mesh.allocationRetryTick = 0;
    arena.allocationMeshRetry.ObserveSuccess(mMapWorld.buildSerial, retryTick, mesh.resourceKey, predictedBytes);
    arena.MarkMaintenanceMutation();
    if (reclaimEnabled) ++arena.reclaimGeometryRevision;
    return report("reserved", Result::Ready);
}

bool NRIRenderer::BuildPreparedPersistentVoxelBottomLevel(const NRIBufferResource& vertices,
    const NRIBufferResource& indices, uint32_t vertexOffset, uint32_t vertexCount,
    uint32_t indexOffset, uint32_t indexCount, uint32_t primitiveCount,
    NRIAccelerationStructureResource& accelerationStructure, NRIBufferResource* buildScratch)
{
    auto& scratch = buildScratch ? *buildScratch : mScratchBuffer;
    auto& resource = accelerationStructure;
    if (!mFrameBuffer || !mFrameBuffer->HasCurrentCommandBuffer() || !resource.accelerationStructure ||
        resource.buildVertexBuffer || resource.buildIndexBuffer || !resource.buildTypeValid ||
        resource.buildType != nri::AccelerationStructureType::BOTTOM_LEVEL ||
        resource.buildFlags != GetNRIPersistentVoxelAllocationBlasFlags() ||
        resource.buildVertexCount < vertexCount || resource.buildIndexCount < indexCount ||
        !scratch.buffer || scratch.size < resource.buildScratchSize) return false;
    // All device allocations were reserved physically before arena publication.
    // Budget contraction cannot cause an extra allocation at this build boundary.
    auto geometry = Geometry(vertexCount, indexCount);
    geometry.triangles.vertexBuffer = vertices.buffer;
    geometry.triangles.vertexOffset = uint64_t(vertexOffset) * sizeof(nri_scene::SceneVertex);
    geometry.triangles.indexBuffer = indices.buffer;
    geometry.triangles.indexOffset = uint64_t(indexOffset) * sizeof(uint32_t);
    nri::BuildBottomLevelAccelerationStructureDesc build = {};
    build.dst = resource.accelerationStructure;
    build.geometries = &geometry;
    build.geometryNum = 1;
    build.scratchBuffer = scratch.buffer;
    mFrameBuffer->mRayTracing.CmdBuildBottomLevelAccelerationStructures(*mFrameBuffer->mCommandBuffer, &build, 1);
    NoteWorldBlasContentChanged();
    nri::BufferBarrierDesc barriers[2] = {};
    barriers[0].buffer = mFrameBuffer->mRayTracing.GetAccelerationStructureBuffer(*resource.accelerationStructure);
    barriers[0].before = NRIResourceAccelerationStructureWriteAccess();
    barriers[0].after = NRIResourceAccelerationStructureReadAccess();
    barriers[1].buffer = scratch.buffer;
    barriers[1].before = NRIResourceAccelerationStructureScratchAccess();
    barriers[1].after = NRIResourceAccelerationStructureScratchAccess();
    nri::BarrierDesc barrier = {};
    barrier.buffers = barriers;
    barrier.bufferNum = 2;
    mFrameBuffer->mCore.CmdBarrier(*mFrameBuffer->mCommandBuffer, barrier);
    mBuiltDynamicSceneASLastFrame = true;
    resource.buildVertexBuffer = vertices.buffer;
    resource.buildIndexBuffer = indices.buffer;
    resource.buildVertexOffset = vertexOffset;
    resource.buildVertexCount = vertexCount;
    resource.buildIndexOffset = indexOffset;
    resource.buildIndexCount = indexCount;
    resource.buildPrimitiveCount = primitiveCount;
    return true;
}
