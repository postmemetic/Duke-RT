#include "nri_perf_audit.h"
#include "nri_trace_stats.h"
#include "nri_scene_lights.h"
#include "nri_renderer.h"
#include "../system/nri_renderdevice.h"
#include "c_cvars.h"

#include <array>
#include <cstdarg>
#include <cstdio>
#include <type_traits>

CVAR(Bool, nri_ptperfauditgpu, false, 0)

namespace
{
    // Fixed storage, no per-frame diagnostic allocation or formatting on the
    // disabled path. A sample contains selected scalars and at most 16 hot rows.
    struct Json
    {
        char data[16384] = {};
        size_t used = 0;
        bool valid = true;
        void Add(const char* format, ...)
        {
            if (!valid) return;
            va_list args;
            va_start(args, format);
            const int count = vsnprintf(data + used, sizeof(data) - used, format, args);
            va_end(args);
            if (count < 0 || size_t(count) >= sizeof(data) - used) { valid = false; return; }
            used += size_t(count);
        }
    };
}

bool NRITraceAuditRequested()
{
    return PerfAuditActive() && nri_ptperfauditgpu && PerfAuditCensusRequested();
}

void NRIAuditResidentArenaGrowth(uint32_t rendererFrame, uint32_t stride,
    uint64_t oldSize, uint64_t oldMemory, uint64_t oldUsed, uint64_t required,
    uint64_t newSize, uint64_t newMemory, uint64_t copied, bool retiredToSlot,
    double allocationMs, double totalMs)
{
    if (!PerfAuditActive()) return;
    Json json;
    json.Add("\"domain\":\"resident_arena\",\"renderer_frame\":%u,\"stride\":%u,"
        "\"old_size\":%llu,\"old_memory\":%llu,\"old_used\":%llu,\"required\":%llu,"
        "\"new_size\":%llu,\"new_memory\":%llu,\"copy_recorded_bytes\":%llu,"
        "\"retired_to_slot\":%s,\"allocation_cpu_ms\":%.6f,\"total_cpu_ms\":%.6f",
        rendererFrame, stride, (unsigned long long)oldSize, (unsigned long long)oldMemory,
        (unsigned long long)oldUsed, (unsigned long long)required, (unsigned long long)newSize,
        (unsigned long long)newMemory, (unsigned long long)copied,
        retiredToSlot ? "true" : "false", allocationMs, totalMs);
    PerfAuditWriteEvent("resident_arena_growth", PerfAuditCurrentToken(), json.data);
}

void NRIAuditResidentRetiredDestroy(uint32_t rendererFrame, uint32_t previousSlotFrame,
    uint32_t slot, bool explicitReset, uint64_t bufferCount, uint64_t bufferBytes,
    uint64_t accelerationCount, uint64_t accelerationBytes, double bufferMs, double accelerationMs)
{
    if (!PerfAuditActive()) return;
    Json json;
    json.Add("\"renderer_frame\":%u,\"previous_slot_frame\":%u,\"slot\":%u,\"reason\":\"%s\","
        "\"buffer_count\":%llu,\"buffer_bytes\":%llu,\"as_count\":%llu,\"as_bytes\":%llu,"
        "\"buffer_cpu_ms\":%.6f,\"as_cpu_ms\":%.6f,\"total_cpu_ms\":%.6f,\"domain\":\"resident_scratch_retired\"",
        rendererFrame, previousSlotFrame, slot, explicitReset ? "explicit-reset" : "slot-reuse",
        (unsigned long long)bufferCount, (unsigned long long)bufferBytes,
        (unsigned long long)accelerationCount, (unsigned long long)accelerationBytes,
        bufferMs, accelerationMs, bufferMs + accelerationMs);
    PerfAuditWriteEvent("resident_retired_destroy", PerfAuditCurrentToken(), json.data);
}

