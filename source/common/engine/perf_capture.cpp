#include "perf_capture.h"

#include "c_cvars.h"
#include "printf.h"

#include <algorithm>
#include <array>
#include <limits>

CUSTOM_CVAR(Int, perf_compactframes, 0, 0)
{
	if (self < 0) self = 0;
	else if (self > 2048) self = 2048;
}

// Delay the next compact capture by a bounded number of presentations. This
// lets a fixed-simulation oracle settle every queued renderer lane before its
// first accepted sample without thawing the simulation clock.
CUSTOM_CVAR(Int, perf_compactwarmupframes, 0, 0)
{
	if (self < 0) self = 0;
	else if (self > 2048) self = 2048;
}

namespace
{
	constexpr uint32_t MaxRecords = 4096;
	constexpr uint32_t MaxFirstUseRecords = 4096;
	constexpr uint32_t MaxFirstUseDrainFrames = 32;
	constexpr uint32_t MinReadbackDrainFrames = 3;

	struct Record
	{
		PerfCompactOuterFrame outer;
		PerfCompactNriStats nri;
		PerfCompactBoundaryStats boundary;
		PerfCompactGpuTiming gpu;
		uint32_t expectedGpuSegments = 0;
		uint32_t resolvedGpuSegments = 0;
		uint32_t eligibleIndex = std::numeric_limits<uint32_t>::max();
		bool eligible = false;
	};

	struct FirstUseRecord
	{
		PerfCompactFirstUseRecord value;
		uint32_t outerRecordIndex = 0;
	};

	enum class CaptureState : uint8_t { Idle, Active, Draining, Aborted };
	struct Capture
	{
		std::array<Record, MaxRecords> records = {};
		std::array<FirstUseRecord, MaxFirstUseRecords> firstUseRecords = {};
		std::array<uint64_t, MaxFirstUseRecords> openFirstUseEvents = {};
		CaptureState state = CaptureState::Idle;
		uint64_t epoch = 0;
		uint64_t nextFirstUseEvent = 1;
		uint32_t requested = 0, observed = 0, eligible = 0, pendingGpu = 0;
		uint32_t firstUseCount = 0, firstUseDropped = 0, openFirstUseCount = 0;
		uint32_t firstUseDrainFrames = 0;
		uint32_t readbackDrainFrames = 0;
		uint32_t rejectState = 0, rejectLevelRendered = 0, rejectNriActive = 0;
		uint32_t rejectNriInvalid = 0, rejectNriNotRendered = 0, rejectBoundaryInvalid = 0;
		uint32_t rejectNotPathTraced = 0, rejectPresent = 0, rejectFrameJoin = 0;
		PerfCompactCaptureToken current;
		const char* abortReason = "none";
	};
	Capture gCapture;

	bool TokenMatches(const PerfCompactCaptureToken& token)
	{
		return token && token.epoch == gCapture.epoch && token.recordIndex < gCapture.observed;
	}

	void ResetCapture()
	{
		const uint64_t epoch = gCapture.epoch;
		for (Record& record : gCapture.records) record = {};
		for (FirstUseRecord& record : gCapture.firstUseRecords) record = {};
		gCapture.openFirstUseEvents.fill(0);
		gCapture.state = CaptureState::Idle;
		gCapture.requested = 0;
		gCapture.observed = 0;
		gCapture.eligible = 0;
		gCapture.pendingGpu = 0;
		gCapture.nextFirstUseEvent = 1;
		gCapture.firstUseCount = 0;
		gCapture.firstUseDropped = 0;
		gCapture.openFirstUseCount = 0;
		gCapture.firstUseDrainFrames = 0;
		gCapture.readbackDrainFrames = 0;
		gCapture.rejectState = 0;
		gCapture.rejectLevelRendered = 0;
		gCapture.rejectNriActive = 0;
		gCapture.rejectNriInvalid = 0;
		gCapture.rejectNriNotRendered = 0;
		gCapture.rejectBoundaryInvalid = 0;
		gCapture.rejectNotPathTraced = 0;
		gCapture.rejectPresent = 0;
		gCapture.rejectFrameJoin = 0;
		gCapture.current = {};
		gCapture.abortReason = "none";
		gCapture.epoch = epoch;
	}

	int32_t FindOpenFirstUseEvent(uint64_t eventId)
	{
		for (uint32_t index = 0; index < gCapture.openFirstUseCount; ++index)
			if (gCapture.openFirstUseEvents[index] == eventId) return (int32_t)index;
		return -1;
	}

	bool SameFirstUseRecord(const PerfCompactFirstUseRecord& a, const PerfCompactFirstUseRecord& b)
	{
		return a.eventId == b.eventId && a.domain == b.domain && a.stage == b.stage &&
			a.state == b.state && a.rendererFrame == b.rendererFrame &&
			a.producerFrame == b.producerFrame && a.submittedFence == b.submittedFence &&
			a.publicationFrame == b.publicationFrame;
	}

