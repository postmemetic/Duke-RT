#include "nri_voxel_compute_allocation.h"
#include "nri_voxel_compute_allocation_policy.h"
#include "nri_persistent_voxel_allocation_policy.h"
#include "nri_renderer.h"
#include "nri_cvars.h"
#include "nri_shader_contracts.h"
#include "perf_audit.h"
#include <algorithm>
#include <cstdio>
#include <limits>
#include <vector>

namespace
{
    nri::BufferDesc Descriptor(const NRIVoxelComputeBufferAllocation& request)
    {
        nri::BufferDesc result = {};
        result.size = request.bytes;
        result.structureStride = request.stride;
        result.usage = request.usage;
        return result;
    }

    bool Compatible(const NRIVoxelComputeBufferAllocation& request)
    {
        const auto& resource = *request.target;
        return resource.buffer && resource.size >= request.bytes && resource.stride == request.stride &&
            resource.memoryLocation == request.location && NRIResourceUsageIncludes(resource.usage, request.usage) &&
            (!request.createView || resource.shaderView);
    }
}

bool QueryNRIVoxelComputeBufferAllocation(const NRIResourceContext& context,
    const NRIVoxelComputeBufferAllocation& request, uint64_t& bytes)
{
    bytes = 0;
    if (!request.bytes || !request.stride || !context.device || !context.core ||
        !context.core->GetBufferMemoryDesc2 || !context.core->GetDeviceDesc(*context.device).features.getMemoryDesc2)
        return false;
    const auto& limits = context.core->GetDeviceDesc(*context.device).memory;
    if (limits.bufferMaxSize && request.bytes > limits.bufferMaxSize) return false;
    nri::MemoryDesc memory = {};
    const auto descriptor = Descriptor(request);
    context.core->GetBufferMemoryDesc2(*context.device, descriptor, request.location, memory);
    if (memory.size < request.bytes || (limits.allocationMaxSize && memory.size > limits.allocationMaxSize)) return false;
    bytes = memory.size;
    return true;
}