NRITraceAuditCapture NRIBeginTraceAudit(bool diagnostic, bool conflictingProbe, uint64_t rendererFrame, uint64_t fence)
{
    if (!NRITraceAuditRequested()) return {};
    static uint64_t nextRequest = 0;
    NRITraceAuditCapture capture = { PerfAuditCurrentToken(), ++nextRequest };
    PerfAuditAdd(PerfAuditMetric::AuditReadbacksRequested);
    char fields[256];
    snprintf(fields, sizeof(fields), "\"request\":%llu,\"renderer_frame\":%llu,\"fence\":%llu,\"diagnostic\":%s",
        (unsigned long long)capture.request, (unsigned long long)rendererFrame, (unsigned long long)fence, diagnostic ? "true" : "false");
    PerfAuditWriteEvent("shader_request", capture.token, fields);
    if (!diagnostic || conflictingProbe)
    {
        NRITraceAuditLoss(capture, rendererFrame, fence, !diagnostic ? "diagnostic_variant_unavailable" : "conflicting_broad_probe");
        return {};
    }
    return capture;
}

void NRITraceAuditLoss(const NRITraceAuditCapture& capture, uint64_t rendererFrame, uint64_t fence, const char* reason, bool abandoned)
{
    if (!capture) return;
    // Old-epoch completions cannot affect a newly started audit's counters.
    if (capture.token.epoch == PerfAuditCurrentToken().epoch)
        PerfAuditAdd(abandoned ? PerfAuditMetric::AuditReadbacksAbandoned : PerfAuditMetric::AuditReadbacksDropped);
    char fields[320];
    snprintf(fields, sizeof(fields), "\"request\":%llu,\"renderer_frame\":%llu,\"fence\":%llu,\"reason\":\"%s\"",
        (unsigned long long)capture.request, (unsigned long long)rendererFrame, (unsigned long long)fence, reason);
    PerfAuditWriteEvent("shader_loss", capture.token, fields);
}

void NRIPublishTraceAudit(const NRITraceAuditCapture& capture, uint64_t rendererFrame, uint64_t fence,
    const NRITraceShaderDispatchMetadata& metadata, const uint32_t* counters,
    const std::vector<NRITraceShaderInstanceAttribution>& attribution, uint32_t totalInstances)
{
    if (!capture) return;
    Json json;
    json.Add("\"layout\":\"trace-audit-v1\",\"valid\":%s,", counters[127] ? "false" : "true");
    json.Add("\"consumer_presentation\":%llu,\"consumer_checkpoint\":%llu,",
        (unsigned long long)PerfAuditCurrentToken().presentation, (unsigned long long)PerfAuditCurrentToken().checkpoint);
    json.Add("\"request\":%llu,\"renderer_frame\":%llu,\"fence\":%llu,\"width\":%u,\"height\":%u,\"flags\":%u,\"bounces\":%u,\"instance_count\":%u,\"attribution_rows\":%u,\"base_scalars\":[",
        (unsigned long long)capture.request, (unsigned long long)rendererFrame, (unsigned long long)fence,
        metadata.renderWidth, metadata.renderHeight, metadata.traceFlags, metadata.lightBounceCount, totalInstances, (uint32_t)attribution.size());
    for (uint32_t i = 0; i < NRI_TRACE_SHADER_BASE_SCALAR_STAT_COUNT; ++i) json.Add("%s%u", i ? "," : "", counters[i]);
    json.Add("],\"filter_query\":[");
    for (uint32_t i = 0; i < 6; ++i) json.Add("%s%u", i ? "," : "", counters[NRI_TRACE_SHADER_FILTER_QUERY_BASE + i]);
    json.Add("],\"ray_metric_columns\":[\"candidates\",\"committed\",\"accepted\",\"restarts\",\"reject_no_shadow\",\"reject_transparent\",\"reject_other_filter\",\"reserved\"],\"ray_metrics\":[");
    for (uint32_t kind = 0; kind < NRI_TRACE_SHADER_RAY_KIND_COUNT; ++kind)
    {
        json.Add("%s[", kind ? "," : "");
        for (uint32_t metric = 0; metric < 8; ++metric)
            json.Add("%s%u", metric ? "," : "", counters[NRI_TRACE_SHADER_PROBE_RAY_BASE + kind * 8u + metric]);
        json.Add("]");
    }
    json.Add("],\"hot\":[");
    std::array<uint32_t, 16> hot = {};
    hot.fill(UINT32_MAX);
    uint64_t totalCommitted = 0, totalAccepted = 0;
    for (uint32_t i = 0; i < attribution.size(); ++i)
    {
        const uint32_t score = counters[NRI_TRACE_SHADER_INSTANCE_COMMITTED_BASE + i];
        totalCommitted += score;
        totalAccepted += counters[NRI_TRACE_SHADER_INSTANCE_ACCEPTED_BASE + i];
        if (!score) continue;
        for (uint32_t rank = 0; rank < hot.size(); ++rank)
        {
            if (hot[rank] != UINT32_MAX && counters[NRI_TRACE_SHADER_INSTANCE_COMMITTED_BASE + hot[rank]] >= score) continue;
            for (size_t j = hot.size() - 1; j > rank; --j) hot[j] = hot[j - 1];
            hot[rank] = i;
            break;
        }
    }
    uint64_t topCommitted = 0;
    for (uint32_t rank = 0; rank < hot.size() && hot[rank] != UINT32_MAX; ++rank)
    {
        const uint32_t i = hot[rank];
        const auto& row = attribution[i];
        topCommitted += counters[NRI_TRACE_SHADER_INSTANCE_COMMITTED_BASE + i];
        json.Add("%s{\"instance\":%u,\"source\":%u,\"primitive_base\":%u,\"primitive_count\":%u,\"metadata\":[%u,%u,%u],\"committed\":%u,\"accepted\":%u,\"ray_committed\":[",
            rank ? "," : "", i, row.dataSource, row.primitiveOffset, row.primitiveCount, row.metadata0, row.metadata1, row.metadata2,
            counters[NRI_TRACE_SHADER_INSTANCE_COMMITTED_BASE + i], counters[NRI_TRACE_SHADER_INSTANCE_ACCEPTED_BASE + i]);
        for (uint32_t kind = 0; kind < NRI_TRACE_SHADER_RAY_KIND_COUNT; ++kind)
            json.Add("%s%u", kind ? "," : "", counters[NRI_TRACE_SHADER_INSTANCE_KIND_COMMITTED_BASE + kind * NRI_TRACE_SHADER_INSTANCE_BUCKET_COUNT + i]);
        json.Add("]}");
    }
    json.Add("],\"attributed_committed\":%llu,\"attributed_accepted\":%llu,\"hot_committed\":%llu,\"instance_capacity\":%u,\"counter_bits\":32,\"counter_overflow\":%s,\"instance_attribution_complete\":%s",
        (unsigned long long)totalCommitted, (unsigned long long)totalAccepted, (unsigned long long)topCommitted, NRI_TRACE_SHADER_INSTANCE_BUCKET_COUNT,
        counters[127] ? "true" : "false", totalInstances <= NRI_TRACE_SHADER_INSTANCE_BUCKET_COUNT ? "true" : "false");
    if (!json.valid) { NRITraceAuditLoss(capture, rendererFrame, fence, "format_overflow"); return; }
    if (capture.token.epoch == PerfAuditCurrentToken().epoch)
    {
        PerfAuditAdd(PerfAuditMetric::AuditReadbacksPublished);
        if (counters[127]) PerfAuditAdd(PerfAuditMetric::AuditReadbacksOverflow);
    }
    PerfAuditWriteEvent("shader_sample", capture.token, json.data);
}

