#include "nri_renderdevice.h"

#include "../renderer/nri_renderer.h"
#include "perf_capture.h"

void NRIRenderDevice::CaptureCompactPerfRendererStats(bool rendered)
{
	const PerfCompactCaptureToken token = PerfCompactCaptureGetCurrentToken();
	if (!token || mRenderer == nullptr) return;

	const auto& shell = mRenderer->GetLastPerfShellTraceStats();
	const auto& resource = mRenderer->GetLastPerfResourceTraceStats();
	PerfCompactNriStats stats = {};
	stats.frame = mLastFrameBoundaryStats.frameNumber;
	stats.traceRendererFrame = shell.traceRendererFrame;
	stats.traceSettingsKey = shell.traceSettingsKey;
	stats.traceWorkloadKey = shell.traceWorkloadKey;
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
