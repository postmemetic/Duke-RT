#include "nri_renderdevice.h"
#include "nri_gpu_timing.h"
#include "../renderer/nri_cvars.h"
#include "../renderer/nri_diagnostic_cadence.h"

#include "../framegen/nri_framegen.h"
#include "../renderer/nri_renderer.h"
#include "../renderer/nri_renderstate.h"
#include "nri_hwbuffer.h"
#include "nri_hwtexture.h"
#include "c_cvars.h"
#include "cmdlib.h"
#include "d_eventbase.h"
#include "i_mainwindow.h"
#include "i_time.h"
#include "printf.h"
#include "textures.h"
#include "v_2ddrawer.h"
#include "v_draw.h"
#include "version.h"
#include "hw_drawinfo.h"
#include "hwrenderer/data/hw_clock.h"
#include "hwrenderer/data/hw_viewpointbuffer.h"
#include "flatvertices.h"
#include "hw_skydome.h"
#include "hw_lightbuffer.h"
#include "hw_bonebuffer.h"
#include "coreplayer.h"
#include "coreactor.h"
#include "gamecontrol.h"
#include "gameupdate.h"
#include "lightoverlay.h"
#include "startup_recovery.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#ifdef ERROR
#undef ERROR
#endif

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <utility>

extern int gametic;





namespace
{
	static int64_t DeltaUnsigned(uint64_t current, uint64_t previous)
	{
		return current >= previous ? (int64_t)(current - previous) : -(int64_t)(previous - current);
	}

	static const char* GetProgressiveRuntimeMutationTraceActionName(NRIRenderer::RuntimeMutationTraceAction action)
	{
		switch (action)
		{
		case NRIRenderer::RuntimeMutationTraceAction::StructuralRebuild: return "rebuild";
		case NRIRenderer::RuntimeMutationTraceAction::MaterialRefresh: return "material-refresh";
		case NRIRenderer::RuntimeMutationTraceAction::ResidentApply: return "resident-apply";
		case NRIRenderer::RuntimeMutationTraceAction::ResidentNoopSkip: return "resident-noop-skip";
		case NRIRenderer::RuntimeMutationTraceAction::ResidentFallback: return "fallback";
		case NRIRenderer::RuntimeMutationTraceAction::Held: return "held";
		case NRIRenderer::RuntimeMutationTraceAction::SyncSkip: return "sync-skip";
		case NRIRenderer::RuntimeMutationTraceAction::DeferredMaterialRefresh: return "deferred-material-refresh";
		case NRIRenderer::RuntimeMutationTraceAction::Failed: return "failed";
		default: return "none";
		}
	}

	static const char* GetProgressiveSceneDataSourceName(uint32_t dataSource)
	{
		switch (dataSource)
		{
		case 0: return "static";
		case 1: return "dynamic";
		case 2: return "persistent_voxel";
		default: return "unknown";
		}
	}

	static float DecodeSpatialAbsenceProbeFloat(uint32_t bits)
	{
		float value = 0.0f;
		static_assert(sizeof(value) == sizeof(bits));
		std::memcpy(&value, &bits, sizeof(value));
		return value;
	}

	static void EmitSpatialAbsenceProbeTrace(
		uint64_t frameNumber,
		const NRIRenderer::PerfTraceShaderStats& shader)
	{
		if (!nri_pt360absenceprobe || !shader.valid)
		{
			return;
		}

		const auto& counters = shader.counters;
		const auto rayBase = [](uint32_t rayKind)
		{
			return NRI_TRACE_SHADER_PROBE_RAY_BASE + rayKind * NRI_TRACE_SHADER_PROBE_RAY_STRIDE;
		};
		const auto sumRayField = [&](uint32_t field)
		{
			uint64_t total = 0;
			for (uint32_t rayKind = 0; rayKind < NRI_TRACE_SHADER_RAY_KIND_COUNT; ++rayKind)
			{
				total += counters[rayBase(rayKind) + field];
			}
			return total;
		};
		const auto sumOutcome = [&](uint32_t outcome)
		{
			return sumRayField(NRI_TRACE_SHADER_PROBE_RAY_OUTCOME_BASE + outcome);
		};
		const uint32_t targetBase = NRI_TRACE_SHADER_PROBE_TARGET_PIXEL_BASE;
		const uint32_t referenceBase = NRI_TRACE_SHADER_PROBE_REFERENCE_PIXEL_BASE;
		const uint32_t targetValid = counters[targetBase];
		const uint32_t referenceValid = counters[referenceBase];
		const uint32_t invalidId = UINT32_MAX;
		const auto pixelId = [&](uint32_t base, uint32_t offset, uint32_t valid)
		{
			return valid != 0 ? counters[base + offset] : invalidId;
		};
		const auto pixelPosition = [&](uint32_t base, uint32_t offset, uint32_t valid)
		{
			return valid != 0 ? DecodeSpatialAbsenceProbeFloat(counters[base + offset]) : 0.0f;
		};

		std::ostringstream line;
		line << std::fixed << std::setprecision(6);
		line << "PERF pt shader 360 absence probe NRI:"
			<< " frame=" << frameNumber
			<< " stats_frame=" << shader.frameNumber
			<< " enabled=1"
			<< " expected_chunk=" << ((int)nri_pt360absenceprobechunk >= 0 ? (uint32_t)(int)nri_pt360absenceprobechunk : invalidId)
			<< " calls=" << sumRayField(NRI_TRACE_SHADER_PROBE_RAY_CALLS)
			<< " disabled=" << sumOutcome(NRI_SPATIAL_ABSENCE_PROBE_DISABLED)
			<< " snapshot_invalid=" << sumOutcome(NRI_SPATIAL_ABSENCE_PROBE_SNAPSHOT_INVALID)
			<< " frame_mismatch=" << sumOutcome(NRI_SPATIAL_ABSENCE_PROBE_FRAME_MISMATCH)
			<< " outside_guard=" << sumOutcome(NRI_SPATIAL_ABSENCE_PROBE_OUTSIDE_GUARD)
			<< " lookup_miss=" << sumOutcome(NRI_SPATIAL_ABSENCE_PROBE_LOOKUP_INVALID)
			<< " outside_union=" << sumOutcome(NRI_SPATIAL_ABSENCE_PROBE_OUTSIDE_UNION)
			<< " exact_miss=" <<
				(sumOutcome(NRI_SPATIAL_ABSENCE_PROBE_NEGATIVE_FOOTPRINT_MISS) +
					sumOutcome(NRI_SPATIAL_ABSENCE_PROBE_PAIR_BOUNDS_MISS) +
					sumOutcome(NRI_SPATIAL_ABSENCE_PROBE_POSITIVE_FOOTPRINT_MISS));
		static constexpr const char* rayNames[NRI_TRACE_SHADER_RAY_KIND_COUNT] = {
			"primary", "ungated", "sun", "point", "emissive", "fast_emissive"
		};
		for (uint32_t rayKind = 0; rayKind < NRI_TRACE_SHADER_RAY_KIND_COUNT; ++rayKind)
		{
			line << " reject_" << rayNames[rayKind] << "=" <<
				counters[rayBase(rayKind) + NRI_TRACE_SHADER_PROBE_RAY_OUTCOME_BASE + NRI_SPATIAL_ABSENCE_PROBE_REJECT];
		}
		line << " target_valid=" << targetValid
			<< " target_source=" << pixelId(targetBase, 1u, targetValid)
			<< " target_instance=" << pixelId(targetBase, 2u, targetValid)
			<< " target_primitive=" << pixelId(targetBase, 3u, targetValid)
			<< " target_chunk=" << pixelId(targetBase, 4u, targetValid)
			<< " target_x=" << pixelPosition(targetBase, 5u, targetValid)
			<< " target_y=" << pixelPosition(targetBase, 6u, targetValid)
			<< " target_z=" << pixelPosition(targetBase, 7u, targetValid)
			<< " target_identity_lo=" << shader.targetPixelAttribution.metadata0
			<< " target_identity_hi=" << shader.targetPixelAttribution.metadata1
			<< " target_generation=" << shader.targetPixelAttribution.metadata2
			<< " reference_valid=" << referenceValid
			<< " reference_source=" << pixelId(referenceBase, 1u, referenceValid)
			<< " reference_instance=" << pixelId(referenceBase, 2u, referenceValid)
			<< " reference_primitive=" << pixelId(referenceBase, 3u, referenceValid)
			<< " reference_chunk=" << pixelId(referenceBase, 4u, referenceValid)
			<< " reference_x=" << pixelPosition(referenceBase, 5u, referenceValid)
			<< " reference_y=" << pixelPosition(referenceBase, 6u, referenceValid)
			<< " reference_z=" << pixelPosition(referenceBase, 7u, referenceValid);
		line << " reference_identity_lo=" << shader.referencePixelAttribution.metadata0
			<< " reference_identity_hi=" << shader.referencePixelAttribution.metadata1
			<< " reference_generation=" << shader.referencePixelAttribution.metadata2;

		for (uint32_t rayKind = 0; rayKind < NRI_TRACE_SHADER_RAY_KIND_COUNT; ++rayKind)
		{
			const uint32_t base = rayBase(rayKind);
			const char* name = rayNames[rayKind];
			const uint32_t candidateValid = counters[base + NRI_TRACE_SHADER_PROBE_RAY_CANDIDATE_CLAIM];
			const uint32_t finalValid = counters[base + NRI_TRACE_SHADER_PROBE_RAY_FINAL_VALID];
			const auto candidateId = [&](uint32_t offset)
			{
				return candidateValid != 0 ? counters[base + offset] : invalidId;
			};
			const auto candidatePosition = [&](uint32_t offset)
			{
				return candidateValid != 0 ? DecodeSpatialAbsenceProbeFloat(counters[base + offset]) : 0.0f;
			};
			const auto finalId = [&](uint32_t offset)
			{
				return finalValid != 0 ? counters[base + offset] : invalidId;
			};
			const auto finalPosition = [&](uint32_t offset)
			{
				return finalValid != 0 ? DecodeSpatialAbsenceProbeFloat(counters[base + offset]) : 0.0f;
			};
			line << " " << name << "_calls=" << counters[base + NRI_TRACE_SHADER_PROBE_RAY_CALLS]
				<< " " << name << "_candidates=" << counters[base + NRI_TRACE_SHADER_PROBE_RAY_CANDIDATES];
			static constexpr const char* outcomeNames[NRI_SPATIAL_ABSENCE_PROBE_OUTCOME_COUNT] = {
				"disabled", "snapshot_invalid", "frame_mismatch", "outside_guard", "lookup_invalid",
				"outside_union", "negative_footprint_miss", "pair_bounds_miss", "positive_footprint_miss", "reject"
			};
			for (uint32_t outcome = 0; outcome < NRI_SPATIAL_ABSENCE_PROBE_OUTCOME_COUNT; ++outcome)
			{
				line << " " << name << "_" << outcomeNames[outcome] << "=" <<
					counters[base + NRI_TRACE_SHADER_PROBE_RAY_OUTCOME_BASE + outcome];
			}
			line << " " << name << "_candidate_valid=" << candidateValid
				<< " " << name << "_candidate_outcome=" << candidateId(NRI_TRACE_SHADER_PROBE_RAY_CANDIDATE_OUTCOME)
				<< " " << name << "_candidate_source=" << candidateId(NRI_TRACE_SHADER_PROBE_RAY_CANDIDATE_SOURCE)
				<< " " << name << "_candidate_instance=" << candidateId(NRI_TRACE_SHADER_PROBE_RAY_CANDIDATE_INSTANCE)
				<< " " << name << "_candidate_primitive=" << candidateId(NRI_TRACE_SHADER_PROBE_RAY_CANDIDATE_PRIMITIVE)
				<< " " << name << "_candidate_chunk=" << candidateId(NRI_TRACE_SHADER_PROBE_RAY_CANDIDATE_CHUNK)
				<< " " << name << "_candidate_x=" << candidatePosition(NRI_TRACE_SHADER_PROBE_RAY_CANDIDATE_POSITION_X)
				<< " " << name << "_candidate_y=" << candidatePosition(NRI_TRACE_SHADER_PROBE_RAY_CANDIDATE_POSITION_Y)
				<< " " << name << "_candidate_z=" << candidatePosition(NRI_TRACE_SHADER_PROBE_RAY_CANDIDATE_POSITION_Z)
				<< " " << name << "_matched_positive_chunk=" << candidateId(NRI_TRACE_SHADER_PROBE_RAY_MATCHED_POSITIVE_CHUNK)
				<< " " << name << "_footprint_stage=" << candidateId(NRI_TRACE_SHADER_PROBE_RAY_FOOTPRINT_STAGE)
				<< " " << name << "_footprint_cell_references=" << candidateId(NRI_TRACE_SHADER_PROBE_RAY_FOOTPRINT_CELL_REFERENCES)
				<< " " << name << "_footprint_best_margin=" << candidatePosition(NRI_TRACE_SHADER_PROBE_RAY_FOOTPRINT_BEST_MARGIN)
				<< " " << name << "_footprint_best_triangle=" << candidateId(NRI_TRACE_SHADER_PROBE_RAY_FOOTPRINT_BEST_TRIANGLE)
				<< " " << name << "_candidate_material_flags=" << candidateId(NRI_TRACE_SHADER_PROBE_RAY_CANDIDATE_MATERIAL_FLAGS)
				<< " " << name << "_candidate_identity_lo=" << shader.candidateAttribution[rayKind].metadata0
				<< " " << name << "_candidate_identity_hi=" << shader.candidateAttribution[rayKind].metadata1
				<< " " << name << "_candidate_generation=" << shader.candidateAttribution[rayKind].metadata2
				<< " " << name << "_final_valid=" << finalValid
				<< " " << name << "_final_source=" << finalId(NRI_TRACE_SHADER_PROBE_RAY_FINAL_SOURCE)
				<< " " << name << "_final_instance=" << finalId(NRI_TRACE_SHADER_PROBE_RAY_FINAL_INSTANCE)
				<< " " << name << "_final_primitive=" << finalId(NRI_TRACE_SHADER_PROBE_RAY_FINAL_PRIMITIVE)
				<< " " << name << "_final_chunk=" << finalId(NRI_TRACE_SHADER_PROBE_RAY_FINAL_CHUNK)
				<< " " << name << "_final_x=" << finalPosition(NRI_TRACE_SHADER_PROBE_RAY_FINAL_POSITION_X)
				<< " " << name << "_final_y=" << finalPosition(NRI_TRACE_SHADER_PROBE_RAY_FINAL_POSITION_Y)
				<< " " << name << "_final_z=" << finalPosition(NRI_TRACE_SHADER_PROBE_RAY_FINAL_POSITION_Z);
			line << " " << name << "_final_identity_lo=" << shader.finalAttribution[rayKind].metadata0
				<< " " << name << "_final_identity_hi=" << shader.finalAttribution[rayKind].metadata1
				<< " " << name << "_final_generation=" << shader.finalAttribution[rayKind].metadata2;
		}
		Printf("%s\n", line.str().c_str());
	}

	static bool ShouldEmitProgressiveSlowdownTrace(uint64_t presentationGeneration)
	{
		if (!nri_ptslowdowntrace)
		{
			return false;
		}
		return ShouldSampleNRIPeriodicDiagnostic(
			presentationGeneration,
			(uint32_t)(std::max)(1, (int)nri_ptslowdowntraceinterval));
	}

	static void EmitProgressiveSlowdownTrace(
		uint64_t frameNumber,
		const NRIRenderer::PerfShellTraceStats& shell,
		const NRIRenderer::PerfResourceTraceStats& resource,
		const NRIRenderer::PerfTraceShaderStats& shader)
	{
		struct PreviousSample
		{
			bool initialized = false;
			uint32_t runtimeMutationActiveChunkCount = 0;
			uint32_t runtimeMutationCachedTriangleCount = 0;
			uint32_t dynamicPrimitiveCount = 0;
			uint32_t sceneInstanceCount = 0;
			uint32_t persistentVoxelInstanceActiveCount = 0;
			uint64_t persistentVoxelZeroRefResourceBytes = 0;
			uint32_t highRuntimeMutationActiveChunkCount = 0;
			uint32_t highRuntimeMutationCachedTriangleCount = 0;
			uint32_t highDynamicPrimitiveCount = 0;
			uint32_t highSceneInstanceCount = 0;
			uint32_t highPersistentVoxelInstanceActiveCount = 0;
			uint64_t highPersistentVoxelZeroRefResourceBytes = 0;
		};
		static PreviousSample previous = {};

		const int64_t deltaRuntimeChunks = previous.initialized ? DeltaUnsigned(shell.runtimeMutationActiveChunkCount, previous.runtimeMutationActiveChunkCount) : 0;
		const int64_t deltaRuntimeTris = previous.initialized ? DeltaUnsigned(shell.runtimeMutationCachedTriangleCount, previous.runtimeMutationCachedTriangleCount) : 0;
		const int64_t deltaDynamicPrims = previous.initialized ? DeltaUnsigned(shell.dynamicPrimitiveCount, previous.dynamicPrimitiveCount) : 0;
		const int64_t deltaSceneInstances = previous.initialized ? DeltaUnsigned(shell.sceneInstanceCount, previous.sceneInstanceCount) : 0;
		const int64_t deltaVoxelInstances = previous.initialized ? DeltaUnsigned(shell.persistentVoxelInstanceActiveCount, previous.persistentVoxelInstanceActiveCount) : 0;
		const int64_t deltaZeroRefBytes = previous.initialized ? DeltaUnsigned(shell.persistentVoxelZeroRefResourceBytes, previous.persistentVoxelZeroRefResourceBytes) : 0;
		const uint32_t highRuntimeChunks = previous.initialized ? (std::max)(previous.highRuntimeMutationActiveChunkCount, shell.runtimeMutationActiveChunkCount) : shell.runtimeMutationActiveChunkCount;
		const uint32_t highRuntimeTris = previous.initialized ? (std::max)(previous.highRuntimeMutationCachedTriangleCount, shell.runtimeMutationCachedTriangleCount) : shell.runtimeMutationCachedTriangleCount;
		const uint32_t highDynamicPrims = previous.initialized ? (std::max)(previous.highDynamicPrimitiveCount, shell.dynamicPrimitiveCount) : shell.dynamicPrimitiveCount;
		const uint32_t highSceneInstances = previous.initialized ? (std::max)(previous.highSceneInstanceCount, shell.sceneInstanceCount) : shell.sceneInstanceCount;
		const uint32_t highVoxelInstances = previous.initialized ? (std::max)(previous.highPersistentVoxelInstanceActiveCount, shell.persistentVoxelInstanceActiveCount) : shell.persistentVoxelInstanceActiveCount;
		const uint64_t highZeroRefBytes = previous.initialized ? (std::max)(previous.highPersistentVoxelZeroRefResourceBytes, shell.persistentVoxelZeroRefResourceBytes) : shell.persistentVoxelZeroRefResourceBytes;
		const double persistentVoxelCpuMs =
			shell.sceneSelectPersistentVoxelBatchMs +
			shell.persistentVoxelTlasInstanceMs +
			shell.geometryBuildPersistentVoxelVariantMs +
			shell.geometryBuildPersistentVoxelAppendMs +
			shell.geometryBuildPersistentVoxelRebuildMs +
			shell.sceneLightPersistentVoxelAppendMs +
			shell.persistentVoxelResourceStatsMs +
			shell.persistentVoxelBatchStatsMs;
		const double runtimeCpuMs =
			shell.runtimeMutationMs +
			shell.runtimeSpaceLinkMs +
			shell.runtimeMutationResidentApplyMs;
		const double textureMs =
			shell.sceneTextureLookupMs +
			shell.sceneTextureRealizeMs +
			shell.sceneTextureDescriptorMs +
			shell.sceneTextureTransitionMs;

		Printf(
			"PERF pt progressive slowdown NRI: frame=%llu interval=%d total=%.3f post_frame_diagnostics_ms=%.3f unattributed_ms=%.3f select=%.3f lights=%.3f dynamic_capture=%.3f dynamic_as=%.3f persistent_batch=%.3f persistent_voxel_cpu=%.3f persistent_voxel_tlas=%.3f persistent_voxel_stats=%.3f persistent_voxel_as=%.3f world_tlas=%.3f runtime_cpu=%.3f material=%.3f material_override=%.3f texture=%.3f texture_lookup=%.3f texture_realize=%.3f texture_descriptor=%.3f texture_transition=%.3f active_prims=%u dyn_prims=%u delta_dyn_prims=%lld high_dyn_prims=%u scene_instances=%u delta_scene_instances=%lld high_scene_instances=%u runtime_chunks=%u delta_runtime_chunks=%lld high_runtime_chunks=%u runtime_cached_tris=%u delta_runtime_tris=%lld high_runtime_tris=%u voxel_cache_entries=%u voxel_batch_actors=%u voxel_instances=%u delta_voxel_instances=%lld high_voxel_instances=%u voxel_pending=%u voxel_mesh_resources=%u voxel_material_resources=%u voxel_resident_bytes=%llu voxel_zero_ref_meshes=%u voxel_zero_ref_materials=%u voxel_zero_ref_bytes=%llu delta_zero_ref_bytes=%lld high_zero_ref_bytes=%llu admission_queue=%u scene_light_records=%u material_calls=%u texture_cache=%u texture_misses=%u resource_upload_bytes=%llu resource_wait_ms=%.3f\n",
			(unsigned long long)frameNumber,
			(int)nri_ptslowdowntraceinterval,
			shell.totalMs,
			shell.postFrameDiagnosticsMs,
			shell.unattributedMs,
			shell.sceneSelectMs,
			shell.sceneLightsMs,
			shell.dynamicCaptureMs,
			shell.dynamicAsMs,
			shell.sceneSelectPersistentVoxelBatchMs,
			persistentVoxelCpuMs,
			shell.persistentVoxelTlasInstanceMs,
			shell.persistentVoxelResourceStatsMs + shell.persistentVoxelBatchStatsMs + shell.sceneInstanceStatsMs,
			shell.persistentVoxelAsMs,
			shell.worldTlasMs,
			runtimeCpuMs,
			shell.materialBuildMs,
			shell.actorOverrideMapBuildMs,
			textureMs,
			shell.sceneTextureLookupMs,
			shell.sceneTextureRealizeMs,
			shell.sceneTextureDescriptorMs,
			shell.sceneTextureTransitionMs,
			shell.activePrimitiveCount,
			shell.dynamicPrimitiveCount,
			(long long)deltaDynamicPrims,
			highDynamicPrims,
			shell.sceneInstanceCount,
			(long long)deltaSceneInstances,
			highSceneInstances,
			shell.runtimeMutationActiveChunkCount,
			(long long)deltaRuntimeChunks,
			highRuntimeChunks,
			shell.runtimeMutationCachedTriangleCount,
			(long long)deltaRuntimeTris,
			highRuntimeTris,
			shell.voxelCacheActorEntries,
			shell.persistentVoxelBatchActorCount,
			shell.persistentVoxelInstanceActiveCount,
			(long long)deltaVoxelInstances,
			highVoxelInstances,
			shell.persistentVoxelPendingInstanceCount,
			shell.persistentVoxelMeshVariantResourceCount,
			shell.persistentVoxelMaterialVariantResourceCount,
			(unsigned long long)shell.persistentVoxelResidentResourceBytes,
			shell.persistentVoxelZeroRefMeshResourceCount,
			shell.persistentVoxelZeroRefMaterialResourceCount,
			(unsigned long long)shell.persistentVoxelZeroRefResourceBytes,
			(long long)deltaZeroRefBytes,
			(unsigned long long)highZeroRefBytes,
			shell.persistentVoxelAdmissionQueueCount,
			shell.sceneLightSurfaceRecordCount,
			shell.materialBuildCalls,
			shell.sceneTextureCacheCount,
			shell.sceneTextureCacheMisses,
			(unsigned long long)resource.sceneUploadBytes,
			resource.waitMs);
		Printf(
			"PERF pt progressive runtime NRI: frame=%llu runtime_cpu=%.3f mutation=%.3f analyze=%.3f rebuild=%.3f structural=%.3f material_refresh=%.3f resident_apply=%.3f resident_live=%.3f resident_geometry=%.3f resident_material=%.3f resident_baseline=%.3f resident_atlas=%.3f resident_atlas_book=%.3f resident_copy=%.3f resident_vertex_cpu=%.3f resident_index_cpu=%.3f resident_vertex_stage=%.3f resident_index_stage=%.3f resident_primitive=%.3f resident_primitive_cpu=%.3f resident_primitive_stage=%.3f resident_order_hash=%.3f resident_blas=%.3f resident_blas_setup=%.3f resident_blas_filter=%.3f resident_blas_create=%.3f resident_blas_scratch=%.3f resident_blas_barrier=%.3f resident_blas_build=%.3f spacelink=%.3f debug_sphere=%.3f candidates=%u analyzed=%u sweep=%u dirty=%u rebuilt=%u held=%u resident_apply_count=%u resident_recover=%u blas_recreate=%u blas_refit=%u\n",
			(unsigned long long)frameNumber,
			runtimeCpuMs,
			shell.runtimeMutationMs,
			shell.runtimeMutationAnalyzeMs,
			shell.runtimeMutationRebuildMs,
			shell.runtimeMutationStructuralRebuildMs,
			shell.runtimeMutationMaterialRefreshMs,
			shell.runtimeMutationResidentApplyMs,
			shell.runtimeMutationResidentApplyLiveBuildMs,
			shell.runtimeMutationResidentApplyGeometryBuildMs,
			shell.runtimeMutationResidentApplyMaterialBuildMs,
			shell.runtimeMutationResidentApplyBaselineCaptureMs,
			shell.runtimeMutationResidentApplyAtlasMs,
			shell.runtimeMutationResidentApplyAtlasBookkeepingMs,
			shell.runtimeMutationResidentApplyVertexIndexCopyMs,
			shell.runtimeMutationResidentApplyVertexCpuCopyMs,
			shell.runtimeMutationResidentApplyIndexCpuCopyMs,
			shell.runtimeMutationResidentApplyVertexStageMs,
			shell.runtimeMutationResidentApplyIndexStageMs,
			shell.runtimeMutationResidentApplyPrimitiveRewriteMs,
			shell.runtimeMutationResidentApplyPrimitiveCpuRewriteMs,
			shell.runtimeMutationResidentApplyPrimitiveStageMs,
			shell.runtimeMutationResidentApplyGeometryOrderHashMs,
			shell.runtimeMutationResidentApplyDownstreamBlasMs,
			shell.runtimeMutationResidentApplyDownstreamBlasSetupMs,
			shell.runtimeMutationResidentApplyDownstreamBlasFilterMs,
			shell.runtimeMutationResidentApplyDownstreamBlasCreateMs,
			shell.runtimeMutationResidentApplyDownstreamBlasScratchMs,
			shell.runtimeMutationResidentApplyDownstreamBlasBarrierMs,
			shell.runtimeMutationResidentApplyDownstreamBlasBuildMs,
			shell.runtimeSpaceLinkMs,
			shell.runtimeDebugSphereMs,
			shell.runtimeMutationCandidateChunks,
			shell.runtimeMutationAnalyzedChunks,
			shell.runtimeMutationBackgroundSweepChunks,
			shell.runtimeMutationDirtyChunks,
			shell.runtimeMutationRebuiltChunks,
			shell.runtimeMutationHeldChunks,
			shell.runtimeMutationResidentApplyCount,
			shell.runtimeMutationResidentApplyRecoverAttemptCount,
			shell.runtimeMutationResidentApplyBlasRecreateCount,
			shell.runtimeMutationResidentApplyBlasRefitOnlyCount);
		Printf(
			"PERF pt progressive mutation tier NRI: frame=%llu structural_visible_ms=%.3f structural_invisible_ms=%.3f material_visible_ms=%.3f material_invisible_ms=%.3f resident_visible_ms=%.3f resident_invisible_ms=%.3f candidates_visible=%u candidates_invisible=%u dirty_visible=%u dirty_invisible=%u structural_visible=%u structural_invisible=%u material_visible=%u material_invisible=%u resident_visible=%u resident_invisible=%u\n",
			(unsigned long long)frameNumber,
			shell.runtimeMutationStructuralRebuildVisibleMs,
			shell.runtimeMutationStructuralRebuildInvisibleMs,
			shell.runtimeMutationMaterialRefreshVisibleMs,
			shell.runtimeMutationMaterialRefreshInvisibleMs,
			shell.runtimeMutationResidentApplyVisibleMs,
			shell.runtimeMutationResidentApplyInvisibleMs,
			shell.runtimeMutationCandidateVisibleChunks,
			shell.runtimeMutationCandidateInvisibleChunks,
			shell.runtimeMutationDirtyVisibleChunks,
			shell.runtimeMutationDirtyInvisibleChunks,
			shell.runtimeMutationStructuralRebuildVisibleChunks,
			shell.runtimeMutationStructuralRebuildInvisibleChunks,
			shell.runtimeMutationMaterialRefreshVisibleChunks,
			shell.runtimeMutationMaterialRefreshInvisibleChunks,
			shell.runtimeMutationResidentApplyVisibleChunks,
			shell.runtimeMutationResidentApplyInvisibleChunks);
		Printf(
			"PERF pt progressive mutation distance NRI: frame=%llu near_distance=%.1f structural_near_ms=%.3f structural_far_ms=%.3f structural_unknown_ms=%.3f material_near_ms=%.3f material_far_ms=%.3f material_unknown_ms=%.3f resident_near_ms=%.3f resident_far_ms=%.3f resident_unknown_ms=%.3f candidate_near=%u candidate_far=%u candidate_unknown=%u candidate_bounds_valid=%u candidate_bounds_invalid=%u dirty_near=%u dirty_far=%u dirty_unknown=%u structural_near=%u structural_far=%u structural_unknown=%u material_near=%u material_far=%u material_unknown=%u resident_near=%u resident_far=%u resident_unknown=%u\n",
			(unsigned long long)frameNumber,
			(double)shell.runtimeMutationNearDistance,
			shell.runtimeMutationStructuralRebuildNearMs,
			shell.runtimeMutationStructuralRebuildFarMs,
			shell.runtimeMutationStructuralRebuildUnknownDistanceMs,
			shell.runtimeMutationMaterialRefreshNearMs,
			shell.runtimeMutationMaterialRefreshFarMs,
			shell.runtimeMutationMaterialRefreshUnknownDistanceMs,
			shell.runtimeMutationResidentApplyNearMs,
			shell.runtimeMutationResidentApplyFarMs,
			shell.runtimeMutationResidentApplyUnknownDistanceMs,
			shell.runtimeMutationCandidateNearChunks,
			shell.runtimeMutationCandidateFarChunks,
			shell.runtimeMutationCandidateUnknownDistanceChunks,
			shell.runtimeMutationCandidateBoundsValidChunks,
			shell.runtimeMutationCandidateBoundsInvalidChunks,
			shell.runtimeMutationDirtyNearChunks,
			shell.runtimeMutationDirtyFarChunks,
			shell.runtimeMutationDirtyUnknownDistanceChunks,
			shell.runtimeMutationStructuralRebuildNearChunks,
			shell.runtimeMutationStructuralRebuildFarChunks,
			shell.runtimeMutationStructuralRebuildUnknownDistanceChunks,
			shell.runtimeMutationMaterialRefreshNearChunks,
			shell.runtimeMutationMaterialRefreshFarChunks,
			shell.runtimeMutationMaterialRefreshUnknownDistanceChunks,
			shell.runtimeMutationResidentApplyNearChunks,
			shell.runtimeMutationResidentApplyFarChunks,
			shell.runtimeMutationResidentApplyUnknownDistanceChunks);
		Printf(
			"PERF pt progressive mutation deferred NRI: frame=%llu material_deferred=%u material_coalesced=%u material_flushed=%u material_pending=%u material_enabled=%u material_near_deferred=%u material_near_coalesced=%u material_near_flushed=%u material_near_pending=%u material_near_budget=%u material_near_enabled=%u structural_deferred=%u structural_coalesced=%u structural_flushed=%u structural_promoted=%u structural_pending=%u structural_budget=%u structural_enabled=%u structural_near_deferred=%u structural_near_coalesced=%u structural_near_flushed=%u structural_near_pending=%u structural_near_budget=%u structural_near_enabled=%u structural_far_deferred=%u structural_far_coalesced=%u structural_far_flushed=%u structural_far_pending=%u structural_far_budget=%u structural_far_enabled=%u\n",
			(unsigned long long)frameNumber,
			shell.runtimeMutationMaterialRefreshDeferredChunks,
			shell.runtimeMutationMaterialRefreshDeferredCoalescedChunks,
			shell.runtimeMutationMaterialRefreshDeferredFlushedChunks,
			shell.runtimeMutationMaterialRefreshDeferredPendingChunks,
			(uint32_t)((bool)nri_ptruntimedeferfarmaterial || (bool)nri_ptruntimedefernearinvisiblematerial),
			shell.runtimeMutationMaterialRefreshDeferredNearChunks,
			shell.runtimeMutationMaterialRefreshDeferredNearCoalescedChunks,
			shell.runtimeMutationMaterialRefreshDeferredNearFlushedChunks,
			shell.runtimeMutationMaterialRefreshDeferredNearPendingChunks,
			shell.runtimeMutationMaterialRefreshDeferredNearBudget,
			(uint32_t)(bool)nri_ptruntimedefernearinvisiblematerial,
			shell.runtimeMutationStructuralRebuildDeferredChunks,
			shell.runtimeMutationStructuralRebuildDeferredCoalescedChunks,
			shell.runtimeMutationStructuralRebuildDeferredFlushedChunks,
			shell.runtimeMutationStructuralRebuildDeferredPromotedChunks,
			shell.runtimeMutationStructuralRebuildDeferredPendingChunks,
			shell.runtimeMutationStructuralRebuildDeferredBudget,
			(uint32_t)((bool)nri_ptruntimedefernearinvisiblestructural || (bool)nri_ptruntimedeferfarstructural),
			shell.runtimeMutationStructuralRebuildDeferredNearChunks,
			shell.runtimeMutationStructuralRebuildDeferredNearCoalescedChunks,
			shell.runtimeMutationStructuralRebuildDeferredNearFlushedChunks,
			shell.runtimeMutationStructuralRebuildDeferredNearPendingChunks,
			shell.runtimeMutationStructuralRebuildDeferredNearBudget,
			(uint32_t)(bool)nri_ptruntimedefernearinvisiblestructural,
			shell.runtimeMutationStructuralRebuildDeferredFarChunks,
			shell.runtimeMutationStructuralRebuildDeferredFarCoalescedChunks,
			shell.runtimeMutationStructuralRebuildDeferredFarFlushedChunks,
			shell.runtimeMutationStructuralRebuildDeferredFarPendingChunks,
			shell.runtimeMutationStructuralRebuildDeferredFarBudget,
			(uint32_t)(bool)nri_ptruntimedeferfarstructural);
		Printf(
			"PERF pt progressive mutation source NRI: frame=%llu candidate_active=%u candidate_visible_resident=%u candidate_startup_visible=%u candidate_unresolved=%u candidate_static_anim=%u candidate_sector_dirty=%u candidate_section_dirty=%u candidate_dragged=%u candidate_signature_watch=%u candidate_background=%u candidate_deferred_structural=%u dirty_active=%u dirty_background=%u structural_active=%u structural_background=%u material_active=%u material_background=%u resident_active=%u resident_background=%u\n",
			(unsigned long long)frameNumber,
			shell.runtimeMutationCandidateActiveReplacementChunks,
			shell.runtimeMutationCandidateVisibleResidentValidationChunks,
			shell.runtimeMutationCandidateStartupVisibleValidationChunks,
			shell.runtimeMutationCandidateUnresolvedTextureChunks,
			shell.runtimeMutationCandidateStaticAnimatedSuppressedChunks,
			shell.runtimeMutationCandidateSectorDirtyChunks,
			shell.runtimeMutationCandidateSectionDirtyChunks,
			shell.runtimeMutationCandidateDraggedChunks,
			shell.runtimeMutationCandidateSignatureWatchChunks,
			shell.runtimeMutationCandidateBackgroundSweepSourceChunks,
			shell.runtimeMutationCandidateDeferredStructuralChunks,
			shell.runtimeMutationDirtyActiveReplacementChunks,
			shell.runtimeMutationDirtyBackgroundSweepChunks,
			shell.runtimeMutationStructuralRebuildActiveReplacementChunks,
			shell.runtimeMutationStructuralRebuildBackgroundSweepChunks,
			shell.runtimeMutationMaterialRefreshActiveReplacementChunks,
			shell.runtimeMutationMaterialRefreshBackgroundSweepChunks,
			shell.runtimeMutationResidentApplyActiveReplacementChunks,
			shell.runtimeMutationResidentApplyBackgroundSweepChunks);
		Printf(
			"PERF pt progressive gpu NRI: frame=%llu frame_graph=%.3f trace_opaque_cpu=%.3f trace_opaque_readback=%.3f trace_opaque_cmd=%.3f trace_opaque_stats_copy=%.3f trace_dispatch_x=%u trace_dispatch_y=%u trace_dispatch_z=%u\n",
			(unsigned long long)frameNumber,
			shell.frameGraphMs,
			shell.traceOpaqueMs,
			shell.traceOpaqueReadbackMs,
			shell.traceOpaqueCommandMs,
			shell.traceOpaqueStatsCopyMs,
			shell.traceOpaqueDispatchX,
			shell.traceOpaqueDispatchY,
			shell.traceOpaqueDispatchZ);
		Printf(
			"PERF pt progressive event NRI: frame=%llu pending_voxels=%u admitted=%u deferred=%u actor_budget_hits=%u prim_budget_hits=%u byte_budget_hits=%u texture_budget_hits=%u prewarm_queued=%u prewarm_deferred=%u runtime_dirty=%u runtime_rebuilt=%u runtime_held=%u resident_apply=%u resident_recover=%u blas_recreate=%u blas_refit=%u dynamic_escape_actors=%u unexpected_dynamic_escape_actors=%u\n",
			(unsigned long long)frameNumber,
			shell.persistentVoxelPendingInstanceCount,
			shell.persistentVoxelOnboardingAdmittedCount,
			shell.persistentVoxelOnboardingDeferredCount,
			shell.persistentVoxelOnboardingActorBudgetHits,
			shell.persistentVoxelOnboardingPrimitiveBudgetHits,
			shell.persistentVoxelOnboardingByteBudgetHits,
			shell.persistentVoxelOnboardingTextureBudgetHits,
			shell.persistentVoxelTexturePrewarmQueuedCount,
			shell.persistentVoxelTexturePrewarmDeferredCount,
			shell.runtimeMutationDirtyChunks,
			shell.runtimeMutationRebuiltChunks,
			shell.runtimeMutationHeldChunks,
			shell.runtimeMutationResidentApplyCount,
			shell.runtimeMutationResidentApplyRecoverAttemptCount,
			shell.runtimeMutationResidentApplyBlasRecreateCount,
			shell.runtimeMutationResidentApplyBlasRefitOnlyCount,
			shell.dynamicVoxelEscapeActorCount,
			shell.dynamicVoxelUnexpectedEscapeActorCount);

		struct GeometryPath
		{
			const char* name;
			double ms;
			uint32_t calls;
			uint32_t prims;
			uint64_t bytes;
		};
		std::array<GeometryPath, 19> geometryPaths = {{
			{ "dynamic_live", shell.geometryBuildDynamicLiveMs, shell.dynamicCaptureCalls, shell.geometryBuildDynamicLivePrimitives, resource.sceneDynamicUploadBytes },
			{ "local_player_reflection", shell.geometryBuildLocalPlayerReflectionMs, 0, 0, 0 },
			{ "merged_dynamic", shell.geometryBuildMergedDynamicMs, 0, shell.dynamicPrimitiveCount, resource.sceneDynamicUploadBytes },
			{ "captured", shell.geometryBuildCapturedMs, 0, shell.activePrimitiveCount, resource.sceneUploadBytes },
			{ "persistent_voxel_variant", shell.geometryBuildPersistentVoxelVariantMs, shell.geometryBuildPersistentVoxelVariantCalls, shell.geometryBuildPersistentVoxelVariantPrimitives, resource.scenePersistentVoxelVariantUploadBytes },
			{ "persistent_voxel_append", shell.geometryBuildPersistentVoxelAppendMs, shell.persistentVoxelOnboardingAdmittedCount, shell.persistentVoxelOnboardingAdmittedCount, resource.scenePersistentVoxelUploadBytes },
			{ "persistent_voxel_rebuild", shell.geometryBuildPersistentVoxelRebuildMs, 0, shell.persistentVoxelInstancePrimitiveCount, resource.scenePersistentVoxelUploadBytes },
			{ "persistent_voxel_tlas", shell.persistentVoxelTlasInstanceMs, 1, shell.persistentVoxelInstancePrimitiveCount, 0 },
			{ "persistent_voxel_resource_stats", shell.persistentVoxelResourceStatsMs, 1, shell.persistentVoxelMeshVariantResourceCount + shell.persistentVoxelMaterialVariantResourceCount, 0 },
			{ "persistent_voxel_batch_stats", shell.persistentVoxelBatchStatsMs, 1, shell.persistentVoxelBatchActorCount, 0 },
			{ "scene_instance_stats", shell.sceneInstanceStatsMs, 1, shell.sceneInstanceCount, 0 },
			{ "persistent_emissive_prune", shell.geometryBuildPersistentEmissivePruneMs, 0, shell.persistentDynamicActorSurfaceCount + shell.persistentDynamicNonActorSurfaceCount, 0 },
			{ "persistent_emissive_rebuild", shell.geometryBuildPersistentEmissiveRebuildMs, 0, shell.persistentDynamicActorSurfaceCount + shell.persistentDynamicNonActorSurfaceCount, 0 },
			{ "static_chunk", shell.geometryBuildStaticChunkMs, shell.geometryBuildStaticChunkCalls, shell.geometryBuildStaticChunkPrimitives, resource.sceneStaticRefreshUploadBytes },
			{ "debug_sphere", shell.geometryBuildDebugSphereMs, shell.runtimeDebugSphereCount, shell.runtimeDebugSpherePrimitiveCount, 0 },
			{ "mutation_rebuild", shell.geometryBuildRuntimeMutationRebuildMs, shell.geometryBuildRuntimeMutationRebuildCalls, shell.geometryBuildRuntimeMutationPrimitives, resource.sceneResidentChunkUploadBytes },
			{ "mutation_material_only", shell.geometryBuildRuntimeMutationMaterialOnlyMs, shell.geometryBuildRuntimeMutationMaterialOnlyCalls, shell.geometryBuildRuntimeMutationPrimitives, resource.sceneResidentChunkUploadBytes },
			{ "spacelink", shell.geometryBuildRuntimeSpaceLinkMs, shell.geometryBuildRuntimeSpaceLinkCalls, shell.geometryBuildRuntimeSpaceLinkPrimitives, 0 },
			{ "resident_apply_recover", shell.geometryBuildResidentApplyMs + shell.geometryBuildResidentRecoverMs, shell.geometryBuildResidentApplyCalls + shell.geometryBuildResidentRecoverCalls, shell.geometryBuildResidentPrimitives, resource.sceneResidentChunkUploadBytes },
		}};
		std::sort(geometryPaths.begin(), geometryPaths.end(), [](const GeometryPath& left, const GeometryPath& right)
		{
			if (left.ms != right.ms)
			{
				return left.ms > right.ms;
			}
			return left.prims > right.prims;
		});

		const uint32_t topCount = (uint32_t)(std::min)((int)geometryPaths.size(), (std::max)(0, (int)nri_ptslowdowntop));
		for (uint32_t i = 0; i < topCount; ++i)
		{
			const GeometryPath& path = geometryPaths[i];
			if (path.ms <= 0.0 && path.calls == 0 && path.prims == 0 && path.bytes == 0)
			{
				continue;
			}
			Printf(
				"PERF pt progressive geometry top NRI: frame=%llu rank=%u path=%s ms=%.3f calls=%u prims=%u bytes=%llu\n",
				(unsigned long long)frameNumber,
				i + 1u,
				path.name,
				path.ms,
				path.calls,
				path.prims,
				(unsigned long long)path.bytes);
		}

		Printf(
			"PERF pt progressive geometry NRI: frame=%llu dynamic_live=%.3f local_player_reflection=%.3f captured=%.3f persistent_voxel_variant=%.3f persistent_voxel_append=%.3f persistent_voxel_rebuild=%.3f persistent_voxel_tlas=%.3f persistent_voxel_resource_stats=%.3f persistent_voxel_batch_stats=%.3f scene_instance_stats=%.3f persistent_emissive=%.3f static_chunk=%.3f mutation_truth=%.3f mutation_rebuild=%.3f mutation_material_only=%.3f spacelink=%.3f resident_apply=%.3f resident_recover=%.3f dynamic_capture_models=%.3f dynamic_capture_model_mesh=%.3f dynamic_capture_model_surface=%.3f dynamic_capture_model_store=%.3f dynamic_walls=%u dynamic_flats=%u dynamic_sprites=%u dynamic_voxel_proxies=%u dynamic_unsupported_models=%u\n",
			(unsigned long long)frameNumber,
			shell.geometryBuildDynamicLiveMs,
			shell.geometryBuildLocalPlayerReflectionMs,
			shell.geometryBuildCapturedMs,
			shell.geometryBuildPersistentVoxelVariantMs,
			shell.geometryBuildPersistentVoxelAppendMs,
			shell.geometryBuildPersistentVoxelRebuildMs,
			shell.persistentVoxelTlasInstanceMs,
			shell.persistentVoxelResourceStatsMs,
			shell.persistentVoxelBatchStatsMs,
			shell.sceneInstanceStatsMs,
			shell.geometryBuildPersistentEmissivePruneMs + shell.geometryBuildPersistentEmissiveRebuildMs,
			shell.geometryBuildStaticChunkMs,
			shell.geometryBuildRuntimeMutationTruthMs,
			shell.geometryBuildRuntimeMutationRebuildMs,
			shell.geometryBuildRuntimeMutationMaterialOnlyMs,
			shell.geometryBuildRuntimeSpaceLinkMs,
			shell.geometryBuildResidentApplyMs,
			shell.geometryBuildResidentRecoverMs,
			shell.dynamicCaptureModelSpritesMs,
			shell.dynamicCaptureModelMeshMs,
			shell.dynamicCaptureModelSurfaceMs,
			shell.dynamicCaptureModelStoreMs,
			shell.dynamicCaptureWallSurfaces,
			shell.dynamicCaptureFlatSurfaces,
			shell.dynamicCaptureSpriteSurfaces,
			shell.dynamicCaptureVoxelProxySurfaces,
			shell.dynamicCaptureUnsupportedModelSurfaces);

		for (uint32_t i = 0; i < topCount && i < NRIRenderer::RuntimeMutationTopTraceCount; ++i)
		{
			const auto& entry = shell.runtimeMutationTopEntries[i];
			if (!entry.valid)
			{
				continue;
			}
			Printf(
				"PERF pt progressive resource top NRI: frame=%llu rank=%u type=runtime_mutation chunk=%u sector=%d action=%s reason_mask=0x%x score=%u surfaces=%u tris=%u mats=%u force_topology=%u resident_geo=%u resident_mat=%u\n",
				(unsigned long long)frameNumber,
				i + 1u,
				entry.chunkIndex,
				entry.sectorIndex,
				GetProgressiveRuntimeMutationTraceActionName(entry.action),
				entry.reasonMask,
				entry.score,
				entry.surfaceCount,
				entry.triangleCount,
				entry.materialCount,
				entry.forceTopology ? 1u : 0u,
				entry.residentGeometryDirty ? 1u : 0u,
				entry.residentMaterialDirty ? 1u : 0u);
		}
		for (uint32_t i = 0; i < topCount && i < shell.dynamicVoxelEscapeTopCount; ++i)
		{
			const auto& entry = shell.dynamicVoxelEscapeTopEntries[i];
			if (!entry.valid)
			{
				continue;
			}
			Printf(
				"PERF pt progressive resource top NRI: frame=%llu rank=%u type=dynamic_voxel_escape actor=%d stat=%d pic=%d voxel=%d reason=%s prims=%u bytes=%llu persistent=%u has_surface=%u\n",
				(unsigned long long)frameNumber,
				i + 1u,
				entry.actorIndex,
				entry.statnum,
				entry.sourcePicnum,
				entry.resolvedVoxelIndex,
				nri_scene::GetDynamicVoxelEscapeReasonName(entry.reason),
				entry.primitiveCount,
				(unsigned long long)entry.totalBytes,
				entry.persistentReady ? 1u : 0u,
				entry.hasCachedSurface ? 1u : 0u);
		}
		for (uint32_t i = 0; i < topCount && i < shell.voxelCacheDuplicateTopCount; ++i)
		{
			const auto& entry = shell.voxelCacheDuplicateTopEntries[i];
			if (!entry.valid)
			{
				continue;
			}
			Printf(
				"PERF pt progressive resource top NRI: frame=%llu rank=%u type=voxel_duplicate mesh_key=0x%llx pic=%d actors=%u persistent_actors=%u prims_per_actor=%u total_prims=%u dup_bytes=%llu\n",
				(unsigned long long)frameNumber,
				i + 1u,
				(unsigned long long)entry.meshKeyHash,
				entry.sourcePicnum,
				entry.actorCount,
				entry.persistentActorCount,
				entry.primitiveCountPerActor,
				entry.totalDuplicatedPrimitives,
				(unsigned long long)entry.duplicatedBytes);
		}
		if (shader.valid)
		{
			for (uint32_t i = 0; i < topCount && i < shader.hotInstanceCount; ++i)
			{
				const auto& entry = shader.hotInstances[i];
				Printf(
					"PERF pt progressive resource top NRI: frame=%llu rank=%u type=shader_hot_instance instance=%u source=%s prim_offset=%u prims=%u committed=%u accepted=%u primary=%u ungated=%u sun=%u point=%u emissive=%u fast_emissive=%u\n",
					(unsigned long long)frameNumber,
					i + 1u,
					entry.instanceId,
					GetProgressiveSceneDataSourceName(entry.dataSource),
					entry.primitiveOffset,
					entry.primitiveCount,
					entry.committed,
					entry.accepted,
					entry.primaryCommitted,
					entry.ungatedCommitted,
					entry.sunCommitted,
					entry.pointCommitted,
					entry.emissiveCommitted,
					entry.fastEmissiveCommitted);
			}
		}

		previous.initialized = true;
		previous.runtimeMutationActiveChunkCount = shell.runtimeMutationActiveChunkCount;
		previous.runtimeMutationCachedTriangleCount = shell.runtimeMutationCachedTriangleCount;
		previous.dynamicPrimitiveCount = shell.dynamicPrimitiveCount;
		previous.sceneInstanceCount = shell.sceneInstanceCount;
		previous.persistentVoxelInstanceActiveCount = shell.persistentVoxelInstanceActiveCount;
		previous.persistentVoxelZeroRefResourceBytes = shell.persistentVoxelZeroRefResourceBytes;
		previous.highRuntimeMutationActiveChunkCount = highRuntimeChunks;
		previous.highRuntimeMutationCachedTriangleCount = highRuntimeTris;
		previous.highDynamicPrimitiveCount = highDynamicPrims;
		previous.highSceneInstanceCount = highSceneInstances;
		previous.highPersistentVoxelInstanceActiveCount = highVoxelInstances;
		previous.highPersistentVoxelZeroRefResourceBytes = highZeroRefBytes;
	}

	struct LevelTransitionDeviceSnapshot
	{
		bool initialized = false;
		bool transitionInProgress = false;
		bool preloadPending = false;
		bool frameBegun = false;
		bool activeTarget = false;
		uint32_t pendingWeaponLightEvents = 0;
		uint32_t weaponLightEventsEnqueuedThisFrame = 0;
	};

	static const char* GetLevelTransitionReasonName(LevelTransitionReason reason)
	{
		switch (reason)
		{
		case LevelTransitionReason::NewGame: return "newgame";
		case LevelTransitionReason::NextLevel: return "nextlevel";
		case LevelTransitionReason::SaveGameLoad: return "savegame";
		case LevelTransitionReason::Startup: return "startup";
		case LevelTransitionReason::MainMenu: return "mainmenu";
		case LevelTransitionReason::Credits: return "credits";
		default: return "unknown";
		}
	}

	static HRESULT CreateDxgiFactoryForTelemetry(IDXGIFactory4** factory)
	{
		if (factory == nullptr)
		{
			return E_POINTER;
		}

		*factory = nullptr;
		HMODULE dxgiModule = GetModuleHandleA("dxgi.dll");
		if (dxgiModule == nullptr)
		{
			dxgiModule = LoadLibraryA("dxgi.dll");
			if (dxgiModule == nullptr)
			{
				return HRESULT_FROM_WIN32(GetLastError());
			}
		}

		using PfnCreateDXGIFactory2 = HRESULT(WINAPI*)(UINT flags, REFIID riid, void** factory);
		auto createFactory2 = reinterpret_cast<PfnCreateDXGIFactory2>(GetProcAddress(dxgiModule, "CreateDXGIFactory2"));
		if (createFactory2 == nullptr)
		{
			return E_NOINTERFACE;
		}

		return createFactory2(0u, IID_PPV_ARGS(factory));
	}

	static bool IsFullscreenPaletteBlendCommand(const F2DDrawer& drawer, const F2DDrawer::RenderCommand& cmd)
	{
		if (cmd.isSpecial != SpecialDrawCommand::NotSpecial ||
			cmd.mTexture != nullptr ||
			cmd.shape2DBufInfo != nullptr ||
			cmd.mType != F2DDrawer::DrawTypeTriangles ||
			(cmd.mFlags & F2DDrawer::DTF_Scissor) != 0 ||
			cmd.mVertCount != 4 ||
			cmd.mIndexCount != 6 ||
			cmd.mVertIndex < 0 ||
			cmd.mVertIndex + 3 >= drawer.mVertices.SSize())
		{
			return false;
		}

		const auto& v0 = drawer.mVertices[cmd.mVertIndex + 0];
		const auto& v1 = drawer.mVertices[cmd.mVertIndex + 1];
		const auto& v2 = drawer.mVertices[cmd.mVertIndex + 2];
		const auto& v3 = drawer.mVertices[cmd.mVertIndex + 3];
		const float width = (float)drawer.GetWidth();
		const float height = (float)drawer.GetHeight();

		return
			v0.x == 0.0f && v0.y == 0.0f &&
			v1.x == 0.0f && v1.y == height &&
			v2.x == width && v2.y == 0.0f &&
			v3.x == width && v3.y == height;
	}

	static constexpr int DefaultSwapChainTextureCount = 3;
	static constexpr float DefaultPtTestLightRadius = 256.0f;
	static constexpr float DefaultPtTestLightOffset = 64.0f;
	static constexpr float DefaultPtTestSphereMetalness = 1.0f;
	static constexpr float DefaultPtTestSphereRoughness = 0.05f;
	static constexpr double BuildTickSeconds = 1.0 / 120.0;
	static NRIRenderDevice* GetActiveNRIRenderDevice();

	static void WorldToPathTracingPosition(const DVector3& worldPos, float out[3])
	{
		out[0] = (float)worldPos.X;
		out[1] = (float)-worldPos.Z;
		out[2] = (float)-worldPos.Y;
	}

	static FString DescribeResolvedMuzzleFlashRuleIds(const ResolvedLightOverlaySet& resolvedSet, uint32_t limit = 16u)
	{
		if (resolvedSet.muzzleFlashRules.Size() == 0)
		{
			return "none";
		}

		TArray<FString> ids;
		ids.Reserve(resolvedSet.muzzleFlashRules.Size());
		for (const auto& rule : resolvedSet.muzzleFlashRules)
		{
			ids.Push(rule.id);
		}

		std::sort(ids.begin(), ids.end(), [](const FString& a, const FString& b)
		{
			return a.CompareNoCase(b) < 0;
		});

		FString result;
		const uint32_t printCount = std::min<uint32_t>((uint32_t)ids.Size(), limit);
		for (uint32_t i = 0; i < printCount; ++i)
		{
			if (!result.IsEmpty())
			{
				result << ",";
			}
			result << ids[i];
		}

		if (printCount < (uint32_t)ids.Size())
		{
			result.AppendFormat(",...(+%u)", (uint32_t)ids.Size() - printCount);
		}

		return result;
	}

	static const ResolvedLightOverlayMuzzleFlashRule* FindResolvedMuzzleFlashRule(const ResolvedLightOverlaySet& resolvedSet, const char* eventId)
	{
		if (eventId == nullptr || *eventId == '\0')
		{
			return nullptr;
		}

		for (const auto& rule : resolvedSet.muzzleFlashRules)
		{
			if (rule.id.CompareNoCase(eventId) == 0)
			{
				return &rule;
			}
		}

		return nullptr;
	}

	static FString DescribeResolvedSmokeEventRuleIds(const ResolvedLightOverlaySet& resolvedSet, uint32_t limit = 16u)
	{
		if (resolvedSet.smokeEventRules.Size() == 0)
		{
			return "none";
		}

		TArray<FString> ids;
		ids.Reserve(resolvedSet.smokeEventRules.Size());
		for (const auto& rule : resolvedSet.smokeEventRules)
		{
			ids.Push(rule.id);
		}
		std::sort(ids.begin(), ids.end(), [](const FString& a, const FString& b)
		{
			return a.CompareNoCase(b) < 0;
		});

		FString result;
		const uint32_t printCount = std::min<uint32_t>((uint32_t)ids.Size(), limit);
		for (uint32_t i = 0; i < printCount; ++i)
		{
			if (!result.IsEmpty()) result << ",";
			result << ids[i];
		}
		if (printCount < (uint32_t)ids.Size())
		{
			result.AppendFormat(",...(+%u)", (uint32_t)ids.Size() - printCount);
		}
		return result;
	}

	static const ResolvedLightOverlaySmokeEventRule* FindResolvedSmokeEventRule(const ResolvedLightOverlaySet& resolvedSet, const char* eventId)
	{
		if (eventId == nullptr || *eventId == '\0') return nullptr;
		for (const auto& rule : resolvedSet.smokeEventRules)
		{
			if (rule.id.CompareNoCase(eventId) == 0) return &rule;
		}
		return nullptr;
	}

	static bool BuildLocalPlayerWeaponLightEvent(const char* eventId, float forwardOffset, PathTracingWeaponLightEvent& outEvent, FString& outError)
	{
		if (eventId == nullptr || *eventId == '\0')
		{
			outError = "missing muzzle-flash event id";
			return false;
		}

		if (netgame)
		{
			outError = "cannot be used in multiplayer";
			return false;
		}

		outEvent = {};
		outEvent.eventId = eventId;
		outEvent.absoluteTimeSeconds = PlayClock > 0 ? (double)PlayClock * BuildTickSeconds : 0.0;

		DCorePlayer* player = PlayerArray[myconnectindex];
		if (player == nullptr)
		{
			outEvent.worldPosition = DVector3(forwardOffset, 0.0, 0.0);
			outEvent.basisRight = DVector3(0.0, 1.0, 0.0);
			outEvent.basisForward = DVector3(1.0, 0.0, 0.0);
			outEvent.basisUp = DVector3(0.0, 0.0, 1.0);
			outEvent.hasBasis = true;
			return true;
		}

		DCoreActor* actor = player->GetActor();
		if (actor == nullptr)
		{
			outEvent.worldPosition = DVector3(forwardOffset, 0.0, 0.0);
			outEvent.basisRight = DVector3(0.0, 1.0, 0.0);
			outEvent.basisForward = DVector3(1.0, 0.0, 0.0);
			outEvent.basisUp = DVector3(0.0, 0.0, 1.0);
			outEvent.hasBasis = true;
			return true;
		}

		const DRotator viewRotation(
			player->getPitchWithView(),
			actor->spr.Angles.Yaw + player->ViewAngles.Yaw,
			actor->spr.Angles.Roll + player->ViewAngles.Roll);
		const DVector3 forward = DVector3(viewRotation).Unit();
		const DVector3 right = DVector3(DRotator(nullAngle, viewRotation.Yaw + DAngle90, nullAngle)).Unit();
		DVector3 up = (forward ^ right).Unit();
		if (up.isZero())
		{
			up = DVector3(0.0, 0.0, 1.0);
		}

		outEvent.hasEmitterActorIndex = true;
		outEvent.emitterActorIndex = actor->GetIndex();
		outEvent.worldPosition = actor->getPosWithOffsetZ() + forward * forwardOffset;
		outEvent.basisRight = right;
		outEvent.basisForward = forward;
		outEvent.basisUp = up;
		outEvent.hasBasis = true;
		// Synthetic smoke-event tests also provide a deterministic impact frame,
		// allowing direction normal/incoming and normaloffset rules to be tested
		// without manufacturing a gameplay collision.
		outEvent.incomingDirection = forward;
		outEvent.hasIncomingDirection = true;
		outEvent.surfaceNormal = -forward;
		outEvent.hasSurfaceNormal = true;
		return true;
	}
}



namespace
{
	static nri::Result(NRI_CALL* gNriGetInterfaceForwarder)(const nri::Device&, const char*, size_t, void*) = nullptr;
	static void (NRI_CALL* gNriDestroyDeviceForwarder)(nri::Device*) = nullptr;
	using PFN_D3D12_GET_DEBUG_INTERFACE = HRESULT(WINAPI*)(REFIID, void**);

	template<typename T>
	static T NRIFlags(T a, T b)
	{
		return (T)((uint32_t)a | (uint32_t)b);
	}

	static nri::StageBits NRIShaderStages()
	{
		return (nri::StageBits)((uint32_t)nri::StageBits::VERTEX_SHADER | (uint32_t)nri::StageBits::FRAGMENT_SHADER);
	}

	static nri::StageBits NRISwapChainAcquireWaitStages()
	{
		// Raze first touches the acquired swapchain image as a copy destination in
		// PostProcessScene() before HUD color-attachment work begins.
		return (nri::StageBits)((uint32_t)nri::StageBits::COPY | (uint32_t)nri::StageBits::COLOR_ATTACHMENT);
	}

	static uint32_t AlignUp(uint32_t value, uint32_t alignment)
	{
		if (alignment <= 1)
		{
			return value;
		}

		const uint32_t remainder = value % alignment;
		return remainder == 0 ? value : value + alignment - remainder;
	}

	static const char* GetDxgiErrorName(HRESULT hr)
	{
		switch (hr)
		{
		case S_OK: return "S_OK";
		case DXGI_ERROR_DEVICE_HUNG: return "DXGI_ERROR_DEVICE_HUNG";
		case DXGI_ERROR_DEVICE_REMOVED: return "DXGI_ERROR_DEVICE_REMOVED";
		case DXGI_ERROR_DEVICE_RESET: return "DXGI_ERROR_DEVICE_RESET";
		case DXGI_ERROR_DRIVER_INTERNAL_ERROR: return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
		case DXGI_ERROR_INVALID_CALL: return "DXGI_ERROR_INVALID_CALL";
		default: return "unknown";
		}
	}

	static PFN_D3D12_GET_DEBUG_INTERFACE GetD3D12GetDebugInterfaceFn()
	{
		static PFN_D3D12_GET_DEBUG_INTERFACE sFn = nullptr;
		static bool sLoaded = false;
		if (!sLoaded)
		{
			HMODULE module = GetModuleHandleW(L"d3d12.dll");
			if (module == nullptr)
			{
				module = LoadLibraryW(L"d3d12.dll");
			}

			if (module != nullptr)
			{
				sFn = reinterpret_cast<PFN_D3D12_GET_DEBUG_INTERFACE>(GetProcAddress(module, "D3D12GetDebugInterface"));
			}

			sLoaded = true;
		}

		return sFn;
	}

	static std::string NarrowWideString(const wchar_t* text)
	{
		if (text == nullptr || *text == L'\0')
		{
			return {};
		}

		const int required = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
		if (required <= 1)
		{
			return {};
		}

		std::string result((size_t)required, '\0');
		WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), required, nullptr, nullptr);
		result.pop_back();
		return result;
	}

	static std::string GetDredDebugName(const char* ansiName, const wchar_t* wideName)
	{
		if (ansiName != nullptr && *ansiName != '\0')
		{
			return ansiName;
		}

		std::string wide = NarrowWideString(wideName);
		return wide.empty() ? std::string("(unnamed)") : wide;
	}

	static const char* GetD3D12AutoBreadcrumbOpName(D3D12_AUTO_BREADCRUMB_OP op)
	{
		switch (op)
		{
		case D3D12_AUTO_BREADCRUMB_OP_SETMARKER: return "SetMarker";
		case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT: return "BeginEvent";
		case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT: return "EndEvent";
		case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED: return "DrawInstanced";
		case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED: return "DrawIndexedInstanced";
		case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT: return "ExecuteIndirect";
		case D3D12_AUTO_BREADCRUMB_OP_DISPATCH: return "Dispatch";
		case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION: return "CopyBufferRegion";
		case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION: return "CopyTextureRegion";
		case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE: return "CopyResource";
		case D3D12_AUTO_BREADCRUMB_OP_COPYTILES: return "CopyTiles";
		case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE: return "ResolveSubresource";
		case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW: return "ClearRenderTargetView";
		case D3D12_AUTO_BREADCRUMB_OP_CLEARUNORDEREDACCESSVIEW: return "ClearUnorderedAccessView";
		case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW: return "ClearDepthStencilView";
		case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER: return "ResourceBarrier";
		case D3D12_AUTO_BREADCRUMB_OP_EXECUTEBUNDLE: return "ExecuteBundle";
		case D3D12_AUTO_BREADCRUMB_OP_PRESENT: return "Present";
		case D3D12_AUTO_BREADCRUMB_OP_RESOLVEQUERYDATA: return "ResolveQueryData";
		case D3D12_AUTO_BREADCRUMB_OP_BEGINSUBMISSION: return "BeginSubmission";
		case D3D12_AUTO_BREADCRUMB_OP_ENDSUBMISSION: return "EndSubmission";
		case D3D12_AUTO_BREADCRUMB_OP_BUILDRAYTRACINGACCELERATIONSTRUCTURE: return "BuildRayTracingAS";
		case D3D12_AUTO_BREADCRUMB_OP_COPYRAYTRACINGACCELERATIONSTRUCTURE: return "CopyRayTracingAS";
		case D3D12_AUTO_BREADCRUMB_OP_DISPATCHRAYS: return "DispatchRays";
		case D3D12_AUTO_BREADCRUMB_OP_SETPIPELINESTATE1: return "SetPipelineState1";
		case D3D12_AUTO_BREADCRUMB_OP_BARRIER: return "Barrier";
		case D3D12_AUTO_BREADCRUMB_OP_BEGIN_COMMAND_LIST: return "BeginCommandList";
		case D3D12_AUTO_BREADCRUMB_OP_DISPATCHGRAPH: return "DispatchGraph";
		case D3D12_AUTO_BREADCRUMB_OP_SETPROGRAM: return "SetProgram";
		default: return "Other";
		}
	}

	static const char* GetD3D12DredAllocationTypeName(D3D12_DRED_ALLOCATION_TYPE type)
	{
		switch (type)
		{
		case D3D12_DRED_ALLOCATION_TYPE_COMMAND_QUEUE: return "CommandQueue";
		case D3D12_DRED_ALLOCATION_TYPE_COMMAND_ALLOCATOR: return "CommandAllocator";
		case D3D12_DRED_ALLOCATION_TYPE_PIPELINE_STATE: return "PipelineState";
		case D3D12_DRED_ALLOCATION_TYPE_COMMAND_LIST: return "CommandList";
		case D3D12_DRED_ALLOCATION_TYPE_FENCE: return "Fence";
		case D3D12_DRED_ALLOCATION_TYPE_DESCRIPTOR_HEAP: return "DescriptorHeap";
		case D3D12_DRED_ALLOCATION_TYPE_HEAP: return "Heap";
		case D3D12_DRED_ALLOCATION_TYPE_QUERY_HEAP: return "QueryHeap";
		case D3D12_DRED_ALLOCATION_TYPE_COMMAND_SIGNATURE: return "CommandSignature";
		case D3D12_DRED_ALLOCATION_TYPE_PIPELINE_LIBRARY: return "PipelineLibrary";
		case D3D12_DRED_ALLOCATION_TYPE_VIDEO_DECODER: return "VideoDecoder";
		case D3D12_DRED_ALLOCATION_TYPE_VIDEO_PROCESSOR: return "VideoProcessor";
		case D3D12_DRED_ALLOCATION_TYPE_RESOURCE: return "Resource";
		case D3D12_DRED_ALLOCATION_TYPE_PASS: return "Pass";
		case D3D12_DRED_ALLOCATION_TYPE_CRYPTOSESSION: return "CryptoSession";
		case D3D12_DRED_ALLOCATION_TYPE_CRYPTOSESSIONPOLICY: return "CryptoSessionPolicy";
		case D3D12_DRED_ALLOCATION_TYPE_PROTECTEDRESOURCESESSION: return "ProtectedResourceSession";
		case D3D12_DRED_ALLOCATION_TYPE_VIDEO_DECODER_HEAP: return "VideoDecoderHeap";
		case D3D12_DRED_ALLOCATION_TYPE_COMMAND_POOL: return "CommandPool";
		case D3D12_DRED_ALLOCATION_TYPE_COMMAND_RECORDER: return "CommandRecorder";
		case D3D12_DRED_ALLOCATION_TYPE_STATE_OBJECT: return "StateObject";
		case D3D12_DRED_ALLOCATION_TYPE_METACOMMAND: return "MetaCommand";
		case D3D12_DRED_ALLOCATION_TYPE_SCHEDULINGGROUP: return "SchedulingGroup";
		case D3D12_DRED_ALLOCATION_TYPE_VIDEO_MOTION_ESTIMATOR: return "VideoMotionEstimator";
		case D3D12_DRED_ALLOCATION_TYPE_VIDEO_MOTION_VECTOR_HEAP: return "VideoMotionVectorHeap";
		case D3D12_DRED_ALLOCATION_TYPE_VIDEO_EXTENSION_COMMAND: return "VideoExtensionCommand";
		case D3D12_DRED_ALLOCATION_TYPE_VIDEO_ENCODER: return "VideoEncoder";
		case D3D12_DRED_ALLOCATION_TYPE_VIDEO_ENCODER_HEAP: return "VideoEncoderHeap";
		default: return "Other";
		}
	}

	static void LogD3D12DredBreadcrumbWindow(const D3D12_AUTO_BREADCRUMB_OP* history, uint32_t breadcrumbCount, uint32_t completedValue, uint32_t nodeIndex)
	{
		if (history == nullptr || breadcrumbCount == 0)
		{
			return;
		}

		const uint32_t clampedCompleted = (std::min)(completedValue, breadcrumbCount - 1);
		const uint32_t start = clampedCompleted > 2 ? clampedCompleted - 2 : 0;
		const uint32_t end = (std::min)(breadcrumbCount, start + 5);
		for (uint32_t i = start; i < end; ++i)
		{
			const char* marker = i == clampedCompleted ? " <last_completed>" : "";
			Printf(TEXTCOLOR_RED "NRI D3D12 DRED breadcrumb[%u].op[%u]: %s%s\n",
				nodeIndex,
				i,
				GetD3D12AutoBreadcrumbOpName(history[i]),
				marker);
		}
	}

	static void LogD3D12DredBreadcrumbNodes(const D3D12_AUTO_BREADCRUMB_NODE* head, const char* context)
	{
		uint32_t nodeIndex = 0;
		for (const D3D12_AUTO_BREADCRUMB_NODE* node = head; node != nullptr && nodeIndex < 6; node = node->pNext, ++nodeIndex)
		{
			const std::string commandListName = GetDredDebugName(node->pCommandListDebugNameA, node->pCommandListDebugNameW);
			const std::string queueName = GetDredDebugName(node->pCommandQueueDebugNameA, node->pCommandQueueDebugNameW);
			const uint32_t breadcrumbCount = node->BreadcrumbCount;
			const uint32_t completedValue = node->pLastBreadcrumbValue != nullptr ? *node->pLastBreadcrumbValue : 0;
			Printf(TEXTCOLOR_RED "NRI D3D12 DRED breadcrumb[%u] after %s: cmdlist=%s queue=%s completed=%u/%u\n",
				nodeIndex,
				context != nullptr ? context : "unknown",
				commandListName.c_str(),
				queueName.c_str(),
				completedValue,
				breadcrumbCount);

			LogD3D12DredBreadcrumbWindow(node->pCommandHistory, breadcrumbCount, completedValue, nodeIndex);
		}
	}

	static void LogD3D12DredBreadcrumbNodes1(const D3D12_AUTO_BREADCRUMB_NODE1* head, const char* context)
	{
		uint32_t nodeIndex = 0;
		for (const D3D12_AUTO_BREADCRUMB_NODE1* node = head; node != nullptr && nodeIndex < 6; node = node->pNext, ++nodeIndex)
		{
			const std::string commandListName = GetDredDebugName(node->pCommandListDebugNameA, node->pCommandListDebugNameW);
			const std::string queueName = GetDredDebugName(node->pCommandQueueDebugNameA, node->pCommandQueueDebugNameW);
			const uint32_t breadcrumbCount = node->BreadcrumbCount;
			const uint32_t completedValue = node->pLastBreadcrumbValue != nullptr ? *node->pLastBreadcrumbValue : 0;
			Printf(TEXTCOLOR_RED "NRI D3D12 DRED breadcrumb[%u] after %s: cmdlist=%s queue=%s completed=%u/%u contexts=%u\n",
				nodeIndex,
				context != nullptr ? context : "unknown",
				commandListName.c_str(),
				queueName.c_str(),
				completedValue,
				breadcrumbCount,
				node->BreadcrumbContextsCount);

			LogD3D12DredBreadcrumbWindow(node->pCommandHistory, breadcrumbCount, completedValue, nodeIndex);

			if (node->pBreadcrumbContexts != nullptr && node->BreadcrumbContextsCount != 0)
			{
				const uint32_t contextCount = (std::min)(node->BreadcrumbContextsCount, 6u);
				for (uint32_t i = 0; i < contextCount; ++i)
				{
					const D3D12_DRED_BREADCRUMB_CONTEXT& breadcrumbContext = node->pBreadcrumbContexts[i];
					const std::string breadcrumbText = NarrowWideString(breadcrumbContext.pContextString);
					Printf(TEXTCOLOR_RED "NRI D3D12 DRED breadcrumb[%u].context[%u]: index=%u text=%s\n",
						nodeIndex,
						i,
						breadcrumbContext.BreadcrumbIndex,
						breadcrumbText.empty() ? "(empty)" : breadcrumbText.c_str());
				}
			}
		}
	}

	static void LogD3D12DredAllocationNodes(const D3D12_DRED_ALLOCATION_NODE* head, const char* label)
	{
		uint32_t allocationIndex = 0;
		for (const D3D12_DRED_ALLOCATION_NODE* allocation = head; allocation != nullptr && allocationIndex < 8; allocation = allocation->pNext, ++allocationIndex)
		{
			const std::string objectName = GetDredDebugName(allocation->ObjectNameA, allocation->ObjectNameW);
			Printf(TEXTCOLOR_RED "NRI D3D12 DRED %s[%u]: type=%s name=%s\n",
				label,
				allocationIndex,
				GetD3D12DredAllocationTypeName(allocation->AllocationType),
				objectName.c_str());
		}
	}

	static void LogD3D12DredAllocationNodes1(const D3D12_DRED_ALLOCATION_NODE1* head, const char* label)
	{
		uint32_t allocationIndex = 0;
		for (const D3D12_DRED_ALLOCATION_NODE1* allocation = head; allocation != nullptr && allocationIndex < 8; allocation = allocation->pNext, ++allocationIndex)
		{
			const std::string objectName = GetDredDebugName(allocation->ObjectNameA, allocation->ObjectNameW);
			Printf(TEXTCOLOR_RED "NRI D3D12 DRED %s[%u]: type=%s name=%s object=%p\n",
				label,
				allocationIndex,
				GetD3D12DredAllocationTypeName(allocation->AllocationType),
				objectName.c_str(),
				allocation->pObject);
		}
	}

	static void ConfigureD3D12Dred()
	{
		if (!nri_dred)
		{
			return;
		}

		const auto getDebugInterface = GetD3D12GetDebugInterfaceFn();
		if (getDebugInterface == nullptr)
		{
			Printf(TEXTCOLOR_RED "NRI D3D12 DRED setup failed: D3D12GetDebugInterface is unavailable.\n");
			return;
		}

		ID3D12DeviceRemovedExtendedDataSettings1* settings1 = nullptr;
		if (SUCCEEDED(getDebugInterface(IID_PPV_ARGS(&settings1))) && settings1 != nullptr)
		{
			settings1->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			settings1->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			settings1->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			settings1->Release();
			return;
		}

		ID3D12DeviceRemovedExtendedDataSettings* settings = nullptr;
		if (SUCCEEDED(getDebugInterface(IID_PPV_ARGS(&settings))) && settings != nullptr)
		{
			settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			settings->Release();
			return;
		}

		Printf(TEXTCOLOR_RED "NRI D3D12 DRED setup failed: DRED settings interfaces are unavailable.\n");
	}

	static void ConfigureD3D12DebugLayer()
	{
		if (!nri_apivalidation)
		{
			return;
		}

		const auto getDebugInterface = GetD3D12GetDebugInterfaceFn();
		if (getDebugInterface == nullptr)
		{
			Printf(TEXTCOLOR_RED "NRI D3D12 API validation requested, but D3D12GetDebugInterface is unavailable.\n");
			return;
		}

		ID3D12Debug* debug = nullptr;
		if (FAILED(getDebugInterface(IID_PPV_ARGS(&debug))) || debug == nullptr)
		{
			Printf(TEXTCOLOR_RED "NRI D3D12 API validation requested, but ID3D12Debug is unavailable.\n");
			return;
		}

		debug->EnableDebugLayer();

		ID3D12Debug1* debug1 = nullptr;
		if (SUCCEEDED(debug->QueryInterface(IID_PPV_ARGS(&debug1))) && debug1 != nullptr)
		{
			debug1->SetEnableGPUBasedValidation(FALSE);
			debug1->SetEnableSynchronizedCommandQueueValidation(FALSE);
			debug1->Release();
		}

		debug->Release();
		Printf("NRI D3D12 debug layer enabled for API validation.\n");
	}

	static void ConfigureD3D12InfoQueue(const nri::CoreInterface& core, nri::Device* device)
	{
		if (!nri_apivalidation || device == nullptr || core.GetDeviceNativeObject == nullptr)
		{
			return;
		}

		auto* d3d12Device = static_cast<ID3D12Device*>(core.GetDeviceNativeObject(device));
		if (d3d12Device == nullptr)
		{
			return;
		}

		ID3D12InfoQueue* infoQueue = nullptr;
		if (FAILED(d3d12Device->QueryInterface(IID_PPV_ARGS(&infoQueue))) || infoQueue == nullptr)
		{
			Printf(TEXTCOLOR_RED "NRI D3D12 debug layer is enabled, but ID3D12InfoQueue is unavailable.\n");
			return;
		}

		infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
		infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
		infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, FALSE);
		infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_INFO, FALSE);
		infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_MESSAGE, FALSE);
		Printf("NRI D3D12 info queue configured: debugger breaks disabled while API validation is on.\n");
		infoQueue->Release();
	}

	template<typename T>
	static void SetNriDebugName(const nri::CoreInterface& core, T* object, const char* name)
	{
		if (object == nullptr || name == nullptr || *name == '\0' || core.SetDebugName == nullptr)
		{
			return;
		}

		core.SetDebugName(reinterpret_cast<nri::Object*>(object), name);
	}

	static void LogD3D12DeviceRemovedReason(const nri::CoreInterface& core, nri::Device* device, const char* context)
	{
		if (device == nullptr || core.GetDeviceNativeObject == nullptr)
		{
			return;
		}

		auto* d3d12Device = static_cast<ID3D12Device*>(core.GetDeviceNativeObject(device));
		if (d3d12Device == nullptr)
		{
			return;
		}

		const HRESULT hr = d3d12Device->GetDeviceRemovedReason();
		if (hr == S_OK)
		{
			Printf(TEXTCOLOR_RED "NRI D3D12 removed reason after %s: %s (0x%08X).\n",
				context != nullptr ? context : "unknown",
				GetDxgiErrorName(hr),
				(unsigned)hr);
			return;
		}

		Printf(TEXTCOLOR_RED "NRI D3D12 removed reason after %s: %s (0x%08X).\n",
			context != nullptr ? context : "unknown",
			GetDxgiErrorName(hr),
			(unsigned)hr);
	}

	static void LogD3D12InfoQueueMessages(const nri::CoreInterface& core, nri::Device* device, const char* context)
	{
		if (device == nullptr || core.GetDeviceNativeObject == nullptr)
		{
			return;
		}

		auto* d3d12Device = static_cast<ID3D12Device*>(core.GetDeviceNativeObject(device));
		if (d3d12Device == nullptr)
		{
			return;
		}

		ID3D12InfoQueue* infoQueue = nullptr;
		if (FAILED(d3d12Device->QueryInterface(IID_PPV_ARGS(&infoQueue))) || infoQueue == nullptr)
		{
			return;
		}

		const UINT64 messageCount = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
		if (messageCount == 0)
		{
			infoQueue->Release();
			return;
		}

		const UINT64 start = messageCount > 16 ? messageCount - 16 : 0;
		for (UINT64 i = start; i < messageCount; ++i)
		{
			SIZE_T messageSize = 0;
			if (FAILED(infoQueue->GetMessage(i, nullptr, &messageSize)) || messageSize == 0)
			{
				continue;
			}

			std::vector<uint8_t> storage(messageSize);
			auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
			if (FAILED(infoQueue->GetMessage(i, message, &messageSize)))
			{
				continue;
			}

			Printf(TEXTCOLOR_RED "NRI D3D12 info queue after %s [%u]: %s\n",
				context != nullptr ? context : "unknown",
				(unsigned)message->ID,
				message->pDescription != nullptr ? message->pDescription : "(no description)");
		}

		infoQueue->ClearStoredMessages();
		infoQueue->Release();
	}

	static NRIRenderDevice* GetActiveNRIRenderDevice()
	{
		if (screen == nullptr || screen->Backend() != 4)
		{
			Printf("The NRI backend is not active.\n");
			return nullptr;
		}

		return static_cast<NRIRenderDevice*>(screen);
	}

	class ScopedNriTiming
	{
	public:
		ScopedNriTiming(glcycle_t& aggregate, double& outputMs)
			: mAggregate(aggregate), mOutputMs(outputMs)
		{
			mOutputMs = 0.0;
			mAggregate.Clock();
			mTimer.ResetAndClock();
		}

		~ScopedNriTiming()
		{
			mTimer.Unclock();
			mOutputMs = mTimer.TimeMS();
			mAggregate.Unclock();
		}

		ScopedNriTiming(const ScopedNriTiming&) = delete;
		ScopedNriTiming& operator=(const ScopedNriTiming&) = delete;

	private:
		glcycle_t& mAggregate;
		double& mOutputMs;
		cycle_t mTimer;
	};

	static const char* GetNriResultName(nri::Result result)
	{
		switch (result)
		{
		case nri::Result::INVALID_SDK:
			return "invalid_sdk";
		case nri::Result::SUCCESS:
			return "success";
		case nri::Result::FAILURE:
			return "failure";
		case nri::Result::INVALID_ARGUMENT:
			return "invalid_argument";
		case nri::Result::OUT_OF_MEMORY:
			return "out_of_memory";
		case nri::Result::UNSUPPORTED:
			return "unsupported";
		case nri::Result::DEVICE_LOST:
			return "device_lost";
		case nri::Result::OUT_OF_DATE:
			return "out_of_date";
		default:
			return "other";
		}
	}

	static const char* GetNriMessageTypeName(nri::Message messageType)
	{
		switch (messageType)
		{
		case nri::Message::INFO:
			return "info";
		case nri::Message::WARNING:
			return "warning";
		case nri::Message::ERROR:
			return "error";
		default:
			return "other";
		}
	}

	static const char* GetNriVendorName(nri::Vendor vendor)
	{
		switch (vendor)
		{
		case nri::Vendor::NVIDIA:
			return "NVIDIA";
		case nri::Vendor::AMD:
			return "AMD";
		case nri::Vendor::INTEL:
			return "Intel";
		default:
			return "Unknown";
		}
	}

	static void NRI_CALL NriMessageCallback(nri::Message messageType, const char* file, uint32_t line, const char* message, void*)
	{
		if (file != nullptr && *file != '\0')
		{
			Printf("NRI %s: %s (%s:%u)\n", GetNriMessageTypeName(messageType), message, file, line);
		}
		else
		{
			Printf("NRI %s: %s\n", GetNriMessageTypeName(messageType), message);
		}
	}

	static uint32_t CountSetBits(uint64_t mask)
	{
		uint32_t count = 0;
		while (mask != 0)
		{
			count += (uint32_t)(mask & 1ull);
			mask >>= 1;
		}
		return count;
	}

	static FString DescribeSwapChainFlags(nri::SwapChainBits flags)
	{
		if (flags == nri::SwapChainBits::NONE)
		{
			return "NONE";
		}

		FString description;
		const auto appendFlag = [&description](const char* text)
		{
			if (!description.IsEmpty())
			{
				description << "|";
			}
			description << text;
		};

		if (((uint8_t)flags & (uint8_t)nri::SwapChainBits::VSYNC) != 0)
		{
			appendFlag("VSYNC");
		}

		if (((uint8_t)flags & (uint8_t)nri::SwapChainBits::ALLOW_TEARING) != 0)
		{
			appendFlag("ALLOW_TEARING");
		}

		if (((uint8_t)flags & (uint8_t)nri::SwapChainBits::WAITABLE) != 0)
		{
			appendFlag("WAITABLE");
		}

		if (((uint8_t)flags & (uint8_t)nri::SwapChainBits::ALLOW_LOW_LATENCY) != 0)
		{
			appendFlag("ALLOW_LOW_LATENCY");
		}

		return description;
	}

	static const char* GetSwapChainFormatName(nri::SwapChainFormat format)
	{
		switch (format)
		{
		case nri::SwapChainFormat::BT709_G10_16BIT: return "BT709_G10_16BIT";
		case nri::SwapChainFormat::BT709_G22_8BIT: return "BT709_G22_8BIT";
		case nri::SwapChainFormat::BT709_G22_10BIT: return "BT709_G22_10BIT";
		case nri::SwapChainFormat::BT2020_G2084_10BIT: return "BT2020_G2084_10BIT";
		default: return "unknown";
		}
	}

	static const char* GetNriFormatName(nri::Format format)
	{
		switch (format)
		{
		case nri::Format::UNKNOWN: return "UNKNOWN";
		case nri::Format::RGBA16_SFLOAT: return "RGBA16_SFLOAT";
		case nri::Format::BGRA8_UNORM: return "BGRA8_UNORM";
		case nri::Format::RGBA8_UNORM: return "RGBA8_UNORM";
		case nri::Format::BGRA8_SRGB: return "BGRA8_SRGB";
		case nri::Format::RGBA8_SRGB: return "RGBA8_SRGB";
		case nri::Format::R10_G10_B10_A2_UNORM: return "R10_G10_B10_A2_UNORM";
		default: return "other";
		}
	}

	static uint32_t GetScreenshotSourceBytesPerPixel(nri::Format format)
	{
		switch (format)
		{
		case nri::Format::BGRA8_UNORM:
		case nri::Format::RGBA8_UNORM:
		case nri::Format::BGRA8_SRGB:
		case nri::Format::RGBA8_SRGB:
		case nri::Format::R10_G10_B10_A2_UNORM:
			return 4;
		case nri::Format::RGBA16_SFLOAT:
			return 8;
		default:
			return 0;
		}
	}

	static float HalfToFloat(uint16_t value)
	{
		const uint32_t sign = (value >> 15) & 1u;
		const uint32_t exponent = (value >> 10) & 0x1fu;
		const uint32_t mantissa = value & 0x3ffu;

		float result = 0.0f;
		if (exponent == 0)
		{
			result = mantissa == 0 ? 0.0f : std::ldexp((float)mantissa, -24);
		}
		else if (exponent == 31)
		{
			result = mantissa == 0 ? std::numeric_limits<float>::infinity() : std::numeric_limits<float>::quiet_NaN();
		}
		else
		{
			result = std::ldexp(1.0f + (float)mantissa / 1024.0f, (int)exponent - 15);
		}

		return sign ? -result : result;
	}

	static uint8_t FloatToByte(float value)
	{
		if (!std::isfinite(value))
		{
			value = 0.0f;
		}
		value = std::clamp(value, 0.0f, 1.0f);
		return (uint8_t)std::lround(value * 255.0f);
	}

	static NRIPTOutputMode GetRequestedPathTracingOutputMode()
	{
		switch ((int)nri_ptoutputmode)
		{
		case 1: return NRIPTOutputMode::HDR;
		default: return NRIPTOutputMode::SDR;
		}
	}

	static NRIPTTonemapMode GetRequestedPathTracingTonemapMode(bool hdrControlsActive)
	{
		const int tonemapValue = hdrControlsActive ? (int)nri_pthdrtonemap : (int)nri_pttonemap;
		switch (tonemapValue)
		{
		case 1: return NRIPTTonemapMode::ACESFitted;
		case 2: return NRIPTTonemapMode::Reinhard;
		default: return NRIPTTonemapMode::Hable;
		}
	}

	static float GetRequestedPathTracingExposure(bool hdrControlsActive)
	{
		return hdrControlsActive ? (float)nri_pthdrexposure : (float)nri_ptexposure;
	}

	static float GetRequestedPathTracingContrast(bool hdrControlsActive)
	{
		return hdrControlsActive ? (float)nri_pthdrcontrast : (float)nri_ptcontrast;
	}

	static float GetRequestedPathTracingSaturation(bool hdrControlsActive)
	{
		return hdrControlsActive ? (float)nri_pthdrsaturation : (float)nri_ptsaturation;
	}

	static float GetRequestedPathTracingShoulder(bool hdrControlsActive)
	{
		return hdrControlsActive ? (float)nri_pthdrshoulder : (float)nri_ptshoulder;
	}

	static float GetRequestedPathTracingToe(bool hdrControlsActive)
	{
		return hdrControlsActive ? (float)nri_pthdrtoe : (float)nri_pttoe;
	}

	static bool IsHdrSwapChainFormat(nri::SwapChainFormat format)
	{
		return format == nri::SwapChainFormat::BT709_G10_16BIT || format == nri::SwapChainFormat::BT2020_G2084_10BIT;
	}

	static nri::Format GetExpectedResolvedTextureFormatForSwapChainFormat(nri::SwapChainFormat format)
	{
		switch (format)
		{
		case nri::SwapChainFormat::BT709_G10_16BIT: return nri::Format::RGBA16_SFLOAT;
		case nri::SwapChainFormat::BT2020_G2084_10BIT: return nri::Format::R10_G10_B10_A2_UNORM;
		default: return nri::Format::UNKNOWN;
		}
	}

	static NRIPTOutputMode GetResolvedPathTracingOutputModeForSwapChainFormat(nri::SwapChainFormat format)
	{
		switch (format)
		{
		case nri::SwapChainFormat::BT709_G10_16BIT: return NRIPTOutputMode::HDRLinear16;
		case nri::SwapChainFormat::BT2020_G2084_10BIT: return NRIPTOutputMode::HDR10PQ;
		default: return NRIPTOutputMode::SDR;
		}
	}

	static bool IsHdrRequestDefinitivelyUnavailableReason(const char* reason)
	{
		if (reason == nullptr || *reason == '\0')
		{
			return false;
		}

		return
			stricmp(reason, "hdr-display-sdr") == 0 ||
			stricmp(reason, "hdr-unsupported-api") == 0 ||
			stricmp(reason, "hdr-swapchain-create-failed-fallback-sdr") == 0 ||
			stricmp(reason, "hdr-wrap-format-mismatch-fallback-sdr") == 0;
	}

	static bool DisplayLuminanceChanged(float previousValue, float currentValue)
	{
		return std::fabs(previousValue - currentValue) > 0.05f;
	}

	static bool HasDisplayDescChanged(const nri::DisplayDesc& previousDesc, const nri::DisplayDesc& currentDesc)
	{
		return
			previousDesc.isHDR != currentDesc.isHDR ||
			DisplayLuminanceChanged(previousDesc.sdrLuminance, currentDesc.sdrLuminance) ||
			DisplayLuminanceChanged(previousDesc.maxLuminance, currentDesc.maxLuminance);
	}

	static FString DescribeSwapChainImageMask(uint64_t mask, uint32_t textureCount)
	{
		if (textureCount == 0)
		{
			return "none";
		}

		FString description;
		for (uint32_t i = 0; i < textureCount; ++i)
		{
			if ((mask & (1ull << i)) == 0)
			{
				continue;
			}

			if (!description.IsEmpty())
			{
				description << ",";
			}
			description.AppendFormat("%u", i);
		}

		if (description.IsEmpty())
		{
			description = "none";
		}

		return description;
	}

	static FString DescribeSwapChainImageCounts(const std::vector<uint64_t>& counts)
	{
		if (counts.empty())
		{
			return "none";
		}

		FString description;
		for (size_t i = 0; i < counts.size(); ++i)
		{
			if (i != 0)
			{
				description << " ";
			}
			description.AppendFormat("%u:%llu", (uint32_t)i, (unsigned long long)counts[i]);
		}

		return description;
	}

	static uint8_t GetRequestedSwapChainTextureCount()
	{
		if (nri_ptswaptextures > 0)
		{
			return (uint8_t)nri_ptswaptextures;
		}

		return DefaultSwapChainTextureCount;
	}

	static nri::SwapChainBits GetRequestedSwapChainFlags()
	{
		switch ((int)nri_ptswapflags)
		{
		case 0:
			return nri::SwapChainBits::NONE;
		case 1:
			return nri::SwapChainBits::ALLOW_TEARING;
		case 2:
			return nri::SwapChainBits::VSYNC;
		case 3:
			return NRIFlags(nri::SwapChainBits::VSYNC, nri::SwapChainBits::ALLOW_TEARING);
		default:
			return vid_vsync ? nri::SwapChainBits::VSYNC : nri::SwapChainBits::ALLOW_TEARING;
		}
	}

	static bool HasRequestedFrameGenerationProvider()
	{
		return (int)nri_framegenprovider != 0;
	}

	static const char* DescribeSwapChainFlagOverride()
	{
		switch ((int)nri_ptswapflags)
		{
		case -1: return "default";
		case 0: return "NONE";
		case 1: return "ALLOW_TEARING";
		case 2: return "VSYNC";
		case 3: return "VSYNC|ALLOW_TEARING";
		default: return "invalid";
		}
	}

}

extern "C" nri::Result NRI_CALL nriGetInterface(const nri::Device& device, const char* interfaceName, size_t interfaceSize, void* interfacePtr)
{
	return gNriGetInterfaceForwarder != nullptr ? gNriGetInterfaceForwarder(device, interfaceName, interfaceSize, interfacePtr) : nri::Result::FAILURE;
}

extern "C" void NRI_CALL nriDestroyDevice(nri::Device* device)
{
	if (gNriDestroyDeviceForwarder != nullptr)
	{
		gNriDestroyDeviceForwarder(device);
	}
}

CCMD(nri_ptcaps)
{
	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->PrintPathTracingCaps();
	}
}

CCMD(nri_pt360absence_selftest)
{
	std::string failure;
	const bool passed = RunNRISpatialAbsenceGateSelfTests(&failure);
	Printf("NRI PT 360 absence selftest: result=%s reason=%s\n",
		passed ? "pass" : "fail",
		failure.empty() ? "none" : failure.c_str());
}

// Compact diagnostic command used by the deterministic motion harness. A
// single command keeps a per-frame arbitrary-pixel GPU probe replay below the
// Windows process command-line limit while still going through the validated
// session-only CVars consumed by the frame snapshot.
CCMD(nri_pt360probestep)
{
	if (argv.argc() != 8)
	{
		Printf("nri_pt360probestep <origin-x> <origin-y> <origin-z> <target-x> <target-y> <reference-x> <reference-y>\n");
		return;
	}

	nri_pt360absenceprobeoriginx = (float)atof(argv[1]);
	nri_pt360absenceprobeoriginy = (float)atof(argv[2]);
	nri_pt360absenceprobeoriginz = (float)atof(argv[3]);
	nri_pt360absenceprobetargetx = atoi(argv[4]);
	nri_pt360absenceprobetargety = atoi(argv[5]);
	nri_pt360absenceprobereferencex = atoi(argv[6]);
	nri_pt360absenceprobereferencey = atoi(argv[7]);
}

CCMD(nri_ptvoxeloccurrence_selftest)
{
	std::string failure;
	const bool passed = RunNRIActorOccurrenceSelfTests(&failure);
	Printf("NRI PT actor occurrence selftest: result=%s reason=%s\n",
		passed ? "pass" : "fail",
		failure.empty() ? "none" : failure.c_str());
}

CCMD(nri_ptstatus)
{
	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->PrintPathTracingStatus();
	}
}

CCMD(nri_ptsmokestatus)
{
	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->PrintPathTracingSmokeStatus();
	}
}

CCMD(nri_ptsmokereset)
{
	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->ResetPathTracingSmoke();
	}
}

CCMD(nri_ptsmoke_test)
{
	auto* frameBuffer = GetActiveNRIRenderDevice();
	if (frameBuffer == nullptr)
	{
		Printf("nri_ptsmoke_test is only available while using the NRI renderer.\n");
		return;
	}
	if (argv.argc() < 2)
	{
		frameBuffer->QueueSyntheticPathTracingSmoke();
		Printf("NRI PT smoke synthetic injection queued. Use nri_ptsmoke_test <event_rule_id> to test authored weapon-event fan-out.\n");
		return;
	}

	const ResolvedLightOverlaySet& resolvedSet = GetResolvedLightOverlaySet();
	const char* ruleId = argv[1];
	const ResolvedLightOverlaySmokeEventRule* rule = FindResolvedSmokeEventRule(resolvedSet, ruleId);
	if (rule == nullptr || !rule->styleResolved)
	{
		Printf("nri_ptsmoke_test: no resolved smoke-event rule '%s'. available=%s\n",
			ruleId,
			DescribeResolvedSmokeEventRuleIds(resolvedSet).GetChars());
		return;
	}

	PathTracingWeaponLightEvent event;
	FString error;
	if (!BuildLocalPlayerWeaponLightEvent(rule->id.GetChars(), DefaultPtTestLightOffset, event, error))
	{
		Printf("nri_ptsmoke_test: %s.\n", error.GetChars());
		return;
	}

	frameBuffer->EmitPathTracingWeaponLightEvent(event);
	Printf("NRI PT smoke event test queued: event=%s actor=%d world_pos=(%.3f, %.3f, %.3f) time=%.4f pending=%u source=%s\n",
		event.eventId.GetChars(),
		event.emitterActorIndex,
		event.worldPosition.X,
		event.worldPosition.Y,
		event.worldPosition.Z,
		event.absoluteTimeSeconds,
		frameBuffer->GetPendingPathTracingWeaponLightEventCount(),
		rule->source.sourceName.GetChars());
}

CCMD(nri_ptchunkdump)
{
	const int32_t chunkIndex = argv.argc() > 1 ? atoi(argv[1]) : -1;
	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->PrintPathTracingMapChunkDump(chunkIndex);
	}
	else
	{
		Printf("nri_ptchunkdump is only available while using the NRI renderer.\n");
	}
}

CCMD(nri_ptchunkcompare)
{
	const int32_t chunkIndex = argv.argc() > 1 ? atoi(argv[1]) : -1;
	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->PrintPathTracingMapChunkCompare(chunkIndex);
	}
	else
	{
		Printf("nri_ptchunkcompare is only available while using the NRI renderer.\n");
	}
}

CCMD(nri_ptbuffers)
{
	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->PrintPathTracingBuffers();
	}
}

CCMD(nri_ptreset)
{
	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->ResetPathTracingHistory();
	}
}

CCMD(nri_ptautoexposurereset)
{
	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->ResetPathTracingAutoExposure();
	}
	else
	{
		Printf("NRI PT auto exposure reset is only available while using the NRI renderer.\n");
	}
}

CCMD(nri_ptlightspawn)
{
	if (argv.argc() < 5)
	{
		Printf("nri_ptlightspawn <r> <g> <b> <intensity> [radius] [offset]: spawns a PT test point light in front of the local player.\n");
		return;
	}

	auto* frameBuffer = GetActiveNRIRenderDevice();
	if (frameBuffer == nullptr)
	{
		Printf("nri_ptlightspawn is only available while using the NRI renderer.\n");
		return;
	}

	const float radius = argv.argc() > 5 ? (float)atof(argv[5]) : DefaultPtTestLightRadius;
	const float offset = argv.argc() > 6 ? (float)atof(argv[6]) : DefaultPtTestLightOffset;
	uint32_t lightId = 0;
	frameBuffer->SpawnPathTracingPointLight(
		(float)atof(argv[1]),
		(float)atof(argv[2]),
		(float)atof(argv[3]),
		(float)atof(argv[4]),
		radius,
		offset,
		lightId);
}

CCMD(nri_ptlightlist)
{
	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->PrintPathTracingPointLights();
	}
	else
	{
		Printf("nri_ptlightlist is only available while using the NRI renderer.\n");
	}
}

CCMD(nri_ptmuzzleflash_test)
{
	if (argv.argc() < 2)
	{
		Printf("nri_ptmuzzleflash_test <rule_id>: emits one synthetic PT muzzle-flash event from the local player.\n");
		return;
	}

	auto* frameBuffer = GetActiveNRIRenderDevice();
	if (frameBuffer == nullptr)
	{
		Printf("nri_ptmuzzleflash_test is only available while using the NRI renderer.\n");
		return;
	}

	const ResolvedLightOverlaySet& resolvedSet = GetResolvedLightOverlaySet();
	const char* ruleId = argv[1];
	const ResolvedLightOverlayMuzzleFlashRule* rule = FindResolvedMuzzleFlashRule(resolvedSet, ruleId);
	if (rule == nullptr)
	{
		Printf("nri_ptmuzzleflash_test: no resolved muzzle-flash rule '%s'. available=%s\n",
			ruleId,
			DescribeResolvedMuzzleFlashRuleIds(resolvedSet).GetChars());
		return;
	}

	PathTracingWeaponLightEvent event;
	FString error;
	if (!BuildLocalPlayerWeaponLightEvent(rule->id.GetChars(), DefaultPtTestLightOffset, event, error))
	{
		Printf("nri_ptmuzzleflash_test: %s.\n", error.GetChars());
		return;
	}

	frameBuffer->EmitPathTracingWeaponLightEvent(event);
	Printf("NRI PT muzzle-flash test queued: event=%s actor=%d world_pos=(%.3f, %.3f, %.3f) time=%.4f pending=%u source=%s\n",
		event.eventId.GetChars(),
		event.emitterActorIndex,
		event.worldPosition.X,
		event.worldPosition.Y,
		event.worldPosition.Z,
		event.absoluteTimeSeconds,
		frameBuffer->GetPendingPathTracingWeaponLightEventCount(),
		rule->source.sourceName.GetChars());
}

CCMD(nri_ptsphere)
{
	if (argv.argc() < 3)
	{
		Printf("nri_ptsphere <diameter> <distance> [metalness] [roughness]: spawns a PT debug sphere along camera forward.\n");
		return;
	}

	auto* frameBuffer = GetActiveNRIRenderDevice();
	if (frameBuffer == nullptr)
	{
		Printf("nri_ptsphere is only available while using the NRI renderer.\n");
		return;
	}

	uint32_t sphereId = 0;
	frameBuffer->SpawnPathTracingDebugSphere(
		(float)atof(argv[1]),
		(float)atof(argv[2]),
		argv.argc() > 3 ? (float)atof(argv[3]) : DefaultPtTestSphereMetalness,
		argv.argc() > 4 ? (float)atof(argv[4]) : DefaultPtTestSphereRoughness,
		sphereId);
}

CCMD(nri_ptspherespawn)
{
	if (argv.argc() < 3)
	{
		Printf("nri_ptspherespawn <diameter> <distance> [metalness] [roughness]: spawns a PT debug sphere along camera forward.\n");
		return;
	}

	auto* frameBuffer = GetActiveNRIRenderDevice();
	if (frameBuffer == nullptr)
	{
		Printf("nri_ptspherespawn is only available while using the NRI renderer.\n");
		return;
	}

	uint32_t sphereId = 0;
	frameBuffer->SpawnPathTracingDebugSphere(
		(float)atof(argv[1]),
		(float)atof(argv[2]),
		argv.argc() > 3 ? (float)atof(argv[3]) : DefaultPtTestSphereMetalness,
		argv.argc() > 4 ? (float)atof(argv[4]) : DefaultPtTestSphereRoughness,
		sphereId);
}

CCMD(nri_ptspherelist)
{
	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->PrintPathTracingDebugSpheres();
	}
	else
	{
		Printf("nri_ptspherelist is only available while using the NRI renderer.\n");
	}
}

CCMD(nri_ptsphereclear)
{
	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->ClearPathTracingDebugSpheres();
	}
	else
	{
		Printf("nri_ptsphereclear is only available while using the NRI renderer.\n");
	}
}

CCMD(nri_ptsphereremove)
{
	if (argv.argc() < 2)
	{
		Printf("nri_ptsphereremove <id>: removes a PT debug sphere by id.\n");
		return;
	}

	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->RemovePathTracingDebugSphere((uint32_t)atoi(argv[1]));
	}
	else
	{
		Printf("nri_ptsphereremove is only available while using the NRI renderer.\n");
	}
}

CCMD(nri_ptlightheuristic_addsprite)
{
	if (argv.argc() < 7)
	{
		Printf("nri_ptlightheuristic_addsprite <tile> <r> <g> <b> <intensity> <radius> [flicker_frames]: adds a PT analytic sprite-tile light heuristic.\n");
		return;
	}

	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		uint32_t ruleId = 0;
		frameBuffer->AddPathTracingSpriteTileLightHeuristic(
			(uint32_t)atoi(argv[1]),
			(float)atof(argv[2]),
			(float)atof(argv[3]),
			(float)atof(argv[4]),
			(float)atof(argv[5]),
			(float)atof(argv[6]),
			argv.argc() > 7 ? (uint32_t)atoi(argv[7]) : 0u,
			ruleId);
	}
	else
	{
		Printf("nri_ptlightheuristic_addsprite is only available while using the NRI renderer.\n");
	}
}

CCMD(nri_ptlightheuristic_clear)
{
	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->ClearPathTracingLightHeuristics();
	}
	else
	{
		Printf("nri_ptlightheuristic_clear is only available while using the NRI renderer.\n");
	}
}

CCMD(nri_ptlightheuristic_list)
{
	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->PrintPathTracingLightHeuristics();
	}
	else
	{
		Printf("nri_ptlightheuristic_list is only available while using the NRI renderer.\n");
	}
}

CCMD(nri_ptlightclear)
{
	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->ClearPathTracingPointLights();
	}
	else
	{
		Printf("nri_ptlightclear is only available while using the NRI renderer.\n");
	}
}

CCMD(nri_ptlightremove)
{
	if (argv.argc() < 2)
	{
		Printf("nri_ptlightremove <id>: removes a PT test point light by id.\n");
		return;
	}

	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->RemovePathTracingPointLight((uint32_t)atoi(argv[1]));
	}
	else
	{
		Printf("nri_ptlightremove is only available while using the NRI renderer.\n");
	}
}

CCMD(nri_ptlightdump)
{
	const float radius = argv.argc() > 1 ? (float)atof(argv[1]) : 2048.0f;
	const uint32_t limit = argv.argc() > 2 ? (uint32_t)atoi(argv[2]) : 32u;
	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->PrintPathTracingSceneLightDump(radius, limit);
	}
	else
	{
		Printf("nri_ptlightdump is only available while using the NRI renderer.\n");
	}
}

CCMD(nri_ptlightdebug_nearplayer)
{
	const float radius = argv.argc() > 1 ? (float)atof(argv[1]) : 1024.0f;
	const uint32_t limit = argv.argc() > 2 ? (uint32_t)atoi(argv[2]) : 16u;
	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->PrintPathTracingSceneLightDump(radius, limit);
	}
	else
	{
		Printf("nri_ptlightdebug_nearplayer is only available while using the NRI renderer.\n");
	}
}

CCMD(nri_ptlightclusterdebug)
{
	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->PrintPathTracingLightClusters();
	}
	else
	{
		Printf("nri_ptlightclusterdebug is only available while using the NRI renderer.\n");
	}
}

CCMD(nri_ptemissiveheuristic_addtile)
{
	if (argv.argc() < 2)
	{
		Printf("nri_ptemissiveheuristic_addtile <tile> [intensity_scale]: adds a PT emissive tile rule using base-texture emission.\n");
		return;
	}

	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		uint32_t ruleId = 0;
		frameBuffer->AddPathTracingTextureEmissiveHeuristic(
			(uint32_t)atoi(argv[1]),
			nri_scene::MaterialEmissiveMode_UseBaseTexture,
			argv.argc() > 2 ? (float)atof(argv[2]) : 1.0f,
			nullptr,
			false,
			ruleId);
	}
	else
	{
		Printf("nri_ptemissiveheuristic_addtile is only available while using the NRI renderer.\n");
	}
}

CCMD(nri_ptemissiveheuristic_addtilemode)
{
	if (argv.argc() < 3)
	{
		Printf("nri_ptemissiveheuristic_addtilemode <tile> <base|glowmap|constant> [intensity_scale] [r g b]: adds a PT emissive tile rule with an explicit source mode.\n");
		return;
	}

	uint32_t emissiveMode = nri_scene::MaterialEmissiveMode_None;
	const char* modeName = argv[2];
	if (!stricmp(modeName, "base") || !stricmp(modeName, "albedo") || !stricmp(modeName, "texture"))
	{
		emissiveMode = nri_scene::MaterialEmissiveMode_UseBaseTexture;
	}
	else if (!stricmp(modeName, "glowmap") || !stricmp(modeName, "glow"))
	{
		emissiveMode = nri_scene::MaterialEmissiveMode_UseGlowmapTexture;
	}
	else if (!stricmp(modeName, "constant") || !stricmp(modeName, "const"))
	{
		emissiveMode = nri_scene::MaterialEmissiveMode_UseConstantColor;
	}
	else
	{
		Printf("Unknown emissive mode '%s'. Expected one of: base, glowmap, constant.\n", modeName);
		return;
	}

	const float intensityScale = argv.argc() > 3 ? (float)atof(argv[3]) : 1.0f;
	bool hasExplicitColor = false;
	float emissiveColor[3] = { 1.0f, 1.0f, 1.0f };
	if (emissiveMode == nri_scene::MaterialEmissiveMode_UseConstantColor && argv.argc() >= 7)
	{
		emissiveColor[0] = (float)atof(argv[4]);
		emissiveColor[1] = (float)atof(argv[5]);
		emissiveColor[2] = (float)atof(argv[6]);
		hasExplicitColor = true;
	}

	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		uint32_t ruleId = 0;
		frameBuffer->AddPathTracingTextureEmissiveHeuristic(
			(uint32_t)atoi(argv[1]),
			emissiveMode,
			intensityScale,
			hasExplicitColor ? emissiveColor : nullptr,
			hasExplicitColor,
			ruleId);
	}
	else
	{
		Printf("nri_ptemissiveheuristic_addtilemode is only available while using the NRI renderer.\n");
	}
}

CCMD(nri_ptemissiveheuristic_clear)
{
	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->ClearPathTracingEmissiveHeuristics();
	}
	else
	{
		Printf("nri_ptemissiveheuristic_clear is only available while using the NRI renderer.\n");
	}
}

CCMD(nri_ptemissiveheuristic_list)
{
	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->PrintPathTracingEmissiveHeuristics();
	}
	else
	{
		Printf("nri_ptemissiveheuristic_list is only available while using the NRI renderer.\n");
	}
}

CCMD(nri_ptemissivedump)
{
	const float radius = argv.argc() > 1 ? (float)atof(argv[1]) : 2048.0f;
	const uint32_t limit = argv.argc() > 2 ? (uint32_t)atoi(argv[2]) : 32u;
	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->PrintPathTracingEmissiveSurfaces(radius, limit);
	}
	else
	{
		Printf("nri_ptemissivedump is only available while using the NRI renderer.\n");
	}
}

CCMD(nri_ptsectorlight_set)
{
	if (argv.argc() < 4)
	{
		Printf("nri_ptsectorlight_set <ambientScale> <hemiScale> <fogScale>: updates PT sector-light heuristic scales.\n");
		return;
	}

	nri_ptsectorambientscale = (float)atof(argv[1]);
	nri_ptsectorhemiscale = (float)atof(argv[2]);
	nri_ptsectorfogscale = (float)atof(argv[3]);
	Printf("NRI PT sector-light scales set: ambient=%.3f hemi=%.3f fog=%.3f\n",
		(float)nri_ptsectorambientscale,
		(float)nri_ptsectorhemiscale,
		(float)nri_ptsectorfogscale);
}

CCMD(nri_ptsectorlight_filter)
{
	if (argv.argc() < 4)
	{
		Printf("nri_ptsectorlight_filter <pal|-1> <minShade> <maxShade> [lotag]: updates PT sector-light heuristic filters.\n");
		return;
	}

	nri_ptsectorfilterpal = atoi(argv[1]);
	nri_ptsectorfilterminshade = atoi(argv[2]);
	nri_ptsectorfiltermaxshade = atoi(argv[3]);
	nri_ptsectorfilterlotag = argv.argc() > 4 ? atoi(argv[4]) : -1;
	Printf("NRI PT sector-light filter set: pal=%d shade=[%d,%d] lotag=%d\n",
		(int)nri_ptsectorfilterpal,
		(int)nri_ptsectorfilterminshade,
		(int)nri_ptsectorfiltermaxshade,
		(int)nri_ptsectorfilterlotag);
}

CCMD(nri_ptsectorlight_clear)
{
	nri_ptsectorlighting = true;
	nri_ptsectorlightmultiplier = 0.0f;
	nri_ptsectorambientscale = 0.20f;
	nri_ptsectorhemiscale = 0.12f;
	nri_ptsectorfogscale = 0.20f;
	nri_ptsectorclamp = 1.0f;
	nri_ptsectorfilterpal = -1;
	nri_ptsectorfilterminshade = -128;
	nri_ptsectorfiltermaxshade = 127;
	nri_ptsectorfilterlotag = -1;
	nri_ptsectorpulseframes = 0;
	nri_ptsectorpulseamount = 0.0f;
	nri_ptsectoremissionsignalstrength = 4.0f;
	nri_ptsectoremissionresponsemin = 0.0f;
	nri_ptsectoremissionresponsemax = 2.0f;
	nri_ptsectoremissionlightmin = 0.0f;
	nri_ptsectoremissionlightmax = 1.0f;
	nri_ptsectoremissionreachmin = 0.0f;
	nri_ptsectoremissionreachmax = 1.6f;
	nri_ptsectoremissionmaterialmin = 0.0f;
	nri_ptsectoremissionmaterialmax = 1.0f;
	Printf("NRI PT sector-light heuristics cleared.\n");
}

CCMD(nri_ptsectorlightdump)
{
	const float radius = argv.argc() > 1 ? (float)atof(argv[1]) : 2048.0f;
	const uint32_t limit = argv.argc() > 2 ? (uint32_t)atoi(argv[2]) : 32u;
	if (auto* frameBuffer = GetActiveNRIRenderDevice())
	{
		frameBuffer->PrintPathTracingSectorLights(radius, limit);
	}
	else
	{
		Printf("nri_ptsectorlightdump is only available while using the NRI renderer.\n");
	}
}

NRIRenderDevice::NRIRenderDevice(void* hMonitor, bool fullscreen)
	: SystemBaseFrameBuffer(hMonitor, fullscreen), mRenderState(std::make_unique<NRIRenderState>(this))
{
	vendorstring = "NRI";
	glslversion = 6.6f;
	mRenderer = std::make_unique<NRIRenderer>(this);
	mGpuTiming = std::make_unique<NRIGpuTiming>();
}

NRIRenderDevice::~NRIRenderDevice()
{
	mShuttingDown = true;
	WaitForCommands(true);
	if (mGpuTiming != nullptr && mDevice != nullptr)
	{
		mGpuTiming->Destroy(mCore);
	}

	delete mVertexData;
	mVertexData = nullptr;
	delete mSkyData;
	mSkyData = nullptr;
	delete mViewpoints;
	mViewpoints = nullptr;
	delete mLights;
	mLights = nullptr;
	delete mBones;
	mBones = nullptr;

	DestroySwapChain();
	mFrameGeneration.Shutdown();
	if (mRenderer != nullptr)
	{
		mRenderer->Shutdown();
	}
	DestroyRenderResources();

	DestroyQueuedFrames();

	if (mFrameFence != nullptr)
	{
		mCore.DestroyFence(mFrameFence);
		mFrameFence = nullptr;
	}
	if (mCommandCompletionFence != nullptr)
	{
		mCore.DestroyFence(mCommandCompletionFence);
		mCommandCompletionFence = nullptr;
	}

	if (mStreamerInstance != nullptr)
	{
		mStreamer.DestroyStreamer(mStreamerInstance);
		mStreamerInstance = nullptr;
	}

	if (mDevice != nullptr && mDestroyDeviceFn != nullptr)
	{
		mDestroyDeviceFn(mDevice);
		mDevice = nullptr;
	}

	if (mNriModule != nullptr)
	{
		FreeLibrary((HMODULE)mNriModule);
		mNriModule = nullptr;
	}

	gNriDestroyDeviceForwarder = nullptr;
	gNriGetInterfaceForwarder = nullptr;
}

void NRIRenderDevice::Update()
{
	double draw2DMs = 0.0;
	double endFrameMs = 0.0;
	double presentShellMs = 0.0;
	double baseUpdateMs = 0.0;
	const double updateStartMs = I_msTimeF();

	if (mInitialized && mFrameBegun)
	{
		double stageStartMs = I_msTimeF();
		if (mFrameGenerationUiTargetActive)
		{
			const bool useUiLocalCompositeFallback = ShouldUseFrameGenerationUiLocalCompositeFallback();
			const uint32_t sceneBlendPrefixCount = GetFrameGenerationSceneBlendPrefixCount();
			if (sceneBlendPrefixCount > 0u)
			{
				SetActiveRenderTarget();
				DrawFrameGenerationSceneBlendPrefix();
				if (NRITextureResource* uiTarget = GetFrameGenerationUiTargetResource(); uiTarget != nullptr)
				{
					mActiveTarget = uiTarget;
					mFrameGenerationUiTargetActive = true;
				}
			}

			Draw2D();
			twod->Clear();
			FinalizeFrameGenerationUiTarget();
			if (useUiLocalCompositeFallback)
			{
				CompositeFrameGenerationUiTexture();
				Draw2D();
				twod->Clear();
			}
		}
		else
		{
			SetActiveRenderTarget();
			Draw2D();
			twod->Clear();
		}

		if (mPendingViewSnapshotCanvas != nullptr && mCurrentPresentTarget != nullptr)
		{
			auto* canvas = mPendingViewSnapshotCanvas;
			mPendingViewSnapshotCanvas = nullptr;
			SnapshotTextureToCanvas(canvas, *mCurrentPresentTarget);
		}

		RecordPendingScreenshotReadbacks();

		draw2DMs = I_msTimeF() - stageStartMs;
		stageStartMs = I_msTimeF();
		mRenderState->EndFrame();
		endFrameMs = I_msTimeF() - stageStartMs;
		stageStartMs = I_msTimeF();
		EndFrameAndPresent();
		presentShellMs = I_msTimeF() - stageStartMs;
	}

	double stageStartMs = I_msTimeF();
	Super::Update();
	baseUpdateMs = I_msTimeF() - stageStartMs;

	if (PerfLoopTraceActive())
	{
		Printf(
			"PERF update trace NRI: frame=%llu draw2d=%.3f endframe=%.3f present_shell=%.3f base=%.3f total=%.3f frame_begun=%d framegen_ui=%d acquired=%d presented=%d\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			draw2DMs,
			endFrameMs,
			presentShellMs,
			baseUpdateMs,
			I_msTimeF() - updateStartMs,
			mFrameBegun ? 1 : 0,
			mFrameGenerationUiTargetActive ? 1 : 0,
			mHasAcquiredSwapChainImage ? 1 : 0,
			mHasPresentedSwapChainFrame ? 1 : 0);
	}
}

void NRIRenderDevice::InitializeState()
{
	SetViewportRects(nullptr);

	StartupRecovery_UpdateStage("nri_load");
	if (!LoadNRI())
	{
		Printf(TEXTCOLOR_RED "NRI backend initialization failed.\n");
		StartupRecovery_MarkNriStartupFailure("nri_load", "load_nri_failed");
		mInitialized = false;
		return;
	}

	StartupRecovery_UpdateStage("nri_create_device");
	if (!CreateDevice())
	{
		Printf(TEXTCOLOR_RED "NRI backend initialization failed.\n");
		mInitialized = false;
		return;
	}

	StartupRecovery_UpdateStage("nri_create_render_resources");
	if (!CreateRenderResources())
	{
		Printf(TEXTCOLOR_RED "NRI backend initialization failed.\n");
		StartupRecovery_MarkNriStartupFailure("nri_create_render_resources", "create_render_resources_failed");
		mInitialized = false;
		return;
	}

	StartupRecovery_UpdateStage("nri_create_swapchain");
	if (!CreateSwapChain())
	{
		Printf(TEXTCOLOR_RED "NRI backend initialization failed.\n");
		StartupRecovery_MarkNriStartupFailure("nri_create_swapchain", "create_swapchain_failed");
		mInitialized = false;
		return;
	}
	mFrameGeneration.Initialize(*this);

	mVertexData = new FFlatVertexBuffer(GetWidth(), GetHeight(), mPipelineNbr);
	mSkyData = new FSkyVertexBuffer;
	mViewpoints = new HWViewpointBuffer(mPipelineNbr);
	mLights = new FLightBuffer(mPipelineNbr);
	mBones = new BoneBuffer(mPipelineNbr);

	LogStartup();
	if (mRenderer != nullptr && !mRenderer->Initialize())
	{
		Printf(TEXTCOLOR_RED "NRI path tracing renderer initialization failed.\n");
	}
	mInitialized = true;
}

bool NRIRenderDevice::CompileNextShader()
{
	return true;
}

int NRIRenderDevice::GetShaderCount()
{
	return 0;
}

const char* NRIRenderDevice::DeviceName() const
{
	return mDeviceName.GetChars();
}

void NRIRenderDevice::BeginFrame()
{
	if (!mInitialized)
	{
		return;
	}

	if (mFrameBegun)
	{
		return;
	}
	if (!ApplyPendingSwapChainRefresh())
	{
		return;
	}
	if (mGpuTiming != nullptr && mDevice != nullptr)
	{
		mGpuTiming->Prepare(mCore, *mDevice);
	}

	mFrameGeneration.BeginFrame(*this);

	mTraceThisFrame = false;
	if (nri_pttraceframes > 0)
	{
		mTraceThisFrame = true;
	}

	Reset2DTextureFrameStats();
	mLastFrameBoundaryStats.frameNumber++;
	mLastFrameBoundaryStats.frameIndex = mFrameIndex;
	mLastFrameBoundaryStats.waitMs = 0.0;
	mLastFrameBoundaryStats.waitForPresentMs = 0.0;
	mLastFrameBoundaryStats.acquireMs = 0.0;
	mLastFrameBoundaryStats.submitMs = 0.0;
	mLastFrameBoundaryStats.presentMs = 0.0;
	mLastFrameBoundaryStats.submittedFenceValue = 0;
	mLastFrameBoundaryStats.waitForPresentResult = nri::Result::SUCCESS;
	mLastFrameBoundaryStats.acquireResult = nri::Result::FAILURE;
	mLastFrameBoundaryStats.presentResult = nri::Result::FAILURE;
	mCurrentQueuedFrameIndex = GetQueuedFrameIndex(mFrameIndex);
	mLastFrameBoundaryStats.queuedFrameIndex = mCurrentQueuedFrameIndex;
	mLastFrameBoundaryStats.swapChainImageIndex = 0;
	mLastFrameBoundaryStats.acquireSemaphoreIndex = 0;
	mLastFrameBoundaryStats.sanityModeEnabled = !!nri_ptsanity;
	mLastFrameBoundaryStats.sanityFrameUsed = false;
	mLastFrameBoundaryStats.sceneTargetSelected = false;
	mLastFrameBoundaryStats.pathTracedSceneRendered = false;
	mLastFrameBoundaryStats.sceneCopiedToPresent = false;
	mLastFrameBoundaryStats.postProcessInvoked = false;
	SelectQueuedFrame(mCurrentQueuedFrameIndex);

	{
		ScopedNriTiming waitTiming(NriPTFrameWait, mLastFrameBoundaryStats.waitMs);
		WaitForCommands(false);
	}
	if (mTerminalDeviceLoss)
	{
		FatalTerminalDeviceLoss("Wait(FrameFenceRecycle)");
	}
	ReleaseRetiredTextureResources(false);
	SetViewportRects(nullptr);

	if (!EnsureSwapChainSize())
	{
		return;
	}

	mAcquireSemaphoreIndex = mSwapChainImages.empty() ? 0 : (uint32_t)(mFrameIndex % mSwapChainImages.size());
	mLastFrameBoundaryStats.acquireSemaphoreIndex = mAcquireSemaphoreIndex;

	if (IsFrameGenerationPresentPathActive())
	{
		IDXGISwapChain4* frameGenSwapChain = mFrameGeneration.GetPresentSwapChain();
		if (frameGenSwapChain == nullptr)
		{
			mFrameGeneration.RequestNativeFallback("present-bridge-invalidated");
			WaitForCommands(true);
			CreateSwapChain();
			return;
		}

		mCurrentSwapChainImage = frameGenSwapChain->GetCurrentBackBufferIndex();
		mLastFrameBoundaryStats.acquireResult = nri::Result::SUCCESS;
		mLastFrameBoundaryStats.swapChainImageIndex = mCurrentSwapChainImage;
		mHasAcquiredSwapChainImage = false;
		if (mCurrentSwapChainImage >= mFrameGenerationPresentImages.size())
		{
			Printf(TEXTCOLOR_RED "NRI framegen present bridge returned backbuffer index %u outside wrapped image range %u.\n",
				mCurrentSwapChainImage,
				(unsigned)mFrameGenerationPresentImages.size());
			mFrameGeneration.RequestNativeFallback("present-bridge-invalidated");
			WaitForCommands(true);
			CreateSwapChain();
			return;
		}
		mCurrentPresentTarget = &mFrameGenerationPresentImages[mCurrentSwapChainImage];
	}
	else
	{
		const bool waitableSwapChain = ((uint32_t)mSwapChainFlags & (uint32_t)nri::SwapChainBits::WAITABLE) != 0;
		const bool allowWaitForPresent = waitableSwapChain;
		if (nri_ptwaitpresent && allowWaitForPresent && mHasPresentedSwapChainFrame && mSwapChain != nullptr)
		{
			nri::Result waitForPresentResult = nri::Result::FAILURE;
			{
				ScopedNriTiming waitPresentTiming(NriPTWaitPresent, mLastFrameBoundaryStats.waitForPresentMs);
				waitForPresentResult = mSwapChainInterface.WaitForPresent(*mSwapChain);
			}
			mLastFrameBoundaryStats.waitForPresentResult = waitForPresentResult;
		}

		nri::Result acquireResult = nri::Result::FAILURE;
		{
			ScopedNriTiming acquireTiming(NriPTAcquireSwap, mLastFrameBoundaryStats.acquireMs);
			acquireResult = mSwapChainInterface.AcquireNextTexture(*mSwapChain, *mSwapChainImages[mAcquireSemaphoreIndex].acquireSemaphore, mCurrentSwapChainImage);
		}
		mLastFrameBoundaryStats.acquireResult = acquireResult;
		mLastFrameBoundaryStats.swapChainImageIndex = mCurrentSwapChainImage;
		if (acquireResult == nri::Result::SUCCESS)
		{
			NoteSwapChainAcquire(mCurrentSwapChainImage);
		}
		if (acquireResult == nri::Result::OUT_OF_DATE)
		{
			if (!EnsureSwapChainSize())
			{
				return;
			}

			{
				ScopedNriTiming reacquireTiming(NriPTAcquireSwap, mLastFrameBoundaryStats.acquireMs);
				acquireResult = mSwapChainInterface.AcquireNextTexture(*mSwapChain, *mSwapChainImages[mAcquireSemaphoreIndex].acquireSemaphore, mCurrentSwapChainImage);
			}
			mLastFrameBoundaryStats.acquireResult = acquireResult;
			mLastFrameBoundaryStats.swapChainImageIndex = mCurrentSwapChainImage;
			if (acquireResult == nri::Result::SUCCESS)
			{
				NoteSwapChainAcquire(mCurrentSwapChainImage);
			}
		}

		if (acquireResult != nri::Result::SUCCESS)
		{
			if (acquireResult == nri::Result::DEVICE_LOST)
			{
				mFrameGeneration.NoteReset("device-lost");
				StartupRecovery_MarkNriDeviceLost("AcquireNextTexture");
			}
			Printf(TEXTCOLOR_RED "NRI failed to acquire swapchain image.\n");
			LogD3D12FailureDiagnostics("AcquireNextTexture");
			if (acquireResult == nri::Result::DEVICE_LOST)
			{
				FatalTerminalDeviceLoss("AcquireNextTexture");
			}
			return;
		}

		mHasAcquiredSwapChainImage = true;
		mCurrentPresentTarget = &mSwapChainImages[mCurrentSwapChainImage].target;
	}

	// Match NRD-Sample's swapchain handling: each acquired image re-enters command recording
	// with unknown local state and must be explicitly transitioned before first use.
	mCurrentPresentTarget->state = {};
	mActiveTarget = mUsingSaveTarget ? &mSaveTarget : mCurrentPresentTarget;
	if (!BeginCommandList("BeginFrame", false))
	{
		ResetFrameTracking(false);
		return;
	}
	mRenderState->BeginFrame();

	if (mViewpoints != nullptr)
	{
		mViewpoints->Clear();
	}

	mFrameBegun = true;
}

FRenderState* NRIRenderDevice::RenderState()
{
	return mRenderState.get();
}

void NRIRenderDevice::Draw2D()
{
	if (!mInitialized)
	{
		return;
	}

	if (twod == nullptr)
	{
		ClearActiveTargetIfPending();
		return;
	}

	const bool restorePendingClearAfterOffscreenFlush = mRenderState->NeedsClear();
	FlushQueued2DTextureRenders();
	if (restorePendingClearAfterOffscreenFlush)
	{
		mRenderState->Clear(CT_Color);
	}

	struct Draw2DTraceStats
	{
		uint32_t commands = 0;
		uint32_t specialCommands = 0;
		uint32_t texturedCommands = 0;
		uint32_t canvasTextureCommands = 0;
		uint32_t scissorCommands = 0;
		uint32_t transformedCommands = 0;
		uint32_t shapeCommands = 0;
		uint32_t triangleCommands = 0;
		uint32_t lineCommands = 0;
		uint32_t pointCommands = 0;
		int32_t vertices = 0;
		int32_t indices = 0;
	};

	auto collectTraceStats = [](F2DDrawer* drawer)
	{
		Draw2DTraceStats stats;
		if (drawer == nullptr)
		{
			return stats;
		}

		stats.vertices = drawer->mVertices.Size();
		stats.indices = drawer->mIndices.Size();
		stats.commands = (uint32_t)drawer->mData.Size();
		for (const auto& cmd : drawer->mData)
		{
			if (cmd.isSpecial != SpecialDrawCommand::NotSpecial)
			{
				stats.specialCommands++;
				continue;
			}

			if (cmd.mTexture != nullptr && cmd.mTexture->isValid())
			{
				stats.texturedCommands++;
				if (cmd.mTexture->isHardwareCanvas())
				{
					stats.canvasTextureCommands++;
				}
			}
			if ((cmd.mFlags & F2DDrawer::DTF_Scissor) != 0)
			{
				stats.scissorCommands++;
			}
			if (cmd.useTransform)
			{
				stats.transformedCommands++;
			}
			if (cmd.shape2DBufInfo != nullptr)
			{
				stats.shapeCommands++;
			}

			switch (cmd.mType)
			{
			case F2DDrawer::DrawTypeLines:
				stats.lineCommands++;
				break;
			case F2DDrawer::DrawTypePoints:
				stats.pointCommands++;
				break;
			case F2DDrawer::DrawTypeTriangles:
			default:
				stats.triangleCommands++;
				break;
			}
		}
		return stats;
	};

	const char* drawMode = "full";
	F2DDrawer* activeDrawer = twod;
	F2DDrawer uiDrawer;
	const double drawStartMs = I_msTimeF();

	if (mFrameGenerationUiTargetActive)
	{
		const uint32_t sceneBlendPrefixCount = GetFrameGenerationSceneBlendPrefixCount();
		if (sceneBlendPrefixCount >= twod->mData.Size())
		{
			ClearActiveTargetIfPending();
			return;
		}

		if (sceneBlendPrefixCount > 0)
		{
			uiDrawer = *twod;
			uiDrawer.mData.Delete(0, (int)sceneBlendPrefixCount);
			activeDrawer = &uiDrawer;
			drawMode = "ui-suffix";
		}
	}

	const auto traceStats = collectTraceStats(activeDrawer);
	::Draw2D(activeDrawer, *mRenderState);
	ClearActiveTargetIfPending();

	if (PerfLoopTraceActive())
	{
		const auto rsTrace = mRenderState->GetPerfTraceStats();
		const auto& texStats = mTexture2DDebugStats;
		Printf(
			"PERF draw2d trace NRI: frame=%llu mode=%s draw_ms=%.3f cmds=%u specials=%u textured=%u canvas=%u scissor=%u xform=%u shapes=%u tris=%u lines=%u points=%u verts=%d indices=%d ensure=%u hits=%u misses=%u uploads=%u recreate=%u bytes=%llu apply_calls=%u indexed=%u pipe_create=%u apply_ms=%.3f pipe_ms=%.3f vstream_ms=%.3f istream_ms=%.3f ensure_ms=%.3f begin_ms=%.3f bind_ms=%.3f drawcall_ms=%.3f\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			drawMode,
			I_msTimeF() - drawStartMs,
			traceStats.commands,
			traceStats.specialCommands,
			traceStats.texturedCommands,
			traceStats.canvasTextureCommands,
			traceStats.scissorCommands,
			traceStats.transformedCommands,
			traceStats.shapeCommands,
			traceStats.triangleCommands,
			traceStats.lineCommands,
			traceStats.pointCommands,
			traceStats.vertices,
			traceStats.indices,
			texStats.ensureCalls,
			texStats.cacheHits,
			texStats.cacheMisses,
			texStats.uploadAttempts,
			texStats.resourceRecreates,
			(unsigned long long)texStats.uploadedBytes,
			rsTrace.applyCalls,
			rsTrace.indexedCalls,
			rsTrace.pipelineCreates,
			rsTrace.applyMs,
			rsTrace.pipelineMs,
			rsTrace.vertexStreamMs,
			rsTrace.indexStreamMs,
			rsTrace.textureEnsureMs,
			rsTrace.beginRenderingMs,
			rsTrace.bindStateMs,
			rsTrace.drawCallMs);
		Printf(
			"PERF twod bind detail NRI: frame=%llu viewport_change=%u viewport_noop=%u scissor_change=%u scissor_noop=%u root_change=%u root_noop=%u sampler_change=%u sampler_noop=%u texture_change=%u texture_noop=%u pipeline_change=%u pipeline_noop=%u vbuf_change=%u vbuf_noop=%u ibuf_change=%u ibuf_noop=%u\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			rsTrace.viewportChanges,
			rsTrace.viewportNoops,
			rsTrace.scissorChanges,
			rsTrace.scissorNoops,
			rsTrace.rootConstantChanges,
			rsTrace.rootConstantNoops,
			rsTrace.samplerSetChanges,
			rsTrace.samplerSetNoops,
			rsTrace.textureSetChanges,
			rsTrace.textureSetNoops,
			rsTrace.pipelineChanges,
			rsTrace.pipelineNoops,
			rsTrace.vertexBufferChanges,
			rsTrace.vertexBufferNoops,
			rsTrace.indexBufferChanges,
			rsTrace.indexBufferNoops);
		mRenderState->ResetPerfTraceStats();
	}
}

void NRIRenderDevice::RequestSwapChainRefresh(const char* reason, bool forceRecreate)
{
	if (!mInitialized || mShuttingDown || mDevice == nullptr || mGraphicsQueue == nullptr)
	{
		return;
	}

	if (!mSwapChainRefreshPending || (forceRecreate && !mSwapChainRefreshForceRecreate))
	{
		mSwapChainRefreshReason = reason != nullptr ? reason : "unspecified";
	}
	mSwapChainRefreshPending = true;
	mSwapChainRefreshForceRecreate = mSwapChainRefreshForceRecreate || forceRecreate;
	mSwapChainRefreshRequestCount++;
}

bool NRIRenderDevice::ApplyPendingSwapChainRefresh()
{
	if (!mSwapChainRefreshPending)
	{
		return true;
	}

	const bool forceRecreate = mSwapChainRefreshForceRecreate;
	const uint32_t requestCount = mSwapChainRefreshRequestCount;
	const FString reason = mSwapChainRefreshReason;
	mSwapChainRefreshPending = false;
	mSwapChainRefreshForceRecreate = false;
	mSwapChainRefreshRequestCount = 0;
	mSwapChainRefreshReason = "none";

	Printf("NRI swapchain refresh: action=%s reason=%s coalesced=%u\n",
		forceRecreate ? "recreate" : "reconcile",
		reason.GetChars(),
		requestCount);
	if (!forceRecreate)
	{
		return true;
	}

	mFrameGeneration.NoteReset(reason.GetChars());
	WaitForCommands(true);
	if (!CreateSwapChain())
	{
		Printf(TEXTCOLOR_RED "NRI failed to apply deferred swapchain refresh '%s'.\n", reason.GetChars());
		return false;
	}
	return true;
}

void NRIRenderDevice::SetVSync(bool vsync)
{
	Super::SetVSync(vsync);
	RequestSwapChainRefresh("vsync-change", false);
}

bool NRIRenderDevice::ShouldRequestFrameGenerationLowLatencySwapChain() const
{
	if (!nri_framegen || !nri_framegenlatency || !HasRequestedFrameGenerationProvider())
	{
		return false;
	}

	if (mDevice == nullptr || mSwapChainInterface.CreateSwapChain == nullptr)
	{
		return false;
	}

	if (GetLiveAPI() != nri::GraphicsAPI::D3D12 || IsFullscreenModeActive())
	{
		return false;
	}

	const nri::DeviceDesc& deviceDesc = mCore.GetDeviceDesc(*mDevice);
	if (!deviceDesc.features.lowLatency)
	{
		return false;
	}

	return
		mLowLatency.SetLatencySleepMode != nullptr &&
		mLowLatency.SetLatencyMarker != nullptr &&
		mLowLatency.LatencySleep != nullptr &&
		mLowLatency.GetLatencyReport != nullptr;
}

nri::SwapChainBits NRIRenderDevice::GetEffectiveRequestedSwapChainFlags() const
{
	nri::SwapChainBits flags = GetRequestedSwapChainFlags();
	if (ShouldRequestFrameGenerationLowLatencySwapChain())
	{
		flags = NRIFlags(flags, nri::SwapChainBits::ALLOW_LOW_LATENCY);
	}
	return flags;
}

bool NRIRenderDevice::ShouldUseFrameGenerationUiTarget() const
{
	if (!mInitialized || mFrameBegun == false || mUsingSaveTarget || mCurrentPresentTarget == nullptr)
	{
		return false;
	}

	const auto& policy = mFrameGeneration.GetPolicy();
	return
		policy.requestedEnabled &&
		policy.requestedProvider != NRIFrameGenerationProvider::Off &&
		policy.resolvedUiMode == NRIFrameGenerationUiMode::UiTexture;
}

bool NRIRenderDevice::ShouldHandoffFrameGenerationUiTexture() const
{
#ifdef _WIN32
	return ShouldUseFrameGenerationUiTarget() && IsFrameGenerationPresentPathActive();
#else
	return false;
#endif
}

bool NRIRenderDevice::ShouldUseFrameGenerationUiLocalCompositeFallback() const
{
	return ShouldUseFrameGenerationUiTarget() && !ShouldHandoffFrameGenerationUiTexture();
}

const char* NRIRenderDevice::GetFrameGenerationUiRouteName() const
{
	const auto& policy = mFrameGeneration.GetPolicy();
	const bool uiTextureRouteRequested =
		mInitialized &&
		!mUsingSaveTarget &&
		policy.requestedEnabled &&
		policy.requestedProvider != NRIFrameGenerationProvider::Off &&
		policy.resolvedUiMode == NRIFrameGenerationUiMode::UiTexture;
	if (!uiTextureRouteRequested)
	{
		return "off";
	}

#ifdef _WIN32
	return IsFrameGenerationPresentPathActive() ? "provider" : "local-composite";
#else
	return "local-composite";
#endif
}

uint32_t NRIRenderDevice::GetFrameGenerationSceneBlendPrefixCount() const
{
	if (twod == nullptr)
	{
		return 0u;
	}

	uint32_t count = 0u;
	for (const auto& cmd : twod->mData)
	{
		if (!IsFullscreenPaletteBlendCommand(*twod, cmd))
		{
			break;
		}

		++count;
	}

	return count;
}

bool NRIRenderDevice::EnsureFrameGenerationUiTexture(uint32_t width, uint32_t height)
{
	if (width == 0 || height == 0)
	{
		return false;
	}

	if (mFrameGenerationUiTexture == nullptr)
	{
		mFrameGenerationUiTexture = MakeGameTexture(new FWrapperTexture((int)width, (int)height, 1), nullptr, ETextureType::SWCanvas);
	}

	auto* wrapper = mFrameGenerationUiTexture != nullptr ? static_cast<FWrapperTexture*>(mFrameGenerationUiTexture->GetTexture()) : nullptr;
	if (wrapper == nullptr)
	{
		return false;
	}

	if (wrapper->GetWidth() != (int)width || wrapper->GetHeight() != (int)height)
	{
		delete mFrameGenerationUiTexture;
		mFrameGenerationUiTexture = MakeGameTexture(new FWrapperTexture((int)width, (int)height, 1), nullptr, ETextureType::SWCanvas);
		wrapper = mFrameGenerationUiTexture != nullptr ? static_cast<FWrapperTexture*>(mFrameGenerationUiTexture->GetTexture()) : nullptr;
		if (wrapper == nullptr)
		{
			return false;
		}
	}

	auto* hwTex = static_cast<NRIHardwareTexture*>(wrapper->GetSystemTexture());
	if (hwTex == nullptr)
	{
		return false;
	}

	hwTex->EnsureCanvas(wrapper);
	return hwTex->GetResource().texture != nullptr && hwTex->GetResource().colorAttachmentView != nullptr;
}

NRITextureResource* NRIRenderDevice::GetFrameGenerationUiTargetResource() const
{
	auto* wrapper = mFrameGenerationUiTexture != nullptr ? static_cast<FWrapperTexture*>(mFrameGenerationUiTexture->GetTexture()) : nullptr;
	if (wrapper == nullptr)
	{
		return nullptr;
	}

	auto* hwTex = static_cast<NRIHardwareTexture*>(wrapper->GetSystemTexture());
	if (hwTex == nullptr)
	{
		return nullptr;
	}

	return &hwTex->GetResource();
}

bool NRIRenderDevice::EnsureViewSnapshotTexture(uint32_t width, uint32_t height, nri::Format format)
{
	if (width == 0 || height == 0)
	{
		return false;
	}

	if (mViewSnapshotTexture == nullptr)
	{
		mViewSnapshotTexture = MakeGameTexture(new FWrapperTexture((int)width, (int)height, 1), nullptr, ETextureType::SWCanvas);
	}

	auto* wrapper = mViewSnapshotTexture != nullptr ? static_cast<FWrapperTexture*>(mViewSnapshotTexture->GetTexture()) : nullptr;
	if (wrapper == nullptr)
	{
		return false;
	}

	auto* hwTex = static_cast<NRIHardwareTexture*>(wrapper->GetSystemTexture());
	if (hwTex == nullptr)
	{
		return false;
	}

	if (wrapper->GetWidth() != (int)width ||
		wrapper->GetHeight() != (int)height ||
		(hwTex->GetResource().texture != nullptr && hwTex->GetResource().format != format))
	{
		delete mViewSnapshotTexture;
		mViewSnapshotTexture = MakeGameTexture(new FWrapperTexture((int)width, (int)height, 1), nullptr, ETextureType::SWCanvas);
		wrapper = mViewSnapshotTexture != nullptr ? static_cast<FWrapperTexture*>(mViewSnapshotTexture->GetTexture()) : nullptr;
		if (wrapper == nullptr)
		{
			return false;
		}
		hwTex = static_cast<NRIHardwareTexture*>(wrapper->GetSystemTexture());
		if (hwTex == nullptr)
		{
			return false;
		}
	}

	hwTex->EnsureCanvas(wrapper, format);
	return hwTex->GetResource().texture != nullptr && hwTex->GetResource().colorAttachmentView != nullptr;
}

NRITextureResource* NRIRenderDevice::GetViewSnapshotTargetResource() const
{
	auto* wrapper = mViewSnapshotTexture != nullptr ? static_cast<FWrapperTexture*>(mViewSnapshotTexture->GetTexture()) : nullptr;
	if (wrapper == nullptr)
	{
		return nullptr;
	}

	auto* hwTex = static_cast<NRIHardwareTexture*>(wrapper->GetSystemTexture());
	if (hwTex == nullptr)
	{
		return nullptr;
	}

	return &hwTex->GetResource();
}

void NRIRenderDevice::ClearTargetColor(NRITextureResource& target, float red, float green, float blue, float alpha)
{
	if (mCommandBuffer == nullptr || target.colorAttachmentView == nullptr)
	{
		return;
	}

	mRenderState->EndFrame();
	PrepareTargetForRendering(target, true);

	nri::AttachmentDesc colorAttachment = {};
	colorAttachment.descriptor = target.colorAttachmentView;
	colorAttachment.loadOp = nri::LoadOp::CLEAR;
	colorAttachment.storeOp = nri::StoreOp::STORE;
	colorAttachment.clearValue.color.f.x = red;
	colorAttachment.clearValue.color.f.y = green;
	colorAttachment.clearValue.color.f.z = blue;
	colorAttachment.clearValue.color.f.w = alpha;

	nri::RenderingDesc renderingDesc = {};
	renderingDesc.colors = &colorAttachment;
	renderingDesc.colorNum = 1;
	mCore.CmdBeginRendering(*mCommandBuffer, renderingDesc);
	mCore.CmdEndRendering(*mCommandBuffer);
	mActiveTarget = &target;
	mRenderState->NotifyExternalTargetWrite();
}

void NRIRenderDevice::ClearActiveTargetIfPending()
{
	if (mActiveTarget == nullptr || mRenderState == nullptr || !mRenderState->NeedsClear())
	{
		return;
	}

	ClearTargetColor(*mActiveTarget, mSceneClearColor[0], mSceneClearColor[1], mSceneClearColor[2], mSceneClearColor[3]);
}

void NRIRenderDevice::BeginFrameGenerationUiTarget()
{
	if (mCurrentPresentTarget == nullptr)
	{
		return;
	}

	if (!EnsureFrameGenerationUiTexture(mCurrentPresentTarget->width, mCurrentPresentTarget->height))
	{
		return;
	}

	NRITextureResource* uiTarget = GetFrameGenerationUiTargetResource();
	if (uiTarget == nullptr)
	{
		return;
	}

	ClearTargetColor(*uiTarget, 0.0f, 0.0f, 0.0f, 0.0f);
	mActiveTarget = uiTarget;
	mFrameGenerationUiTargetActive = true;
}

void NRIRenderDevice::DrawFrameGenerationSceneBlendPrefix()
{
	if (twod == nullptr)
	{
		return;
	}

	const uint32_t sceneBlendPrefixCount = GetFrameGenerationSceneBlendPrefixCount();
	if (sceneBlendPrefixCount == 0u)
	{
		return;
	}

	F2DDrawer sceneBlendDrawer = *twod;
	sceneBlendDrawer.mData.Clamp(sceneBlendPrefixCount);
	::Draw2D(&sceneBlendDrawer, *mRenderState);
}

void NRIRenderDevice::FinalizeFrameGenerationUiTarget()
{
	if (!mFrameGenerationUiTargetActive)
	{
		return;
	}

	NRITextureResource* uiTarget = GetFrameGenerationUiTargetResource();
	if (uiTarget != nullptr)
	{
		TransitionTexture(*uiTarget, NRIShaderResourceState());
		if (ShouldHandoffFrameGenerationUiTexture() || ShouldUseFrameGenerationUiLocalCompositeFallback())
		{
			mFrameGeneration.SetUiTexture(uiTarget);
		}
	}

	mRenderState->EndFrame();
	mActiveTarget = mCurrentPresentTarget;
	mFrameGenerationUiTargetActive = false;
}

void NRIRenderDevice::CompositeFrameGenerationUiTexture()
{
	if (mFrameGenerationUiTexture == nullptr || mCurrentPresentTarget == nullptr || twod == nullptr)
	{
		return;
	}

	SetActiveRenderTarget();
	DrawTexture(twod, mFrameGenerationUiTexture, 0, 0, DTA_Masked, false, TAG_DONE);
}

void NRIRenderDevice::DestroyFrameGenerationUiTexture()
{
	delete mFrameGenerationUiTexture;
	mFrameGenerationUiTexture = nullptr;
	mFrameGenerationUiTargetActive = false;
}

void NRIRenderDevice::DestroyViewSnapshotTexture()
{
	delete mViewSnapshotTexture;
	mViewSnapshotTexture = nullptr;
}

bool NRIRenderDevice::EnsureSaveTarget(uint32_t width, uint32_t height)
{
	if (width == 0 || height == 0)
	{
		return false;
	}

	if (mSaveTarget.texture != nullptr && mSaveTarget.width == width && mSaveTarget.height == height)
	{
		return true;
	}

	DestroyTextureResource(mSaveTarget);
	return CreateOwnedTexture(mSaveTarget, width, height, nri::Format::BGRA8_UNORM, NRIFlags(nri::TextureUsageBits::SHADER_RESOURCE, nri::TextureUsageBits::COLOR_ATTACHMENT));
}

void NRIRenderDevice::WaitForCommands(bool finish)
{
	if (mDevice == nullptr)
	{
		return;
	}

	if (finish)
	{
		if (mTerminalDeviceLoss)
		{
			if (!mShuttingDown)
			{
				FatalTerminalDeviceLoss("DeviceWaitIdle");
			}
			return;
		}
		mCore.DeviceWaitIdle(mDevice);
		if (mCreatedDeviceApi == nri::GraphicsAPI::D3D12 &&
			mNativeD3D12Device != nullptr &&
			mNativeD3D12Device->GetDeviceRemovedReason() != S_OK)
		{
			MarkTerminalDeviceLoss("DeviceWaitIdle");
			LogD3D12FailureDiagnostics("DeviceWaitIdle");
			if (!mShuttingDown)
			{
				FatalTerminalDeviceLoss("DeviceWaitIdle");
			}
		}
		return;
	}

	if (mFrameFence == nullptr || mQueuedFrames.empty())
	{
		return;
	}

	if (mFrameIndex < mQueuedFrames.size())
	{
		return;
	}

	const uint64_t recycleFenceValue = 1 + mFrameIndex - mQueuedFrames.size();
	if (recycleFenceValue != 0)
	{
		WaitForFenceValue(*mFrameFence, recycleFenceValue, "Wait(FrameFenceRecycle)");
	}
}

bool NRIRenderDevice::TryGetFenceValue(nri::Fence& fence, const char* context, uint64_t& outCompletedFenceValue)
{
	outCompletedFenceValue = 0;
	if (mTerminalDeviceLoss)
	{
		return false;
	}

	outCompletedFenceValue = mCore.GetFenceValue(fence);
	if (outCompletedFenceValue != UINT64_MAX)
	{
		return true;
	}

	MarkTerminalDeviceLoss(context);
	LogD3D12FailureDiagnostics(context);
	return false;
}

bool NRIRenderDevice::WaitForFenceValue(nri::Fence& fence, uint64_t fenceValue, const char* context)
{
	if (fenceValue == 0)
	{
		return true;
	}
	if (mTerminalDeviceLoss)
	{
		return false;
	}

	mCore.Wait(fence, fenceValue);

	uint64_t completedFenceValue = 0;
	if (!TryGetFenceValue(fence, context, completedFenceValue))
	{
		return false;
	}
	if (completedFenceValue >= fenceValue)
	{
		return true;
	}

	Printf(TEXTCOLOR_RED "NRI fence wait returned before completion: context=%s requested=%llu completed=%llu.\n",
		context != nullptr ? context : "unknown",
		(unsigned long long)fenceValue,
		(unsigned long long)completedFenceValue);
	MarkTerminalDeviceLoss(context);
	LogD3D12FailureDiagnostics(context);
	return false;
}

bool NRIRenderDevice::IsCommandFenceValueComplete(uint64_t fenceValue)
{
	if (fenceValue == 0)
	{
		return true;
	}
	if (IsCommandFenceValueAbandoned(fenceValue))
	{
		return false;
	}
	if (mCommandCompletionFence == nullptr)
	{
		return false;
	}

	uint64_t completedFenceValue = 0;
	if (!TryGetFenceValue(*mCommandCompletionFence, "GetFenceValue(CommandCompletionFence)", completedFenceValue))
	{
		return false;
	}
	return completedFenceValue >= fenceValue;
}

bool NRIRenderDevice::IsFrameFenceValueComplete(uint64_t fenceValue)
{
	if (fenceValue == 0)
	{
		return true;
	}
	if (mFrameFence == nullptr)
	{
		return false;
	}

	uint64_t completedFenceValue = 0;
	if (!TryGetFenceValue(*mFrameFence, "GetFenceValue(FrameFence)", completedFenceValue))
	{
		return false;
	}
	return completedFenceValue >= fenceValue;
}

bool NRIRenderDevice::IsCommandFenceValueAbandoned(uint64_t fenceValue) const
{
	return fenceValue != 0 && mAbandonedCommandFenceValues.find(fenceValue) != mAbandonedCommandFenceValues.end();
}

void NRIRenderDevice::AbandonRecordingCommandFenceValue()
{
	if (mRecordingCommandFenceValue == 0)
	{
		return;
	}
	mAbandonedCommandFenceValues.insert(mRecordingCommandFenceValue);
	mRecordingCommandFenceValue = 0;
}

bool NRIRenderDevice::SubmitAndWaitCurrentCommandBuffer()
{
	mLastSubmitAndWaitResult = nri::Result::SUCCESS;
	if (mCommandBuffer == nullptr || !mCommandBufferOpen)
	{
		return true;
	}

	if (mGpuTiming != nullptr) mGpuTiming->FinalizeSegment(mCore, *mCommandBuffer);
	mCore.EndCommandBuffer(*mCommandBuffer);
	mCommandBufferOpen = false;

	const nri::CommandBuffer* commandBuffers[] = { mCommandBuffer };
	nri::Fence* submitFence = nullptr;
	const nri::Result fenceResult = mCore.CreateFence(*mDevice, 0, submitFence);
	if (fenceResult != nri::Result::SUCCESS)
	{
		if (mGpuTiming != nullptr) mGpuTiming->AbandonSlot(mCurrentQueuedFrameIndex);
		AbandonRecordingCommandFenceValue();
		mLastSubmitAndWaitResult = fenceResult;
		if (mCreatedDeviceApi == nri::GraphicsAPI::D3D12 &&
			mNativeD3D12Device != nullptr &&
			mNativeD3D12Device->GetDeviceRemovedReason() != S_OK)
		{
			mLastSubmitAndWaitResult = nri::Result::DEVICE_LOST;
		}
		return false;
	}

	nri::FenceSubmitDesc signalFences[2] = {};
	signalFences[0].fence = submitFence;
	signalFences[0].value = 1;
	uint32_t signalFenceCount = 1;
	if (mCommandCompletionFence != nullptr && mRecordingCommandFenceValue != 0)
	{
		signalFences[signalFenceCount++] = { mCommandCompletionFence, mRecordingCommandFenceValue, nri::StageBits::NONE };
	}

	nri::QueueSubmitDesc submitDesc = {};
	submitDesc.commandBuffers = commandBuffers;
	submitDesc.commandBufferNum = 1;
	submitDesc.signalFences = signalFences;
	submitDesc.signalFenceNum = signalFenceCount;
	const nri::Result submitResult = mCore.QueueSubmit(*mGraphicsQueue, submitDesc);
	if (submitResult == nri::Result::SUCCESS)
	{
		mRecordingCommandFenceValue = 0;
	}
	else
	{
		if (mGpuTiming != nullptr) mGpuTiming->AbandonSlot(mCurrentQueuedFrameIndex);
		AbandonRecordingCommandFenceValue();
	}
	mLastSubmitAndWaitResult = submitResult;
	bool waitSucceeded = submitResult == nri::Result::SUCCESS;
	if (submitResult == nri::Result::SUCCESS)
	{
		waitSucceeded = WaitForFenceValue(*submitFence, signalFences[0].value, "Wait(SubmitFence)");
		if (!waitSucceeded)
		{
			mLastSubmitAndWaitResult = nri::Result::DEVICE_LOST;
		}
		else if (mGpuTiming != nullptr)
		{
			mGpuTiming->RetireSlot(mCore, mCurrentQueuedFrameIndex);
		}
	}
	if (!waitSucceeded && mGpuTiming != nullptr) mGpuTiming->AbandonSlot(mCurrentQueuedFrameIndex);

	mCore.DestroyFence(submitFence);
	return submitResult == nri::Result::SUCCESS && waitSucceeded;
}

bool NRIRenderDevice::IsPreloadSubmitBudgetHit() const
{
	return mPreloadSubmitBudgetHit;
}

uint32_t NRIRenderDevice::GetPreloadSubmitCountThisTick() const
{
	return mPreloadSubmitsThisTick;
}

uint32_t NRIRenderDevice::GetPreloadSubmitLimitThisTick() const
{
	return mPreloadMaxSubmitsThisTick;
}

bool NRIRenderDevice::HasTerminalDeviceLoss() const
{
	return mTerminalDeviceLoss;
}

void NRIRenderDevice::MarkTerminalDeviceLoss(const char* context)
{
	if (mTerminalDeviceLoss)
	{
		return;
	}

	mTerminalDeviceLoss = true;
	if (!mLoggedTerminalDeviceLoss)
	{
		mLoggedTerminalDeviceLoss = true;
		Printf(TEXTCOLOR_RED "NRI terminal device loss: context=%s preload_pending=%u preload_context=%u command_open=%u submits=%u limit=%u.\n",
			context != nullptr ? context : "unknown",
			mPathTracingLevelPreloadPending ? 1u : 0u,
			mPreloadCommandContextActive ? 1u : 0u,
			mCommandBufferOpen ? 1u : 0u,
			mPreloadSubmitsThisTick,
			mPreloadMaxSubmitsThisTick);
	}
	mPathTracingLevelPreloadPending = false;
	StartupRecovery_MarkNriDeviceLost(context != nullptr ? context : "NRI");
}

[[noreturn]] void NRIRenderDevice::FatalTerminalDeviceLoss(const char* context)
{
	const char* failureContext = context != nullptr ? context : "NRI";
	MarkTerminalDeviceLoss(failureContext);

	FString reason = "device_lost";
	if (mCreatedDeviceApi == nri::GraphicsAPI::D3D12 && mNativeD3D12Device != nullptr)
	{
		const HRESULT hr = mNativeD3D12Device->GetDeviceRemovedReason();
		reason = FStringf("%s (0x%08X)", GetDxgiErrorName(hr), (unsigned)hr);
	}

	I_FatalError("NRI renderer device lost during %s: %s.\n"
		"The renderer cannot recover this graphics device during the current run. "
		"Startup recovery has recorded the device-loss state and will apply Safe Mode on the next launch.",
		failureContext,
		reason.GetChars());
}

bool NRIRenderDevice::SubmitWaitAndRestartCommandList(const char* reason)
{
	if (mTerminalDeviceLoss)
	{
		return false;
	}
	if (mPreloadCommandContextActive &&
		mPreloadMaxSubmitsThisTick != 0 &&
		mPreloadSubmitsThisTick >= mPreloadMaxSubmitsThisTick)
	{
		mPreloadSubmitBudgetHit = true;
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=preload-submit result=wait reason=submit-budget-hit submits=%u limit=%u last_reason=%s requested_reason=%s\n",
				mPreloadSubmitsThisTick,
				mPreloadMaxSubmitsThisTick,
				mLastPreloadSubmitReason.GetChars(),
				reason != nullptr ? reason : "unknown");
		}
		return false;
	}

	const bool wasOpen = mCommandBufferOpen;
	if (!SubmitAndWaitCurrentCommandBuffer())
	{
		Printf(TEXTCOLOR_RED "NRI SubmitWaitAndRestartCommandList failed (reason=%s).\n",
			reason != nullptr ? reason : "unknown");
		if (mLastSubmitAndWaitResult == nri::Result::DEVICE_LOST)
		{
			mFrameGeneration.NoteReset("device-lost");
			MarkTerminalDeviceLoss(reason != nullptr ? reason : "SubmitWaitAndRestartCommandList");
		}
		LogD3D12FailureDiagnostics(reason != nullptr ? reason : "SubmitWaitAndRestartCommandList");
		if (mLastSubmitAndWaitResult == nri::Result::DEVICE_LOST)
		{
			FatalTerminalDeviceLoss(reason != nullptr ? reason : "SubmitWaitAndRestartCommandList");
		}
		return false;
	}
	if (mPreloadCommandContextActive && wasOpen)
	{
		mPreloadSubmitsThisTick++;
		mLastPreloadSubmitReason = reason != nullptr ? reason : "unknown";
		if (mPreloadMaxSubmitsThisTick != 0 && mPreloadSubmitsThisTick >= mPreloadMaxSubmitsThisTick)
		{
			mPreloadSubmitBudgetHit = true;
		}
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=preload-submit result=submitted reason=%s submits=%u limit=%u budget_hit=%u\n",
				mLastPreloadSubmitReason.GetChars(),
				mPreloadSubmitsThisTick,
				mPreloadMaxSubmitsThisTick,
				mPreloadSubmitBudgetHit ? 1u : 0u);
		}
	}
	if (!wasOpen)
	{
		return true;
	}
	return BeginCommandList(reason != nullptr ? reason : "SubmitWaitAndRestartCommandList", false);
}

bool NRIRenderDevice::BeginPreloadCommandContext(const char* reason)
{
	if (mTerminalDeviceLoss)
	{
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=preload-command result=failed reason=terminal-device-loss\n");
		}
		return false;
	}
	if (!mInitialized || mFrameBegun || mCommandBufferOpen || mPreloadCommandContextActive)
	{
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=preload-command result=wait reason=busy initialized=%u frame_begun=%u command_open=%u active=%u\n",
				mInitialized ? 1u : 0u,
				mFrameBegun ? 1u : 0u,
				mCommandBufferOpen ? 1u : 0u,
				mPreloadCommandContextActive ? 1u : 0u);
		}
		return false;
	}

	mCurrentQueuedFrameIndex = GetQueuedFrameIndex(mFrameIndex);
	WaitForCommands(false);
	if (mTerminalDeviceLoss)
	{
		FatalTerminalDeviceLoss("Wait(FrameFenceRecycle)");
	}
	ReleaseRetiredTextureResources(false);
	mCurrentPresentTarget = nullptr;
	mActiveTarget = nullptr;
	mHasAcquiredSwapChainImage = false;
	mHasPresentedSwapChainFrame = false;
	mPreloadCommandContextActive = true;
	mPreloadSubmitsThisTick = 0;
	mPreloadMaxSubmitsThisTick = std::max<int>(0, (int)nri_ptpreloadmaxsubmitspertick);
	mPreloadSubmitBudgetHit = false;
	mLastPreloadSubmitReason = "none";
	if (!BeginCommandList(reason != nullptr ? reason : "preload", true))
	{
		mPreloadCommandContextActive = false;
		return false;
	}
	return true;
}

bool NRIRenderDevice::EndPreloadCommandContext(const char* reason)
{
	if (!mPreloadCommandContextActive)
	{
		return true;
	}

	const bool success = SubmitAndWaitCurrentCommandBuffer();
	if (success && !mPreloadSubmitBudgetHit)
	{
		mPreloadSubmitsThisTick++;
		mLastPreloadSubmitReason = reason != nullptr ? reason : "unknown";
	}
	mPreloadCommandContextActive = false;
	mActiveTarget = nullptr;
	mCurrentPresentTarget = nullptr;
	if (!success)
	{
		Printf(TEXTCOLOR_RED "NRI preload command context submit failed (reason=%s).\n",
			reason != nullptr ? reason : "unknown");
		if (mLastSubmitAndWaitResult == nri::Result::DEVICE_LOST)
		{
			mFrameGeneration.NoteReset("device-lost");
			MarkTerminalDeviceLoss(reason != nullptr ? reason : "EndPreloadCommandContext");
		}
		LogD3D12FailureDiagnostics(reason != nullptr ? reason : "EndPreloadCommandContext");
		if (mLastSubmitAndWaitResult == nri::Result::DEVICE_LOST)
		{
			FatalTerminalDeviceLoss(reason != nullptr ? reason : "EndPreloadCommandContext");
		}
	}
	else if ((int)nri_ptloadingtrace >= 1)
	{
		Printf("NRI PT loading gate: event=preload-submit result=submitted reason=%s submits=%u limit=%u budget_hit=%u final=1\n",
			reason != nullptr ? reason : mLastPreloadSubmitReason.GetChars(),
			mPreloadSubmitsThisTick,
			mPreloadMaxSubmitsThisTick,
			mPreloadSubmitBudgetHit ? 1u : 0u);
	}
	return success;
}

void NRIRenderDevice::SetSaveBuffers(bool yes)
{
	mUsingSaveTarget = yes;
	if (!mInitialized)
	{
		return;
	}

	const uint32_t saveWidth = mRequestedSaveTargetWidth != 0 ? mRequestedSaveTargetWidth : SAVEPICWIDTH;
	const uint32_t saveHeight = mRequestedSaveTargetHeight != 0 ? mRequestedSaveTargetHeight : SAVEPICHEIGHT;
	if (yes && !EnsureSaveTarget(saveWidth, saveHeight))
	{
		Printf(TEXTCOLOR_RED "NRI failed to create the savepic render target.\n");
		mActiveTarget = mCurrentPresentTarget;
		return;
	}

	mRenderState->EndFrame();
	mActiveTarget = yes ? &mSaveTarget : mCurrentPresentTarget;
	mFrameGenerationUiTargetActive = false;
}

bool NRIRenderDevice::PrepareSavePicScene(int width, int height)
{
	if (!mInitialized || width <= 0 || height <= 0)
	{
		return false;
	}

	mRequestedSaveTargetWidth = (uint32_t)width;
	mRequestedSaveTargetHeight = (uint32_t)height;
	if (!EnsureSaveTarget(mRequestedSaveTargetWidth, mRequestedSaveTargetHeight))
	{
		Printf(TEXTCOLOR_RED "NRI failed to prepare the savepic render target (%dx%d).\n", width, height);
		return false;
	}

	if (mFrameBegun)
	{
		return true;
	}

	mStandaloneSavePicFrame = true;
	SelectQueuedFrame(GetQueuedFrameIndex(mFrameIndex));
	if (!BeginCommandList("PrepareSavePicScene", true))
	{
		mStandaloneSavePicFrame = false;
		return false;
	}

	mRenderState->BeginFrame();
	if (mViewpoints != nullptr)
	{
		mViewpoints->Clear();
	}

	mCurrentPresentTarget = nullptr;
	mActiveTarget = &mSaveTarget;
	mFrameGenerationUiTargetActive = false;
	mFrameBegun = true;
	return true;
}

void NRIRenderDevice::FinishSavePicScene()
{
	if (!mStandaloneSavePicFrame)
	{
		mRequestedSaveTargetWidth = 0;
		mRequestedSaveTargetHeight = 0;
		return;
	}

	mRenderState->EndFrame();
	SubmitAndWaitCurrentCommandBuffer();
	ResetFrameTracking(false);
	mUsingSaveTarget = false;
	mStandaloneSavePicFrame = false;
	mRequestedSaveTargetWidth = 0;
	mRequestedSaveTargetHeight = 0;
}

void NRIRenderDevice::ImageTransitionScene(bool)
{
}

void NRIRenderDevice::SetSceneRenderTarget(bool)
{
	if (!mInitialized)
	{
		return;
	}

	if (mUsingSaveTarget)
	{
		mRenderState->EndFrame();
		mActiveTarget = &mSaveTarget;
		mLastFrameBoundaryStats.sceneTargetSelected = false;
		return;
	}

	if (mCurrentPresentTarget == nullptr)
	{
		return;
	}

	if (mSceneTarget.texture == nullptr ||
		mSceneTarget.width != mCurrentPresentTarget->width ||
		mSceneTarget.height != mCurrentPresentTarget->height ||
		mSceneTarget.format != mCurrentPresentTarget->format)
	{
		DestroyTextureResource(mSceneTarget);
		if (!CreateOwnedTexture(mSceneTarget,
			mCurrentPresentTarget->width,
			mCurrentPresentTarget->height,
			mCurrentPresentTarget->format,
			NRIFlags(nri::TextureUsageBits::SHADER_RESOURCE, nri::TextureUsageBits::COLOR_ATTACHMENT)))
		{
			Printf(TEXTCOLOR_RED "NRI failed to create the scene render target.\n");
			mActiveTarget = mCurrentPresentTarget;
			mLastFrameBoundaryStats.sceneTargetSelected = false;
			return;
		}
	}

	mRenderState->EndFrame();
	mActiveTarget = &mSceneTarget;
	mLastFrameBoundaryStats.sceneTargetSelected = true;
}

void NRIRenderDevice::SetActiveRenderTarget()
{
	mRenderState->EndFrame();
	mActiveTarget = mUsingSaveTarget ? &mSaveTarget : mCurrentPresentTarget;
	mLastFrameBoundaryStats.sceneTargetSelected = (mActiveTarget == &mSceneTarget);
	mFrameGenerationUiTargetActive = false;
}

void NRIRenderDevice::PostProcessScene(bool swscene, int, float, const std::function<void()> &afterBloomDrawEndScene2D)
{
	static bool sLoggedScenePostProcessCopy = false;

	if (!mInitialized)
	{
		if (afterBloomDrawEndScene2D)
		{
			afterBloomDrawEndScene2D();
		}
		return;
	}

	mLastFrameBoundaryStats.postProcessInvoked = true;

	if (!mUsingSaveTarget && !swscene && mCommandBuffer != nullptr &&
		mCurrentPresentTarget != nullptr && mSceneTarget.texture != nullptr && mActiveTarget == &mSceneTarget)
	{
		if (nri_ptdebug > 0 && !sLoggedScenePostProcessCopy)
		{
			Printf("NRI scene postprocess: copying scene target %ux%u to the present target before 2D composition.\n",
				mSceneTarget.width,
				mSceneTarget.height);
			sLoggedScenePostProcessCopy = true;
		}
		mRenderState->EndFrame();
		TransitionTexture(mSceneTarget, NRICopySourceState());
		TransitionTexture(*mCurrentPresentTarget, NRICopyDestinationState());
		mCore.CmdCopyTexture(*mCommandBuffer, *mCurrentPresentTarget->texture, nullptr, *mSceneTarget.texture, nullptr);
		mActiveTarget = mCurrentPresentTarget;
		mRenderState->NotifyExternalTargetWrite();
		mLastFrameBoundaryStats.sceneCopiedToPresent = true;
	}
	else
	{
		SetActiveRenderTarget();
	}

	if (afterBloomDrawEndScene2D)
	{
		afterBloomDrawEndScene2D();
	}

	if (ShouldUseFrameGenerationUiTarget())
	{
		BeginFrameGenerationUiTarget();
	}
}

bool NRIRenderDevice::RenderPathTracedScene(HWDrawInfo& di, int drawmode, bool portal)
{
	static bool sLoggedFirstSceneAttempt = false;
	static bool sLoggedFrameShellSkip = false;
	static bool sLoggedOffscreenCanvasBypass = false;
	static bool sLoggedOffscreenCanvasSoftFallback = false;

	if (!mInitialized)
	{
		return false;
	}

	if (mLevelTransitionInProgress)
	{
		return true;
	}

	if (nri_ptdebug > 0 && !sLoggedFirstSceneAttempt)
	{
		Printf("NRI RenderPathTracedScene: drawmode=%d portal=%s active_target=%s size=%ux%u\n",
			drawmode,
			portal ? "yes" : "no",
			mActiveTarget == &mSceneTarget ? "scene" : (mActiveTarget == mCurrentPresentTarget ? "present" : (mActiveTarget == &mSaveTarget ? "save" : "other")),
			mActiveTarget != nullptr ? mActiveTarget->width : 0,
			mActiveTarget != nullptr ? mActiveTarget->height : 0);
		sLoggedFirstSceneAttempt = true;
	}

	if (nri_ptsanity && drawmode == DM_MAINVIEW && !portal)
	{
		mLastFrameBoundaryStats.sanityFrameUsed = true;
		return RenderPathTracingSanityFrame();
	}

	if (!mUsingSaveTarget && (!mFrameBegun || mCommandBuffer == nullptr || mActiveTarget == nullptr))
	{
		if (nri_ptdebug > 0 && !sLoggedFrameShellSkip)
		{
			Printf(TEXTCOLOR_ORANGE "NRI skipping raster fallback because the onscreen frame shell is unavailable (frame_begun=%s command_buffer=%s active_target=%s).\n",
				mFrameBegun ? "true" : "false",
				mCommandBuffer != nullptr ? "yes" : "no",
				mActiveTarget != nullptr ? "yes" : "no");
			sLoggedFrameShellSkip = true;
		}
		return true;
	}

	if (mRenderer == nullptr)
	{
		return false;
	}

	if (drawmode == DM_OFFSCREEN && mActiveCanvasTexture != nullptr)
	{
		if (!sLoggedOffscreenCanvasBypass || nri_ptdebug > 0)
		{
			Printf(TEXTCOLOR_ORANGE "NRI camera texture bypass: skipping PT offscreen rendering for canvas targets to avoid frame-resource size thrash with the main view; %s.\n",
				mActiveCanvasTexture->bFirstUpdate ? "clearing the target" : "preserving the previous canvas contents");
			sLoggedOffscreenCanvasBypass = true;
		}

		if (mActiveCanvasTexture->bFirstUpdate && mActiveTarget != nullptr)
		{
			ClearTargetColor(*mActiveTarget, mSceneClearColor[0], mSceneClearColor[1], mSceneClearColor[2], mSceneClearColor[3]);
		}
		return true;
	}

	NRIScopedGpuTiming sceneGpuTiming(
		drawmode == DM_MAINVIEW && !portal ? this : nullptr,
		NRIGpuTimingScope::Scene);
	const bool rendered = mRenderer->RenderScene(di, drawmode, portal);
	if (drawmode == DM_MAINVIEW && !portal)
	{
		CaptureCompactPerfRendererStats(rendered);
	}
	if (!rendered && drawmode == DM_OFFSCREEN && mActiveCanvasTexture != nullptr)
	{
		if (!sLoggedOffscreenCanvasSoftFallback || nri_ptdebug > 0)
		{
			Printf(TEXTCOLOR_ORANGE "NRI camera texture fallback: PT offscreen render failed while updating a canvas target; skipping unsupported raster fallback and %s.\n",
				mActiveCanvasTexture->bFirstUpdate ? "clearing the target" : "preserving the previous canvas contents");
			sLoggedOffscreenCanvasSoftFallback = true;
		}

		if (mActiveCanvasTexture->bFirstUpdate && mActiveTarget != nullptr)
		{
			ClearTargetColor(*mActiveTarget, mSceneClearColor[0], mSceneClearColor[1], mSceneClearColor[2], mSceneClearColor[3]);
		}
		return true;
	}
	mLastFrameBoundaryStats.pathTracedSceneRendered = mLastFrameBoundaryStats.pathTracedSceneRendered || rendered;
	if (rendered && ShouldEmitProgressiveSlowdownTrace(
		GetGameUpdateSnapshot().presentationGeneration))
	{
		EmitProgressiveSlowdownTrace(
			mLastFrameBoundaryStats.frameNumber,
			mRenderer->GetLastPerfShellTraceStats(),
			mRenderer->GetLastPerfResourceTraceStats(),
			mRenderer->GetLastPerfTraceShaderStats());
	}
	if (rendered && (PerfLoopTraceActive() || (bool)nri_pt360absenceprobe))
	{
		const auto& shell = mRenderer->GetLastPerfShellTraceStats();
		const auto& resource = mRenderer->GetLastPerfResourceTraceStats();
		const auto& shader = mRenderer->GetLastPerfTraceShaderStats();
		const auto getRuntimeMutationTraceActionName = [](NRIRenderer::RuntimeMutationTraceAction action) -> const char*
		{
			switch (action)
			{
			case NRIRenderer::RuntimeMutationTraceAction::StructuralRebuild: return "rebuild";
			case NRIRenderer::RuntimeMutationTraceAction::MaterialRefresh: return "material-refresh";
			case NRIRenderer::RuntimeMutationTraceAction::ResidentApply: return "resident-apply";
			case NRIRenderer::RuntimeMutationTraceAction::ResidentNoopSkip: return "resident-noop-skip";
			case NRIRenderer::RuntimeMutationTraceAction::ResidentFallback: return "fallback";
			case NRIRenderer::RuntimeMutationTraceAction::Held: return "held";
			case NRIRenderer::RuntimeMutationTraceAction::SyncSkip: return "sync-skip";
			case NRIRenderer::RuntimeMutationTraceAction::DeferredMaterialRefresh: return "deferred-material-refresh";
			case NRIRenderer::RuntimeMutationTraceAction::Failed: return "failed";
			default: return "none";
			}
		};
		const auto getRuntimeSectorDirtyPreviousStateSourceName =
			[](NRIRenderer::RuntimeSectorDirtyTruthTraceEntry::PreviousStateSource source) -> const char*
		{
			switch (source)
			{
			case NRIRenderer::RuntimeSectorDirtyTruthTraceEntry::PreviousStateSource::Replacement: return "replacement";
			case NRIRenderer::RuntimeSectorDirtyTruthTraceEntry::PreviousStateSource::Resident: return "resident";
			default: return "none";
			}
		};
		const auto getSceneDataSourceName = [](uint32_t dataSource) -> const char*
		{
			switch (dataSource)
			{
			case 0: return "static";
			case 1: return "dynamic";
			case 2: return "persistent_voxel";
			default: return "unknown";
			}
		};
		Printf("----------perf trace frame %llu\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber);
		Printf(
			"PERF pt shell trace NRI: frame=%llu total=%.3f init=%.3f map=%.3f state=%.3f select=%.3f lights=%.3f resident=%.3f emissive=%.3f emissive_tlas=%.3f surface=%.3f graph=%.3f post_frame_diagnostics_ms=%.3f unattributed_ms=%.3f other=%.3f used_static=%d used_dynamic=%d persistent=%d prims=%u dyn_prims=%u mats=%u scene_instances=%u\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.totalMs,
			shell.initResourcesMs,
			shell.mapWorldMs,
			shell.updateStateMs,
			shell.sceneSelectMs,
			shell.sceneLightsMs,
			shell.residentLightRefreshMs,
			shell.emissiveUpdateMs,
			shell.emissiveTlasMs,
			shell.surfaceProbeMs,
			shell.frameGraphMs,
			shell.postFrameDiagnosticsMs,
			shell.unattributedMs,
			shell.otherMs,
			shell.usedStaticMapScene ? 1 : 0,
			shell.usedDynamicOverlay ? 1 : 0,
			shell.usedPersistentDynamicEmissiveCache ? 1 : 0,
			shell.activePrimitiveCount,
			shell.dynamicPrimitiveCount,
			shell.activeMaterialCount,
			shell.sceneInstanceCount);
		Printf(
			"PERF pt success diagnostics NRI: frame=%llu basic=%u composition=%u persistent_status=%u as_summary=%u deep_scene=%u deep_cache_hits=%u deep_rebuilds=%u instance_rows=%u persistent_status_calls=%u static_chunk_rows=%u static_chunk_updates=%u static_surface_rows=%u static_surface_updates=%u registry_rows=%u temp_containers=%u voxel_dup_audits=%u voxel_dup_rows=%u voxel_dup_temp_containers=%u\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.successDiagnosticsBasicCollected ? 1u : 0u,
			shell.successDiagnosticsInstanceCompositionCollected ? 1u : 0u,
			shell.successDiagnosticsPersistentVoxelStatusCollected ? 1u : 0u,
			shell.successDiagnosticsAsSummaryCollected ? 1u : 0u,
			shell.successDiagnosticsDeepSceneAuditCollected ? 1u : 0u,
			shell.successDiagnosticsDeepSceneAuditCacheHits,
			shell.successDiagnosticsDeepSceneAuditRebuilds,
			shell.successDiagnosticsInstanceRowsScanned,
			shell.successDiagnosticsPersistentStatusCalls,
			shell.successDiagnosticsStaticChunkRowsScanned,
			shell.successDiagnosticsStaticChunkRowsIncrementallyUpdated,
			shell.successDiagnosticsStaticSurfaceRowsScanned,
			shell.successDiagnosticsStaticSurfaceRowsIncrementallyUpdated,
			shell.successDiagnosticsRegistryRowsScanned,
			shell.successDiagnosticsTemporaryContainersBuilt,
			shell.dynamicCaptureVoxelDuplicationAuditCalls,
			shell.dynamicCaptureVoxelDuplicationAuditEntriesScanned,
			shell.dynamicCaptureVoxelDuplicationAuditTemporaryContainersBuilt);
		Printf(
			"PERF pt scene composition NRI: frame=%llu inst_static=%u inst_dynamic=%u inst_persistent_voxel=%u voxel_mesh_variants=%u voxel_instances=%u voxel_prims=%u voxel_mats=%u voxel_prim_min=%u voxel_prim_max=%u\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.sceneInstanceStaticCount,
			shell.sceneInstanceDynamicCount,
			shell.sceneInstancePersistentVoxelCount,
			shell.persistentVoxelMeshVariantResourceCount,
			shell.persistentVoxelInstanceActiveCount,
			shell.persistentVoxelInstancePrimitiveCount,
			shell.persistentVoxelInstanceMaterialCount,
			shell.persistentVoxelInstanceMinPrimitiveCount,
			shell.persistentVoxelInstanceMaxPrimitiveCount);
		Printf(
			"PERF pt as model NRI: frame=%llu world_tlas_objects=%u world_tlas_entries=%u world_mask_all_workloads_refs=%u world_mask_other_refs=%u emissive_tlas_objects=%u emissive_tlas_entries=%u blas_static=%u blas_dynamic=%u blas_voxel_unique=%u blas_voxel_actor=%u entries_static=%u entries_dynamic=%u entries_voxel=%u scene_records=%u\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.asWorldTlasObjects,
			shell.asWorldTlasEntries,
			shell.asWorldTlasMaskAllWorkloadsRefs,
			shell.asWorldTlasMaskOtherRefs,
			shell.asEmissiveTlasObjects,
			shell.asEmissiveTlasEntries,
			shell.asBlasStatic,
			shell.asBlasDynamic,
			shell.asBlasVoxelUnique,
			shell.asBlasVoxelActor,
			shell.asEntriesStatic,
			shell.asEntriesDynamic,
			shell.asEntriesVoxel,
			shell.asSceneRecords);
		Printf(
			"PERF pt as reuse NRI: frame=%llu voxel_unique_geometry_keys=%u voxel_actor_instances=%u voxel_shared_blas_refs=%u static_unique_geometry_signatures=%u static_segment_blas=%u static_chunk_owned_blas=%u dynamic_unique_geometry_signatures=%u blas_cache_hits=%u blas_built_this_frame=%u monolithic_dynamic_blas_builds=%u\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.asVoxelUniqueGeometryKeys,
			shell.asVoxelActorInstances,
			shell.asVoxelSharedBlasRefs,
			shell.asStaticUniqueGeometrySignatures,
			shell.asStaticSegmentBlas,
			shell.asStaticChunkOwnedBlas,
			shell.asDynamicUniqueGeometrySignatures,
			shell.asBlasCacheHits,
			shell.asBlasBuiltThisFrame,
			shell.asMonolithicDynamicBlasBuilds);
		if (shell.successDiagnosticsDeepSceneAuditCollected)
		{
			Printf(
				"PERF pt static segment diagnostics NRI: frame=%llu candidate_chunks=%u unique_geometry_signatures=%u duplicate_keys=%u duplicate_refs=%u portal_chunks=%u local_space_chunks=%u animated_chunks=%u atlas_eligible_chunks=%u registry_mapped_chunks=%u candidate_surfaces=%u wall_candidates=%u floor_candidates=%u ceiling_candidates=%u portal_candidates=%u local_space_surfaces=%u animated_surfaces=%u material_risk_surfaces=%u contiguous_chunk_surfaces=%u chunk_owned_blas=%u segment_blas=%u\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.asStaticSegmentCandidateChunks,
			shell.asStaticSegmentUniqueGeometrySignatures,
			shell.asStaticSegmentDuplicateKeys,
			shell.asStaticSegmentDuplicateRefs,
			shell.asStaticSegmentPortalChunks,
			shell.asStaticSegmentLocalSpaceChunks,
			shell.asStaticSegmentAnimatedChunks,
			shell.asStaticSegmentAtlasEligibleChunks,
			shell.asStaticSegmentRegistryMappedChunks,
			shell.asStaticSegmentCandidateSurfaces,
			shell.asStaticSegmentWallCandidates,
			shell.asStaticSegmentFloorCandidates,
			shell.asStaticSegmentCeilingCandidates,
			shell.asStaticSegmentPortalCandidates,
			shell.asStaticSegmentLocalSpaceSurfaces,
			shell.asStaticSegmentAnimatedSurfaces,
			shell.asStaticSegmentMaterialRiskSurfaces,
			shell.asStaticSegmentContiguousChunkSurfaces,
			shell.asStaticChunkOwnedBlas,
			shell.asStaticSegmentBlas);
			Printf(
				"PERF pt static segment cache NRI: frame=%llu candidates=%u entries=%u hits=%u misses=%u duplicate_refs=%u resident_blas=%u builds_this_frame=%u builds_last_rebuild=%u invalidations=%u resident_bytes=%llu blas_build_enabled=%u\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.asStaticSegmentCacheCandidates,
			shell.asStaticSegmentCacheEntries,
			shell.asStaticSegmentCacheHits,
			shell.asStaticSegmentCacheMisses,
			shell.asStaticSegmentCacheDuplicateRefs,
			shell.asStaticSegmentCacheResidentBlas,
			shell.asStaticSegmentCacheBuildsThisFrame,
			shell.asStaticSegmentCacheBuildsLastRebuild,
			shell.asStaticSegmentCacheInvalidations,
			(unsigned long long)shell.asStaticSegmentCacheResidentBytes,
			shell.asStaticSegmentCacheBlasBuildEnabled ? 1u : 0u);
			Printf(
				"PERF pt static segment route NRI: frame=%llu routed_segment=%u chunk_fallback=%u reject_disabled=%u reject_missing_cache=%u reject_missing_blas=%u segment_blas_refs=%u chunk_blas_refs=%u\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.asStaticSegmentRouteRouted,
			shell.asStaticSegmentRouteChunkFallback,
			shell.asStaticSegmentRouteRejectDisabled,
			shell.asStaticSegmentRouteRejectMissingCache,
			shell.asStaticSegmentRouteRejectMissingBlas,
			shell.asStaticSegmentRouteSegmentBlasRefs,
				shell.asStaticSegmentRouteChunkBlasRefs);
		}
		Printf(
			"PERF pt emissive as model NRI: frame=%llu enabled=%u records=%u records_static=%u records_dynamic=%u records_persistent_voxel=%u static_record_matched_chunks=%u static_record_unmatched_chunks=%u dynamic_records=%u persistent_voxel_ignored_records=%u static_chunk_refs=%u dynamic_aggregate_refs=%u mask_all_workloads_refs=%u mask_other_refs=%u payload_cache_hits=%u payload_cache_misses=%u sampling_surface_static=%u sampling_surface_captured=%u sampling_surface_runtime_mutation=%u sampling_surface_dynamic=%u sampling_surface_persistent_voxel=%u sampling_output_static=%u sampling_output_dynamic=%u sampling_output_persistent_voxel=%u sampling_skipped_persistent_voxel=%u\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.emissiveAsEnabled ? 1u : 0u,
			shell.emissiveAsRecords,
			shell.emissiveAsRecordsStatic,
			shell.emissiveAsRecordsDynamic,
			shell.emissiveAsRecordsPersistentVoxel,
			shell.emissiveAsStaticRecordMatchedChunks,
			shell.emissiveAsStaticRecordUnmatchedChunks,
			shell.emissiveAsDynamicRecordCount,
			shell.emissiveAsPersistentVoxelIgnoredRecords,
			shell.emissiveAsStaticChunkRefs,
			shell.emissiveAsDynamicAggregateRefs,
			shell.emissiveAsMaskAllWorkloadsRefs,
			shell.emissiveAsMaskOtherRefs,
			shell.emissiveAsPayloadCacheHits,
			shell.emissiveAsPayloadCacheMisses,
			shell.emissiveSamplingSurfaceStatic,
			shell.emissiveSamplingSurfaceCaptured,
			shell.emissiveSamplingSurfaceRuntimeMutation,
			shell.emissiveSamplingSurfaceDynamic,
			shell.emissiveSamplingSurfacePersistentVoxel,
			shell.emissiveSamplingOutputStaticRecords,
			shell.emissiveSamplingOutputDynamicRecords,
			shell.emissiveSamplingOutputPersistentVoxelRecords,
			shell.emissiveSamplingSkippedPersistentVoxelSurfaces);
		Printf(
			"PERF pt dynamic overlay blas NRI: frame=%llu build_enabled=%u route_enabled=%u build_budget=%u domains=%u vertices=%u indices=%u prims=%u mats=%u eligible_domains=%u eligible_prims=%u fallback_domains=%u fallback_prims=%u reject_disabled=%u reject_static_overlay=%u reject_runtime_space_link=%u reject_runtime_mutation=%u reject_local_player_reflection=%u reject_runtime_debug_sphere=%u reject_surface_light_overlay=%u reject_material_base=%u build_attempts=%u build_successes=%u cache_hits=%u cache_misses=%u routed_instances=%u monolithic_refs=%u\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.dynamicOverlayBlasBuildEnabled ? 1u : 0u,
			shell.dynamicOverlayBlasRouteEnabled ? 1u : 0u,
			shell.dynamicOverlayBlasBuildBudget,
			shell.dynamicOverlayBlasDomainCount,
			shell.dynamicOverlayBlasVertexCount,
			shell.dynamicOverlayBlasIndexCount,
			shell.dynamicOverlayBlasPrimitiveCount,
			shell.dynamicOverlayBlasMaterialCount,
			shell.dynamicOverlayBlasEligibleDomains,
			shell.dynamicOverlayBlasEligiblePrimitives,
			shell.dynamicOverlayBlasFallbackDomains,
			shell.dynamicOverlayBlasFallbackPrimitives,
			shell.dynamicOverlayBlasRejectDisabled,
			shell.dynamicOverlayBlasRejectStaticOverlay,
			shell.dynamicOverlayBlasRejectRuntimeSpaceLink,
			shell.dynamicOverlayBlasRejectRuntimeMutation,
			shell.dynamicOverlayBlasRejectLocalPlayerReflection,
			shell.dynamicOverlayBlasRejectRuntimeDebugSphere,
			shell.dynamicOverlayBlasRejectSurfaceLightOverlay,
			shell.dynamicOverlayBlasRejectMaterialBase,
			shell.dynamicOverlayBlasBuildAttempts,
			shell.dynamicOverlayBlasBuildSuccesses,
			shell.dynamicOverlayBlasCacheHits,
			shell.dynamicOverlayBlasCacheMisses,
			shell.dynamicOverlayBlasRoutedInstances,
			shell.dynamicOverlayBlasMonolithicRefs);
		Printf(
			"PERF pt world tlas detail NRI: frame=%llu total=%.3f retire=%.3f instance_upload=%.3f create=%.3f memory=%.3f scratch=%.3f descriptor=%.3f build=%.3f update=%.3f barrier=%.3f calls=%u exact_reuses=%u updates=%u update_reason_mask=0x%08x update_dirty_ranges=%u update_dirty_instances=%u update_dirty_bytes=%llu blas_override_updates=%u full_builds=%u full_reason_mask=0x%08x full_change_mask=0x%08x full_update_reject_mask=0x%08x full_update_gate_mask=0x%08x full_destination_reuses=%u full_destination_creates=%u full_growths=%u full_growth_mask=0x%08x full_reuse_reject_mask=0x%08x full_reuse_runtime_fallbacks=%u same_command_rotations=%u blas_generation=%llu instances=%u creates=%u scratch_queries=%u scratch_grows=%u scratch_requested=%llu build_scratch_requested=%llu update_scratch_requested=%llu scratch_allocated=%llu memory_bytes=%llu descriptor_creates=%u barriers=%u\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.worldTlasMs,
			shell.worldTlasRetireMs,
			shell.worldTlasInstanceUploadMs,
			shell.worldTlasCreateMs,
			shell.worldTlasMemoryMs,
			shell.worldTlasScratchMs,
			shell.worldTlasDescriptorMs,
			shell.worldTlasBuildMs,
			shell.worldTlasUpdateMs,
			shell.worldTlasBarrierMs,
			shell.worldTlasBuildCalls,
			shell.worldTlasExactReuseCalls,
			shell.worldTlasUpdateCalls,
			shell.worldTlasUpdateReasonMask,
			shell.worldTlasUpdateDirtyRangeCount,
			shell.worldTlasUpdateDirtyInstanceCount,
			(unsigned long long)shell.worldTlasUpdateDirtyBytes,
			shell.worldTlasBlasOverrideUpdateCalls,
			shell.worldTlasFullBuildCalls,
			shell.worldTlasFullBuildReasonMask,
			shell.worldTlasFullBuildChangeReasonMask,
			shell.worldTlasFullBuildUpdateRejectReasonMask,
			shell.worldTlasFullBuildUpdateGateReasonMask,
			shell.worldTlasFullBuildDestinationReuseCalls,
			shell.worldTlasFullBuildDestinationCreateCalls,
			shell.worldTlasFullBuildGrowthCalls,
			shell.worldTlasFullBuildGrowthReasonMask,
			shell.worldTlasFullBuildReuseRejectReasonMask,
			shell.worldTlasFullBuildReuseRuntimeFallbacks,
			shell.worldTlasSameCommandResourceRotations,
			(unsigned long long)shell.worldTlasBlasContentGeneration,
			shell.worldTlasInstanceCount,
			shell.worldTlasCreateCalls,
			shell.worldTlasScratchQueries,
			shell.worldTlasScratchGrowCount,
			(unsigned long long)shell.worldTlasScratchRequestedBytes,
			(unsigned long long)shell.worldTlasBuildScratchRequestedBytes,
			(unsigned long long)shell.worldTlasUpdateScratchRequestedBytes,
			(unsigned long long)shell.worldTlasScratchAllocatedBytes,
			(unsigned long long)shell.worldTlasMemoryBytes,
			shell.worldTlasDescriptorCreateCalls,
			shell.worldTlasBarrierCount);
		if (shell.successDiagnosticsDeepSceneAuditCollected)
		{
			Printf(
				"PERF pt scene record audit NRI: frame=%llu records=%u static=%u dynamic=%u persistent_voxel=%u invalid_source=%u visibility_chunked=%u legacy_compatible=%u material_indirection=%u\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.sceneRecordAuditRecords,
			shell.sceneRecordAuditStatic,
			shell.sceneRecordAuditDynamic,
			shell.sceneRecordAuditPersistentVoxel,
			shell.sceneRecordAuditInvalidSource,
			shell.sceneRecordAuditVisibilityChunked,
			shell.sceneRecordAuditLegacyCompatible,
			shell.sceneRecordAuditMaterialIndirection);
			Printf(
				"PERF pt hit metadata NRI: frame=%llu records=%u primitive_base_mismatches=%u material_base_mismatches=%u legacy_primitive_offset_matches=%u persistent_material_base_records=%u\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.hitMetadataAuditRecords,
			shell.hitMetadataPrimitiveBaseMismatches,
			shell.hitMetadataMaterialBaseMismatches,
			shell.hitMetadataLegacyPrimitiveOffsetMatches,
				shell.hitMetadataPersistentMaterialBaseRecords);
		}
		Printf(
			"PERF pt voxel shared blas NRI: frame=%llu active_actors=%u unique_desired_keys=%u resident_shared_assets=%u queued_shared_assets=%u eligible_build_keys=%u build_attempts=%u build_successes=%u build_failures=%u cache_hits=%u cache_misses=%u actor_refs=%u routed_legacy=%u routed_shared=%u fallback_last_valid=%u active_referenced_assets=%u unreferenced_resident_assets=%u resident_bytes=%llu active_referenced_bytes=%llu unreferenced_resident_bytes=%llu route_eligible_actors=%u route_reject_missing_resident=%u route_reject_non_local=%u route_reject_transform_keyed=%u route_reject_invalid_material=%u route_reject_invalid_transform=%u route_reject_geometry_mismatch=%u reject_missing_key=%u reject_disabled=%u reject_non_local=%u reject_transform_keyed=%u reject_missing_buffers=%u reject_invalid_counts=%u reject_build_budget=%u reject_geometry_mismatch=%u\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.voxelSharedBlasActiveActors,
			shell.voxelSharedBlasUniqueDesiredKeys,
			shell.voxelSharedBlasResidentAssets,
			shell.voxelSharedBlasQueuedAssets,
			shell.voxelSharedBlasEligibleBuildKeys,
			shell.voxelSharedBlasBuildAttempts,
			shell.voxelSharedBlasBuildSuccesses,
			shell.voxelSharedBlasBuildFailures,
			shell.voxelSharedBlasCacheHits,
			shell.voxelSharedBlasCacheMisses,
			shell.voxelSharedBlasActorRefs,
			shell.voxelSharedBlasRoutedLegacy,
			shell.voxelSharedBlasRoutedShared,
			shell.voxelSharedBlasFallbackLastValid,
			shell.voxelSharedBlasActiveReferencedAssets,
			shell.voxelSharedBlasUnreferencedResidentAssets,
			(unsigned long long)shell.voxelSharedBlasResidentBytes,
			(unsigned long long)shell.voxelSharedBlasActiveReferencedBytes,
			(unsigned long long)shell.voxelSharedBlasUnreferencedResidentBytes,
			shell.voxelSharedBlasRouteEligibleActors,
			shell.voxelSharedBlasRouteRejectMissingResident,
			shell.voxelSharedBlasRouteRejectNonLocal,
			shell.voxelSharedBlasRouteRejectTransformKeyed,
			shell.voxelSharedBlasRouteRejectInvalidMaterial,
			shell.voxelSharedBlasRouteRejectInvalidTransform,
			shell.voxelSharedBlasRouteRejectGeometryMismatch,
			shell.voxelSharedBlasRejectMissingKey,
			shell.voxelSharedBlasRejectDisabled,
			shell.voxelSharedBlasRejectNonLocal,
			shell.voxelSharedBlasRejectTransformKeyed,
			shell.voxelSharedBlasRejectMissingBuffers,
			shell.voxelSharedBlasRejectInvalidCounts,
			shell.voxelSharedBlasRejectBuildBudget,
			shell.voxelSharedBlasRejectGeometryMismatch);
		Printf(
			"PERF pt voxel local share profile NRI: frame=%llu active_actors=%u local_space=%u baked_transform=%u unknown_space=%u transform_keyed=%u local_identity_transform=%u local_non_identity_transform=%u shareable_local=%u shareable_unique_keys=%u shareable_duplicate_refs=%u shareable_single_actor_keys=%u shareable_multi_actor_keys=%u resident_shareable_keys=%u eligible_not_resident=%u reject_missing_mesh=%u reject_non_local=%u reject_transform_keyed=%u reject_missing_buffers=%u reject_invalid_counts=%u reject_invalid_material=%u reject_invalid_transform=%u reject_geometry_mismatch=%u resident_bytes=%llu active_referenced_bytes=%llu unreferenced_resident_bytes=%llu\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.voxelLocalShareProfileActiveActors,
			shell.voxelLocalShareProfileLocalSpaceActors,
			shell.voxelLocalShareProfileBakedTransformActors,
			shell.voxelLocalShareProfileUnknownSpaceActors,
			shell.voxelLocalShareProfileTransformKeyedActors,
			shell.voxelLocalShareProfileLocalIdentityTransformActors,
			shell.voxelLocalShareProfileLocalNonIdentityTransformActors,
			shell.voxelLocalShareProfileShareableLocalActors,
			shell.voxelLocalShareProfileShareableUniqueKeys,
			shell.voxelLocalShareProfileShareableDuplicateActorRefs,
			shell.voxelLocalShareProfileShareableSingleActorKeys,
			shell.voxelLocalShareProfileShareableMultiActorKeys,
			shell.voxelLocalShareProfileResidentShareableKeys,
			shell.voxelLocalShareProfileEligibleNotResidentActors,
			shell.voxelLocalShareProfileRejectMissingMesh,
			shell.voxelLocalShareProfileRejectNonLocal,
			shell.voxelLocalShareProfileRejectTransformKeyed,
			shell.voxelLocalShareProfileRejectMissingBuffers,
			shell.voxelLocalShareProfileRejectInvalidCounts,
			shell.voxelLocalShareProfileRejectInvalidMaterial,
			shell.voxelLocalShareProfileRejectInvalidTransform,
			shell.voxelLocalShareProfileRejectGeometryMismatch,
			(unsigned long long)shell.voxelSharedBlasResidentBytes,
			(unsigned long long)shell.voxelSharedBlasActiveReferencedBytes,
			(unsigned long long)shell.voxelSharedBlasUnreferencedResidentBytes);
		Printf(
			"PERF pt voxel shared key audit NRI: frame=%llu actors=%u keys=%u safe_keys=%u unsafe_keys=%u geometry_mismatch_keys=%u count_mismatch_keys=%u material_variant_keys=%u material_count_mismatch_keys=%u source_picnum_alias_keys=%u voxel_index_alias_keys=%u source_state_alias_actor_refs=%u bake_space_mismatch_keys=%u transform_basis_mismatch_keys=%u local_shareable_unsafe_keys=%u\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.voxelSharedKeyAuditActors,
			shell.voxelSharedKeyAuditKeys,
			shell.voxelSharedKeyAuditSafeKeys,
			shell.voxelSharedKeyAuditUnsafeKeys,
			shell.voxelSharedKeyAuditGeometryMismatchKeys,
			shell.voxelSharedKeyAuditCountMismatchKeys,
			shell.voxelSharedKeyAuditMaterialVariantKeys,
			shell.voxelSharedKeyAuditMaterialCountMismatchKeys,
			shell.voxelSharedKeyAuditSourcePicnumAliasKeys,
			shell.voxelSharedKeyAuditVoxelIndexAliasKeys,
			shell.voxelSharedKeyAuditSourceStateAliasActorRefs,
			shell.voxelSharedKeyAuditBakeSpaceMismatchKeys,
			shell.voxelSharedKeyAuditTransformBasisMismatchKeys,
			shell.voxelSharedKeyAuditLocalShareableUnsafeKeys);
		Printf(
			"PERF pt voxel local invariant NRI: frame=%llu local_actors=%u identity_transform=%u non_identity_transform=%u suspicious_world_bounds=%u missing_bounds=%u invalid_transform=%u baked_fallback=%u unknown_space=%u max_bounds_center=%.3f max_bounds_abs=%.3f\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.voxelLocalSpaceInvariantLocalActors,
			shell.voxelLocalSpaceInvariantLocalIdentityTransformActors,
			shell.voxelLocalSpaceInvariantLocalNonIdentityTransformActors,
			shell.voxelLocalSpaceInvariantSuspiciousWorldBoundsActors,
			shell.voxelLocalSpaceInvariantMissingBoundsActors,
			shell.voxelLocalSpaceInvariantInvalidTransformActors,
			shell.voxelLocalSpaceInvariantBakedFallbackActors,
			shell.voxelLocalSpaceInvariantUnknownSpaceActors,
			shell.voxelLocalSpaceInvariantMaxBoundsCenterMagnitude,
			shell.voxelLocalSpaceInvariantMaxBoundsAbs);
		const auto& shaderObserver = shader.observer;
		if (shader.valid || shaderObserver.copiesRequested != 0 || shaderObserver.pendingReadbackCount != 0)
		{
			Printf(
				"PERF pt shader stats observer NRI: frame=%llu stats_frame=%llu valid=%u copies=%llu recorded=%llu busy=%llu no_fence=%llu published=%llu superseded=%llu abandoned=%llu map_fail=%llu pending=%u attribution_rows=%llu attribution_bytes=%llu\n",
				(unsigned long long)mLastFrameBoundaryStats.frameNumber,
				(unsigned long long)shader.frameNumber,
				shader.valid ? 1u : 0u,
				(unsigned long long)shaderObserver.copiesRequested,
				(unsigned long long)shaderObserver.copiesRecorded,
				(unsigned long long)shaderObserver.copiesDroppedBusy,
				(unsigned long long)shaderObserver.copiesDroppedNoFence,
				(unsigned long long)shaderObserver.readbacksPublished,
				(unsigned long long)shaderObserver.readbacksSuperseded,
				(unsigned long long)shaderObserver.readbacksAbandoned,
				(unsigned long long)shaderObserver.readbackMapFailures,
				shaderObserver.pendingReadbackCount,
				(unsigned long long)shaderObserver.attributionRowsCopied,
				(unsigned long long)shaderObserver.attributionBytesCopied);
		}
		if (shader.valid)
		{
			const auto& c = shader.counters;
			Printf(
				"PERF pt shader trace NRI: frame=%llu stats_frame=%llu trace_calls=%u primary=%u ungated=%u sun=%u point=%u emissive=%u fast_emissive=%u committed=%u miss=%u accept_static=%u accept_dynamic=%u accept_voxel=%u skips=%u max_skip=%u limit=%u\n",
				(unsigned long long)mLastFrameBoundaryStats.frameNumber,
				(unsigned long long)shader.frameNumber,
				c[0], c[1], c[2], c[3], c[4], c[5], c[6],
				c[7], c[8], c[9], c[10], c[11], c[12], c[13], c[14]);
			Printf(
				"PERF pt shader reject NRI: frame=%llu stats_frame=%llu reflection=%u visible=%u hidden_flat=%u oneway=%u transparent=%u noshadow=%u reject_static=%u reject_dynamic=%u reject_voxel=%u runtime_candidates=%u runtime_dist=%u runtime_lambert=%u runtime_shadow_rays=%u emissive_samples=%u emissive_shadow_rays=%u instance_committed_overflow=%u instance_accepted_overflow=%u\n",
				(unsigned long long)mLastFrameBoundaryStats.frameNumber,
				(unsigned long long)shader.frameNumber,
				c[15], c[16], c[17], c[18], c[19], c[20], c[21], c[22], c[23],
				c[24], c[25], c[26], c[27], c[28], c[29], c[30], c[31]);
			Printf(
				"PERF pt shader phase NRI: frame=%llu stats_frame=%llu primary_hit=%u primary_miss=%u hit_static=%u hit_dynamic=%u hit_voxel=%u fullbright=%u emissive_material=%u dir_shadow_tests=%u runtime_tile_nonempty=%u runtime_tile_max=%u runtime_shadow_visible=%u runtime_shadow_occluded=%u indirect_diffuse_calls=%u indirect_diffuse_bounces=%u indirect_diffuse_misses=%u indirect_specular_calls=%u indirect_specular_bounces=%u indirect_specular_misses=%u sun_shadow_calls=%u point_shadow_calls=%u fast_emissive_shadow_calls=%u runtime_soft_shadow_samples=%u runtime_soft_transport=%u\n",
				(unsigned long long)mLastFrameBoundaryStats.frameNumber,
				(unsigned long long)shader.frameNumber,
				c[32], c[33], c[34], c[35], c[36], c[37], c[38], c[39],
				c[40], c[41], c[42], c[43], c[52], c[53], c[56], c[54], c[55], c[57],
				c[61], c[60], c[58], c[62], c[63]);
			Printf(
				"PERF pt shader emissive detail NRI: frame=%llu stats_frame=%llu candidate_none=%u light_zero=%u distance_reject=%u receiver_lambert_reject=%u emitter_lambert_reject=%u visibility_visible=%u visibility_occluded=%u contributed=%u traced_shadow_calls=%u fast_shadow_calls=%u\n",
				(unsigned long long)mLastFrameBoundaryStats.frameNumber,
				(unsigned long long)shader.frameNumber,
				c[44], c[45], c[46], c[47], c[48], c[49], c[50], c[51], c[59], c[58]);
			Printf(
				"PERF pt shader 360 absence NRI: frame=%llu stats_frame=%llu primary=%u ungated=%u sun=%u point=%u emissive=%u fast_emissive=%u snapshot_fail_open=%u witness_tests=%u snapshot_invalid=%u frame_mismatch=%u outside_guard=%u lookup_miss=%u outside_union=%u exact_miss=%u actor_census_reject=%u\n",
				(unsigned long long)mLastFrameBoundaryStats.frameNumber,
				(unsigned long long)shader.frameNumber,
				c[64], c[65], c[66], c[67], c[68], c[69], c[70], c[71],
				c[72], c[73], c[74], c[75], c[76], c[77], c[78]);
			EmitSpatialAbsenceProbeTrace(mLastFrameBoundaryStats.frameNumber, shader);
			for (uint32_t hotIndex = 0; hotIndex < shader.hotInstanceCount; ++hotIndex)
			{
				const auto& hot = shader.hotInstances[hotIndex];
				Printf(
					"PERF pt shader hot instance NRI: frame=%llu stats_frame=%llu rank=%u instance=%u source=%s primitive_offset=%u primitive_count=%u metadata0=%u metadata1=%u metadata2=%u committed=%u accepted=%u primary=%u ungated=%u sun=%u point=%u emissive=%u fast_emissive=%u\n",
					(unsigned long long)mLastFrameBoundaryStats.frameNumber,
					(unsigned long long)shader.frameNumber,
					hotIndex + 1u,
					hot.instanceId,
					getSceneDataSourceName(hot.dataSource),
					hot.primitiveOffset,
					hot.primitiveCount,
					hot.metadata0,
					hot.metadata1,
					hot.metadata2,
					hot.committed,
					hot.accepted,
					hot.primaryCommitted,
					hot.ungatedCommitted,
					hot.sunCommitted,
					hot.pointCommitted,
					hot.emissiveCommitted,
					hot.fastEmissiveCommitted);
			}
		}
		const double sceneSelectAccountedMs =
			shell.sceneSelectStaticMapMs +
			shell.runtimeSpaceLinkMs +
			shell.runtimeMutationMs +
			shell.dynamicCaptureMs +
			shell.geometryBuildDynamicLiveMs +
			shell.geometryBuildLocalPlayerReflectionMs +
			shell.sceneSelectPersistentVoxelBatchMs +
			shell.sceneSelectPersistentEmissiveMs +
			shell.persistentDynamicMs +
			shell.sceneSelectDynamicMergeMs +
			shell.sceneSelectLightMergeMs +
			shell.runtimeDebugSphereMs +
			shell.overlayAppendMs +
			shell.sceneSelectStaticInstancesMs +
			shell.sceneSelectMaterialBridgeMs +
			shell.sceneSelectPaletteMs +
			shell.sceneSelectTexturesMs +
			shell.sceneSelectMaterialSplitMs +
			shell.sceneSelectBufferUploadMs +
			shell.persistentVoxelAsMs +
			shell.dynamicAsMs +
			shell.sceneSelectInstanceHandlesMs +
			shell.worldTlasMs +
			shell.sceneDataSetMs +
			shell.sceneSelectTexturePrepMs +
			shell.sceneSelectStateCommitMs;
		const double sceneSelectUnaccountedMs = shell.sceneSelectMs - sceneSelectAccountedMs;
		Printf(
			"PERF pt shell detail NRI: frame=%llu static_scene=%.3f mutation=%.3f mutation_analyze=%.3f mutation_rebuild=%.3f mutation_append=%.3f mutation_candidates=%u mutation_analyzed=%u mutation_sweep=%u mutation_dirty=%u mutation_rebuilt=%u mutation_held=%u mutation_prims=%u mutation_mats=%u spacelink=%.3f spacelink_prims=%u spacelink_mats=%u debug_sphere=%.3f debug_view=%.3f debug_geo=%.3f debug_mats=%.3f debug_tune=%.3f debug_spheres=%u debug_lons=%u debug_lats=%u debug_prims=%u debug_mats_out=%u overlay=%.3f overlay_prims=%u overlay_mats=%u dynamic_capture=%.3f persistent=%.3f dynamic_as=%.3f dynamic_as_setup=%.3f dynamic_as_create=%.3f dynamic_as_scratch=%.3f dynamic_as_build=%.3f dynamic_as_barrier=%.3f dynamic_as_prims=%u dynamic_as_verts=%u dynamic_as_indices=%u restore_static=%.3f copy_final=%.3f\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.staticSceneMs,
			shell.runtimeMutationMs,
			shell.runtimeMutationAnalyzeMs,
			shell.runtimeMutationRebuildMs,
			shell.runtimeMutationAppendMs,
			shell.runtimeMutationCandidateChunks,
			shell.runtimeMutationAnalyzedChunks,
			shell.runtimeMutationBackgroundSweepChunks,
			shell.runtimeMutationDirtyChunks,
			shell.runtimeMutationRebuiltChunks,
			shell.runtimeMutationHeldChunks,
			shell.runtimeMutationPrimitiveCount,
			shell.runtimeMutationMaterialCount,
			shell.runtimeSpaceLinkMs,
			shell.runtimeSpaceLinkPrimitiveCount,
			shell.runtimeSpaceLinkMaterialCount,
			shell.runtimeDebugSphereMs,
			shell.runtimeDebugSphereViewMs,
			shell.runtimeDebugSphereGeoMs,
			shell.runtimeDebugSphereMaterialMs,
			shell.runtimeDebugSphereTuneMs,
			shell.runtimeDebugSphereCount,
			shell.runtimeDebugSphereLongitudeSegments,
			shell.runtimeDebugSphereLatitudeSegments,
			shell.runtimeDebugSpherePrimitiveCount,
			shell.runtimeDebugSphereMaterialCount,
			shell.overlayAssembleMs,
			shell.overlayPrimitiveCount,
			shell.overlayMaterialCount,
			shell.dynamicCaptureMs,
			shell.persistentDynamicMs,
			shell.dynamicAsMs,
			shell.dynamicAsSetupMs,
			shell.dynamicAsCreateMs,
			shell.dynamicAsScratchMs,
			shell.dynamicAsBuildMs,
			shell.dynamicAsBarrierMs,
			shell.dynamicAsPrimitiveCount,
			shell.dynamicAsVertexCount,
			shell.dynamicAsIndexCount,
			shell.restoreStaticSceneMs,
			shell.copyFinalMs);
		Printf(
			"PERF pt local player reflection detail NRI: frame=%llu capture=%.3f geometry_total=%.3f geometry_build=%.3f portal_assign=%.3f material=%.3f append=%.3f append_geo=%.3f append_mat=%.3f vertices=%u indices=%u prims=%u mats=%u bytes=%llu\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.localPlayerReflectionCaptureMs,
			shell.geometryBuildLocalPlayerReflectionMs,
			shell.localPlayerReflectionGeometryBuildMs,
			shell.localPlayerReflectionPortalAssignMs,
			shell.localPlayerReflectionMaterialBuildMs,
			shell.overlayLocalPlayerReflectionMs,
			shell.overlayLocalPlayerReflectionGeometryMs,
			shell.overlayLocalPlayerReflectionMaterialMs,
			shell.overlayLocalPlayerReflectionAppend.vertexCount,
			shell.overlayLocalPlayerReflectionAppend.indexCount,
			shell.overlayLocalPlayerReflectionAppend.primitiveCount,
			shell.overlayLocalPlayerReflectionAppend.materialCount,
			(unsigned long long)shell.overlayLocalPlayerReflectionAppend.byteCount);
		Printf(
			"PERF pt local player reflection geometry NRI: frame=%llu wall_ms=%.3f flat_ms=%.3f sprite_ms=%.3f raw_facing=%u raw_voxels=%u captured_surfaces=%u captured_match=%u captured_other=%u captured_actorless=%u filtered_surfaces=%u wall_surfaces=%u flat_surfaces=%u sprite_surfaces=%u indexed_surfaces=%u fan_surfaces=%u strip_surfaces=%u skipped_surfaces=%u source_vertices=%u source_indices=%u output_vertices=%u output_indices=%u output_prims=%u vertex_grows=%u index_grows=%u primitive_grows=%u provenance_grows=%u\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.localPlayerReflectionGeometryBuildWallMs,
			shell.localPlayerReflectionGeometryBuildFlatMs,
			shell.localPlayerReflectionGeometryBuildSpriteMs,
			shell.localPlayerReflectionCaptureRawFacingSprites,
			shell.localPlayerReflectionCaptureRawVoxelSprites,
			shell.localPlayerReflectionCaptureSurfaces,
			shell.localPlayerReflectionCaptureMatchingActorSurfaces,
			shell.localPlayerReflectionCaptureOtherActorSurfaces,
			shell.localPlayerReflectionCaptureActorlessSurfaces,
			shell.localPlayerReflectionCaptureFilteredSurfaces,
			shell.localPlayerReflectionGeometryWallSurfaces,
			shell.localPlayerReflectionGeometryFlatSurfaces,
			shell.localPlayerReflectionGeometrySpriteSurfaces,
			shell.localPlayerReflectionGeometryIndexedSurfaces,
			shell.localPlayerReflectionGeometryTriangleFanSurfaces,
			shell.localPlayerReflectionGeometrySpriteStripSurfaces,
			shell.localPlayerReflectionGeometrySkippedSurfaces,
			shell.localPlayerReflectionGeometrySourceVertices,
			shell.localPlayerReflectionGeometrySourceIndices,
			shell.overlayLocalPlayerReflectionAppend.vertexCount,
			shell.overlayLocalPlayerReflectionAppend.indexCount,
			shell.overlayLocalPlayerReflectionAppend.primitiveCount,
			shell.localPlayerReflectionGeometryVertexGrowths,
			shell.localPlayerReflectionGeometryIndexGrowths,
			shell.localPlayerReflectionGeometryPrimitiveGrowths,
			shell.localPlayerReflectionGeometryProvenanceGrowths);
		Printf(
			"PERF pt local player reflection append NRI: frame=%llu total=%.3f geo=%.3f mat=%.3f vertices=%u indices=%u prims=%u mats=%u vertex_bytes=%llu index_bytes=%llu primitive_bytes=%llu material_bytes=%llu total_bytes=%llu geometry_grows=%u material_grows=%u\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.overlayLocalPlayerReflectionMs,
			shell.overlayLocalPlayerReflectionGeometryMs,
			shell.overlayLocalPlayerReflectionMaterialMs,
			shell.overlayLocalPlayerReflectionAppend.vertexCount,
			shell.overlayLocalPlayerReflectionAppend.indexCount,
			shell.overlayLocalPlayerReflectionAppend.primitiveCount,
			shell.overlayLocalPlayerReflectionAppend.materialCount,
			(unsigned long long)shell.overlayLocalPlayerReflectionAppend.vertexBytes,
			(unsigned long long)shell.overlayLocalPlayerReflectionAppend.indexBytes,
			(unsigned long long)shell.overlayLocalPlayerReflectionAppend.primitiveBytes,
			(unsigned long long)shell.overlayLocalPlayerReflectionAppend.materialBytes,
			(unsigned long long)shell.overlayLocalPlayerReflectionAppend.byteCount,
			shell.overlayLocalPlayerReflectionAppend.geometryGrowthEvents,
			shell.overlayLocalPlayerReflectionAppend.materialGrowthEvents);
		Printf(
			"PERF pt dynamic as input NRI: frame=%llu total=%.3f setup=%.3f create=%.3f scratch=%.3f build=%.3f barrier=%.3f total_prims=%u total_vertices=%u total_indices=%u creates=%u reuses=%u scratch_queries=%u scratch_grows=%u scratch_requested_bytes=%llu as_bytes=%llu spacelink_prims=%u spacelink_bytes=%llu mutation_prims=%u mutation_bytes=%llu dynamic_prims=%u dynamic_bytes=%llu local_player_reflection_prims=%u local_player_reflection_bytes=%llu debug_sphere_prims=%u debug_sphere_bytes=%llu\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.dynamicAsMs,
			shell.dynamicAsSetupMs,
			shell.dynamicAsCreateMs,
			shell.dynamicAsScratchMs,
			shell.dynamicAsBuildMs,
			shell.dynamicAsBarrierMs,
			shell.dynamicAsPrimitiveCount,
			shell.dynamicAsVertexCount,
			shell.dynamicAsIndexCount,
			shell.dynamicAsCreateCalls,
			shell.dynamicAsReuseCount,
			shell.dynamicAsScratchQueries,
			shell.dynamicAsScratchGrowCount,
			(unsigned long long)shell.dynamicAsScratchRequestedBytes,
			(unsigned long long)shell.dynamicAsMemoryBytes,
			shell.dynamicAsRuntimeSpaceLinkPrimitives,
			(unsigned long long)shell.dynamicAsRuntimeSpaceLinkBytes,
			shell.dynamicAsRuntimeMutationPrimitives,
			(unsigned long long)shell.dynamicAsRuntimeMutationBytes,
			shell.dynamicAsDynamicPrimitives,
			(unsigned long long)shell.dynamicAsDynamicBytes,
			shell.dynamicAsLocalPlayerReflectionPrimitives,
			(unsigned long long)shell.dynamicAsLocalPlayerReflectionBytes,
			shell.dynamicAsDebugSpherePrimitives,
			(unsigned long long)shell.dynamicAsDebugSphereBytes);
		const double overlayPathAccountedMs =
			shell.overlayAppendMs +
			shell.sceneSelectStaticInstancesMs +
			shell.sceneSelectMaterialBridgeMs +
			shell.sceneSelectPaletteMs +
			shell.sceneSelectTexturesMs +
			shell.sceneSelectMaterialSplitMs +
			shell.sceneSelectBufferUploadMs +
			shell.persistentVoxelAsMs +
			shell.dynamicAsMs +
			shell.sceneSelectInstanceHandlesMs +
			shell.worldTlasMs +
			shell.sceneDataSetMs +
			shell.sceneSelectTexturePrepMs +
			shell.sceneSelectStateCommitMs;
		const double overlayPathResidualMs = shell.overlayAssembleMs - overlayPathAccountedMs;
		Printf(
			"PERF pt overlay detail NRI: frame=%llu total=%.3f append=%.3f append_reset=%.3f append_sources=%.3f append_bookkeeping=%.3f spacelink=%.3f spacelink_geo=%.3f spacelink_mat=%.3f spacelink_prims=%u spacelink_mats=%u mutation=%.3f mutation_geo=%.3f mutation_mat=%.3f mutation_prims=%u mutation_mats=%u dynamic=%.3f dynamic_geo=%.3f dynamic_mat=%.3f dynamic_prims=%u dynamic_mats=%u local_player_reflection=%.3f local_player_reflection_geo=%.3f local_player_reflection_mat=%.3f local_player_reflection_prims=%u local_player_reflection_mats=%u debug_sphere=%.3f debug_sphere_geo=%.3f debug_sphere_mat=%.3f debug_sphere_prims=%u debug_sphere_mats=%u persistent_voxel_actors=%u persistent_voxel_prims=%u persistent_voxel_mats=%u overlay_prims=%u overlay_mats=%u\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.overlayAssembleMs,
			shell.overlayAppendMs,
			shell.overlayAppendResetMs,
			shell.overlayAppendSourcesMs,
			shell.overlayAppendBookkeepingMs,
			shell.overlayRuntimeSpaceLinkMs,
			shell.overlayRuntimeSpaceLinkGeometryMs,
			shell.overlayRuntimeSpaceLinkMaterialMs,
			shell.overlayRuntimeSpaceLinkPrimitiveCount,
			shell.overlayRuntimeSpaceLinkMaterialCount,
			shell.overlayRuntimeMutationMs,
			shell.overlayRuntimeMutationGeometryMs,
			shell.overlayRuntimeMutationMaterialMs,
			shell.overlayRuntimeMutationPrimitiveCount,
			shell.overlayRuntimeMutationMaterialCount,
			shell.overlayDynamicMs,
			shell.overlayDynamicGeometryMs,
			shell.overlayDynamicMaterialMs,
			shell.overlayDynamicPrimitiveCount,
			shell.overlayDynamicMaterialCount,
			shell.overlayLocalPlayerReflectionMs,
			shell.overlayLocalPlayerReflectionGeometryMs,
			shell.overlayLocalPlayerReflectionMaterialMs,
			shell.overlayLocalPlayerReflectionPrimitiveCount,
			shell.overlayLocalPlayerReflectionMaterialCount,
			shell.overlayDebugSphereMs,
			shell.overlayDebugSphereGeometryMs,
			shell.overlayDebugSphereMaterialMs,
			shell.overlayDebugSpherePrimitiveCount,
			shell.overlayDebugSphereMaterialCount,
			shell.overlayPersistentVoxelActorCount,
			shell.overlayPersistentVoxelPrimitiveCount,
			shell.overlayPersistentVoxelMaterialCount,
			shell.overlayPrimitiveCount,
			shell.overlayMaterialCount);
		Printf(
			"PERF pt overlay append stamp NRI: frame=%llu total=%.3f dynamic=%.3f local_player_reflection=%.3f dynamic_prims=%u local_player_reflection_prims=%u\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.overlayAppendProducerStampMs,
			shell.overlayAppendDynamicStampMs,
			shell.overlayAppendLocalPlayerReflectionStampMs,
			shell.overlayDynamicAppend.primitiveCount,
			shell.overlayLocalPlayerReflectionAppend.primitiveCount);
		const auto& spacelinkAppend = shell.overlayRuntimeSpaceLinkAppend;
		const auto& mutationAppend = shell.overlayRuntimeMutationAppend;
		const auto& dynamicAppend = shell.overlayDynamicAppend;
		const auto& localPlayerReflectionAppend = shell.overlayLocalPlayerReflectionAppend;
		const auto& debugSphereAppend = shell.overlayDebugSphereAppend;
		const auto& persistentVoxelAppend = shell.overlayPersistentVoxelAppend;
		Printf(
			"PERF pt overlay append sources NRI: frame=%llu spacelink_ms=%.3f spacelink_geo_ms=%.3f spacelink_mat_ms=%.3f spacelink_vertices=%u spacelink_indices=%u spacelink_prims=%u spacelink_mats=%u spacelink_bytes=%llu mutation_ms=%.3f mutation_geo_ms=%.3f mutation_mat_ms=%.3f mutation_vertices=%u mutation_indices=%u mutation_prims=%u mutation_mats=%u mutation_bytes=%llu dynamic_ms=%.3f dynamic_geo_ms=%.3f dynamic_mat_ms=%.3f dynamic_vertices=%u dynamic_indices=%u dynamic_prims=%u dynamic_mats=%u dynamic_bytes=%llu local_player_reflection_ms=%.3f local_player_reflection_geo_ms=%.3f local_player_reflection_mat_ms=%.3f local_player_reflection_vertices=%u local_player_reflection_indices=%u local_player_reflection_prims=%u local_player_reflection_mats=%u local_player_reflection_bytes=%llu debug_sphere_ms=%.3f debug_sphere_geo_ms=%.3f debug_sphere_mat_ms=%.3f debug_sphere_vertices=%u debug_sphere_indices=%u debug_sphere_prims=%u debug_sphere_mats=%u debug_sphere_bytes=%llu persistent_voxel_actors=%u persistent_voxel_vertices=%u persistent_voxel_indices=%u persistent_voxel_prims=%u persistent_voxel_mats=%u persistent_voxel_bytes=%llu overlay_prims=%u overlay_mats=%u\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.overlayRuntimeSpaceLinkMs,
			shell.overlayRuntimeSpaceLinkGeometryMs,
			shell.overlayRuntimeSpaceLinkMaterialMs,
			spacelinkAppend.vertexCount,
			spacelinkAppend.indexCount,
			spacelinkAppend.primitiveCount,
			spacelinkAppend.materialCount,
			(unsigned long long)spacelinkAppend.byteCount,
			shell.overlayRuntimeMutationMs,
			shell.overlayRuntimeMutationGeometryMs,
			shell.overlayRuntimeMutationMaterialMs,
			mutationAppend.vertexCount,
			mutationAppend.indexCount,
			mutationAppend.primitiveCount,
			mutationAppend.materialCount,
			(unsigned long long)mutationAppend.byteCount,
			shell.overlayDynamicMs,
			shell.overlayDynamicGeometryMs,
			shell.overlayDynamicMaterialMs,
			dynamicAppend.vertexCount,
			dynamicAppend.indexCount,
			dynamicAppend.primitiveCount,
			dynamicAppend.materialCount,
			(unsigned long long)dynamicAppend.byteCount,
			shell.overlayLocalPlayerReflectionMs,
			shell.overlayLocalPlayerReflectionGeometryMs,
			shell.overlayLocalPlayerReflectionMaterialMs,
			localPlayerReflectionAppend.vertexCount,
			localPlayerReflectionAppend.indexCount,
			localPlayerReflectionAppend.primitiveCount,
			localPlayerReflectionAppend.materialCount,
			(unsigned long long)localPlayerReflectionAppend.byteCount,
			shell.overlayDebugSphereMs,
			shell.overlayDebugSphereGeometryMs,
			shell.overlayDebugSphereMaterialMs,
			debugSphereAppend.vertexCount,
			debugSphereAppend.indexCount,
			debugSphereAppend.primitiveCount,
			debugSphereAppend.materialCount,
			(unsigned long long)debugSphereAppend.byteCount,
			shell.overlayPersistentVoxelActorCount,
			persistentVoxelAppend.vertexCount,
			persistentVoxelAppend.indexCount,
			persistentVoxelAppend.primitiveCount,
			persistentVoxelAppend.materialCount,
			(unsigned long long)persistentVoxelAppend.byteCount,
			shell.overlayPrimitiveCount,
			shell.overlayMaterialCount);
		Printf(
			"PERF pt overlay path NRI: frame=%llu total=%.3f accounted=%.3f residual=%.3f append=%.3f static_instances=%.3f material_bridge=%.3f palette=%.3f textures=%.3f material_split=%.3f buffer_upload=%.3f persistent_voxel_as=%.3f dynamic_as=%.3f instance_handles=%.3f persistent_voxel_tlas=%.3f world_tlas=%.3f scene_data=%.3f texture_prep=%.3f state_commit=%.3f overlay_prims=%u overlay_mats=%u persistent_voxel_actors=%u persistent_voxel_prims=%u\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.overlayAssembleMs,
			overlayPathAccountedMs,
			overlayPathResidualMs,
			shell.overlayAppendMs,
			shell.sceneSelectStaticInstancesMs,
			shell.sceneSelectMaterialBridgeMs,
			shell.sceneSelectPaletteMs,
			shell.sceneSelectTexturesMs,
			shell.sceneSelectMaterialSplitMs,
			shell.sceneSelectBufferUploadMs,
			shell.persistentVoxelAsMs,
			shell.dynamicAsMs,
			shell.sceneSelectInstanceHandlesMs,
			shell.persistentVoxelTlasInstanceMs,
			shell.worldTlasMs,
			shell.sceneDataSetMs,
			shell.sceneSelectTexturePrepMs,
			shell.sceneSelectStateCommitMs,
			shell.overlayPrimitiveCount,
			shell.overlayMaterialCount,
			shell.overlayPersistentVoxelActorCount,
			shell.overlayPersistentVoxelPrimitiveCount);
		Printf(
			"PERF pt scene select detail NRI: frame=%llu persistent_voxel_as=%.3f persistent_voxel_as_calls=%u persistent_voxel_as_builds=%u persistent_voxel_as_instances=%u persistent_voxel_meshes=%u persistent_voxel_tlas_instances=%u persistent_voxel_transform_updates=%u persistent_voxel_baked_fallback=%u world_tlas=%.3f world_tlas_calls=%u world_tlas_instances=%u scene_data=%.3f scene_data_calls=%u\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.persistentVoxelAsMs,
			shell.persistentVoxelAsCalls,
			shell.persistentVoxelAsBuilds,
			shell.persistentVoxelAsInstances,
			shell.persistentVoxelSharedMeshResources,
			shell.persistentVoxelTlasInstances,
			shell.persistentVoxelInstanceTransformUpdates,
			shell.persistentVoxelBakedFallbackInstances,
			shell.worldTlasMs,
			shell.worldTlasBuildCalls,
			shell.worldTlasInstanceCount,
			shell.sceneDataSetMs,
			shell.sceneDataSetCalls);
		Printf(
			"PERF pt scene select phases NRI: frame=%llu static_map=%.3f persistent_batch=%.3f persistent_emissive=%.3f dynamic_merge=%.3f dynamic_merge_copy=%.3f dynamic_merge_append=%.3f dynamic_merge_stats=%.3f dynamic_merge_geo=%.3f dynamic_merge_portal=%.3f dynamic_merge_material=%.3f light_merge=%.3f static_instances=%.3f material_bridge=%.3f palette=%.3f textures=%.3f material_split=%.3f buffer_upload=%.3f instance_handles=%.3f texture_prep=%.3f state_commit=%.3f\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.sceneSelectStaticMapMs,
			shell.sceneSelectPersistentVoxelBatchMs,
			shell.sceneSelectPersistentEmissiveMs,
			shell.sceneSelectDynamicMergeMs,
			shell.sceneSelectDynamicMergeCopyMs,
			shell.sceneSelectDynamicMergeAppendMs,
			shell.sceneSelectDynamicMergeStatsMs,
			shell.sceneSelectDynamicMergeGeometryMs,
			shell.sceneSelectDynamicMergePortalAssignMs,
			shell.sceneSelectDynamicMergeMaterialMs,
			shell.sceneSelectLightMergeMs,
			shell.sceneSelectStaticInstancesMs,
			shell.sceneSelectMaterialBridgeMs,
			shell.sceneSelectPaletteMs,
			shell.sceneSelectTexturesMs,
			shell.sceneSelectMaterialSplitMs,
			shell.sceneSelectBufferUploadMs,
			shell.sceneSelectInstanceHandlesMs,
			shell.sceneSelectTexturePrepMs,
			shell.sceneSelectStateCommitMs);
		Printf(
			"PERF pt material residency NRI: frame=%llu resident_rebuilds=%u resident_hits=%u static_rows_copied=%u persistent_rows_appended=%u overlay_rows_appended=%u resident_rows_reused=%u\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.sceneMaterialResidentRebuilds,
			shell.sceneMaterialResidentHits,
			shell.sceneMaterialStaticRowsCopied,
			shell.sceneMaterialPersistentRowsAppended,
			shell.sceneMaterialOverlayRowsAppended,
			shell.sceneMaterialResidentRowsReused);
		Printf(
			"PERF pt scene select accounting NRI: frame=%llu select=%.3f accounted=%.3f unaccounted=%.3f static_map=%.3f spacelink=%.3f mutation=%.3f dynamic_capture=%.3f dynamic_geo=%.3f local_player_reflection_geo=%.3f persistent_batch=%.3f persistent_emissive=%.3f persistent_dynamic=%.3f dynamic_merge=%.3f dynamic_merge_copy=%.3f dynamic_merge_append=%.3f dynamic_merge_stats=%.3f dynamic_merge_geo=%.3f dynamic_merge_portal=%.3f dynamic_merge_material=%.3f dynamic_merge_live_surfaces=%u dynamic_merge_cache_surfaces=%u dynamic_merge_appended_surfaces=%u dynamic_merge_duplicate_surfaces=%u dynamic_merge_cache_prims=%u dynamic_merge_cache_mats=%u dynamic_merge_delta_attempts=%u dynamic_merge_delta_used=%u dynamic_merge_delta_fallbacks=%u dynamic_merge_delta_fallback_nonzero=%u light_merge=%.3f debug_sphere=%.3f overlay=%.3f static_instances=%.3f material_bridge=%.3f palette=%.3f textures=%.3f material_split=%.3f buffer_upload=%.3f persistent_voxel_as=%.3f dynamic_as=%.3f instance_handles=%.3f world_tlas=%.3f scene_data=%.3f texture_prep=%.3f state_commit=%.3f\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.sceneSelectMs,
			sceneSelectAccountedMs,
			sceneSelectUnaccountedMs,
			shell.sceneSelectStaticMapMs,
			shell.runtimeSpaceLinkMs,
			shell.runtimeMutationMs,
			shell.dynamicCaptureMs,
			shell.geometryBuildDynamicLiveMs,
			shell.geometryBuildLocalPlayerReflectionMs,
			shell.sceneSelectPersistentVoxelBatchMs,
			shell.sceneSelectPersistentEmissiveMs,
			shell.persistentDynamicMs,
			shell.sceneSelectDynamicMergeMs,
			shell.sceneSelectDynamicMergeCopyMs,
			shell.sceneSelectDynamicMergeAppendMs,
			shell.sceneSelectDynamicMergeStatsMs,
			shell.sceneSelectDynamicMergeGeometryMs,
			shell.sceneSelectDynamicMergePortalAssignMs,
			shell.sceneSelectDynamicMergeMaterialMs,
			shell.dynamicMergeLiveSurfaceCount,
			shell.dynamicMergePersistentCacheSurfaceCount,
			shell.dynamicMergeAppendedPersistentSurfaceCount,
			shell.dynamicMergeDuplicatePersistentSurfaceCount,
			shell.dynamicMergeAppendedPersistentPrimitiveCount,
			shell.dynamicMergeAppendedPersistentMaterialCount,
			shell.dynamicMergeDeltaAppendAttempts,
			shell.dynamicMergeDeltaAppendUsed,
			shell.dynamicMergeDeltaAppendFallbacks,
			shell.dynamicMergeDeltaAppendFallbackNonZeroSurfaces,
			shell.sceneSelectLightMergeMs,
			shell.runtimeDebugSphereMs,
			shell.overlayAppendMs,
			shell.sceneSelectStaticInstancesMs,
			shell.sceneSelectMaterialBridgeMs,
			shell.sceneSelectPaletteMs,
			shell.sceneSelectTexturesMs,
			shell.sceneSelectMaterialSplitMs,
			shell.sceneSelectBufferUploadMs,
			shell.persistentVoxelAsMs,
			shell.dynamicAsMs,
			shell.sceneSelectInstanceHandlesMs,
			shell.worldTlasMs,
			shell.sceneDataSetMs,
			shell.sceneSelectTexturePrepMs,
			shell.sceneSelectStateCommitMs);
		Printf(
			"PERF pt scene buffer upload detail NRI: frame=%llu total=%.3f primitive_rewrite=%.3f rewrite_primitive_hash=%.3f rewrite_provenance_hash=%.3f rewrite_visibility_hash=%.3f rewrite_copy=%.3f rewrite_resolve=%.3f rewrite_store=%.3f rewrite_resolve_prims=%u rewrite_resolve_mapchunk=%u rewrite_resolve_sector=%u rewrite_resolve_sector_miss=%u rewrite_cache_checks=%u rewrite_cache_hits=%u rewrite_cache_misses=%u rewrite_cache_invalid=%u rewrite_cache_primitive=%u rewrite_cache_provenance=%u rewrite_cache_visibility=%u rewrite_cache_count=%u payload_hash=%.3f hash_checks=%u hash_hits=%u hash_skips=%u hash_misses=%u hash_uploads=%u hash_reject_missing=%u hash_reject_size=%u hash_reject_stride=%u hash_reject_forced=%u vertex_hash_hits=%u index_hash_hits=%u primitive_hash_hits=%u material_hash_hits=%u vertex_hash_skips=%u index_hash_skips=%u primitive_hash_skips=%u material_hash_skips=%u vertex_hash_misses=%u index_hash_misses=%u primitive_hash_misses=%u material_hash_misses=%u producer_stamp_checks=%u producer_stamp_uses=%u producer_stamp_fallbacks=%u producer_stamp_rewrite_primitive=%u producer_stamp_rewrite_provenance=%u producer_stamp_vertex=%u producer_stamp_index=%u producer_stamp_primitive=%u producer_stamp_material=%u wait_check=%.3f wait=%.3f wait_count=%u growth_events=%u growth_old=%llu growth_requested=%llu growth_allocated=%llu growth_headroom=%llu vertex_ms=%.3f vertex_requested=%llu vertex_uploaded=%llu vertex_grow=%u vertex_overwrite=%u index_ms=%.3f index_requested=%llu index_uploaded=%llu index_grow=%u index_overwrite=%u primitive_ms=%.3f primitive_requested=%llu primitive_uploaded=%llu primitive_grow=%u primitive_overwrite=%u material_ms=%.3f material_requested=%llu material_uploaded=%llu material_grow=%u material_overwrite=%u persistent_voxel_material_ms=%.3f persistent_voxel_material_requested=%llu persistent_voxel_material_dirty=%llu persistent_voxel_material_uploaded=%llu persistent_voxel_material_uploads=%u persistent_voxel_material_batches=%u persistent_voxel_material_batch_ranges=%u persistent_voxel_material_batch_rejects=%u persistent_voxel_material_batch_copies=%u persistent_voxel_material_batch_barriers=%u persistent_voxel_material_batch_gap=%llu\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.sceneSelectBufferUploadMs,
			shell.sceneSelectBufferUploadPrimitiveRewriteMs,
			shell.sceneSelectBufferUploadPrimitiveRewritePrimitiveHashMs,
			shell.sceneSelectBufferUploadPrimitiveRewriteProvenanceHashMs,
			shell.sceneSelectBufferUploadPrimitiveRewriteVisibilityHashMs,
			shell.sceneSelectBufferUploadPrimitiveRewriteCopyMs,
			shell.sceneSelectBufferUploadPrimitiveRewriteResolveMs,
			shell.sceneSelectBufferUploadPrimitiveRewriteStoreMs,
			shell.sceneSelectBufferUploadPrimitiveRewriteResolvePrimitives,
			shell.sceneSelectBufferUploadPrimitiveRewriteResolveMapChunk,
			shell.sceneSelectBufferUploadPrimitiveRewriteResolveSectorFallback,
			shell.sceneSelectBufferUploadPrimitiveRewriteResolveSectorMiss,
			shell.sceneSelectBufferUploadPrimitiveRewriteCacheChecks,
			shell.sceneSelectBufferUploadPrimitiveRewriteCacheHits,
			shell.sceneSelectBufferUploadPrimitiveRewriteCacheMisses,
			shell.sceneSelectBufferUploadPrimitiveRewriteCacheRejectInvalid,
			shell.sceneSelectBufferUploadPrimitiveRewriteCacheRejectPrimitive,
			shell.sceneSelectBufferUploadPrimitiveRewriteCacheRejectProvenance,
			shell.sceneSelectBufferUploadPrimitiveRewriteCacheRejectVisibility,
			shell.sceneSelectBufferUploadPrimitiveRewriteCacheRejectCount,
			shell.sceneSelectBufferUploadPayloadHashMs,
			shell.sceneSelectBufferUploadPayloadHashChecks,
			shell.sceneSelectBufferUploadPayloadHashHits,
			shell.sceneSelectBufferUploadPayloadHashSkips,
			shell.sceneSelectBufferUploadPayloadHashMisses,
			shell.sceneSelectBufferUploadPayloadHashUploads,
			shell.sceneSelectBufferUploadPayloadHashRejectMissing,
			shell.sceneSelectBufferUploadPayloadHashRejectSize,
			shell.sceneSelectBufferUploadPayloadHashRejectStride,
			shell.sceneSelectBufferUploadPayloadHashRejectForced,
			shell.sceneSelectBufferUploadPayloadHashVertexHits,
			shell.sceneSelectBufferUploadPayloadHashIndexHits,
			shell.sceneSelectBufferUploadPayloadHashPrimitiveHits,
			shell.sceneSelectBufferUploadPayloadHashMaterialHits,
			shell.sceneSelectBufferUploadPayloadHashVertexSkips,
			shell.sceneSelectBufferUploadPayloadHashIndexSkips,
			shell.sceneSelectBufferUploadPayloadHashPrimitiveSkips,
			shell.sceneSelectBufferUploadPayloadHashMaterialSkips,
			shell.sceneSelectBufferUploadPayloadHashVertexMisses,
			shell.sceneSelectBufferUploadPayloadHashIndexMisses,
			shell.sceneSelectBufferUploadPayloadHashPrimitiveMisses,
			shell.sceneSelectBufferUploadPayloadHashMaterialMisses,
			shell.sceneSelectBufferUploadProducerStampChecks,
			shell.sceneSelectBufferUploadProducerStampUses,
			shell.sceneSelectBufferUploadProducerStampFallbacks,
			shell.sceneSelectBufferUploadProducerStampRewritePrimitiveUses,
			shell.sceneSelectBufferUploadProducerStampRewriteProvenanceUses,
			shell.sceneSelectBufferUploadProducerStampVertexUses,
			shell.sceneSelectBufferUploadProducerStampIndexUses,
			shell.sceneSelectBufferUploadProducerStampPrimitiveUses,
			shell.sceneSelectBufferUploadProducerStampMaterialUses,
			shell.sceneSelectBufferUploadWaitCheckMs,
			shell.sceneSelectBufferUploadWaitMs,
			shell.sceneSelectBufferUploadWaitCount,
			shell.sceneSelectBufferUploadGrowthEvents,
			(unsigned long long)shell.sceneSelectBufferUploadGrowthOldBytes,
			(unsigned long long)shell.sceneSelectBufferUploadGrowthRequestedBytes,
			(unsigned long long)shell.sceneSelectBufferUploadGrowthAllocatedBytes,
			(unsigned long long)shell.sceneSelectBufferUploadGrowthHeadroomBytes,
			shell.sceneSelectBufferUploadVertexMs,
			(unsigned long long)shell.sceneSelectBufferUploadVertexRequestedBytes,
			(unsigned long long)shell.sceneSelectBufferUploadVertexUploadedBytes,
			shell.sceneSelectBufferUploadVertexGrowEvents,
			shell.sceneSelectBufferUploadVertexOverwriteEvents,
			shell.sceneSelectBufferUploadIndexMs,
			(unsigned long long)shell.sceneSelectBufferUploadIndexRequestedBytes,
			(unsigned long long)shell.sceneSelectBufferUploadIndexUploadedBytes,
			shell.sceneSelectBufferUploadIndexGrowEvents,
			shell.sceneSelectBufferUploadIndexOverwriteEvents,
			shell.sceneSelectBufferUploadPrimitiveMs,
			(unsigned long long)shell.sceneSelectBufferUploadPrimitiveRequestedBytes,
			(unsigned long long)shell.sceneSelectBufferUploadPrimitiveUploadedBytes,
			shell.sceneSelectBufferUploadPrimitiveGrowEvents,
			shell.sceneSelectBufferUploadPrimitiveOverwriteEvents,
			shell.sceneSelectBufferUploadMaterialMs,
			(unsigned long long)shell.sceneSelectBufferUploadMaterialRequestedBytes,
			(unsigned long long)shell.sceneSelectBufferUploadMaterialUploadedBytes,
			shell.sceneSelectBufferUploadMaterialGrowEvents,
			shell.sceneSelectBufferUploadMaterialOverwriteEvents,
			shell.sceneSelectBufferUploadPersistentVoxelMaterialMs,
			(unsigned long long)shell.sceneSelectBufferUploadPersistentVoxelMaterialRequestedBytes,
			(unsigned long long)shell.sceneSelectBufferUploadPersistentVoxelMaterialDirtyBytes,
			(unsigned long long)shell.sceneSelectBufferUploadPersistentVoxelMaterialUploadedBytes,
			shell.sceneSelectBufferUploadPersistentVoxelMaterialUploads,
			shell.sceneSelectBufferUploadPersistentVoxelMaterialBatches,
			shell.sceneSelectBufferUploadPersistentVoxelMaterialBatchRanges,
			shell.sceneSelectBufferUploadPersistentVoxelMaterialBatchRejects,
			shell.sceneSelectBufferUploadPersistentVoxelMaterialBatchCopyCommands,
			shell.sceneSelectBufferUploadPersistentVoxelMaterialBatchBarrierCommands,
			(unsigned long long)shell.sceneSelectBufferUploadPersistentVoxelMaterialBatchGapBytes);
		Printf(
			"PERF pt scene buffer producer identity NRI: frame=%llu checks=%u uses=%u fallbacks=%u stamped_bytes=%llu fallback_bytes=%llu fallback_spans=%u coverage_rejects=%u validation_checks=%u validation_mismatches=%u visibility_cache_hits=%u visibility_cache_builds=%u visibility_validation_checks=%u visibility_validation_mismatches=%u\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.sceneSelectBufferUploadProducerStampChecks,
			shell.sceneSelectBufferUploadProducerStampUses,
			shell.sceneSelectBufferUploadProducerStampFallbacks,
			(unsigned long long)shell.sceneSelectBufferUploadProducerStampStampedBytes,
			(unsigned long long)shell.sceneSelectBufferUploadProducerStampFallbackBytes,
			shell.sceneSelectBufferUploadProducerStampFallbackSpans,
			shell.sceneSelectBufferUploadProducerStampCoverageRejects,
			shell.sceneSelectBufferUploadIdentityValidationChecks,
			shell.sceneSelectBufferUploadIdentityValidationMismatches,
			shell.sceneSelectBufferUploadVisibilityIdentityCacheHits,
			shell.sceneSelectBufferUploadVisibilityIdentityCacheBuilds,
			shell.sceneSelectBufferUploadVisibilityIdentityValidationChecks,
			shell.sceneSelectBufferUploadVisibilityIdentityValidationMismatches);
		Printf(
			"PERF pt scene buffer dirty range detail NRI: frame=%llu dirty_range=%.3f dirty_checks=%u dirty_skips=%u dirty_forced_full=%u dirty_missing_mirror=%u dirty_size_mismatch=%u dirty_source_full=%u dirty_source_byte_scan=%u dirty_source_typed=%u dirty_raw_ranges=%u dirty_ranges=%u dirty_changed=%llu dirty_uploaded=%llu dirty_gap=%llu dirty_reject_coalesce=%u range_uploads=%u range_upload_bytes=%llu range_fallbacks=%u range_fallback_fragmented=%u range_fallback_large=%u primitive_range_uploads=%u material_range_uploads=%u vertex_dirty_ranges=%u vertex_dirty_changed=%llu vertex_dirty_uploaded=%llu index_dirty_ranges=%u index_dirty_changed=%llu index_dirty_uploaded=%llu primitive_dirty_ranges=%u primitive_dirty_changed=%llu primitive_dirty_uploaded=%llu material_dirty_ranges=%u material_dirty_changed=%llu material_dirty_uploaded=%llu\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.sceneSelectBufferUploadDirtyRangeMs,
			shell.sceneSelectBufferUploadDirtyRangeChecks,
			shell.sceneSelectBufferUploadDirtyRangeSkips,
			shell.sceneSelectBufferUploadDirtyRangeForcedFull,
			shell.sceneSelectBufferUploadDirtyRangeMissingMirror,
			shell.sceneSelectBufferUploadDirtyRangeSizeMismatch,
			shell.sceneSelectBufferUploadDirtyRangeSourceFull,
			shell.sceneSelectBufferUploadDirtyRangeSourceByteScan,
			shell.sceneSelectBufferUploadDirtyRangeSourceTyped,
			shell.sceneSelectBufferUploadDirtyRangeRawRanges,
			shell.sceneSelectBufferUploadDirtyRangeCoalescedRanges,
			(unsigned long long)shell.sceneSelectBufferUploadDirtyRangeChangedBytes,
			(unsigned long long)shell.sceneSelectBufferUploadDirtyRangeUploadedBytes,
			(unsigned long long)shell.sceneSelectBufferUploadDirtyRangeGapBytes,
			shell.sceneSelectBufferUploadDirtyRangeRejectedCoalesces,
			shell.sceneSelectBufferUploadRangeUploads,
			(unsigned long long)shell.sceneSelectBufferUploadRangeUploadedBytes,
			shell.sceneSelectBufferUploadRangeFallbacks,
			shell.sceneSelectBufferUploadRangeFallbackFragmented,
			shell.sceneSelectBufferUploadRangeFallbackLarge,
			shell.sceneSelectBufferUploadPrimitiveRangeUploads,
			shell.sceneSelectBufferUploadMaterialRangeUploads,
			shell.sceneSelectBufferUploadVertexDirtyRanges,
			(unsigned long long)shell.sceneSelectBufferUploadVertexDirtyChangedBytes,
			(unsigned long long)shell.sceneSelectBufferUploadVertexDirtyUploadedBytes,
			shell.sceneSelectBufferUploadIndexDirtyRanges,
			(unsigned long long)shell.sceneSelectBufferUploadIndexDirtyChangedBytes,
			(unsigned long long)shell.sceneSelectBufferUploadIndexDirtyUploadedBytes,
			shell.sceneSelectBufferUploadPrimitiveDirtyRanges,
			(unsigned long long)shell.sceneSelectBufferUploadPrimitiveDirtyChangedBytes,
			(unsigned long long)shell.sceneSelectBufferUploadPrimitiveDirtyUploadedBytes,
			shell.sceneSelectBufferUploadMaterialDirtyRanges,
			(unsigned long long)shell.sceneSelectBufferUploadMaterialDirtyChangedBytes,
			(unsigned long long)shell.sceneSelectBufferUploadMaterialDirtyUploadedBytes);
		Printf(
			"PERF pt scene buffer typed upload NRI: frame=%llu typed_sources=%u byte_scan_sources=%u vertex_ranges=%u index_ranges=%u primitive_ranges=%u material_ranges=%u range_uploads=%u range_uploaded=%llu validation_mismatches=%u\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.sceneSelectBufferUploadDirtyRangeSourceTyped,
			shell.sceneSelectBufferUploadDirtyRangeSourceByteScan,
			shell.sceneSelectBufferUploadVertexRangeUploads,
			shell.sceneSelectBufferUploadIndexRangeUploads,
			shell.sceneSelectBufferUploadPrimitiveRangeUploads,
			shell.sceneSelectBufferUploadMaterialRangeUploads,
			shell.sceneSelectBufferUploadRangeUploads,
			(unsigned long long)shell.sceneSelectBufferUploadRangeUploadedBytes,
			shell.sceneSelectBufferUploadIdentityValidationMismatches);
		const auto getSceneBufferUploadDomainName =
			[](NRIRenderer::SceneBufferUploadDomain domain) -> const char*
		{
			switch (domain)
			{
			case NRIRenderer::SceneBufferUploadDomain::StaticOverlay: return "static_overlay";
			case NRIRenderer::SceneBufferUploadDomain::RuntimeSpaceLink: return "runtime_space_link";
			case NRIRenderer::SceneBufferUploadDomain::RuntimeMutation: return "runtime_mutation";
			case NRIRenderer::SceneBufferUploadDomain::Dynamic: return "dynamic";
			case NRIRenderer::SceneBufferUploadDomain::LocalPlayerReflection: return "local_player_reflection";
			case NRIRenderer::SceneBufferUploadDomain::RuntimeDebugSphere: return "runtime_debug_sphere";
			case NRIRenderer::SceneBufferUploadDomain::SurfaceLightOverlay: return "surface_light_overlay";
			case NRIRenderer::SceneBufferUploadDomain::PersistentVoxelMaterial: return "persistent_voxel_material";
			case NRIRenderer::SceneBufferUploadDomain::Count: break;
			}
			return "unknown";
		};
		for (size_t domainIndex = 0; domainIndex < NRIRenderer::SceneBufferUploadDomainCount; ++domainIndex)
		{
			const auto domain = (NRIRenderer::SceneBufferUploadDomain)domainIndex;
			const auto& entry = shell.sceneSelectBufferUploadDomains[domainIndex];
			if (entry.payloadBytes == 0 &&
				entry.uploadedBytes == 0 &&
				entry.dirtyUploadedBytes == 0 &&
				entry.growthEvents == 0 &&
				entry.hashChecks == 0 &&
				entry.stampChecks == 0 &&
				entry.waitMs <= 0.0)
			{
				continue;
			}
			Printf(
				"PERF pt scene buffer upload domain NRI: frame=%llu domain=%s payload_bytes=%llu vertex_payload=%llu index_payload=%llu primitive_payload=%llu material_payload=%llu hash_checks=%u hash_misses=%u stamp_checks=%u stamp_misses=%u growth_events=%u growth_requested=%llu growth_allocated=%llu dirty_ranges=%u dirty_changed=%llu dirty_uploaded=%llu uploaded=%llu primitive_uploaded=%llu material_uploaded=%llu wait_ms=%.3f\n",
				(unsigned long long)mLastFrameBoundaryStats.frameNumber,
				getSceneBufferUploadDomainName(domain),
				(unsigned long long)entry.payloadBytes,
				(unsigned long long)entry.vertexPayloadBytes,
				(unsigned long long)entry.indexPayloadBytes,
				(unsigned long long)entry.primitivePayloadBytes,
				(unsigned long long)entry.materialPayloadBytes,
				entry.hashChecks,
				entry.hashMisses,
				entry.stampChecks,
				entry.stampMisses,
				entry.growthEvents,
				(unsigned long long)entry.growthRequestedBytes,
				(unsigned long long)entry.growthAllocatedBytes,
				entry.dirtyRanges,
				(unsigned long long)entry.dirtyChangedBytes,
				(unsigned long long)entry.dirtyUploadedBytes,
				(unsigned long long)entry.uploadedBytes,
				(unsigned long long)entry.primitiveUploadedBytes,
				(unsigned long long)entry.materialUploadedBytes,
				entry.waitMs);
		}
		Printf(
			"PERF pt scene state detail NRI: frame=%llu scene_data=%.3f scene_data_calls=%u wait_check=%.3f wait=%.3f wait_count=%u wait0_reason=\"%s\" wait0_buffer=\"%s\" wait0_ms=%.3f wait1_reason=\"%s\" wait1_buffer=\"%s\" wait1_ms=%.3f reprojection=%.3f visible_flat=%.3f visible_chunk=%.3f scene_instance=%.3f scene_instance_requested=%llu scene_instance_uploaded=%llu portal=%.3f portal_requested=%llu portal_uploaded=%llu runtime_light_hash=%.3f runtime_light=%.3f runtime_light_uploads=%u runtime_light_hits=%u runtime_light_requested=%llu runtime_light_uploaded=%llu runtime_light_cluster=%.3f runtime_light_cluster_uploads=%u runtime_light_cluster_hits=%u runtime_light_cluster_requested=%llu runtime_light_cluster_uploaded=%llu emissive=%.3f emissive_uploads=%u emissive_hits=%u emissive_requested=%llu emissive_uploaded=%llu sector_light=%.3f sector_light_uploads=%u sector_light_hits=%u sector_light_requested=%llu sector_light_uploaded=%llu descriptor_build=%.3f descriptor_validate=%.3f descriptor_update=%.3f descriptor_hash=%.3f scene_data_slot=%u scene_data_slots=%u scene_data_slot_enabled=%u scene_data_slot_fallbacks=%u scene_data_slot_over_cap=%u scene_data_slot_waits=%u scene_data_slot_grows=%u scene_data_slot_used_bytes=%llu scene_data_slot_capacity_bytes=%llu scene_data_ring_capacity_bytes=%llu scene_data_ring_high_water_bytes=%llu descriptor_updates=%u descriptor_deferred=%u descriptor_nulls=%u resource_grow=%u resource_overwrite=%u snapshot_gen=%llu descriptor_gen=%llu scene_instance_count=%u tlas_instance_count=%u portal_count=%u scene_instance_hash=%llu tlas_instance_hash=%llu portal_hash=%llu snapshot_mismatches=%u state_commit=%.3f state_flags=%.3f state_dynamic=%.3f state_geometry=%.3f state_stats=%.3f\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.sceneDataSetMs,
			shell.sceneDataSetCalls,
			shell.sceneDataSetWaitCheckMs,
			shell.sceneDataSetWaitMs,
			shell.sceneDataSetWaitCount,
			shell.sceneDataSetWaitEventReason[0] != nullptr ? shell.sceneDataSetWaitEventReason[0] : "none",
			shell.sceneDataSetWaitEventBuffer[0] != nullptr ? shell.sceneDataSetWaitEventBuffer[0] : "none",
			shell.sceneDataSetWaitEventMs[0],
			shell.sceneDataSetWaitEventReason[1] != nullptr ? shell.sceneDataSetWaitEventReason[1] : "none",
			shell.sceneDataSetWaitEventBuffer[1] != nullptr ? shell.sceneDataSetWaitEventBuffer[1] : "none",
			shell.sceneDataSetWaitEventMs[1],
			shell.sceneDataSetReprojectionMs,
			shell.sceneDataSetVisibleFlatPlaneMs,
			shell.sceneDataSetVisibleChunkMs,
			shell.sceneDataSetSceneInstanceMs,
			(unsigned long long)shell.sceneDataSetSceneInstanceRequestedBytes,
			(unsigned long long)shell.sceneDataSetSceneInstanceUploadedBytes,
			shell.sceneDataSetPortalMs,
			(unsigned long long)shell.sceneDataSetPortalRequestedBytes,
			(unsigned long long)shell.sceneDataSetPortalUploadedBytes,
			shell.sceneDataSetRuntimeLightHashMs,
			shell.sceneDataSetRuntimeLightUploadMs,
			shell.sceneDataSetRuntimeLightUploads,
			shell.sceneDataSetRuntimeLightCacheHits,
			(unsigned long long)shell.sceneDataSetRuntimeLightRequestedBytes,
			(unsigned long long)shell.sceneDataSetRuntimeLightUploadedBytes,
			shell.sceneDataSetRuntimeLightClusterMs,
			shell.sceneDataSetRuntimeLightClusterUploads,
			shell.sceneDataSetRuntimeLightClusterCacheHits,
			(unsigned long long)shell.sceneDataSetRuntimeLightClusterRequestedBytes,
			(unsigned long long)shell.sceneDataSetRuntimeLightClusterUploadedBytes,
			shell.sceneDataSetEmissiveMs,
			shell.sceneDataSetEmissiveUploads,
			shell.sceneDataSetEmissiveCacheHits,
			(unsigned long long)shell.sceneDataSetEmissiveRequestedBytes,
			(unsigned long long)shell.sceneDataSetEmissiveUploadedBytes,
			shell.sceneDataSetSectorLightMs,
			shell.sceneDataSetSectorLightUploads,
			shell.sceneDataSetSectorLightCacheHits,
			(unsigned long long)shell.sceneDataSetSectorLightRequestedBytes,
			(unsigned long long)shell.sceneDataSetSectorLightUploadedBytes,
			shell.sceneDataSetDescriptorBuildMs,
			shell.sceneDataSetDescriptorValidateMs,
			shell.sceneDataSetDescriptorUpdateMs,
			shell.sceneDataSetDescriptorHashMs,
			shell.sceneDataSetFrameSlot,
			shell.sceneDataSetFrameSlotCount,
			shell.sceneDataSetFrameSlotEnabled,
			shell.sceneDataSetFrameSlotFallbacks,
			shell.sceneDataSetFrameSlotOverCap,
			shell.sceneDataSetFrameSlotWaits,
			shell.sceneDataSetFrameSlotGrows,
			(unsigned long long)shell.sceneDataSetFrameSlotUsedBytes,
			(unsigned long long)shell.sceneDataSetFrameSlotCapacityBytes,
			(unsigned long long)shell.sceneDataSetFrameRingCapacityBytes,
			(unsigned long long)shell.sceneDataSetFrameRingHighWaterBytes,
			shell.sceneDataSetDescriptorUpdateCount,
			shell.sceneDataSetDeferredDescriptorUpdateCount,
			shell.sceneDataSetDescriptorNullCount,
			shell.sceneDataSetResourceGrowEvents,
			shell.sceneDataSetResourceOverwriteEvents,
			(unsigned long long)shell.sceneDataSnapshotGeneration,
			(unsigned long long)shell.sceneDataDescriptorGeneration,
			shell.sceneDataSceneInstanceCount,
			shell.sceneDataTlasInstanceCount,
			shell.sceneDataPortalCount,
			(unsigned long long)shell.sceneDataSceneInstanceHash,
			(unsigned long long)shell.sceneDataTlasInstanceHash,
			(unsigned long long)shell.sceneDataPortalHash,
			shell.sceneDataSnapshotMismatchCount,
			shell.sceneSelectStateCommitMs,
			shell.sceneSelectStateCommitFlagsMs,
			shell.sceneSelectStateCommitDynamicStateMs,
			shell.sceneSelectStateCommitGeometryStateMs,
			shell.sceneSelectStateCommitStatsMs);
		const double stateCommitTopAccounted =
			shell.sceneSelectStateCommitFlagsMs +
			shell.sceneSelectStateCommitDynamicStateMs +
			shell.sceneSelectStateCommitGeometryStateMs +
			shell.sceneSelectStateCommitStatsMs;
		const double stateCommitItemizedAccounted =
			shell.sceneSelectStateCommitFlagsMs +
			shell.sceneSelectStateCommitDynamicCoreMs +
			shell.sceneSelectStateCommitDynamicLocalPlayerReflectionMs +
			shell.sceneSelectStateCommitGeometrySelectMs +
			shell.sceneSelectStateCommitGeometryStaticCopyMs +
			shell.sceneSelectStateCommitGeometryAppendMs +
			shell.sceneSelectStateCommitStatsBaseMs +
			shell.sceneSelectStateCommitStatsPersistentVoxelMs +
			shell.sceneSelectStateCommitStatsLocalPlayerReflectionMs +
			shell.sceneSelectStateCommitStatsMergeMs;
		Printf(
			"PERF pt state commit detail NRI: frame=%llu total=%.3f top_accounted=%.3f top_residual=%.3f itemized_accounted=%.3f itemized_residual=%.3f flags=%.3f dynamic=%.3f dynamic_core=%.3f dynamic_local_player_reflection=%.3f geometry=%.3f geometry_select=%.3f geometry_static_copy=%.3f geometry_append=%.3f stats=%.3f stats_base=%.3f stats_persistent_voxel=%.3f stats_local_player_reflection=%.3f stats_merge=%.3f selected_dynamic=%u active_dynamic=%u local_player_reflection=%u geometry_combined=%u geometry_static_only=%u stats_persistent_voxel_count=%u stats_local_player_reflection_count=%u combined_prims=%u combined_mats=%u\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.sceneSelectStateCommitMs,
			stateCommitTopAccounted,
			shell.sceneSelectStateCommitMs - stateCommitTopAccounted,
			stateCommitItemizedAccounted,
			shell.sceneSelectStateCommitMs - stateCommitItemizedAccounted,
			shell.sceneSelectStateCommitFlagsMs,
			shell.sceneSelectStateCommitDynamicStateMs,
			shell.sceneSelectStateCommitDynamicCoreMs,
			shell.sceneSelectStateCommitDynamicLocalPlayerReflectionMs,
			shell.sceneSelectStateCommitGeometryStateMs,
			shell.sceneSelectStateCommitGeometrySelectMs,
			shell.sceneSelectStateCommitGeometryStaticCopyMs,
			shell.sceneSelectStateCommitGeometryAppendMs,
			shell.sceneSelectStateCommitStatsMs,
			shell.sceneSelectStateCommitStatsBaseMs,
			shell.sceneSelectStateCommitStatsPersistentVoxelMs,
			shell.sceneSelectStateCommitStatsLocalPlayerReflectionMs,
			shell.sceneSelectStateCommitStatsMergeMs,
			shell.sceneSelectStateCommitSelectedDynamic,
			shell.sceneSelectStateCommitActiveDynamic,
			shell.sceneSelectStateCommitLocalPlayerReflection,
			shell.sceneSelectStateCommitGeometryCombined,
			shell.sceneSelectStateCommitGeometryStaticOnly,
			shell.sceneSelectStateCommitStatsPersistentVoxel,
			shell.sceneSelectStateCommitStatsLocalPlayerReflection,
			shell.sceneSelectStateCommitCombinedPrimitiveCount,
			shell.sceneSelectStateCommitCombinedMaterialCount);
		Printf(
			"PERF pt state commit generations NRI: frame=%llu changed_domains=%u static_map_gen=%llu static_map_changed=%u runtime_mutation_gen=%llu runtime_mutation_changed=%u dynamic_actors_gen=%llu dynamic_actors_changed=%u local_player_reflection_gen=%llu local_player_reflection_changed=%u persistent_voxels_gen=%llu persistent_voxels_changed=%u material_bridge_gen=%llu material_bridge_changed=%u textures_gen=%llu textures_changed=%u tlas_instances_gen=%llu tlas_instances_changed=%u scene_constants_gen=%llu scene_constants_changed=%u\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.sceneSelectStateCommitChangedDomainCount,
			(unsigned long long)shell.sceneSelectStateCommitGenStaticMap,
			shell.sceneSelectStateCommitChangedStaticMap,
			(unsigned long long)shell.sceneSelectStateCommitGenRuntimeMutation,
			shell.sceneSelectStateCommitChangedRuntimeMutation,
			(unsigned long long)shell.sceneSelectStateCommitGenDynamicActors,
			shell.sceneSelectStateCommitChangedDynamicActors,
			(unsigned long long)shell.sceneSelectStateCommitGenLocalPlayerReflection,
			shell.sceneSelectStateCommitChangedLocalPlayerReflection,
			(unsigned long long)shell.sceneSelectStateCommitGenPersistentVoxels,
			shell.sceneSelectStateCommitChangedPersistentVoxels,
			(unsigned long long)shell.sceneSelectStateCommitGenMaterialBridge,
			shell.sceneSelectStateCommitChangedMaterialBridge,
			(unsigned long long)shell.sceneSelectStateCommitGenTextures,
			shell.sceneSelectStateCommitChangedTextures,
			(unsigned long long)shell.sceneSelectStateCommitGenTlasInstances,
			shell.sceneSelectStateCommitChangedTlasInstances,
			(unsigned long long)shell.sceneSelectStateCommitGenSceneConstants,
			shell.sceneSelectStateCommitChangedSceneConstants);
		Printf(
			"PERF pt scene reuse NRI: frame=%llu presentation_gen=%llu simulation_gen=%llu engine_gen=%llu map_gen=%llu ticks=%u zero_tick=%u main_view=%u drawmode=%d surface_light_called=%u surface_light_hit=%u surface_light_candidate_hit=%u surface_light_build=%u surface_light_reject=%u surface_light_validation_checked=%u surface_light_validation_mismatch=%u surface_light_key=%llu surface_light_ms=%.3f texture_called=%u texture_hit=%u texture_candidate_hit=%u texture_build=%u texture_reject=%u texture_validation_checked=%u texture_validation_mismatch=%u texture_dynamic=%u texture_miss_mask=%u texture_key=%llu texture_ms=%.3f\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			(unsigned long long)shell.sceneReusePresentationGeneration,
			(unsigned long long)shell.sceneReuseSimulationGeneration,
			(unsigned long long)shell.sceneReuseEngineGeneration,
			(unsigned long long)shell.sceneReuseMapBuildSerial,
			shell.sceneReuseTicksExecuted,
			shell.sceneReuseZeroTickCandidate,
			drawmode == DM_MAINVIEW ? 1u : 0u,
			drawmode,
			shell.sceneReuseSurfaceLightCalled,
			shell.sceneReuseSurfaceLightHit,
			shell.sceneReuseSurfaceLightCandidateHit,
			shell.sceneReuseSurfaceLightBuild,
			shell.sceneReuseSurfaceLightReject,
			shell.sceneReuseSurfaceLightValidationChecked,
			shell.sceneReuseSurfaceLightValidationMismatch,
			(unsigned long long)shell.sceneReuseSurfaceLightKey,
			shell.sceneSelectSurfaceLightMs,
			shell.sceneReuseTextureCalled,
			shell.sceneReuseTextureHit,
			shell.sceneReuseTextureCandidateHit,
			shell.sceneReuseTextureBuild,
			shell.sceneReuseTextureReject,
			shell.sceneReuseTextureValidationChecked,
			shell.sceneReuseTextureValidationMismatch,
			shell.sceneReuseTextureDynamicCount,
			shell.sceneReuseTextureMissReasonMask,
			(unsigned long long)shell.sceneReuseTextureKey,
			shell.sceneSelectTexturesMs);
		Printf(
			"PERF pt dynamic capture detail NRI: frame=%llu calls=%u walls=%u flats=%u sprites=%u voxel_proxies=%u unsupported_models=%u voxel_stores=%u voxel_rebuilds=%u voxel_deferred=%u mesh_builds=%u mesh_deferred=%u mesh_hits=%u mesh_misses=%u mesh_invalid=%u duplication_audits=%u duplication_entries_scanned=%u duplication_temp_containers=%u duplication_ms=%.3f maintenance_calls=%u maintenance_skips=%u maintenance_legacy=%u maintenance_delta=%u maintenance_reason=0x%08x live_actors_enumerated=%u cache_entries_scanned=%u maintenance_removals=%u transform_syncs=%u lifecycle_events_applied=%u lifecycle_events_discarded=%u lifecycle_inserts=%u lifecycle_removes=%u lifecycle_stats=%u lifecycle_resets=%u lifecycle_overflows=%u lifecycle_removal_marked=%u lifecycle_ms=%.3f live_enumeration_ms=%.3f reconcile_ms=%.3f model_candidates=%u model_sorted=%u model_sort_skipped=%u scratch_reuses=%u scratch_grows=%u scratch_fallbacks=%u budget_truncations=%u surface_builds=%u count=%.3f wall=%.3f flat=%.3f facing=%.3f model=%.3f model_classify=%.3f model_mesh=%.3f model_mesh_build=%.3f model_sort=%.3f model_surface=%.3f model_store=%.3f voxel_frame=%.3f stats=%.3f\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.dynamicCaptureCalls,
			shell.dynamicCaptureWallSurfaces,
			shell.dynamicCaptureFlatSurfaces,
			shell.dynamicCaptureSpriteSurfaces,
			shell.dynamicCaptureVoxelProxySurfaces,
			shell.dynamicCaptureUnsupportedModelSurfaces,
			shell.dynamicCaptureVoxelCacheStores,
			shell.dynamicCaptureVoxelCacheRebuilds,
			shell.dynamicCaptureVoxelCacheDeferred,
			shell.dynamicCaptureVoxelMeshBuilds,
			shell.dynamicCaptureVoxelMeshDeferred,
			shell.dynamicCaptureVoxelMeshHits,
			shell.dynamicCaptureVoxelMeshMisses,
			shell.dynamicCaptureVoxelMeshInvalid,
			shell.dynamicCaptureVoxelDuplicationAuditCalls,
			shell.dynamicCaptureVoxelDuplicationAuditEntriesScanned,
			shell.dynamicCaptureVoxelDuplicationAuditTemporaryContainersBuilt,
			shell.dynamicCaptureVoxelDuplicationAuditMs,
			shell.dynamicCaptureVoxelMaintenanceCalls,
			shell.dynamicCaptureVoxelMaintenanceSimulationSkips,
			shell.dynamicCaptureVoxelMaintenanceLegacyReconciles,
			shell.dynamicCaptureVoxelMaintenanceDeltaReconciles,
			shell.dynamicCaptureVoxelMaintenanceReasonMask,
			shell.dynamicCaptureVoxelMaintenanceLiveActorsEnumerated,
			shell.dynamicCaptureVoxelMaintenanceCacheEntriesScanned,
			shell.dynamicCaptureVoxelMaintenanceRemovals,
			shell.dynamicCaptureVoxelMaintenanceTransformSyncs,
			shell.dynamicCaptureVoxelLifecycleEventsApplied,
			shell.dynamicCaptureVoxelLifecycleEventsDiscarded,
			shell.dynamicCaptureVoxelLifecycleInsertEvents,
			shell.dynamicCaptureVoxelLifecycleRemoveEvents,
			shell.dynamicCaptureVoxelLifecycleStatEvents,
			shell.dynamicCaptureVoxelLifecycleResetEvents,
			shell.dynamicCaptureVoxelLifecycleOverflows,
			shell.dynamicCaptureVoxelLifecycleRemovalEntriesMarked,
			shell.dynamicCaptureVoxelLifecycleMs,
			shell.dynamicCaptureVoxelLiveEnumerationMs,
			shell.dynamicCaptureVoxelReconcileMs,
			shell.dynamicCaptureModelActorCandidates,
			shell.dynamicCaptureModelActorSorted,
			shell.dynamicCaptureModelActorSortSkipped,
			shell.dynamicCaptureModelScratchReuses,
			shell.dynamicCaptureModelScratchGrows,
			shell.dynamicCaptureModelScratchFallbacks,
			shell.dynamicCaptureModelBudgetTruncations,
			shell.dynamicCaptureModelSurfaceBuilds,
			shell.dynamicCaptureCountMs,
			shell.dynamicCaptureWallsMs,
			shell.dynamicCaptureFlatsMs,
			shell.dynamicCaptureFacingSpritesMs,
			shell.dynamicCaptureModelSpritesMs,
			shell.dynamicCaptureModelClassifyMs,
			shell.dynamicCaptureModelMeshMs,
			shell.dynamicCaptureModelMeshBuildMs,
			shell.dynamicCaptureModelSortMs,
			shell.dynamicCaptureModelSurfaceMs,
			shell.dynamicCaptureModelStoreMs,
			shell.dynamicCaptureVoxelFrameMs,
			shell.dynamicCaptureStatsMs);
		if (nri_voxelstats)
		{
			auto getVoxelMeshBakeSpaceName = [](nri_scene::VoxelMeshBakeSpace bakeSpace) -> const char*
			{
				switch (bakeSpace)
				{
				case nri_scene::VoxelMeshBakeSpace::LocalSpace: return "local";
				case nri_scene::VoxelMeshBakeSpace::BakedTransform: return "baked";
				default: return "unknown";
				}
			};
			Printf(
				"PERF pt voxel reuse NRI: frame=%llu actor_entries=%u actor_surfaces=%u unique_mesh=%u unique_material=%u local_space=%u baked_transform=%u unknown_space=%u transform_keyed=%u unique_basis=%u invariant_warnings=%u actor_prims=%u dup_vertex_bytes=%llu dup_index_bytes=%llu dup_primitive_bytes=%llu dup_total_bytes=%llu mesh_builds=%u canonical_surface_builds=%u canonical_surface_hits=%u canonical_surface_invalid=%u surface_bakes=%u persistent_uploads=%u persistent_variant_uploads=%u blas_mesh_builds=%u blas_unique_mesh_builds=%u shared_mesh_resources=%u tlas_actor_instances=%u transform_only_updates=%u baked_fallback_instances=%u\n",
				(unsigned long long)mLastFrameBoundaryStats.frameNumber,
				shell.voxelCacheActorEntries,
				shell.voxelCacheActorSurfaces,
				shell.voxelCacheUniqueMeshKeys,
				shell.voxelCacheUniqueMaterialKeys,
				shell.voxelCacheLocalSpaceSurfaces,
				shell.voxelCacheBakedTransformSurfaces,
				shell.voxelCacheUnknownSpaceSurfaces,
				shell.voxelCacheTransformKeyedSurfaces,
				shell.voxelCacheUniqueTransformBases,
				shell.voxelCacheInvariantWarnings,
				shell.voxelCacheActorPrimitives,
				(unsigned long long)shell.voxelCacheDuplicatedVertexBytes,
				(unsigned long long)shell.voxelCacheDuplicatedIndexBytes,
				(unsigned long long)shell.voxelCacheDuplicatedPrimitiveBytes,
				(unsigned long long)shell.voxelCacheDuplicatedTotalBytes,
				shell.dynamicCaptureVoxelMeshBuilds,
				shell.dynamicCaptureVoxelCanonicalSurfaceBuilds,
				shell.dynamicCaptureVoxelCanonicalSurfaceHits,
				shell.dynamicCaptureVoxelCanonicalSurfaceInvalid,
				shell.dynamicCaptureVoxelCacheStores + shell.dynamicCaptureVoxelCacheRebuilds,
				shell.persistentVoxelOnboardingAdmittedCount,
				resource.scenePersistentVoxelVariantUploadCalls,
				shell.persistentVoxelAsBuilds,
				shell.persistentVoxelAsUniqueMeshBuilds,
				shell.persistentVoxelSharedMeshResources,
				shell.persistentVoxelTlasInstances,
				shell.persistentVoxelInstanceTransformUpdates,
				shell.persistentVoxelBakedFallbackInstances);
			for (uint32_t duplicateIndex = 0; duplicateIndex < shell.voxelCacheDuplicateTopCount; ++duplicateIndex)
			{
				const auto& duplicate = shell.voxelCacheDuplicateTopEntries[duplicateIndex];
				if (!duplicate.valid)
				{
					continue;
				}
				Printf(
					"PERF pt voxel duplicate top NRI: frame=%llu rank=%u mesh_key=0x%llx source_pic=%d actors=%u persistent_actors=%u space=%s transform_keyed=%u basis_count=%u example_basis=0x%llx prims_per_actor=%u total_prims=%u dup_bytes=%llu\n",
					(unsigned long long)mLastFrameBoundaryStats.frameNumber,
					duplicateIndex + 1u,
					(unsigned long long)duplicate.meshKeyHash,
					duplicate.sourcePicnum,
					duplicate.actorCount,
					duplicate.persistentActorCount,
					getVoxelMeshBakeSpaceName(duplicate.bakeSpace),
					duplicate.transformKeyedActorCount,
					duplicate.uniqueBasisSignatureCount,
					(unsigned long long)duplicate.exampleBasisSignature,
					duplicate.primitiveCountPerActor,
					duplicate.totalDuplicatedPrimitives,
					(unsigned long long)duplicate.duplicatedBytes);
			}
			Printf(
				"PERF pt voxel dynamic escapes NRI: frame=%llu actors=%u eligible=%u forced_dynamic=%u prims=%u vertex_bytes=%llu index_bytes=%llu primitive_bytes=%llu material_bytes=%llu total_bytes=%llu\n",
				(unsigned long long)mLastFrameBoundaryStats.frameNumber,
				shell.dynamicVoxelEscapeActorCount,
				shell.dynamicVoxelEscapeEligibleActorCount,
				shell.dynamicVoxelEscapeForcedActorCount,
				shell.dynamicVoxelEscapePrimitiveCount,
				(unsigned long long)shell.dynamicVoxelEscapeVertexBytes,
				(unsigned long long)shell.dynamicVoxelEscapeIndexBytes,
				(unsigned long long)shell.dynamicVoxelEscapePrimitiveBytes,
				(unsigned long long)shell.dynamicVoxelEscapeMaterialBytes,
				(unsigned long long)shell.dynamicVoxelEscapeTotalBytes);
			Printf(
				"PERF pt voxel dynamic classification NRI: frame=%llu expected_actors=%u unexpected_actors=%u expected_prims=%u unexpected_prims=%u expected_bytes=%llu unexpected_bytes=%llu\n",
				(unsigned long long)mLastFrameBoundaryStats.frameNumber,
				shell.dynamicVoxelExpectedEscapeActorCount,
				shell.dynamicVoxelUnexpectedEscapeActorCount,
				shell.dynamicVoxelExpectedEscapePrimitiveCount,
				shell.dynamicVoxelUnexpectedEscapePrimitiveCount,
				(unsigned long long)shell.dynamicVoxelExpectedEscapeTotalBytes,
				(unsigned long long)shell.dynamicVoxelUnexpectedEscapeTotalBytes);
			for (uint32_t escapeIndex = 0; escapeIndex < shell.dynamicVoxelEscapeTopCount; ++escapeIndex)
			{
				const auto& escape = shell.dynamicVoxelEscapeTopEntries[escapeIndex];
				if (!escape.valid)
				{
					continue;
				}
				Printf(
					"PERF pt voxel dynamic escape top NRI: frame=%llu rank=%u actor=%d stat=%d pic=%d voxel_index=%d reason=%s mesh_variant=0x%llx mat_variant=0x%llx prims=%u vertex_bytes=%llu index_bytes=%llu primitive_bytes=%llu material_bytes=%llu total_bytes=%llu persistent=%u has_surface=%u\n",
					(unsigned long long)mLastFrameBoundaryStats.frameNumber,
					escapeIndex + 1u,
					escape.actorIndex,
					escape.statnum,
					escape.sourcePicnum,
					escape.resolvedVoxelIndex,
					nri_scene::GetDynamicVoxelEscapeReasonName(escape.reason),
					(unsigned long long)escape.meshVariantHash,
					(unsigned long long)escape.materialVariantHash,
					escape.primitiveCount,
					(unsigned long long)escape.vertexBytes,
					(unsigned long long)escape.indexBytes,
					(unsigned long long)escape.primitiveBytes,
					(unsigned long long)escape.materialBytes,
					(unsigned long long)escape.totalBytes,
					escape.persistentReady ? 1u : 0u,
					escape.hasCachedSurface ? 1u : 0u);
			}
			for (uint32_t escapeIndex = 0; escapeIndex < shell.dynamicVoxelUnexpectedEscapeTopCount; ++escapeIndex)
			{
				const auto& escape = shell.dynamicVoxelUnexpectedEscapeTopEntries[escapeIndex];
				if (!escape.valid)
				{
					continue;
				}
				Printf(
					"PERF pt voxel dynamic unexpected top NRI: frame=%llu rank=%u actor=%d stat=%d pic=%d voxel_index=%d reason=%s mesh_variant=0x%llx mat_variant=0x%llx prims=%u vertex_bytes=%llu index_bytes=%llu primitive_bytes=%llu material_bytes=%llu total_bytes=%llu persistent=%u has_surface=%u\n",
					(unsigned long long)mLastFrameBoundaryStats.frameNumber,
					escapeIndex + 1u,
					escape.actorIndex,
					escape.statnum,
					escape.sourcePicnum,
					escape.resolvedVoxelIndex,
					nri_scene::GetDynamicVoxelEscapeReasonName(escape.reason),
					(unsigned long long)escape.meshVariantHash,
					(unsigned long long)escape.materialVariantHash,
					escape.primitiveCount,
					(unsigned long long)escape.vertexBytes,
					(unsigned long long)escape.indexBytes,
					(unsigned long long)escape.primitiveBytes,
					(unsigned long long)escape.materialBytes,
					(unsigned long long)escape.totalBytes,
					escape.persistentReady ? 1u : 0u,
					escape.hasCachedSurface ? 1u : 0u);
			}
		}
		Printf(
			"PERF pt geometry build detail NRI: frame=%llu dynamic_live=%.3f dynamic_live_prims=%u local_player_reflection=%.3f merged_dynamic=%.3f captured=%.3f persistent_voxel_variant=%.3f persistent_voxel_variant_calls=%u persistent_voxel_variant_prims=%u persistent_voxel_append=%.3f persistent_voxel_rebuild=%.3f persistent_emissive_prune=%.3f persistent_emissive_rebuild=%.3f static_chunk=%.3f static_chunk_calls=%u static_chunk_prims=%u debug_sphere=%.3f mutation_truth=%.3f mutation_truth_calls=%u mutation_rebuild=%.3f mutation_rebuild_calls=%u mutation_material_only=%.3f mutation_material_only_calls=%u mutation_prims=%u spacelink=%.3f spacelink_calls=%u spacelink_prims=%u resident_apply=%.3f resident_apply_calls=%u resident_recover=%.3f resident_recover_calls=%u resident_prims=%u\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.geometryBuildDynamicLiveMs,
			shell.geometryBuildDynamicLivePrimitives,
			shell.geometryBuildLocalPlayerReflectionMs,
			shell.geometryBuildMergedDynamicMs,
			shell.geometryBuildCapturedMs,
			shell.geometryBuildPersistentVoxelVariantMs,
			shell.geometryBuildPersistentVoxelVariantCalls,
			shell.geometryBuildPersistentVoxelVariantPrimitives,
			shell.geometryBuildPersistentVoxelAppendMs,
			shell.geometryBuildPersistentVoxelRebuildMs,
			shell.geometryBuildPersistentEmissivePruneMs,
			shell.geometryBuildPersistentEmissiveRebuildMs,
			shell.geometryBuildStaticChunkMs,
			shell.geometryBuildStaticChunkCalls,
			shell.geometryBuildStaticChunkPrimitives,
			shell.geometryBuildDebugSphereMs,
			shell.geometryBuildRuntimeMutationTruthMs,
			shell.geometryBuildRuntimeMutationTruthCalls,
			shell.geometryBuildRuntimeMutationRebuildMs,
			shell.geometryBuildRuntimeMutationRebuildCalls,
			shell.geometryBuildRuntimeMutationMaterialOnlyMs,
			shell.geometryBuildRuntimeMutationMaterialOnlyCalls,
			shell.geometryBuildRuntimeMutationPrimitives,
			shell.geometryBuildRuntimeSpaceLinkMs,
			shell.geometryBuildRuntimeSpaceLinkCalls,
			shell.geometryBuildRuntimeSpaceLinkPrimitives,
			shell.geometryBuildResidentApplyMs,
			shell.geometryBuildResidentApplyCalls,
			shell.geometryBuildResidentRecoverMs,
			shell.geometryBuildResidentRecoverCalls,
			shell.geometryBuildResidentPrimitives);
		Printf(
			"PERF pt persistent voxel onboarding NRI: frame=%llu candidates=%u admitted=%u deferred=%u actor_budget_hits=%u prim_budget_hits=%u byte_budget_hits=%u texture_budget_hits=%u estimated_bytes=%llu admitted_bytes=%llu deferred_bytes=%llu byte_budget=%llu\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.persistentVoxelOnboardingCandidateCount,
			shell.persistentVoxelOnboardingAdmittedCount,
			shell.persistentVoxelOnboardingDeferredCount,
			shell.persistentVoxelOnboardingActorBudgetHits,
			shell.persistentVoxelOnboardingPrimitiveBudgetHits,
			shell.persistentVoxelOnboardingByteBudgetHits,
			shell.persistentVoxelOnboardingTextureBudgetHits,
			(unsigned long long)shell.persistentVoxelOnboardingEstimatedBytes,
			(unsigned long long)shell.persistentVoxelOnboardingAdmittedBytes,
			(unsigned long long)shell.persistentVoxelOnboardingDeferredBytes,
			(unsigned long long)shell.persistentVoxelOnboardingByteBudget);
		Printf(
			"PERF pt persistent voxel batch detail NRI: frame=%llu admission_pump=%.3f cache_entries=%.3f sort=%.3f instance_sync=%.3f existing_actor_map=%.3f actor_loop=%.3f material_variant=%.3f mesh_admission=%.3f material_bridge=%.3f batch_state=%.3f admission_pending=%u texture_prewarm_deferred=%u material_invalid=%u budget_deferred=%u transform_updates=%u serial_fast_path=%u\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.sceneSelectPersistentVoxelAdmissionPumpMs,
			shell.persistentVoxelBatchCacheEntryMs,
			shell.persistentVoxelBatchSortMs,
			shell.persistentVoxelBatchInstanceSyncMs,
			shell.persistentVoxelBatchExistingActorMapMs,
			shell.persistentVoxelBatchActorLoopMs,
			shell.persistentVoxelBatchMaterialVariantMs,
			shell.persistentVoxelBatchMeshAdmissionMs,
			shell.persistentVoxelBatchMaterialBridgeMs,
			shell.persistentVoxelBatchStateMs,
			shell.persistentVoxelOnboardingAdmissionPendingCount,
			shell.persistentVoxelOnboardingTexturePrewarmDeferredCount,
			shell.persistentVoxelOnboardingMaterialInvalidCount,
			shell.persistentVoxelOnboardingBudgetDeferredCount,
			shell.persistentVoxelInstanceTransformUpdates,
			shell.persistentVoxelBatchSerialFastPathCount);
		Printf(
			"PERF pt persistent voxel texture prewarm NRI: frame=%llu queued=%u processed=%u deferred=%u hits=%u misses=%u estimated_bytes=%llu processed_bytes=%llu deferred_bytes=%llu byte_budget=%llu ms=%.3f\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.persistentVoxelTexturePrewarmQueuedCount,
			shell.persistentVoxelTexturePrewarmProcessedCount,
			shell.persistentVoxelTexturePrewarmDeferredCount,
			shell.persistentVoxelTexturePrewarmHitCount,
			shell.persistentVoxelTexturePrewarmMissCount,
			(unsigned long long)shell.persistentVoxelTexturePrewarmEstimatedBytes,
			(unsigned long long)shell.persistentVoxelTexturePrewarmProcessedBytes,
			(unsigned long long)shell.persistentVoxelTexturePrewarmDeferredBytes,
			(unsigned long long)shell.persistentVoxelTexturePrewarmByteBudget,
			shell.persistentVoxelTexturePrewarmMs);
		Printf(
			"PERF pt scene light detail NRI: frame=%llu records=%u static=%u mutation=%u captured=%u dynamic=%u persistent_voxel=%u append_static=%.3f append_mutation=%.3f append_captured=%.3f append_dynamic=%.3f append_persistent_voxel=%.3f rebuild_analytic=%.3f rebuild_emissive=%.3f rebuild_sector=%.3f sprite_rules=%u sprite_record_scans=%u actor_overlay_rules=%u actor_surface_lookups=%u actor_full_scans=%u actor_surface_scans=%u actor_indexed_candidates=%u topology_keys=%u topology_rebuild=%u property_only=%u topology_sort_skipped=%u topology_added=%u topology_removed=%u topology_rebound=%u ordered_key_hash=0x%016llx soft=%u surviving_index_changed=%u surviving_soft_index_changed=%u topology_sort=%.3f\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.sceneLightSurfaceRecordCount,
			shell.sceneLightStaticRecordCount,
			shell.sceneLightRuntimeMutationRecordCount,
			shell.sceneLightCapturedRecordCount,
			shell.sceneLightDynamicRecordCount,
			shell.sceneLightPersistentVoxelRecordCount,
			shell.sceneLightStaticAppendMs,
			shell.sceneLightRuntimeMutationAppendMs,
			shell.sceneLightCapturedAppendMs,
			shell.sceneLightDynamicAppendMs,
			shell.sceneLightPersistentVoxelAppendMs,
			shell.sceneLightAnalyticMs,
			shell.sceneLightEmissiveMs,
			shell.sceneLightSectorMs,
			shell.sceneLightSpriteTileRuleCount,
			shell.sceneLightSpriteRecordCandidateScans,
			shell.sceneLightActorOverlayRuleCount,
			shell.sceneLightActorOverlaySurfaceLookups,
			shell.sceneLightActorOverlayFullRecordScans,
			shell.sceneLightActorOverlaySurfaceCandidateScans,
			shell.sceneLightActorOverlayIndexedCandidateCount,
			shell.sceneLightTopologyKeyCount,
			shell.sceneLightTopologyRebuildCount,
			shell.sceneLightPropertyOnlyUpdateCount,
			shell.sceneLightTopologySortSkippedCount,
			shell.sceneLightTopologyAddedKeyCount,
			shell.sceneLightTopologyRemovedKeyCount,
			shell.sceneLightTopologyReboundKeyCount,
			(unsigned long long)shell.sceneLightOrderedStableKeyHash,
			shell.sceneLightSoftLightCount,
			shell.sceneLightSurvivingIndexChangeCount,
			shell.sceneLightSurvivingSoftIndexChangeCount,
			shell.sceneLightTopologySortMs);
		Printf(
			"PERF pt cache detail NRI: frame=%llu mutation_active=%u mutation_valid=%u mutation_excl_static=%u mutation_cached_surfaces=%u mutation_cached_tris=%u mutation_cached_mats=%u mutation_cached_states=%u mutation_mat_cache_hits=%u mutation_mat_cache_misses=%u mutation_mat_cache_stores=%u anim_slice_cached_states=%u anim_slice_cache_hits=%u anim_slice_cache_misses=%u anim_slice_cache_stores=%u anim_slice_apply_hits=%u anim_slice_apply_misses=%u anim_slice_sync_skip_hits=%u anim_gpu_cache_hits=%u anim_gpu_cache_misses=%u anim_gpu_cache_stores=%u persistent_actor=%u persistent_non_actor=%u persistent_walls=%u persistent_flats=%u persistent_sprites=%u persistent_highwater_surfaces=%u persistent_highwater_prims=%u persistent_highwater_mats=%u persistent_highwater_actor=%u persistent_highwater_sprites=%u\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.runtimeMutationActiveChunkCount,
			shell.runtimeMutationValidChunkCount,
			shell.runtimeMutationExcludedStaticChunkCount,
			shell.runtimeMutationCachedSurfaceCount,
			shell.runtimeMutationCachedTriangleCount,
			shell.runtimeMutationCachedMaterialCount,
			shell.runtimeMutationCachedMaterialStateCount,
			shell.runtimeMutationMaterialCacheHitCount,
			shell.runtimeMutationMaterialCacheMissCount,
			shell.runtimeMutationMaterialCacheStoreCount,
			shell.staticAnimatedResidentSliceCacheEntryCount,
			shell.staticAnimatedResidentSliceCacheHitCount,
			shell.staticAnimatedResidentSliceCacheMissCount,
			shell.staticAnimatedResidentSliceCacheStoreCount,
			shell.staticAnimatedResidentSliceApplyHitCount,
			shell.staticAnimatedResidentSliceApplyMissCount,
			shell.staticAnimatedResidentSliceSyncSkipHitCount,
			shell.staticAnimatedResidentGpuPayloadCacheHitCount,
			shell.staticAnimatedResidentGpuPayloadCacheMissCount,
			shell.staticAnimatedResidentGpuPayloadCacheStoreCount,
			shell.persistentDynamicActorSurfaceCount,
			shell.persistentDynamicNonActorSurfaceCount,
			shell.persistentDynamicWallSurfaceCount,
			shell.persistentDynamicFlatSurfaceCount,
			shell.persistentDynamicSpriteSurfaceCount,
			shell.persistentDynamicHighWaterSurfaceCount,
			shell.persistentDynamicHighWaterPrimitiveCount,
			shell.persistentDynamicHighWaterMaterialCount,
			shell.persistentDynamicHighWaterActorSurfaceCount,
			shell.persistentDynamicHighWaterSpriteSurfaceCount);
		Printf(
			"PERF pt mutation detail NRI: frame=%llu structural_ms=%.3f material_refresh_ms=%.3f structural=%u material_refresh=%u refresh_delta=%u refresh_delta_mask=0x%x refresh_hwcanvas=%u refresh_animated=%u struct_delta=%u struct_delta_mask=0x%x struct_view=%u struct_static_anim_flip=%u struct_excl_static_flip=%u struct_force_topology=%u struct_invalid=%u hwcanvas_chunks=%u\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.runtimeMutationStructuralRebuildMs,
			shell.runtimeMutationMaterialRefreshMs,
			shell.runtimeMutationStructuralRebuildChunks,
			shell.runtimeMutationMaterialRefreshChunks,
			shell.runtimeMutationMaterialRefreshReplacementDeltaChunks,
			shell.runtimeMutationMaterialRefreshReasonMaskOr,
			shell.runtimeMutationMaterialRefreshHardwareCanvasChunks,
			shell.runtimeMutationMaterialRefreshAnimatedChunks,
			shell.runtimeMutationStructuralReplacementDeltaChunks,
			shell.runtimeMutationStructuralReplacementDeltaReasonMaskOr,
			shell.runtimeMutationStructuralReplacementViewChangedChunks,
			shell.runtimeMutationStructuralStaticAnimatedModeFlipChunks,
			shell.runtimeMutationStructuralExcludeStaticFlipChunks,
			shell.runtimeMutationStructuralForcedTopologyChunks,
			shell.runtimeMutationStructuralInvalidChunks,
			shell.runtimeMutationHardwareCanvasChunkCount);
		Printf(
			"PERF pt mutation outcomes NRI: frame=%llu invalid_force_topology=%u force_topology_proofs=%u force_topology_downgrades=%u force_topology_downgrade_noop=%u force_topology_downgrade_material=%u force_topology_proof_prepare_failed=%u force_topology_proof_no_replacement=%u force_topology_proof_geometry_changed=%u force_topology_proof_unsafe_reason=%u force_topology_proof_invisible=%u force_topology_proof_reason_mismatch=%u invalid_applied=%u resident_noop_skips=%u invalid_failed=%u invalid_sync_skip=%u resident_noop_candidates=%u resident_noop_mask=0x%x noop_block_not_authoritative=%u noop_block_resident_unavailable=%u noop_block_replacement_invalid=%u noop_block_exclude_static=%u noop_block_surface_count=%u noop_block_material_count=%u noop_block_primitive_count=%u valid_structural=%u valid_material=%u\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.runtimeMutationInvalidForceTopologyCount,
			shell.runtimeMutationForceTopologyProofChecks,
			shell.runtimeMutationForceTopologyDowngradeCount,
			shell.runtimeMutationForceTopologyDowngradeNoopCount,
			shell.runtimeMutationForceTopologyDowngradeMaterialOnlyCount,
			shell.runtimeMutationForceTopologyDowngradePrepareFailedCount,
			shell.runtimeMutationForceTopologyDowngradeNoReplacementCount,
			shell.runtimeMutationForceTopologyDowngradeGeometryChangedCount,
			shell.runtimeMutationForceTopologyDowngradeUnsafeReasonCount,
			shell.runtimeMutationForceTopologyDowngradeInvisibleCount,
			shell.runtimeMutationForceTopologyDowngradeReasonMismatchCount,
			shell.runtimeMutationInvalidAppliedCount,
			shell.runtimeMutationResidentNoopSkipCount,
			shell.runtimeMutationInvalidFailedCount,
			shell.runtimeMutationInvalidSyncSkipCount,
			shell.runtimeMutationResidentNoopCandidateCount,
			shell.runtimeMutationResidentNoopCandidateReasonMaskOr,
			shell.runtimeMutationResidentNoopBlockNotAuthoritativeCount,
			shell.runtimeMutationResidentNoopBlockResidentUnavailableCount,
			shell.runtimeMutationResidentNoopBlockReplacementInvalidCount,
			shell.runtimeMutationResidentNoopBlockExcludeStaticCount,
			shell.runtimeMutationResidentNoopBlockSurfaceCountMismatch,
			shell.runtimeMutationResidentNoopBlockMaterialCountMismatch,
			shell.runtimeMutationResidentNoopBlockPrimitiveCountMismatch,
			shell.runtimeMutationValidStructuralCount,
			shell.runtimeMutationValidMaterialCount);
		Printf(
			"PERF pt resident apply detail NRI: frame=%llu total_ms=%.3f live_build_ms=%.3f geometry_ms=%.3f material_ms=%.3f baseline_ms=%.3f atlas_ms=%.3f atlas_bookkeeping_ms=%.3f vertex_index_copy_ms=%.3f vertex_cpu_ms=%.3f index_cpu_ms=%.3f vertex_stage_ms=%.3f index_stage_ms=%.3f primitive_rewrite_ms=%.3f primitive_cpu_ms=%.3f primitive_stage_ms=%.3f geometry_order_hash_ms=%.3f downstream_blas_ms=%.3f downstream_blas_setup_ms=%.3f downstream_blas_filter_ms=%.3f downstream_blas_create_ms=%.3f downstream_blas_scratch_ms=%.3f downstream_blas_barrier_ms=%.3f downstream_blas_build_ms=%.3f calls=%u material_only=%u structural=%u fast_material_only=%u certified_material_only=%u slow_material_only=%u material_only_exclusive=%u block_no_resident=%u block_replacement_invalid=%u block_material_count=%u material_hash_checks=%u material_hash_skips=%u material_hash_misses=%u material_hash_rejects=%u geometry_hash_checks=%u geometry_hash_skips=%u geometry_hash_misses=%u geometry_hash_rejects=%u geometry_hash_blas_skips=%u geometry_order_checks=%u geometry_order_equiv=%u geometry_order_misses=%u geometry_order_rejects=%u stage_vertex_ranges=%u stage_index_ranges=%u stage_primitive_ranges=%u stage_vertex_bytes=%llu stage_index_bytes=%llu stage_primitive_bytes=%llu stage_coalesced_ranges=%u stage_coalesced_bytes=%llu stage_coalesced_gap_bytes=%llu stage_coalesce_rejects=%u stage_map_ms=%.3f stage_memcpy_ms=%.3f stage_command_ms=%.3f stage_batches=%u stage_batch_ranges=%u stage_copy_cmds=%u stage_barrier_cmds=%u stage_scratch_grows=%u stage_scratch_grow_bytes=%llu preserve_geo=%u preserve_index=%u preserve_primitive=%u blas_reuse=%u blas_update=%u blas_refit_only=%u blas_recreate=%u blas_scratch_queries=%u blas_scratch_cache_hits=%u blas_scratch_cache_misses=%u blas_scratch_grows=%u blas_build_cmds=%u blas_scratch_barriers=%u keep_geo_slice=%u keep_mat_slice=%u empty_remove=%u recover_attempts=%u recover_success=%u atlas_grows=%u\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.runtimeMutationResidentApplyMs,
			shell.runtimeMutationResidentApplyLiveBuildMs,
			shell.runtimeMutationResidentApplyGeometryBuildMs,
			shell.runtimeMutationResidentApplyMaterialBuildMs,
			shell.runtimeMutationResidentApplyBaselineCaptureMs,
			shell.runtimeMutationResidentApplyAtlasMs,
			shell.runtimeMutationResidentApplyAtlasBookkeepingMs,
			shell.runtimeMutationResidentApplyVertexIndexCopyMs,
			shell.runtimeMutationResidentApplyVertexCpuCopyMs,
			shell.runtimeMutationResidentApplyIndexCpuCopyMs,
			shell.runtimeMutationResidentApplyVertexStageMs,
			shell.runtimeMutationResidentApplyIndexStageMs,
			shell.runtimeMutationResidentApplyPrimitiveRewriteMs,
			shell.runtimeMutationResidentApplyPrimitiveCpuRewriteMs,
			shell.runtimeMutationResidentApplyPrimitiveStageMs,
			shell.runtimeMutationResidentApplyGeometryOrderHashMs,
			shell.runtimeMutationResidentApplyDownstreamBlasMs,
			shell.runtimeMutationResidentApplyDownstreamBlasSetupMs,
			shell.runtimeMutationResidentApplyDownstreamBlasFilterMs,
			shell.runtimeMutationResidentApplyDownstreamBlasCreateMs,
			shell.runtimeMutationResidentApplyDownstreamBlasScratchMs,
			shell.runtimeMutationResidentApplyDownstreamBlasBarrierMs,
			shell.runtimeMutationResidentApplyDownstreamBlasBuildMs,
			shell.runtimeMutationResidentApplyCount,
			shell.runtimeMutationResidentApplyMaterialOnlyCount,
			shell.runtimeMutationResidentApplyStructuralCount,
			shell.runtimeMutationResidentApplyFastMaterialOnlyCount,
			shell.runtimeMutationResidentApplyCertifiedMaterialOnlyCount,
			shell.runtimeMutationResidentApplySlowMaterialOnlyCount,
			shell.runtimeMutationResidentApplyMaterialOnlyExclusiveCount,
			shell.runtimeMutationResidentApplyMaterialOnlyNoResidentChunkCount,
			shell.runtimeMutationResidentApplyMaterialOnlyInvalidReplacementCount,
			shell.runtimeMutationResidentApplyMaterialOnlyMaterialCountMismatchCount,
			shell.runtimeMutationResidentApplyMaterialPayloadHashCheckCount,
			shell.runtimeMutationResidentApplyMaterialPayloadHashSkipCount,
			shell.runtimeMutationResidentApplyMaterialPayloadHashMissCount,
			shell.runtimeMutationResidentApplyMaterialPayloadHashRejectCount,
			shell.runtimeMutationResidentApplyGeometryPayloadHashCheckCount,
			shell.runtimeMutationResidentApplyGeometryPayloadHashSkipCount,
			shell.runtimeMutationResidentApplyGeometryPayloadHashMissCount,
			shell.runtimeMutationResidentApplyGeometryPayloadHashRejectCount,
			shell.runtimeMutationResidentApplyGeometryPayloadHashBlasSkipCount,
			shell.runtimeMutationResidentApplyGeometryPayloadOrderCheckCount,
			shell.runtimeMutationResidentApplyGeometryPayloadOrderEquivalentCount,
			shell.runtimeMutationResidentApplyGeometryPayloadOrderMissCount,
			shell.runtimeMutationResidentApplyGeometryPayloadOrderRejectCount,
			shell.runtimeMutationResidentApplyVertexStageRangeCount,
			shell.runtimeMutationResidentApplyIndexStageRangeCount,
			shell.runtimeMutationResidentApplyPrimitiveStageRangeCount,
			(unsigned long long)shell.runtimeMutationResidentApplyVertexStageBytes,
			(unsigned long long)shell.runtimeMutationResidentApplyIndexStageBytes,
			(unsigned long long)shell.runtimeMutationResidentApplyPrimitiveStageBytes,
			shell.runtimeMutationResidentApplyCoalescedStageRangeCount,
			(unsigned long long)shell.runtimeMutationResidentApplyCoalescedStageBytes,
			(unsigned long long)shell.runtimeMutationResidentApplyCoalescedStageGapBytes,
			shell.runtimeMutationResidentApplyCoalescedStageRejectCount,
			shell.runtimeMutationResidentApplyStageMapMs,
			shell.runtimeMutationResidentApplyStageMemcpyMs,
			shell.runtimeMutationResidentApplyStageCommandMs,
			shell.runtimeMutationResidentApplyStageBatchCount,
			shell.runtimeMutationResidentApplyStageBatchRangeCount,
			shell.runtimeMutationResidentApplyStageCopyCommandCount,
			shell.runtimeMutationResidentApplyStageBarrierCommandCount,
			shell.runtimeMutationResidentApplyStageScratchGrowCount,
			(unsigned long long)shell.runtimeMutationResidentApplyStageScratchGrowBytes,
			shell.runtimeMutationResidentApplyPreserveGeometryCount,
			shell.runtimeMutationResidentApplyPreserveIndexCount,
			shell.runtimeMutationResidentApplyPreservePrimitiveCount,
			shell.runtimeMutationResidentApplyBlasReuseCount,
			shell.runtimeMutationResidentApplyBlasUpdateCount,
			shell.runtimeMutationResidentApplyBlasRefitOnlyCount,
			shell.runtimeMutationResidentApplyBlasRecreateCount,
			shell.runtimeMutationResidentApplyBlasScratchQueryCount,
			shell.runtimeMutationResidentApplyBlasScratchCacheHitCount,
			shell.runtimeMutationResidentApplyBlasScratchCacheMissCount,
			shell.runtimeMutationResidentApplyBlasScratchGrowCount,
			shell.runtimeMutationResidentApplyBlasBuildCommandCount,
			shell.runtimeMutationResidentApplyBlasScratchBarrierCount,
			shell.runtimeMutationResidentApplyKeepGeometrySliceCount,
			shell.runtimeMutationResidentApplyKeepMaterialSliceCount,
			shell.runtimeMutationResidentApplyEmptyRemovalCount,
			shell.runtimeMutationResidentApplyRecoverAttemptCount,
			shell.runtimeMutationResidentApplyRecoverSuccessCount,
			shell.runtimeMutationResidentApplyAtlasGrowCount);
		Printf(
			"PERF pt resident blas recreate summary NRI: frame=%llu recreates=%u no_prev_as=%u recovered_empty=%u slice_moved=%u topology_changed=%u force_topology=%u\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.runtimeMutationResidentApplyBlasRecreateCount,
			shell.runtimeMutationResidentApplyBlasRecreateNoPreviousAsCount,
			shell.runtimeMutationResidentApplyBlasRecreateRecoveredEmptyCount,
			shell.runtimeMutationResidentApplyBlasRecreateSliceMovedCount,
			shell.runtimeMutationResidentApplyBlasRecreateTopologyChangedCount,
			shell.runtimeMutationResidentApplyBlasRecreateForceTopologyCount);
		Printf(
			"PERF pt resident blas refit summary NRI: frame=%llu probes=%u hits=%u reject_no_prev_as=%u reject_index_count=%u reject_primitive_count=%u reject_zero_index=%u reject_zero_primitive=%u\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.runtimeMutationResidentApplyBlasRefitProbeCount,
			shell.runtimeMutationResidentApplyBlasRefitOnlyCount,
			shell.runtimeMutationResidentApplyBlasRefitRejectNoPreviousAsCount,
			shell.runtimeMutationResidentApplyBlasRefitRejectIndexCountMismatchCount,
			shell.runtimeMutationResidentApplyBlasRefitRejectPrimitiveCountMismatchCount,
			shell.runtimeMutationResidentApplyBlasRefitRejectZeroIndexCount,
			shell.runtimeMutationResidentApplyBlasRefitRejectZeroPrimitiveCount);
		const NRISE29FloorDeformerRouteFrameStats se29Deformer =
			mRenderer->GetSE29FloorDeformerRouteFrameStats();
		Printf(
			"PERF pt SE29 deformer NRI: frame=%llu candidates=%u admitted=%u exact_fallbacks=%u resident_rejects=%u layout_reject_mask=0x%x policy_reject_mask=0x%x dependency_groups=%u scheduled=%u budget_deferred=%u pending=%u pending_high_water=%u max_pending_age=%llu planned_vertex_spans=%u planned_primitive_spans=%u planned_vertex_bytes=%llu planned_primitive_bytes=%llu planned_refit_chunks=%u partial_upload_chunks=%u partial_vertex_spans=%u partial_primitive_spans=%u partial_vertex_bytes=%llu partial_primitive_bytes=%llu apply_failures=%u blas_updated=%u blas_recreated=%u\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			se29Deformer.candidates,
			se29Deformer.admitted,
			se29Deformer.exactFallbacks,
			se29Deformer.residentRejects,
			se29Deformer.layoutRejectMaskOr,
			se29Deformer.policyRejectMaskOr,
			se29Deformer.dependencyGroups,
			se29Deformer.scheduled,
			se29Deformer.budgetDeferred,
			se29Deformer.pending,
			se29Deformer.pendingHighWater,
			(unsigned long long)se29Deformer.maxPendingAge,
			se29Deformer.vertexSpans,
			se29Deformer.primitiveSpans,
			(unsigned long long)se29Deformer.vertexBytes,
			(unsigned long long)se29Deformer.primitiveBytes,
			se29Deformer.plannedRefitChunks,
			se29Deformer.partialUploadChunks,
			se29Deformer.partialUploadVertexSpans,
			se29Deformer.partialUploadPrimitiveSpans,
			(unsigned long long)se29Deformer.partialUploadVertexBytes,
			(unsigned long long)se29Deformer.partialUploadPrimitiveBytes,
			se29Deformer.applyFailures,
			se29Deformer.blasUpdated,
			se29Deformer.blasRecreated);
		const NRIMapMaterialOnlyRouteFrameStats materialRoute =
			mRenderer->GetMapMaterialOnlyRouteFrameStats();
		const nri_scene::PTMapMaterialStateVariantStats materialVariants =
			mRenderer->GetMapMaterialVariantStats();
		Printf(
			"PERF pt map material route NRI: frame=%llu candidates=%u admitted=%u terminal=%u animated=%u variant_hits=%u variant_inserts=%u variant_evictions=%u record_evictions=%u reject_mask=0x%llx preflight_reject_mask=0x%llx validation_failure_mask=0x%llx requests=%llu eligible=%llu layout_rejects=%llu total_hits=%llu total_inserts=%llu total_variant_evictions=%llu total_record_evictions=%llu fallbacks=%llu epoch_resets=%llu resident_records=%u resident_variants=%u variant_high_water=%u\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			materialRoute.candidates,
			materialRoute.admitted,
			materialRoute.terminalAdmissions,
			materialRoute.animatedAdmissions,
			materialRoute.variantHits,
			materialRoute.variantInserts,
			materialRoute.variantEvictions,
			materialRoute.recordEvictions,
			(unsigned long long)materialRoute.rejectMask,
			(unsigned long long)materialRoute.preflightRejectMask,
			(unsigned long long)materialRoute.validationFailureMask,
			(unsigned long long)materialVariants.requests,
			(unsigned long long)materialVariants.eligible,
			(unsigned long long)materialVariants.layoutRejects,
			(unsigned long long)materialVariants.hits,
			(unsigned long long)materialVariants.inserts,
			(unsigned long long)materialVariants.variantEvictions,
			(unsigned long long)materialVariants.recordEvictions,
			(unsigned long long)materialVariants.fallbacks,
			(unsigned long long)materialVariants.epochBuildResets,
			materialVariants.residentRecords,
			materialVariants.residentVariants,
			materialVariants.residentVariantHighWater);
		for (size_t index = 0; index < NRIRenderer::RuntimeResidentBlasRecreateTraceCount; ++index)
		{
			const auto& entry = shell.runtimeResidentBlasRecreateEntries[index];
			if (!entry.valid)
			{
				continue;
			}

			Printf(
				"PERF pt resident blas recreate top NRI: frame=%llu rank=%u chunk=%u sector=%d reasons=0x%x fallback=0x%x surfaces=%u tris=%u mats=%u force_topology=%u recovered_empty=%u kept_slice=%u topology_changed=%u had_as=%u\n",
				(unsigned long long)mLastFrameBoundaryStats.frameNumber,
				(unsigned)(index + 1),
				entry.chunkIndex,
				entry.sectorIndex,
				entry.reasonMask,
				entry.fallbackMask,
				entry.surfaceCount,
				entry.triangleCount,
				entry.materialCount,
				entry.forceTopology ? 1u : 0u,
				entry.recoveredEmpty ? 1u : 0u,
				entry.keptGeometrySlice ? 1u : 0u,
				entry.topologyChanged ? 1u : 0u,
				entry.hadAccelerationStructure ? 1u : 0u);
		}
		for (size_t index = 0; index < NRIRenderer::RuntimeResidentBlasRefitRejectTraceCount; ++index)
		{
			const auto& entry = shell.runtimeResidentBlasRefitRejectEntries[index];
			if (!entry.valid)
			{
				continue;
			}

			Printf(
				"PERF pt resident blas refit top NRI: frame=%llu rank=%u chunk=%u sector=%d reasons=0x%x reject=0x%x prev_indices=%u live_indices=%u prev_prims=%u live_prims=%u had_as=%u\n",
				(unsigned long long)mLastFrameBoundaryStats.frameNumber,
				(unsigned)(index + 1),
				entry.chunkIndex,
				entry.sectorIndex,
				entry.reasonMask,
				entry.rejectMask,
				entry.previousIndexCount,
				entry.liveIndexCount,
				entry.previousPrimitiveCount,
				entry.livePrimitiveCount,
				entry.hadAccelerationStructure ? 1u : 0u);
		}
		Printf(
			"PERF pt structural rebuild summary NRI: frame=%llu total=%u delta=%u view_changed=%u static_anim_flip=%u excl_static_flip=%u force_topology=%u invalid=%u material_only=%u sector_mat_only=%u wall_mat_only=%u mixed_mat=%u geometry_or_dirty=%u\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.runtimeMutationStructuralRebuildChunks,
			shell.runtimeMutationStructuralReplacementDeltaChunks,
			shell.runtimeMutationStructuralReplacementViewChangedChunks,
			shell.runtimeMutationStructuralStaticAnimatedModeFlipChunks,
			shell.runtimeMutationStructuralExcludeStaticFlipChunks,
			shell.runtimeMutationStructuralForcedTopologyChunks,
			shell.runtimeMutationStructuralInvalidChunks,
			shell.runtimeMutationStructuralMaterialOnlyChunks,
			shell.runtimeMutationStructuralSectorMaterialOnlyChunks,
			shell.runtimeMutationStructuralWallMaterialOnlyChunks,
			shell.runtimeMutationStructuralMixedMaterialOnlyChunks,
			shell.runtimeMutationStructuralGeometryOrDirtyChunks);
		for (size_t index = 0; index < NRIRenderer::RuntimeStructuralRebuildTraceCount; ++index)
		{
			const auto& entry = shell.runtimeStructuralRebuildEntries[index];
			if (!entry.valid)
			{
				continue;
			}

			Printf(
				"PERF pt structural rebuild top NRI: frame=%llu rank=%u chunk=%u sector=%d reasons=0x%x triggers=0x%x surfaces=%u tris=%u mats=%u action=%s material_only=%u sector_mat_only=%u wall_mat_only=%u mixed_mat=%u geometry_or_dirty=%u\n",
				(unsigned long long)mLastFrameBoundaryStats.frameNumber,
				(unsigned)(index + 1),
				entry.chunkIndex,
				entry.sectorIndex,
				entry.reasonMask,
				entry.triggerMask,
				entry.surfaceCount,
				entry.triangleCount,
				entry.materialCount,
				getRuntimeMutationTraceActionName(entry.action),
				entry.materialOnly ? 1u : 0u,
				entry.sectorMaterialOnly ? 1u : 0u,
				entry.wallMaterialOnly ? 1u : 0u,
				entry.mixedMaterialOnly ? 1u : 0u,
				entry.geometryOrDirty ? 1u : 0u);
		}
		Printf(
			"PERF pt geometry-dirty summary NRI: frame=%llu sector_geom_only=%u wall_geom_only=%u sector_wall_geom=%u dirty_only=%u geom_dirty_mixed=%u force_topology_only=%u real_count_change=%u walls_only_change=%u flats_only_change=%u walls_flats_change=%u\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.runtimeMutationGeometryDirtySectorGeometryOnlyChunks,
			shell.runtimeMutationGeometryDirtyWallGeometryOnlyChunks,
			shell.runtimeMutationGeometryDirtySectorWallGeometryChunks,
			shell.runtimeMutationGeometryDirtyDirtyOnlyChunks,
			shell.runtimeMutationGeometryDirtyGeometryDirtyMixedChunks,
			shell.runtimeMutationGeometryDirtyForceTopologyOnlyChunks,
			shell.runtimeMutationGeometryDirtyRealCountChangeChunks,
			shell.runtimeMutationGeometryDirtyWallsOnlyChangedChunks,
			shell.runtimeMutationGeometryDirtyFlatsOnlyChangedChunks,
			shell.runtimeMutationGeometryDirtyWallsAndFlatsChangedChunks);
		for (size_t index = 0; index < NRIRenderer::RuntimeGeometryDirtyTraceCount; ++index)
		{
			const auto& entry = shell.runtimeGeometryDirtyEntries[index];
			if (!entry.valid)
			{
				continue;
			}

			Printf(
				"PERF pt geometry-dirty top NRI: frame=%llu rank=%u chunk=%u sector=%d reasons=0x%x family=0x%x prev_walls=%u live_walls=%u prev_flats=%u live_flats=%u prev_tris=%u live_tris=%u prev_mats=%u live_mats=%u force_topology=%u count_changed=%u walls_changed=%u flats_changed=%u\n",
				(unsigned long long)mLastFrameBoundaryStats.frameNumber,
				(unsigned)(index + 1),
				entry.chunkIndex,
				entry.sectorIndex,
				entry.reasonMask,
				entry.familyMask,
				entry.previousWallCount,
				entry.liveWallCount,
				entry.previousFlatCount,
				entry.liveFlatCount,
				entry.previousTriangleCount,
				entry.liveTriangleCount,
				entry.previousMaterialCount,
				entry.liveMaterialCount,
				entry.forceTopology ? 1u : 0u,
				entry.countChanged ? 1u : 0u,
				entry.wallsChanged ? 1u : 0u,
				entry.flatsChanged ? 1u : 0u);
		}
		Printf(
			"PERF pt recurring-chunk summary NRI: frame=%llu tracked=%u recurring=%u visits=%u unique_states=%u transitions=%u repeated_state_hits=%u aba_hits=%u max_unique_states=%u\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.runtimeRecurringChunkTrackedCount,
			shell.runtimeRecurringChunkRecurringCount,
			shell.runtimeRecurringChunkVisitCount,
			shell.runtimeRecurringChunkUniqueStateCount,
			shell.runtimeRecurringChunkTransitionCount,
			shell.runtimeRecurringChunkRepeatedStateHitCount,
			shell.runtimeRecurringChunkAbaRecurrenceCount,
			shell.runtimeRecurringChunkMaxUniqueStateCount);
		for (size_t index = 0; index < NRIRenderer::RuntimeRecurringChunkTraceCount; ++index)
		{
			const auto& entry = shell.runtimeRecurringChunkEntries[index];
			if (!entry.valid)
			{
				continue;
			}

			Printf(
				"PERF pt recurring-chunk top NRI: frame=%llu rank=%u chunk=%u sector=%d reasons=0x%x visits=%u unique_states=%u transitions=%u repeated_state_hits=%u aba_hits=%u prev_sig=0x%llx last_sig=0x%llx walls=%u flats=%u tris=%u mats=%u\n",
				(unsigned long long)mLastFrameBoundaryStats.frameNumber,
				(unsigned)(index + 1),
				entry.chunkIndex,
				entry.sectorIndex,
				entry.lastReasonMask,
				entry.visitCount,
				entry.uniqueStateCount,
				entry.transitionCount,
				entry.repeatedStateHitCount,
				entry.abaRecurrenceCount,
				(unsigned long long)entry.previousStateSignature,
				(unsigned long long)entry.lastStateSignature,
				entry.lastWallCount,
				entry.lastFlatCount,
				entry.lastTriangleCount,
				entry.lastMaterialCount);
		}
		Printf(
			"PERF pt material-only mismatch summary NRI: frame=%llu mismatches=%u refresh=%u rebuild=%u filtered_wall_only=%u filtered_flat_only=%u filtered_mixed=%u resident_wall_only=%u resident_flat_only=%u resident_mixed=%u\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.runtimeMutationMaterialOnlyMismatchCount,
			shell.runtimeMutationMaterialOnlyMismatchRefreshCount,
			shell.runtimeMutationMaterialOnlyMismatchRebuildCount,
			shell.runtimeMutationMaterialOnlyMismatchFilteredWallOnlyCount,
			shell.runtimeMutationMaterialOnlyMismatchFilteredFlatOnlyCount,
			shell.runtimeMutationMaterialOnlyMismatchFilteredMixedCount,
			shell.runtimeMutationMaterialOnlyMismatchResidentWallOnlyCount,
			shell.runtimeMutationMaterialOnlyMismatchResidentFlatOnlyCount,
			shell.runtimeMutationMaterialOnlyMismatchResidentMixedCount);
		for (size_t index = 0; index < NRIRenderer::RuntimeMaterialOnlyMismatchTraceCount; ++index)
		{
			const auto& entry = shell.runtimeMaterialOnlyMismatchEntries[index];
			if (!entry.valid)
			{
				continue;
			}

			Printf(
				"PERF pt material-only mismatch top NRI: frame=%llu rank=%u source=%s chunk=%u sector=%d reasons=0x%x filtered_surfaces=%u filtered_mats=%u resident_mats=%u filtered_walls=%u filtered_flats=%u resident_walls=%u resident_flats=%u\n",
				(unsigned long long)mLastFrameBoundaryStats.frameNumber,
				(unsigned)(index + 1),
				entry.refreshPath ? "refresh" : "rebuild",
				entry.chunkIndex,
				entry.sectorIndex,
				entry.reasonMask,
				entry.filteredSurfaceCount,
				entry.filteredMaterialCount,
				entry.residentMaterialCount,
				entry.filteredWallCount,
				entry.filteredFlatCount,
				entry.residentWallCount,
				entry.residentFlatCount);
		}
		for (size_t index = 0; index < NRIRenderer::RuntimeMutationTopTraceCount; ++index)
		{
			const auto& entry = shell.runtimeMutationTopEntries[index];
			if (!entry.valid)
			{
				continue;
			}

			Printf(
				"PERF pt mutation top NRI: frame=%llu rank=%u chunk=%u sector=%d reasons=0x%x section_dirty=%u surfaces=%u tris=%u mats=%u action=%s force_topology=%u resident_mat=%u resident_geo=%u recovered_empty=%u\n",
				(unsigned long long)mLastFrameBoundaryStats.frameNumber,
				(unsigned)(index + 1),
				entry.chunkIndex,
				entry.sectorIndex,
				entry.reasonMask,
				entry.sectionDirtyCount,
				entry.surfaceCount,
				entry.triangleCount,
				entry.materialCount,
				getRuntimeMutationTraceActionName(entry.action),
				entry.forceTopology ? 1u : 0u,
				entry.residentMaterialDirty ? 1u : 0u,
				entry.residentGeometryDirty ? 1u : 0u,
				entry.recoveredEmpty ? 1u : 0u);
		}
		for (size_t index = 0; index < NRIRenderer::RuntimeInvisibleProofTraceCount; ++index)
		{
			const auto& entry = shell.runtimeInvisibleProofEntries[index];
			if (!entry.valid)
			{
				continue;
			}

			Printf(
				"PERF pt mutation invisible proof NRI: frame=%llu rank=%u chunk=%u sector=%d reasons=0x%x sources=0x%x score=%u prev_surfaces=%u prev_tris=%u prev_mats=%u resident_prims=%u resident_mats=%u replacement_valid=%u resident_authoritative=%u resident_available=%u visible_floor=%u visible_ceiling=%u exact_cached=%u exact_match=%u animated_match=%u exclude_static=%u static_anim=%u has_anim=%u anim_suppressed=%u hardware_canvas=%u portal=%u sector_light=%u safe_resident_noop=%u\n",
				(unsigned long long)mLastFrameBoundaryStats.frameNumber,
				(unsigned)(index + 1),
				entry.chunkIndex,
				entry.sectorIndex,
				entry.reasonMask,
				entry.sourceMask,
				entry.score,
				entry.previousSurfaceCount,
				entry.previousTriangleCount,
				entry.previousMaterialCount,
				entry.residentPrimitiveCount,
				entry.residentMaterialCount,
				entry.replacementValid ? 1u : 0u,
				entry.residentAuthoritative ? 1u : 0u,
				entry.residentAvailable ? 1u : 0u,
				entry.visibleFloor ? 1u : 0u,
				entry.visibleCeiling ? 1u : 0u,
				entry.exactSignatureCached ? 1u : 0u,
				entry.exactSignatureMatch ? 1u : 0u,
				entry.animatedMaterialMatch ? 1u : 0u,
				entry.excludeStaticChunk ? 1u : 0u,
				entry.staticAnimatedReplacement ? 1u : 0u,
				entry.hasAnimatedTextureCandidates ? 1u : 0u,
				entry.animatedRefreshSuppressed ? 1u : 0u,
				entry.hardwareCanvas ? 1u : 0u,
				entry.portalChunk ? 1u : 0u,
				entry.sectorLightingCandidate ? 1u : 0u,
				entry.safeResidentNoopCandidate ? 1u : 0u);
		}
		for (size_t index = 0; index < NRIRenderer::RuntimeSectorDirtyTruthTraceCount; ++index)
		{
			const auto& entry = shell.runtimeSectorDirtyTruthEntries[index];
			if (!entry.valid)
			{
				continue;
			}

			Printf(
				"PERF pt sector dirty truth NRI: frame=%llu rank=%u chunk=%u sector=%d reasons=0x%x prev_source=%s force_topology=%u baseline_changed=%u geometry_changed=%u material_changed=%u prev_surfaces=%u live_surfaces=%u prev_tris=%u live_tris=%u\n",
				(unsigned long long)mLastFrameBoundaryStats.frameNumber,
				(unsigned)(index + 1),
				entry.chunkIndex,
				entry.sectorIndex,
				entry.reasonMask,
				getRuntimeSectorDirtyPreviousStateSourceName(entry.previousStateSource),
				entry.forceTopology ? 1u : 0u,
				entry.baselineChanged ? 1u : 0u,
				entry.geometryChanged ? 1u : 0u,
				entry.materialChanged ? 1u : 0u,
				entry.previousSurfaceCount,
				entry.liveSurfaceCount,
				entry.previousTriangleCount,
				entry.liveTriangleCount);
		}
		Printf(
			"PERF pt animated steady NRI: frame=%llu suppressed_active=%u suppressions=%u unique_touched=%u material_refresh=%u attempts=%u suppressed_attempts=%u unsuppressed_attempts=%u resident_apply=%u suppressed_resident_apply=%u unsuppressed_resident_apply=%u sync_skip=%u\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.runtimeAnimatedSuppressedActiveCount,
			shell.runtimeAnimatedSuppressionEmitCount,
			shell.runtimeAnimatedUniqueTouchedCount,
			shell.runtimeAnimatedMaterialRefreshCount,
			shell.runtimeAnimatedAttemptCount,
			shell.runtimeAnimatedSuppressedAttemptCount,
			shell.runtimeAnimatedUnsuppressedAttemptCount,
			shell.runtimeAnimatedResidentApplyCount,
			shell.runtimeAnimatedSuppressedResidentApplyCount,
			shell.runtimeAnimatedUnsuppressedResidentApplyCount,
			shell.runtimeAnimatedSyncSkipCount);
		for (size_t index = 0; index < NRIRenderer::RuntimeAnimatedChurnTraceCount; ++index)
		{
			const auto& entry = shell.runtimeAnimatedChurnEntries[index];
			if (!entry.valid)
			{
				continue;
			}

			Printf(
				"PERF pt animated top NRI: frame=%llu rank=%u chunk=%u sector=%d suppressed=%u material_refresh=%u attempts=%u resident_apply=%u sync_skip=%u\n",
				(unsigned long long)mLastFrameBoundaryStats.frameNumber,
				(unsigned)(index + 1),
				entry.chunkIndex,
				entry.sectorIndex,
				entry.suppressed ? 1u : 0u,
				entry.materialRefreshes,
				entry.runtimeAttempts,
				entry.residentApplies,
				entry.syncSkips);
		}
		Printf(
			"PERF pt texture detail NRI: frame=%llu reason=%s requested=%u actor_materials=%u base=%u glow=%u normal=%u metallic=%u roughness=%u emissive=%u cache=%u misses=%u inserts=%u transitions=%u lookup_ms=%.3f realize_ms=%.3f descriptor_ms=%.3f transition_ms=%.3f stable_slots=%u slots_live=%u slots_quarantined=%u slots_free=%u slot_reuses=%llu slot_exhaustions=%llu stable_descriptor_hits=%u stable_descriptor_misses=%u descriptor_writes=%u descriptor_skips=%u descriptor_rows=%u material_builds=%u override_builds=%u override_ms=%.3f material_ms=%.3f\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.sceneTextureReason.empty() ? "none" : shell.sceneTextureReason.c_str(),
			shell.sceneTextureRequestedCount,
			shell.sceneTextureReferencedActorMaterialCount,
			shell.sceneTextureReferencedBaseCount,
			shell.sceneTextureReferencedGlowCount,
			shell.sceneTextureReferencedNormalCount,
			shell.sceneTextureReferencedMetallicCount,
			shell.sceneTextureReferencedRoughnessCount,
			shell.sceneTextureReferencedEmissiveCount,
			shell.sceneTextureCacheCount,
			shell.sceneTextureCacheMisses,
			shell.sceneTextureCacheInserts,
			shell.sceneTextureTransitionCount,
			shell.sceneTextureLookupMs,
			shell.sceneTextureRealizeMs,
			shell.sceneTextureDescriptorMs,
			shell.sceneTextureTransitionMs,
			shell.sceneTextureStableSlotMode,
			shell.sceneTextureSlotsLive,
			shell.sceneTextureSlotsQuarantined,
			shell.sceneTextureSlotsFree,
			(unsigned long long)shell.sceneTextureSlotReuses,
			(unsigned long long)shell.sceneTextureSlotExhaustions,
			shell.sceneTextureStableDescriptorHits,
			shell.sceneTextureStableDescriptorMisses,
			shell.sceneTextureDescriptorWrites,
			shell.sceneTextureDescriptorSkips,
			shell.sceneTextureDescriptorRowsWritten,
			shell.materialBuildCalls,
			shell.actorOverrideMapBuildCalls,
			shell.actorOverrideMapBuildMs,
			shell.materialBuildMs);
		for (size_t index = 0; index < NRIRenderer::MaterialBuildTraceSlotCount; ++index)
		{
			const auto& entry = shell.materialBuildByLabel[index];
			if (entry.calls == 0 && entry.overrideBuildCalls == 0)
			{
				continue;
			}

			Printf(
				"PERF pt material detail NRI: frame=%llu label=%s calls=%u override_builds=%u actor_rule_map_builds=%u actor_rule_cache_hits=%u actor_rule_cache_misses=%u actor_rules=%u actor_rule_stamped=%u actor_rule_skip_non_sprite=%u actor_rule_skip_no_actor=%u fullbright_flagged=%u materials=%u actor_materials=%u textures=%u base=%u glow=%u normal=%u metallic=%u roughness=%u emissive=%u override_ms=%.3f actor_rule_ms=%.3f stamp_ms=%.3f fullbright_flag_ms=%.3f material_ms=%.3f override_apply_ms=%.3f\n",
				(unsigned long long)mLastFrameBoundaryStats.frameNumber,
				NRIRenderer::GetMaterialBuildTraceSlotName((NRIRenderer::MaterialBuildTraceSlot)index),
				entry.calls,
				entry.overrideBuildCalls,
				entry.actorOverlayRuleMapBuilds,
				entry.actorOverlayRuleMapCacheHits,
				entry.actorOverlayRuleMapCacheMisses,
				entry.actorOverlayRuleCount,
				entry.actorOverlayStampedSpriteSurfaces,
				entry.actorOverlaySkippedNonSpriteSurfaces,
				entry.actorOverlaySkippedNoActorSurfaces,
				entry.fullbrightFlaggedSurfaces,
				entry.materialCount,
				entry.actorMaterialCount,
				entry.textureCount,
				entry.baseTextureCount,
				entry.glowTextureCount,
				entry.normalTextureCount,
				entry.metallicTextureCount,
				entry.roughnessTextureCount,
				entry.emissiveTextureCount,
				entry.overrideBuildMs,
				entry.actorOverlayRuleBuildMs,
				entry.actorOverlayStampMs,
				entry.fullbrightFlagMs,
				entry.materialBuildMs,
				entry.actorOverrideApplyMs);
		}
		Printf(
			"PERF pt actor overflow summary NRI: frame=%llu materials=%u base=%u normal=%u metallic=%u roughness=%u emissive=%u omitted=%u\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			shell.actorOverflowMaterialCount,
			shell.actorOverflowBaseClampCount,
			shell.actorOverflowNormalClampCount,
			shell.actorOverflowMetallicClampCount,
			shell.actorOverflowRoughnessClampCount,
			shell.actorOverflowEmissiveClampCount,
			shell.actorOverflowTraceOmittedCount);
		Printf(
			"PERF pt resource trace NRI: frame=%llu waits=%u wait_ms=%.3f grow=%u overwrite=%u scene_uploads=%u scene_bytes=%llu data_uploads=%u data_bytes=%llu emissive_uploads=%u emissive_bytes=%llu\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			resource.waitCalls,
			resource.waitMs,
			resource.growEvents,
			resource.overwriteEvents,
			resource.sceneUploadCalls,
			(unsigned long long)resource.sceneUploadBytes,
			resource.sceneDataUploadCalls,
			(unsigned long long)resource.sceneDataUploadBytes,
			resource.emissiveUploadCalls,
			(unsigned long long)resource.emissiveUploadBytes);
		Printf(
			"PERF pt resource scene detail NRI: frame=%llu dynamic=%u/%llu resident_chunk=%u/%llu persistent_voxel=%u/%llu persistent_variant=%u/%llu static_refresh=%u/%llu other=%u/%llu vertex=%llu index=%llu primitive=%llu material=%llu\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			resource.sceneDynamicUploadCalls,
			(unsigned long long)resource.sceneDynamicUploadBytes,
			resource.sceneResidentChunkUploadCalls,
			(unsigned long long)resource.sceneResidentChunkUploadBytes,
			resource.scenePersistentVoxelUploadCalls,
			(unsigned long long)resource.scenePersistentVoxelUploadBytes,
			resource.scenePersistentVoxelVariantUploadCalls,
			(unsigned long long)resource.scenePersistentVoxelVariantUploadBytes,
			resource.sceneStaticRefreshUploadCalls,
			(unsigned long long)resource.sceneStaticRefreshUploadBytes,
			resource.sceneOtherUploadCalls,
			(unsigned long long)resource.sceneOtherUploadBytes,
			(unsigned long long)resource.sceneVertexUploadBytes,
			(unsigned long long)resource.sceneIndexUploadBytes,
			(unsigned long long)resource.scenePrimitiveUploadBytes,
			(unsigned long long)resource.sceneMaterialUploadBytes);
		Printf(
			"PERF pt resource waits NRI: frame=%llu resident_chunk=%u/%.3f resident_chunk_blas=%u/%.3f scene_data=%u/%.3f scene_buffer=%u/%.3f emissive_sampling=%u/%.3f world_tlas_instance=%u/%.3f world_tlas_scratch=%u/%.3f emissive_tlas_instance=%u/%.3f emissive_tlas_scratch=%u/%.3f other=%u/%.3f\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			resource.residentChunkWriteWaitCalls,
			resource.residentChunkWriteWaitMs,
			resource.residentChunkBlasRebuildWaitCalls,
			resource.residentChunkBlasRebuildWaitMs,
			resource.sceneDataUploadWaitCalls,
			resource.sceneDataUploadWaitMs,
			resource.sceneBufferUploadWaitCalls,
			resource.sceneBufferUploadWaitMs,
			resource.emissiveSamplingUploadWaitCalls,
			resource.emissiveSamplingUploadWaitMs,
			resource.worldTlasInstanceUploadWaitCalls,
			resource.worldTlasInstanceUploadWaitMs,
			resource.worldTlasScratchResizeWaitCalls,
			resource.worldTlasScratchResizeWaitMs,
			resource.emissiveTlasInstanceUploadWaitCalls,
			resource.emissiveTlasInstanceUploadWaitMs,
			resource.emissiveTlasScratchResizeWaitCalls,
			resource.emissiveTlasScratchResizeWaitMs,
			resource.otherWaitCalls,
			resource.otherWaitMs);
		Printf(
			"PERF pt resident write batch NRI: frame=%llu chunks=%u geom_dirty=%u mat_dirty=%u recover_empty=%u mat_fallback=%u blas_rebuild=%u vertex_bytes=%llu index_bytes=%llu prim_bytes=%llu material_bytes=%llu\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			resource.residentChunkBatchChunkCount,
			resource.residentChunkBatchGeometryDirtyCount,
			resource.residentChunkBatchMaterialDirtyCount,
			resource.residentChunkBatchRecoverEmptyCount,
			resource.residentChunkBatchMaterialFallbackCount,
			resource.residentChunkBatchBlasRebuildCount,
			(unsigned long long)resource.residentChunkBatchVertexBytes,
			(unsigned long long)resource.residentChunkBatchIndexBytes,
			(unsigned long long)resource.residentChunkBatchPrimitiveBytes,
			(unsigned long long)resource.residentChunkBatchMaterialBytes);
	}
	return rendered;
}

bool NRIRenderDevice::HasActiveSceneFrame() const
{
	return mInitialized && mFrameBegun && mCommandBuffer != nullptr && mActiveTarget != nullptr;
}

bool NRIRenderDevice::HasCurrentCommandBuffer() const
{
	return mCommandBuffer != nullptr;
}

bool NRIRenderDevice::HasActiveTarget() const
{
	return mActiveTarget != nullptr;
}

bool NRIRenderDevice::StartPathTracingLevelPreload()
{
	if (mLevelTransitionInProgress)
	{
		mPathTracingLevelPreloadPending = false;
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=device-start result=skip reason=level-transition initialized=%u renderer=%u pending=%u frame_begun=%u active_target=%u\n",
				mInitialized ? 1u : 0u,
				mRenderer != nullptr ? 1u : 0u,
				mPathTracingLevelPreloadPending ? 1u : 0u,
				mFrameBegun ? 1u : 0u,
				mActiveTarget != nullptr ? 1u : 0u);
		}
		return false;
	}

	if (!mInitialized || mRenderer == nullptr)
	{
		mPathTracingLevelPreloadPending = true;
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=device-start result=pending reason=renderer-not-ready initialized=%u renderer=%u pending=%u frame_begun=%u active_target=%u\n",
				mInitialized ? 1u : 0u,
				mRenderer != nullptr ? 1u : 0u,
				mPathTracingLevelPreloadPending ? 1u : 0u,
				mFrameBegun ? 1u : 0u,
				mActiveTarget != nullptr ? 1u : 0u);
		}
		return true;
	}

	if (!mRenderer->RefreshPathTracingAvailability() || !mRenderer->IsPathTracingSupported())
	{
		mPathTracingLevelPreloadPending = true;
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=device-start result=pending reason=pt-unavailable-at-start initialized=%u renderer=%u pending=%u frame_begun=%u active_target=%u\n",
				mInitialized ? 1u : 0u,
				mRenderer != nullptr ? 1u : 0u,
				mPathTracingLevelPreloadPending ? 1u : 0u,
				mFrameBegun ? 1u : 0u,
				mActiveTarget != nullptr ? 1u : 0u);
		}
		return true;
	}

	mPathTracingLevelPreloadPending = true;
	if ((int)nri_ptloadingtrace >= 1)
	{
		Printf("NRI PT loading gate: event=device-start result=pending reason=ready initialized=%u renderer=%u pending=%u frame_begun=%u active_target=%u\n",
			mInitialized ? 1u : 0u,
			mRenderer != nullptr ? 1u : 0u,
			mPathTracingLevelPreloadPending ? 1u : 0u,
			mFrameBegun ? 1u : 0u,
			mActiveTarget != nullptr ? 1u : 0u);
	}
	return true;
}

bool NRIRenderDevice::TickPathTracingLevelPreload()
{
	if (mTerminalDeviceLoss)
	{
		mPathTracingLevelPreloadPending = false;
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=device-tick result=failed reason=terminal-device-loss\n");
		}
		return false;
	}

	if (!mPathTracingLevelPreloadPending)
	{
		if ((int)nri_ptloadingtrace >= 2)
		{
			Printf("NRI PT loading gate: event=device-tick result=ready reason=no-pending initialized=%u renderer=%u frame_begun=%u active_target=%u\n",
				mInitialized ? 1u : 0u,
				mRenderer != nullptr ? 1u : 0u,
				mFrameBegun ? 1u : 0u,
				mActiveTarget != nullptr ? 1u : 0u);
		}
		return true;
	}

	if (mLevelTransitionInProgress)
	{
		mPathTracingLevelPreloadPending = false;
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=device-tick result=ready reason=level-transition initialized=%u renderer=%u frame_begun=%u active_target=%u\n",
				mInitialized ? 1u : 0u,
				mRenderer != nullptr ? 1u : 0u,
				mFrameBegun ? 1u : 0u,
				mActiveTarget != nullptr ? 1u : 0u);
		}
		return true;
	}

	if (!mInitialized || mRenderer == nullptr)
	{
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=device-tick result=wait reason=renderer-not-ready initialized=%u renderer=%u frame_begun=%u active_target=%u\n",
				mInitialized ? 1u : 0u,
				mRenderer != nullptr ? 1u : 0u,
				mFrameBegun ? 1u : 0u,
				mActiveTarget != nullptr ? 1u : 0u);
		}
		return false;
	}

	const bool useStandalonePreload = !mFrameBegun;
	bool standaloneContextOpened = false;
	if (useStandalonePreload)
	{
		standaloneContextOpened = BeginPreloadCommandContext("level-preload");
		if (!standaloneContextOpened)
		{
			return false;
		}
	}

	if (mCommandBuffer == nullptr || (!useStandalonePreload && mActiveTarget == nullptr))
	{
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=device-tick result=wait reason=command-context-not-ready initialized=%u renderer=%u frame_begun=%u command_buffer=%u active_target=%u standalone_context=%u\n",
				mInitialized ? 1u : 0u,
				mRenderer != nullptr ? 1u : 0u,
				mFrameBegun ? 1u : 0u,
				mCommandBuffer != nullptr ? 1u : 0u,
				mActiveTarget != nullptr ? 1u : 0u,
				mPreloadCommandContextActive ? 1u : 0u);
		}
		if (standaloneContextOpened)
		{
			EndPreloadCommandContext("level-preload-abort");
		}
		return false;
	}

	const uint32_t fallbackWidth = (uint32_t)(std::max)(GetWidth(), 1);
	const uint32_t fallbackHeight = (uint32_t)(std::max)(GetHeight(), 1);
	const uint32_t outputWidth = mSceneViewport.width > 0 ? (uint32_t)mSceneViewport.width : fallbackWidth;
	const uint32_t outputHeight = mSceneViewport.height > 0 ? (uint32_t)mSceneViewport.height : fallbackHeight;
	const uint32_t targetWidth = mActiveTarget != nullptr ? std::max<uint32_t>(mActiveTarget->width, 1u) : outputWidth;
	const uint32_t targetHeight = mActiveTarget != nullptr ? std::max<uint32_t>(mActiveTarget->height, 1u) : outputHeight;
	bool ready = mRenderer->PreloadLevelScene(outputWidth, outputHeight, targetWidth, targetHeight, !useStandalonePreload, useStandalonePreload);
	if (standaloneContextOpened && !EndPreloadCommandContext("level-preload"))
	{
		ready = false;
	}
	if (ready)
	{
		mPathTracingLevelPreloadPending = false;
		LogLevelTransitionSnapshot("preload-ready", mCurrentLevelTransition, mPathTracingLevelPreloadPending, 0);
	}
	if ((int)nri_ptloadingtrace >= 1)
	{
		Printf("NRI PT loading gate: event=device-tick result=%s reason=renderer-preload output=%ux%u target=%ux%u pending=%u\n",
			ready ? "ready" : "wait",
			outputWidth,
			outputHeight,
			targetWidth,
			targetHeight,
			mPathTracingLevelPreloadPending ? 1u : 0u);
	}
	return ready;
}

bool NRIRenderDevice::IsPathTracingLevelPreloadPending() const
{
	return mPathTracingLevelPreloadPending;
}

void NRIRenderDevice::CancelPathTracingLevelPreload()
{
	if ((int)nri_ptloadingtrace >= 2 && mPathTracingLevelPreloadPending)
	{
		Printf("NRI PT loading gate: event=device-cancel pending=1 initialized=%u renderer=%u frame_begun=%u active_target=%u\n",
			mInitialized ? 1u : 0u,
			mRenderer != nullptr ? 1u : 0u,
			mFrameBegun ? 1u : 0u,
			mActiveTarget != nullptr ? 1u : 0u);
	}
	mPathTracingLevelPreloadPending = false;
}

void NRIRenderDevice::NotifyPathTracingLevelFirstFrameRelease()
{
	if (mRenderer != nullptr)
	{
		mRenderer->TraceStartupMutationProbe("first-frame-release");
		mRenderer->OnLevelFirstFrameRelease();
	}
	LogLevelTransitionSnapshot("first-frame-release", mCurrentLevelTransition, mPathTracingLevelPreloadPending, 0);
	TraceVoxelPreloadLifecycle("first-frame-release", mCurrentLevelTransition);
}

void NRIRenderDevice::NotifyPathTracingLevelPreloadFinalCheckRelease()
{
	if (mRenderer != nullptr)
	{
		mRenderer->TraceStartupMutationProbe("final-check-release");
	}
	LogLevelTransitionSnapshot("final-check-release", mCurrentLevelTransition, mPathTracingLevelPreloadPending, 0);
	TraceVoxelPreloadLifecycle("final-check-release", mCurrentLevelTransition);
}

void NRIRenderDevice::TraceVoxelPreloadLifecycle(const char* stage, const LevelTransitionInfo& info) const
{
	if ((int)nri_ptloadingtrace < 1)
	{
		return;
	}

	const auto now = std::chrono::steady_clock::now();
	const double elapsedMs =
		mLevelTransitionTimelineSerial == info.serial && mLevelTransitionAcceptedTime.time_since_epoch().count() != 0 ?
		std::chrono::duration<double, std::milli>(now - mLevelTransitionAcceptedTime).count() : 0.0;
	const NRIRenderer::MemoryTelemetry rendererMemory =
		mRenderer != nullptr ? mRenderer->GetMemoryTelemetry() : NRIRenderer::MemoryTelemetry{};
	const NRIAdapterMemoryTelemetry adapterMemory = GetAdapterMemoryTelemetry();
	Printf("PERF pt voxel preload lifecycle NRI: stage=%s transition_serial=%llu old=%s new=%s elapsed_ms=%.3f tracked_bytes=%llu local_usage_bytes=%llu local_budget_bytes=%llu nonlocal_usage_bytes=%llu nonlocal_budget_bytes=%llu live_usage_available=%u\n",
		stage != nullptr ? stage : "unknown",
		(unsigned long long)info.serial,
		info.oldLevelName.IsNotEmpty() ? info.oldLevelName.GetChars() : "(none)",
		info.newLevelName.IsNotEmpty() ? info.newLevelName.GetChars() : "(none)",
		elapsedMs,
		(unsigned long long)rendererMemory.totalTrackedBytes,
		(unsigned long long)adapterMemory.localUsageBytes,
		(unsigned long long)adapterMemory.localBudgetBytes,
		(unsigned long long)adapterMemory.nonLocalUsageBytes,
		(unsigned long long)adapterMemory.nonLocalBudgetBytes,
		adapterMemory.liveUsageAvailable ? 1u : 0u);
}

void NRIRenderDevice::LogLevelTransitionSnapshot(const char* phase, const LevelTransitionInfo& info, bool preloadPending, uint32_t clearedWeaponLightEvents) const
{
	if (!nri_ptscenestats && (int)nri_ptloadingtrace < 1)
	{
		return;
	}

	LevelTransitionDeviceSnapshot deviceSnapshot = {};
	deviceSnapshot.initialized = mInitialized;
	deviceSnapshot.transitionInProgress = mLevelTransitionInProgress;
	deviceSnapshot.preloadPending = mPathTracingLevelPreloadPending;
	deviceSnapshot.frameBegun = mFrameBegun;
	deviceSnapshot.activeTarget = mActiveTarget != nullptr;
	deviceSnapshot.pendingWeaponLightEvents = (uint32_t)mPendingPathTracingWeaponLightEvents.Size();
	deviceSnapshot.weaponLightEventsEnqueuedThisFrame = mPathTracingWeaponLightEventsEnqueuedThisFrame;

	const NRIRenderer::LevelTransitionSnapshot rendererSnapshot =
		mRenderer != nullptr ? mRenderer->BuildLevelTransitionSnapshot() : NRIRenderer::LevelTransitionSnapshot{};
	const uint32_t portalCleanupAnomalies = HWDrawInfo::ConsumePortalCleanupAnomalyCount();

	Printf(
		"NRI PT level transition: phase=%s serial=%llu reason=%s old=%s new=%s dev_init=%s dev_transition=%s preload_pending=%s pending_weapon_events=%u cleared_weapon_events=%u weapon_events_enqueued=%u frame_begun=%s active_target=%s map_valid=%s map_build_serial=%llu map_chunks=%u map_surfaces=%u static_valid=%s static_build_serial=%llu static_chunks=%u static_materials=%u static_tex=%s static_buf=%s static_as=%s texture_cache=%u sky_texture_cache=%u mutation_chunks=%u mutation_active=%u mutation_valid=%u registry_valid=%s registry_entries=%u registry_chunks=%u registry_active=%u registry_mapped=%u registry_as=%u lighting_invalidation=%s probe_valid=%s probe_hit=%s probe_wall=%d probe_chunk=%d muzzle_slots=%u muzzle_active=%u lights_analytic=%u lights_manual=%u lights_emissive=%u lights_sector=%u debug_spheres=%u test_lights=%u pv_mesh=%u pv_material=%u pv_batch=%u pv_active=%u pv_records=%u pv_admit=%u pv_req_pending=%u pv_req_ready=%u pv_opt_pending=%u pv_failed=%u pv_resident_bytes=%llu pv_zero_ref_bytes=%llu pv_cold_mesh=%u pv_cold_material=%u pv_cold_prims=%llu pv_generation=%u pv_build_serial=%llu pv_desired=%u pv_desired_preload=%u pv_desired_actor=%u pv_gpu_ready=%u pv_retained=%u pv_queued=%u pv_queue_bytes=%llu pv_mesh_missing=%u pv_material_only=%u pv_blas_only=%u pv_forced=%u pv_preferred=%u scene_instance_bytes=%llu visible_chunk_bytes=%llu visible_flat_bytes=%llu reprojection_bytes=%llu dynamic_scratch_bytes=%llu world_tlas_scratch_bytes=%llu portal_cleanup_anomalies=%u\n",
		phase != nullptr ? phase : "unknown",
		(unsigned long long)info.serial,
		GetLevelTransitionReasonName(info.reason),
		info.oldLevelName.IsNotEmpty() ? info.oldLevelName.GetChars() : "(none)",
		info.newLevelName.IsNotEmpty() ? info.newLevelName.GetChars() : "(none)",
		deviceSnapshot.initialized ? "yes" : "no",
		deviceSnapshot.transitionInProgress ? "yes" : "no",
		preloadPending ? "yes" : "no",
		deviceSnapshot.pendingWeaponLightEvents,
		clearedWeaponLightEvents,
		deviceSnapshot.weaponLightEventsEnqueuedThisFrame,
		deviceSnapshot.frameBegun ? "yes" : "no",
		deviceSnapshot.activeTarget ? "yes" : "no",
		rendererSnapshot.mapWorldValid ? "yes" : "no",
		(unsigned long long)rendererSnapshot.mapWorldBuildSerial,
		rendererSnapshot.mapWorldChunkCount,
		rendererSnapshot.mapWorldSurfaceCount,
		rendererSnapshot.staticSceneValid ? "yes" : "no",
		(unsigned long long)rendererSnapshot.staticSceneBuildSerial,
		rendererSnapshot.staticSceneChunkCount,
		rendererSnapshot.staticSceneMaterialCount,
		rendererSnapshot.staticSceneTexturesResident ? "yes" : "no",
		rendererSnapshot.staticSceneBuffersResident ? "yes" : "no",
		rendererSnapshot.staticSceneAccelerationResident ? "yes" : "no",
		rendererSnapshot.textureCacheCount,
		rendererSnapshot.skyTextureCacheCount,
		rendererSnapshot.runtimeMutationChunkCount,
		rendererSnapshot.runtimeMutationActiveChunkCount,
		rendererSnapshot.runtimeMutationValidChunkCount,
		rendererSnapshot.residentChunkRegistryValid ? "yes" : "no",
		rendererSnapshot.residentChunkRegistryEntryCount,
		rendererSnapshot.residentChunkRegistryChunkCount,
		rendererSnapshot.residentChunkRegistryActiveChunkCount,
		rendererSnapshot.residentChunkRegistryMappedChunkCount,
		rendererSnapshot.residentChunkRegistryAccelerationResidentChunkCount,
		rendererSnapshot.pendingStaticMapLightingInvalidation ? "yes" : "no",
		rendererSnapshot.surfaceProbeValid ? "yes" : "no",
		rendererSnapshot.surfaceProbeHit ? "yes" : "no",
		rendererSnapshot.surfaceProbeWallIndex,
		rendererSnapshot.surfaceProbeMapChunkIndex,
		rendererSnapshot.transientMuzzleFlashSlotCount,
		rendererSnapshot.transientMuzzleFlashActiveCount,
		rendererSnapshot.analyticLightCount,
		rendererSnapshot.manualLightCount,
		rendererSnapshot.emissiveSurfaceCount,
		rendererSnapshot.activeSectorLightCount,
		rendererSnapshot.runtimeDebugSphereCount,
		rendererSnapshot.runtimeTestLightCount,
		rendererSnapshot.persistentVoxelMeshResources,
		rendererSnapshot.persistentVoxelMaterialResources,
		rendererSnapshot.persistentVoxelBatchActors,
		rendererSnapshot.persistentVoxelActiveInstances,
		rendererSnapshot.persistentVoxelInstanceRecords,
		rendererSnapshot.persistentVoxelAdmissionQueue,
		rendererSnapshot.persistentVoxelRequiredAdmissionPending,
		rendererSnapshot.persistentVoxelRequiredAdmissionReady,
		rendererSnapshot.persistentVoxelOptionalAdmissionPending,
		rendererSnapshot.persistentVoxelFailedAdmission,
		(unsigned long long)rendererSnapshot.persistentVoxelResidentBytes,
		(unsigned long long)rendererSnapshot.persistentVoxelZeroRefBytes,
		rendererSnapshot.persistentVoxelColdMeshes,
		rendererSnapshot.persistentVoxelColdMaterials,
		(unsigned long long)rendererSnapshot.persistentVoxelColdPrimitives,
		rendererSnapshot.persistentVoxelResidencyGeneration,
		(unsigned long long)rendererSnapshot.persistentVoxelResidencyBuildSerial,
		rendererSnapshot.persistentVoxelLastDesired,
		rendererSnapshot.persistentVoxelLastDesiredPreload,
		rendererSnapshot.persistentVoxelLastDesiredActor,
		rendererSnapshot.persistentVoxelLastGpuReady,
		rendererSnapshot.persistentVoxelLastRetained,
		rendererSnapshot.persistentVoxelLastQueued,
		(unsigned long long)rendererSnapshot.persistentVoxelLastQueuedBytes,
		rendererSnapshot.persistentVoxelLastMeshMissing,
		rendererSnapshot.persistentVoxelLastMaterialOnly,
		rendererSnapshot.persistentVoxelLastBlasOnly,
		rendererSnapshot.persistentVoxelLastForced,
		rendererSnapshot.persistentVoxelLastPreferred,
		(unsigned long long)rendererSnapshot.sceneInstanceBufferBytes,
		(unsigned long long)rendererSnapshot.visibleChunkBufferBytes,
		(unsigned long long)rendererSnapshot.visibleFlatBufferBytes,
		(unsigned long long)rendererSnapshot.reprojectionBufferBytes,
		(unsigned long long)rendererSnapshot.dynamicScratchBufferBytes,
		(unsigned long long)rendererSnapshot.worldTlasScratchBufferBytes,
		portalCleanupAnomalies);
}

void NRIRenderDevice::NotifyLevelUnloadBegin(const LevelTransitionInfo& info)
{
	mCurrentLevelTransition = info;
	mLevelTransitionInProgress = true;
	mLevelTransitionAcceptedTime = std::chrono::steady_clock::now();
	mLevelTransitionTimelineSerial = info.serial;
	TraceVoxelPreloadLifecycle("map-command-accepted", info);

	const bool hadPendingPreload = mPathTracingLevelPreloadPending;
	CancelPathTracingLevelPreload();

	if (mInitialized)
	{
		WaitForCommands(true);
	}

	const uint32_t clearedWeaponLightEvents = ClearPendingPathTracingWeaponLightEvents();
	ResetLevelTransitionShellState();
	if (mRenderer != nullptr)
	{
		mRenderer->OnLevelUnloadBegin(info);
	}
	LogLevelTransitionSnapshot("unload-begin", info, hadPendingPreload, clearedWeaponLightEvents);
	TraceVoxelPreloadLifecycle("unload-begin", info);
}

void NRIRenderDevice::NotifyLevelUnloadComplete(const LevelTransitionInfo& info)
{
	mCurrentLevelTransition = info;
	ResetLevelTransitionShellState();
	assert(mPendingPathTracingWeaponLightEvents.Size() == 0);
	if (mRenderer != nullptr)
	{
		mRenderer->OnLevelUnloadComplete(info);
	}
	LogLevelTransitionSnapshot("unload-complete", info, mPathTracingLevelPreloadPending, 0);
	TraceVoxelPreloadLifecycle("unload-complete", info);
}

void NRIRenderDevice::NotifyLevelLoadBegin(const LevelTransitionInfo& info)
{
	mCurrentLevelTransition = info;
	if (mRenderer != nullptr)
	{
		mRenderer->OnLevelLoadBegin(info);
	}
	mLevelTransitionInProgress = false;
	mPathTracingLevelPreloadPending = false;
	mNextPathTracingWeaponLightEventSerial = 1;
	mPathTracingWeaponLightEventsEnqueuedThisFrame = 0;
	LogLevelTransitionSnapshot("load-begin", info, mPathTracingLevelPreloadPending, 0);
	TraceVoxelPreloadLifecycle("level-load-begin", info);
}

bool NRIRenderDevice::ShouldSkipSceneBuildForPathTracedScene(int drawmode, bool portal) const
{
	return !!nri_ptsanity && drawmode == DM_MAINVIEW && !portal;
}

bool NRIRenderDevice::RenderPathTracingSanityFrame()
{
	if (mCommandBuffer == nullptr || mActiveTarget == nullptr || mActiveTarget->colorAttachmentView == nullptr)
	{
		return false;
	}

	mRenderState->EndFrame();
	PrepareTargetForRendering(*mActiveTarget, true);

	nri::AttachmentDesc colorAttachment = {};
	colorAttachment.descriptor = mActiveTarget->colorAttachmentView;
	colorAttachment.loadOp = nri::LoadOp::CLEAR;
	colorAttachment.storeOp = nri::StoreOp::STORE;
	colorAttachment.clearValue.color.f.x = mSceneClearColor[0];
	colorAttachment.clearValue.color.f.y = mSceneClearColor[1];
	colorAttachment.clearValue.color.f.z = mSceneClearColor[2];
	colorAttachment.clearValue.color.f.w = mSceneClearColor[3];

	nri::RenderingDesc renderingDesc = {};
	renderingDesc.colors = &colorAttachment;
	renderingDesc.colorNum = 1;
	mCore.CmdBeginRendering(*mCommandBuffer, renderingDesc);
	mCore.CmdEndRendering(*mCommandBuffer);
	mRenderState->NotifyExternalTargetWrite();
	return true;
}

IHardwareTexture* NRIRenderDevice::CreateHardwareTexture(int numchannels)
{
	return new NRIHardwareTexture(this, numchannels);
}

IVertexBuffer* NRIRenderDevice::CreateVertexBuffer()
{
	return new NRIHardwareVertexBuffer();
}

IIndexBuffer* NRIRenderDevice::CreateIndexBuffer()
{
	return new NRIHardwareIndexBuffer();
}

IDataBuffer* NRIRenderDevice::CreateDataBuffer(int bindingpoint, bool ssbo, bool needsresize)
{
	return new NRIHardwareDataBuffer(bindingpoint, ssbo, needsresize);
}

FTexture* NRIRenderDevice::WipeStartScreen()
{
	SetViewportRects(nullptr);

	auto tex = new FWrapperTexture(mScreenViewport.width, mScreenViewport.height, 1);
	auto systex = static_cast<NRIHardwareTexture*>(tex->GetSystemTexture());
	systex->CreateWipeTexture(mScreenViewport.width, mScreenViewport.height, "WipeStartScreen");
	return tex;
}

FTexture* NRIRenderDevice::WipeEndScreen()
{
	Draw2D();
	twod->Clear();

	auto tex = new FWrapperTexture(mScreenViewport.width, mScreenViewport.height, 1);
	auto systex = static_cast<NRIHardwareTexture*>(tex->GetSystemTexture());
	systex->CreateWipeTexture(mScreenViewport.width, mScreenViewport.height, "WipeEndScreen");
	return tex;
}

bool NRIRenderDevice::QueueScreenshot(FileWriter* file, const char* filename)
{
	if (file == nullptr)
	{
		return false;
	}

	if (!mInitialized)
	{
		return false;
	}

	PendingScreenshotCapture capture;
	capture.file.reset(file);
	capture.fileName = filename != nullptr ? filename : "";
	capture.serial = mNextScreenshotCaptureSerial++;
	capture.width = (uint32_t)GetWidth();
	capture.height = (uint32_t)GetHeight();
	Printf("NRI screenshot queued: serial=%llu path=\"%s\"\n",
		(unsigned long long)capture.serial,
		capture.fileName.GetChars());
	mPendingScreenshotCaptures.push_back(std::move(capture));
	return true;
}

TArray<uint8_t> NRIRenderDevice::GetScreenshotBuffer(int& pitch, ESSType& color_type, float& gamma)
{
	const int w = SCREENWIDTH;
	const int h = SCREENHEIGHT;

	TArray<uint8_t> buffer(w * h * 3, true);
	CopyScreenToBuffer(w, h, buffer.Data());

	pitch = w * 3;
	color_type = SS_RGB;
	gamma = 1.0f;
	return buffer;
}

void NRIRenderDevice::RefreshNativeFrameGenerationHandles()
{
	mNativeD3D12Device = nullptr;
	mNativeD3D12GraphicsQueue = nullptr;

	if (GetLiveAPI() != nri::GraphicsAPI::D3D12)
	{
		return;
	}

	if (mDevice != nullptr && mCore.GetDeviceNativeObject != nullptr)
	{
		mNativeD3D12Device = static_cast<ID3D12Device*>(mCore.GetDeviceNativeObject(mDevice));
	}

	if (mGraphicsQueue != nullptr && mCore.GetQueueNativeObject != nullptr)
	{
		mNativeD3D12GraphicsQueue = static_cast<ID3D12CommandQueue*>(mCore.GetQueueNativeObject(mGraphicsQueue));
	}
}

void NRIRenderDevice::RefreshNativeFrameGenerationSwapChain()
{
	mNativeD3D12SwapChain = nullptr;
}

bool NRIRenderDevice::RefreshFrameGenerationPresentTargets()
{
#ifndef _WIN32
	return false;
#else
	DestroyFrameGenerationPresentTargets();
	mFrameGenerationPresentAllowsTearing = false;

	if (mDevice == nullptr || mWrapperD3D12.CreateTextureD3D12 == nullptr || !mFrameGeneration.IsPresentBridgeActive())
	{
		return false;
	}

	IDXGISwapChain4* swapChain = mFrameGeneration.GetPresentSwapChain();
	if (swapChain == nullptr)
	{
		return false;
	}

	DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
	if (FAILED(swapChain->GetDesc1(&swapChainDesc)))
	{
		return false;
	}

	mFrameGenerationPresentAllowsTearing = (swapChainDesc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0;
	mFrameGenerationPresentImages.resize(swapChainDesc.BufferCount);
	for (UINT i = 0; i < swapChainDesc.BufferCount; ++i)
	{
		ID3D12Resource* nativeResource = nullptr;
		if (FAILED(swapChain->GetBuffer(i, IID_PPV_ARGS(&nativeResource))) || nativeResource == nullptr)
		{
			DestroyFrameGenerationPresentTargets();
			return false;
		}

		nri::TextureD3D12Desc textureDesc = {};
		textureDesc.d3d12Resource = nativeResource;

		auto& target = mFrameGenerationPresentImages[i];
		if (mWrapperD3D12.CreateTextureD3D12(*mDevice, textureDesc, target.texture) != nri::Result::SUCCESS)
		{
			nativeResource->Release();
			DestroyFrameGenerationPresentTargets();
			return false;
		}
		nativeResource->Release();

		target.owned = true;
		const nri::TextureDesc& wrappedDesc = mCore.GetTextureDesc(*target.texture);
		target.width = wrappedDesc.width;
		target.height = wrappedDesc.height;
		target.layerNum = wrappedDesc.layerNum;
		target.format = wrappedDesc.format;
		target.usage = wrappedDesc.usage;
		target.type = wrappedDesc.type;
		target.shaderViewType = nri::TextureView::TEXTURE;
		target.state = {};

		if (!CreateTextureViews(target))
		{
			DestroyFrameGenerationPresentTargets();
			return false;
		}
	}

	return true;
#endif
}

void NRIRenderDevice::DestroyFrameGenerationPresentTargets()
{
	for (auto& target : mFrameGenerationPresentImages)
	{
		DestroyTextureResource(target);
	}

	mFrameGenerationPresentImages.clear();
	mFrameGenerationPresentAllowsTearing = false;
}

void NRIRenderDevice::PrintPathTracingCaps() const
{
	if (mDevice == nullptr)
	{
		Printf("NRI PT capabilities are unavailable because the device is not initialized.\n");
		return;
	}

	const nri::DeviceDesc& deviceDesc = mCore.GetDeviceDesc(*mDevice);
	Printf("NRI PT caps: api=%s shader_model=%u.%u ray_tracing_tier=%u texture2D_max=%u root_constants=%u root_descriptors=%u descriptor_sets=%u\n",
		(const char*)nri_api,
		deviceDesc.shaderModel / 10,
		deviceDesc.shaderModel % 10,
		deviceDesc.tiers.rayTracing,
		deviceDesc.dimensions.texture2DMaxDim,
		deviceDesc.pipelineLayout.rootConstantMaxSize,
		deviceDesc.pipelineLayout.rootDescriptorMaxNum,
		deviceDesc.pipelineLayout.descriptorSetMaxNum);
	Printf("NRI PT upscalers: NIS=%s DLSS-SR=%s DLRR=%s portal_depth=%d\n",
		mUpscaler.IsUpscalerSupported(*mDevice, nri::UpscalerType::NIS) ? "yes" : "no",
		mUpscaler.IsUpscalerSupported(*mDevice, nri::UpscalerType::DLSR) ? "yes" : "no",
		mUpscaler.IsUpscalerSupported(*mDevice, nri::UpscalerType::DLRR) ? "yes" : "no",
		(int)nri_ptportaldepth);
	const auto& frameGenPolicy = mFrameGeneration.GetPolicy();
	const auto& frameGenPresentContract = mFrameGeneration.GetPresentContract();
	Printf("NRI PT framegen caps: requested=%s provider=%s resolved=%s output=%s->%s contract=%s scope=%s api=%s shader_model=%u.%u window=%s low_latency=%s->%s(avail=%s iface=%s swapchain=%s) async=%s->%s(avail=%s) ui=%s->%s(route=%s) swapchain=%s native=device:%s queue:%s swapchain:%s waitable=%s runtime=%s reason=%s\n",
		frameGenPolicy.requestedEnabled ? "on" : "off",
		NRIFrameGenerationContext::GetProviderName(frameGenPolicy.requestedProvider),
		NRIFrameGenerationContext::GetProviderName(frameGenPolicy.resolvedProvider),
		GetNRIPTOutputModeName(frameGenPolicy.requestedOutputMode),
		GetNRIPTOutputModeName(frameGenPolicy.resolvedOutputMode),
		NRIFrameGenerationContext::GetOutputContractName(frameGenPolicy.resolvedOutputContract),
		frameGenPolicy.outputContractScope,
		frameGenPolicy.selectedApiName,
		frameGenPolicy.shaderModel / 10u,
		frameGenPolicy.shaderModel % 10u,
		NRIFrameGenerationContext::GetWindowModeName(frameGenPolicy.fullscreenActive),
		frameGenPolicy.requestedLowLatency ? "on" : "off",
		frameGenPolicy.resolvedLowLatency ? "on" : "off",
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.lowLatencyAvailable),
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.lowLatencyInterfaceAvailable),
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.lowLatencySwapChainEnabled),
		frameGenPolicy.requestedAsync ? "on" : "off",
		frameGenPolicy.resolvedAsync ? "on" : "off",
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.asyncWorkloadAvailable),
		NRIFrameGenerationContext::GetUiModeName(frameGenPolicy.requestedUiMode),
		NRIFrameGenerationContext::GetUiModeName(frameGenPolicy.resolvedUiMode),
		GetFrameGenerationUiRouteName(),
		frameGenPolicy.swapChainReady ? "ready" : "cold",
		frameGenPolicy.nativeDeviceAvailable ? "ok" : "missing",
		frameGenPolicy.nativeGraphicsQueueAvailable ? "ok" : "missing",
		frameGenPolicy.nativeSwapChainAvailable ? "ok" : "missing",
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.waitableSwapChainAvailable),
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.providerRuntimeSupported),
		frameGenPolicy.resolvedReason);
	Printf("NRI PT framegen present contract: output=%s->%s proxy=%s hdr_swapchain=%s swapchain=%s texture=%s active=%s dxgi=%s active_dxgi=%s transfer=%s luminance=%.3f..%.3f hdr_scale=%.3f reason=%s\n",
		GetNRIPTOutputModeName(frameGenPresentContract.requestedOutputMode),
		GetNRIPTOutputModeName(frameGenPresentContract.resolvedOutputMode),
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPresentContract.proxyAllowed),
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPresentContract.usesHdrSwapChain),
		NRIFrameGenerationContext::GetSwapChainFormatName(frameGenPresentContract.createdSwapChainFormat),
		NRIFrameGenerationContext::GetNriFormatName(frameGenPresentContract.resolvedTextureFormat),
		NRIFrameGenerationContext::GetNriFormatName(frameGenPresentContract.activePresentTargetFormat),
		frameGenPresentContract.resolvedDxgiFormatValid ? NRIFrameGenerationContext::GetDxgiFormatName(frameGenPresentContract.resolvedDxgiFormat) : "unknown",
		frameGenPresentContract.activePresentTargetDxgiFormatValid ? NRIFrameGenerationContext::GetDxgiFormatName(frameGenPresentContract.activePresentTargetDxgiFormat) : "unknown",
		NRIFrameGenerationContext::GetPresentTransferFunctionName(frameGenPresentContract.transferFunction),
		frameGenPresentContract.minLuminance,
		frameGenPresentContract.maxLuminance,
		frameGenPresentContract.hdrPaperWhiteScale,
		frameGenPresentContract.resolvedReason);
	Printf("NRI PT framegen native: device=%s queue=%s swapchain=%s path=%s\n",
		mNativeD3D12Device != nullptr ? "ok" : "missing",
		mNativeD3D12GraphicsQueue != nullptr ? "ok" : "missing",
		mNativeD3D12SwapChain != nullptr ? "ok" : "missing",
		GetLiveAPI() == nri::GraphicsAPI::D3D12 ? "nri-public-device-queue-only" : "unsupported-api");
	const auto& frameGenProvider = mFrameGeneration.GetProviderState();
	Printf("NRI PT framegen provider: runtime=%s funcs=%s context=%s swapctx=%s bridge=%s debug=%s no_swapchain_notify=%s cfg=%s prepare=%s fg_dispatch=%s ui_reg=%s camera=%s lib=%s version=%s dims=render:%ux%u display:%ux%u counts=cfg:%llu prep:%llu fg:%llu frames=%llu/%llu query=%s/%s create=%s/%s config=%s/%s prepare=%s dispatch=%s vram=fg:%s:%llu/%llu sc:%s:%llu/%llu resets=%llu last_reset=%s present=%s/%s count=%llu reason=%s\n",
		frameGenProvider.runtimeLoaded ? "yes" : "no",
		frameGenProvider.runtimeFunctionsLoaded ? "yes" : "no",
		frameGenProvider.contextCreated ? "yes" : "no",
		frameGenProvider.swapChainContextCreated ? "yes" : "no",
		frameGenProvider.presentBridgeReady ? "yes" : "no",
		frameGenProvider.debugConfigured ? "yes" : "no",
		frameGenProvider.noSwapChainNotify ? "yes" : "no",
		frameGenProvider.configuredThisFrame ? "yes" : "no",
		frameGenProvider.prepareDispatchedThisFrame ? "yes" : "no",
		frameGenProvider.frameGenerationDispatchedThisFrame ? "yes" : "no",
		frameGenProvider.uiResourceRegisteredThisFrame ? "yes" : "no",
		frameGenProvider.prepareCameraInfoProvided ? "yes" : "no",
		frameGenProvider.runtimeLibrary,
		frameGenProvider.providerVersion,
		frameGenProvider.contextRenderWidth,
		frameGenProvider.contextRenderHeight,
		frameGenProvider.contextDisplayWidth,
		frameGenProvider.contextDisplayHeight,
		(unsigned long long)frameGenProvider.configureCount,
		(unsigned long long)frameGenProvider.prepareCount,
		(unsigned long long)frameGenProvider.dispatchCount,
		(unsigned long long)frameGenProvider.lastConfiguredFrameId,
		(unsigned long long)frameGenProvider.lastPreparedFrameId,
		NRIFrameGenerationContext::GetProviderReturnCodeName(frameGenProvider.lastQueryResult),
		NRIFrameGenerationContext::GetProviderReturnCodeName(frameGenProvider.lastSwapChainQueryResult),
		NRIFrameGenerationContext::GetProviderReturnCodeName(frameGenProvider.lastCreateResult),
		NRIFrameGenerationContext::GetProviderReturnCodeName(frameGenProvider.lastSwapChainCreateResult),
		NRIFrameGenerationContext::GetProviderReturnCodeName(frameGenProvider.lastConfigureResult),
		NRIFrameGenerationContext::GetProviderReturnCodeName(frameGenProvider.lastSwapChainConfigureResult),
		NRIFrameGenerationContext::GetProviderReturnCodeName(frameGenProvider.lastPrepareResult),
		NRIFrameGenerationContext::GetProviderReturnCodeName(frameGenProvider.lastDispatchResult),
		frameGenProvider.memoryUsageValid ? "yes" : "no",
		(unsigned long long)frameGenProvider.totalUsageBytes,
		(unsigned long long)frameGenProvider.aliasableUsageBytes,
		frameGenProvider.swapChainMemoryUsageValid ? "yes" : "no",
		(unsigned long long)frameGenProvider.swapChainTotalUsageBytes,
		(unsigned long long)frameGenProvider.swapChainAliasableUsageBytes,
		(unsigned long long)frameGenProvider.resetCount,
		frameGenProvider.lastResetReason,
		frameGenProvider.lastPresentMode,
		GetNriResultName(frameGenProvider.lastPresentResult),
		(unsigned long long)frameGenProvider.presentCount,
		frameGenProvider.lastStatusReason);
	Printf("NRI PT framegen present: current=%s bridge_active=%s generated=%s fallback_pending=%s last=%s result=%s\n",
		frameGenProvider.frameGenerationDispatchedThisFrame ? "generated" :
			(frameGenProvider.presentUsedBridgeThisFrame ? "passthrough" : "native"),
		frameGenProvider.presentBridgeReady ? "yes" : "no",
		frameGenProvider.frameGenerationDispatchedThisFrame ? "yes" : "no",
		frameGenProvider.nativeFallbackRequested ? "yes" : "no",
		frameGenProvider.lastPresentMode,
		NRIFrameGenerationContext::GetPresentResultName(frameGenProvider.lastPresentResult));
	const auto& lowLatencyState = mFrameGeneration.GetLowLatencyState();
	Printf("NRI PT low-latency: iface=%s swapchain=%s configured=%s sleep=%s count=%llu markers=%llu present=%s set_mode=%s sleep_result=%s sim=%s/%s submit=%s/%s report=%s present_us=%llu..%llu\n",
		lowLatencyState.interfaceAvailable ? "yes" : "no",
		lowLatencyState.swapChainEnabled ? "yes" : "no",
		lowLatencyState.sleepModeConfigured ? "yes" : "no",
		lowLatencyState.sleepInvoked ? "yes" : "no",
		(unsigned long long)lowLatencyState.latencySleepCount,
		(unsigned long long)lowLatencyState.markerCount,
		lowLatencyState.presentBoundarySeen ? "yes" : "no",
		GetNriResultName(lowLatencyState.setSleepModeResult),
		GetNriResultName(lowLatencyState.latencySleepResult),
		GetNriResultName(lowLatencyState.simulationStartMarkerResult),
		GetNriResultName(lowLatencyState.simulationEndMarkerResult),
		GetNriResultName(lowLatencyState.renderSubmitStartMarkerResult),
		GetNriResultName(lowLatencyState.renderSubmitEndMarkerResult),
		GetNriResultName(lowLatencyState.latencyReportResult),
		(unsigned long long)lowLatencyState.latencyReport.presentStartTimeUs,
		(unsigned long long)lowLatencyState.latencyReport.presentEndTimeUs);

	if (mRenderer != nullptr)
	{
		Printf("NRI PT availability: %s", mRenderer->IsPathTracingSupported() ? "available" : "raster-fallback");
		if (!mRenderer->IsPathTracingSupported())
		{
			Printf(" (%s)", mRenderer->GetAvailabilityReason());
		}
		Printf("\n");
	}
}

void NRIRenderDevice::PrintPathTracingStatus() const
{
	PrintPathTracingCaps();
	PrintFrameBoundaryStatus();
	PrintFrameSequenceStatus();
	PrintSwapChainStatus();
	PrintFrameShellStatus();
	Print2DTextureStatus();
	PrintVramTelemetryStatus();
	if (mRenderer != nullptr)
	{
		mRenderer->PrintStatus();
	}
}

void NRIRenderDevice::ResolvePathTracingSwapChainOutput(nri::SwapChainFormat& outRequestedFormat, nri::SwapChainFormat& outResolvedFormat, const char*& outReason) const
{
	const NRIPTOutputMode requestedMode = GetRequestedPathTracingOutputMode();
	outRequestedFormat = nri::SwapChainFormat::BT709_G22_8BIT;
	outResolvedFormat = nri::SwapChainFormat::BT709_G22_8BIT;
	outReason = "requested-sdr";

	if (requestedMode == NRIPTOutputMode::SDR)
	{
		return;
	}

	switch (requestedMode)
	{
	case NRIPTOutputMode::HDR:
		outRequestedFormat = nri::SwapChainFormat::BT709_G10_16BIT;
		if (GetSelectedAPI() != nri::GraphicsAPI::D3D12 && GetSelectedAPI() != nri::GraphicsAPI::VK)
		{
			outReason = "hdr-unsupported-api";
			return;
		}
		if (!mHasSwapChainDisplayDesc)
		{
			outReason = "hdr-await-display-desc";
			return;
		}
		if (!mSwapChainDisplayDesc.isHDR)
		{
			outReason = "hdr-display-sdr";
			return;
		}
		outResolvedFormat = outRequestedFormat;
		outReason = "hdr-resolved-linear16";
		return;

	case NRIPTOutputMode::HDRLinear16:
		outRequestedFormat = nri::SwapChainFormat::BT709_G10_16BIT;
		if (GetSelectedAPI() != nri::GraphicsAPI::D3D12)
		{
			outReason = "hdr-linear16-unsupported-api";
			return;
		}
		if (!mHasSwapChainDisplayDesc)
		{
			outReason = "hdr-linear16-await-display-desc";
			return;
		}
		if (!mSwapChainDisplayDesc.isHDR)
		{
			outReason = "hdr-linear16-display-sdr";
			return;
		}
		outResolvedFormat = outRequestedFormat;
		outReason = "hdr-linear16";
		return;

	case NRIPTOutputMode::HDR10PQ:
		outRequestedFormat = nri::SwapChainFormat::BT2020_G2084_10BIT;
		if (GetSelectedAPI() != nri::GraphicsAPI::D3D12)
		{
			outReason = "hdr10-pq-unsupported-api";
			return;
		}
		if (!mHasSwapChainDisplayDesc)
		{
			outReason = "hdr10-pq-await-display-desc";
			return;
		}
		if (!mSwapChainDisplayDesc.isHDR)
		{
			outReason = "hdr10-pq-display-sdr";
			return;
		}
		outResolvedFormat = nri::SwapChainFormat::BT709_G10_16BIT;
		outReason = "hdr10-pq-deferred-linear16";
		return;

	default:
		return;
	}
}

NRIPTOutputPolicy NRIRenderDevice::GetPathTracingOutputPolicy() const
{
	NRIPTOutputPolicy policy = {};
	policy.resolvedMode = GetResolvedPathTracingOutputModeForSwapChainFormat(mCreatedSwapChainFormat);
	policy.hdrSwapChainActive = IsHdrSwapChainFormat(mCreatedSwapChainFormat);
	const bool hdrControlsActive = policy.hdrSwapChainActive;
	policy.requestedMode = GetRequestedPathTracingOutputMode();
	policy.tonemapMode = GetRequestedPathTracingTonemapMode(hdrControlsActive);
	policy.exposure = GetRequestedPathTracingExposure(hdrControlsActive);
	policy.contrast = GetRequestedPathTracingContrast(hdrControlsActive);
	policy.saturation = GetRequestedPathTracingSaturation(hdrControlsActive);
	policy.shoulder = GetRequestedPathTracingShoulder(hdrControlsActive);
	policy.toe = GetRequestedPathTracingToe(hdrControlsActive);
	policy.paperWhiteNits = (float)nri_ptpaperwhite;
	policy.displayInfoAvailable = mHasSwapChainDisplayDesc;
	policy.displayHdrSupported = mHasSwapChainDisplayDesc && mSwapChainDisplayDesc.isHDR;
	policy.displayMaxLuminance = mHasSwapChainDisplayDesc ? mSwapChainDisplayDesc.maxLuminance : 80.0f;
	policy.displaySdrLuminance = mHasSwapChainDisplayDesc ? mSwapChainDisplayDesc.sdrLuminance : 80.0f;
	policy.offscreenHdrTarget = true;

	return policy;
}

void NRIRenderDevice::SyncPathTracingOutputModeCVarWithSwapChainState(const char* evaluatedResolveReason)
{
	if (GetRequestedPathTracingOutputMode() == NRIPTOutputMode::SDR)
	{
		return;
	}

	if (mCreatedSwapChainFormat != nri::SwapChainFormat::BT709_G22_8BIT)
	{
		return;
	}

	const char* reason =
		evaluatedResolveReason != nullptr && *evaluatedResolveReason != '\0' ?
			evaluatedResolveReason :
			mSwapChainOutputResolveReason.GetChars();
	const bool displayForcesSdr = mHasSwapChainDisplayDesc && !mSwapChainDisplayDesc.isHDR;
	if (!displayForcesSdr && !IsHdrRequestDefinitivelyUnavailableReason(reason))
	{
		return;
	}

	Printf("NRI PT output sync: requested=%s created_format=%s display_hdr=%s reason=%s; updating nri_ptoutputmode to SDR.\n",
		GetNRIPTOutputModeName(GetRequestedPathTracingOutputMode()),
		GetSwapChainFormatName(mCreatedSwapChainFormat),
		!mHasSwapChainDisplayDesc ? "unknown" : (mSwapChainDisplayDesc.isHDR ? "yes" : "no"),
		reason != nullptr && *reason != '\0' ? reason : "unknown");
	nri_ptoutputmode = 0;
}

void NRIRenderDevice::PrintPathTracingOutputModeChange(uint32_t frameIndex, NRIPTOutputMode previousRequestedMode, NRIPTOutputMode previousResolvedMode) const
{
	const NRIPTOutputPolicy outputPolicy = GetPathTracingOutputPolicy();
	nri::SwapChainFormat requestedOutputFormat = nri::SwapChainFormat::BT709_G22_8BIT;
	nri::SwapChainFormat resolvedOutputFormat = nri::SwapChainFormat::BT709_G22_8BIT;
	const char* outputResolveReason = "requested-sdr";
	ResolvePathTracingSwapChainOutput(requestedOutputFormat, resolvedOutputFormat, outputResolveReason);

	Printf("NRI PT output policy change: frame=%u api=%s requested_mode=%s->%s resolved_mode=%s->%s requested_format=%s desired_format=%s created_format=%s display_desc=%s display_hdr=%s reason=%s\n",
		frameIndex,
		(const char*)nri_api,
		GetNRIPTOutputModeName(previousRequestedMode),
		GetNRIPTOutputModeName(outputPolicy.requestedMode),
		GetNRIPTOutputModeName(previousResolvedMode),
		GetNRIPTOutputModeName(outputPolicy.resolvedMode),
		GetSwapChainFormatName(requestedOutputFormat),
		GetSwapChainFormatName(resolvedOutputFormat),
		GetSwapChainFormatName(mCreatedSwapChainFormat),
		GetNriResultName(mSwapChainDisplayDescResult),
		outputPolicy.displayHdrSupported ? "yes" : "no",
		outputResolveReason);
}

void NRIRenderDevice::PrintPathTracingBuffers() const
{
	PrintPathTracingCaps();
	PrintFrameBoundaryStatus();
	PrintFrameSequenceStatus();
	PrintSwapChainStatus();
	PrintFrameShellStatus();
	Print2DTextureStatus();
	PrintVramTelemetryStatus();
	if (mRenderer != nullptr)
	{
		mRenderer->PrintSceneBufferStatus();
	}
}

void NRIRenderDevice::PrintPathTracingSurfaceProbeStatus() const
{
	if (mRenderer == nullptr)
	{
		Printf("NRI PT surface probe is unavailable because the renderer is not initialized.\n");
		return;
	}

	mRenderer->PrintSurfaceProbeStatus();
}

bool NRIRenderDevice::BuildPathTracingEmissiveLightEditTarget(PathTracingEmissiveLightEditTarget& outTarget) const
{
	outTarget = {};
	if (mRenderer == nullptr)
	{
		outTarget.failureReason = "renderer is not initialized";
		return false;
	}

	return mRenderer->BuildEmissiveLightEditTarget(outTarget);
}

bool NRIRenderDevice::BuildPathTracingSurfaceLightEditTarget(PathTracingEmissiveLightEditTarget& outTarget) const
{
	outTarget = {};
	if (mRenderer == nullptr)
	{
		outTarget.failureReason = "renderer is not initialized";
		return false;
	}

	return mRenderer->BuildSurfaceLightEditTarget(outTarget);
}

bool NRIRenderDevice::ProjectPathTracingEditorLine(const float renderStart[3], const float renderEnd[3],
	DVector2& outStart, DVector2& outEnd) const
{
	outStart = {};
	outEnd = {};
	if (mRenderer == nullptr)
	{
		return false;
	}

	float projectedStart[2] = {};
	float projectedEnd[2] = {};
	if (!mRenderer->ProjectEditorLineToScreen(renderStart, renderEnd, projectedStart, projectedEnd))
	{
		return false;
	}
	if (mScreenViewport.width <= 0 || mScreenViewport.height <= 0)
	{
		return false;
	}
	const float targetHeight = mActiveTarget != nullptr
		? (float)std::max<uint32_t>(mActiveTarget->height, 1u)
		: (float)mScreenViewport.height;
	const float screenLeft = (float)mScreenViewport.left;
	const float screenTop = targetHeight - (float)mScreenViewport.top - (float)mScreenViewport.height;
	const float logicalScaleX = (float)GetWidth() / (float)mScreenViewport.width;
	const float logicalScaleY = (float)GetHeight() / (float)mScreenViewport.height;
	auto toLogical = [&](const float projected[2])
	{
		return DVector2(
			(projected[0] - screenLeft) * logicalScaleX,
			(projected[1] - screenTop) * logicalScaleY);
	};
	outStart = toLogical(projectedStart);
	outEnd = toLogical(projectedEnd);
	return true;
}

void NRIRenderDevice::EmitPathTracingWeaponLightEvent(const PathTracingWeaponLightEvent& event)
{
	if (mLevelTransitionInProgress || event.eventId.IsEmpty())
	{
		return;
	}

	PathTracingWeaponLightEvent queuedEvent = event;
	queuedEvent.serial = mNextPathTracingWeaponLightEventSerial++;
	mPendingPathTracingWeaponLightEvents.Push(std::move(queuedEvent));
	mPathTracingWeaponLightEventsEnqueuedThisFrame++;
}

void NRIRenderDevice::EmitPathTracingActorSpriteTraceEvent(const PathTracingActorSpriteTraceEvent& event)
{
	if (mLevelTransitionInProgress)
	{
		return;
	}

	if (mRenderer != nullptr)
	{
		mRenderer->TraceActorSpriteEvent(event);
	}
}

void NRIRenderDevice::ConsumePathTracingWeaponLightEvents(TArray<PathTracingWeaponLightEvent>& outEvents)
{
	outEvents.Clear();
	outEvents.Swap(mPendingPathTracingWeaponLightEvents);
}

uint32_t NRIRenderDevice::ClearPendingPathTracingWeaponLightEvents()
{
	const uint32_t clearedCount = (uint32_t)mPendingPathTracingWeaponLightEvents.Size();
	mPendingPathTracingWeaponLightEvents.Clear();
	mPathTracingWeaponLightEventsEnqueuedThisFrame = 0;
	return clearedCount;
}

uint32_t NRIRenderDevice::GetPendingPathTracingWeaponLightEventCount() const
{
	return (uint32_t)mPendingPathTracingWeaponLightEvents.Size();
}

void NRIRenderDevice::PrintPathTracingMapChunkDump(int32_t chunkIndex) const
{
	if (mRenderer == nullptr)
	{
		Printf("NRI PT chunk dump is unavailable because the renderer is not initialized.\n");
		return;
	}

	mRenderer->PrintMapChunkDump(chunkIndex);
}

void NRIRenderDevice::PrintPathTracingMapChunkCompare(int32_t chunkIndex) const
{
	if (mRenderer == nullptr)
	{
		Printf("NRI PT chunk compare is unavailable because the renderer is not initialized.\n");
		return;
	}

	mRenderer->PrintMapChunkCompare(chunkIndex);
}

void NRIRenderDevice::PrintFrameBoundaryStatus() const
{
	const auto& stats = mLastFrameBoundaryStats;
	Printf("NRI PT frame boundary: frame=%llu frame_index=%llu qframe=%u sanity_mode=%s last_frame=%s wait=%2.3f wait_present=%2.3f acquire=%2.3f submit=%2.3f present=%2.3f wait_present_result=%s acquire_result=%s present_result=%s image=%u sem_index=%u submit_fence=%llu\n",
		(unsigned long long)stats.frameNumber,
		(unsigned long long)stats.frameIndex,
		stats.queuedFrameIndex,
		stats.sanityModeEnabled ? "on" : "off",
		stats.sanityFrameUsed ? "clear-only" : "normal",
		stats.waitMs,
		stats.waitForPresentMs,
		stats.acquireMs,
		stats.submitMs,
		stats.presentMs,
		GetNriResultName(stats.waitForPresentResult),
		GetNriResultName(stats.acquireResult),
		GetNriResultName(stats.presentResult),
		stats.swapChainImageIndex,
		stats.acquireSemaphoreIndex,
		(unsigned long long)stats.submittedFenceValue);
}

void NRIRenderDevice::PrintFrameSequenceStatus() const
{
	bool anyValid = false;
	FString line = "NRI PT frame sequence:";
	for (uint32_t i = 0; i < FrameSequenceHistorySize; ++i)
	{
		const uint32_t index = (mFrameSequenceWriteIndex + i) % FrameSequenceHistorySize;
		const FrameSequenceEntry& entry = mFrameSequenceHistory[index];
		if (!entry.valid)
		{
			continue;
		}

		anyValid = true;
		line.AppendFormat(" [f%llu i%llu q%u a%u@s%u -> p%u@s%u fence=%llu pres=%s %s]",
			(unsigned long long)entry.frameNumber,
			(unsigned long long)entry.frameIndex,
			entry.queuedFrameIndex,
			entry.acquiredImageIndex,
			entry.acquireSemaphoreIndex,
			entry.presentedImageIndex,
			entry.releaseSemaphoreIndex,
			(unsigned long long)entry.submittedFenceValue,
			GetNriResultName(entry.presentResult),
			entry.sanityFrameUsed ? "sanity" : "normal");
	}

	if (!anyValid)
	{
		line << " none";
	}

	Printf("%s\n", line.GetChars());
}

void NRIRenderDevice::PrintSwapChainStatus() const
{
	const FString flagText = DescribeSwapChainFlags(mSwapChainFlags);
	const FString acquiredImages = DescribeSwapChainImageMask(mObservedSwapChainAcquireMask, mSwapChainTextureCount);
	const FString presentedImages = DescribeSwapChainImageMask(mObservedSwapChainPresentMask, mSwapChainTextureCount);
	const FString acquireCounts = DescribeSwapChainImageCounts(mSwapChainAcquireCounts);
	const FString presentCounts = DescribeSwapChainImageCounts(mSwapChainPresentCounts);
	const FString abandonCounts = DescribeSwapChainImageCounts(mSwapChainAbandonCounts);
	const NRIPTOutputPolicy outputPolicy = GetPathTracingOutputPolicy();
	const float hdrPaperWhiteScale = GetNRIPTHdrPaperWhiteScale(outputPolicy);
	const float hdrHeadroom = GetNRIPTHdrHeadroomInPaperWhites(outputPolicy);
	const float hdrMaxScale = GetNRIPTHdrMaxOutputScale(outputPolicy);
	const bool hdrUiContractActive = outputPolicy.hdrSwapChainActive;
	Printf("NRI PT swapchain: textures=%u queued_frames=%u vsync=%s flags=%s texture_override=%d flag_override=%s wait_present=%s acquire_seen=%u/%u [%s] present_seen=%u/%u [%s]\n",
		(uint32_t)mSwapChainTextureCount,
		(uint32_t)mSwapChainQueuedFrameNum,
		vid_vsync ? "on" : "off",
		flagText.GetChars(),
		(int)nri_ptswaptextures,
		DescribeSwapChainFlagOverride(),
		nri_ptwaitpresent ? "on" : "off",
		CountSetBits(mObservedSwapChainAcquireMask),
		(uint32_t)mSwapChainTextureCount,
		acquiredImages.GetChars(),
		CountSetBits(mObservedSwapChainPresentMask),
		(uint32_t)mSwapChainTextureCount,
		presentedImages.GetChars());
	Printf("NRI PT swapchain output: requested_mode=%s resolved_mode=%s control_block=%s requested_format=%s created_format=%s resolved_texture_format=%s tonemap=%s exposure=%.3f contrast=%.3f saturation=%.3f shoulder=%.3f toe=%.3f paper_white=%.1f hdr_paper_scale=%.3f hdr_headroom=%.3f hdr_max_scale=%.3f display_info=%s display_hdr=%s display_sdr_nits=%.1f display_max_nits=%.1f display_desc_result=%s reason=%s\n",
		GetNRIPTOutputModeName(outputPolicy.requestedMode),
		GetNRIPTOutputModeName(outputPolicy.resolvedMode),
		GetNRIPTOutputControlBlockName(outputPolicy),
		GetSwapChainFormatName(mRequestedSwapChainFormat),
		GetSwapChainFormatName(mCreatedSwapChainFormat),
		GetNriFormatName(mResolvedSwapChainTextureFormat),
		GetNRIPTTonemapModeName(outputPolicy.tonemapMode),
		outputPolicy.exposure,
		outputPolicy.contrast,
		outputPolicy.saturation,
		outputPolicy.shoulder,
		outputPolicy.toe,
		outputPolicy.paperWhiteNits,
		hdrPaperWhiteScale,
		hdrHeadroom,
		hdrMaxScale,
		mHasSwapChainDisplayDesc ? "yes" : "no",
		outputPolicy.displayHdrSupported ? "yes" : "no",
		outputPolicy.displaySdrLuminance,
		outputPolicy.displayMaxLuminance,
		GetNriResultName(mSwapChainDisplayDescResult),
		mSwapChainOutputResolveReason.IsEmpty() ? "unknown" : mSwapChainOutputResolveReason.GetChars());
	Printf("NRI PT ui output: present_contract=%s gamma=%.3f hdr_scale=%.3f active_target=%s framegen_ui_target=%s\n",
		hdrUiContractActive ? "hdr-paperwhite" : "sdr-direct",
		hdrUiContractActive ? 2.2f : 1.0f,
		hdrUiContractActive ? hdrPaperWhiteScale : 1.0f,
		mActiveTarget == mCurrentPresentTarget ? "present" :
			(mActiveTarget == &mSceneTarget ? "scene" : (mActiveTarget == &mSaveTarget ? "save" : "other")),
		mFrameGenerationUiTargetActive ? "yes" : "no");
	Printf("NRI PT swapchain counts: acquire=[%s] present=[%s] abandoned=[%s]\n",
		acquireCounts.GetChars(),
		presentCounts.GetChars(),
		abandonCounts.GetChars());
}

const char* NRIRenderDevice::DescribeTextureTarget(const NRITextureResource* target) const
{
	if (target == nullptr)
	{
		return "null";
	}

	if (target == mCurrentPresentTarget)
	{
		return "present";
	}

	if (target == &mSceneTarget)
	{
		return "scene";
	}

	if (target == &mSaveTarget)
	{
		return "save";
	}

	return "other";
}

void NRIRenderDevice::PrintFrameShellStatus() const
{
	const auto& stats = mLastFrameBoundaryStats;
	const NRITextureResource* activeTarget = mActiveTarget != nullptr ? mActiveTarget : mCurrentPresentTarget;
	Printf("NRI PT frame shell: active=%s present=%s frame_begun=%s cmd_open=%s scene_selected=%s pt_rendered=%s postprocess=%s scene_copy=%s active_state=(a=%u l=%u s=0x%x) present_state=(a=%u l=%u s=0x%x) scene_state=(a=%u l=%u s=0x%x)\n",
		DescribeTextureTarget(activeTarget),
		DescribeTextureTarget(mCurrentPresentTarget),
		mFrameBegun ? "yes" : "no",
		mCommandBufferOpen ? "yes" : "no",
		stats.sceneTargetSelected ? "yes" : "no",
		stats.pathTracedSceneRendered ? "yes" : "no",
		stats.postProcessInvoked ? "yes" : "no",
		stats.sceneCopiedToPresent ? "yes" : "no",
		activeTarget != nullptr ? (uint32_t)activeTarget->state.access : 0u,
		activeTarget != nullptr ? (uint32_t)activeTarget->state.layout : 0u,
		activeTarget != nullptr ? (uint32_t)activeTarget->state.stages : 0u,
		mCurrentPresentTarget != nullptr ? (uint32_t)mCurrentPresentTarget->state.access : 0u,
		mCurrentPresentTarget != nullptr ? (uint32_t)mCurrentPresentTarget->state.layout : 0u,
		mCurrentPresentTarget != nullptr ? (uint32_t)mCurrentPresentTarget->state.stages : 0u,
		mSceneTarget.texture != nullptr ? (uint32_t)mSceneTarget.state.access : 0u,
		mSceneTarget.texture != nullptr ? (uint32_t)mSceneTarget.state.layout : 0u,
		mSceneTarget.texture != nullptr ? (uint32_t)mSceneTarget.state.stages : 0u);
}

void NRIRenderDevice::RecordFrameSequence(uint32_t releaseSemaphoreIndex, uint64_t submittedFenceValue, nri::Result presentResult)
{
	FrameSequenceEntry& entry = mFrameSequenceHistory[mFrameSequenceWriteIndex];
	entry = {};
	entry.frameNumber = mLastFrameBoundaryStats.frameNumber;
	entry.frameIndex = mLastFrameBoundaryStats.frameIndex;
	entry.submittedFenceValue = submittedFenceValue;
	entry.queuedFrameIndex = mLastFrameBoundaryStats.queuedFrameIndex;
	entry.acquiredImageIndex = mLastFrameBoundaryStats.swapChainImageIndex;
	entry.acquireSemaphoreIndex = mLastFrameBoundaryStats.acquireSemaphoreIndex;
	entry.presentedImageIndex = mCurrentSwapChainImage;
	entry.releaseSemaphoreIndex = releaseSemaphoreIndex;
	entry.presentResult = presentResult;
	entry.sanityFrameUsed = mLastFrameBoundaryStats.sanityFrameUsed;
	entry.valid = true;
	mFrameSequenceWriteIndex = (mFrameSequenceWriteIndex + 1) % FrameSequenceHistorySize;
}

void NRIRenderDevice::Print2DTextureStatus() const
{
	const auto& stats = mTexture2DDebugStats;
	Printf("NRI 2D textures: frame=%llu ensure=%u canvas=%u hits=%u misses=%u uploads=%u failures=%u create=%u recreate=%u bytes=%llu total_bytes=%llu resident_bytes=%llu peak_resident=%llu\n",
		(unsigned long long)stats.frameNumber,
		stats.ensureCalls,
		stats.canvasEnsures,
		stats.cacheHits,
		stats.cacheMisses,
		stats.uploadAttempts,
		stats.uploadFailures,
		stats.resourceCreates,
		stats.resourceRecreates,
		(unsigned long long)stats.uploadedBytes,
		(unsigned long long)stats.totalUploadedBytes,
		(unsigned long long)stats.residentBytes,
		(unsigned long long)stats.peakResidentBytes);
	Printf("NRI 2D totals: ensures=%llu canvas=%llu hits=%llu misses=%llu uploads=%llu failures=%llu create=%llu recreate=%llu\n",
		(unsigned long long)stats.totalEnsureCalls,
		(unsigned long long)stats.totalCanvasEnsures,
		(unsigned long long)stats.totalCacheHits,
		(unsigned long long)stats.totalCacheMisses,
		(unsigned long long)stats.totalUploadAttempts,
		(unsigned long long)stats.totalUploadFailures,
		(unsigned long long)stats.totalResourceCreates,
		(unsigned long long)stats.totalResourceRecreates);
}

NRIAdapterMemoryTelemetry NRIRenderDevice::GetAdapterMemoryTelemetry() const
{
	NRIAdapterMemoryTelemetry telemetry = {};
	telemetry.localBudgetBytes = mAdapterLocalBudgetBytes;
	telemetry.nonLocalBudgetBytes = mAdapterNonLocalBudgetBytes;
#ifdef _WIN32
	if (GetLiveAPI() == nri::GraphicsAPI::D3D12 && mNativeD3D12Device != nullptr)
	{
		IDXGIFactory4* factory = nullptr;
		if (SUCCEEDED(CreateDxgiFactoryForTelemetry(&factory)) && factory != nullptr)
		{
			IDXGIAdapter3* adapter = nullptr;
			const LUID adapterLuid = mNativeD3D12Device->GetAdapterLuid();
			if (SUCCEEDED(factory->EnumAdapterByLuid(adapterLuid, IID_PPV_ARGS(&adapter))) && adapter != nullptr)
			{
				DXGI_QUERY_VIDEO_MEMORY_INFO localInfo = {};
				DXGI_QUERY_VIDEO_MEMORY_INFO nonLocalInfo = {};
				if (SUCCEEDED(adapter->QueryVideoMemoryInfo(0u, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &localInfo)) &&
					SUCCEEDED(adapter->QueryVideoMemoryInfo(0u, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocalInfo)))
				{
					telemetry.localBudgetBytes = localInfo.Budget;
					telemetry.localUsageBytes = localInfo.CurrentUsage;
					telemetry.nonLocalBudgetBytes = nonLocalInfo.Budget;
					telemetry.nonLocalUsageBytes = nonLocalInfo.CurrentUsage;
					telemetry.liveUsageAvailable = true;
				}
				adapter->Release();
			}
			factory->Release();
		}
	}
#endif
	return telemetry;
}

void NRIRenderDevice::PrintVramTelemetryStatus() const
{
	const NRIRenderer::MemoryTelemetry rendererMemory = mRenderer != nullptr ? mRenderer->GetMemoryTelemetry() : NRIRenderer::MemoryTelemetry{};
	const uint64_t shellTextureBytes =
		mSceneTarget.memorySize +
		mSaveTarget.memorySize;
	const uint64_t trackedLocalUsageBytes =
		rendererMemory.totalTrackedBytes +
		shellTextureBytes +
		mTexture2DDebugStats.residentBytes;
	const uint64_t trackedNonLocalUsageBytes = 0;
	double localPressurePct =
		mAdapterLocalBudgetBytes > 0 ?
		(100.0 * (double)trackedLocalUsageBytes / (double)mAdapterLocalBudgetBytes) : 0.0;
	const bool is4KMode =
		rendererMemory.outputWidth >= 3840 ||
		rendererMemory.outputHeight >= 2160 ||
		rendererMemory.renderWidth >= 3840 ||
		rendererMemory.renderHeight >= 2160;
	const NRIAdapterMemoryTelemetry adapterMemory = GetAdapterMemoryTelemetry();
	const uint64_t liveLocalBudgetBytes = adapterMemory.localBudgetBytes;
	const uint64_t liveLocalUsageBytes = adapterMemory.localUsageBytes;
	const uint64_t liveNonLocalBudgetBytes = adapterMemory.nonLocalBudgetBytes;
	const uint64_t liveNonLocalUsageBytes = adapterMemory.nonLocalUsageBytes;
	const bool hasLiveDxgiTelemetry = adapterMemory.liveUsageAvailable;

	if (hasLiveDxgiTelemetry && liveLocalBudgetBytes > 0)
	{
		localPressurePct = 100.0 * (double)liveLocalUsageBytes / (double)liveLocalBudgetBytes;
	}

	Printf("NRI PT vram budget: local=%llu nonlocal=%llu tracked_local=%llu tracked_nonlocal=%llu local_pressure=%.1f%%\n",
		(unsigned long long)mAdapterLocalBudgetBytes,
		(unsigned long long)mAdapterNonLocalBudgetBytes,
		(unsigned long long)trackedLocalUsageBytes,
		(unsigned long long)trackedNonLocalUsageBytes,
		localPressurePct);
	Printf("NRI PT vram families: frame=%llu scene=%llu sky=%llu shell=%llu tex2d=%llu buffers=%llu accel=%llu total=%llu\n",
		(unsigned long long)rendererMemory.frameTextureBytes,
		(unsigned long long)rendererMemory.sceneTextureBytes,
		(unsigned long long)rendererMemory.skyTextureBytes,
		(unsigned long long)shellTextureBytes,
		(unsigned long long)mTexture2DDebugStats.residentBytes,
		(unsigned long long)rendererMemory.sceneBufferBytes,
		(unsigned long long)rendererMemory.accelerationStructureBytes,
		(unsigned long long)trackedLocalUsageBytes);
	if (hasLiveDxgiTelemetry)
	{
		Printf("NRI PT vram live: source=dxgi local_usage=%llu local_budget=%llu nonlocal_usage=%llu nonlocal_budget=%llu\n",
			(unsigned long long)liveLocalUsageBytes,
			(unsigned long long)liveLocalBudgetBytes,
			(unsigned long long)liveNonLocalUsageBytes,
			(unsigned long long)liveNonLocalBudgetBytes);
	}
	else
	{
		Printf("NRI PT vram live: unavailable source=%s\n",
			GetLiveAPI() == nri::GraphicsAPI::D3D12 ? "dxgi-query-failed" : "unsupported-api");
	}
	Printf("NRI PT vram wrapped: swapchain=%u framegen_present=%u tracked_bytes=unknown\n",
		(uint32_t)mSwapChainImages.size(),
		(uint32_t)mFrameGenerationPresentImages.size());

	if (is4KMode && localPressurePct >= 80.0)
	{
		Printf(TEXTCOLOR_ORANGE "NRI PT vram warning: 4K local usage is at %.1f%% of budget.\n", localPressurePct);
	}
}

void NRIRenderDevice::Reset2DTextureFrameStats()
{
	mTexture2DDebugStats.frameNumber++;
	mTexture2DDebugStats.ensureCalls = 0;
	mTexture2DDebugStats.canvasEnsures = 0;
	mTexture2DDebugStats.cacheHits = 0;
	mTexture2DDebugStats.cacheMisses = 0;
	mTexture2DDebugStats.uploadAttempts = 0;
	mTexture2DDebugStats.uploadFailures = 0;
	mTexture2DDebugStats.resourceCreates = 0;
	mTexture2DDebugStats.resourceRecreates = 0;
	mTexture2DDebugStats.uploadedBytes = 0;
}

void NRIRenderDevice::Note2DTextureEnsure(bool canvas)
{
	mTexture2DDebugStats.ensureCalls++;
	mTexture2DDebugStats.totalEnsureCalls++;
	if (canvas)
	{
		mTexture2DDebugStats.canvasEnsures++;
		mTexture2DDebugStats.totalCanvasEnsures++;
	}
}

void NRIRenderDevice::Note2DTextureCacheHit()
{
	mTexture2DDebugStats.cacheHits++;
	mTexture2DDebugStats.totalCacheHits++;
}

void NRIRenderDevice::Note2DTextureCacheMiss()
{
	mTexture2DDebugStats.cacheMisses++;
	mTexture2DDebugStats.totalCacheMisses++;
}

void NRIRenderDevice::Note2DTextureUploadAttempt(uint64_t bytes, bool success)
{
	mTexture2DDebugStats.uploadAttempts++;
	mTexture2DDebugStats.totalUploadAttempts++;
	mTexture2DDebugStats.uploadedBytes += bytes;
	mTexture2DDebugStats.totalUploadedBytes += bytes;
	if (!success)
	{
		mTexture2DDebugStats.uploadFailures++;
		mTexture2DDebugStats.totalUploadFailures++;
	}
}

void NRIRenderDevice::Note2DTextureResourceCreate(bool recreated)
{
	if (recreated)
	{
		mTexture2DDebugStats.resourceRecreates++;
		mTexture2DDebugStats.totalResourceRecreates++;
	}
	else
	{
		mTexture2DDebugStats.resourceCreates++;
		mTexture2DDebugStats.totalResourceCreates++;
	}
}

void NRIRenderDevice::Note2DTextureResidentBytesChanged(uint64_t oldBytes, uint64_t newBytes)
{
	if (newBytes >= oldBytes)
	{
		mTexture2DDebugStats.residentBytes += newBytes - oldBytes;
	}
	else
	{
		mTexture2DDebugStats.residentBytes -= oldBytes - newBytes;
	}

	mTexture2DDebugStats.peakResidentBytes = (std::max)(mTexture2DDebugStats.peakResidentBytes, mTexture2DDebugStats.residentBytes);
}

void NRIRenderDevice::NoteSwapChainAcquire(uint32_t imageIndex)
{
	if (imageIndex < 64)
	{
		mObservedSwapChainAcquireMask |= (1ull << imageIndex);
	}

	if (imageIndex < mSwapChainAcquireCounts.size())
	{
		mSwapChainAcquireCounts[imageIndex]++;
	}
}

void NRIRenderDevice::NoteSwapChainPresent(uint32_t imageIndex)
{
	if (imageIndex < 64)
	{
		mObservedSwapChainPresentMask |= (1ull << imageIndex);
	}

	if (imageIndex < mSwapChainPresentCounts.size())
	{
		mSwapChainPresentCounts[imageIndex]++;
	}
}

void NRIRenderDevice::NoteSwapChainAbandon(uint32_t imageIndex)
{
	if (imageIndex < mSwapChainAbandonCounts.size())
	{
		mSwapChainAbandonCounts[imageIndex]++;
	}
}

void NRIRenderDevice::ResetPathTracingHistory()
{
	if (mRenderer == nullptr)
	{
		Printf("NRI PT history reset is unavailable because the renderer is not initialized.\n");
		return;
	}

	mRenderer->ResetHistory();
	Printf("NRI PT history reset requested.\n");
}

void NRIRenderDevice::ResetPathTracingAutoExposure()
{
	if (mRenderer == nullptr)
	{
		Printf("NRI PT auto exposure reset is unavailable because the renderer is not initialized.\n");
		return;
	}

	mRenderer->RequestAutoExposureReset("console");
	Printf("NRI PT auto exposure reset requested.\n");
}

void NRIRenderDevice::PrintPathTracingSmokeStatus() const
{
	if (mRenderer != nullptr)
	{
		mRenderer->PrintSmokeStatus();
	}
}

void NRIRenderDevice::ResetPathTracingSmoke()
{
	if (mRenderer != nullptr)
	{
		mRenderer->ResetSmoke("console");
	}
}

void NRIRenderDevice::QueueSyntheticPathTracingSmoke()
{
	if (mRenderer != nullptr)
	{
		mRenderer->QueueSyntheticSmoke();
	}
}

void NRIRenderDevice::NotifyPathTracingCameraCut(const char* reason)
{
	if (mRenderer != nullptr)
	{
		mRenderer->NotifyCameraCut(reason);
	}
}

void NRIRenderDevice::SetPathTracingGuiCaptureState(bool active)
{
	mPathTracingGuiCaptureActive = active;
	if (mRenderer != nullptr)
	{
		mRenderer->SetGuiCaptureState(active);
	}
}

bool NRIRenderDevice::SetPathTracingEditorPointLight(const DVector3& worldPosition, const float color[3], float intensity, float radius)
{
	if (mRenderer == nullptr || color == nullptr || intensity <= 0.0f || radius <= 0.0f)
	{
		return false;
	}

	if (!mRenderer->RefreshPathTracingAvailability())
	{
		return false;
	}

	float renderPosition[3] = {};
	WorldToPathTracingPosition(worldPosition, renderPosition);
	if (mPathTracingEditorPointLightActive &&
		mRenderer->UpdateRuntimePointLight(mPathTracingEditorPointLightId, renderPosition, color, intensity, radius))
	{
		return true;
	}

	uint32_t lightId = 0;
	if (!mRenderer->AddRuntimePointLight(renderPosition, color, intensity, radius, lightId))
	{
		mPathTracingEditorPointLightActive = false;
		mPathTracingEditorPointLightId = 0;
		return false;
	}

	mPathTracingEditorPointLightActive = true;
	mPathTracingEditorPointLightId = lightId;
	return true;
}

void NRIRenderDevice::ClearPathTracingEditorPointLight()
{
	if (!mPathTracingEditorPointLightActive)
	{
		return;
	}

	if (mRenderer != nullptr)
	{
		mRenderer->RemoveRuntimePointLight(mPathTracingEditorPointLightId);
	}
	mPathTracingEditorPointLightActive = false;
	mPathTracingEditorPointLightId = 0;
}

bool NRIRenderDevice::SpawnPathTracingPointLight(float red, float green, float blue, float intensity, float radius, float offset, uint32_t& outId)
{
	if (mRenderer == nullptr)
	{
		Printf("NRI PT test lights are unavailable because the renderer is not initialized.\n");
		return false;
	}

	if (!mRenderer->RefreshPathTracingAvailability())
	{
		Printf("NRI PT test lights are unavailable because path tracing is not active (%s).\n", mRenderer->GetAvailabilityReason());
		return false;
	}

	if (netgame)
	{
		Printf("nri_ptlightspawn cannot be used in multiplayer.\n");
		return false;
	}

	if (gamestate != GS_LEVEL)
	{
		Printf("nri_ptlightspawn: must be in a level.\n");
		return false;
	}

	DCorePlayer* player = PlayerArray[myconnectindex];
	if (player == nullptr)
	{
		Printf("nri_ptlightspawn: no local player is available.\n");
		return false;
	}

	DCoreActor* actor = player->GetActor();
	if (actor == nullptr)
	{
		Printf("nri_ptlightspawn: local player actor is unavailable.\n");
		return false;
	}

	if (intensity <= 0.0f)
	{
		Printf("nri_ptlightspawn: intensity must be > 0.\n");
		return false;
	}

	if (radius <= 0.0f)
	{
		Printf("nri_ptlightspawn: radius must be > 0.\n");
		return false;
	}

	if (offset < 0.0f)
	{
		Printf("nri_ptlightspawn: offset must be >= 0.\n");
		return false;
	}

	const DRotator viewRotation(
		player->getPitchWithView(),
		actor->spr.Angles.Yaw + player->ViewAngles.Yaw,
		actor->spr.Angles.Roll + player->ViewAngles.Roll);
	const DVector3 forward(viewRotation);
	const DVector3 spawnPosition = actor->getPosWithOffsetZ() + forward * offset;
	float renderPosition[3] = {};
	WorldToPathTracingPosition(spawnPosition, renderPosition);
	const float lightColor[3] = {
		red < 0.0f ? 0.0f : red,
		green < 0.0f ? 0.0f : green,
		blue < 0.0f ? 0.0f : blue,
	};
	if (!mRenderer->AddRuntimePointLight(renderPosition, lightColor, intensity, radius, outId))
	{
		Printf("nri_ptlightspawn: failed to add PT test light. active=%u limit=64\n", mRenderer->GetRuntimePointLightCount());
		return false;
	}

	Printf("NRI PT test light spawned: id=%u world_pos=(%.3f, %.3f, %.3f) render_pos=(%.3f, %.3f, %.3f) color=(%.3f, %.3f, %.3f) intensity=%.3f radius=%.3f offset=%.3f\n",
		outId,
		spawnPosition.X,
		spawnPosition.Y,
		spawnPosition.Z,
		renderPosition[0],
		renderPosition[1],
		renderPosition[2],
		lightColor[0],
		lightColor[1],
		lightColor[2],
		intensity,
		radius,
		offset);
	return true;
}

bool NRIRenderDevice::RemovePathTracingPointLight(uint32_t id)
{
	if (mRenderer == nullptr)
	{
		Printf("NRI PT test lights are unavailable because the renderer is not initialized.\n");
		return false;
	}

	if (!mRenderer->RemoveRuntimePointLight(id))
	{
		Printf("nri_ptlightremove: no PT test light with id=%u.\n", id);
		return false;
	}

	Printf("NRI PT test light removed: id=%u remaining=%u\n", id, mRenderer->GetRuntimePointLightCount());
	return true;
}

void NRIRenderDevice::ClearPathTracingPointLights()
{
	if (mRenderer == nullptr)
	{
		Printf("NRI PT test lights are unavailable because the renderer is not initialized.\n");
		return;
	}

	const uint32_t clearedCount = mRenderer->GetRuntimePointLightCount();
	mRenderer->ClearRuntimePointLights();
	Printf("NRI PT test lights cleared: count=%u\n", clearedCount);
}

void NRIRenderDevice::PrintPathTracingPointLights() const
{
	if (mRenderer == nullptr)
	{
		Printf("NRI PT test lights are unavailable because the renderer is not initialized.\n");
		return;
	}

	mRenderer->PrintRuntimePointLights();
}

bool NRIRenderDevice::SpawnPathTracingDebugSphere(float diameter, float distance, float metalness, float roughness, uint32_t& outId)
{
	if (mRenderer == nullptr)
	{
		Printf("NRI PT debug spheres are unavailable because the renderer is not initialized.\n");
		return false;
	}

	if (!mRenderer->RefreshPathTracingAvailability())
	{
		Printf("NRI PT debug spheres are unavailable because path tracing is not active (%s).\n", mRenderer->GetAvailabilityReason());
		return false;
	}

	if (netgame)
	{
		Printf("nri_ptsphere cannot be used in multiplayer.\n");
		return false;
	}

	if (gamestate != GS_LEVEL)
	{
		Printf("nri_ptsphere: must be in a level.\n");
		return false;
	}

	DCorePlayer* player = PlayerArray[myconnectindex];
	if (player == nullptr)
	{
		Printf("nri_ptsphere: no local player is available.\n");
		return false;
	}

	DCoreActor* actor = player->GetActor();
	if (actor == nullptr)
	{
		Printf("nri_ptsphere: local player actor is unavailable.\n");
		return false;
	}

	if (diameter <= 0.0f)
	{
		Printf("nri_ptsphere: diameter must be > 0.\n");
		return false;
	}

	if (distance < 0.0f)
	{
		Printf("nri_ptsphere: distance must be >= 0.\n");
		return false;
	}

	const float clampedMetalness = clamp(metalness, 0.0f, 1.0f);
	const float clampedRoughness = clamp(roughness, 0.0f, 1.0f);
	const DRotator viewRotation(
		player->getPitchWithView(),
		actor->spr.Angles.Yaw + player->ViewAngles.Yaw,
		actor->spr.Angles.Roll + player->ViewAngles.Roll);
	const DVector3 forward(viewRotation);
	const DVector3 spawnPosition = actor->getPosWithOffsetZ() + forward * distance;
	float renderPosition[3] = {};
	WorldToPathTracingPosition(spawnPosition, renderPosition);
	if (!mRenderer->AddRuntimeDebugSphere(renderPosition, diameter, clampedMetalness, clampedRoughness, outId))
	{
		Printf("nri_ptsphere: failed to add PT debug sphere. active=%u limit=64\n", mRenderer->GetRuntimeDebugSphereCount());
		return false;
	}

	Printf("NRI PT debug sphere spawned: id=%u world_pos=(%.3f, %.3f, %.3f) render_pos=(%.3f, %.3f, %.3f) diameter=%.3f metalness=%.3f roughness=%.3f distance=%.3f\n",
		outId,
		spawnPosition.X,
		spawnPosition.Y,
		spawnPosition.Z,
		renderPosition[0],
		renderPosition[1],
		renderPosition[2],
		diameter,
		clampedMetalness,
		clampedRoughness,
		distance);
	return true;
}

bool NRIRenderDevice::RemovePathTracingDebugSphere(uint32_t id)
{
	if (mRenderer == nullptr)
	{
		Printf("NRI PT debug spheres are unavailable because the renderer is not initialized.\n");
		return false;
	}

	if (!mRenderer->RemoveRuntimeDebugSphere(id))
	{
		Printf("nri_ptsphereremove: no PT debug sphere with id=%u.\n", id);
		return false;
	}

	Printf("NRI PT debug sphere removed: id=%u remaining=%u\n", id, mRenderer->GetRuntimeDebugSphereCount());
	return true;
}

void NRIRenderDevice::ClearPathTracingDebugSpheres()
{
	if (mRenderer == nullptr)
	{
		Printf("NRI PT debug spheres are unavailable because the renderer is not initialized.\n");
		return;
	}

	const uint32_t clearedCount = mRenderer->GetRuntimeDebugSphereCount();
	mRenderer->ClearRuntimeDebugSpheres();
	Printf("NRI PT debug spheres cleared: count=%u\n", clearedCount);
}

void NRIRenderDevice::PrintPathTracingDebugSpheres() const
{
	if (mRenderer == nullptr)
	{
		Printf("NRI PT debug spheres are unavailable because the renderer is not initialized.\n");
		return;
	}

	mRenderer->PrintRuntimeDebugSpheres();
}

bool NRIRenderDevice::AddPathTracingSpriteTileLightHeuristic(uint32_t textureId, float red, float green, float blue, float intensity, float radius, uint32_t flickerFrames, uint32_t& outRuleId)
{
	if (mRenderer == nullptr)
	{
		Printf("NRI PT analytic light heuristics are unavailable because the renderer is not initialized.\n");
		return false;
	}

	if (intensity <= 0.0f)
	{
		Printf("nri_ptlightheuristic_addsprite: intensity must be > 0.\n");
		return false;
	}

	if (radius <= 0.0f)
	{
		Printf("nri_ptlightheuristic_addsprite: radius must be > 0.\n");
		return false;
	}

	const float lightColor[3] = {
		red < 0.0f ? 0.0f : red,
		green < 0.0f ? 0.0f : green,
		blue < 0.0f ? 0.0f : blue,
	};
	if (!mRenderer->AddSpriteTileLightHeuristic(textureId, lightColor, intensity, radius, flickerFrames, outRuleId))
	{
		Printf("nri_ptlightheuristic_addsprite: failed to add analytic light heuristic for tile=%u.\n", textureId);
		return false;
	}

	Printf("NRI PT analytic light heuristic added: rule=%u tile=%u color=(%.3f, %.3f, %.3f) intensity=%.3f radius=%.3f flicker_frames=%u\n",
		outRuleId,
		textureId,
		lightColor[0],
		lightColor[1],
		lightColor[2],
		intensity,
		radius,
		flickerFrames);
	return true;
}

void NRIRenderDevice::ClearPathTracingLightHeuristics()
{
	if (mRenderer == nullptr)
	{
		Printf("NRI PT analytic light heuristics are unavailable because the renderer is not initialized.\n");
		return;
	}

	mRenderer->ClearSpriteTileLightHeuristics();
	Printf("NRI PT analytic light heuristics cleared.\n");
}

void NRIRenderDevice::PrintPathTracingLightHeuristics() const
{
	if (mRenderer == nullptr)
	{
		Printf("NRI PT analytic light heuristics are unavailable because the renderer is not initialized.\n");
		return;
	}

	mRenderer->PrintSpriteTileLightHeuristics();
}

void NRIRenderDevice::PrintPathTracingSceneLightDump(float radius, uint32_t limit) const
{
	if (mRenderer == nullptr)
	{
		Printf("NRI PT scene-light dump is unavailable because the renderer is not initialized.\n");
		return;
	}

	mRenderer->PrintSceneLightDump(radius, limit);
}

void NRIRenderDevice::PrintPathTracingLightClusters() const
{
	if (mRenderer == nullptr)
	{
		Printf("NRI PT light-cluster debug is unavailable because the renderer is not initialized.\n");
		return;
	}

	mRenderer->PrintRuntimeLightClusterStatus();
}

bool NRIRenderDevice::AddPathTracingTextureEmissiveHeuristic(uint32_t textureId, uint32_t emissiveMode, float intensityScale, const float* emissiveColor, bool hasExplicitColor, uint32_t& outRuleId)
{
	if (mRenderer == nullptr)
	{
		Printf("NRI PT emissive heuristics are unavailable because the renderer is not initialized.\n");
		return false;
	}

	if (!mRenderer->AddTextureEmissiveHeuristic(textureId, emissiveMode, intensityScale, emissiveColor, hasExplicitColor, outRuleId))
	{
		Printf("NRI PT emissive heuristic add failed: tile=%u mode=%u intensity_scale=%.3f\n", textureId, emissiveMode, intensityScale);
		return false;
	}

	Printf("NRI PT emissive heuristic %u added: tile=%u mode=%u intensity_scale=%.3f explicit_color=%s\n",
		outRuleId,
		textureId,
		emissiveMode,
		intensityScale,
		hasExplicitColor ? "yes" : "no");
	return true;
}

void NRIRenderDevice::ClearPathTracingEmissiveHeuristics()
{
	if (mRenderer == nullptr)
	{
		Printf("NRI PT emissive heuristics are unavailable because the renderer is not initialized.\n");
		return;
	}

	mRenderer->ClearTextureEmissiveHeuristics();
	Printf("NRI PT emissive heuristics cleared.\n");
}

void NRIRenderDevice::PrintPathTracingEmissiveHeuristics() const
{
	if (mRenderer == nullptr)
	{
		Printf("NRI PT emissive heuristics are unavailable because the renderer is not initialized.\n");
		return;
	}

	mRenderer->PrintTextureEmissiveHeuristics();
}

void NRIRenderDevice::NotifyPathTracingGlowControlChange()
{
	if (mRenderer == nullptr)
	{
		return;
	}

	mRenderer->NotifyGlowControlChange();
}

void NRIRenderDevice::NotifyPathTracingMaterialLightingCalibrationChange()
{
	if (mRenderer == nullptr)
	{
		return;
	}

	mRenderer->NotifyMaterialLightingCalibrationChange();
}

void NRIRenderDevice::NotifyPathTracingAnalyticLightSettingsChange()
{
	if (mRenderer == nullptr)
	{
		return;
	}

	mRenderer->NotifyAnalyticLightSettingsChange();
}

void NRIRenderDevice::NotifyPathTracingDebugSphereTessellationChange()
{
	if (mRenderer == nullptr)
	{
		return;
	}

	mRenderer->NotifyDebugSphereTessellationChange();
}

void NRIRenderDevice::PrintPathTracingEmissiveSurfaces(float radius, uint32_t limit) const
{
	if (mRenderer == nullptr)
	{
		Printf("NRI PT emissive-surface dump is unavailable because the renderer is not initialized.\n");
		return;
	}

	mRenderer->PrintEmissiveSurfaceDump(radius, limit);
}

void NRIRenderDevice::PrintPathTracingSectorLights(float radius, uint32_t limit) const
{
	if (mRenderer == nullptr)
	{
		Printf("NRI PT sector-light dump is unavailable because the renderer is not initialized.\n");
		return;
	}

	mRenderer->PrintSectorLightDump(radius, limit);
}

void NRIRenderDevice::LogStartup()
{
	if (mLoggedStartup || mDevice == nullptr)
	{
		return;
	}

	const nri::DeviceDesc& deviceDesc = mCore.GetDeviceDesc(*mDevice);
	const char* startupApi = V_GetStartupNriAPI();
	mDeviceName = FStringf("NRI (%s) - %s", startupApi, deviceDesc.adapterDesc.name);
	vendorstring = mDeviceName.GetChars();

	Printf("NRI device: " TEXTCOLOR_ORANGE "%s\n", deviceDesc.adapterDesc.name);
	Printf("NRI graphics API: %s\n", startupApi);
	Printf("Max. texture size: %u\n", deviceDesc.dimensions.texture2DMaxDim);
	Printf("Root constant limit: %u\n", deviceDesc.pipelineLayout.rootConstantMaxSize);
	Printf("Shader model: %u.%u\n", deviceDesc.shaderModel / 10, deviceDesc.shaderModel % 10);
	Printf("Ray tracing tier: %u\n", deviceDesc.tiers.rayTracing);
	Printf("NRI queued frames: %u\n", (uint32_t)mQueuedFrames.size());
	Printf("NRI swapchain policy: textures=%u queued_frames=%u vsync=%s flags=%s\n",
		(uint32_t)mSwapChainTextureCount,
		(uint32_t)mSwapChainQueuedFrameNum,
		vid_vsync ? "on" : "off",
		DescribeSwapChainFlags(mSwapChainFlags).GetChars());
	Printf("Upscaler support: NIS=%s DLSS-SR=%s DLRR=%s\n",
		mUpscaler.IsUpscalerSupported(*mDevice, nri::UpscalerType::NIS) ? "yes" : "no",
		mUpscaler.IsUpscalerSupported(*mDevice, nri::UpscalerType::DLSR) ? "yes" : "no",
		mUpscaler.IsUpscalerSupported(*mDevice, nri::UpscalerType::DLRR) ? "yes" : "no");
	const auto& frameGenPolicy = mFrameGeneration.GetPolicy();
	Printf("Frame generation policy: requested=%s provider=%s resolved=%s api=%s shader_model=%u.%u window=%s low_latency=%s->%s(avail=%s iface=%s swapchain=%s) async=%s->%s(avail=%s) ui=%s->%s(route=%s) swapchain=%s native=device:%s queue:%s swapchain:%s waitable=%s runtime=%s reason=%s\n",
		frameGenPolicy.requestedEnabled ? "on" : "off",
		NRIFrameGenerationContext::GetProviderName(frameGenPolicy.requestedProvider),
		NRIFrameGenerationContext::GetProviderName(frameGenPolicy.resolvedProvider),
		frameGenPolicy.selectedApiName,
		frameGenPolicy.shaderModel / 10u,
		frameGenPolicy.shaderModel % 10u,
		NRIFrameGenerationContext::GetWindowModeName(frameGenPolicy.fullscreenActive),
		frameGenPolicy.requestedLowLatency ? "on" : "off",
		frameGenPolicy.resolvedLowLatency ? "on" : "off",
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.lowLatencyAvailable),
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.lowLatencyInterfaceAvailable),
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.lowLatencySwapChainEnabled),
		frameGenPolicy.requestedAsync ? "on" : "off",
		frameGenPolicy.resolvedAsync ? "on" : "off",
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.asyncWorkloadAvailable),
		NRIFrameGenerationContext::GetUiModeName(frameGenPolicy.requestedUiMode),
		NRIFrameGenerationContext::GetUiModeName(frameGenPolicy.resolvedUiMode),
		GetFrameGenerationUiRouteName(),
		frameGenPolicy.swapChainReady ? "ready" : "cold",
		frameGenPolicy.nativeDeviceAvailable ? "ok" : "missing",
		frameGenPolicy.nativeGraphicsQueueAvailable ? "ok" : "missing",
		frameGenPolicy.nativeSwapChainAvailable ? "ok" : "missing",
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.waitableSwapChainAvailable),
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.providerRuntimeSupported),
		frameGenPolicy.resolvedReason);

	mLoggedStartup = true;
}

bool NRIRenderDevice::LoadNRI()
{
	if (mNriModule != nullptr)
	{
		return true;
	}

	HMODULE module = LoadLibraryA("NRI.dll");
	if (module == nullptr)
	{
		FString localPath = progdir;
		localPath << "NRI.dll";
		module = LoadLibraryA(localPath.GetChars());
	}

	if (module == nullptr)
	{
		Printf(TEXTCOLOR_RED "Failed to load NRI.dll.\n");
		return false;
	}

	mEnumerateAdapters = (PFN_nriEnumerateAdapters)GetProcAddress(module, "nriEnumerateAdapters");
	mCreateDeviceFn = (PFN_nriCreateDevice)GetProcAddress(module, "nriCreateDevice");
	mDestroyDeviceFn = (PFN_nriDestroyDevice)GetProcAddress(module, "nriDestroyDevice");
	mGetInterfaceFn = (PFN_nriGetInterface)GetProcAddress(module, "nriGetInterface");

	if (mEnumerateAdapters == nullptr || mCreateDeviceFn == nullptr || mDestroyDeviceFn == nullptr || mGetInterfaceFn == nullptr)
	{
		Printf(TEXTCOLOR_RED "NRI.dll is missing required exports.\n");
		FreeLibrary(module);
		return false;
	}

	gNriDestroyDeviceForwarder = mDestroyDeviceFn;
	gNriGetInterfaceForwarder = mGetInterfaceFn;
	mNriModule = module;
	return true;
}

bool NRIRenderDevice::CreateDevice()
{
	mLoggedD3D12FailureDred = false;
	const nri::GraphicsAPI selectedApi = GetSelectedAPI();
	const char* startupApi = V_GetStartupNriAPI();
	const bool enableGraphicsApiValidation = nri_apivalidation && selectedApi == nri::GraphicsAPI::D3D12;
	if (nri_apivalidation && selectedApi == nri::GraphicsAPI::VK)
	{
		Printf("NRI Vulkan graphics API validation is temporarily disabled; continuing with NRI validation only.\n");
	}

	if (selectedApi == nri::GraphicsAPI::D3D12)
	{
		ConfigureD3D12DebugLayer();
		ConfigureD3D12Dred();
	}

	nri::AdapterDesc adapters[8] = {};
	uint32_t adapterCount = (uint32_t)std::size(adapters);
	const nri::Result enumerateResult = mEnumerateAdapters(adapters, adapterCount);
	if (enumerateResult != nri::Result::SUCCESS || adapterCount == 0)
	{
		Printf(TEXTCOLOR_RED "Failed to enumerate NRI adapters (result=%s, count=%u).\n", GetNriResultName(enumerateResult), adapterCount);
		StartupRecovery_MarkNriCreateResult(startupApi, false, "enumerate_adapters_failed", false, nullptr);
		return false;
	}

	for (uint32_t i = 0; i < adapterCount; ++i)
	{
		const auto& adapter = adapters[i];
		const double videoMemoryGiB = (double)adapter.videoMemorySize / (1024.0 * 1024.0 * 1024.0);
		const double sharedMemoryGiB = (double)adapter.sharedSystemMemorySize / (1024.0 * 1024.0 * 1024.0);
		Printf("NRI adapter[%u]: %s (vendor=%s, video=%.2f GiB, shared=%.2f GiB, graphicsQueues=%u)\n",
			i,
			adapter.name,
			GetNriVendorName(adapter.vendor),
			videoMemoryGiB,
			sharedMemoryGiB,
			adapter.queueNum[(uint32_t)nri::QueueType::GRAPHICS]);
	}

	nri::DeviceCreationDesc creationDesc = {};
	creationDesc.graphicsAPI = selectedApi;
	creationDesc.adapterDesc = &adapters[0];
	mAdapterLocalBudgetBytes = adapters[0].videoMemorySize;
	mAdapterNonLocalBudgetBytes = adapters[0].sharedSystemMemorySize;
	creationDesc.callbackInterface.MessageCallback = &NriMessageCallback;
	creationDesc.enableGraphicsAPIValidation = enableGraphicsApiValidation;
	creationDesc.enableNRIValidation = !!nri_validation;
	creationDesc.disableVKRayTracing = false;
	// D3D12 ray-tracing builds assign acceleration-structure resources to the
	// legacy state domain. Keep subsequent barriers in that same domain instead
	// of illegally switching those resources to enhanced barriers.
	creationDesc.disableD3D12EnhancedBarriers = selectedApi == nri::GraphicsAPI::D3D12;
	creationDesc.vkBindingOffsets = {};
	Printf("NRI CreateDevice config: api=%s nri_validation=%s api_validation=%s dred=%s enhanced_barriers=%s\n",
		startupApi,
		nri_validation ? "on" : "off",
		nri_apivalidation ? "on" : "off",
		nri_dred ? "on" : "off",
		creationDesc.disableD3D12EnhancedBarriers ? "off" : "on");

	const nri::Result createResult = mCreateDeviceFn(creationDesc, mDevice);
	if (createResult != nri::Result::SUCCESS)
	{
		Printf(TEXTCOLOR_RED "Failed to create NRI device for API '%s' using adapter '%s' (result=%s).\n",
			startupApi,
			adapters[0].name,
			GetNriResultName(createResult));
		if (createResult == nri::Result::INVALID_SDK)
		{
			Printf(TEXTCOLOR_RED "NRI reported INVALID_SDK. Check that raze.exe exports D3D12SDKVersion/D3D12SDKPath and that an AgilitySDK runtime directory is staged beside the executable.\n");
		}
		StartupRecovery_MarkNriCreateResult(
			startupApi,
			false,
			GetNriResultName(createResult),
			createResult == nri::Result::UNSUPPORTED,
			adapters[0].name);
		return false;
	}

	mCreatedDeviceApi = selectedApi;

	if (mGetInterfaceFn(*mDevice, NRI_INTERFACE(nri::CoreInterface), &mCore) != nri::Result::SUCCESS ||
		mGetInterfaceFn(*mDevice, NRI_INTERFACE(nri::HelperInterface), &mHelper) != nri::Result::SUCCESS ||
		mGetInterfaceFn(*mDevice, NRI_INTERFACE(nri::RayTracingInterface), &mRayTracing) != nri::Result::SUCCESS ||
		mGetInterfaceFn(*mDevice, NRI_INTERFACE(nri::StreamerInterface), &mStreamer) != nri::Result::SUCCESS ||
		mGetInterfaceFn(*mDevice, NRI_INTERFACE(nri::SwapChainInterface), &mSwapChainInterface) != nri::Result::SUCCESS ||
		mGetInterfaceFn(*mDevice, NRI_INTERFACE(nri::UpscalerInterface), &mUpscaler) != nri::Result::SUCCESS)
	{
		Printf(TEXTCOLOR_RED "Failed to retrieve NRI interfaces.\n");
		StartupRecovery_MarkNriStartupFailure("nri_get_interfaces", "get_interfaces_failed");
		return false;
	}
	if (selectedApi == nri::GraphicsAPI::D3D12)
	{
		ConfigureD3D12InfoQueue(mCore, mDevice);
		const nri::Result wrapperResult = mGetInterfaceFn(*mDevice, NRI_INTERFACE(nri::WrapperD3D12Interface), &mWrapperD3D12);
		if (wrapperResult != nri::Result::SUCCESS)
		{
			mWrapperD3D12 = {};
		}
	}

	mLowLatency = {};
	const nri::Result lowLatencyResult = mGetInterfaceFn(*mDevice, NRI_INTERFACE(nri::LowLatencyInterface), &mLowLatency);
	if (lowLatencyResult != nri::Result::SUCCESS)
	{
		mLowLatency = {};
	}
	Printf("NRI low-latency interface: requested=%s result=%s available=%s\n",
		selectedApi == nri::GraphicsAPI::D3D12 ? "yes" : "no",
		GetNriResultName(lowLatencyResult),
		mLowLatency.SetLatencySleepMode != nullptr ? "yes" : "no");

	if (mCore.GetQueue(*mDevice, nri::QueueType::GRAPHICS, 0, mGraphicsQueue) != nri::Result::SUCCESS ||
		mCore.CreateFence(*mDevice, 0, mFrameFence) != nri::Result::SUCCESS ||
		mCore.CreateFence(*mDevice, 0, mCommandCompletionFence) != nri::Result::SUCCESS)
	{
		Printf(TEXTCOLOR_RED "Failed to create NRI queue objects.\n");
		StartupRecovery_MarkNriStartupFailure("nri_create_queue_objects", "queue_objects_failed");
		return false;
	}
	SetNriDebugName(mCore, mGraphicsQueue, "Raze.GraphicsQueue");
	SetNriDebugName(mCore, mFrameFence, "Raze.FrameFence");
	SetNriDebugName(mCore, mCommandCompletionFence, "Raze.CommandCompletionFence");
	RefreshNativeFrameGenerationHandles();
	Printf("NRI framegen native handles: api=%s device=%s queue=%s swapchain=%s\n",
		startupApi,
		mNativeD3D12Device != nullptr ? "ok" : "missing",
		mNativeD3D12GraphicsQueue != nullptr ? "ok" : "missing",
		"pending");

	if (!CreateQueuedFrames())
	{
		Printf(TEXTCOLOR_RED "Failed to create NRI queued frame resources.\n");
		StartupRecovery_MarkNriStartupFailure("nri_create_queued_frames", "queued_frames_failed");
		return false;
	}

	nri::StreamerDesc streamerDesc = {};
	streamerDesc.constantBufferMemoryLocation = nri::MemoryLocation::DEVICE_UPLOAD;
	streamerDesc.constantBufferSize = 1024 * 1024;
	streamerDesc.dynamicBufferMemoryLocation = nri::MemoryLocation::DEVICE_UPLOAD;
	streamerDesc.dynamicBufferDesc = {};
	streamerDesc.dynamicBufferDesc.usage = NRIFlags(nri::BufferUsageBits::VERTEX_BUFFER, nri::BufferUsageBits::INDEX_BUFFER);
	streamerDesc.queuedFrameNum = QueuedFrameCount;
	if (mStreamer.CreateStreamer(*mDevice, streamerDesc, mStreamerInstance) != nri::Result::SUCCESS)
	{
		Printf(TEXTCOLOR_RED "Failed to create NRI streamer.\n");
		StartupRecovery_MarkNriStartupFailure("nri_create_streamer", "streamer_failed");
		return false;
	}
	SetNriDebugName(mCore, mStreamerInstance, "Raze.Streamer");

	StartupRecovery_MarkNriCreateResult(startupApi, true, "startup_ok", false, adapters[0].name);
	return true;
}

void NRIRenderDevice::LogD3D12FailureDiagnostics(const char* context)
{
	if (GetLiveAPI() != nri::GraphicsAPI::D3D12)
	{
		return;
	}

	LogD3D12DeviceRemovedReason(mCore, mDevice, context);
	LogD3D12InfoQueueMessages(mCore, mDevice, context);

	if (mLoggedD3D12FailureDred || !nri_dred)
	{
		return;
	}

	mLoggedD3D12FailureDred = true;

	if (mDevice == nullptr || mCore.GetDeviceNativeObject == nullptr)
	{
		Printf(TEXTCOLOR_RED "NRI D3D12 DRED after %s: native D3D12 device is unavailable.\n",
			context != nullptr ? context : "unknown");
		return;
	}

	auto* d3d12Device = static_cast<ID3D12Device*>(mCore.GetDeviceNativeObject(mDevice));
	if (d3d12Device == nullptr)
	{
		Printf(TEXTCOLOR_RED "NRI D3D12 DRED after %s: native D3D12 device query returned null.\n",
			context != nullptr ? context : "unknown");
		return;
	}

	ID3D12DeviceRemovedExtendedData2* dred2 = nullptr;
	if (SUCCEEDED(d3d12Device->QueryInterface(IID_PPV_ARGS(&dred2))) && dred2 != nullptr)
	{
		const D3D12_DRED_DEVICE_STATE deviceState = dred2->GetDeviceState();
		Printf(TEXTCOLOR_RED "NRI D3D12 DRED after %s: using interface v2, device_state=%u.\n",
			context != nullptr ? context : "unknown",
			(unsigned)deviceState);

		D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs = {};
		const HRESULT breadcrumbsHr = dred2->GetAutoBreadcrumbsOutput1(&breadcrumbs);
		if (SUCCEEDED(breadcrumbsHr))
		{
			if (breadcrumbs.pHeadAutoBreadcrumbNode == nullptr)
			{
				Printf(TEXTCOLOR_RED "NRI D3D12 DRED breadcrumbs after %s: none.\n",
					context != nullptr ? context : "unknown");
			}
			else
			{
				LogD3D12DredBreadcrumbNodes1(breadcrumbs.pHeadAutoBreadcrumbNode, context);
			}
		}
		else
		{
			Printf(TEXTCOLOR_RED "NRI D3D12 DRED breadcrumbs after %s: GetAutoBreadcrumbsOutput1 failed (%s, 0x%08X).\n",
				context != nullptr ? context : "unknown",
				GetDxgiErrorName(breadcrumbsHr),
				(unsigned)breadcrumbsHr);
		}

		D3D12_DRED_PAGE_FAULT_OUTPUT1 pageFault = {};
		const HRESULT pageFaultHr = dred2->GetPageFaultAllocationOutput1(&pageFault);
		if (SUCCEEDED(pageFaultHr))
		{
			Printf(TEXTCOLOR_RED "NRI D3D12 DRED page fault after %s: VA=0x%llX\n",
				context != nullptr ? context : "unknown",
				(unsigned long long)pageFault.PageFaultVA);
			LogD3D12DredAllocationNodes1(pageFault.pHeadExistingAllocationNode, "existing_allocation");
			LogD3D12DredAllocationNodes1(pageFault.pHeadRecentFreedAllocationNode, "recent_freed");
		}
		else
		{
			Printf(TEXTCOLOR_RED "NRI D3D12 DRED page fault after %s: GetPageFaultAllocationOutput1 failed (%s, 0x%08X).\n",
				context != nullptr ? context : "unknown",
				GetDxgiErrorName(pageFaultHr),
				(unsigned)pageFaultHr);
		}

		dred2->Release();
		return;
	}

	ID3D12DeviceRemovedExtendedData1* dred1 = nullptr;
	if (SUCCEEDED(d3d12Device->QueryInterface(IID_PPV_ARGS(&dred1))) && dred1 != nullptr)
	{
		Printf(TEXTCOLOR_RED "NRI D3D12 DRED after %s: using interface v1.\n",
			context != nullptr ? context : "unknown");

		D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs = {};
		const HRESULT breadcrumbsHr = dred1->GetAutoBreadcrumbsOutput1(&breadcrumbs);
		if (SUCCEEDED(breadcrumbsHr))
		{
			if (breadcrumbs.pHeadAutoBreadcrumbNode == nullptr)
			{
				Printf(TEXTCOLOR_RED "NRI D3D12 DRED breadcrumbs after %s: none.\n",
					context != nullptr ? context : "unknown");
			}
			else
			{
				LogD3D12DredBreadcrumbNodes1(breadcrumbs.pHeadAutoBreadcrumbNode, context);
			}
		}
		else
		{
			Printf(TEXTCOLOR_RED "NRI D3D12 DRED breadcrumbs after %s: GetAutoBreadcrumbsOutput1 failed (%s, 0x%08X).\n",
				context != nullptr ? context : "unknown",
				GetDxgiErrorName(breadcrumbsHr),
				(unsigned)breadcrumbsHr);
		}

		D3D12_DRED_PAGE_FAULT_OUTPUT1 pageFault = {};
		const HRESULT pageFaultHr = dred1->GetPageFaultAllocationOutput1(&pageFault);
		if (SUCCEEDED(pageFaultHr))
		{
			Printf(TEXTCOLOR_RED "NRI D3D12 DRED page fault after %s: VA=0x%llX\n",
				context != nullptr ? context : "unknown",
				(unsigned long long)pageFault.PageFaultVA);
			LogD3D12DredAllocationNodes1(pageFault.pHeadExistingAllocationNode, "existing_allocation");
			LogD3D12DredAllocationNodes1(pageFault.pHeadRecentFreedAllocationNode, "recent_freed");
		}
		else
		{
			Printf(TEXTCOLOR_RED "NRI D3D12 DRED page fault after %s: GetPageFaultAllocationOutput1 failed (%s, 0x%08X).\n",
				context != nullptr ? context : "unknown",
				GetDxgiErrorName(pageFaultHr),
				(unsigned)pageFaultHr);
		}

		dred1->Release();
		return;
	}

	ID3D12DeviceRemovedExtendedData* dred = nullptr;
	const HRESULT dredHr = d3d12Device->QueryInterface(IID_PPV_ARGS(&dred));
	if (FAILED(dredHr) || dred == nullptr)
	{
		Printf(TEXTCOLOR_RED "NRI D3D12 DRED after %s: failed to query interfaces v2/v1/v0 (last=%s, 0x%08X).\n",
			context != nullptr ? context : "unknown",
			GetDxgiErrorName(dredHr),
			(unsigned)dredHr);
		return;
	}

	Printf(TEXTCOLOR_RED "NRI D3D12 DRED after %s: using interface v0.\n",
		context != nullptr ? context : "unknown");

	D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs = {};
	const HRESULT breadcrumbsHr = dred->GetAutoBreadcrumbsOutput(&breadcrumbs);
	if (SUCCEEDED(breadcrumbsHr))
	{
		if (breadcrumbs.pHeadAutoBreadcrumbNode == nullptr)
		{
			Printf(TEXTCOLOR_RED "NRI D3D12 DRED breadcrumbs after %s: none.\n",
				context != nullptr ? context : "unknown");
		}
		else
		{
			LogD3D12DredBreadcrumbNodes(breadcrumbs.pHeadAutoBreadcrumbNode, context);
		}
	}
	else
	{
		Printf(TEXTCOLOR_RED "NRI D3D12 DRED breadcrumbs after %s: GetAutoBreadcrumbsOutput failed (%s, 0x%08X).\n",
			context != nullptr ? context : "unknown",
			GetDxgiErrorName(breadcrumbsHr),
			(unsigned)breadcrumbsHr);
	}

	D3D12_DRED_PAGE_FAULT_OUTPUT pageFault = {};
	const HRESULT pageFaultHr = dred->GetPageFaultAllocationOutput(&pageFault);
	if (SUCCEEDED(pageFaultHr))
	{
		Printf(TEXTCOLOR_RED "NRI D3D12 DRED page fault after %s: VA=0x%llX\n",
			context != nullptr ? context : "unknown",
			(unsigned long long)pageFault.PageFaultVA);
		LogD3D12DredAllocationNodes(pageFault.pHeadExistingAllocationNode, "existing_allocation");
		LogD3D12DredAllocationNodes(pageFault.pHeadRecentFreedAllocationNode, "recent_freed");
	}
	else
	{
		Printf(TEXTCOLOR_RED "NRI D3D12 DRED page fault after %s: GetPageFaultAllocationOutput failed (%s, 0x%08X).\n",
			context != nullptr ? context : "unknown",
			GetDxgiErrorName(pageFaultHr),
			(unsigned)pageFaultHr);
	}

	dred->Release();
}

bool NRIRenderDevice::CreateSwapChain()
{
	if (mDevice == nullptr || mGraphicsQueue == nullptr || mainwindow.GetHandle() == nullptr)
	{
		return false;
	}
	const NRIPTOutputMode requestedOutputMode = GetRequestedPathTracingOutputMode();
	nri::SwapChainFormat requestedOutputFormat = nri::SwapChainFormat::BT709_G22_8BIT;
	nri::SwapChainFormat resolvedOutputFormat = nri::SwapChainFormat::BT709_G22_8BIT;
	const char* outputResolveReason = "requested-sdr";
	ResolvePathTracingSwapChainOutput(requestedOutputFormat, resolvedOutputFormat, outputResolveReason);

	DestroySwapChain();

	const uint32_t width = (uint32_t)(std::max)(GetClientWidth(), 1);
	const uint32_t height = (uint32_t)(std::max)(GetClientHeight(), 1);

	nri::SwapChainDesc swapChainDesc = {};
	swapChainDesc.window.windows.hwnd = mainwindow.GetHandle();
	swapChainDesc.queue = mGraphicsQueue;
	swapChainDesc.width = width;
	swapChainDesc.height = height;
	swapChainDesc.textureNum = GetRequestedSwapChainTextureCount();
	swapChainDesc.format = resolvedOutputFormat;
	swapChainDesc.flags = GetEffectiveRequestedSwapChainFlags();
	swapChainDesc.queuedFrameNum = QueuedFrameCount;

	const bool tryFrameGenPresentBridge =
		nri_framegen &&
		requestedOutputMode == NRIPTOutputMode::SDR &&
		HasRequestedFrameGenerationProvider() &&
		GetLiveAPI() == nri::GraphicsAPI::D3D12 &&
		!IsFullscreenModeActive() &&
		!mFrameGeneration.ConsumeNativeFallbackRequest();

	const auto resetCreateState = [&](nri::SwapChainFormat createdFormat, const char* reason)
	{
		mRequestedSwapChainFormat = requestedOutputFormat;
		mCreatedSwapChainFormat = createdFormat;
		mResolvedSwapChainTextureFormat = nri::Format::UNKNOWN;
		mSwapChainDisplayDesc = {};
		mSwapChainDisplayDescResult = nri::Result::FAILURE;
		mHasSwapChainDisplayDesc = false;
		mSwapChainOutputResolveReason = reason;
		mSwapChainFlags = swapChainDesc.flags;
		mSwapChainQueuedFrameNum = swapChainDesc.queuedFrameNum;
		mSwapChainTextureCount = 0;
		mObservedSwapChainAcquireMask = 0;
		mObservedSwapChainPresentMask = 0;
		mSwapChainAcquireCounts.clear();
		mSwapChainPresentCounts.clear();
		mSwapChainAbandonCounts.clear();
		mHasPresentedSwapChainFrame = false;
	};

	const char* activeOutputResolveReason = outputResolveReason;
	bool forcingSdrFallback = false;
	for (;;)
	{
		swapChainDesc.format = forcingSdrFallback ? nri::SwapChainFormat::BT709_G22_8BIT : resolvedOutputFormat;
		resetCreateState(swapChainDesc.format, activeOutputResolveReason);

		if (tryFrameGenPresentBridge)
		{
			RefreshNativeFrameGenerationSwapChain();
			mFrameGeneration.OnSwapChainCreated(*this);
			if (mFrameGeneration.IsPresentBridgeActive() && RefreshFrameGenerationPresentTargets())
			{
				mSwapChainTextureCount = (uint8_t)(std::min<size_t>)(mFrameGenerationPresentImages.size(), 255u);
				mSwapChainAcquireCounts.assign(mFrameGenerationPresentImages.size(), 0);
				mSwapChainPresentCounts.assign(mFrameGenerationPresentImages.size(), 0);
				mSwapChainAbandonCounts.assign(mFrameGenerationPresentImages.size(), 0);
				Printf("NRI framegen proxy swapchain created: textures=%u queued_frames=%u vsync=%s flags=%s wait_present=%s size=%ux%u requested_mode=%s requested_format=%s created_format=%s reason=%s\n",
					(uint32_t)mSwapChainTextureCount,
					(uint32_t)mSwapChainQueuedFrameNum,
					vid_vsync ? "on" : "off",
					DescribeSwapChainFlags(mSwapChainFlags).GetChars(),
					nri_ptwaitpresent ? "on" : "off",
					width,
					height,
					GetNRIPTOutputModeName(requestedOutputMode),
					GetSwapChainFormatName(mRequestedSwapChainFormat),
					GetSwapChainFormatName(mCreatedSwapChainFormat),
					mSwapChainOutputResolveReason.GetChars());
				Printf("NRI framegen native handles: api=%s device=%s queue=%s swapchain=%s\n",
					(const char*)nri_api,
					mNativeD3D12Device != nullptr ? "ok" : "missing",
					mNativeD3D12GraphicsQueue != nullptr ? "ok" : "missing",
					mNativeD3D12SwapChain != nullptr ? "ok" : "missing");
				if (mRenderer != nullptr)
				{
					mRenderer->PrintSwapChainRenderConfig();
				}
				return true;
			}

			DestroyFrameGenerationPresentTargets();
			mFrameGeneration.OnSwapChainDestroyed(*this);
			Printf(TEXTCOLOR_YELLOW "NRI framegen proxy swapchain creation failed; falling back to the native NRI swapchain path.\n");
		}

		nri::Result createSwapChainResult = mSwapChainInterface.CreateSwapChain(*mDevice, swapChainDesc, mSwapChain);
		if (createSwapChainResult != nri::Result::SUCCESS)
		{
			if (swapChainDesc.format != nri::SwapChainFormat::BT709_G22_8BIT)
			{
				Printf(TEXTCOLOR_YELLOW "NRI swapchain create fallback: requested_mode=%s requested_format=%s created_format=%s failed (%s); retrying SDR.\n",
					GetNRIPTOutputModeName(requestedOutputMode),
					GetSwapChainFormatName(mRequestedSwapChainFormat),
					GetSwapChainFormatName(swapChainDesc.format),
					GetNriResultName(createSwapChainResult));
				forcingSdrFallback = true;
				activeOutputResolveReason =
					requestedOutputMode == NRIPTOutputMode::HDR ?
						"hdr-swapchain-create-failed-fallback-sdr" :
						"swapchain-create-failed-fallback-sdr";
				continue;
			}

			Printf(TEXTCOLOR_RED "Failed to create NRI swapchain.\n");
			return false;
		}

		RefreshNativeFrameGenerationSwapChain();
		mFrameGeneration.OnSwapChainCreated(*this);
		if (mFrameGeneration.IsPresentBridgeActive() && !RefreshFrameGenerationPresentTargets())
		{
			Printf(TEXTCOLOR_RED "NRI framegen present bridge is active but proxy backbuffer wrapping failed; falling back to native present path.\n");
		}
		SetNriDebugName(mCore, mSwapChain, "Raze.SwapChain");

		uint32_t textureCount = 0;
		nri::Texture* const* textures = mSwapChainInterface.GetSwapChainTextures(*mSwapChain, textureCount);
		mSwapChainImages.resize(textureCount);
		mSwapChainQueuedFrameNum = swapChainDesc.queuedFrameNum;
		mSwapChainTextureCount = (uint8_t)(std::min<uint32_t>)(textureCount, 255u);
		mObservedSwapChainAcquireMask = 0;
		mObservedSwapChainPresentMask = 0;
		mSwapChainAcquireCounts.assign(textureCount, 0);
		mSwapChainPresentCounts.assign(textureCount, 0);
		mSwapChainAbandonCounts.assign(textureCount, 0);
		mHasPresentedSwapChainFrame = false;

		for (uint32_t i = 0; i < textureCount; ++i)
		{
			auto& image = mSwapChainImages[i];
			image.target.texture = textures[i];
			image.target.owned = false;
			const std::string imageName = "Raze.SwapChainImage[" + std::to_string(i) + "]";
			SetNriDebugName(mCore, image.target.texture, imageName.c_str());

			const nri::TextureDesc& desc = mCore.GetTextureDesc(*textures[i]);
			image.target.width = desc.width;
			image.target.height = desc.height;
			image.target.format = desc.format;
			mResolvedSwapChainTextureFormat = desc.format;
			image.target.usage = desc.usage;
			image.target.state = {};

			if (!CreateTextureViews(image.target))
			{
				return false;
			}

			if (mCore.CreateFence(*mDevice, nri::SWAPCHAIN_SEMAPHORE, image.acquireSemaphore) != nri::Result::SUCCESS ||
				mCore.CreateFence(*mDevice, nri::SWAPCHAIN_SEMAPHORE, image.releaseSemaphore) != nri::Result::SUCCESS)
			{
				return false;
			}
			const std::string acquireFenceName = "Raze.SwapChainAcquire[" + std::to_string(i) + "]";
			const std::string releaseFenceName = "Raze.SwapChainRelease[" + std::to_string(i) + "]";
			SetNriDebugName(mCore, image.acquireSemaphore, acquireFenceName.c_str());
			SetNriDebugName(mCore, image.releaseSemaphore, releaseFenceName.c_str());
		}

		RefreshSwapChainDisplayDesc(false);

		const nri::Format expectedResolvedTextureFormat = GetExpectedResolvedTextureFormatForSwapChainFormat(mCreatedSwapChainFormat);
		if (swapChainDesc.format != nri::SwapChainFormat::BT709_G22_8BIT &&
			expectedResolvedTextureFormat != nri::Format::UNKNOWN &&
			mResolvedSwapChainTextureFormat != expectedResolvedTextureFormat)
		{
			Printf(TEXTCOLOR_YELLOW "NRI swapchain wrap fallback: requested_mode=%s requested_format=%s created_format=%s resolved_texture_format=%s expected_texture_format=%s; retrying SDR.\n",
				GetNRIPTOutputModeName(requestedOutputMode),
				GetSwapChainFormatName(mRequestedSwapChainFormat),
				GetSwapChainFormatName(mCreatedSwapChainFormat),
				GetNriFormatName(mResolvedSwapChainTextureFormat),
				GetNriFormatName(expectedResolvedTextureFormat));
			DestroySwapChain();
			forcingSdrFallback = true;
			activeOutputResolveReason =
				requestedOutputMode == NRIPTOutputMode::HDR ?
					"hdr-wrap-format-mismatch-fallback-sdr" :
					"swapchain-wrap-format-mismatch-fallback-sdr";
			continue;
		}

		Printf("NRI swapchain created: textures=%u queued_frames=%u vsync=%s flags=%s requested_mode=%s requested_format=%s created_format=%s resolved_texture_format=%s texture_override=%d flag_override=%s wait_present=%s size=%ux%u display_desc=%s hdr=%s sdr_nits=%.1f max_nits=%.1f reason=%s\n",
			(uint32_t)mSwapChainTextureCount,
			(uint32_t)mSwapChainQueuedFrameNum,
			vid_vsync ? "on" : "off",
			DescribeSwapChainFlags(mSwapChainFlags).GetChars(),
			GetNRIPTOutputModeName(requestedOutputMode),
			GetSwapChainFormatName(mRequestedSwapChainFormat),
			GetSwapChainFormatName(mCreatedSwapChainFormat),
			GetNriFormatName(mResolvedSwapChainTextureFormat),
			(int)nri_ptswaptextures,
			DescribeSwapChainFlagOverride(),
			nri_ptwaitpresent ? "on" : "off",
			width,
			height,
			GetNriResultName(mSwapChainDisplayDescResult),
			mHasSwapChainDisplayDesc && mSwapChainDisplayDesc.isHDR ? "yes" : "no",
			mHasSwapChainDisplayDesc ? mSwapChainDisplayDesc.sdrLuminance : 80.0f,
			mHasSwapChainDisplayDesc ? mSwapChainDisplayDesc.maxLuminance : 80.0f,
			mSwapChainOutputResolveReason.GetChars());
		Printf("NRI framegen native handles: api=%s device=%s queue=%s swapchain=%s\n",
			(const char*)nri_api,
			mNativeD3D12Device != nullptr ? "ok" : "missing",
			mNativeD3D12GraphicsQueue != nullptr ? "ok" : "missing",
			mNativeD3D12SwapChain != nullptr ? "ok" : "missing");
		if (mRenderer != nullptr)
		{
			mRenderer->PrintSwapChainRenderConfig();
		}
		SyncPathTracingOutputModeCVarWithSwapChainState();

		return true;
	}
}

bool NRIRenderDevice::CreateQueuedFrames()
{
	DestroyQueuedFrames();

	mQueuedFrames.resize(QueuedFrameCount);
	for (size_t i = 0; i < mQueuedFrames.size(); ++i)
	{
		QueuedFrame& queuedFrame = mQueuedFrames[i];
		if (mCore.CreateCommandAllocator(*mGraphicsQueue, queuedFrame.commandAllocator) != nri::Result::SUCCESS ||
			mCore.CreateCommandBuffer(*queuedFrame.commandAllocator, queuedFrame.commandBuffer) != nri::Result::SUCCESS)
		{
			DestroyQueuedFrames();
			return false;
		}

		const std::string allocatorName = "Raze.QueuedFrameAllocator[" + std::to_string(i) + "]";
		const std::string commandBufferName = "Raze.QueuedFrameCommandBuffer[" + std::to_string(i) + "]";
		SetNriDebugName(mCore, queuedFrame.commandAllocator, allocatorName.c_str());
		SetNriDebugName(mCore, queuedFrame.commandBuffer, commandBufferName.c_str());
	}

	SelectQueuedFrame(0);
	return true;
}

void NRIRenderDevice::DestroySwapChain()
{
	DestroyFrameGenerationPresentTargets();
	RefreshNativeFrameGenerationSwapChain();
	ResetFrameTracking();
	mSwapChainFlags = nri::SwapChainBits::NONE;
	mRequestedSwapChainFormat = nri::SwapChainFormat::BT709_G22_8BIT;
	mCreatedSwapChainFormat = nri::SwapChainFormat::BT709_G22_8BIT;
	mResolvedSwapChainTextureFormat = nri::Format::UNKNOWN;
	mSwapChainDisplayDesc = {};
	mSwapChainDisplayDescResult = nri::Result::FAILURE;
	mSwapChainQueuedFrameNum = 0;
	mSwapChainTextureCount = 0;
	mObservedSwapChainAcquireMask = 0;
	mObservedSwapChainPresentMask = 0;
	mSwapChainAcquireCounts.clear();
	mSwapChainPresentCounts.clear();
	mSwapChainAbandonCounts.clear();
	mHasPresentedSwapChainFrame = false;
	mHasSwapChainDisplayDesc = false;
	mSwapChainOutputResolveReason = "requested-sdr";
	mFrameGeneration.OnSwapChainDestroyed(*this);

	for (auto& image : mSwapChainImages)
	{
		if (image.target.colorAttachmentView != nullptr)
		{
			mCore.DestroyDescriptor(image.target.colorAttachmentView);
			image.target.colorAttachmentView = nullptr;
		}

		if (image.target.shaderView != nullptr)
		{
			mCore.DestroyDescriptor(image.target.shaderView);
			image.target.shaderView = nullptr;
		}

		if (image.acquireSemaphore != nullptr)
		{
			mCore.DestroyFence(image.acquireSemaphore);
			image.acquireSemaphore = nullptr;
		}

		if (image.releaseSemaphore != nullptr)
		{
			mCore.DestroyFence(image.releaseSemaphore);
			image.releaseSemaphore = nullptr;
		}
	}

	mSwapChainImages.clear();

	if (mSwapChain != nullptr)
	{
		mSwapChainInterface.DestroySwapChain(mSwapChain);
		mSwapChain = nullptr;
	}
	RefreshNativeFrameGenerationSwapChain();
}

void NRIRenderDevice::DestroyQueuedFrames()
{
	mCommandAllocator = nullptr;
	mCommandBuffer = nullptr;
	mCurrentQueuedFrameIndex = 0;

	for (QueuedFrame& queuedFrame : mQueuedFrames)
	{
		if (queuedFrame.commandBuffer != nullptr)
		{
			mCore.DestroyCommandBuffer(queuedFrame.commandBuffer);
			queuedFrame.commandBuffer = nullptr;
		}

		if (queuedFrame.commandAllocator != nullptr)
		{
			mCore.DestroyCommandAllocator(queuedFrame.commandAllocator);
			queuedFrame.commandAllocator = nullptr;
		}
	}

	mQueuedFrames.clear();
}

bool NRIRenderDevice::CreateRenderResources()
{
	mDiagnosticShaderVariantEffective = false;
	mShaderVariantWarningEmitted = false;
	mShaderVariantSelectionEmitted = false;
	if (!LoadShaderBlob(GetSelectedAPI() == nri::GraphicsAPI::D3D12 ? "Nri2D.vs.dxil" : "Nri2D.vs.spirv", mVertexShaderBlob) ||
		!LoadShaderBlob(GetSelectedAPI() == nri::GraphicsAPI::D3D12 ? "Nri2D.ps.dxil" : "Nri2D.ps.spirv", mPixelShaderBlob))
	{
		return false;
	}

	nri::DescriptorRangeDesc samplerRange = {};
	samplerRange.baseRegisterIndex = 0;
	samplerRange.descriptorNum = 1;
	samplerRange.descriptorType = nri::DescriptorType::SAMPLER;
	samplerRange.shaderStages = NRIShaderStages();

	nri::DescriptorRangeDesc textureRange = {};
	textureRange.baseRegisterIndex = 0;
	textureRange.descriptorNum = 1;
	textureRange.descriptorType = nri::DescriptorType::TEXTURE;
	textureRange.shaderStages = NRIShaderStages();
	textureRange.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

	nri::DescriptorSetDesc descriptorSets[2] = {};
	descriptorSets[0].registerSpace = 0;
	descriptorSets[0].ranges = &samplerRange;
	descriptorSets[0].rangeNum = 1;
	descriptorSets[1].registerSpace = 1;
	descriptorSets[1].ranges = &textureRange;
	descriptorSets[1].rangeNum = 1;
	descriptorSets[1].flags = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;

	nri::RootConstantDesc rootConstant = {};
	rootConstant.registerIndex = 0;
	rootConstant.size = sizeof(NRIShaderConstants);
	rootConstant.shaderStages = NRIShaderStages();

	nri::PipelineLayoutDesc pipelineLayoutDesc = {};
	pipelineLayoutDesc.rootRegisterSpace = 2;
	pipelineLayoutDesc.rootConstants = &rootConstant;
	pipelineLayoutDesc.rootConstantNum = 1;
	pipelineLayoutDesc.descriptorSets = descriptorSets;
	pipelineLayoutDesc.descriptorSetNum = 2;
	pipelineLayoutDesc.shaderStages = NRIShaderStages();

	if (mCore.CreatePipelineLayout(*mDevice, pipelineLayoutDesc, mPipelineLayout) != nri::Result::SUCCESS)
	{
		Printf(TEXTCOLOR_RED "Failed to create NRI pipeline layout.\n");
		return false;
	}

	nri::DescriptorPoolDesc poolDesc = {};
	poolDesc.descriptorSetMaxNum = 4096;
	poolDesc.samplerMaxNum = 32;
	poolDesc.textureMaxNum = 16384;
	poolDesc.storageTextureMaxNum = 128;
	poolDesc.structuredBufferMaxNum = 512;
	poolDesc.storageStructuredBufferMaxNum = 512;
	poolDesc.accelerationStructureMaxNum = 16;

	if (mCore.CreateDescriptorPool(*mDevice, poolDesc, mDescriptorPool) != nri::Result::SUCCESS)
	{
		Printf(TEXTCOLOR_RED "Failed to create NRI descriptor pool.\n");
		return false;
	}

	auto createSampler = [this](NRISamplerMode mode, bool clamp, bool linear)
	{
		nri::SamplerDesc samplerDesc = {};
		samplerDesc.filters.min = linear ? nri::Filter::LINEAR : nri::Filter::NEAREST;
		samplerDesc.filters.mag = linear ? nri::Filter::LINEAR : nri::Filter::NEAREST;
		samplerDesc.filters.mip = linear ? nri::Filter::LINEAR : nri::Filter::NEAREST;
		samplerDesc.filters.op = nri::FilterOp::AVERAGE;
		samplerDesc.addressModes.u = clamp ? nri::AddressMode::CLAMP_TO_EDGE : nri::AddressMode::REPEAT;
		samplerDesc.addressModes.v = clamp ? nri::AddressMode::CLAMP_TO_EDGE : nri::AddressMode::REPEAT;
		samplerDesc.addressModes.w = clamp ? nri::AddressMode::CLAMP_TO_EDGE : nri::AddressMode::REPEAT;
		samplerDesc.compareOp = nri::CompareOp::NONE;

		return mCore.CreateSampler(*mDevice, samplerDesc, mSamplers[(size_t)mode]) == nri::Result::SUCCESS;
	};

	if (!createSampler(NRISamplerMode::ClampLinear, true, true) ||
		!createSampler(NRISamplerMode::WrapLinear, false, true) ||
		!createSampler(NRISamplerMode::ClampPoint, true, false) ||
		!createSampler(NRISamplerMode::WrapPoint, false, false))
	{
		Printf(TEXTCOLOR_RED "Failed to create NRI samplers.\n");
		return false;
	}

	for (size_t i = 0; i < (size_t)NRISamplerMode::Count; ++i)
	{
		nri::DescriptorSet* set = nullptr;
		if (mCore.AllocateDescriptorSets(*mDescriptorPool, *mPipelineLayout, 0, &set, 1, 0) != nri::Result::SUCCESS)
		{
			return false;
		}

		const nri::Descriptor* samplerDescriptor = mSamplers[i];
		nri::UpdateDescriptorRangeDesc updateDesc = {};
		updateDesc.descriptorSet = set;
		updateDesc.rangeIndex = 0;
		updateDesc.descriptors = &samplerDescriptor;
		updateDesc.descriptorNum = 1;
		mCore.UpdateDescriptorRanges(&updateDesc, 1);
		mSamplerSets[i] = set;
	}

	mWhiteTexture = new NRIHardwareTexture(this, 4);
	uint32_t whitePixel = 0xffffffffu;
	mWhiteTexture->CreateTexture((unsigned char*)&whitePixel, 1, 1, 0, false, "WhiteTexture");
	mWhiteTextureSet = mWhiteTexture->GetResource().textureSet;
	return mWhiteTextureSet != nullptr;
}

void NRIRenderDevice::DestroyRenderResources()
{
	ClearPendingScreenshotReadbacks();
	DestroyFrameGenerationPresentTargets();
	DestroyFrameGenerationUiTexture();
	DestroyViewSnapshotTexture();

	delete mWhiteTexture;
	mWhiteTexture = nullptr;
	mWhiteTextureSet = nullptr;

	ReleaseRetiredTextureResources(true);

	DestroyTextureResource(mSaveTarget);
	DestroyTextureResource(mSceneTarget);

	if (mPipelineLayout != nullptr)
	{
		mCore.DestroyPipelineLayout(mPipelineLayout);
		mPipelineLayout = nullptr;
	}

	if (mDescriptorPool != nullptr)
	{
		mCore.DestroyDescriptorPool(mDescriptorPool);
		mDescriptorPool = nullptr;
	}

	for (auto& samplerSet : mSamplerSets)
	{
		samplerSet = nullptr;
	}

	for (auto& sampler : mSamplers)
	{
		if (sampler != nullptr)
		{
			mCore.DestroyDescriptor(sampler);
			sampler = nullptr;
		}
	}
}

bool NRIRenderDevice::BeginCommandList(const char* reason, bool waitForSlotReuse)
{
	if (mDescriptorPool == nullptr || mQueuedFrames.empty())
	{
		return false;
	}

	SelectQueuedFrame(mCurrentQueuedFrameIndex);
	if (mCommandAllocator == nullptr || mCommandBuffer == nullptr)
	{
		return false;
	}

	if (mCommandBufferOpen)
	{
		Printf(TEXTCOLOR_RED "NRI BeginCommandList blocked: command buffer already open (reason=%s queued_frame=%u frame_index=%llu frame_begun=%s).\n",
			reason != nullptr ? reason : "unknown",
			(unsigned)mCurrentQueuedFrameIndex,
			(unsigned long long)mFrameIndex,
			mFrameBegun ? "true" : "false");
		return false;
	}

	if (waitForSlotReuse && mFrameFence != nullptr)
	{
		QueuedFrame& queuedFrame = mQueuedFrames[mCurrentQueuedFrameIndex];
		if (queuedFrame.hasSubmittedWork && queuedFrame.lastSubmittedFenceValue != 0)
		{
			if (!WaitForFenceValue(*mFrameFence, queuedFrame.lastSubmittedFenceValue, "Wait(FrameSlotReuse)"))
			{
				return false;
			}
		}
	}
	if (mGpuTiming != nullptr)
	{
		mGpuTiming->RetireSlot(mCore, mCurrentQueuedFrameIndex);
	}

	mCore.ResetCommandAllocator(*mCommandAllocator);
	const bool success = mCore.BeginCommandBuffer(*mCommandBuffer, mDescriptorPool) == nri::Result::SUCCESS;
	mCommandBufferOpen = success;
	mRecordingCommandFenceValue = success ? mNextCommandFenceValue++ : 0;
	if (success && mGpuTiming != nullptr)
	{
		mGpuTiming->BeginSegment(mCore, *mCommandBuffer, mCurrentQueuedFrameIndex, mFrameIndex);
	}
	if (!success)
	{
		Printf(TEXTCOLOR_RED "NRI BeginCommandList failed (reason=%s queued_frame=%u frame_index=%llu frame_begun=%s).\n",
			reason != nullptr ? reason : "unknown",
			(unsigned)mCurrentQueuedFrameIndex,
			(unsigned long long)mFrameIndex,
			mFrameBegun ? "true" : "false");
	}
	return success;
}

bool NRIRenderDevice::EnsureSwapChainSize()
{
	if (mSwapChain != nullptr)
	{
		RefreshSwapChainDisplayDesc(true);
	}

	const NRIPTOutputMode requestedOutputMode = GetRequestedPathTracingOutputMode();
	nri::SwapChainFormat requestedOutputFormat = nri::SwapChainFormat::BT709_G22_8BIT;
	nri::SwapChainFormat resolvedOutputFormat = nri::SwapChainFormat::BT709_G22_8BIT;
	const char* outputResolveReason = "requested-sdr";
	ResolvePathTracingSwapChainOutput(requestedOutputFormat, resolvedOutputFormat, outputResolveReason);
	SyncPathTracingOutputModeCVarWithSwapChainState(outputResolveReason);

	if (mSwapChain == nullptr)
	{
		if (!mFrameGenerationPresentImages.empty() && IsFrameGenerationPresentPathActive())
		{
			const uint32_t width = (uint32_t)(std::max)(GetClientWidth(), 1);
			const uint32_t height = (uint32_t)(std::max)(GetClientHeight(), 1);
			const nri::SwapChainBits requestedFlags = GetEffectiveRequestedSwapChainFlags();
			if (requestedOutputMode == NRIPTOutputMode::SDR &&
				mFrameGenerationPresentImages[0].width == width &&
				mFrameGenerationPresentImages[0].height == height &&
				mSwapChainFlags == requestedFlags &&
				mCreatedSwapChainFormat == resolvedOutputFormat)
			{
				return true;
			}

			mFrameGeneration.NoteReset(
				(mFrameGenerationPresentImages[0].width != width || mFrameGenerationPresentImages[0].height != height) ?
					"swapchain-resize" :
					(mSwapChainFlags != requestedFlags ? "swapchain-flags-change" : "swapchain-format-change"));
			if (mCreatedSwapChainFormat != resolvedOutputFormat)
			{
				Printf("NRI swapchain policy change: requested_mode=%s requested_format=%s created_format=%s desired_format=%s reason=%s\n",
					GetNRIPTOutputModeName(GetRequestedPathTracingOutputMode()),
					GetSwapChainFormatName(requestedOutputFormat),
					GetSwapChainFormatName(mCreatedSwapChainFormat),
					GetSwapChainFormatName(resolvedOutputFormat),
					outputResolveReason);
			}
			WaitForCommands(true);
		}
		return CreateSwapChain();
	}

	const uint32_t width = (uint32_t)(std::max)(GetClientWidth(), 1);
	const uint32_t height = (uint32_t)(std::max)(GetClientHeight(), 1);
	const nri::SwapChainBits requestedFlags = GetEffectiveRequestedSwapChainFlags();
	const uint8_t requestedTextureCount = GetRequestedSwapChainTextureCount();
	if (!mSwapChainImages.empty() &&
		mSwapChainImages[0].target.width == width &&
		mSwapChainImages[0].target.height == height &&
		mSwapChainFlags == requestedFlags &&
		mSwapChainTextureCount == requestedTextureCount &&
		mCreatedSwapChainFormat == resolvedOutputFormat)
	{
		return true;
	}

	if (mCreatedSwapChainFormat != resolvedOutputFormat)
	{
		Printf("NRI swapchain policy change: requested_mode=%s requested_format=%s created_format=%s desired_format=%s reason=%s\n",
			GetNRIPTOutputModeName(GetRequestedPathTracingOutputMode()),
			GetSwapChainFormatName(requestedOutputFormat),
			GetSwapChainFormatName(mCreatedSwapChainFormat),
			GetSwapChainFormatName(resolvedOutputFormat),
			outputResolveReason);
	}

	mFrameGeneration.NoteReset(
		(!mSwapChainImages.empty() && (mSwapChainImages[0].target.width != width || mSwapChainImages[0].target.height != height)) ?
			"swapchain-resize" :
			(mSwapChainFlags != requestedFlags ? "swapchain-flags-change" :
				(mSwapChainTextureCount != requestedTextureCount ? "swapchain-texture-count-change" : "swapchain-format-change")));
	WaitForCommands(true);
	return CreateSwapChain();
}

bool NRIRenderDevice::RefreshSwapChainDisplayDesc(bool logChanges)
{
	if (mSwapChain == nullptr)
	{
		return false;
	}

	const nri::Result previousResult = mSwapChainDisplayDescResult;
	const bool hadDisplayDesc = mHasSwapChainDisplayDesc;
	const nri::DisplayDesc previousDesc = mSwapChainDisplayDesc;

	nri::DisplayDesc refreshedDesc = {};
	const nri::Result refreshResult = mSwapChainInterface.GetDisplayDesc(*mSwapChain, refreshedDesc);
	mSwapChainDisplayDescResult = refreshResult;

	if (refreshResult == nri::Result::SUCCESS)
	{
		const bool descChanged = !hadDisplayDesc || HasDisplayDescChanged(previousDesc, refreshedDesc);
		mSwapChainDisplayDesc = refreshedDesc;
		mHasSwapChainDisplayDesc = true;

		if (logChanges && (previousResult != nri::Result::SUCCESS || descChanged))
		{
			Printf("NRI swapchain display change: created_format=%s display_desc=%s->%s hdr=%s->%s sdr_nits=%.1f->%.1f max_nits=%.1f->%.1f\n",
				GetSwapChainFormatName(mCreatedSwapChainFormat),
				GetNriResultName(previousResult),
				GetNriResultName(refreshResult),
				hadDisplayDesc && previousDesc.isHDR ? "yes" : "no",
				refreshedDesc.isHDR ? "yes" : "no",
				hadDisplayDesc ? previousDesc.sdrLuminance : 80.0f,
				refreshedDesc.sdrLuminance,
				hadDisplayDesc ? previousDesc.maxLuminance : 80.0f,
				refreshedDesc.maxLuminance);
		}

		return true;
	}

	if (logChanges && previousResult == nri::Result::SUCCESS)
	{
		Printf(TEXTCOLOR_YELLOW "NRI swapchain display refresh failed (%s); preserving cached display state hdr=%s sdr_nits=%.1f max_nits=%.1f\n",
			GetNriResultName(refreshResult),
			hadDisplayDesc && previousDesc.isHDR ? "yes" : "no",
			hadDisplayDesc ? previousDesc.sdrLuminance : 80.0f,
			hadDisplayDesc ? previousDesc.maxLuminance : 80.0f);
	}

	return mHasSwapChainDisplayDesc;
}

void NRIRenderDevice::EndFrameAndPresent()
{
	const double presentShellStartMs = I_msTimeF();
	double frameGenEndMs = 0.0;
	double configureDispatchMs = 0.0;
	double transitionMs = 0.0;
	double endCommandMs = 0.0;
	double simulationEndMs = 0.0;
	double submitPrepMs = 0.0;
	double submitCallMs = 0.0;
	double streamerEndMs = 0.0;
	double presentCallMs = 0.0;
	double tracePrintMs = 0.0;
	double resetMs = 0.0;
	double stageStartMs = I_msTimeF();

	mFrameGeneration.EndFrame(*this);
	frameGenEndMs = I_msTimeF() - stageStartMs;

	static int sLoggedPresentCount = 0;

	if (!mFrameBegun || mCommandBuffer == nullptr || mCurrentPresentTarget == nullptr)
	{
		ResetFrameTracking();
		return;
	}

	stageStartMs = I_msTimeF();
	mFrameGeneration.ConfigureAndDispatchFrame(*this);
	configureDispatchMs = I_msTimeF() - stageStartMs;
	stageStartMs = I_msTimeF();
	TransitionTexture(*mCurrentPresentTarget, { nri::AccessBits::NONE, nri::Layout::PRESENT, nri::StageBits::NONE });
	transitionMs = I_msTimeF() - stageStartMs;
	stageStartMs = I_msTimeF();
	if (mGpuTiming != nullptr) mGpuTiming->FinalizeSegment(mCore, *mCommandBuffer);
	mCore.EndCommandBuffer(*mCommandBuffer);
	mCommandBufferOpen = false;
	endCommandMs = I_msTimeF() - stageStartMs;

	const uint64_t submittedFenceValue = 1 + mFrameIndex;
	mSubmittedFenceValue = submittedFenceValue;
	mLastFrameBoundaryStats.submittedFenceValue = submittedFenceValue;
	const nri::FenceSubmitDesc frameFence = { mFrameFence, submittedFenceValue, nri::StageBits::NONE };
	const nri::FenceSubmitDesc commandFence = { mCommandCompletionFence, mRecordingCommandFenceValue, nri::StageBits::NONE };
	const nri::CommandBuffer* commandBuffers[] = { mCommandBuffer };

	stageStartMs = I_msTimeF();
	mFrameGeneration.OnSimulationEnd(*this);
	simulationEndMs = I_msTimeF() - stageStartMs;
	nri::QueueSubmitDesc submitDesc = {};
	submitDesc.commandBuffers = commandBuffers;
	submitDesc.commandBufferNum = 1;
	nri::FenceSubmitDesc waitFence = {};
	nri::FenceSubmitDesc signalFences[3] = {};
	uint32_t signalFenceCount = 0;
	if (!IsFrameGenerationPresentPathActive())
	{
		waitFence = { mSwapChainImages[mAcquireSemaphoreIndex].acquireSemaphore, 0, NRISwapChainAcquireWaitStages() };
		const nri::FenceSubmitDesc releaseFence = { mSwapChainImages[mCurrentSwapChainImage].releaseSemaphore, 0, nri::StageBits::NONE };
		signalFences[signalFenceCount++] = releaseFence;
		signalFences[signalFenceCount++] = frameFence;
		submitDesc.waitFences = &waitFence;
		submitDesc.waitFenceNum = 1;
		const bool lowLatencySwapChainEnabled = ((uint32_t)mSwapChainFlags & (uint32_t)nri::SwapChainBits::ALLOW_LOW_LATENCY) != 0;
		submitDesc.swapChain = lowLatencySwapChainEnabled ? mSwapChain : nullptr;
	}
	else
	{
		signalFences[signalFenceCount++] = frameFence;
		submitDesc.swapChain = nullptr;
	}
	if (commandFence.fence != nullptr && commandFence.value != 0)
	{
		signalFences[signalFenceCount++] = commandFence;
	}
	submitDesc.signalFences = signalFences;
	submitDesc.signalFenceNum = signalFenceCount;
	nri::Result submitResult = nri::Result::FAILURE;
	stageStartMs = I_msTimeF();
	mFrameGeneration.OnRenderSubmitStart(*this);
	submitPrepMs = I_msTimeF() - stageStartMs;
	{
		ScopedNriTiming submitTiming(NriPTQueueSubmit, mLastFrameBoundaryStats.submitMs);
		stageStartMs = I_msTimeF();
		submitResult = mCore.QueueSubmit(*mGraphicsQueue, submitDesc);
		submitCallMs = I_msTimeF() - stageStartMs;
	}
	if (submitResult == nri::Result::SUCCESS)
	{
		mRecordingCommandFenceValue = 0;
	}
	else
	{
		if (mGpuTiming != nullptr) mGpuTiming->AbandonSlot(mCurrentQueuedFrameIndex);
		AbandonRecordingCommandFenceValue();
	}
	mFrameGeneration.OnRenderSubmitEnd(*this);
	if (submitResult != nri::Result::SUCCESS)
	{
		if (submitResult == nri::Result::DEVICE_LOST)
		{
			mFrameGeneration.NoteReset("device-lost");
			StartupRecovery_MarkNriDeviceLost("QueueSubmit");
		}
		Printf(TEXTCOLOR_RED "NRI QueueSubmit failed with result '%s'.\n", GetNriResultName(submitResult));
		LogD3D12FailureDiagnostics("QueueSubmit");
		if (submitResult == nri::Result::DEVICE_LOST)
		{
			FatalTerminalDeviceLoss("QueueSubmit");
		}
	}

	stageStartMs = I_msTimeF();
	mStreamer.EndStreamerFrame(*mStreamerInstance);
	streamerEndMs = I_msTimeF() - stageStartMs;
	nri::Result presentResult = nri::Result::FAILURE;
	mFrameGeneration.OnPresentStart(*this);
	{
		ScopedNriTiming presentTiming(NriPTQueuePresent, mLastFrameBoundaryStats.presentMs);
		stageStartMs = I_msTimeF();
		if (IsFrameGenerationPresentPathActive())
		{
			if (!mFrameGeneration.Present(*this, !!vid_vsync, mFrameGenerationPresentAllowsTearing, presentResult))
			{
				presentResult = nri::Result::FAILURE;
			}
		}
		else
		{
			presentResult = mSwapChainInterface.QueuePresent(*mSwapChain, *mSwapChainImages[mCurrentSwapChainImage].releaseSemaphore);
		}
		presentCallMs = I_msTimeF() - stageStartMs;
	}
	mFrameGeneration.OnPresentEnd(*this, presentResult);
	mLastFrameBoundaryStats.presentResult = presentResult;
	if (presentResult == nri::Result::SUCCESS)
	{
		if (!IsFrameGenerationPresentPathActive())
		{
			NoteSwapChainPresent(mCurrentSwapChainImage);
		}
		mHasPresentedSwapChainFrame = true;
		StartupRecovery_NoteNriGameplayPresent(gametic, gamestate == GS_LEVEL);
		if (nri_ptdebug > 0 && sLoggedPresentCount < 4)
		{
			Printf("NRI present: frame_index=%llu image=%u queued_frame=%u\n",
				(unsigned long long)mFrameIndex,
				mCurrentSwapChainImage,
				mCurrentQueuedFrameIndex);
			sLoggedPresentCount++;
		}
	}
	else
	{
		mHasPresentedSwapChainFrame = false;
		if (presentResult == nri::Result::DEVICE_LOST)
		{
			mFrameGeneration.NoteReset("device-lost");
			StartupRecovery_MarkNriDeviceLost(IsFrameGenerationPresentPathActive() ? "FramegenPresent" : "QueuePresent");
		}
		Printf(TEXTCOLOR_RED "NRI QueuePresent failed with result '%s'.\n", GetNriResultName(presentResult));
		LogD3D12FailureDiagnostics(IsFrameGenerationPresentPathActive() ? "FramegenPresent" : "QueuePresent");
		if (presentResult == nri::Result::DEVICE_LOST)
		{
			FatalTerminalDeviceLoss(IsFrameGenerationPresentPathActive() ? "FramegenPresent" : "QueuePresent");
		}
		if (IsFrameGenerationPresentPathActive() && presentResult != nri::Result::DEVICE_LOST)
		{
			mFrameGeneration.RequestNativeFallback("proxy-present-failed");
			RequestSwapChainRefresh("proxy-present-failed", true);
			Printf(TEXTCOLOR_YELLOW "NRI framegen present fallback: scheduled native swapchain recreation for the next frame boundary.\n");
		}
	}
	if (mCurrentQueuedFrameIndex < mQueuedFrames.size())
	{
		QueuedFrame& queuedFrame = mQueuedFrames[mCurrentQueuedFrameIndex];
		queuedFrame.lastSubmittedFenceValue = submittedFenceValue;
		queuedFrame.lastSubmittedFrameIndex = mFrameIndex;
		queuedFrame.hasSubmittedWork = true;
	}
	CaptureCompactPerfFrameBoundary(presentResult == nri::Result::SUCCESS);
	RecordFrameSequence(mCurrentSwapChainImage, submittedFenceValue, presentResult);
	FinishPendingScreenshotReadbacks(submitResult == nri::Result::SUCCESS, submittedFenceValue);
	const bool tracedGameplayFrame = mTraceThisFrame && (mLastFrameBoundaryStats.pathTracedSceneRendered || mLastFrameBoundaryStats.postProcessInvoked);
	if (tracedGameplayFrame)
	{
		stageStartMs = I_msTimeF();
		PrintFrameBoundaryStatus();
		PrintSwapChainStatus();
		PrintFrameShellStatus();
		Print2DTextureStatus();
		PrintVramTelemetryStatus();
		tracePrintMs = I_msTimeF() - stageStartMs;
		const int remainingTraceFrames = (int)nri_pttraceframes - 1;
		nri_pttraceframes = remainingTraceFrames > 0 ? remainingTraceFrames : 0;
	}
	if (nri_ptdebug > 0 && mPathTracingWeaponLightEventsEnqueuedThisFrame > 0)
	{
		Printf("NRI PT weapon-light events: frame=%llu enqueued=%u pending=%u\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			mPathTracingWeaponLightEventsEnqueuedThisFrame,
			(uint32_t)mPendingPathTracingWeaponLightEvents.Size());
	}
	stageStartMs = I_msTimeF();
	ResetFrameTracking(presentResult == nri::Result::SUCCESS);
	resetMs = I_msTimeF() - stageStartMs;
	mPathTracingWeaponLightEventsEnqueuedThisFrame = 0;
	if (PerfLoopTraceActive())
	{
		Printf(
			"PERF present trace NRI: frame=%llu fg_end=%.3f dispatch=%.3f transition=%.3f endcmd=%.3f sim_end=%.3f submit_prep=%.3f submit_call=%.3f streamer=%.3f present_call=%.3f trace_print=%.3f reset=%.3f total=%.3f present_ok=%d\n",
			(unsigned long long)mLastFrameBoundaryStats.frameNumber,
			frameGenEndMs,
			configureDispatchMs,
			transitionMs,
			endCommandMs,
			simulationEndMs,
			submitPrepMs,
			submitCallMs,
			streamerEndMs,
			presentCallMs,
			tracePrintMs,
			resetMs,
			I_msTimeF() - presentShellStartMs,
			presentResult == nri::Result::SUCCESS ? 1 : 0);
	}
	mFrameIndex++;
}

void NRIRenderDevice::RenderTextureView(FCanvasTexture* tex, std::function<void(IntRect&)> renderFunc)
{
	if (!mInitialized || tex == nullptr)
	{
		return;
	}

	auto* hwTex = static_cast<NRIHardwareTexture*>(tex->GetHardwareTexture(0, 0));
	hwTex->EnsureCanvas(tex);

	NRITextureResource* previousTarget = mActiveTarget;
	FCanvasTexture* previousCanvasTexture = mActiveCanvasTexture;
	FTexture* previousCanvasSourceTexture = mActiveCanvasSourceTexture;
	mRenderState->EndFrame();
	mActiveTarget = &hwTex->GetResource();
	mActiveCanvasTexture = tex;
	mActiveCanvasSourceTexture = tex;

	IntRect bounds = {};
	bounds.width = tex->GetWidth();
	bounds.height = tex->GetHeight();
	renderFunc(bounds);

	mRenderState->EndFrame();
	TransitionTexture(hwTex->GetResource(), NRIShaderResourceState());
	mActiveTarget = previousTarget;
	mActiveCanvasTexture = previousCanvasTexture;
	mActiveCanvasSourceTexture = previousCanvasSourceTexture;
	tex->SetUpdated(true);
}

void NRIRenderDevice::RenderTextureView(FGameTexture* tex, std::function<void(IntRect&)> renderFunc)
{
	if (!mInitialized || tex == nullptr || tex->GetTexture() == nullptr)
	{
		return;
	}

	FTexture* source = tex->GetTexture();
	auto* hwTex = static_cast<NRIHardwareTexture*>(source->GetHardwareTexture(0, 0));
	hwTex->EnsureCanvas(source);

	NRITextureResource* previousTarget = mActiveTarget;
	FCanvasTexture* previousCanvasTexture = mActiveCanvasTexture;
	FTexture* previousCanvasSourceTexture = mActiveCanvasSourceTexture;
	mRenderState->EndFrame();
	mActiveTarget = &hwTex->GetResource();
	mActiveCanvasTexture = nullptr;
	mActiveCanvasSourceTexture = source;

	IntRect bounds = {};
	bounds.width = source->GetWidth();
	bounds.height = source->GetHeight();
	renderFunc(bounds);

	mRenderState->EndFrame();
	TransitionTexture(hwTex->GetResource(), NRIShaderResourceState());
	mActiveTarget = previousTarget;
	mActiveCanvasTexture = previousCanvasTexture;
	mActiveCanvasSourceTexture = previousCanvasSourceTexture;
}

void NRIRenderDevice::SnapshotCurrentViewToCanvas(FCanvasTexture* tex)
{
	if (!mInitialized || tex == nullptr)
	{
		return;
	}

	if (mFrameBegun && mCurrentPresentTarget != nullptr)
	{
		mPendingViewSnapshotCanvas = tex;
		return;
	}

	auto* hwTex = static_cast<NRIHardwareTexture*>(tex->GetHardwareTexture(0, 0));
	hwTex->EnsureCanvas(tex);
	if (CopyCurrentTargetToTexture(hwTex->GetResource()))
	{
		tex->SetUpdated(true);
	}
}

void NRIRenderDevice::RecordPendingScreenshotReadbacks()
{
	if (mPendingScreenshotCaptures.empty())
	{
		return;
	}

	if (!mFrameBegun || mCommandBuffer == nullptr || mCurrentPresentTarget == nullptr || mCurrentPresentTarget->texture == nullptr)
	{
		return;
	}

	NRITextureResource& source = *mCurrentPresentTarget;
	const uint32_t bytesPerPixel = GetScreenshotSourceBytesPerPixel(source.format);
	if (bytesPerPixel == 0)
	{
		Printf(TEXTCOLOR_RED "NRI screenshot failed: unsupported source format '%s'.\n", GetNriFormatName(source.format));
		ClearPendingScreenshotReadbacks();
		return;
	}

	mRenderState->EndFrame();
	TransitionTexture(source, NRICopySourceState());

	const nri::DeviceDesc& deviceDesc = mCore.GetDeviceDesc(*mDevice);
	for (auto& capture : mPendingScreenshotCaptures)
	{
		if (capture.readbackRecorded || capture.readbackBuffer != nullptr)
		{
			continue;
		}

		capture.width = (std::min)(capture.width, source.width);
		capture.height = (std::min)(capture.height, source.height);
		if (capture.width == 0 || capture.height == 0)
		{
			continue;
		}

		capture.sourceFormat = source.format;
		capture.rowPitch = AlignUp(capture.width * bytesPerPixel, deviceDesc.memoryAlignment.uploadBufferTextureRow);
		capture.slicePitch = AlignUp(capture.rowPitch * capture.height, deviceDesc.memoryAlignment.uploadBufferTextureSlice);

		nri::BufferDesc readbackDesc = {};
		readbackDesc.size = capture.slicePitch;
		if (mCore.CreateCommittedBuffer(*mDevice, nri::MemoryLocation::HOST_READBACK, 0.0f, readbackDesc, capture.readbackBuffer) != nri::Result::SUCCESS)
		{
			Printf(TEXTCOLOR_RED "NRI screenshot failed: could not create readback buffer.\n");
			continue;
		}

		nri::TextureRegionDesc region = {};
		region.width = capture.width;
		region.height = capture.height;
		region.depth = 1;
		region.planes = nri::PlaneBits::COLOR;

		nri::TextureDataLayoutDesc layout = {};
		layout.rowPitch = capture.rowPitch;
		layout.slicePitch = capture.slicePitch;

		mCore.CmdReadbackTextureToBuffer(*mCommandBuffer, *capture.readbackBuffer, layout, *source.texture, region);
		capture.readbackRecorded = true;
		capture.shellFrameIndex = mFrameIndex;
		capture.rendererFrameIndex = mRenderer != nullptr ? mRenderer->GetLastCompletedFrameIndex() : ~0u;
		Printf("NRI screenshot frame: serial=%llu renderer_frame=%u shell_frame=%llu path=\"%s\"\n",
			(unsigned long long)capture.serial,
			capture.rendererFrameIndex,
			(unsigned long long)capture.shellFrameIndex,
			capture.fileName.GetChars());
	}
}

void NRIRenderDevice::FinishPendingScreenshotReadbacks(bool submitted, uint64_t submittedFenceValue)
{
	if (mPendingScreenshotCaptures.empty())
	{
		return;
	}

	if (!submitted || mFrameFence == nullptr)
	{
		Printf(TEXTCOLOR_RED "NRI screenshot failed: frame submission did not complete.\n");
		ClearPendingScreenshotReadbacks();
		return;
	}

	if (!WaitForFenceValue(*mFrameFence, submittedFenceValue, "Wait(ScreenshotReadback)"))
	{
		Printf(TEXTCOLOR_RED "NRI screenshot failed: frame-fence wait did not complete.\n");
		ClearPendingScreenshotReadbacks();
		return;
	}

	for (auto& capture : mPendingScreenshotCaptures)
	{
		if (!capture.readbackRecorded || capture.readbackBuffer == nullptr || capture.file == nullptr)
		{
			Printf(TEXTCOLOR_RED "NRI screenshot failed: readback was not recorded.\n");
			continue;
		}

		const uint8_t* pixels = (const uint8_t*)mCore.MapBuffer(*capture.readbackBuffer, 0, capture.slicePitch);
		if (pixels == nullptr)
		{
			Printf(TEXTCOLOR_RED "NRI screenshot failed: could not map readback buffer.\n");
			continue;
		}

		TArray<uint8_t> image(capture.width * capture.height * 3, true);
		for (uint32_t y = 0; y < capture.height; ++y)
		{
			const uint8_t* src = pixels + (size_t)y * capture.rowPitch;
			uint8_t* dst = image.Data() + (size_t)y * capture.width * 3u;

			for (uint32_t x = 0; x < capture.width; ++x)
			{
				switch (capture.sourceFormat)
				{
				case nri::Format::BGRA8_UNORM:
				case nri::Format::BGRA8_SRGB:
					dst[x * 3 + 0] = src[x * 4 + 2];
					dst[x * 3 + 1] = src[x * 4 + 1];
					dst[x * 3 + 2] = src[x * 4 + 0];
					break;
				case nri::Format::RGBA8_UNORM:
				case nri::Format::RGBA8_SRGB:
					dst[x * 3 + 0] = src[x * 4 + 0];
					dst[x * 3 + 1] = src[x * 4 + 1];
					dst[x * 3 + 2] = src[x * 4 + 2];
					break;
				case nri::Format::RGBA16_SFLOAT:
				{
					const uint16_t* half = (const uint16_t*)(src + x * 8);
					dst[x * 3 + 0] = FloatToByte(HalfToFloat(half[0]));
					dst[x * 3 + 1] = FloatToByte(HalfToFloat(half[1]));
					dst[x * 3 + 2] = FloatToByte(HalfToFloat(half[2]));
					break;
				}
				case nri::Format::R10_G10_B10_A2_UNORM:
				{
					const uint32_t packed = *(const uint32_t*)(src + x * 4);
					dst[x * 3 + 0] = (uint8_t)(((packed >> 0) & 0x3ffu) * 255u / 1023u);
					dst[x * 3 + 1] = (uint8_t)(((packed >> 10) & 0x3ffu) * 255u / 1023u);
					dst[x * 3 + 2] = (uint8_t)(((packed >> 20) & 0x3ffu) * 255u / 1023u);
					break;
				}
				default:
					dst[x * 3 + 0] = 0;
					dst[x * 3 + 1] = 0;
					dst[x * 3 + 2] = 0;
					break;
				}
			}
		}

		mCore.UnmapBuffer(*capture.readbackBuffer);

		FStringf software(GAMENAME " %s", GetVersionString());
		if (!M_CreatePNG(capture.file.get(), image.Data(), nullptr, SS_RGB, capture.width, capture.height, capture.width * 3, 1.0f) ||
			!M_AppendPNGText(capture.file.get(), "Software", software.GetChars()) ||
			!M_FinishPNG(capture.file.get()))
		{
			Printf("Failed writing screenshot\n");
		}
		else
		{
			Printf("NRI screenshot completed: serial=%llu renderer_frame=%u shell_frame=%llu path=\"%s\"\n",
				(unsigned long long)capture.serial,
				capture.rendererFrameIndex,
				(unsigned long long)capture.shellFrameIndex,
				capture.fileName.GetChars());
			Printf("screenshot saved\n");
		}
	}

	ClearPendingScreenshotReadbacks();
}

void NRIRenderDevice::ClearPendingScreenshotReadbacks()
{
	for (auto& capture : mPendingScreenshotCaptures)
	{
		if (capture.readbackBuffer != nullptr && mDevice != nullptr)
		{
			mCore.DestroyBuffer(capture.readbackBuffer);
			capture.readbackBuffer = nullptr;
		}
	}
	mPendingScreenshotCaptures.clear();
}

void NRIRenderDevice::CopyScreenToBuffer(int width, int height, uint8_t* buffer)
{
	if (buffer == nullptr || width <= 0 || height <= 0)
	{
		return;
	}

	NRITextureResource* source = mActiveTarget != nullptr ? mActiveTarget : mCurrentPresentTarget;
	if (source == nullptr || source->texture == nullptr)
	{
		memset(buffer, 0, (size_t)width * (size_t)height * 3);
		return;
	}

	const bool useStandaloneSavePicFrame = mStandaloneSavePicFrame && mFrameBegun && mCommandBuffer != nullptr && mCommandBufferOpen;
	if (mFrameBegun)
	{
		mRenderState->EndFrame();
	}

	if (!useStandaloneSavePicFrame && !BeginCommandList("CopyScreenToBuffer", !mFrameBegun))
	{
		memset(buffer, 0, (size_t)width * (size_t)height * 3);
		return;
	}

	TransitionTexture(*source, NRICopySourceState());

	const nri::DeviceDesc& deviceDesc = mCore.GetDeviceDesc(*mDevice);
	const uint32_t rowPitch = AlignUp((uint32_t)width * 4u, deviceDesc.memoryAlignment.uploadBufferTextureRow);
	const uint32_t slicePitch = AlignUp(rowPitch * (uint32_t)height, deviceDesc.memoryAlignment.uploadBufferTextureSlice);

	nri::BufferDesc readbackDesc = {};
	readbackDesc.size = slicePitch;
	nri::Buffer* readbackBuffer = nullptr;
	if (mCore.CreateCommittedBuffer(*mDevice, nri::MemoryLocation::HOST_READBACK, 0.0f, readbackDesc, readbackBuffer) != nri::Result::SUCCESS)
	{
		memset(buffer, 0, (size_t)width * (size_t)height * 3);
		return;
	}

	nri::TextureRegionDesc region = {};
	region.width = (uint32_t)width;
	region.height = (uint32_t)height;
	region.depth = 1;
	region.planes = nri::PlaneBits::COLOR;

	nri::TextureDataLayoutDesc layout = {};
	layout.rowPitch = rowPitch;
	layout.slicePitch = slicePitch;

	mCore.CmdReadbackTextureToBuffer(*mCommandBuffer, *readbackBuffer, layout, *source->texture, region);
	if (!SubmitAndWaitCurrentCommandBuffer())
	{
		mCore.DestroyBuffer(readbackBuffer);
		memset(buffer, 0, (size_t)width * (size_t)height * 3);
		return;
	}

	const uint8_t* pixels = (const uint8_t*)mCore.MapBuffer(*readbackBuffer, 0, slicePitch);
	for (int y = 0; y < height; ++y)
	{
		const uint8_t* src = pixels + (size_t)y * rowPitch;
		uint8_t* dst = buffer + (size_t)y * (size_t)width * 3u;

		for (int x = 0; x < width; ++x)
		{
			dst[x * 3 + 0] = src[x * 4 + 0];
			dst[x * 3 + 1] = src[x * 4 + 1];
			dst[x * 3 + 2] = src[x * 4 + 2];
		}
	}

	mCore.UnmapBuffer(*readbackBuffer);
	mCore.DestroyBuffer(readbackBuffer);
}

void NRIRenderDevice::TransitionTexture(NRITextureResource& texture, nri::AccessLayoutStage after)
{
	if (texture.texture == nullptr)
	{
		return;
	}

	if (texture.state.access == after.access && texture.state.layout == after.layout && texture.state.stages == after.stages)
	{
		return;
	}

	nri::TextureBarrierDesc barrier = {};
	barrier.texture = texture.texture;
	barrier.before = texture.state;
	barrier.after = after;
	barrier.mipNum = 1;
	barrier.layerNum = 1;
	barrier.planes = nri::PlaneBits::COLOR;

	nri::BarrierDesc barriers = {};
	barriers.textures = &barrier;
	barriers.textureNum = 1;
	mCore.CmdBarrier(*mCommandBuffer, barriers);
	texture.state = after;
}

void NRIRenderDevice::PrepareTargetForRendering(NRITextureResource& target, bool)
{
	TransitionTexture(target, NRIColorAttachmentState());
}

void NRIRenderDevice::FinishTargetRendering(NRITextureResource& target, nri::AccessLayoutStage after)
{
	TransitionTexture(target, after);
}

void NRIRenderDevice::DestroyTextureResource(NRITextureResource& resource)
{
	if (resource.colorAttachmentView != nullptr)
	{
		mCore.DestroyDescriptor(resource.colorAttachmentView);
		resource.colorAttachmentView = nullptr;
	}

	if (resource.storageView != nullptr)
	{
		mCore.DestroyDescriptor(resource.storageView);
		resource.storageView = nullptr;
	}

	if (resource.shaderView != nullptr)
	{
		mCore.DestroyDescriptor(resource.shaderView);
		resource.shaderView = nullptr;
	}

	if (resource.owned && resource.texture != nullptr)
	{
		mCore.DestroyTexture(resource.texture);
	}

	resource.texture = nullptr;
	resource.owned = false;
	resource.width = 0;
	resource.height = 0;
	resource.layerNum = 1;
	resource.format = nri::Format::UNKNOWN;
	resource.memorySize = 0;
	resource.memoryLocation = nri::MemoryLocation::DEVICE;
	resource.type = nri::TextureType::TEXTURE_2D;
	resource.shaderViewType = nri::TextureView::TEXTURE;
	resource.usage = nri::TextureUsageBits::NONE;
	resource.state = {};
}

void NRIRenderDevice::RetireTextureResource(NRITextureResource& resource)
{
	if (resource.texture == nullptr &&
		resource.shaderView == nullptr &&
		resource.storageView == nullptr &&
		resource.colorAttachmentView == nullptr)
	{
		DestroyTextureResource(resource);
		return;
	}

	const uint64_t currentFrameFenceValue = mFrameBegun ? (1 + mFrameIndex) : 0;
	const uint64_t retireFenceValue = currentFrameFenceValue > mSubmittedFenceValue ? currentFrameFenceValue : mSubmittedFenceValue;
	if (mDevice == nullptr || mFrameFence == nullptr || retireFenceValue == 0)
	{
		DestroyTextureResource(resource);
		return;
	}

	RetiredTextureResource retired = {};
	retired.resource = resource;
	retired.fenceValue = retireFenceValue;
	mRetiredTextureResources.push_back(retired);
	resource = {};
}

void NRIRenderDevice::ReleaseRetiredTextureResources(bool finish)
{
	if (mRetiredTextureResources.empty())
	{
		return;
	}

	uint64_t completedFenceValue = 0;
	if (!finish)
	{
		if (mFrameFence == nullptr)
		{
			return;
		}
		if (!TryGetFenceValue(*mFrameFence, "GetFenceValue(FrameFence)", completedFenceValue))
		{
			return;
		}
	}
	else
	{
		WaitForCommands(true);
	}

	for (size_t i = 0; i < mRetiredTextureResources.size();)
	{
		RetiredTextureResource& retired = mRetiredTextureResources[i];
		if (finish || retired.fenceValue <= completedFenceValue)
		{
			DestroyTextureResource(retired.resource);
			mRetiredTextureResources[i] = mRetiredTextureResources.back();
			mRetiredTextureResources.pop_back();
			continue;
		}
		i++;
	}
}

bool NRIRenderDevice::CreateTextureViews(NRITextureResource& resource)
{
	const uint32_t usage = (uint32_t)resource.usage;
	nri::TextureViewDesc shaderViewDesc = {};
	shaderViewDesc.texture = resource.texture;
	shaderViewDesc.type = resource.shaderViewType;
	shaderViewDesc.format = resource.format;
	shaderViewDesc.mipNum = 1;
	shaderViewDesc.layerNum = resource.layerNum;
	shaderViewDesc.sliceNum = 1;
	shaderViewDesc.readonlyPlanes = nri::PlaneBits::COLOR;
	shaderViewDesc.components = { nri::ComponentSwizzle::IDENTITY, nri::ComponentSwizzle::IDENTITY, nri::ComponentSwizzle::IDENTITY, nri::ComponentSwizzle::IDENTITY };

	if ((usage & (uint32_t)nri::TextureUsageBits::SHADER_RESOURCE) != 0)
	{
		if (mCore.CreateTextureView(shaderViewDesc, resource.shaderView) != nri::Result::SUCCESS)
		{
			return false;
		}

		if (resource.textureSet == nullptr)
		{
			resource.textureSet = CreateTextureSet(resource.shaderView);
			if (resource.textureSet == nullptr)
			{
				return false;
			}
		}
		else
		{
			const nri::Descriptor* descriptor = resource.shaderView;
			nri::UpdateDescriptorRangeDesc updateDesc = {};
			updateDesc.descriptorSet = resource.textureSet;
			updateDesc.rangeIndex = 0;
			updateDesc.descriptors = &descriptor;
			updateDesc.descriptorNum = 1;
			mCore.UpdateDescriptorRanges(&updateDesc, 1);
		}
	}

	if ((usage & (uint32_t)nri::TextureUsageBits::SHADER_RESOURCE_STORAGE) != 0)
	{
		nri::TextureViewDesc storageViewDesc = shaderViewDesc;
		if (resource.format == nri::Format::BGRA8_SRGB)
		{
			storageViewDesc.format = nri::Format::BGRA8_UNORM;
		}
		else if (resource.format == nri::Format::RGBA8_SRGB)
		{
			// Match NRD-Sample: UAVs use the non-sRGB twin of the underlying texture format.
			storageViewDesc.format = nri::Format::RGBA8_UNORM;
		}
		storageViewDesc.type = nri::TextureView::STORAGE_TEXTURE;
		if (mCore.CreateTextureView(storageViewDesc, resource.storageView) != nri::Result::SUCCESS)
		{
			return false;
		}
	}

	if ((usage & (uint32_t)nri::TextureUsageBits::COLOR_ATTACHMENT) != 0)
	{
		nri::TextureViewDesc colorViewDesc = shaderViewDesc;
		colorViewDesc.type = nri::TextureView::COLOR_ATTACHMENT;
		if (mCore.CreateTextureView(colorViewDesc, resource.colorAttachmentView) != nri::Result::SUCCESS)
		{
			return false;
		}
	}

	return true;
}

bool NRIRenderDevice::CreateOwnedTexture(NRITextureResource& resource, uint32_t width, uint32_t height, nri::Format format, nri::TextureUsageBits usage, nri::TextureType type, uint32_t layerNum, nri::TextureView shaderViewType)
{
	nri::TextureDesc textureDesc = {};
	textureDesc.type = type;
	textureDesc.usage = usage;
	textureDesc.format = format;
	textureDesc.width = width;
	textureDesc.height = height;
	textureDesc.depth = 1;
	textureDesc.mipNum = 1;
	textureDesc.layerNum = layerNum;
	textureDesc.sampleNum = 1;

	if (mCore.CreateCommittedTexture(*mDevice, nri::MemoryLocation::DEVICE, 0.0f, textureDesc, resource.texture) != nri::Result::SUCCESS)
	{
		return false;
	}

	resource.width = width;
	resource.height = height;
	resource.layerNum = layerNum;
	resource.format = format;
	nri::MemoryDesc memoryDesc = {};
	mCore.GetTextureMemoryDesc(*resource.texture, nri::MemoryLocation::DEVICE, memoryDesc);
	resource.memorySize = memoryDesc.size;
	resource.memoryLocation = nri::MemoryLocation::DEVICE;
	resource.type = type;
	resource.shaderViewType = shaderViewType;
	resource.usage = usage;
	resource.owned = true;
	resource.state = {};
	return CreateTextureViews(resource);
}

bool NRIRenderDevice::UploadTextureData(NRITextureResource& resource, const void* data, uint32_t width, uint32_t height, uint32_t rowPitch)
{
	if (data == nullptr || width == 0 || height == 0 || rowPitch == 0)
	{
		return false;
	}

	const uint32_t slicePitch = rowPitch * height;
	std::vector<uint8_t> uploadCopy(slicePitch);
	memcpy(uploadCopy.data(), data, slicePitch);

	nri::TextureSubresourceUploadDesc subresource = {};
	subresource.slices = uploadCopy.data();
	subresource.sliceNum = 1;
	subresource.rowPitch = rowPitch;
	subresource.slicePitch = slicePitch;

	return UploadTextureSubresources(resource, &subresource, 1, width, height);
}

bool NRIRenderDevice::UploadTextureDataAsync(
	NRITextureResource& resource,
	const void* data,
	uint32_t width,
	uint32_t height,
	uint32_t rowPitch,
	uint64_t& outFenceValue)
{
	outFenceValue = 0;
	if (data == nullptr || width == 0 || height == 0 || rowPitch == 0 ||
		resource.texture == nullptr || !mFrameBegun || !mCommandBufferOpen ||
		mCommandBuffer == nullptr || mStreamerInstance == nullptr)
	{
		return false;
	}
	const uint64_t recordingFenceValue = GetRecordingCommandFenceValue();
	if (recordingFenceValue == 0)
	{
		return false;
	}

	nri::TextureRegionDesc region = {};
	region.width = width;
	region.height = height;
	region.depth = 1;
	region.planes = nri::PlaneBits::COLOR;
	nri::StreamTextureDataDesc streamDesc = {};
	streamDesc.data = data;
	streamDesc.dataRowPitch = rowPitch;
	streamDesc.dataSlicePitch = rowPitch * height;
	streamDesc.dstTexture = resource.texture;
	streamDesc.dstRegion = region;

	const nri::BufferOffset streamed = mStreamer.StreamTextureData(*mStreamerInstance, streamDesc);
	if (streamed.buffer == nullptr)
	{
		return false;
	}
	TransitionTexture(resource, NRICopyDestinationState());
	mStreamer.CmdCopyStreamedData(*mCommandBuffer, *mStreamerInstance);
	TransitionTexture(resource, NRIShaderResourceState());
	resource.width = width;
	resource.height = height;
	outFenceValue = recordingFenceValue;
	return true;
}

bool NRIRenderDevice::UploadTextureSubresources(NRITextureResource& resource, const nri::TextureSubresourceUploadDesc* subresources, uint32_t subresourceNum, uint32_t width, uint32_t height)
{
	if (subresources == nullptr || subresourceNum == 0 || width == 0 || height == 0)
	{
		return false;
	}

	nri::TextureUploadDesc uploadDesc = {};
	uploadDesc.subresources = subresources;
	uploadDesc.texture = resource.texture;
	uploadDesc.after = NRIShaderResourceState();
	uploadDesc.planes = nri::PlaneBits::COLOR;

	const nri::Result result = mHelper.UploadData(*mGraphicsQueue, &uploadDesc, 1, nullptr, 0);
	if (result == nri::Result::SUCCESS)
	{
		resource.state = NRIShaderResourceState();
		resource.width = width;
		resource.height = height;
		return true;
	}

	return false;
}

bool NRIRenderDevice::CopyTextureToTexture(NRITextureResource& destination, NRITextureResource& source)
{
	if (source.texture == nullptr || destination.texture == nullptr)
	{
		return false;
	}

	const bool useActiveFrameCommandBuffer = mFrameBegun && mCommandBuffer != nullptr;
	if (mFrameBegun)
	{
		mRenderState->EndFrame();
	}

	if (!useActiveFrameCommandBuffer && !BeginCommandList("CopyTextureToTexture", true))
	{
		return false;
	}

	const nri::AccessLayoutStage sourceStateBeforeCopy = source.state;
	TransitionTexture(source, NRICopySourceState());
	TransitionTexture(destination, NRICopyDestinationState());
	mCore.CmdCopyTexture(*mCommandBuffer, *destination.texture, nullptr, *source.texture, nullptr);
	TransitionTexture(source, sourceStateBeforeCopy);
	TransitionTexture(destination, NRIShaderResourceState());

	if (useActiveFrameCommandBuffer)
	{
		return true;
	}

	if (mGpuTiming != nullptr) mGpuTiming->FinalizeSegment(mCore, *mCommandBuffer);
	mCore.EndCommandBuffer(*mCommandBuffer);
	mCommandBufferOpen = false;

	const nri::CommandBuffer* commandBuffers[] = { mCommandBuffer };
	nri::Fence* copyFence = nullptr;
	if (mCore.CreateFence(*mDevice, 0, copyFence) != nri::Result::SUCCESS)
	{
		if (mGpuTiming != nullptr) mGpuTiming->AbandonSlot(mCurrentQueuedFrameIndex);
		AbandonRecordingCommandFenceValue();
		return false;
	}
	nri::FenceSubmitDesc frameFence = {};
	frameFence.fence = copyFence;
	frameFence.value = 1;

	nri::QueueSubmitDesc submitDesc = {};
	submitDesc.commandBuffers = commandBuffers;
	submitDesc.commandBufferNum = 1;
	submitDesc.signalFences = &frameFence;
	submitDesc.signalFenceNum = 1;
	const nri::Result submitResult = mCore.QueueSubmit(*mGraphicsQueue, submitDesc);
	bool waitSucceeded = submitResult == nri::Result::SUCCESS;
	if (submitResult == nri::Result::SUCCESS)
	{
		waitSucceeded = WaitForFenceValue(*copyFence, frameFence.value, "Wait(CopyTextureFence)");
		if (waitSucceeded)
		{
			mRecordingCommandFenceValue = 0;
			if (mGpuTiming != nullptr) mGpuTiming->RetireSlot(mCore, mCurrentQueuedFrameIndex);
		}
	}
	else
	{
		if (mGpuTiming != nullptr) mGpuTiming->AbandonSlot(mCurrentQueuedFrameIndex);
		AbandonRecordingCommandFenceValue();
	}
	if (!waitSucceeded && mGpuTiming != nullptr) mGpuTiming->AbandonSlot(mCurrentQueuedFrameIndex);
	mCore.DestroyFence(copyFence);
	return submitResult == nri::Result::SUCCESS && waitSucceeded;
}

bool NRIRenderDevice::CopyCurrentTargetToTexture(NRITextureResource& destination)
{
	NRITextureResource* source = mFrameBegun && mActiveTarget != nullptr ? mActiveTarget : mCurrentPresentTarget;
	if (source == nullptr || source->texture == nullptr || destination.texture == nullptr)
	{
		return false;
	}

	return CopyTextureToTexture(destination, *source);
}

bool NRIRenderDevice::SnapshotTextureToCanvas(FCanvasTexture* tex, NRITextureResource& source)
{
	if (tex == nullptr || source.texture == nullptr || twod == nullptr)
	{
		return false;
	}

	auto* canvasHwTex = static_cast<NRIHardwareTexture*>(tex->GetHardwareTexture(0, 0));
	if (canvasHwTex == nullptr)
	{
		return false;
	}
	canvasHwTex->EnsureCanvas(tex);

	if (!EnsureViewSnapshotTexture(source.width, source.height, source.format))
	{
		return false;
	}

	NRITextureResource* snapshotTarget = GetViewSnapshotTargetResource();
	if (snapshotTarget == nullptr)
	{
		return false;
	}

	if (!CopyTextureToTexture(*snapshotTarget, source))
	{
		return false;
	}

	NRITextureResource* previousTarget = mActiveTarget;
	mRenderState->EndFrame();
	mActiveTarget = &canvasHwTex->GetResource();
	mRenderState->NotifyExternalTargetWrite();
	ClearTargetColor(*mActiveTarget, 0.0f, 0.0f, 0.0f, 1.0f);
	mRenderState->SetColorMask(true, true, true, false);

	F2DDrawer snapshotDrawer;
	DrawTexture(&snapshotDrawer, mViewSnapshotTexture, 0, 0,
		DTA_DestWidth, tex->GetWidth(),
		DTA_DestHeight, tex->GetHeight(),
		DTA_FlipY, RenderTextureIsFlipped(),
		DTA_LegacyRenderStyle, STYLE_Source,
		DTA_Masked, false,
		TAG_DONE);
	::Draw2D(&snapshotDrawer, *mRenderState, 0, 0, tex->GetWidth(), tex->GetHeight());
	snapshotDrawer.Clear();
	mRenderState->SetColorMask(true, true, true, true);

	tex->SetUpdated(true);

	mRenderState->EndFrame();
	TransitionTexture(canvasHwTex->GetResource(), NRIShaderResourceState());
	mActiveTarget = previousTarget;
	mRenderState->NotifyExternalTargetWrite();

	return true;
}

bool NRIRenderDevice::LoadShaderBlob(const char* fileName, std::vector<uint8_t>& outBlob)
{
	const bool diagnosticRequested = FString(nri_shadervariant).CompareNoCase("diagnostic") == 0;
	const bool diagnosticCandidate = std::strcmp(fileName, "TraceOpaque.cs.dxil") == 0 ||
		std::strcmp(fileName, "TraceOpaque.cs.spirv") == 0 ||
		std::strcmp(fileName, "TraceOpaqueCache.cs.dxil") == 0 ||
		std::strcmp(fileName, "TraceOpaqueCache.cs.spirv") == 0 ||
		std::strcmp(fileName, "SmokeViewWorkProjectTiles.cs.dxil") == 0 ||
		std::strcmp(fileName, "SmokeViewWorkProjectTiles.cs.spirv") == 0;
	FString shaderPath = progdir;
	shaderPath << "shaders/nri/";
	if (diagnosticRequested && diagnosticCandidate)
	{
		FString manifestPath = shaderPath;
		manifestPath << "nri-shaders.json";
		std::ifstream manifestFile(manifestPath.GetChars(), std::ios::binary);
		const std::string manifest((std::istreambuf_iterator<char>(manifestFile)),
			std::istreambuf_iterator<char>());
		const std::string declaredPath = std::string("\"path\": \"variants/diagnostic/") + fileName + "\"";
		if (manifest.find("\"resolvedProfile\": \"DEVELOPER\"") != std::string::npos &&
			manifest.find(declaredPath) != std::string::npos)
		{
			shaderPath << "variants/diagnostic/";
			mDiagnosticShaderVariantEffective = true;
			if (!mShaderVariantSelectionEmitted)
			{
				Printf("NRI shader variant: diagnostic (developer manifest).\n");
				mShaderVariantSelectionEmitted = true;
			}
		}
		else if (!mShaderVariantWarningEmitted)
		{
			Printf(TEXTCOLOR_ORANGE "NRI diagnostic shader variant is unavailable; using production shaders.\n");
			mShaderVariantWarningEmitted = true;
		}
	}
	shaderPath << fileName;

	std::ifstream file(shaderPath.GetChars(), std::ios::binary | std::ios::ate);
	if (!file.is_open())
	{
		Printf(TEXTCOLOR_RED "Failed to open NRI shader '%s'.\n", shaderPath.GetChars());
		return false;
	}

	const std::streamsize size = file.tellg();
	file.seekg(0, std::ios::beg);
	outBlob.resize((size_t)size);
	return file.read((char*)outBlob.data(), size).good();
}

const void* NRIRenderDevice::GetVertexShaderBytecode(size_t& size) const
{
	size = mVertexShaderBlob.size();
	return mVertexShaderBlob.empty() ? nullptr : mVertexShaderBlob.data();
}

const void* NRIRenderDevice::GetPixelShaderBytecode(size_t& size) const
{
	size = mPixelShaderBlob.size();
	return mPixelShaderBlob.empty() ? nullptr : mPixelShaderBlob.data();
}

nri::GraphicsAPI NRIRenderDevice::GetSelectedAPI() const
{
	return FString(V_GetStartupNriAPI()).CompareNoCase("d3d12") == 0 ? nri::GraphicsAPI::D3D12 : nri::GraphicsAPI::VK;
}

nri::GraphicsAPI NRIRenderDevice::GetLiveAPI() const
{
	return mDevice != nullptr ? mCreatedDeviceApi : GetSelectedAPI();
}

NRIBackendCapabilities NRIRenderDevice::BuildBackendCapabilities() const
{
	NRIBackendCapabilities capabilities = {};
	capabilities.liveApi = GetLiveAPI();
	capabilities.d3d12 = capabilities.liveApi == nri::GraphicsAPI::D3D12;
	capabilities.vulkan = capabilities.liveApi == nri::GraphicsAPI::VK;
	capabilities.lowLatencyInterfaceAvailable =
		mLowLatency.SetLatencySleepMode != nullptr &&
		mLowLatency.SetLatencyMarker != nullptr &&
		mLowLatency.LatencySleep != nullptr &&
		mLowLatency.GetLatencyReport != nullptr;
	capabilities.lowLatencySwapChainEnabled =
		mSwapChain != nullptr &&
		(((uint32_t)mSwapChainFlags & (uint32_t)nri::SwapChainBits::ALLOW_LOW_LATENCY) != 0);
	capabilities.dredRequested = !!nri_dred;

#ifdef _WIN32
	capabilities.nativeD3D12DeviceAvailable = mNativeD3D12Device != nullptr;
	capabilities.nativeD3D12GraphicsQueueAvailable = mNativeD3D12GraphicsQueue != nullptr;
	capabilities.nativeD3D12SwapChainAvailable = mNativeD3D12SwapChain != nullptr;
#endif

	if (mDevice != nullptr)
	{
		const nri::DeviceDesc& deviceDesc = mCore.GetDeviceDesc(*mDevice);
		capabilities.shaderModel = deviceDesc.shaderModel;
		capabilities.lowLatencyFeatureAvailable = !!deviceDesc.features.lowLatency;
		capabilities.lowLatencyAvailable = capabilities.lowLatencyFeatureAvailable && capabilities.lowLatencyInterfaceAvailable;
		capabilities.waitableSwapChainAvailable = !!deviceDesc.features.waitableSwapChain;
	}

	return capabilities;
}

NRISamplerMode NRIRenderDevice::GetSamplerMode(int clampMode) const
{
	const bool point = clampMode == CLAMP_NOFILTER || clampMode == CLAMP_NOFILTER_X || clampMode == CLAMP_NOFILTER_Y || clampMode == CLAMP_NOFILTER_XY;
	const bool clamp = clampMode != CLAMP_NONE && clampMode != CLAMP_CAMTEX;

	if (point)
	{
		return clamp ? NRISamplerMode::ClampPoint : NRISamplerMode::WrapPoint;
	}

	return clamp ? NRISamplerMode::ClampLinear : NRISamplerMode::WrapLinear;
}

nri::DescriptorSet* NRIRenderDevice::GetSamplerSet(NRISamplerMode mode) const
{
	return mSamplerSets[(size_t)mode];
}

nri::DescriptorSet* NRIRenderDevice::CreateTextureSet(nri::Descriptor* shaderView)
{
	nri::DescriptorSet* set = nullptr;
	if (mCore.AllocateDescriptorSets(*mDescriptorPool, *mPipelineLayout, 1, &set, 1, 0) != nri::Result::SUCCESS)
	{
		return nullptr;
	}

	const nri::Descriptor* descriptor = shaderView;
	nri::UpdateDescriptorRangeDesc updateDesc = {};
	updateDesc.descriptorSet = set;
	updateDesc.rangeIndex = 0;
	updateDesc.descriptors = &descriptor;
	updateDesc.descriptorNum = 1;
	mCore.UpdateDescriptorRanges(&updateDesc, 1);
	return set;
}

void NRIRenderDevice::ResetFrameTracking(bool presentedAcquiredImage)
{
	if (mHasAcquiredSwapChainImage && !presentedAcquiredImage)
	{
		NoteSwapChainAbandon(mCurrentSwapChainImage);
	}

	if (mCommandBufferOpen)
	{
		AbandonRecordingCommandFenceValue();
	}
	mFrameBegun = false;
	mCommandBufferOpen = false;
	mCurrentPresentTarget = nullptr;
	mActiveTarget = nullptr;
	mFrameGenerationUiTargetActive = false;
	mHasAcquiredSwapChainImage = false;
	mCurrentSwapChainImage = 0;
	mStandaloneSavePicFrame = false;
	mPreloadCommandContextActive = false;
}

void NRIRenderDevice::ResetLevelTransitionShellState()
{
	ResetFrameTracking(false);
	ClearPendingScreenshotReadbacks();
	mUsingSaveTarget = false;
	mActiveCanvasTexture = nullptr;
	mActiveCanvasSourceTexture = nullptr;
	mPendingViewSnapshotCanvas = nullptr;
	mPathTracingLevelPreloadPending = false;
	mPathTracingWeaponLightEventsEnqueuedThisFrame = 0;
}

uint32_t NRIRenderDevice::GetQueuedFrameIndex(uint64_t frameIndex) const
{
	return mQueuedFrames.empty() ? 0 : (uint32_t)(frameIndex % mQueuedFrames.size());
}

void NRIRenderDevice::SelectQueuedFrame(uint32_t queuedFrameIndex)
{
	if (mQueuedFrames.empty())
	{
		mCurrentQueuedFrameIndex = 0;
		mCommandAllocator = nullptr;
		mCommandBuffer = nullptr;
		return;
	}

	mCurrentQueuedFrameIndex = queuedFrameIndex % (uint32_t)mQueuedFrames.size();
	QueuedFrame& queuedFrame = mQueuedFrames[mCurrentQueuedFrameIndex];
	mCommandAllocator = queuedFrame.commandAllocator;
	mCommandBuffer = queuedFrame.commandBuffer;
}