void NRIAuditEmissiveCensus(const SceneLightSystem& lights)
{
    if (!PerfAuditCensusRequested()) return;
    PerfAuditScope timer(PerfAuditMetric::AuditCensusNs);
    const auto& cache = lights.GetPersistentDynamicEmissiveCache();
    uint64_t logical = 0, capacity = 0, vertices = 0, indices = 0;
    std::array<uint32_t, 4096> owners = {}, ownerRows = {};
    owners.fill(UINT32_MAX);
    uint32_t ownerCount = 0, ownerOverflow = 0, multipleRowOwners = 0, nonActorRows = 0;
    auto countVector = [&](const auto& rows)
    {
        using Row = typename std::decay_t<decltype(rows)>::value_type;
        logical += rows.size() * sizeof(Row);
        capacity += rows.capacity() * sizeof(Row);
    };
    auto countSurfaces = [&](const auto& rows)
    {
        countVector(rows);
        for (const auto& surface : rows)
        {
            countVector(surface.vertices); countVector(surface.indices);
            countVector(surface.primitiveLocalMaterialSlots);
            vertices += surface.vertices.size(); indices += surface.indices.size();
            if (surface.provenance.actorIndex < 0) { nonActorRows++; continue; }
            const uint32_t actor = (uint32_t)surface.provenance.actorIndex;
            bool found = false;
            for (size_t probe = 0; probe < owners.size(); ++probe)
            {
                const size_t slot = (size_t(actor * 2654435761u) + probe) & (owners.size() - 1);
                if (owners[slot] != UINT32_MAX && owners[slot] != actor) continue;
                if (owners[slot] == UINT32_MAX) { owners[slot] = actor; ownerCount++; }
                if (++ownerRows[slot] == 2) multipleRowOwners++;
                found = true;
                break;
            }
            if (!found) ownerOverflow++;
        }
    };
    countSurfaces(cache.sceneView.opaqueWalls);
    countSurfaces(cache.sceneView.opaqueFlats);
    countSurfaces(cache.sceneView.opaqueSprites);
    countVector(cache.geometry.vertices); countVector(cache.geometry.indices);
    countVector(cache.geometry.primitives); countVector(cache.geometry.primitiveProvenance);
    countVector(cache.materialBridge.materials); countVector(cache.materialBridge.lightMetadata); countVector(cache.materialBridge.textures);
    PerfAuditSet(PerfAuditMetric::EmissiveRetainedSurfaces, cache.surfaceCount);
    PerfAuditSet(PerfAuditMetric::EmissiveRetainedVertices, vertices);
    PerfAuditSet(PerfAuditMetric::EmissiveRetainedIndices, indices);
    PerfAuditSet(PerfAuditMetric::EmissiveRetainedMaterialRows, cache.materialBridge.materials.size());
    PerfAuditSet(PerfAuditMetric::EmissiveRetainedBytes, logical);
    PerfAuditSet(PerfAuditMetric::EmissiveRetainedCapacityBytes, capacity);
    PerfAuditSet(PerfAuditMetric::EmissiveRetainedOwners, ownerCount);
    char fields[512];
    snprintf(fields, sizeof(fields), "\"surfaces\":%u,\"actor_owners\":%u,\"owner_complete\":%s,\"owner_overflow_rows\":%u,\"multiple_row_owners\":%u,\"nonactor_rows\":%u,\"logical_bytes\":%llu,\"capacity_bytes\":%llu",
        cache.surfaceCount, ownerCount, ownerOverflow == 0 ? "true" : "false", ownerOverflow, multipleRowOwners, nonActorRows,
        (unsigned long long)logical, (unsigned long long)capacity);
    PerfAuditWriteEvent("emissive_census", PerfAuditCurrentToken(), fields);
}

