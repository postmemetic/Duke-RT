#include "nri_renderer.h"
#include "../../../platform/loading_window.h"
#include "nri_persistent_voxel_geometry_arena_policy.h"
#include "nri_persistent_voxel_allocation_policy.h"
#include "nri_persistent_voxel_services.h"
#include "nri_voxel_compute_preload.h"
#include "nri_voxel_required_arena_layout.h"
#include "nri_cvars.h"
#include "../system/nri_renderdevice.h"
#include "perf_audit.h"
#include "printf.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <limits>
#include <tuple>

CVAR(Bool, nri_ptvoxelarenarequiredlayout, false, 0)
CVAR(Int, nri_ptvoxelruntimeextentmb, 0, 0)

namespace
{
// Unstarted direct preloads enter the ordinary admission queue in this order.
// Equal mesh dimensions yield the same initial stream sizes for every material.
uint32_t FirstRequiredPose(const std::vector<nri_scene::PrecachedVoxelVariantView>& variants)
{
    const nri_scene::PrecachedVoxelVariantView* first = nullptr;
    const auto order = [](const auto& pose)
    {
        const int rank = pose.admissionRank || pose.priority <= 0 ? pose.admissionRank : pose.priority * 10000 + 9900;
        return std::make_tuple(rank, pose.priority, !pose.gpuForce, !pose.gpuPrefer, UINT32_MAX - pose.primitiveCount);
    };
    for (const auto& pose : variants)
    {
        if (!pose.directOnlyAdmission || !pose.preloadGeometry || !pose.primitiveCount ||
            !nri_scene::IsRequiredVoxelPreload(pose.sourceBits, pose.priority, pose.gpuForce, pose.gpuPrefer)) continue;
        if (!first || order(pose) < order(*first)) first = &pose;
    }
    return first ? first->primitiveCount : 0u;
}
}