	void MeasureFirstUseClosure(uint32_t& pending, uint32_t& duplicates, uint32_t& unresolved)
	{
		pending = duplicates = unresolved = 0;
		for (uint32_t i = 0; i < gCapture.firstUseCount; ++i)
		{
			const PerfCompactFirstUseRecord& event = gCapture.firstUseRecords[i].value;
			for (uint32_t j = i + 1; j < gCapture.firstUseCount; ++j)
			{
				if (SameFirstUseRecord(event, gCapture.firstUseRecords[j].value))
				{
					duplicates++;
					break;
				}
			}
			bool firstOccurrence = true;
			for (uint32_t j = 0; j < i; ++j)
			{
				if (gCapture.firstUseRecords[j].value.eventId == event.eventId)
				{
					firstOccurrence = false;
					break;
				}
			}
			if (!firstOccurrence) continue;

			uint32_t begins = 0, ends = 0;
			bool submitted = false, published = false, abandoned = false;
			for (uint32_t j = i; j < gCapture.firstUseCount; ++j)
			{
				const PerfCompactFirstUseRecord& candidate = gCapture.firstUseRecords[j].value;
				if (candidate.eventId != event.eventId) continue;
				if ((candidate.flags & PerfCompactFirstUseBegin) != 0) begins++;
				if ((candidate.flags & PerfCompactFirstUseEnd) != 0) ends++;
				submitted = submitted || candidate.submittedFence != 0;
				published = published || candidate.publicationFrame != 0;
				abandoned = abandoned || candidate.state == PerfCompactFirstUseState::Cancelled ||
					candidate.state == PerfCompactFirstUseState::Failed;
			}
			if (begins > ends) pending++;
			if (submitted && !published && !abandoned) unresolved++;
		}
	}

	void FlushFirstUseRecords()
	{
		for (uint32_t index = 0; index < gCapture.firstUseCount; ++index)
		{
			const FirstUseRecord& stored = gCapture.firstUseRecords[index];
			const PerfCompactFirstUseRecord& event = stored.value;
			const uint64_t outerFrame = stored.outerRecordIndex < gCapture.observed ?
				gCapture.records[stored.outerRecordIndex].outer.traceFrame : 0;
			const uint64_t joinedRendererFrame =
				stored.outerRecordIndex < gCapture.observed &&
				gCapture.records[stored.outerRecordIndex].nri.valid &&
				gCapture.records[stored.outerRecordIndex].nri.frame != 0 ?
				gCapture.records[stored.outerRecordIndex].nri.frame : event.rendererFrame;
			Printf("PERF compact first use NRI: schema=1 record=%u event=%llu outer_frame=%llu nri_frame=%llu producer_frame=%llu queued_slot=%u submitted_fence=%llu publication_frame=%llu domain=%u stage=%u state=%u flags=0x%x actor=%llu source=%llu mesh=%llu material=%llu signature=%llu texture=%llu cpu_ms=%.3f bytes=%llu count=%u compact=1 epoch=%llu\n",
				index, (unsigned long long)event.eventId, (unsigned long long)outerFrame,
				(unsigned long long)joinedRendererFrame, (unsigned long long)event.producerFrame,
				event.queuedSlot, (unsigned long long)event.submittedFence,
				(unsigned long long)event.publicationFrame, (uint32_t)event.domain,
				(uint32_t)event.stage, (uint32_t)event.state, event.flags,
				(unsigned long long)event.actorLifecycleKey, (unsigned long long)event.sourceKey,
				(unsigned long long)event.meshKey, (unsigned long long)event.materialKey,
				(unsigned long long)event.validatedSignature, (unsigned long long)event.textureKey,
				event.cpuMs, (unsigned long long)event.bytes, event.count,
				(unsigned long long)gCapture.epoch);
		}
	}