NRIPersistentVoxelAllocationResult ForecastNRIVoxelComputeProviderAllocations(
    const NRIResourceContext& context, FVoxelModel* model, NRIVoxelComputeProviderForecast& forecast, bool queueMissingSource)
{
    using Result = NRIPersistentVoxelAllocationResult;
    forecast = {};
    NRIVoxelComputeProviderSource source;
    if (!ReadNRIVoxelComputeProviderSource(model, source, queueMissingSource)) return Result::DeferredSource;
    forecast.sourcePage = source.pageIndex;
    const auto add = [&](uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::MemoryLocation location)
    {
        NRIVoxelComputeBufferAllocation request;
        request.bytes = size;
        request.stride = stride;
        request.usage = usage;
        request.location = location;
        uint64_t bytes = 0;
        if (!QueryNRIVoxelComputeBufferAllocation(context, request, bytes) || bytes > UINT64_MAX - forecast.bytes) return false;
        forecast.bytes += bytes;
        ++forecast.bufferCount;
        return true;
    };
    const auto device = nri::MemoryLocation::DEVICE;
    const auto upload = nri::MemoryLocation::DEVICE_UPLOAD;
    const auto readback = nri::MemoryLocation::HOST_READBACK;
    const auto read = nri::BufferUsageBits::SHADER_RESOURCE;
    const auto write = nri::BufferUsageBits::SHADER_RESOURCE_STORAGE;
    const auto none = nri::BufferUsageBits::NONE;
    const uint64_t slabPageBytes = uint64_t(source.slabCapacity) * sizeof(NRIVoxelComputeSlabRecord);
    const uint64_t colorPageBytes = uint64_t(source.colorRunCapacity) * sizeof(NRIVoxelComputeColorRunRecord);
    if ((!source.slabAllocated && !add(slabPageBytes, sizeof(NRIVoxelComputeSlabRecord), read, device)) ||
        (!source.colorRunsAllocated && !add(colorPageBytes, sizeof(NRIVoxelComputeColorRunRecord), read, device)) ||
        (!source.uploaded && (!add(slabPageBytes, sizeof(NRIVoxelComputeSlabRecord), none, upload) ||
            !add(colorPageBytes, sizeof(NRIVoxelComputeColorRunRecord), none, upload)))) return Result::RequirementsUnavailable;
    forecast.reservation.rawSourceBytes = forecast.bytes;
    // Reserve a fresh slot for this job even when a reusable slot probably fits.
    // Per-job aligned requirements bound a batched allocation, and parallel
    // scratch is exactly additive in source slab counts. This does not depend
    // on which of the four fenced completion slots eventually becomes free.
    if (!add(sizeof(NRIVoxelComputeJob), sizeof(NRIVoxelComputeJob), none, upload) ||
        !add(sizeof(NRIVoxelComputeJob), sizeof(NRIVoxelComputeJob), read, device) ||
        !add(sizeof(NRIVoxelComputeResult), sizeof(NRIVoxelComputeResult), write, device) ||
        !add(sizeof(NRIVoxelComputeResult), sizeof(NRIVoxelComputeResult), none, readback) ||
        !add(sizeof(NRIVoxelComputeFaceRecord), sizeof(NRIVoxelComputeFaceRecord), read, device) ||
        !add(uint64_t(source.slabCount) * sizeof(NRIVoxelComputeSlabScratch), sizeof(NRIVoxelComputeSlabScratch), write, device))
        return Result::RequirementsUnavailable;
    if (source.fullReadback &&
        (!add(uint64_t(source.vertexCount) * sizeof(NRIVoxelComputeSceneVertex), sizeof(NRIVoxelComputeSceneVertex), none, readback) ||
         !add(uint64_t(source.indexCount) * sizeof(uint32_t), sizeof(uint32_t), none, readback) ||
         !add(uint64_t(source.primitiveCount) * sizeof(NRIVoxelComputePrimitiveData), sizeof(NRIVoxelComputePrimitiveData), none, readback)))
        return Result::RequirementsUnavailable;
    forecast.reservation.dispatchBytes = forecast.bytes - forecast.reservation.rawSourceBytes;
    return Result::Ready;
}

