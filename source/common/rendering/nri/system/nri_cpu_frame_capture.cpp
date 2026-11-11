#include "nri_cpu_frame_capture.h"
#include "../renderer/nri_renderer.h"
#include "perf_audit.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace
{
    struct Snapshot
    {
        char bytes[12288];
        size_t used = 0;
        bool valid = true;
        void Add(const char* format, ...)
        {
            if (!valid) return;
            va_list args;
            va_start(args, format);
            const int length = vsnprintf(bytes + used, sizeof(bytes) - used, format, args);
            va_end(args);
            if (length < 0 || size_t(length) >= sizeof(bytes) - used) valid = false;
            else used += size_t(length);
        }
        void Time(const char* name, double value)
        {
            if (std::isfinite(value)) Add(",\"%s\":%.6f", name, value);
            else Add(",\"%s\":null", name);
        }
    };
}

void NRICaptureCpuFrame(const NRIRenderer& renderer, uint64_t boundaryFrame, bool rendered)
{
    if (!PerfAuditCpuFrameRequested()) return;
    const uint64_t start = PerfAuditClockNs();
    const auto& shell = renderer.GetLastPerfShellTraceStats();
    const auto& resource = renderer.GetLastPerfResourceTraceStats();
    Snapshot json;
    json.Add("\"snapshot_version\":1,\"renderer_frame\":%llu,\"boundary_frame\":%llu,"
        "\"valid\":%s,\"rendered\":%s,\"census\":%s,\"timing_unit\":\"ms\","
        "\"nested_scopes\":true,\"settings_key\":%llu,\"workload_key\":%llu",
        (unsigned long long)shell.traceRendererFrame, (unsigned long long)boundaryFrame,
        rendered ? "true" : "false", rendered ? "true" : "false",
        PerfAuditCensusRequested() ? "true" : "false",
        (unsigned long long)shell.traceSettingsKey, (unsigned long long)shell.traceWorkloadKey);
#define TIME(name, member) json.Time(name, shell.member)
    TIME("total_ms", totalMs);
    TIME("state_ms", updateStateMs);
    TIME("select_ms", sceneSelectMs);
    TIME("lights_ms", sceneLightsMs);
    TIME("emissive_update_ms", emissiveUpdateMs);
    TIME("frame_graph_ms", frameGraphMs);
    TIME("trace_cpu_ms", traceOpaqueMs);
    TIME("postlight_material_ms", postLightMaterialMs);
    TIME("motion_stage_ms", mapMotionStageMs);
    TIME("post_diagnostics_ms", postFrameDiagnosticsMs);
    TIME("unattributed_ms", unattributedMs);
    TIME("mutation_ms", runtimeMutationMs);
    TIME("mutation_discovery_ms", runtimeMutationDiscoveryMs);
    TIME("mutation_analyze_ms", runtimeMutationAnalyzeMs);
    TIME("mutation_structural_ms", runtimeMutationStructuralRebuildMs);
    TIME("mutation_material_ms", runtimeMutationMaterialRefreshMs);
    TIME("mutation_resident_ms", runtimeMutationResidentApplyMs);
    TIME("mutation_commit_ms", runtimeMutationCommitMs);
    TIME("mutation_atlas_ms", runtimeMutationResidentApplyAtlasMs);
    TIME("mutation_blas_ms", runtimeMutationResidentApplyDownstreamBlasMs);
    TIME("material_build_ms", materialBuildMs);
    TIME("textures_ms", sceneSelectTexturesMs);
    TIME("material_split_ms", sceneSelectMaterialSplitMs);
    TIME("buffer_upload_ms", sceneSelectBufferUploadMs);
    TIME("scene_data_ms", sceneDataSetMs);
    TIME("dynamic_capture_ms", dynamicCaptureMs);
    TIME("voxel_batch_ms", sceneSelectPersistentVoxelBatchMs);
    TIME("voxel_admission_ms", sceneSelectPersistentVoxelAdmissionPumpMs);
    TIME("voxel_actor_loop_ms", persistentVoxelBatchActorLoopMs);
    TIME("voxel_material_variant_ms", persistentVoxelBatchMaterialVariantMs);
    TIME("voxel_material_bridge_ms", persistentVoxelBatchMaterialBridgeMs);
    TIME("dynamic_as_ms", dynamicAsMs);
    TIME("persistent_voxel_as_ms", persistentVoxelAsMs);
    TIME("world_tlas_ms", worldTlasMs);
#undef TIME
    json.Time("resource_wait_ms", resource.waitMs);
    json.Add(",\"mutation_candidates\":%u,\"mutation_analyzed\":%u,\"mutation_sweep\":%u,"
        "\"scene_upload_bytes\":%llu,\"scene_data_upload_bytes\":%llu,"
        "\"scene_upload_calls\":%u,\"scene_data_upload_calls\":%u,"
        "\"resource_growths\":%u,\"resource_overwrites\":%u,\"material_build_ms_by_slot\":[",
        shell.runtimeMutationCandidateChunks, shell.runtimeMutationAnalyzedChunks,
        shell.runtimeMutationBackgroundSweepChunks, (unsigned long long)resource.sceneUploadBytes,
        (unsigned long long)resource.sceneDataUploadBytes, resource.sceneUploadCalls,
        resource.sceneDataUploadCalls, resource.growEvents, resource.overwriteEvents);
    for (size_t i = 0; i < shell.materialBuildByLabel.size(); ++i)
    {
        const double value = shell.materialBuildByLabel[i].materialBuildMs;
        if (std::isfinite(value)) json.Add("%s%.6f", i ? "," : "", value);
        else json.Add("%snull", i ? "," : "");
    }
    json.Add("]");
    if (json.valid)
    {
        PerfAuditWriteEvent("scene_cpu_frame", PerfAuditCurrentToken(), json.bytes);
        PerfAuditAdd(PerfAuditMetric::CpuFrameCaptureRecords);
    }
    else PerfAuditAdd(PerfAuditMetric::AuditEventsDropped);
    PerfAuditAdd(PerfAuditMetric::CpuFrameCaptureNs, PerfAuditClockNs() - start);
}
