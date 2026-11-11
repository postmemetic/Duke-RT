#include "nri_renderdevice.h"
#include "nri_cpu_frame_capture.h"

#include "../renderer/nri_renderer.h"
#include "perf_capture.h"
#include "perf_audit.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace
{
	struct AuditSnapshotJson
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
		void Time(const char* name, double value)
		{
			if (std::isfinite(value)) Add(",\"%s\":%.6f", name, value);
			else Add(",\"%s\":null", name);
		}
		void Write(const char* kind)
		{
			if (valid) PerfAuditWriteEvent(kind, PerfAuditCurrentToken(), data);
			else PerfAuditAdd(PerfAuditMetric::AuditEventsDropped);
		}
	};

	void CaptureAuditSceneSnapshot(const NRIRenderer::PerfShellTraceStats& shell,
		const NRIRenderer::PerfResourceTraceStats& resource, uint64_t boundaryFrame, bool rendered)
	{
		AuditSnapshotJson json;
		json.Add("\"snapshot_version\":2,\"upload_counters_available\":true,\"renderer_frame\":%llu,\"boundary_frame\":%llu,\"valid\":%s,\"rendered\":%s,\"timing_unit\":\"ms\",\"timing_gate\":\"audit_active\",\"nested_scopes\":true,\"settings_key\":%llu,\"workload_key\":%llu",
			(unsigned long long)shell.traceRendererFrame, (unsigned long long)boundaryFrame,
			rendered ? "true" : "false", rendered ? "true" : "false",
			(unsigned long long)shell.traceSettingsKey, (unsigned long long)shell.traceWorkloadKey);
		// Reuse the owners' snapshots. These are CPU scopes, including command
		// recording and any existing waits; GPU execution has its own event.
#define AUDIT_TIME(name, field) json.Time(name, shell.field)
		AUDIT_TIME("total_ms", totalMs);
		AUDIT_TIME("init_ms", initResourcesMs);
		AUDIT_TIME("map_ms", mapWorldMs);
		AUDIT_TIME("state_ms", updateStateMs);
		AUDIT_TIME("select_ms", sceneSelectMs);
		AUDIT_TIME("lights_ms", sceneLightsMs);
		AUDIT_TIME("resident_light_refresh_ms", residentLightRefreshMs);
		AUDIT_TIME("emissive_update_ms", emissiveUpdateMs);
		AUDIT_TIME("emissive_tlas_ms", emissiveTlasMs);
		AUDIT_TIME("frame_graph_ms", frameGraphMs);
		AUDIT_TIME("post_diagnostics_ms", postFrameDiagnosticsMs);
		AUDIT_TIME("unattributed_ms", unattributedMs);
		AUDIT_TIME("static_scene_ms", staticSceneMs);
		AUDIT_TIME("mutation_ms", runtimeMutationMs);
		AUDIT_TIME("mutation_discovery_ms", runtimeMutationDiscoveryMs);
		AUDIT_TIME("mutation_budget_ms", runtimeMutationBudgetMs);
		AUDIT_TIME("mutation_analyze_ms", runtimeMutationAnalyzeMs);
		AUDIT_TIME("mutation_structural_ms", runtimeMutationStructuralRebuildMs);
		AUDIT_TIME("mutation_material_ms", runtimeMutationMaterialRefreshMs);
		AUDIT_TIME("mutation_resident_ms", runtimeMutationResidentApplyMs);
		AUDIT_TIME("mutation_commit_ms", runtimeMutationCommitMs);
		AUDIT_TIME("mutation_geometry_build_ms", geometryBuildRuntimeMutationRebuildMs);
		AUDIT_TIME("mutation_geometry_bridge_ms", runtimeMutationGeometryBridgeMs);
		AUDIT_TIME("mutation_resident_geometry_ms", runtimeMutationResidentApplyGeometryBuildMs);
		AUDIT_TIME("mutation_resident_material_ms", runtimeMutationResidentApplyMaterialBuildMs);
		AUDIT_TIME("mutation_resident_atlas_ms", runtimeMutationResidentApplyAtlasMs);
		AUDIT_TIME("mutation_resident_blas_ms", runtimeMutationResidentApplyDownstreamBlasMs);
		AUDIT_TIME("dynamic_capture_ms", dynamicCaptureMs);
		AUDIT_TIME("voxel_batch_ms", sceneSelectPersistentVoxelBatchMs);
		AUDIT_TIME("voxel_admission_ms", sceneSelectPersistentVoxelAdmissionPumpMs);
		AUDIT_TIME("voxel_cache_entry_ms", persistentVoxelBatchCacheEntryMs);
		AUDIT_TIME("voxel_sort_ms", persistentVoxelBatchSortMs);
		AUDIT_TIME("voxel_instance_sync_ms", persistentVoxelBatchInstanceSyncMs);
		AUDIT_TIME("voxel_existing_actor_map_ms", persistentVoxelBatchExistingActorMapMs);
		AUDIT_TIME("voxel_actor_loop_ms", persistentVoxelBatchActorLoopMs);
		AUDIT_TIME("voxel_material_variant_ms", persistentVoxelBatchMaterialVariantMs);
		AUDIT_TIME("voxel_material_bridge_ms", persistentVoxelBatchMaterialBridgeMs);
		AUDIT_TIME("voxel_mesh_admission_ms", persistentVoxelBatchMeshAdmissionMs);
		AUDIT_TIME("voxel_state_ms", persistentVoxelBatchStateMs);
		AUDIT_TIME("material_bridge_ms", sceneSelectMaterialBridgeMs);
		AUDIT_TIME("material_build_ms", materialBuildMs);
		AUDIT_TIME("actor_override_map_ms", actorOverrideMapBuildMs);
		AUDIT_TIME("textures_ms", sceneSelectTexturesMs);
		AUDIT_TIME("texture_prepare_ms", sceneSelectTexturePrepMs);
		AUDIT_TIME("texture_lookup_ms", sceneTextureLookupMs);
		AUDIT_TIME("texture_realize_ms", sceneTextureRealizeMs);
		AUDIT_TIME("texture_descriptor_ms", sceneTextureDescriptorMs);
		AUDIT_TIME("texture_transition_ms", sceneTextureTransitionMs);
		AUDIT_TIME("buffer_upload_ms", sceneSelectBufferUploadMs);
		AUDIT_TIME("buffer_upload_primitive_rewrite_ms", sceneSelectBufferUploadPrimitiveRewriteMs);
		AUDIT_TIME("buffer_upload_hash_ms", sceneSelectBufferUploadPayloadHashMs);
		AUDIT_TIME("buffer_upload_dirty_range_ms", sceneSelectBufferUploadDirtyRangeMs);
		AUDIT_TIME("buffer_upload_wait_ms", sceneSelectBufferUploadWaitMs);
		AUDIT_TIME("buffer_upload_vertex_ms", sceneSelectBufferUploadVertexMs);
		AUDIT_TIME("buffer_upload_index_ms", sceneSelectBufferUploadIndexMs);
		AUDIT_TIME("buffer_upload_primitive_ms", sceneSelectBufferUploadPrimitiveMs);
		AUDIT_TIME("buffer_upload_material_ms", sceneSelectBufferUploadMaterialMs);
		AUDIT_TIME("buffer_upload_voxel_material_ms", sceneSelectBufferUploadPersistentVoxelMaterialMs);
		AUDIT_TIME("persistent_voxel_as_ms", persistentVoxelAsMs);
		AUDIT_TIME("dynamic_as_ms", dynamicAsMs);
		AUDIT_TIME("world_tlas_ms", worldTlasMs);
		AUDIT_TIME("scene_data_ms", sceneDataSetMs);
		AUDIT_TIME("state_commit_ms", sceneSelectStateCommitMs);
		AUDIT_TIME("persistent_emissive_ms", sceneSelectPersistentEmissiveMs);
		AUDIT_TIME("dynamic_merge_ms", sceneSelectDynamicMergeMs);
		AUDIT_TIME("dynamic_merge_copy_ms", sceneSelectDynamicMergeCopyMs);
		AUDIT_TIME("dynamic_merge_geometry_ms", sceneSelectDynamicMergeGeometryMs);
		AUDIT_TIME("dynamic_merge_material_ms", sceneSelectDynamicMergeMaterialMs);
		AUDIT_TIME("light_static_append_ms", sceneLightStaticAppendMs);
		AUDIT_TIME("light_mutation_append_ms", sceneLightRuntimeMutationAppendMs);
		AUDIT_TIME("light_captured_append_ms", sceneLightCapturedAppendMs);
		AUDIT_TIME("light_dynamic_append_ms", sceneLightDynamicAppendMs);
		AUDIT_TIME("light_voxel_append_ms", sceneLightPersistentVoxelAppendMs);
		AUDIT_TIME("light_analytic_ms", sceneLightAnalyticMs);
		AUDIT_TIME("light_emissive_ms", sceneLightEmissiveMs);
		AUDIT_TIME("light_sector_ms", sceneLightSectorMs);
		AUDIT_TIME("light_topology_sort_ms", sceneLightTopologySortMs);
		AUDIT_TIME("trace_cpu_ms", traceOpaqueMs);
		AUDIT_TIME("trace_readback_ms", traceOpaqueReadbackMs);
		AUDIT_TIME("trace_command_ms", traceOpaqueCommandMs);
		AUDIT_TIME("trace_stats_copy_ms", traceOpaqueStatsCopyMs);
#undef AUDIT_TIME
		json.Time("resource_wait_ms", resource.waitMs);
		json.Time("scene_buffer_wait_ms", resource.sceneBufferUploadWaitMs);
		json.Time("resident_chunk_write_wait_ms", resource.residentChunkWriteWaitMs);
		json.Time("resident_chunk_blas_wait_ms", resource.residentChunkBlasRebuildWaitMs);
		json.Time("scene_data_wait_ms", resource.sceneDataUploadWaitMs);
		json.Time("emissive_upload_wait_ms", resource.emissiveSamplingUploadWaitMs);
		json.Add(",\"wait_calls\":%u,\"scene_upload_bytes\":%llu,\"scene_data_upload_bytes\":%llu,\"emissive_upload_bytes\":%llu,\"resource_growths\":%u,\"resource_overwrites\":%u",
			resource.waitCalls, (unsigned long long)resource.sceneUploadBytes, (unsigned long long)resource.sceneDataUploadBytes,
			(unsigned long long)resource.emissiveUploadBytes, resource.growEvents, resource.overwriteEvents);
		json.Add(",\"scene_upload_calls\":%u,\"scene_data_upload_calls\":%u,\"emissive_upload_calls\":%u,\"scene_vertex_upload_bytes\":%llu,\"scene_index_upload_bytes\":%llu,\"scene_primitive_upload_bytes\":%llu,\"scene_material_upload_bytes\":%llu",
			resource.sceneUploadCalls, resource.sceneDataUploadCalls, resource.emissiveUploadCalls,
			(unsigned long long)resource.sceneVertexUploadBytes, (unsigned long long)resource.sceneIndexUploadBytes,
			(unsigned long long)resource.scenePrimitiveUploadBytes, (unsigned long long)resource.sceneMaterialUploadBytes);
		json.Add(",\"scene_dynamic_upload_bytes\":%llu,\"scene_resident_chunk_upload_bytes\":%llu,\"scene_persistent_voxel_upload_bytes\":%llu,\"scene_persistent_voxel_variant_upload_bytes\":%llu,\"scene_static_refresh_upload_bytes\":%llu,\"scene_other_upload_bytes\":%llu,\"material_build_ms_by_slot\":[",
			(unsigned long long)resource.sceneDynamicUploadBytes, (unsigned long long)resource.sceneResidentChunkUploadBytes,
			(unsigned long long)resource.scenePersistentVoxelUploadBytes, (unsigned long long)resource.scenePersistentVoxelVariantUploadBytes,
			(unsigned long long)resource.sceneStaticRefreshUploadBytes, (unsigned long long)resource.sceneOtherUploadBytes);
		for (size_t slot = 0; slot < shell.materialBuildByLabel.size(); ++slot)
		{
			const double value = shell.materialBuildByLabel[slot].materialBuildMs;
			if (std::isfinite(value)) json.Add("%s%.6f", slot ? "," : "", value);
			else json.Add("%snull", slot ? "," : "");
		}
		json.Add("],\"basic_counts_available\":%s,\"composition_counts_available\":%s,\"texture_inner_trace_times_available\":true",
			shell.successDiagnosticsBasicCollected ? "true" : "false",
			shell.successDiagnosticsInstanceCompositionCollected ? "true" : "false");
		if (shell.successDiagnosticsBasicCollected)
			json.Add(",\"active_primitives\":%u,\"active_materials\":%u,\"scene_instances\":%u",
				shell.activePrimitiveCount, shell.activeMaterialCount, shell.sceneInstanceCount);
		else json.Add(",\"active_primitives\":null,\"active_materials\":null,\"scene_instances\":null");
		json.Write("scene_cpu");
	}
}