	void FlushRecordOwners(const Record& record)
	{
		const auto& outer = record.outer;
		const auto& nri = record.nri;
		const auto& boundary = record.boundary;
		Printf("PERF render trace NRI: frame=%llu nri_frame=%llu total=%.3f init=%.3f res=%.3f state=%.3f capture=%.3f geo=%.3f mats=%.3f textures=%.3f buffers=%.3f as=%.3f graph=%.3f wait=%.3f wait_present=%.3f acquire=%.3f submit=%.3f present=%.3f trace=%.3f denoise=%.3f compose=%.3f upscale=%.3f final=%.3f compact=1 epoch=%llu sample=%u\n",
			(unsigned long long)outer.traceFrame, (unsigned long long)nri.frame,
			outer.nriTotalMs, outer.nriInitializeMs,
			outer.nriFrameResourcesMs, outer.nriUpdateStateMs, outer.nriSceneCaptureMs,
			outer.nriGeometryBuildMs, outer.nriMaterialBuildMs, outer.nriSceneTexturesMs,
			outer.nriSceneBuffersMs, outer.nriAccelerationMs, outer.nriFrameGraphMs,
			boundary.waitMs, boundary.waitPresentMs, boundary.acquireMs, boundary.submitMs,
			boundary.presentMs, outer.nriTraceMs, outer.nriDenoiseMs, outer.nriComposeMs,
			outer.nriUpscaleMs, outer.nriFinalMs, (unsigned long long)gCapture.epoch,
			record.eligibleIndex);
		Printf("PERF pt shell trace NRI: frame=%llu nri_frame=%llu total=%.3f init=%.3f map=%.3f state=%.3f select=%.3f lights=%.3f frame_graph=%.3f post_diag=%.3f unattributed=%.3f active_prims=%u dynamic_prims=%u materials=%u instances=%u compact=1 epoch=%llu sample=%u\n",
			(unsigned long long)outer.traceFrame, (unsigned long long)nri.frame,
			nri.totalMs, nri.initMs, nri.mapMs, nri.stateMs,
			nri.selectMs, nri.lightsMs, nri.frameGraphMs, nri.postDiagnosticsMs,
			nri.unattributedMs, nri.activePrimitives, nri.dynamicPrimitives,
			nri.activeMaterials, nri.sceneInstances, (unsigned long long)gCapture.epoch,
			record.eligibleIndex);
		Printf("PERF pt scene select accounting NRI: frame=%llu nri_frame=%llu total=%.3f mutation=%.3f mutation_discovery=%.3f mutation_budget=%.3f mutation_analyze=%.3f mutation_structural_ms=%.3f mutation_material_ms=%.3f mutation_resident_ms=%.3f mutation_commit=%.3f dynamic_capture=%.3f persistent_batch=%.3f material_bridge=%.3f textures=%.3f buffer_upload=%.3f persistent_voxel_as=%.3f dynamic_as=%.3f world_tlas=%.3f scene_data=%.3f state_commit=%.3f unaccounted=%.3f structural=%u material=%u resident=%u candidates=%u analyzed=%u sweep=%u candidate_active=%u candidate_visible=%u candidate_startup_visible=%u candidate_unresolved=%u candidate_static_animated=%u candidate_sector_dirty=%u candidate_section_dirty=%u candidate_dragged=%u candidate_signature_watch=%u candidate_deferred_material=%u candidate_deferred_structural=%u structural_chunk0=%u structural_reason0=0x%x structural_trigger0=0x%x structural_chunk1=%u structural_reason1=0x%x structural_trigger1=0x%x structural_chunk2=%u structural_reason2=0x%x structural_trigger2=0x%x structural_chunk3=%u structural_reason3=0x%x structural_trigger3=0x%x upload_bytes=%llu compact=1 epoch=%llu sample=%u\n",
			(unsigned long long)outer.traceFrame, (unsigned long long)nri.frame,
			nri.selectMs, nri.mutationMs, nri.mutationDiscoveryMs, nri.mutationBudgetMs,
			nri.mutationAnalyzeMs, nri.mutationStructuralMs, nri.mutationMaterialMs,
			nri.mutationResidentMs, nri.mutationCommitMs, nri.dynamicCaptureMs,
			nri.persistentBatchMs, nri.materialBridgeMs, nri.texturesMs, nri.bufferUploadMs,
			nri.persistentVoxelAsMs, nri.dynamicAsMs, nri.worldTlasMs, nri.sceneDataMs,
			nri.stateCommitMs, nri.unattributedMs, nri.mutationStructural,
			nri.mutationMaterial, nri.mutationResident, nri.mutationCandidates,
			nri.mutationAnalyzed, nri.mutationSweep, nri.mutationCandidateActive,
			nri.mutationCandidateVisible, nri.mutationCandidateStartupVisible,
			nri.mutationCandidateUnresolved, nri.mutationCandidateStaticAnimated,
			nri.mutationCandidateSectorDirty, nri.mutationCandidateSectionDirty,
			nri.mutationCandidateDragged, nri.mutationCandidateSignatureWatch,
			nri.mutationCandidateDeferredMaterial, nri.mutationCandidateDeferredStructural,
			nri.mutationStructuralChunk[0], nri.mutationStructuralReason[0], nri.mutationStructuralTrigger[0],
			nri.mutationStructuralChunk[1], nri.mutationStructuralReason[1], nri.mutationStructuralTrigger[1],
			nri.mutationStructuralChunk[2], nri.mutationStructuralReason[2], nri.mutationStructuralTrigger[2],
			nri.mutationStructuralChunk[3], nri.mutationStructuralReason[3], nri.mutationStructuralTrigger[3],
			(unsigned long long)nri.sceneUploadBytes, (unsigned long long)gCapture.epoch,
			record.eligibleIndex);
		Printf("PERF pt voxel pressure compact NRI: frame=%llu nri_frame=%llu reason=0x%x flags=0x%x admission_rows=%u resource_rows=%u compact=1 epoch=%llu sample=%u\n",
			(unsigned long long)outer.traceFrame, (unsigned long long)nri.frame,
			nri.voxelPressureReason, nri.voxelPressureFlags, nri.voxelPressureEntries,
			nri.voxelPressureResources,
			(unsigned long long)gCapture.epoch, record.eligibleIndex);
		Printf("PERF pt voxel batch compact NRI: frame=%llu nri_frame=%llu total=%.3f pump=%.3f cache_entries=%.3f sort=%.3f instance_sync=%.3f existing_actors=%.3f actor_loop=%.3f material_variant=%.3f mesh_admission=%.3f material_bridge=%.3f state=%.3f compact=1 epoch=%llu sample=%u\n",
			(unsigned long long)outer.traceFrame, (unsigned long long)nri.frame,
			nri.persistentBatchMs, nri.voxelAdmissionPumpMs, nri.voxelBatchCacheEntryMs,
			nri.voxelBatchSortMs, nri.voxelBatchInstanceSyncMs, nri.voxelBatchExistingActorMapMs,
			nri.voxelBatchActorLoopMs, nri.voxelBatchMaterialVariantMs,
			nri.voxelBatchMeshAdmissionMs, nri.voxelBatchMaterialBridgeMs, nri.voxelBatchStateMs,
			(unsigned long long)gCapture.epoch, record.eligibleIndex);
		Printf("PERF pt mutation preparation compact NRI: frame=%llu nri_frame=%llu geometry=%.3f bridge=%.3f portal=%.3f deformer=%.3f materials=%.3f compact=1 epoch=%llu sample=%u\n",
			(unsigned long long)outer.traceFrame, (unsigned long long)nri.frame,
			nri.mutationGeometryBuildMs, nri.mutationGeometryBridgeMs,
			nri.mutationPortalAssignMs, nri.mutationDeformerCanonicalMs,
			nri.mutationMaterialBuildMs,
			(unsigned long long)gCapture.epoch, record.eligibleIndex);
		Printf("PERF pt resource waits NRI: frame=%llu nri_frame=%llu total=%u/%.3f compact=1 epoch=%llu sample=%u\n",
			(unsigned long long)outer.traceFrame, (unsigned long long)nri.frame,
			nri.resourceWaitCalls, nri.resourceWaitMs,
			(unsigned long long)gCapture.epoch, record.eligibleIndex);
		Printf("PERF pt trace workload NRI: frame=%llu nri_frame=%llu renderer_frame=%llu schema=5 settings_key=%llu workload_key=%llu render_w=%u render_h=%u output_w=%u output_h=%u dispatch_x=%u dispatch_y=%u dispatch_z=%u light_bounces=%u mirror_bounces=%u portal_depth=%u emissive_samples=%u emissive_requested=%u emissive_budget=%u indirect_requested=%u indirect_effective=%u indirect_active=%u hit_recon=%u runtime_lights=%u light_tiles_x=%u light_tiles_y=%u light_tile_size=%u light_tile_indices=%u light_tile_max=%u light_shadow_budget=%u light_shadow_candidates=%u light_shadow_selected=%u light_shadow_overflow=%u light_shadow_tile_max=%u light_shadow_selected_tile_max=%u light_shadow_selection_hash=%llu light_shadow_retained=%u light_shadow_replaced=%u light_shadow_expired=%u light_shadow_retained_hash=%llu light_shadow_replaced_hash=%llu light_shadow_expired_hash=%llu emissive_prims=%u emissive_power=%.3f voxel_occurrences=%u voxel_instance_prims=%llu voxel_occurrence_control=%u flags=%u debug=%u bootstrap=%u upscaler=%u upscaler_mode=%u denoiser=%u direct_scene=%u directional=%u directional_shadow=%u split_shadow=%u fast_emissive_shadow=%u visible_chunk_gate=%u compact=1 epoch=%llu sample=%u\n",
			(unsigned long long)outer.traceFrame, (unsigned long long)nri.frame,
			(unsigned long long)nri.traceRendererFrame,
			(unsigned long long)nri.traceSettingsKey,
			(unsigned long long)nri.traceWorkloadKey,
			nri.traceRenderWidth, nri.traceRenderHeight, nri.traceOutputWidth, nri.traceOutputHeight,
			nri.traceDispatchX, nri.traceDispatchY, nri.traceDispatchZ,
			nri.traceLightBounces, nri.traceMirrorBounces, nri.tracePortalDepth, nri.traceEmissiveSamples,
			nri.traceEmissiveRequestedSamples, nri.traceEmissivePrimaryBudget,
			nri.traceIndirectSamplingRequestedMode,
			nri.traceIndirectSamplingEffectiveMode, nri.traceIndirectSamplingActiveMode,
			nri.traceHitDistanceReconstructionMode,
			nri.traceRuntimeLights, nri.traceRuntimeLightTilesX, nri.traceRuntimeLightTilesY,
			nri.traceRuntimeLightTileSize, nri.traceRuntimeLightTileIndices, nri.traceRuntimeLightMaxOccupancy,
			nri.traceRuntimeLightShadowBudget, nri.traceRuntimeLightShadowCandidates,
			nri.traceRuntimeLightShadowSelected, nri.traceRuntimeLightShadowOverflow,
			nri.traceRuntimeLightShadowTileMax, nri.traceRuntimeLightShadowSelectedTileMax,
			(unsigned long long)nri.traceRuntimeLightShadowSelectionHash,
			nri.traceRuntimeLightShadowRetained, nri.traceRuntimeLightShadowReplaced,
			nri.traceRuntimeLightShadowExpired,
			(unsigned long long)nri.traceRuntimeLightShadowRetainedKeyHash,
			(unsigned long long)nri.traceRuntimeLightShadowReplacedKeyHash,
			(unsigned long long)nri.traceRuntimeLightShadowExpiredKeyHash,
			nri.traceEmissivePrimitiveCount, nri.traceEmissiveTotalPower,
			nri.traceVoxelOccurrences, (unsigned long long)nri.traceVoxelInstancePrimitives,
			nri.traceVoxelOccurrenceControl,
			nri.traceFlags, nri.traceDebugMode, nri.traceBootstrapMode,
			nri.traceUpscalerKind, nri.traceUpscalerMode, nri.traceDenoiserMode,
			nri.traceDirectScene, nri.traceDirectional, nri.traceDirectionalShadow,
			nri.traceSplitShadow, nri.traceFastEmissiveShadow, nri.traceVisibleChunkGate,
			(unsigned long long)gCapture.epoch, record.eligibleIndex);
		Printf("PERF pt gpu timing NRI: frame=%llu nri_frame=%llu segment=%.3f scene=%.3f trace=%.3f trace_dispatch=%.3f denoise=%.3f compose=%.3f upscale=%.3f final=%.3f smoke_simulation=%.3f smoke_volume=%.3f smoke_total=%.3f smoke_detail_total=%.3f smoke_grid_allocate=%.3f smoke_grid_initialize=%.3f smoke_grid_deposit=%.3f smoke_grid_halo=%.3f smoke_grid_simulate=%.3f smoke_grid_rebuild=%.3f smoke_dormant_archive=%.3f smoke_dormant_promote=%.3f smoke_dormant_evolve=%.3f smoke_world_active=%.3f smoke_world_link=%.3f smoke_world_proposal=%.3f smoke_world_seed=%.3f smoke_world_temporal=%.3f smoke_world_filter=%.3f smoke_world_scatter=%.3f smoke_carrier=%.3f smoke_view_prepare=%.3f smoke_materialize=%.3f smoke_analytic_materialize=%.3f smoke_view_point=%.3f smoke_view_directional=%.3f smoke_view_direct_reuse=%.3f smoke_view_emissive=%.3f smoke_analytic_emissive_build=%.3f smoke_analytic_emissive_apply=%.3f smoke_view_indirect=%.3f smoke_integrate=%.3f smoke_reconstruction=%.3f segments=%u invalid=%u dropped=%u resolved=%u expected=%u compact=1 epoch=%llu sample=%u\n",
			(unsigned long long)outer.traceFrame, (unsigned long long)nri.frame,
			record.gpu.segmentMs, record.gpu.sceneMs,
			record.gpu.traceMs, record.gpu.traceDispatchMs, record.gpu.denoiseMs, record.gpu.compositionMs,
			record.gpu.upscaleMs, record.gpu.finalMs,
			record.gpu.smokeSimulationMs, record.gpu.smokeVolumeMs,
			record.gpu.smokeSimulationMs + record.gpu.smokeVolumeMs,
			record.gpu.SmokeDetailTotalMs(),
			record.gpu.smokeGridAllocateMs, record.gpu.smokeGridInitializeMs,
			record.gpu.smokeGridDepositMs, record.gpu.smokeGridHaloMs,
			record.gpu.smokeGridSimulateMs, record.gpu.smokeGridRebuildMs,
			record.gpu.smokeDormantArchiveMs, record.gpu.smokeDormantPromoteMs,
			record.gpu.smokeDormantEvolveMs,
			record.gpu.smokeWorldActiveMs, record.gpu.smokeWorldLinkMs,
			record.gpu.smokeWorldProposalMs, record.gpu.smokeWorldSeedMs,
			record.gpu.smokeWorldTemporalMs, record.gpu.smokeWorldFilterMs,
			record.gpu.smokeWorldScatterMs, record.gpu.smokeCarrierMs,
			record.gpu.smokeViewPrepareMs, record.gpu.smokeMaterializeMs,
			record.gpu.smokeAnalyticMaterializeMs,
			record.gpu.smokeViewPointMs, record.gpu.smokeViewDirectionalMs,
			record.gpu.smokeViewDirectReuseMs, record.gpu.smokeViewEmissiveMs,
			record.gpu.smokeAnalyticEmissiveBuildMs, record.gpu.smokeAnalyticEmissiveApplyMs,
			record.gpu.smokeViewIndirectMs, record.gpu.smokeIntegrateMs,
			record.gpu.smokeReconstructionMs,
			record.gpu.segmentCount,
			record.gpu.invalidPairs, record.gpu.droppedScopes, record.resolvedGpuSegments,
			record.expectedGpuSegments, (unsigned long long)gCapture.epoch,
			record.eligibleIndex);
	}