void NRIRenderDevice::CapturePerfAuditMemoryCensus() const
{
    if (!PerfAuditCensusRequested()) return;
    PerfAuditScope timer(PerfAuditMetric::AuditCensusNs);
    if (mRenderer != nullptr) mRenderer->CapturePerfAuditVoxelCensus();
    const auto adapter = GetAdapterMemoryTelemetry();
    PerfAuditSet(PerfAuditMetric::GpuLocalBudgetBytes, adapter.localBudgetBytes);
    PerfAuditSet(PerfAuditMetric::GpuLocalUsageBytes, adapter.localUsageBytes);
    PerfAuditSet(PerfAuditMetric::GpuNonLocalBudgetBytes, adapter.nonLocalBudgetBytes);
    PerfAuditSet(PerfAuditMetric::GpuNonLocalUsageBytes, adapter.nonLocalUsageBytes);
    const auto renderer = mRenderer != nullptr ? mRenderer->GetMemoryTelemetry() : NRIRenderer::MemoryTelemetry{};
    PerfAuditSet(PerfAuditMetric::GpuTrackedBufferBytes, renderer.sceneBufferBytes);
    PerfAuditSet(PerfAuditMetric::GpuTrackedTextureBytes, renderer.frameTextureBytes + renderer.sceneTextureBytes + renderer.skyTextureBytes);
    PerfAuditSet(PerfAuditMetric::GpuTrackedAsBytes, renderer.accelerationStructureBytes);
    uint64_t retiredBytes = 0, oldestRetireFence = UINT64_MAX;
    for (const auto& row : mRetiredTextureResources)
    {
        retiredBytes += row.resource.memorySize;
        if (row.fenceValue < oldestRetireFence) oldestRetireFence = row.fenceValue;
    }
    if (mRetiredTextureResources.empty()) oldestRetireFence = 0;
    PerfAuditSet(PerfAuditMetric::TextureRetiredCount, mRetiredTextureResources.size());
    PerfAuditSet(PerfAuditMetric::TextureRetiredBytes, retiredBytes);
    char fields[1024];
    snprintf(fields, sizeof(fields),
        "\"renderer_frame\":%llu,\"adapter_live_usage_available\":%s,\"tracked_renderer_subtotal_bytes\":%llu,\"tracked_renderer_complete\":false,\"device_retired_texture_bytes\":%llu,\"device_retired_texture_count\":%llu,\"device_retired_vector_capacity\":%llu,\"shell_texture_bytes\":%llu,\"texture_2d_bytes\":%llu,\"oldest_retire_fence\":%llu,\"recording_fence\":%llu",
        (unsigned long long)mFrameIndex, adapter.liveUsageAvailable ? "true" : "false",
        (unsigned long long)renderer.totalTrackedBytes, (unsigned long long)retiredBytes,
        (unsigned long long)mRetiredTextureResources.size(), (unsigned long long)mRetiredTextureResources.capacity(),
        (unsigned long long)(mSceneTarget.memorySize + mSaveTarget.memorySize), (unsigned long long)mTexture2DDebugStats.residentBytes,
        (unsigned long long)oldestRetireFence, (unsigned long long)GetRecordingCommandFenceValue());
    PerfAuditWriteEvent("renderer_memory", PerfAuditCurrentToken(), fields);
}