void NRIRenderDevice::CaptureCompactPerfRendererStats(bool rendered)
{
	const PerfCompactCaptureToken token = PerfCompactCaptureGetCurrentToken();
	const bool audit = PerfAuditCensusRequested();
	const bool cpuFrame = PerfAuditCpuFrameRequested();
	if ((!token && !audit && !cpuFrame) || mRenderer == nullptr) return;

	const auto& shell = mRenderer->GetLastPerfShellTraceStats();
	const auto& resource = mRenderer->GetLastPerfResourceTraceStats();
	if (audit) CaptureAuditSceneSnapshot(shell, resource, mLastFrameBoundaryStats.frameNumber, rendered);
	if (cpuFrame) NRICaptureCpuFrame(*mRenderer, mLastFrameBoundaryStats.frameNumber, rendered);
	if (!token) return;
	PerfCompactNriStats stats = {};
	stats.frame = mLastFrameBoundaryStats.frameNumber;
	stats.traceRendererFrame = shell.traceRendererFrame;
	stats.traceSettingsKey = shell.traceSettingsKey;
	stats.traceWorkloadKey = shell.traceWorkloadKey;
	stats.traceSettingsKeyWithoutStaticTangent = shell.traceSettingsKeyWithoutStaticTangent;
	stats.traceWorkloadKeyWithoutStaticTangent = shell.traceWorkloadKeyWithoutStaticTangent;
	stats.traceStaticTangentRequested = shell.traceStaticTangentRequested;
	stats.traceStaticTangentActive = shell.traceStaticTangentActive;
	stats.traceAux1 = shell.traceAux1;
	stats.totalMs = shell.totalMs;
	stats.initMs = shell.initResourcesMs;
	stats.mapMs = shell.mapWorldMs;
	stats.stateMs = shell.updateStateMs;
	stats.selectMs = shell.sceneSelectMs;
	stats.lightsMs = shell.sceneLightsMs;
	stats.frameGraphMs = shell.frameGraphMs;
	stats.postDiagnosticsMs = shell.postFrameDiagnosticsMs;
	stats.unattributedMs = shell.unattributedMs;
	stats.mutationMs = shell.runtimeMutationMs;
	stats.mutationDiscoveryMs = shell.runtimeMutationDiscoveryMs;
	stats.mutationBudgetMs = shell.runtimeMutationBudgetMs;
	stats.mutationAnalyzeMs = shell.runtimeMutationAnalyzeMs;
	stats.mutationStructuralMs = shell.runtimeMutationStructuralRebuildMs;
	stats.mutationMaterialMs = shell.runtimeMutationMaterialRefreshMs;
	stats.mutationResidentMs = shell.runtimeMutationResidentApplyMs;
	stats.mutationCommitMs = shell.runtimeMutationCommitMs;
	stats.mutationGeometryBuildMs = shell.geometryBuildRuntimeMutationRebuildMs;
	stats.mutationGeometryBridgeMs = shell.runtimeMutationGeometryBridgeMs;
	stats.mutationPortalAssignMs = shell.runtimeMutationPortalAssignMs;
	stats.mutationDeformerCanonicalMs = shell.runtimeMutationDeformerCanonicalMs;
	stats.mutationMaterialBuildMs = shell.materialBuildByLabel[
		(size_t)NRIRenderer::MaterialBuildTraceSlot::RuntimeMutationChunk].materialBuildMs;
	stats.dynamicCaptureMs = shell.dynamicCaptureMs;
	stats.persistentBatchMs = shell.sceneSelectPersistentVoxelBatchMs;
	stats.voxelAdmissionPumpMs = shell.sceneSelectPersistentVoxelAdmissionPumpMs;
	stats.voxelBatchCacheEntryMs = shell.persistentVoxelBatchCacheEntryMs;
	stats.voxelBatchSortMs = shell.persistentVoxelBatchSortMs;
	stats.voxelBatchInstanceSyncMs = shell.persistentVoxelBatchInstanceSyncMs;
	stats.voxelBatchExistingActorMapMs = shell.persistentVoxelBatchExistingActorMapMs;
	stats.voxelBatchActorLoopMs = shell.persistentVoxelBatchActorLoopMs;
	stats.voxelBatchMaterialVariantMs = shell.persistentVoxelBatchMaterialVariantMs;
	stats.voxelBatchMeshAdmissionMs = shell.persistentVoxelBatchMeshAdmissionMs;
	stats.voxelBatchMaterialBridgeMs = shell.persistentVoxelBatchMaterialBridgeMs;
	stats.voxelBatchStateMs = shell.persistentVoxelBatchStateMs;
	stats.materialBridgeMs = shell.sceneSelectMaterialBridgeMs;
	stats.texturesMs = shell.sceneSelectTexturesMs;
	stats.bufferUploadMs = shell.sceneSelectBufferUploadMs;
	stats.persistentVoxelAsMs = shell.persistentVoxelAsMs;
	stats.dynamicAsMs = shell.dynamicAsMs;
	stats.worldTlasMs = shell.worldTlasMs;
	stats.sceneDataMs = shell.sceneDataSetMs;
	stats.stateCommitMs = shell.sceneSelectStateCommitMs;
	stats.resourceWaitMs = resource.waitMs;
	stats.sceneUploadBytes = resource.sceneUploadBytes;
	stats.resourceWaitCalls = resource.waitCalls;
	stats.activePrimitives = shell.activePrimitiveCount;
	stats.dynamicPrimitives = shell.dynamicPrimitiveCount;
	stats.activeMaterials = shell.activeMaterialCount;
	stats.sceneInstances = shell.sceneInstanceCount;
	stats.mutationStructural = shell.runtimeMutationValidStructuralCount;
	stats.mutationMaterial = shell.runtimeMutationValidMaterialCount;
	stats.mutationResident = shell.runtimeMutationResidentApplyCount;
	stats.mutationCandidates = shell.runtimeMutationCandidateChunks;
	stats.mutationAnalyzed = shell.runtimeMutationAnalyzedChunks;
	stats.mutationSweep = shell.runtimeMutationBackgroundSweepChunks;
	stats.mutationCandidateActive = shell.runtimeMutationCandidateActiveReplacementChunks;
	stats.mutationCandidateVisible = shell.runtimeMutationCandidateVisibleResidentValidationChunks;
	stats.mutationCandidateStartupVisible = shell.runtimeMutationCandidateStartupVisibleValidationChunks;
	stats.mutationCandidateUnresolved = shell.runtimeMutationCandidateUnresolvedTextureChunks;
	stats.mutationCandidateStaticAnimated = shell.runtimeMutationCandidateStaticAnimatedSuppressedChunks;
	stats.mutationCandidateSectorDirty = shell.runtimeMutationCandidateSectorDirtyChunks;
	stats.mutationCandidateSectionDirty = shell.runtimeMutationCandidateSectionDirtyChunks;
	stats.mutationCandidateDragged = shell.runtimeMutationCandidateDraggedChunks;
	stats.mutationCandidateSignatureWatch = shell.runtimeMutationCandidateSignatureWatchChunks;
	stats.mutationCandidateDeferredMaterial = shell.runtimeMutationCandidateDeferredMaterialChunks;
	stats.mutationCandidateDeferredStructural = shell.runtimeMutationCandidateDeferredStructuralChunks;
	for (uint32_t index = 0; index < 4u; ++index)
	{
		const auto& entry = shell.runtimeStructuralRebuildEntries[index];
		if (!entry.valid)
		{
			continue;
		}
		stats.mutationStructuralChunk[index] = entry.chunkIndex;
		stats.mutationStructuralReason[index] = entry.reasonMask;
		stats.mutationStructuralTrigger[index] = entry.triggerMask;
	}
	stats.voxelPressureReason = shell.persistentVoxelPressureReason;
	stats.voxelPressureEntries = shell.persistentVoxelPressureAdmissionRows;
	stats.voxelPressureResources = shell.persistentVoxelPressureResourceRows;
	stats.voxelPressureFlags = shell.persistentVoxelPressureFlags;
	stats.traceRenderWidth = shell.traceRenderWidth;
	stats.traceRenderHeight = shell.traceRenderHeight;
	stats.traceOutputWidth = shell.traceOutputWidth;
	stats.traceOutputHeight = shell.traceOutputHeight;
	stats.traceDispatchX = shell.traceOpaqueDispatchX;
	stats.traceDispatchY = shell.traceOpaqueDispatchY;
	stats.traceDispatchZ = shell.traceOpaqueDispatchZ;
	stats.traceLightBounces = shell.traceLightBounceCount;
	stats.traceMirrorBounces = shell.traceMirrorBounceCount;
	stats.tracePortalDepth = shell.tracePortalDepth;
	stats.traceEmissiveSamples = shell.traceEmissiveSampleCount;
	stats.traceEmissiveRequestedSamples = shell.traceEmissiveRequestedSampleCount;
	stats.traceEmissivePrimaryBudget = shell.traceEmissivePrimarySampleBudget;
	stats.traceIndirectSamplingRequestedMode = shell.traceIndirectSamplingRequestedMode;
	stats.traceIndirectSamplingEffectiveMode = shell.traceIndirectSamplingEffectiveMode;
	stats.traceIndirectSamplingActiveMode = shell.traceIndirectSamplingActiveMode;
	stats.traceHitDistanceReconstructionMode = shell.traceHitDistanceReconstructionMode;
	stats.traceRuntimeLights = shell.traceRuntimeLightCount;
	stats.traceRuntimeLightTilesX = shell.traceRuntimeLightTileCountX;
	stats.traceRuntimeLightTilesY = shell.traceRuntimeLightTileCountY;
	stats.traceRuntimeLightTileSize = shell.traceRuntimeLightTileSize;
	stats.traceRuntimeLightTileIndices = shell.traceRuntimeLightTileIndexCount;
	stats.traceRuntimeLightMaxOccupancy = shell.traceRuntimeLightMaxTileOccupancy;
	stats.traceRuntimeLightShadowBudget = shell.traceRuntimeLightShadowBudget;
	stats.traceRuntimeLightShadowCandidates = shell.traceRuntimeLightShadowCandidateReferenceCount;
	stats.traceRuntimeLightShadowSelected = shell.traceRuntimeLightShadowSelectedReferenceCount;
	stats.traceRuntimeLightShadowOverflow = shell.traceRuntimeLightShadowOverflowReferenceCount;
	stats.traceRuntimeLightShadowTileMax = shell.traceRuntimeLightMaxShadowCandidatesPerTile;
	stats.traceRuntimeLightShadowSelectedTileMax = shell.traceRuntimeLightMaxShadowSelectedPerTile;
	stats.traceRuntimeLightShadowSelectionHash = shell.traceRuntimeLightShadowSelectionHash;
	stats.traceRuntimeLightShadowRetained = shell.traceRuntimeLightShadowRetainedReferenceCount;
	stats.traceRuntimeLightShadowReplaced = shell.traceRuntimeLightShadowReplacedReferenceCount;
	stats.traceRuntimeLightShadowExpired = shell.traceRuntimeLightShadowExpiredReferenceCount;
	stats.traceRuntimeLightShadowRetainedKeyHash = shell.traceRuntimeLightShadowRetainedKeyHash;
	stats.traceRuntimeLightShadowReplacedKeyHash = shell.traceRuntimeLightShadowReplacedKeyHash;
	stats.traceRuntimeLightShadowExpiredKeyHash = shell.traceRuntimeLightShadowExpiredKeyHash;
	stats.traceEmissivePrimitiveCount = shell.traceEmissivePrimitiveCount;
	stats.traceEmissiveTotalPower = shell.traceEmissiveTotalPower;
	stats.traceFlags = shell.traceFlags;
	stats.traceDebugMode = shell.traceDebugMode;
	stats.traceBootstrapMode = shell.traceBootstrapMode;
	stats.traceUpscalerKind = shell.traceUpscalerKind;
	stats.traceUpscalerMode = shell.traceUpscalerMode;
	stats.traceDenoiserMode = shell.traceDenoiserMode;
	stats.traceDirectScene = shell.traceDirectScene;
	stats.traceDirectional = shell.traceDirectional;
	stats.traceDirectionalShadow = shell.traceDirectionalShadow;
	stats.traceSplitShadow = shell.traceSplitShadow;
	stats.traceFastEmissiveShadow = shell.traceFastEmissiveShadow;
	stats.traceVisibleChunkGate = shell.traceVisibleChunkGate;
	stats.traceVoxelOccurrences = shell.sceneInstancePersistentVoxelCount;
	stats.traceVoxelOccurrenceControl = shell.traceVoxelOccurrenceControl;
	stats.traceVoxelInstancePrimitives = shell.persistentVoxelInstancePrimitiveCount;
	stats.rendered = rendered;
	stats.valid = true;
	PerfCompactCaptureNoteNri(token, stats);
}