	void FlushLoopRecord(const Record& record)
	{
		const auto& f = record.outer;
		Printf("PERF loop trace: frame=%llu presentation_gen=%llu simulation_gen=%llu engine_gen=%llu state=level gametic=%d startframe_ms=%.3f try_ms=%.3f try_traced_ms=%.3f display_ms=%.3f display_begin_ms=%.3f display_render_ms=%.3f display_overlay_ms=%.3f display_update_ms=%.3f starttic_ms=%.3f music_ms=%.3f frame_ms=%.3f do_wait=%d realtics=%d avail=%d counts=%d ticks=%d wait_loops=%d zero_return=%d wait_return=%d paused_return=%d fixed_return=%d fixed_tail_suppressed=%u display_skip=%d level_rendered=1 compact=1 epoch=%llu sample=%u\n",
			(unsigned long long)f.traceFrame, (unsigned long long)f.presentationGeneration,
			(unsigned long long)f.simulationGeneration, (unsigned long long)f.engineGeneration,
			f.gametic, f.startFrameMs, f.tryMs, f.tryTracedMs, f.displayMs,
			f.displayBeginMs, f.displayRenderMs, f.displayOverlayMs, f.displayUpdateMs,
			f.startTicMs, f.musicMs, f.frameMs, f.doWait ? 1 : 0, f.realtics,
			f.availabletics, f.counts, f.ticks, f.waitLoops, f.zeroReturn ? 1 : 0,
			f.waitReturn ? 1 : 0, f.pausedReturn ? 1 : 0,
			f.fixedSimulationReturn ? 1 : 0, f.fixedSimulationSuppressedTailTicks,
			f.displaySkipped ? 1 : 0,
			(unsigned long long)gCapture.epoch, record.eligibleIndex);
	}
}