void NRIAuditTlasCensus(const std::vector<nri::TopLevelInstance>& instances,
    const std::vector<SceneInstanceData>& records, uint64_t rendererFrame, uint64_t mapEpoch)
{
    if (!PerfAuditCensusRequested()) return;
    PerfAuditScope timer(PerfAuditMetric::AuditCensusNs);
    // Bounded census scratch. Overflow is reported and invalidates uniqueness;
    // no allocation proportional to the level's accumulated instances.
    std::array<uint64_t, 4096> handles = {};
    uint32_t unique = 0, nonOpaque = 0, overflow = 0;
    std::array<uint32_t, 4> sources = {};
    for (const auto& instance : instances)
    {
        nonOpaque += ((uint32_t)instance.flags & (uint32_t)nri::TopLevelInstanceBits::FORCE_NON_OPAQUE) != 0 ? 1u : 0u;
        const uint64_t handle = instance.accelerationStructureHandle;
        if (!handle) { overflow++; continue; }
        const size_t start = size_t((handle ^ (handle >> 32u)) * 0x9e3779b97f4a7c15ull) & (handles.size() - 1);
        bool found = false;
        for (size_t probe = 0; probe < handles.size(); ++probe)
        {
            auto& entry = handles[(start + probe) & (handles.size() - 1)];
            if (entry == handle) { found = true; break; }
            if (!entry) { entry = handle; unique++; found = true; break; }
        }
        if (!found) overflow++;
        const uint32_t id = instance.instanceId;
        const uint32_t source = id < records.size() ? records[id].dataSource : UINT32_MAX;
        sources[source < 3 ? source : 3]++;
    }
    PerfAuditSet(PerfAuditMetric::TraceOccurrences, instances.size());
    PerfAuditSet(PerfAuditMetric::TraceUniqueBlas, unique);
    PerfAuditSet(PerfAuditMetric::TraceNonOpaqueOccurrences, nonOpaque);
    char fields[768];
    snprintf(fields, sizeof(fields),
        "\"renderer_frame\":%llu,\"map_epoch\":%llu,\"occurrences\":%llu,\"unique_blas\":%u,\"unique_complete\":%s,\"unique_overflow\":%u,\"nonopaque\":%u,\"source_occurrences\":[%u,%u,%u,%u],\"cpu_instance_bytes\":%llu,\"cpu_instance_capacity_bytes\":%llu,\"cpu_record_bytes\":%llu,\"cpu_record_capacity_bytes\":%llu",
        (unsigned long long)rendererFrame, (unsigned long long)mapEpoch, (unsigned long long)instances.size(), unique,
        overflow == 0 ? "true" : "false", overflow, nonOpaque, sources[0], sources[1], sources[2], sources[3],
        (unsigned long long)(instances.size() * sizeof(nri::TopLevelInstance)), (unsigned long long)(instances.capacity() * sizeof(nri::TopLevelInstance)),
        (unsigned long long)(records.size() * sizeof(SceneInstanceData)), (unsigned long long)(records.capacity() * sizeof(SceneInstanceData)));
    PerfAuditWriteEvent("tlas_census", PerfAuditCurrentToken(), fields);
}