void NRIRenderDevice::CaptureCompactPerfFrameBoundary(bool presentOk)
{
	if (PerfAuditActive())
	{
		AuditSnapshotJson json;
		json.Add("\"renderer_frame\":%llu,\"valid\":true,\"path_traced\":%s,\"present_ok\":%s,\"timing_unit\":\"ms\",\"timing_gate\":\"always_collected\"",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			mLastFrameBoundaryStats.pathTracedSceneRendered ? "true" : "false", presentOk ? "true" : "false");
		json.Time("wait_ms", mLastFrameBoundaryStats.waitMs);
		json.Time("wait_present_ms", mLastFrameBoundaryStats.waitForPresentMs);
		json.Time("acquire_ms", mLastFrameBoundaryStats.acquireMs);
		json.Time("submit_ms", mLastFrameBoundaryStats.submitMs);
		json.Time("present_ms", mLastFrameBoundaryStats.presentMs);
		json.Write("frame_boundary");
	}
	const PerfCompactCaptureToken token = PerfCompactCaptureGetCurrentToken();
	if (!token) return;
	PerfCompactBoundaryStats stats = {};
	stats.frame = mLastFrameBoundaryStats.frameNumber;
	stats.waitMs = mLastFrameBoundaryStats.waitMs;
	stats.waitPresentMs = mLastFrameBoundaryStats.waitForPresentMs;
	stats.acquireMs = mLastFrameBoundaryStats.acquireMs;
	stats.submitMs = mLastFrameBoundaryStats.submitMs;
	stats.presentMs = mLastFrameBoundaryStats.presentMs;
	stats.pathTraced = mLastFrameBoundaryStats.pathTracedSceneRendered;
	stats.presentOk = presentOk;
	stats.valid = true;
	PerfCompactCaptureNoteBoundary(token, stats);
}