void PerfCompactCaptureFlushIfReady()
{
	if (gCapture.state != CaptureState::Draining && gCapture.state != CaptureState::Aborted) return;
	if (gCapture.state == CaptureState::Draining && gCapture.pendingGpu != 0) return;
	if (gCapture.state == CaptureState::Draining && gCapture.readbackDrainFrames < MinReadbackDrainFrames) return;
	if (gCapture.state == CaptureState::Draining && gCapture.openFirstUseCount != 0)
	{
		if (gCapture.firstUseDrainFrames < MaxFirstUseDrainFrames) return;
		gCapture.abortReason = "first-use-drain-exhausted";
		gCapture.state = CaptureState::Aborted;
	}
	if (gCapture.state == CaptureState::Draining)
	{
		for (uint32_t i = 0; i < gCapture.observed; ++i)
			if (gCapture.records[i].eligible) FlushRecordOwners(gCapture.records[i]);
		for (uint32_t i = 0; i < gCapture.observed; ++i)
			if (gCapture.records[i].eligible) FlushLoopRecord(gCapture.records[i]);
	}
	FlushFirstUseRecords();
	uint32_t firstUsePending = 0, firstUseDuplicates = 0, firstUseUnresolved = 0;
	MeasureFirstUseClosure(firstUsePending, firstUseDuplicates, firstUseUnresolved);
	Printf("PERF compact capture complete: epoch=%llu status=%s requested=%u eligible=%u observed=%u pending_gpu=%u dropped=0 readback_drain_frames=%u first_use_records=%u first_use_pending=%u first_use_dropped=%u first_use_duplicates=%u first_use_unresolved=%u first_use_drain_frames=%u reject_state=%u reject_level_rendered=%u reject_nri_active=%u reject_nri_invalid=%u reject_nri_not_rendered=%u reject_boundary_invalid=%u reject_not_path_traced=%u reject_present=%u reject_frame_join=%u reason=%s\n",
		(unsigned long long)gCapture.epoch,
		gCapture.state == CaptureState::Draining ? "complete" : "aborted",
		gCapture.requested, gCapture.eligible, gCapture.observed, gCapture.pendingGpu,
		gCapture.readbackDrainFrames,
		gCapture.firstUseCount, firstUsePending, gCapture.firstUseDropped,
		firstUseDuplicates, firstUseUnresolved, gCapture.firstUseDrainFrames,
		gCapture.rejectState, gCapture.rejectLevelRendered, gCapture.rejectNriActive,
		gCapture.rejectNriInvalid, gCapture.rejectNriNotRendered, gCapture.rejectBoundaryInvalid,
		gCapture.rejectNotPathTraced, gCapture.rejectPresent, gCapture.rejectFrameJoin,
		gCapture.abortReason);
	ResetCapture();
}