NRIPersistentVoxelAllocationResult ReserveNRIVoxelComputeBuffers(NRIRenderer& renderer,
    const NRIVoxelComputeBufferAllocation* requests, uint32_t count, const char* phase,
    const NRIVoxelComputeAllocationOwners& owners)
{
    using Result = NRIPersistentVoxelAllocationResult;
    if (!(bool)nri_ptvoxelallocationbudget) return Result::Disabled;
    if (!requests || !count || !renderer.GetRecordingCommandFenceValue()) return Result::DeferredFence;
    const auto services = renderer.BuildResourceServices();
    const auto& context = services.context;
    std::vector<uint64_t> predicted(count);
    std::vector<NRIBufferResource> pending(count);
    uint64_t bytes = 0, actualBytes = 0, postUsageBytes = 0, postBudgetBytes = 0, postRawBudgetBytes = 0;
    const uint64_t reserve = uint64_t(std::max(0, (int)nri_ptvoxelallocationreservemb)) * 1024 * 1024;
    const auto budget = renderer.GetPersistentVoxelBudgetSnapshot(true);
    // Forecast promises reserve space from new geometry admissions. The provider
    // spends that space here; subtract promises, never concrete allocations.
    uint64_t credit = 0;
    const auto owns = [&](const PersistentVoxelMeshVariantResource& mesh)
    {
        bool owner = false;
        for (uint32_t i = 0; i != owners.sourceCount; ++i) owner |= mesh.sourceModel == owners.sourceModels[i];
        for (uint32_t i = 0; i != owners.meshCount; ++i) owner |= mesh.resourceKey == owners.meshKeys[i];
        return owner;
    };
    for (const auto& item : renderer.mPersistentVoxels.admissionQueue)
    {
        const auto& mesh = item.second.uploadMeshResource;
        if (owns(mesh)) credit = NRIVoxelComputeSaturatingAdd(credit, mesh.computeProviderReservation.Bytes(owners.stage));
    }
    const auto materialized = [&]()
    {
        for (auto& item : renderer.mPersistentVoxels.admissionQueue)
        {
            auto& mesh = item.second.uploadMeshResource;
            if (owns(mesh)) mesh.computeProviderReservation.Materialized(owners.stage);
        }
        renderer.mPersistentVoxels.MarkMaintenanceMutation();
    };
    credit = std::min(credit, budget.computeReservedBytes);
    const uint64_t physicalUsage = NRIVoxelComputeUsageAfterOwnerCredit(budget.usageBytes, budget.computeReservedBytes, credit);
    auto peak = BuildNRIPersistentVoxelAllocationBudget(physicalUsage, budget.budgetBytes, 0, reserve);
    const auto report = [&](const char* decision, Result result)
    {
        if (result == Result::DeferredBudget)
            renderer.mPersistentVoxels.preloadHandoff.ObserveBudgetDenial(renderer.mMapWorld.buildSerial);
        if (PerfAuditActive())
        {
            char fields[1280];
            snprintf(fields, sizeof(fields), "\"phase\":\"%s\",\"decision\":\"%s\",\"budget_age_frames\":0,"
                "\"usage_after_owner_credit_bytes\":%llu,\"reserved_provider_bytes\":%llu,\"budget_bytes\":%llu,"
                "\"allocation_bytes\":%llu,\"actual_allocation_bytes\":%llu,\"projected_peak_bytes\":%llu,\"buffers\":%u,\"owner_credit_bytes\":%llu,"
                "\"retry_tick\":%llu,\"retry_clock_unit\":\"presentation\","
                "\"raw_budget_bytes\":%llu,\"allocation_ceiling_bytes\":%llu,\"allocation_ceiling_remaining_ticks\":%llu,"
                "\"post_usage_after_owner_credit_bytes\":%llu,\"post_budget_bytes\":%llu,\"post_raw_budget_bytes\":%llu,\"observed_budget_ceiling_bytes\":%llu",
                phase, decision, (unsigned long long)physicalUsage, (unsigned long long)budget.computeReservedBytes,
                (unsigned long long)budget.budgetBytes, (unsigned long long)bytes, (unsigned long long)actualBytes,
                (unsigned long long)peak.projectedBytes, count, (unsigned long long)credit,
                (unsigned long long)GetNRIVoxelAdmissionRetryClock(), (unsigned long long)budget.rawBudgetBytes,
                (unsigned long long)budget.allocationCeilingBytes, (unsigned long long)budget.allocationCeilingRemainingTicks,
                (unsigned long long)postUsageBytes, (unsigned long long)postBudgetBytes, (unsigned long long)postRawBudgetBytes,
                (unsigned long long)renderer.mPersistentVoxels.allocationBudgetCeiling.ceilingBytes);
            PerfAuditWriteEvent("voxel_compute_allocation", PerfAuditCurrentToken(), fields);
        }
        return result;
    };
    for (uint32_t i = 0; i != count; ++i)
    {
        if (!requests[i].target) return report("invalid-target", Result::RequirementsUnavailable);
        if (Compatible(requests[i])) continue;
        // Raw DEVICE pages can also be referenced by other submitted slots.
        // Their immutable capacity is established once; only upload backing
        // may be replaced after this page's upload fence completes.
        if (owners.stage == NRIVoxelComputeProviderStage::RawSource && requests[i].target->buffer &&
            requests[i].location == nri::MemoryLocation::DEVICE)
            return report("immutable-page-mismatch", Result::RequirementsUnavailable);
        if (!QueryNRIVoxelComputeBufferAllocation(context, requests[i], predicted[i]) ||
            predicted[i] > UINT64_MAX - bytes) return report("requirements-unavailable", Result::RequirementsUnavailable);
        bytes += predicted[i];
    }
    if (!bytes) { materialized(); return report("reused", Result::Ready); }
    peak = BuildNRIPersistentVoxelAllocationBudget(physicalUsage, budget.budgetBytes, bytes, reserve);
    if (!peak.fits) return report("defer-budget", Result::DeferredBudget);
    const auto discard = [&]() { for (auto& resource : pending) services.DestroyBufferResource(resource); };
    for (uint32_t i = 0; i != count; ++i)
    {
        if (!predicted[i]) continue;
        auto& resource = pending[i];
        const auto& request = requests[i];
        const auto desc = Descriptor(request);
        if (context.core->CreateCommittedBuffer(*context.device, request.location, 0.0f, desc, resource.buffer) != nri::Result::SUCCESS)
        { discard(); return report("allocation-retry", Result::AllocationFailed); }
        nri::MemoryDesc memory = {};
        context.core->GetBufferMemoryDesc(*resource.buffer, request.location, memory);
        resource.size = resource.usedSize = request.bytes;
        resource.memorySize = memory.size;
        resource.stride = request.stride;
        resource.usage = request.usage;
        resource.memoryLocation = request.location;
        if (memory.size != predicted[i] || memory.size > UINT64_MAX - actualBytes)
        { discard(); return report("requirements-changed", Result::AllocationFailed); }
        actualBytes += memory.size;
        if (request.createView)
        {
            nri::BufferViewDesc view = {};
            view.buffer = resource.buffer;
            view.type = request.view;
            view.size = nri::WHOLE_SIZE;
            view.structureStride = request.stride;
            if (context.core->CreateBufferView(view, resource.shaderView) != nri::Result::SUCCESS)
            { discard(); return report("view-retry", Result::AllocationFailed); }
        }
    }
    const auto post = renderer.GetPersistentVoxelBudgetSnapshot(true);
    const auto floor = BuildNRIPersistentVoxelAllocationBudget(physicalUsage, post.budgetBytes, actualBytes, 0);
    const uint64_t postPhysical = NRIVoxelComputeUsageAfterOwnerCredit(post.usageBytes, post.computeReservedBytes, credit);
    postUsageBytes = postPhysical;
    postBudgetBytes = post.budgetBytes;
    postRawBudgetBytes = post.rawBudgetBytes;
    peak = BuildNRIPersistentVoxelAllocationBudget(std::max(floor.projectedBytes, postPhysical), post.budgetBytes, 0, reserve);
    if (floor.overflow || !peak.fits)
    {
        renderer.mPersistentVoxels.allocationBudgetCeiling.ObservePostDenial(renderer.mMapWorld.buildSerial,
            GetNRIVoxelAdmissionRetryClock(), budget.rawBudgetBytes, post.rawBudgetBytes);
        discard();
        if (actualBytes) renderer.mVoxelBudgetSnapshotFrame = UINT32_MAX;
        return report("defer-post-budget", Result::DeferredBudget);
    }
    for (uint32_t i = 0; i != count; ++i)
    {
        if (!predicted[i]) continue;
        // Caller established the previous slot/upload fence has completed.
        // No command in this recording references the replacement yet.
        services.DestroyBufferResource(*requests[i].target);
        *requests[i].target = pending[i];
        pending[i] = {};
    }
    renderer.mPersistentVoxels.allocationBudgetCeiling.ObserveSuccess(renderer.mMapWorld.buildSerial,
        GetNRIVoxelAdmissionRetryClock(), actualBytes, post.liveAvailable ? post.liveUsageBytes : 0, post.rawBudgetBytes);
    materialized();
    return report("reserved", Result::Ready);
}
