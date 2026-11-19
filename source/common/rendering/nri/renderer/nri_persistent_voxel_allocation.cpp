#include "nri_persistent_voxel_allocation.h"
#include "nri_persistent_voxel_allocation_policy.h"
#include "nri_voxel_optional_growth_audit.h"
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

    void FormatAdmissionIntent(char (&fields)[384], const NRIVoxelGeometryAdmissionIntent& intent)
    {
        snprintf(fields, sizeof(fields), "\"growth_intent\":\"%s\",\"admission_origins\":%u,\"source_bits\":%u,"
            "\"priority\":%d,\"gpu_force\":%s,\"gpu_prefer\":%s,\"runtime_requested\":%s,\"owns_geometry_work\":%s,"
            "\"required_hint\":%s,\"required_preload\":%s,\"preload_input_hints\":%u,\"runtime_pump\":%s,\"after_handoff\":%s,\"optional_manifest\":%s",
            intent.Kind(), unsigned(intent.origins), intent.sourceBits, intent.priority, intent.force ? "true" : "false",
            intent.prefer ? "true" : "false", intent.RuntimeActor() ? "true" : "false", intent.ownsWork ? "true" : "false",
            intent.Required() ? "true" : "false", intent.requiredPreload ? "true" : "false", unsigned(intent.inputHints.bits),
            intent.runtimePump ? "true" : "false", intent.afterHandoff ? "true" : "false",
            intent.PreferredManifest() && !intent.ownsWork ? "true" : "false");
    }

    NRIVoxelOptionalGrowth::Decision CheckOptionalGrowth(NRIPersistentVoxelResidency& arena, NRIRenderDevice& device,
        uint64_t mesh, const std::array<uint32_t, 3>& rows, uint64_t map, uint32_t frame, uint64_t tick, uint64_t reserve,
        const NRIVoxelGeometryAdmissionIntent& intent)
    {
        const bool audit = PerfAuditActive();
        const uint64_t start = audit ? PerfAuditClockNs() : 0;
        NRIVoxelOptionalGrowth::Demand demand;
        demand.authoritative = arena.actorDemand.HasCurrentMeshAuthority(frame);
        demand.consumers = arena.actorDemand.Upgrade(mesh, frame).requestedConsumers;
        auto active = arena.activeMeshReferences.find(mesh);
        demand.active = active == arena.activeMeshReferences.end() ? 0 : active->second;
        const auto& pins = arena.enemyPolicy.MeshPins();
        auto family = pins.find(mesh);
        demand.family = family != pins.end() && family->second != 0;
        demand.staged = arena.enemyFamilyStaging.MeshPins().count(mesh) != 0;
        if (const char* bypass = demand.Bypass()) return {false, bypass};

        NRIVoxelOptionalGrowth::Footprint footprint{rows, arena.allocationArenaRetry.Generation()};
        uint64_t oldBytes = 0;
        const auto stream = [&](const NRIBufferResource& backing, uint64_t required)
        {
            if (required <= backing.size) return;
            oldBytes = NRIVoxelComputeSaturatingAdd(oldBytes, backing.memorySize);
            footprint.bytes = NRIVoxelComputeSaturatingAdd(footprint.bytes, required);
        };
        stream(arena.vertexBuffer, uint64_t(arena.arenaVertexCursor) * sizeof(nri_scene::SceneVertex));
        stream(arena.indexBuffer, uint64_t(arena.arenaIndexCursor) * sizeof(uint32_t));
        stream(arena.primitiveBuffer, uint64_t(arena.arenaPrimitiveCursor) * sizeof(nri_scene::PrimitiveData));
        if (oldBytes < NRIVoxelOptionalGrowth::MinimumCopyBytes) return {false, "existing-or-small"};

        auto& policy = arena.optionalGrowth;
        const bool query = policy.sampleTick != tick;
        if (query)
        {
            const auto live = device.GetAdapterMemoryTelemetry();
            policy.sampleTick = tick;
            policy.usage = live.localUsageBytes;
            policy.budget = live.liveUsageAvailable ? live.localBudgetBytes : 0;
        }
        const uint64_t testBudget = uint64_t(std::max(0, (int)nri_ptvoxelbudgettestbytes));
        const uint64_t budget = testBudget ? std::min(policy.budget, testBudget) : policy.budget;
        auto& history = arena.allocationBudgetCeiling;
        history.SetBackoffEnabled((bool)nri_ptvoxelallocationretrybackoff);
        history.BeginTick(map, tick);
        const uint64_t ceiling = history.ceilingBytes;
        // The ordinary cap has a short hold. Under backoff, its longer
        // same-map observation also expires on proven physical recovery.
        const uint64_t recent = intent.afterHandoff && history.backoffEnabled ? history.lastDeniedBudgetBytes : 0;
        const uint64_t floor = NRIVoxelComputeSaturatingAdd(policy.usage, NRIVoxelComputeSaturatingAdd(footprint.bytes, reserve));
        const auto result = policy.Consider(mesh, tick, footprint, floor, budget, ceiling, recent);
        if (audit)
        {
            const uint64_t cpuNs = PerfAuditClockNs() - start;
            if (!NRIVoxelOptionalGrowthAudit::Record(map, intent, result, floor, budget, recent, cpuNs, query))
                return result;
            char fields[1408], origin[384];
            FormatAdmissionIntent(origin, intent);
            const char* action = result.defer ? (result.first ? "defer" : "repeat") : "probe";
            const char* pressure = NRIVoxelOptionalGrowth::PressureBasis(floor, budget, recent);
            snprintf(fields, sizeof(fields), "\"renderer_frame\":%u,\"mesh_key\":%llu,\"retry_tick\":%llu,\"retry_clock_unit\":\"presentation\","
                "\"action\":\"%s\",\"reason\":\"%s\",%s,\"current_demand_authority\":true,"
                "\"requested_consumers\":%u,\"active_references\":%u,\"family_pinned\":%u,\"family_staged\":%u,"
                "\"old_arena_bytes\":%llu,\"minimum_arena_bytes\":%llu,\"projected_floor_bytes\":%llu,\"reserve_bytes\":%llu,"
                "\"live_usage_bytes\":%llu,\"budget_bytes\":%llu,\"observed_ceiling_bytes\":%llu,\"budget_query\":%s,"
                "\"recent_denied_budget_bytes\":%llu,\"denial_age_presentations\":%llu,\"pressure_basis\":\"%s\","
                "\"maximum_probe_ticks\":%llu,\"minimum_copy_bytes\":%llu,\"cpu_ns\":%llu",
                frame, (unsigned long long)mesh, (unsigned long long)tick, action, result.reason, origin,
                demand.consumers, demand.active, unsigned(demand.family), unsigned(demand.staged),
                (unsigned long long)oldBytes, (unsigned long long)footprint.bytes, (unsigned long long)floor,
                (unsigned long long)reserve, (unsigned long long)policy.usage, (unsigned long long)budget, (unsigned long long)ceiling,
                query ? "true" : "false", (unsigned long long)recent, (unsigned long long)(recent ? tick - history.deniedTick : 0),
                pressure ? pressure : "none",
                (unsigned long long)NRIVoxelOptionalGrowth::ProbeTicks,
                (unsigned long long)NRIVoxelOptionalGrowth::MinimumCopyBytes, (unsigned long long)cpuNs);
            PerfAuditWriteEvent("voxel_optional_arena_growth", PerfAuditCurrentToken(), fields);
        }
        return result;
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
    uint32_t vertexCount, uint32_t indexCount, uint32_t primitiveCount, bool privateBuildInputs,
    const NRIVoxelGeometryAdmissionIntent& intent)
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
    const bool arenaRecovery = (bool)nri_ptvoxelarenarecovery && !arena.loadingWarmupActive && !arena.preloadPending;
    arena.allocationArenaRetry.BeginTick(mMapWorld.buildSerial, retryTick, arenaRecovery);
    const bool reclaimEnabled = (bool)nri_ptvoxeladmissionreclaim;
    const std::array<uint32_t, 3> requestedRows = { vertexCount, indexCount, primitiveCount };
    const bool growsArena = uint64_t(arena.arenaVertexCursor) * sizeof(nri_scene::SceneVertex) > arena.vertexBuffer.size ||
        uint64_t(arena.arenaIndexCursor) * sizeof(uint32_t) > arena.indexBuffer.size ||
        uint64_t(arena.arenaPrimitiveCursor) * sizeof(nri_scene::PrimitiveData) > arena.primitiveBuffer.size;
    const uint64_t reserve = uint64_t(std::max(0, (int)nri_ptvoxelallocationreservemb)) * 1024 * 1024;
    const bool mayYield = intent.MayYield();
    const char* optionalReason = !mayYield ? intent.Kind() : !growsArena ? "existing-or-small" : "disabled";
    if (mayYield && growsArena && arenaRecovery && reclaimEnabled && !arena.retentionQuarantined && mesh.resourceKey)
    {
        const auto choice = CheckOptionalGrowth(arena, *mFrameBuffer, mesh.resourceKey, requestedRows,
            mMapWorld.buildSerial, mFrameIndex, retryTick, reserve, intent);
        optionalReason = choice.reason;
        if (choice.defer)
        {
            mesh.allocationRetryTick = NRIVoxelComputeSaturatingAdd(retryTick, 1);
            return Result::DeferredCooldown;
        }
    }
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
    uint64_t predictedBytes = 0, actualBytes = 0, oldArenaBytes = 0, newArenaBytes = 0;
    uint32_t candidatePercent = arena.allocationGrowthRetry.SelectPercent(
        uint32_t(std::clamp((int)nri_ptvoxelarenagrowthpercent, 100, 200)));
    uint64_t postUsageBytes = 0, postBudgetBytes = 0, postRawBudgetBytes = 0;
    uint32_t retryFrames = 0;
    NRIVoxelAllocationTiming timing;
    auto budget = timing.Measure(timing.budgetBeforeNs, [&] { return GetPersistentVoxelBudgetSnapshot(true); });
    const uint64_t preflightStart = timing.active ? PerfAuditClockNs() : 0;
    NRIPersistentVoxelAllocationBudget peak;
    NRIVoxelComputeProviderForecast provider;
    uint64_t reclaimSignature = 0;
    bool requirementsComplete = false;
    const auto reclaimRequest = [&]
    {
        NRIVoxelReclaimRequest request;
        request.mesh = mesh.resourceKey; request.epoch = arena.residencyMapGeneration;
        request.tick = retryTick; request.revision = arena.reclaimGeometryRevision;
        request.rows = requestedRows; request.usage = budget.usageBytes;
        request.budget = budget.budgetBytes; request.reserve = reserve;
        request.nonArenaBytes = NRIVoxelComputeSaturatingAdd(predictedBytes - std::min(predictedBytes, newArenaBytes), provider.bytes);
        request.signature = reclaimSignature; request.avoidedArenaBytes = newArenaBytes;
        return request;
    };
    const auto report = [&](const char* decision, Result result)
    {
        if (result == Result::AllocationFailed || (result == Result::DeferredBudget && actualBytes != 0))
            arena.allocationMeshRetry.ObserveDenial(mMapWorld.buildSerial, retryTick, mesh.resourceKey, predictedBytes);
        if (result == Result::DeferredBudget)
            arena.preloadHandoff.ObserveBudgetDenial(mMapWorld.buildSerial);
        if (reclaimEnabled && requirementsComplete && result == Result::DeferredBudget && newArenaBytes &&
            arena.actorDemand.Upgrade(mesh.resourceKey, mFrameIndex).consumers)
        {
            auto request = reclaimRequest();
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
            char fields[2688], origin[384];
            FormatAdmissionIntent(origin, intent);
            snprintf(fields, sizeof(fields), "\"renderer_frame\":%u,\"map_epoch\":%llu,\"decision\":\"%s\","
                "\"mesh_key\":%llu,%s,\"optional_growth_reason\":\"%s\",\"budget_age_frames\":0,\"live_available\":%s,\"usage_bytes\":%llu,"
                "\"budget_bytes\":%llu,\"reserve_bytes\":%llu,\"combined_allocation_bytes\":%llu,\"geometry_allocation_bytes\":%llu,"
                "\"actual_allocation_bytes\":%llu,\"projected_peak_bytes\":%llu,\"growth_percent\":%u,"
                "\"old_arena_bytes\":%llu,\"new_arena_bytes\":%llu,\"concrete_as_scratch_reservation\":true,\"provider_forecast_bytes\":%llu,"
                "\"post_usage_bytes\":%llu,\"post_budget_bytes\":%llu,\"minimum_after_post_denial\":%s,\"shared_retry_frames\":%u,"
                "\"retry_tick\":%llu,\"retry_clock_unit\":\"presentation\",\"cpu_ms\":%.6f,"
                "\"raw_budget_bytes\":%llu,\"allocation_ceiling_bytes\":%llu,\"allocation_ceiling_remaining_ticks\":%llu,"
                "\"post_raw_budget_bytes\":%llu,\"observed_budget_ceiling_bytes\":%llu,"
                "\"observed_budget_hold_ticks\":%llu,\"observed_budget_contraction_streak\":%u,\"observed_budget_probe_reason\":\"%s\"",
                mFrameIndex, (unsigned long long)mMapWorld.buildSerial, decision, (unsigned long long)mesh.resourceKey,
                origin, optionalReason, budget.liveAvailable ? "true" : "false",
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
            if (timing.attempted && mFrameBuffer->mCommandCompletionFence)
                timing.completedAfter = core.GetFenceValue(*mFrameBuffer->mCommandCompletionFence);
            NRIAuditVoxelAllocationTiming(timing, mFrameIndex, mesh.resourceKey, decision);
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

    if (arenaRecovery && reclaimEnabled && growsArena && oldArenaBytes && budget.liveAvailable &&
        !arena.retentionQuarantined &&
        arena.actorDemand.Upgrade(mesh.resourceKey, mFrameIndex).consumers)
    {
        auto request = reclaimRequest();
        const std::array<uint64_t, 3> capacity = {arena.vertexBuffer.size / strides[0],
            arena.indexBuffer.size / strides[1], arena.primitiveBuffer.size / strides[2]};
        const auto preview = arena.admissionReclaim.BeforeGrowth(request, capacity, oldArenaBytes, peak.projectedBytes);
        if (preview != NRIVoxelAdmissionReclaim::GrowthPreview::Proceed)
        {
            if (preview == NRIVoxelAdmissionReclaim::GrowthPreview::Queued)
                arena.ReportAdmissionReclaim("request", "before-physical-arena-growth", request, mFrameIndex);
            // The caller rolls back its provisional lease before the next pump
            // snapshots pins and the real allocator. No native resources exist yet.
            mesh.allocationRetryTick = NRIVoxelComputeSaturatingAdd(retryTick, 1);
            return report("defer-arena-reclaim-preview", Result::DeferredCooldown);
        }
    }

    // A completed native rejection is evidence for later replacements of the
    // same backing. Exact allocation-free or smaller fits remain independent.
    NRIVoxelArenaReplacement replacement;
    if (arenaRecovery && budget.liveAvailable && oldArenaBytes && newArenaBytes > oldArenaBytes)
    {
        replacement.streamMask = unsigned(replace[0]) | (unsigned(replace[1]) << 1) | (unsigned(replace[2]) << 2);
        replacement.growthPercent = candidatePercent;
        replacement.geometryBytes = predictedBytes;
        replacement.arenaBytes = newArenaBytes;
        replacement.projectedBytes = peak.projectedBytes;
        replacement.rawBudgetBytes = budget.rawBudgetBytes;
        // The candidate passed the preflight; the base includes owner credit,
        // other reservations, the provider and reserve, but no proposed geometry.
        replacement.rawHeadroomBytes = budget.rawBudgetBytes - (peak.projectedBytes - predictedBytes);
        if (const auto* prior = arena.allocationArenaRetry.Blocking(replacement, mesh.resourceKey))
        {
            NRIAuditVoxelArenaRetry("defer", mFrameIndex, retryTick, mMapWorld.buildSerial,
                arena.allocationArenaRetry.Generation(), mesh.resourceKey, replacement, *prior);
            // Re-evaluate exact requirements and live room next presentation.
            // The prior physical failure alone sets the maximum probe interval.
            mesh.allocationRetryTick = NRIVoxelComputeSaturatingAdd(retryTick, 1);
            return report("defer-recent-arena-denial", Result::DeferredCooldown);
        }
    }

    std::array<NRIBufferResource, 6> unpublished = {};
    std::array<double, 6> allocationMs = {};
    NRIAccelerationStructureResource unpublishedAs;
    if (timing.active && predictedBytes != 0)
    {
        timing.attempted = true;
        timing.preflightNs = PerfAuditClockNs() - preflightStart;
        timing.recordingFence = mFrameBuffer->GetRecordingCommandFenceValue();
        timing.submittedFence = mFrameBuffer->mLastSubmittedCommandFenceValue;
        if (mFrameBuffer->mCommandCompletionFence)
            timing.completedBefore = core.GetFenceValue(*mFrameBuffer->mCommandCompletionFence);
    }
    const auto discard = [&]()
    {
        for (size_t i = 0; i < unpublished.size(); ++i)
        {
            if (unpublished[i].buffer)
                timing.Measure(timing.resources[i].discardNs, [&] { DestroyBufferResource(unpublished[i]); });
        }
        if (unpublishedAs.accelerationStructure)
            timing.Measure(timing.resources.back().discardNs, [&] { DestroyAccelerationStructureResource(unpublishedAs); });
    };
    const auto deferCommitted = [&](const char* decision, uint64_t rawBudget, bool finalLiveCheck)
    {
        // No commands reference these resources. Preserve the finite retry,
        // exact-footprint retry and reclaim handoff of a final budget denial.
        arena.allocationGrowthRetry.ObservePostDenial(candidatePercent, oldArenaBytes, newArenaBytes);
        if (actualBytes)
            arena.allocationBudgetCeiling.ObservePostDenial(mMapWorld.buildSerial, retryTick,
                budget.rawBudgetBytes, rawBudget);
        discard();
        if (finalLiveCheck && replacement.streamMask)
        {
            const double elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            if (const auto* denial = arena.allocationArenaRetry.Remember(replacement, mesh.resourceKey,
                rawBudget, peak.projectedBytes, elapsedMs))
                NRIAuditVoxelArenaRetry("remember", mFrameIndex, retryTick, mMapWorld.buildSerial,
                    arena.allocationArenaRetry.Generation(), mesh.resourceKey, replacement, *denial);
        }
        return report(decision, Result::DeferredBudget);
    };
    const int interimMode = std::clamp((int)nri_ptvoxelinterimbudget, 0, 2);
    for (size_t i = 0; i != targets.size(); ++i)
    {
        // The first three targets are the shared arenas. Only probe an existing
        // arena replacement with at least 1 MiB of real native work still avoidable.
        // Reused resources, provider promises and the safety reserve do not qualify.
        if (i == 3 && interimMode && budget.liveAvailable && oldArenaBytes &&
            predictedBytes - actualBytes >= 1024 * 1024)
        {
            auto& mid = timing.interim;
            mid.mode = interimMode;
            mid.committed = actualBytes;
            mid.remaining = predictedBytes - actualBytes;
            // No renderer state or ownership changes during the unpublished
            // transaction. Query the adapter directly to keep observation passive.
            const auto live = timing.Measure(mid.queryNs, [&] { return mFrameBuffer->GetAdapterMemoryTelemetry(); });
            mid.liveAvailable = live.liveUsageAvailable && live.localBudgetBytes != 0;
            if (mid.liveAvailable)
            {
                mid.liveUsage = live.localUsageBytes;
                mid.liveBudget = live.localBudgetBytes;
                const uint64_t overrideLimit = uint64_t(std::max(0, (int)nri_ptvoxelbudgettestbytes));
                const uint64_t raw = overrideLimit ? std::min(overrideLimit, mid.liveBudget) : mid.liveBudget;
                mid.effectiveBudget = budget.allocationCeilingBytes ? std::min(raw, budget.allocationCeilingBytes) : raw;
                const uint64_t usage = NRIVoxelComputeUsageAfterOwnerCredit(
                    NRIVoxelComputeSaturatingAdd(mid.liveUsage, budget.computeReservedBytes),
                    budget.computeReservedBytes, mesh.computeProviderReservation.Total());
                const auto forecast = ForecastNRIPartiallyCommittedVoxelBudget(budget.usageBytes, usage,
                    mid.committed, mid.remaining, provider.bytes, reserve, mid.effectiveBudget);
                mid.projected = forecast.projectedBytes;
                mid.wouldDefer = !forecast.fits;
                if (interimMode == 2 && mid.wouldDefer)
                {
                    peak = forecast;
                    return deferCommitted("defer-interim-budget", raw, false);
                }
            }
        }
        if (!replace[i]) continue;
        auto& resource = unpublished[i];
        const auto& desc = descriptors[i];
        const auto allocationStart = std::chrono::steady_clock::now();
        auto& resourceTiming = timing.resources[i];
        resourceTiming.bytes = predicted[i];
        if (!timing.Measure(resourceTiming.createNs, [&] {
            return CreateBufferWithoutViewAtLocation(resource, desc.size, desc.structureStride, desc.usage, nri::MemoryLocation::DEVICE);
        }))
        { discard(); return report("buffer-allocation-failed", Result::AllocationFailed); }
        if (i != 5)
        {
            nri::BufferViewDesc view = {};
            view.buffer = resource.buffer;
            view.type = nri::BufferView::STRUCTURED_BUFFER;
            view.size = nri::WHOLE_SIZE;
            view.structureStride = desc.structureStride;
            if (timing.Measure(resourceTiming.viewNs, [&] { return core.CreateBufferView(view, resource.shaderView); }) != nri::Result::SUCCESS)
            { discard(); return report("view-allocation-failed", Result::AllocationFailed); }
            if (NRIResourceUsageIncludes(desc.usage, nri::BufferUsageBits::SHADER_RESOURCE_STORAGE))
            {
                view.type = nri::BufferView::STORAGE_STRUCTURED_BUFFER;
                if (timing.Measure(resourceTiming.viewNs, [&] { return core.CreateBufferView(view, resource.storageView); }) != nri::Result::SUCCESS)
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
        auto& asTiming = timing.resources.back();
        asTiming.bytes = asBytes;
        if (timing.Measure(asTiming.createNs, [&] {
            return rt.CreateCommittedAccelerationStructure(*mFrameBuffer->mDevice, nri::MemoryLocation::DEVICE,
                0.0f, asDesc, unpublishedAs.accelerationStructure);
        }) != nri::Result::SUCCESS)
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
    const auto postBudget = timing.Measure(timing.budgetAfterNs, [&] { return GetPersistentVoxelBudgetSnapshot(true); });
    postUsageBytes = postBudget.usageBytes;
    postBudgetBytes = postBudget.budgetBytes;
    postRawBudgetBytes = postBudget.rawBudgetBytes;
    const uint64_t postUsage = NRIVoxelComputeUsageAfterOwnerCredit(postBudget.usageBytes,
        postBudget.computeReservedBytes, mesh.computeProviderReservation.Total());
    peak = ForecastNRIPartiallyCommittedVoxelBudget(budget.usageBytes, postUsage, actualBytes, 0,
        provider.bytes, reserve, postBudget.budgetBytes);
    if ((actualBytes != 0 || provider.bytes != 0) && !peak.fits)
        return deferCommitted("defer-post-budget", postBudget.rawBudgetBytes, postBudget.liveAvailable);

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
            const uint64_t copyStart = timing.active ? PerfAuditClockNs() : 0;
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
            if (timing.active)
            {
                timing.copyRecordNs += PerfAuditClockNs() - copyStart;
                timing.copyBytes += copied;
            }
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
        timing.Measure(timing.retireNs, [&] { RetireResidentBufferResource(old); });
        old = next;
        next = {};
    }
    PerfAuditSet(PerfAuditMetric::VoxelGeometryArenaOverlapBytes, oldArenaBytes + newArenaBytes);
    if (newArenaBytes) arena.allocationArenaRetry.BackingChanged();
    if (!reuseAs)
    {
        timing.Measure(timing.retireNs, [&] { RetireResidentAccelerationStructure(mesh.accelerationStructure); });
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