void PerfCompactCaptureBeginOuterFrame(uint64_t presentationGeneration)
{
	PerfCompactCaptureFlushIfReady();
	if (gCapture.state == CaptureState::Idle &&
		(int)perf_compactframes > 0 &&
		(int)perf_compactwarmupframes > 0)
	{
		perf_compactwarmupframes = (int)perf_compactwarmupframes - 1;
		gCapture.current = {};
		return;
	}
	if (gCapture.state == CaptureState::Idle && (int)perf_compactframes > 0)
	{
		const uint32_t requested = (uint32_t)(int)perf_compactframes;
		perf_compactframes = 0;
		perf_compactwarmupframes = 0;
		ResetCapture();
		if (++gCapture.epoch == 0) gCapture.epoch = 1;
		gCapture.requested = requested;
		gCapture.state = CaptureState::Active;
	}
	const bool drainFirstUse = gCapture.state == CaptureState::Draining &&
		gCapture.openFirstUseCount != 0 && gCapture.firstUseDrainFrames < MaxFirstUseDrainFrames;
	if (gCapture.state == CaptureState::Draining && gCapture.readbackDrainFrames < MinReadbackDrainFrames)
		gCapture.readbackDrainFrames++;
	if (gCapture.state != CaptureState::Active && !drainFirstUse)
	{
		gCapture.current = {};
		return;
	}
	if (gCapture.observed >= MaxRecords)
	{
		PerfCompactCaptureAbort("capacity-exhausted");
		return;
	}
	const uint32_t index = gCapture.observed++;
	gCapture.records[index] = {};
	gCapture.current = { gCapture.epoch, presentationGeneration, index };
	if (drainFirstUse) gCapture.firstUseDrainFrames++;
}