void NRIRenderer::PreSizePersistentVoxelGeometryArenas(
    const NRIVoxelComputePreloadStats& stats, const NRIVoxelComputePreloadSettings& settings,
    const std::vector<nri_scene::PrecachedVoxelVariantView>& variants)
{
    auto& arena = mPersistentVoxels;
    const uint64_t buildSerial = mMapWorld.buildSerial;
    if (buildSerial == 0 || arena.arenaPresizeBuildSerial == buildSerial ||
        (stats.rawSelectedUniqueGeometryBytes == 0 && stats.rawRuntimeWithheldUniqueGeometryBytes == 0))
        return;

    // This is optional capacity planning, never a migration of published data.
    // One attempt per map prevents a failed reservation from stalling every load tick.
    arena.arenaPresizeBuildSerial = buildSerial;
    arena.arenaUsesRequiredLayout = false;
    arena.startupFamilyBudget = {};
    NRIVoxelStartupFamilyBudget families;
    const auto start = std::chrono::steady_clock::now();
    const bool requiredLayout = (bool)nri_ptvoxelarenarequiredlayout && (bool)nri_ptvoxelallocationbudget &&
        (bool)nri_ptactorspritefallback && !settings.strict;
    const bool inventorySizing = (bool)nri_ptvoxelallocationbudget && ((bool)nri_ptvoxelarenastartupinventory || requiredLayout);
    const auto& inventory = stats.startupGeometry;
    const uint64_t preloadBytes = inventory.requiredBytes + inventory.optionalPreloadBytes;
    const auto plan = BuildNRIPersistentVoxelGeometryArenaPlan(
        inventorySizing ? preloadBytes : stats.rawSelectedUniqueGeometryBytes,
        inventorySizing ? inventory.withheldBytes : stats.rawRuntimeWithheldUniqueGeometryBytes,
        stats.rawSelectedLargestUniqueGeometryBytes);
    // When full inventory won't fit, warm the admitted poses, allow one late
    // alias of a warmed mesh and one actual deferred mesh before the first grow.
    const auto pressurePlan = BuildNRIPersistentVoxelGeometryArenaPlan(preloadBytes,
        inventory.largestWithheldBytes, inventory.largestPreloadBytes);
    NRIAdapterMemoryTelemetry adapter = {};
    NRIAdapterMemoryTelemetry postAdapter = {};
    NRIPersistentVoxelGeometryArenaBudget budget = {};
    uint64_t currentUsage = 0, predictedBytes = 0, committedBytes = 0, selectedBytes = 0, preflightBytes = 0;
    uint32_t testedCandidates = 0;
    uint32_t firstRequiredPrimitives = 0;
    bool selectedRequiredLayout = false;
    std::array<uint64_t, 3> ordinaryRows{};
    uint64_t extraPrimitives = 0;
    const char* extentFallback = "none";
    const auto report = [&](const char* decision)
    {
        const double elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (PerfAuditActive())
        {
            char fields[2560];
            snprintf(fields, sizeof(fields),
                "\"renderer_frame\":%u,\"map_epoch\":%llu,\"phase\":\"loading\",\"decision\":\"%s\","
                "\"selected_geometry_bytes\":%llu,\"withheld_geometry_bytes\":%llu,\"alias_reserve_bytes\":%llu,"
                "\"full_target_geometry_bytes\":%llu,\"selected_target_geometry_bytes\":%llu,\"plan_kind\":\"%s\","
                "\"inventory_sizing\":%s,\"required_layout\":%s,\"first_required_primitives\":%u,\"runtime_extent_primitives\":%llu,\"runtime_extent_fallback\":\"%s\",\"required_geometry_bytes\":%llu,\"optional_preload_bytes\":%llu,"
                "\"pressure_target_bytes\":%llu,\"pressure_alias_bytes\":%llu,\"pressure_withheld_bytes\":%llu,"
                "\"preflight_allocation_bytes\":%llu,\"accounting_allowance_bytes\":%llu,"
                "\"placed_meshes\":%zu,\"placed_geometry_bytes\":%llu,\"placed_blas_bytes\":%llu,\"placed_scratch_bytes\":%llu,\"placed_material_bytes\":%llu,"
                "\"candidate_count\":%u,\"budget_candidate_valid\":%s,\"predicted_allocation_bytes\":%llu,\"committed_bytes\":%llu,"
                "\"live_budget_available\":%s,\"current_usage_bytes\":%llu,\"local_budget_bytes\":%llu,"
                "\"minimum_reserve_bytes\":%llu,\"future_bytes\":%llu,\"required_headroom_bytes\":%llu,"
                "\"available_headroom_bytes\":%llu,\"post_live_budget_available\":%s,"
                "\"post_local_usage_bytes\":%llu,\"post_local_budget_bytes\":%llu,\"cpu_ms\":%.6f",
                mFrameIndex, (unsigned long long)buildSerial, decision,
                (unsigned long long)plan.plannedGeometryBytes, (unsigned long long)plan.plannedRuntimeTailBytes,
                (unsigned long long)plan.lateAliasReserveBytes, (unsigned long long)plan.targetGeometryBytes,
                (unsigned long long)selectedBytes, selectedBytes == 0 ? "none" :
                    (selectedBytes >= plan.targetGeometryBytes ? "full" : "partial"),
                inventorySizing ? "true" : "false", selectedRequiredLayout ? "true" : "false", firstRequiredPrimitives,
                (unsigned long long)extraPrimitives, extentFallback,
                (unsigned long long)inventory.requiredBytes, (unsigned long long)inventory.optionalPreloadBytes,
                (unsigned long long)pressurePlan.targetGeometryBytes, (unsigned long long)inventory.largestPreloadBytes,
                (unsigned long long)inventory.largestWithheldBytes, (unsigned long long)preflightBytes,
                (unsigned long long)(inventorySizing ? NRIVoxelStartupAccountingAllowance : 0), families.meshes.size(),
                (unsigned long long)families.geometryBytes, (unsigned long long)families.blasBytes,
                (unsigned long long)families.largestScratchBytes, (unsigned long long)families.materialBytes, testedCandidates,
                selectedBytes != 0 ? "true" : "false",
                (unsigned long long)predictedBytes, (unsigned long long)committedBytes,
                adapter.liveUsageAvailable ? "true" : "false", (unsigned long long)currentUsage,
                (unsigned long long)adapter.localBudgetBytes, (unsigned long long)settings.minimumLocalMemoryReserveBytes,
                (unsigned long long)budget.futureBytes, (unsigned long long)budget.requiredBytes,
                (unsigned long long)budget.availableBytes, postAdapter.liveUsageAvailable ? "true" : "false",
                (unsigned long long)postAdapter.localUsageBytes, (unsigned long long)postAdapter.localBudgetBytes, elapsedMs);
            PerfAuditWriteEvent("resident_arena_plan", PerfAuditCurrentToken(), fields);
        }
        if ((int)nri_ptloadingtrace >= 1 || (int)nri_ptvoxelcomputetrace > 0)
            Printf("NRI PT voxel arena plan: build_serial=%llu decision=%s full_target=%llu selected_target=%llu candidates=%u predicted=%llu committed=%llu usage=%llu budget=%llu required_headroom=%llu available_headroom=%llu ms=%.3f\n",
                (unsigned long long)buildSerial, decision, (unsigned long long)plan.targetGeometryBytes,
                (unsigned long long)selectedBytes, testedCandidates,
                (unsigned long long)predictedBytes, (unsigned long long)committedBytes,
                (unsigned long long)currentUsage, (unsigned long long)adapter.localBudgetBytes,
                (unsigned long long)budget.requiredBytes, (unsigned long long)budget.availableBytes, elapsedMs);
    };
    if (arena.arenaVertexCursor || arena.arenaIndexCursor || arena.arenaPrimitiveCursor ||
        arena.vertexBuffer.buffer || arena.indexBuffer.buffer || arena.primitiveBuffer.buffer)
    { report("retained-arena"); return; }
    arena.InvalidateRuntimeExtent();
    constexpr uint64_t bytesPerPrimitive = 2ull * sizeof(nri_scene::SceneVertex) +
        3ull * sizeof(uint32_t) + sizeof(nri_scene::PrimitiveData);
    if (plan.overflow || (inventorySizing && pressurePlan.overflow) || plan.targetGeometryBytes == 0 ||
        plan.targetGeometryBytes > std::numeric_limits<uint64_t>::max() - (bytesPerPrimitive - 1))
    { report("estimate-overflow"); return; }
    const uint64_t startupTarget = (bool)nri_ptvoxelallocationbudget ?
        GetNRIPersistentVoxelStartupTarget(plan.targetGeometryBytes,
            uint32_t(std::clamp((int)nri_ptvoxelarenastartuppercent, 0, 100)),
            uint64_t(std::max(0, (int)nri_ptvoxelarenastartupmaxmb)) * 1024 * 1024) : plan.targetGeometryBytes;
    if (!startupTarget) { report("startup-reservation-disabled"); return; }
    const uint64_t requestedPrimitives = (startupTarget + bytesPerPrimitive - 1) / bytesPerPrimitive;
    if (!mFrameBuffer || !mFrameBuffer->mDevice || !mFrameBuffer->mCore.GetBufferMemoryDesc2 ||
        !mFrameBuffer->mCore.GetDeviceDesc(*mFrameBuffer->mDevice).features.getMemoryDesc2)
    { report("memory-requirements-unavailable"); return; }
    adapter = mFrameBuffer->GetAdapterMemoryTelemetry();
    if (!adapter.liveUsageAvailable || adapter.localBudgetBytes == 0)
    { report("live-budget-unavailable"); return; }
    currentUsage = std::max(GetMemoryTelemetry().totalTrackedBytes, adapter.localUsageBytes);
    budget.availableBytes = adapter.localBudgetBytes > currentUsage ? adapter.localBudgetBytes - currentUsage : 0;

    const auto geometryUsage = NRIResourceFlags(NRIResourceFlags(nri::BufferUsageBits::SHADER_RESOURCE,
        nri::BufferUsageBits::SHADER_RESOURCE_STORAGE), nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT);
    const auto primitiveUsage = NRIResourceFlags(nri::BufferUsageBits::SHADER_RESOURCE,
        nri::BufferUsageBits::SHADER_RESOURCE_STORAGE);
    const auto& limits = mFrameBuffer->mCore.GetDeviceDesc(*mFrameBuffer->mDevice).memory;
    uint64_t maximumPrimitives = std::min<uint64_t>(requestedPrimitives, std::numeric_limits<uint32_t>::max() / 3ull);
    if (limits.bufferMaxSize != 0)
    {
        maximumPrimitives = std::min<uint64_t>(maximumPrimitives, limits.bufferMaxSize / (2ull * sizeof(nri_scene::SceneVertex)));
        maximumPrimitives = std::min<uint64_t>(maximumPrimitives, limits.bufferMaxSize / (3ull * sizeof(uint32_t)));
        maximumPrimitives = std::min<uint64_t>(maximumPrimitives, limits.bufferMaxSize / sizeof(nri_scene::PrimitiveData));
    }
    uint64_t partialTarget = maximumPrimitives;
    if (inventorySizing)
    {
        const uint64_t bytes = std::min(startupTarget, pressurePlan.targetGeometryBytes);
        partialTarget = std::min(maximumPrimitives, (bytes + bytesPerPrimitive - 1) / bytesPerPrimitive);
        if (maximumPrimitives < requestedPrimitives) maximumPrimitives = partialTarget;
    }
    const uint64_t minimumMeshBytes = inventorySizing && inventory.largestPreloadBytes ?
        inventory.largestPreloadBytes : stats.rawSelectedLargestUniqueGeometryBytes;
    const uint64_t minimumPrimitives = std::max<uint64_t>(1, (minimumMeshBytes + bytesPerPrimitive - 1) / bytesPerPrimitive);
    std::array<nri::BufferDesc, 3> descriptors = {};
    std::array<uint64_t, 3> predicted = {};
    if (inventorySizing && (bool)nri_ptvoxelstartupfamilybudget)
        families = PriceNRIVoxelStartupFamilies(*mFrameBuffer->mDevice, mFrameBuffer->mRayTracing, limits.alignmentDefault, variants);
    constexpr std::array<uint32_t, 3> strides{sizeof(nri_scene::SceneVertex), sizeof(uint32_t), sizeof(nri_scene::PrimitiveData)};
    const auto priceRows = [&](const std::array<uint64_t, 3>& rows)
    {
        LoadingWindow::Scope::ServiceIfActive();
        std::array<nri::BufferDesc, 3> candidate = {};
        candidate[0].size = rows[0] * strides[0];
        candidate[0].structureStride = sizeof(nri_scene::SceneVertex);
        candidate[0].usage = geometryUsage;
        candidate[1].size = rows[1] * strides[1];
        candidate[1].structureStride = sizeof(uint32_t);
        candidate[1].usage = geometryUsage;
        candidate[2].size = rows[2] * strides[2];
        candidate[2].structureStride = sizeof(nri_scene::PrimitiveData);
        candidate[2].usage = primitiveUsage;
        uint64_t candidateBytes = 0, logicalBytes = 0;
        uint64_t chargedBytes = inventorySizing ? NRIVoxelStartupAccountingAllowance : 0;
        std::array<uint64_t, 3> requirementsBytes = {};
        for (size_t i = 0; i < candidate.size(); ++i)
        {
            if (!rows[i] || (limits.bufferMaxSize && candidate[i].size > limits.bufferMaxSize)) return false;
            logicalBytes += candidate[i].size;
            nri::MemoryDesc requirements = {};
            mFrameBuffer->mCore.GetBufferMemoryDesc2(*mFrameBuffer->mDevice, candidate[i], nri::MemoryLocation::DEVICE, requirements);
            if (requirements.size < candidate[i].size ||
                requirements.size > std::numeric_limits<uint64_t>::max() - candidateBytes ||
                (limits.allocationMaxSize != 0 && requirements.size > limits.allocationMaxSize)) return false;
            requirementsBytes[i] = requirements.size;
            candidateBytes += requirements.size;
            if (inventorySizing && !AccumulateNRIVoxelStartupHeapCharge(requirements.size,
                std::max(limits.alignmentDefault, requirements.alignment), chargedBytes)) return false;
        }
        // A partial plan budgets future AS/scratch for the selected geometry
        // capacity, not an assertion that the entire manifest will fit later.
        auto candidateBudget = BuildNRIPersistentVoxelGeometryArenaBudget(currentUsage, adapter.localBudgetBytes,
            settings.minimumLocalMemoryReserveBytes, inventorySizing ? chargedBytes : candidateBytes, logicalBytes,
            stats.rawSelectedUniqueSourceBytes, settings.peakEstimatePercent);
        if (!candidateBudget.fits) return false;
        if (!families.meshes.empty())
        {
            // The ordinary percentage forecast already includes AS and scratch. Apply the native floor once.
            uint64_t nativeFuture = families.FutureBytes();
            if (!AccumulateNRIVoxelStartupHeapCharge(stats.rawSelectedUniqueSourceBytes, 1, nativeFuture)) return false;
            if (nativeFuture > candidateBudget.futureBytes)
            {
                const uint64_t extra = nativeFuture - candidateBudget.futureBytes;
                if (extra > candidateBudget.availableBytes - candidateBudget.requiredBytes) return false;
                candidateBudget.requiredBytes += extra;
                candidateBudget.futureBytes = nativeFuture;
            }
        }
        descriptors = candidate;
        predicted = requirementsBytes;
        predictedBytes = candidateBytes;
        preflightBytes = inventorySizing ? chargedBytes : candidateBytes;
        selectedBytes = logicalBytes;
        budget = candidateBudget;
        return true;
    };
    const auto pricePrimitives = [&](uint64_t n) { return priceRows({2 * n, 3 * n, n}); };
    if (requiredLayout && inventory.requiredBytes && inventory.requiredBytes % bytesPerPrimitive == 0 &&
        maximumPrimitives >= minimumPrimitives)
    {
        // Prefer the entire configured inventory when funded. Under pressure,
        // first try the normal growth boundary covering the required cohort.
        ++testedCandidates;
        if (!pricePrimitives(maximumPrimitives))
        {
            firstRequiredPrimitives = FirstRequiredPose(variants);
            const auto rows = NRIRequiredArenaRows(firstRequiredPrimitives, inventory.requiredBytes / bytesPerPrimitive,
                uint32_t(std::clamp((int)nri_ptvoxelarenagrowthpercent, 100, 200)));
            const uint64_t bytes = rows[0] * strides[0] + rows[1] * strides[1] + rows[2] * strides[2];
            if (bytes && bytes <= startupTarget)
            {
                const uint64_t extentBudget = uint64_t(std::max(0, (int)nri_ptvoxelruntimeextentmb)) * 1024 * 1024;
                const uint64_t extra = std::min(extentBudget, startupTarget - bytes) / bytesPerPrimitive;
                const int pageMode = (int)nri_ptvoxeloverflowpage;
                if (extra >= 2 && (pageMode == 1 || pageMode == 2) && arena.SelectedOverflowMesh())
                {
                    const std::array<uint64_t, 3> extended{rows[0] + 2 * extra, rows[1] + 3 * extra, rows[2] + extra};
                    constexpr uint64_t ordinaryLimit = NRIPersistentVoxelGeometryRangeHandle::PageTag;
                    ++testedCandidates;
                    if (extended[0] <= ordinaryLimit && extended[1] <= UINT32_MAX && extended[2] <= ordinaryLimit &&
                        priceRows(extended))
                    {
                        extraPrimitives = extra;
                        selectedRequiredLayout = true;
                        ordinaryRows = rows;
                    }
                    else extentFallback = "extended-preflight";
                }
                if (!selectedRequiredLayout)
                {
                    ++testedCandidates;
                    selectedRequiredLayout = priceRows(rows);
                }
            }
        }
    }
    if (!selectedBytes)
    {
        const auto selection = SelectNRIPersistentVoxelGeometryArenaCapacity(maximumPrimitives, minimumPrimitives, pricePrimitives, partialTarget);
        testedCandidates += selection.candidates;
        if (!selection.primitives) { selectedBytes = 0; report("insufficient-meaningful-capacity"); return; }
    }

    std::array<NRIBufferResource, 3> reserved = {};
    const auto discard = [&]()
    {
        for (auto& resource : reserved) DestroyBufferResource(resource);
        committedBytes = 0;
    };
    const auto services = BuildNRIPersistentVoxelAdmissionServices(*this);
    const auto reserve = [&]() -> const char*
    {
        for (size_t i = 0; i < reserved.size(); ++i)
        {
            if (!services.EnsureArenaBuffer(reserved[i], descriptors[i].size, descriptors[i].structureStride,
                descriptors[i].usage, NRIResourceComputeShaderResourceAccess())) return "allocation-failed";
            // Reservation creates no geometry. Ignore unused rows in any later copy.
            reserved[i].usedSize = 0;
            if (reserved[i].memorySize != predicted[i]) return "memory-requirements-changed";
            committedBytes += reserved[i].memorySize;
        }
        // Budget can change while the driver allocates. A second live query and a
        // conservative initial+actual floor protect against delayed usage reporting.
        postAdapter = mFrameBuffer->GetAdapterMemoryTelemetry();
        const uint64_t usageFloor = currentUsage > std::numeric_limits<uint64_t>::max() - committedBytes ?
            std::numeric_limits<uint64_t>::max() : currentUsage + committedBytes;
        const uint64_t finalUsage = std::max(usageFloor, postAdapter.localUsageBytes);
        const uint64_t remaining = postAdapter.localBudgetBytes > finalUsage ? postAdapter.localBudgetBytes - finalUsage : 0;
        if (!postAdapter.liveUsageAvailable || postAdapter.localBudgetBytes == 0 || finalUsage > postAdapter.localBudgetBytes ||
            remaining < budget.futureBytes || remaining - budget.futureBytes < settings.minimumLocalMemoryReserveBytes)
            return "budget-changed";
        return nullptr;
    };
    if (const char* failure = reserve())
    {
        discard();
        if (!extraPrimitives) { report(failure); return; }
        // An optional extent must never cost the required cohort its ordinary
        // layout. These buffers were unpublished and no commands reference them.
        extentFallback = failure;
        extraPrimitives = selectedBytes = predictedBytes = preflightBytes = 0;
        selectedRequiredLayout = false;
        postAdapter = {};
        budget = {};
        adapter = mFrameBuffer->GetAdapterMemoryTelemetry();
        currentUsage = std::max(GetMemoryTelemetry().totalTrackedBytes, adapter.localUsageBytes);
        if (!adapter.liveUsageAvailable || !adapter.localBudgetBytes) { report("live-budget-unavailable"); return; }
        budget.availableBytes = adapter.localBudgetBytes > currentUsage ? adapter.localBudgetBytes - currentUsage : 0;
        ++testedCandidates;
        if (!priceRows(ordinaryRows)) { report("required-layout-reprice-denied"); return; }
        selectedRequiredLayout = true;
        if (const char* ordinaryFailure = reserve()) { discard(); report(ordinaryFailure); return; }
    }

    if (extraPrimitives)
    {
        arena.runtimeExtent.extraPrimitives = extraPrimitives;
        arena.runtimeExtent.baseRows = ordinaryRows;
        for (size_t i = 0; i < reserved.size(); ++i)
        {
            arena.runtimeExtent.buffers[i] = reserved[i].buffer;
            arena.runtimeExtent.physicalBytes[i] = descriptors[i].size;
            arena.runtimeExtentViews[i] = reserved[i];
            // Views already span the complete descriptor. Expose only the
            // ordinary prefix to preload, growth, and reclaim sizing.
            reserved[i].size = ordinaryRows[i] * strides[i];
        }
    }
    arena.vertexBuffer = reserved[0];
    arena.indexBuffer = reserved[1];
    arena.primitiveBuffer = reserved[2];
    arena.arenaUsesRequiredLayout = selectedRequiredLayout;
    arena.allocationArenaRetry.BackingChanged();
    report("reserved");
    if (!families.meshes.empty() && selectedBytes - extraPrimitives * bytesPerPrimitive >= families.geometryBytes)
    {
        families.build = buildSerial;
        arena.startupFamilyBudget = std::move(families);
    }
}