bool PerfCompactCaptureTimingActive()
{
	return (gCapture.state == CaptureState::Active || gCapture.state == CaptureState::Draining) &&
		(bool)gCapture.current;
}

bool PerfCompactCaptureReadbackDrainActive()
{
	return gCapture.state == CaptureState::Draining &&
		(gCapture.pendingGpu != 0 || gCapture.readbackDrainFrames <= MinReadbackDrainFrames);
}

PerfCompactCaptureToken PerfCompactCaptureGetCurrentToken() { return PerfCompactCaptureTimingActive() ? gCapture.current : PerfCompactCaptureToken{}; }

void PerfCompactCaptureNoteNri(const PerfCompactCaptureToken& token, const PerfCompactNriStats& stats)
{
	if (TokenMatches(token)) gCapture.records[token.recordIndex].nri = stats;
}

void PerfCompactCaptureNoteBoundary(const PerfCompactCaptureToken& token, const PerfCompactBoundaryStats& stats)
{
	if (TokenMatches(token)) gCapture.records[token.recordIndex].boundary = stats;
}

void PerfCompactCaptureExpectGpuSegment(const PerfCompactCaptureToken& token)
{
	if (!TokenMatches(token)) return;
	gCapture.records[token.recordIndex].expectedGpuSegments++;
	gCapture.pendingGpu++;
}

void PerfCompactCaptureResolveGpuSegment(const PerfCompactCaptureToken& token, const PerfCompactGpuTiming& timing)
{
	if (!TokenMatches(token)) return;
	Record& r = gCapture.records[token.recordIndex];
	if (r.resolvedGpuSegments >= r.expectedGpuSegments) return;
	r.resolvedGpuSegments++;
	r.gpu.segmentMs += timing.segmentMs; r.gpu.sceneMs += timing.sceneMs;
	r.gpu.traceMs += timing.traceMs; r.gpu.traceDispatchMs += timing.traceDispatchMs;
	r.gpu.denoiseMs += timing.denoiseMs;
	r.gpu.compositionMs += timing.compositionMs; r.gpu.upscaleMs += timing.upscaleMs;
	r.gpu.finalMs += timing.finalMs; r.gpu.segmentCount += timing.segmentCount;
	r.gpu.smokeSimulationMs += timing.smokeSimulationMs;
	r.gpu.smokeVolumeMs += timing.smokeVolumeMs;
	r.gpu.smokeGridAllocateMs += timing.smokeGridAllocateMs;
	r.gpu.smokeGridInitializeMs += timing.smokeGridInitializeMs;
	r.gpu.smokeGridDepositMs += timing.smokeGridDepositMs;
	r.gpu.smokeGridHaloMs += timing.smokeGridHaloMs;
	r.gpu.smokeGridSimulateMs += timing.smokeGridSimulateMs;
	r.gpu.smokeGridRebuildMs += timing.smokeGridRebuildMs;
	r.gpu.smokeDormantArchiveMs += timing.smokeDormantArchiveMs;
	r.gpu.smokeDormantPromoteMs += timing.smokeDormantPromoteMs;
	r.gpu.smokeDormantEvolveMs += timing.smokeDormantEvolveMs;
	r.gpu.smokeWorldActiveMs += timing.smokeWorldActiveMs;
	r.gpu.smokeWorldLinkMs += timing.smokeWorldLinkMs;
	r.gpu.smokeWorldProposalMs += timing.smokeWorldProposalMs;
	r.gpu.smokeWorldSeedMs += timing.smokeWorldSeedMs;
	r.gpu.smokeWorldTemporalMs += timing.smokeWorldTemporalMs;
	r.gpu.smokeWorldFilterMs += timing.smokeWorldFilterMs;
	r.gpu.smokeWorldScatterMs += timing.smokeWorldScatterMs;
	r.gpu.smokeCarrierMs += timing.smokeCarrierMs;
	r.gpu.smokeViewPrepareMs += timing.smokeViewPrepareMs;
	r.gpu.smokeMaterializeMs += timing.smokeMaterializeMs;
	r.gpu.smokeAnalyticMaterializeMs += timing.smokeAnalyticMaterializeMs;
	r.gpu.smokeViewPointMs += timing.smokeViewPointMs;
	r.gpu.smokeViewDirectionalMs += timing.smokeViewDirectionalMs;
	r.gpu.smokeViewDirectReuseMs += timing.smokeViewDirectReuseMs;
	r.gpu.smokeViewEmissiveMs += timing.smokeViewEmissiveMs;
	r.gpu.smokeAnalyticEmissiveBuildMs += timing.smokeAnalyticEmissiveBuildMs;
	r.gpu.smokeAnalyticEmissiveApplyMs += timing.smokeAnalyticEmissiveApplyMs;
	r.gpu.smokeViewIndirectMs += timing.smokeViewIndirectMs;
	r.gpu.smokeIntegrateMs += timing.smokeIntegrateMs;
	r.gpu.smokeReconstructionMs += timing.smokeReconstructionMs;
	r.gpu.invalidPairs += timing.invalidPairs; r.gpu.droppedScopes += timing.droppedScopes;
	if (gCapture.pendingGpu > 0) gCapture.pendingGpu--;
}

uint64_t PerfCompactCaptureNoteFirstUse(const PerfCompactFirstUseRecord& record)
{
	if ((gCapture.state != CaptureState::Active && gCapture.state != CaptureState::Draining) ||
		!TokenMatches(gCapture.current)) return record.eventId;
	if (gCapture.state == CaptureState::Draining &&
		(record.eventId == 0 || (record.flags & PerfCompactFirstUseBegin) != 0))
	{
		return record.eventId;
	}
	PerfCompactFirstUseRecord stored = record;
	if (stored.eventId == 0)
	{
		stored.eventId = 0x8000000000000000ull |
			((gCapture.epoch & 0x7fffffffull) << 32) | gCapture.nextFirstUseEvent++;
	}
	const int32_t openIndex = FindOpenFirstUseEvent(stored.eventId);
	if (gCapture.state == CaptureState::Draining && openIndex < 0)
	{
		return stored.eventId;
	}
	if (gCapture.firstUseCount >= MaxFirstUseRecords)
	{
		gCapture.firstUseDropped++;
		return stored.eventId;
	}
	FirstUseRecord& destination = gCapture.firstUseRecords[gCapture.firstUseCount++];
	destination.value = stored;
	destination.outerRecordIndex = gCapture.current.recordIndex;
	if ((stored.flags & PerfCompactFirstUseBegin) != 0 && openIndex < 0 &&
		gCapture.openFirstUseCount < MaxFirstUseRecords)
	{
		gCapture.openFirstUseEvents[gCapture.openFirstUseCount++] = stored.eventId;
	}
	if ((stored.flags & PerfCompactFirstUseEnd) != 0)
	{
		const int32_t resolvedIndex = FindOpenFirstUseEvent(stored.eventId);
		if (resolvedIndex >= 0)
		{
			const uint32_t index = (uint32_t)resolvedIndex;
			gCapture.openFirstUseEvents[index] = gCapture.openFirstUseEvents[--gCapture.openFirstUseCount];
			gCapture.openFirstUseEvents[gCapture.openFirstUseCount] = 0;
		}
	}
	return stored.eventId;
}

void PerfCompactCaptureEndOuterFrame(const PerfCompactOuterFrame& frame)
{
	const PerfCompactCaptureToken token = gCapture.current;
	if (!TokenMatches(token)) return;
	Record& r = gCapture.records[token.recordIndex];
	r.outer = frame;
	r.eligible = gCapture.state == CaptureState::Active && frame.stateIsLevel && frame.levelRendered &&
		frame.nriActive && r.nri.valid && r.nri.rendered && r.boundary.valid &&
		r.boundary.pathTraced && r.boundary.presentOk && r.nri.frame == r.boundary.frame;
	if (gCapture.state == CaptureState::Active && !r.eligible)
	{
		if (!frame.stateIsLevel) gCapture.rejectState++;
		if (!frame.levelRendered) gCapture.rejectLevelRendered++;
		if (!frame.nriActive) gCapture.rejectNriActive++;
		if (!r.nri.valid) gCapture.rejectNriInvalid++;
		if (!r.nri.rendered) gCapture.rejectNriNotRendered++;
		if (!r.boundary.valid) gCapture.rejectBoundaryInvalid++;
		if (!r.boundary.pathTraced) gCapture.rejectNotPathTraced++;
		if (!r.boundary.presentOk) gCapture.rejectPresent++;
		if (r.nri.valid && r.boundary.valid && r.nri.frame != r.boundary.frame) gCapture.rejectFrameJoin++;
	}
	if (r.eligible) r.eligibleIndex = gCapture.eligible++;
	gCapture.current = {};
	if (gCapture.eligible >= gCapture.requested) gCapture.state = CaptureState::Draining;
}

void PerfCompactCaptureAbort(const char* reason)
{
	if (gCapture.state == CaptureState::Idle) return;
	gCapture.current = {};
	gCapture.abortReason = reason != nullptr ? reason : "unknown";
	gCapture.state = CaptureState::Aborted;
}
