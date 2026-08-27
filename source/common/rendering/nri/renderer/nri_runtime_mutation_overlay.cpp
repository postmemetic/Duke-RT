#include "nri_renderer.h"
#include "nri_cvars.h"

#include "../scene/nri_hash.h"
#include "../scene/nri_map_builder.h"
#include "../../hwrenderer/data/hw_clock.h"
#include "nri_render_geometry_helpers.h"
#include "nri_runtime_mutation_shared.h"
#include "nri_runtime_mutation_trace.h"
#include "c_cvars.h"
#include "gamecontrol.h"
#include "gamestate.h"
#include "hw_sections.h"
#include "mapinfo.h"
#include "printf.h"
#include "texturemanager.h"

#include <array>
#include <chrono>
#include <cstring>
#include <unordered_map>


namespace
{
	using namespace nri_runtime_mutation;
}

// Runtime mutation overlay renderer-service implementation.

bool NRIRenderer::BuildRuntimeMapMutationOverlay(nri_scene::GeometryData& outGeometry, nri_scene::MaterialBridgeData& outMaterials, bool* outResidentStaticSceneChanged)
{
	outGeometry = {};
	outMaterials = {};
	if (outResidentStaticSceneChanged != nullptr)
	{
		*outResidentStaticSceneChanged = false;
	}
	mRuntimeMutation.BeginFrameState();
	bool residentStaticSceneChanged = false;
	bool residentMaterialDirty = false;
	bool residentGeometryDirty = false;
	bool startupMaterialOnlyMutationDetected = false;
	std::vector<uint32_t> residentMaterialChunkListIndices;
	std::vector<uint32_t> animatedResidentApplyMaterialChunkListIndices;
	std::vector<uint32_t> residentGeometryChunkListIndices;
	mRuntimeMutation.ClearResidentGeometryUploadRanges();
	const NRIRuntimeMutationSettings runtimeMutationSettings = BuildNRIRuntimeMutationSettingsFromCVars();
	const bool tracePtPerf = ShouldTracePtPerf();
	const bool collectRuntimeMutationTiming = ShouldCollectRuntimeMutationPerfTiming();
	const bool collectRuntimeMutationCacheStats = tracePtPerf || (bool)nri_ptslowdowntrace;
	mLastPerfShellTraceStats.runtimeMutationDiscoveryMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationBudgetMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationCommitMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildVisibleMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildInvisibleMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildNearMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildFarMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildUnknownDistanceMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshVisibleMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshInvisibleMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshNearMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshFarMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshUnknownDistanceMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyVisibleMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyInvisibleMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyNearMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyFarMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyUnknownDistanceMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationNearDistance = runtimeMutationSettings.nearDistance;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyLiveBuildMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryBuildMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialBuildMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyBaselineCaptureMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyAtlasMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyAtlasBookkeepingMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyVertexIndexCopyMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyVertexCpuCopyMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyIndexCpuCopyMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyVertexStageMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyIndexStageMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyPrimitiveRewriteMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyPrimitiveCpuRewriteMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyPrimitiveStageMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryOrderHashMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyDownstreamBlasMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyDownstreamBlasSetupMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyDownstreamBlasFilterMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyDownstreamBlasCreateMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyDownstreamBlasScratchMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyDownstreamBlasBarrierMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyDownstreamBlasBuildMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationCandidateChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationAnalyzedChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationBackgroundSweepChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationDirtyChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationRebuiltChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationHeldChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationCandidateVisibleChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationCandidateInvisibleChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationCandidateNearChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationCandidateFarChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationCandidateUnknownDistanceChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationCandidateBoundsValidChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationCandidateBoundsInvalidChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationCandidateActiveReplacementChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationCandidateVisibleResidentValidationChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationCandidateStartupVisibleValidationChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationCandidateUnresolvedTextureChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationCandidateStaticAnimatedSuppressedChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationCandidateSectorDirtyChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationCandidateSectionDirtyChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationCandidateDraggedChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationCandidateSignatureWatchChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationCandidateBackgroundSweepSourceChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationCandidateDeferredMaterialChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationCandidateDeferredStructuralChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationDirtyVisibleChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationDirtyInvisibleChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationDirtyNearChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationDirtyFarChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationDirtyUnknownDistanceChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationDirtyActiveReplacementChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationDirtyBackgroundSweepChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildVisibleChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildInvisibleChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildNearChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildFarChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildUnknownDistanceChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildActiveReplacementChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildBackgroundSweepChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredCoalescedChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredFlushedChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredPromotedChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredPendingChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredNearChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredNearCoalescedChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredNearFlushedChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredNearPendingChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredNearBudget =
		runtimeMutationSettings.nearInvisibleStructuralBudget;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredFarChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredFarCoalescedChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredFarFlushedChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredFarPendingChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredFarBudget =
		runtimeMutationSettings.farStructuralBudget;
	mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredBudget =
		mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredNearBudget +
		mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredFarBudget;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshVisibleChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshInvisibleChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshNearChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshFarChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshUnknownDistanceChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshDeferredChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshDeferredCoalescedChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshDeferredFlushedChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshDeferredPendingChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshDeferredNearChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshDeferredNearCoalescedChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshDeferredNearFlushedChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshDeferredNearPendingChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshDeferredNearBudget =
		runtimeMutationSettings.nearInvisibleMaterialBudget;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshActiveReplacementChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshBackgroundSweepChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyVisibleChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyInvisibleChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyNearChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyFarChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyUnknownDistanceChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyActiveReplacementChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyBackgroundSweepChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshAnimatedChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshReplacementDeltaChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshHardwareCanvasChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralReplacementDeltaChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralReplacementViewChangedChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralStaticAnimatedModeFlipChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralExcludeStaticFlipChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralForcedTopologyChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralInvalidChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralMaterialOnlyChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralSectorMaterialOnlyChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralWallMaterialOnlyChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralMixedMaterialOnlyChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralGeometryOrDirtyChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationGeometryDirtySectorGeometryOnlyChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationGeometryDirtyWallGeometryOnlyChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationGeometryDirtySectorWallGeometryChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationGeometryDirtyDirtyOnlyChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationGeometryDirtyGeometryDirtyMixedChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationGeometryDirtyForceTopologyOnlyChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationGeometryDirtyRealCountChangeChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationGeometryDirtyWallsOnlyChangedChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationGeometryDirtyFlatsOnlyChangedChunks = 0;
	mLastPerfShellTraceStats.runtimeMutationGeometryDirtyWallsAndFlatsChangedChunks = 0;
	mLastPerfShellTraceStats.runtimeRecurringChunkTrackedCount = 0;
	mLastPerfShellTraceStats.runtimeRecurringChunkRecurringCount = 0;
	mLastPerfShellTraceStats.runtimeRecurringChunkVisitCount = 0;
	mLastPerfShellTraceStats.runtimeRecurringChunkUniqueStateCount = 0;
	mLastPerfShellTraceStats.runtimeRecurringChunkTransitionCount = 0;
	mLastPerfShellTraceStats.runtimeRecurringChunkRepeatedStateHitCount = 0;
	mLastPerfShellTraceStats.runtimeRecurringChunkAbaRecurrenceCount = 0;
	mLastPerfShellTraceStats.runtimeRecurringChunkMaxUniqueStateCount = 0;
	mLastPerfShellTraceStats.runtimeMutationHardwareCanvasChunkCount = 0;
	mLastPerfShellTraceStats.runtimeMutationStructuralReplacementDeltaReasonMaskOr = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialRefreshReasonMaskOr = 0;
	mLastPerfShellTraceStats.runtimeMutationInvalidForceTopologyCount = 0;
	mLastPerfShellTraceStats.runtimeMutationForceTopologyProofChecks = 0;
	mLastPerfShellTraceStats.runtimeMutationForceTopologyDowngradeCount = 0;
	mLastPerfShellTraceStats.runtimeMutationForceTopologyDowngradeNoopCount = 0;
	mLastPerfShellTraceStats.runtimeMutationForceTopologyDowngradeMaterialOnlyCount = 0;
	mLastPerfShellTraceStats.runtimeMutationForceTopologyDowngradePrepareFailedCount = 0;
	mLastPerfShellTraceStats.runtimeMutationForceTopologyDowngradeNoReplacementCount = 0;
	mLastPerfShellTraceStats.runtimeMutationForceTopologyDowngradeGeometryChangedCount = 0;
	mLastPerfShellTraceStats.runtimeMutationForceTopologyDowngradeUnsafeReasonCount = 0;
	mLastPerfShellTraceStats.runtimeMutationForceTopologyDowngradeInvisibleCount = 0;
	mLastPerfShellTraceStats.runtimeMutationForceTopologyDowngradeReasonMismatchCount = 0;
	mLastPerfShellTraceStats.runtimeMutationInvalidAppliedCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentNoopSkipCount = 0;
	mLastPerfShellTraceStats.runtimeMutationInvalidFailedCount = 0;
	mLastPerfShellTraceStats.runtimeMutationInvalidSyncSkipCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentNoopCandidateCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentNoopCandidateReasonMaskOr = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentNoopBlockNotAuthoritativeCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentNoopBlockResidentUnavailableCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentNoopBlockReplacementInvalidCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentNoopBlockExcludeStaticCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentNoopBlockSurfaceCountMismatch = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentNoopBlockMaterialCountMismatch = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentNoopBlockPrimitiveCountMismatch = 0;
	mLastPerfShellTraceStats.runtimeMutationValidMaterialCount = 0;
	mLastPerfShellTraceStats.runtimeMutationValidStructuralCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialOnlyCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyStructuralCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyFastMaterialOnlyCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyCertifiedMaterialOnlyCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplySlowMaterialOnlyCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialOnlyExclusiveCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialOnlyNoResidentChunkCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialOnlyInvalidReplacementCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialOnlyMaterialCountMismatchCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialPayloadHashCheckCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialPayloadHashSkipCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialPayloadHashMissCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyMaterialPayloadHashRejectCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryPayloadHashCheckCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryPayloadHashSkipCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryPayloadHashMissCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryPayloadHashRejectCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryPayloadHashBlasSkipCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryPayloadOrderCheckCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryPayloadOrderEquivalentCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryPayloadOrderMissCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyGeometryPayloadOrderRejectCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyVertexStageRangeCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyIndexStageRangeCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyPrimitiveStageRangeCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyVertexStageBytes = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyIndexStageBytes = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyPrimitiveStageBytes = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyCoalescedStageRangeCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyCoalescedStageRejectCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyCoalescedStageBytes = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyCoalescedStageGapBytes = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyStageMapMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyStageMemcpyMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyStageCommandMs = 0.0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyStageBatchCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyStageBatchRangeCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyStageCopyCommandCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyStageBarrierCommandCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyStageScratchGrowCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyStageScratchGrowBytes = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyPreserveGeometryCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyPreserveIndexCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyPreservePrimitiveCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasReuseCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasUpdateCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRefitOnlyCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRefitProbeCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRefitRejectNoPreviousAsCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRefitRejectIndexCountMismatchCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRefitRejectPrimitiveCountMismatchCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRefitRejectZeroIndexCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRefitRejectZeroPrimitiveCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRecreateCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRecreateNoPreviousAsCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRecreateRecoveredEmptyCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRecreateSliceMovedCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRecreateTopologyChangedCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRecreateForceTopologyCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasScratchQueryCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasScratchCacheHitCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasScratchCacheMissCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasScratchGrowCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasBuildCommandCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasScratchBarrierCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyKeepGeometrySliceCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyKeepMaterialSliceCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyEmptyRemovalCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyRecoverAttemptCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyRecoverSuccessCount = 0;
	mLastPerfShellTraceStats.runtimeMutationResidentApplyAtlasGrowCount = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialOnlyMismatchCount = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialOnlyMismatchRefreshCount = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialOnlyMismatchRebuildCount = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialOnlyMismatchFilteredWallOnlyCount = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialOnlyMismatchFilteredFlatOnlyCount = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialOnlyMismatchFilteredMixedCount = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialOnlyMismatchResidentWallOnlyCount = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialOnlyMismatchResidentFlatOnlyCount = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialOnlyMismatchResidentMixedCount = 0;
	mLastPerfShellTraceStats.runtimeAnimatedSuppressedActiveCount = 0;
	mLastPerfShellTraceStats.runtimeAnimatedSuppressionEmitCount = 0;
	mLastPerfShellTraceStats.runtimeAnimatedUniqueTouchedCount = 0;
	mLastPerfShellTraceStats.runtimeAnimatedMaterialRefreshCount = 0;
	mLastPerfShellTraceStats.runtimeAnimatedAttemptCount = 0;
	mLastPerfShellTraceStats.runtimeAnimatedSuppressedAttemptCount = 0;
	mLastPerfShellTraceStats.runtimeAnimatedUnsuppressedAttemptCount = 0;
	mLastPerfShellTraceStats.runtimeAnimatedResidentApplyCount = 0;
	mLastPerfShellTraceStats.runtimeAnimatedSuppressedResidentApplyCount = 0;
	mLastPerfShellTraceStats.runtimeAnimatedUnsuppressedResidentApplyCount = 0;
	mLastPerfShellTraceStats.runtimeAnimatedSyncSkipCount = 0;
	mLastPerfShellTraceStats.runtimeMutationActiveChunkCount = 0;
	mLastPerfShellTraceStats.runtimeMutationValidChunkCount = 0;
	mLastPerfShellTraceStats.runtimeMutationExcludedStaticChunkCount = 0;
	mLastPerfShellTraceStats.runtimeMutationCachedSurfaceCount = 0;
	mLastPerfShellTraceStats.runtimeMutationCachedTriangleCount = 0;
	mLastPerfShellTraceStats.runtimeMutationCachedMaterialCount = 0;
	mLastPerfShellTraceStats.runtimeMutationCachedMaterialStateCount = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialCacheHitCount = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialCacheMissCount = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialCacheStoreCount = 0;
	mLastPerfShellTraceStats.staticAnimatedResidentSliceCacheEntryCount = 0;
	mLastPerfShellTraceStats.staticAnimatedResidentSliceCacheHitCount = 0;
	mLastPerfShellTraceStats.staticAnimatedResidentSliceCacheMissCount = 0;
	mLastPerfShellTraceStats.staticAnimatedResidentSliceCacheStoreCount = 0;
	mLastPerfShellTraceStats.staticAnimatedResidentSliceApplyHitCount = 0;
	mLastPerfShellTraceStats.staticAnimatedResidentSliceApplyMissCount = 0;
	mLastPerfShellTraceStats.staticAnimatedResidentSliceSyncSkipHitCount = 0;
	mLastPerfShellTraceStats.staticAnimatedResidentGpuPayloadCacheHitCount = 0;
	mLastPerfShellTraceStats.staticAnimatedResidentGpuPayloadCacheMissCount = 0;
	mLastPerfShellTraceStats.staticAnimatedResidentGpuPayloadCacheStoreCount = 0;
	mLastPerfShellTraceStats.runtimeMutationPrimitiveCount = 0;
	mLastPerfShellTraceStats.runtimeMutationMaterialCount = 0;
	mLastPerfShellTraceStats.runtimeMutationTopEntries = {};
	mLastPerfShellTraceStats.runtimeSectorDirtyTruthEntries = {};
	mLastPerfShellTraceStats.runtimeAnimatedChurnEntries = {};
	mLastPerfShellTraceStats.runtimeMaterialOnlyMismatchEntries = {};
	mLastPerfShellTraceStats.runtimeResidentBlasRecreateEntries = {};
	mLastPerfShellTraceStats.runtimeResidentBlasRefitRejectEntries = {};
	mLastPerfShellTraceStats.runtimeStructuralRebuildEntries = {};
	mLastPerfShellTraceStats.runtimeGeometryDirtyEntries = {};
	mLastPerfShellTraceStats.runtimeRecurringChunkEntries = {};
	mLastPerfShellTraceStats.runtimeInvisibleProofEntries = {};
	if (mRuntimeRecurringChunkTrackerBuildSerial != mMapWorld.buildSerial)
	{
		mRuntimeRecurringChunkTrackerBuildSerial = mMapWorld.buildSerial;
		mRuntimeRecurringChunkTrackers.clear();
	}
	if (mRuntimeRecurringChunkTrackers.size() != mMapWorld.chunks.size())
	{
		mRuntimeRecurringChunkTrackers.resize(mMapWorld.chunks.size());
	}
	struct RuntimeAnimatedFrameTraceStats
	{
		bool touched = false;
		bool suppressed = false;
		uint32_t materialRefreshes = 0;
		uint32_t runtimeAttempts = 0;
		uint32_t residentApplies = 0;
		uint32_t syncSkips = 0;
	};
	std::vector<RuntimeAnimatedFrameTraceStats> runtimeAnimatedFrameStats;
	if (tracePtPerf)
	{
		runtimeAnimatedFrameStats.resize(mStaticSceneResidency.Registry().entries.size());
	}
	const auto recordRuntimeAnimatedFrame =
		[this, tracePtPerf, &runtimeAnimatedFrameStats](uint32_t chunkIndex,
			bool suppressed,
			bool materialRefresh,
			bool attempted,
			bool residentApply,
			bool syncSkip)
	{
		if (!tracePtPerf)
		{
			return;
		}
		if (chunkIndex >= runtimeAnimatedFrameStats.size())
		{
			return;
		}

		auto& entry = runtimeAnimatedFrameStats[chunkIndex];
		if (!entry.touched)
		{
			entry.touched = true;
			mLastPerfShellTraceStats.runtimeAnimatedUniqueTouchedCount++;
		}
		entry.suppressed = entry.suppressed || suppressed;
		if (materialRefresh)
		{
			entry.materialRefreshes++;
			mLastPerfShellTraceStats.runtimeAnimatedMaterialRefreshCount++;
		}
		if (attempted)
		{
			entry.runtimeAttempts++;
			mLastPerfShellTraceStats.runtimeAnimatedAttemptCount++;
			if (suppressed)
			{
				mLastPerfShellTraceStats.runtimeAnimatedSuppressedAttemptCount++;
			}
			else
			{
				mLastPerfShellTraceStats.runtimeAnimatedUnsuppressedAttemptCount++;
			}
		}
		if (residentApply)
		{
			entry.residentApplies++;
			mLastPerfShellTraceStats.runtimeAnimatedResidentApplyCount++;
			if (suppressed)
			{
				mLastPerfShellTraceStats.runtimeAnimatedSuppressedResidentApplyCount++;
			}
			else
			{
				mLastPerfShellTraceStats.runtimeAnimatedUnsuppressedResidentApplyCount++;
			}
		}
		if (syncSkip)
		{
			entry.syncSkips++;
			mLastPerfShellTraceStats.runtimeAnimatedSyncSkipCount++;
		}
	};
	const auto hasCachedResidentAnimatedSlice =
		[this](uint32_t chunkListIndex,
			uint64_t animatedGeometrySignature,
			uint64_t animatedMaterialSignature) -> bool
	{
		if (chunkListIndex >= mStaticMapScene.chunks.size())
		{
			return false;
		}

		const auto& chunkCache = mStaticMapScene.chunks[chunkListIndex];
		if (!chunkCache.active || chunkCache.materialCount == 0)
		{
			return false;
		}

		const uint64_t materialBridgeHash = HashMaterialBridgeSummary(chunkCache.materialBridge);
		for (const auto& cacheEntry : chunkCache.residentMaterialSliceCache)
		{
			if (cacheEntry.animatedGeometrySignature == animatedGeometrySignature &&
				cacheEntry.animatedMaterialSignature == animatedMaterialSignature &&
				cacheEntry.materialBridgeHash == materialBridgeHash &&
				cacheEntry.materialCount == chunkCache.materialCount)
			{
				return true;
			}
		}

		return false;
	};

	const auto recordRuntimeMutationTopEntry =
		[this](uint32_t chunkIndex,
			int32_t sectorIndex,
			uint32_t reasonMask,
			uint32_t sectionDirtyCount,
			uint32_t surfaceCount,
			uint32_t triangleCount,
			uint32_t materialCount,
			RuntimeMutationTraceAction action,
			bool forceTopology,
			bool residentMaterialDirty,
			bool residentGeometryDirty,
			bool recoveredEmpty)
	{
		if (!ShouldTracePtPerf())
		{
			return;
		}

		RuntimeMutationTopTraceEntry entry = {};
		entry.valid = true;
		entry.chunkIndex = chunkIndex;
		entry.sectorIndex = sectorIndex;
		entry.reasonMask = reasonMask;
		entry.sectionDirtyCount = sectionDirtyCount;
		entry.surfaceCount = surfaceCount;
		entry.triangleCount = triangleCount;
		entry.materialCount = materialCount;
		entry.action = action;
		entry.forceTopology = forceTopology;
		entry.residentMaterialDirty = residentMaterialDirty;
		entry.residentGeometryDirty = residentGeometryDirty;
		entry.recoveredEmpty = recoveredEmpty;
		InsertRankedTraceEntry(
			mLastPerfShellTraceStats.runtimeMutationTopEntries,
			entry,
			ScoreRuntimeMutationTopTraceEntry);
	};
	const auto recordRuntimeInvisibleProofEntry =
		[this](const nri_scene::PTMapChunk& mapChunk,
			uint32_t reasonMask,
			uint32_t sourceMask,
			const RuntimeMapMutationCache::ChunkReplacement& replacement,
			const ResidentMapChunkRegistry::Entry* residentEntry,
			bool visibleFloor,
			bool visibleCeiling,
			bool exactSignatureCached,
			bool exactSignatureMatch,
			bool animatedMaterialMatch,
			bool hardwareCanvas,
			bool portalChunk,
			bool sectorLightingCandidate,
			bool safeResidentNoopCandidate)
	{
		if (!ShouldTracePtPerf())
		{
			return;
		}

		RuntimeInvisibleProofTraceEntry entry = {};
		entry.valid = true;
		entry.chunkIndex = mapChunk.chunkIndex;
		entry.sectorIndex = mapChunk.sectorIndex;
		entry.reasonMask = reasonMask;
		entry.sourceMask = sourceMask;
		entry.previousSurfaceCount = replacement.surfaceCount;
		entry.previousTriangleCount = replacement.triangleCount;
		entry.previousMaterialCount = (uint32_t)replacement.materialBridge.materials.size();
		entry.residentPrimitiveCount = residentEntry != nullptr ? residentEntry->primitiveCount : 0u;
		entry.residentMaterialCount = residentEntry != nullptr ? residentEntry->materialCount : 0u;
		entry.replacementValid = replacement.valid;
		entry.residentAuthoritative = replacement.residentAuthoritative;
		entry.residentAvailable =
			residentEntry != nullptr &&
			residentEntry->valid &&
			residentEntry->active &&
			residentEntry->mappedInStaticScene;
		entry.visibleFloor = visibleFloor;
		entry.visibleCeiling = visibleCeiling;
		entry.exactSignatureCached = exactSignatureCached;
		entry.exactSignatureMatch = exactSignatureMatch;
		entry.animatedMaterialMatch = animatedMaterialMatch;
		entry.excludeStaticChunk = replacement.excludeStaticChunk;
		entry.staticAnimatedReplacement = replacement.staticAnimatedReplacement;
		entry.hasAnimatedTextureCandidates = residentEntry != nullptr && residentEntry->hasAnimatedTextureCandidates;
		entry.animatedRefreshSuppressed = residentEntry != nullptr && residentEntry->animatedRefreshSuppressed;
		entry.hardwareCanvas = hardwareCanvas;
		entry.portalChunk = portalChunk;
		entry.sectorLightingCandidate = sectorLightingCandidate;
		entry.safeResidentNoopCandidate = safeResidentNoopCandidate;
		InsertRankedTraceEntry(
			mLastPerfShellTraceStats.runtimeInvisibleProofEntries,
			entry,
			ScoreRuntimeInvisibleProofTraceEntry);
	};

	const auto recordSectorDirtyTruthEntry =
		[this](uint32_t chunkIndex,
			int32_t sectorIndex,
			uint32_t reasonMask,
			RuntimeSectorDirtyTruthTraceEntry::PreviousStateSource previousStateSource,
			bool forceTopology,
			bool baselineChanged,
			bool geometryChanged,
			bool materialChanged,
			uint32_t previousSurfaceCount,
			uint32_t liveSurfaceCount,
			uint32_t previousTriangleCount,
			uint32_t liveTriangleCount)
	{
		if (!ShouldTracePtPerf())
		{
			return;
		}

		RuntimeSectorDirtyTruthTraceEntry entry = {};
		entry.valid = true;
		entry.chunkIndex = chunkIndex;
		entry.sectorIndex = sectorIndex;
		entry.reasonMask = reasonMask;
		entry.previousStateSource = previousStateSource;
		entry.forceTopology = forceTopology;
		entry.baselineChanged = baselineChanged;
		entry.geometryChanged = geometryChanged;
		entry.materialChanged = materialChanged;
		entry.previousSurfaceCount = previousSurfaceCount;
		entry.liveSurfaceCount = liveSurfaceCount;
		entry.previousTriangleCount = previousTriangleCount;
		entry.liveTriangleCount = liveTriangleCount;
		InsertRankedTraceEntry(
			mLastPerfShellTraceStats.runtimeSectorDirtyTruthEntries,
			entry,
			ScoreRuntimeSectorDirtyTruthEntry);
	};
	const auto recordMaterialOnlyMismatchEntry =
		[this](uint32_t chunkIndex,
			int32_t sectorIndex,
			bool refreshPath,
			uint32_t reasonMask,
			uint32_t filteredSurfaceCount,
			uint32_t filteredMaterialCount,
			uint32_t residentMaterialCount,
			uint32_t filteredWallCount,
			uint32_t filteredFlatCount,
			uint32_t residentWallCount,
			uint32_t residentFlatCount)
	{
		if (!ShouldTracePtPerf())
		{
			return;
		}

		RuntimeMaterialOnlyMismatchTraceEntry entry = {};
		entry.valid = true;
		entry.chunkIndex = chunkIndex;
		entry.sectorIndex = sectorIndex;
		entry.refreshPath = refreshPath;
		entry.reasonMask = reasonMask;
		entry.filteredSurfaceCount = filteredSurfaceCount;
		entry.filteredMaterialCount = filteredMaterialCount;
		entry.residentMaterialCount = residentMaterialCount;
		entry.filteredWallCount = filteredWallCount;
		entry.filteredFlatCount = filteredFlatCount;
		entry.residentWallCount = residentWallCount;
		entry.residentFlatCount = residentFlatCount;
		InsertRankedTraceEntry(
			mLastPerfShellTraceStats.runtimeMaterialOnlyMismatchEntries,
			entry,
			ScoreRuntimeMaterialOnlyMismatchTraceEntry);
	};
	const auto recordStructuralRebuildEntry =
		[this](uint32_t chunkIndex,
			int32_t sectorIndex,
			uint32_t reasonMask,
			uint32_t triggerMask,
			uint32_t surfaceCount,
			uint32_t triangleCount,
			uint32_t materialCount,
			RuntimeMutationTraceAction action,
			bool materialOnly,
			bool sectorMaterialOnly,
			bool wallMaterialOnly,
			bool mixedMaterialOnly,
			bool geometryOrDirty)
	{
		if (!ShouldCollectRuntimeMutationPerfTiming())
		{
			return;
		}

		RuntimeStructuralRebuildTraceEntry entry = {};
		entry.valid = true;
		entry.chunkIndex = chunkIndex;
		entry.sectorIndex = sectorIndex;
		entry.reasonMask = reasonMask;
		entry.triggerMask = triggerMask;
		entry.surfaceCount = surfaceCount;
		entry.triangleCount = triangleCount;
		entry.materialCount = materialCount;
		entry.action = action;
		entry.materialOnly = materialOnly;
		entry.sectorMaterialOnly = sectorMaterialOnly;
		entry.wallMaterialOnly = wallMaterialOnly;
		entry.mixedMaterialOnly = mixedMaterialOnly;
		entry.geometryOrDirty = geometryOrDirty;
		InsertRankedTraceEntry(
			mLastPerfShellTraceStats.runtimeStructuralRebuildEntries,
			entry,
			ScoreRuntimeStructuralRebuildTraceEntry);
	};
	const auto recordGeometryDirtyEntry =
		[this](uint32_t chunkIndex,
			int32_t sectorIndex,
			uint32_t reasonMask,
			uint32_t previousWallCount,
			uint32_t liveWallCount,
			uint32_t previousFlatCount,
			uint32_t liveFlatCount,
			uint32_t previousTriangleCount,
			uint32_t liveTriangleCount,
			uint32_t previousMaterialCount,
			uint32_t liveMaterialCount,
			bool forceTopology)
	{
		const bool sectorGeometry = (reasonMask & nri_scene::PTMapChunkMutationReason_SectorGeometry) != 0;
		const bool wallGeometry = (reasonMask & nri_scene::PTMapChunkMutationReason_WallGeometry) != 0;
		const bool dirtyOnlyBits =
			(reasonMask &
				(nri_scene::PTMapChunkMutationReason_SectorDirty |
				 nri_scene::PTMapChunkMutationReason_SectionDirty |
				 nri_scene::PTMapChunkMutationReason_Dragged)) != 0;
		const bool wallsChanged = previousWallCount != liveWallCount;
		const bool flatsChanged = previousFlatCount != liveFlatCount;
		const bool countChanged =
			wallsChanged ||
			flatsChanged ||
			previousTriangleCount != liveTriangleCount ||
			previousMaterialCount != liveMaterialCount;

		uint32_t familyMask = 0;
		if (sectorGeometry && !wallGeometry && !dirtyOnlyBits)
		{
			familyMask |= RuntimeGeometryDirtyFamily_SectorGeometryOnly;
			mLastPerfShellTraceStats.runtimeMutationGeometryDirtySectorGeometryOnlyChunks++;
		}
		else if (!sectorGeometry && wallGeometry && !dirtyOnlyBits)
		{
			familyMask |= RuntimeGeometryDirtyFamily_WallGeometryOnly;
			mLastPerfShellTraceStats.runtimeMutationGeometryDirtyWallGeometryOnlyChunks++;
		}
		else if (sectorGeometry && wallGeometry && !dirtyOnlyBits)
		{
			familyMask |= RuntimeGeometryDirtyFamily_SectorWallGeometry;
			mLastPerfShellTraceStats.runtimeMutationGeometryDirtySectorWallGeometryChunks++;
		}
		else if (!sectorGeometry && !wallGeometry && dirtyOnlyBits)
		{
			familyMask |= RuntimeGeometryDirtyFamily_DirtyOnly;
			mLastPerfShellTraceStats.runtimeMutationGeometryDirtyDirtyOnlyChunks++;
		}
		else
		{
			familyMask |= RuntimeGeometryDirtyFamily_GeometryDirtyMixed;
			mLastPerfShellTraceStats.runtimeMutationGeometryDirtyGeometryDirtyMixedChunks++;
		}

		if (forceTopology && !countChanged)
		{
			mLastPerfShellTraceStats.runtimeMutationGeometryDirtyForceTopologyOnlyChunks++;
		}
		if (countChanged)
		{
			mLastPerfShellTraceStats.runtimeMutationGeometryDirtyRealCountChangeChunks++;
		}
		if (wallsChanged && !flatsChanged)
		{
			mLastPerfShellTraceStats.runtimeMutationGeometryDirtyWallsOnlyChangedChunks++;
		}
		else if (!wallsChanged && flatsChanged)
		{
			mLastPerfShellTraceStats.runtimeMutationGeometryDirtyFlatsOnlyChangedChunks++;
		}
		else if (wallsChanged && flatsChanged)
		{
			mLastPerfShellTraceStats.runtimeMutationGeometryDirtyWallsAndFlatsChangedChunks++;
		}

		if (countChanged &&
			chunkIndex < mRuntimeRecurringChunkTrackers.size())
		{
			auto& tracker = mRuntimeRecurringChunkTrackers[chunkIndex];
			const uint64_t stateSignature = ComputeRecurringChunkStateSignature(
				reasonMask,
				liveWallCount,
				liveFlatCount,
				liveTriangleCount,
				liveMaterialCount);
			if (!tracker.valid)
			{
				tracker.valid = true;
				tracker.chunkIndex = chunkIndex;
				tracker.sectorIndex = sectorIndex;
			}
			tracker.lastReasonMask = reasonMask;
			tracker.visitCount++;
			tracker.lastWallCount = liveWallCount;
			tracker.lastFlatCount = liveFlatCount;
			tracker.lastTriangleCount = liveTriangleCount;
			tracker.lastMaterialCount = liveMaterialCount;

			bool seenBefore = false;
			for (uint32_t i = 0; i < tracker.uniqueStateCount; ++i)
			{
				if (tracker.seenStateSignatures[i] == stateSignature)
				{
					seenBefore = true;
					break;
				}
			}
			if (!seenBefore && tracker.uniqueStateCount < tracker.seenStateSignatures.size())
			{
				tracker.seenStateSignatures[tracker.uniqueStateCount++] = stateSignature;
			}
			if (tracker.lastStateSignature != 0 &&
				tracker.lastStateSignature != stateSignature)
			{
				tracker.transitionCount++;
				if (seenBefore)
				{
					tracker.repeatedStateHitCount++;
				}
				if (tracker.previousStateSignature != 0 &&
					tracker.previousStateSignature == stateSignature)
				{
					tracker.abaRecurrenceCount++;
				}
			}
			tracker.previousStateSignature = tracker.lastStateSignature;
			tracker.lastStateSignature = stateSignature;
		}

		if (!ShouldTracePtPerf())
		{
			return;
		}

		RuntimeGeometryDirtyTraceEntry entry = {};
		entry.valid = true;
		entry.chunkIndex = chunkIndex;
		entry.sectorIndex = sectorIndex;
		entry.reasonMask = reasonMask;
		entry.familyMask = familyMask;
		entry.previousWallCount = previousWallCount;
		entry.liveWallCount = liveWallCount;
		entry.previousFlatCount = previousFlatCount;
		entry.liveFlatCount = liveFlatCount;
		entry.previousTriangleCount = previousTriangleCount;
		entry.liveTriangleCount = liveTriangleCount;
		entry.previousMaterialCount = previousMaterialCount;
		entry.liveMaterialCount = liveMaterialCount;
		entry.forceTopology = forceTopology;
		entry.countChanged = countChanged;
		entry.wallsChanged = wallsChanged;
		entry.flatsChanged = flatsChanged;
		InsertRankedTraceEntry(
			mLastPerfShellTraceStats.runtimeGeometryDirtyEntries,
			entry,
			ScoreRuntimeGeometryDirtyTraceEntry);
	};
	if (!mStaticMapScene.valid ||
		mRuntimeMutation.GetCacheChunkCount() != (uint32_t)mMapWorld.chunks.size())
	{
		return false;
	}
	const auto runtimeMutationDiscoveryStart = collectRuntimeMutationTiming ?
		std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
	NRIMapMoverRigidRouteFrameInput rigidRouteInput;
	rigidRouteInput.movers = &mMapMovers;
	rigidRouteInput.shadowState = &mMapMoverShadow.GetState();
	rigidRouteInput.mapWorld = &mMapWorld;
	rigidRouteInput.staticScene = &mStaticMapScene;
	rigidRouteInput.atlas = &mStaticMapChunkAtlas;
	rigidRouteInput.registry = &mStaticSceneResidency.Registry();
	rigidRouteInput.runtimeMutation = &mRuntimeMutation;
	rigidRouteInput.frameIndex = mFrameIndex;
	rigidRouteInput.mode = (int)nri_ptmapmovermode;
	rigidRouteInput.traceMode = (int)nri_ptmapmovershadow;
	mMapMoverRigidRoute.Update(rigidRouteInput);
	mSE29FloorDeformerRoute.BeginFrame(
		mFrameIndex,
		mMapWorld.buildSerial,
		mMapMovers.GetMapEpoch());
	mMapMaterialOnlyRoute.BeginFrame(mFrameIndex);
	static constexpr uint8_t kVisibleResidentValidationWindow = 8;
	static constexpr uint8_t kVisibleResidentUnresolvedTextureValidationWindow = 64;
	static constexpr uint8_t kStartupVisibleResidentValidationWindow = 64;
	mRuntimeMutation.EnsureSignatureWatchlist(mMapWorld.buildSerial, (uint32_t)mMapWorld.chunks.size());
	const auto isVisibleSuppressedStaticAnimatedChunk = [&](uint32_t mapChunkIndex) -> bool
	{
		if (!IsChunkMarkedVisible(mCurrentVisibleChunkWords, mapChunkIndex))
		{
			return false;
		}

		if (mapChunkIndex < mStaticSceneResidency.Registry().entries.size())
		{
			const auto& entry = mStaticSceneResidency.Registry().entries[mapChunkIndex];
			return
				entry.valid &&
				entry.hasAnimatedTextureCandidates &&
				entry.animatedRefreshSuppressed;
		}

		return false;
	};
	const auto isFlatPlaneMarkedVisible = [](const std::vector<uint32_t>& visibleFlatPlaneWords, int32_t sectorIndex, bool ceiling) -> bool
	{
		if (sectorIndex < 0)
		{
			return false;
		}

		const uint32_t planeIndex = (uint32_t)sectorIndex * 2u + (ceiling ? 1u : 0u);
		const uint32_t wordIndex = planeIndex / 32u;
		const uint32_t bitIndex = planeIndex % 32u;
		return wordIndex < visibleFlatPlaneWords.size() && ((visibleFlatPlaneWords[wordIndex] >> bitIndex) & 1u) != 0u;
	};
	const auto mapChunkHasPortalSurface = [this](const nri_scene::PTMapChunk& mapChunk) -> bool
	{
		const uint32_t begin = mapChunk.firstSurface;
		const uint32_t end = std::min<uint32_t>(begin + mapChunk.surfaceCount, (uint32_t)mMapWorld.surfaces.size());
		for (uint32_t surfaceIndex = begin; surfaceIndex < end; ++surfaceIndex)
		{
			if (mMapWorld.surfaces[surfaceIndex].kind == nri_scene::PTMapSurfaceKind::Portal)
			{
				return true;
			}
		}

		return false;
	};
	for (const auto& mapChunk : mMapWorld.chunks)
	{
		if (mapChunk.chunkIndex < mPendingStartupVisibleChunkValidation.size() &&
			mPendingStartupVisibleChunkValidation[mapChunk.chunkIndex] != 0u &&
			!IsChunkMarkedVisible(mCurrentVisibleChunkWords, mapChunk.chunkIndex))
		{
			mPendingStartupVisibleChunkValidation[mapChunk.chunkIndex] = 0u;
		}
	}
	const bool runtimeMutationWorklistEnabled = runtimeMutationSettings.worklistEnabled;
	mRuntimeMutation.BeginWorklistFrame((uint32_t)mMapWorld.chunks.size());
	const std::vector<uint32_t>& runtimeMutationCandidateSourceMasks =
		mRuntimeMutation.GetWorklistCandidateSourceMasks();
	uint32_t runtimeMutationCandidateCount = 0;
	uint32_t runtimeMutationSignatureWatchlistCandidateCount = 0;
	uint32_t runtimeMutationBackgroundSweepCandidateCount = 0;
	enum class RuntimeMutationDistanceTier : uint8_t
	{
		Unknown,
		Near,
		Far,
	};
	const float runtimeMutationNearDistance = runtimeMutationSettings.nearDistance;
	const float runtimeMutationNearDistanceSquared = runtimeMutationNearDistance * runtimeMutationNearDistance;
	const auto getRuntimeMutationDistanceTier = [&](uint32_t chunkListIndex, bool chunkVisibleNow) -> RuntimeMutationDistanceTier
	{
		if (chunkVisibleNow)
		{
			return RuntimeMutationDistanceTier::Near;
		}
		if (chunkListIndex >= mMapWorld.chunks.size())
		{
			return RuntimeMutationDistanceTier::Unknown;
		}

		const auto& bounds = mMapWorld.chunks[chunkListIndex].bounds;
		if (!bounds.valid)
		{
			return RuntimeMutationDistanceTier::Unknown;
		}

		float distanceSquared = 0.0f;
		for (int axis = 0; axis < 3; ++axis)
		{
			float distance = 0.0f;
			if (mCurrentCameraPos[axis] < bounds.min[axis])
			{
				distance = bounds.min[axis] - mCurrentCameraPos[axis];
			}
			else if (mCurrentCameraPos[axis] > bounds.max[axis])
			{
				distance = mCurrentCameraPos[axis] - bounds.max[axis];
			}
			distanceSquared += distance * distance;
		}
		return distanceSquared <= runtimeMutationNearDistanceSquared ?
			RuntimeMutationDistanceTier::Near :
			RuntimeMutationDistanceTier::Far;
	};
	const auto markWorklistCandidate =
		[&](uint32_t chunkListIndex, uint32_t sourceMask)
	{
		if (chunkListIndex >= runtimeMutationCandidateSourceMasks.size() || sourceMask == 0)
		{
			return;
		}
		if (runtimeMutationCandidateSourceMasks[chunkListIndex] == 0)
		{
			runtimeMutationCandidateCount++;
			const uint32_t mapChunkIndex = chunkListIndex < mMapWorld.chunks.size() ? mMapWorld.chunks[chunkListIndex].chunkIndex : chunkListIndex;
			const bool chunkVisibleNow = IsChunkMarkedVisible(mCurrentVisibleChunkWords, mapChunkIndex);
			const RuntimeMutationDistanceTier distanceTier = getRuntimeMutationDistanceTier(chunkListIndex, chunkVisibleNow);
			if (chunkVisibleNow)
			{
				mLastPerfShellTraceStats.runtimeMutationCandidateVisibleChunks++;
			}
			else
			{
				mLastPerfShellTraceStats.runtimeMutationCandidateInvisibleChunks++;
				switch (distanceTier)
				{
				case RuntimeMutationDistanceTier::Near:
					mLastPerfShellTraceStats.runtimeMutationCandidateNearChunks++;
					break;
				case RuntimeMutationDistanceTier::Far:
					mLastPerfShellTraceStats.runtimeMutationCandidateFarChunks++;
					break;
				default:
					mLastPerfShellTraceStats.runtimeMutationCandidateUnknownDistanceChunks++;
					break;
				}
			}
			if (chunkListIndex < mMapWorld.chunks.size() && mMapWorld.chunks[chunkListIndex].bounds.valid)
			{
				mLastPerfShellTraceStats.runtimeMutationCandidateBoundsValidChunks++;
			}
			else
			{
				mLastPerfShellTraceStats.runtimeMutationCandidateBoundsInvalidChunks++;
			}
		}
		if ((runtimeMutationCandidateSourceMasks[chunkListIndex] & sourceMask) == 0)
		{
			if ((sourceMask & RuntimeMutationWorklistCandidateSource_ActiveReplacement) != 0)
			{
				mLastPerfShellTraceStats.runtimeMutationCandidateActiveReplacementChunks++;
			}
			if ((sourceMask & RuntimeMutationWorklistCandidateSource_VisibleResidentValidation) != 0)
			{
				mLastPerfShellTraceStats.runtimeMutationCandidateVisibleResidentValidationChunks++;
			}
			if ((sourceMask & RuntimeMutationWorklistCandidateSource_StartupVisibleValidation) != 0)
			{
				mLastPerfShellTraceStats.runtimeMutationCandidateStartupVisibleValidationChunks++;
			}
			if ((sourceMask & RuntimeMutationWorklistCandidateSource_UnresolvedAuthoredTextures) != 0)
			{
				mLastPerfShellTraceStats.runtimeMutationCandidateUnresolvedTextureChunks++;
			}
			if ((sourceMask & RuntimeMutationWorklistCandidateSource_StaticAnimatedSuppressed) != 0)
			{
				mLastPerfShellTraceStats.runtimeMutationCandidateStaticAnimatedSuppressedChunks++;
			}
			if ((sourceMask & RuntimeMutationWorklistCandidateSource_SectorDirty) != 0)
			{
				mLastPerfShellTraceStats.runtimeMutationCandidateSectorDirtyChunks++;
			}
			if ((sourceMask & RuntimeMutationWorklistCandidateSource_SectionDirty) != 0)
			{
				mLastPerfShellTraceStats.runtimeMutationCandidateSectionDirtyChunks++;
			}
			if ((sourceMask & RuntimeMutationWorklistCandidateSource_Dragged) != 0)
			{
				mLastPerfShellTraceStats.runtimeMutationCandidateDraggedChunks++;
			}
			if ((sourceMask & RuntimeMutationWorklistCandidateSource_SignatureWatchlist) != 0)
			{
				mLastPerfShellTraceStats.runtimeMutationCandidateSignatureWatchChunks++;
			}
			if ((sourceMask & RuntimeMutationWorklistCandidateSource_BackgroundSweep) != 0)
			{
				mLastPerfShellTraceStats.runtimeMutationCandidateBackgroundSweepSourceChunks++;
			}
			if ((sourceMask & RuntimeMutationWorklistCandidateSource_DeferredMaterialRefresh) != 0)
			{
				mLastPerfShellTraceStats.runtimeMutationCandidateDeferredMaterialChunks++;
			}
			if ((sourceMask & RuntimeMutationWorklistCandidateSource_DeferredStructuralRebuild) != 0)
			{
				mLastPerfShellTraceStats.runtimeMutationCandidateDeferredStructuralChunks++;
			}
		}
		mRuntimeMutation.MarkWorklistCandidate(chunkListIndex, sourceMask);
	};
	struct RuntimeMutationWorklistValidationState
	{
		bool enabled = false;
		int verbosity = 0;
		uint32_t candidateCount = 0;
		uint32_t fullDirtyCount = 0;
		uint32_t falseNegativeCount = 0;
		uint32_t falsePositiveCount = 0;
		uint32_t falseNegativeTraceCount = 0;
		uint32_t signatureWatchlistCandidateCount = 0;
		uint32_t signatureWatchlistSeedCount = 0;
		uint32_t backgroundSweepCandidateCount = 0;
		std::array<uint32_t, 128> falseNegativeReasonMaskCounts = {};
		std::vector<uint32_t> sourceMasks;
		std::vector<uint8_t> fullDirty;
	};
	RuntimeMutationWorklistValidationState worklistValidation = {};
	worklistValidation.verbosity = (int)nri_ptmutationworklistvalidate;
	worklistValidation.enabled = worklistValidation.verbosity > 0;
	for (uint32_t candidateListIndex = 0; candidateListIndex < (uint32_t)mMapWorld.chunks.size(); ++candidateListIndex)
	{
		const auto& mapChunk = mMapWorld.chunks[candidateListIndex];
		if (mMapMoverRigidRoute.ShouldBypassExactChunk(mapChunk.chunkIndex))
		{
			continue;
		}
		const auto* replacement = mRuntimeMutation.FindReplacement(candidateListIndex);
		if (replacement == nullptr)
		{
			continue;
		}
		if (replacement->active || (replacement->valid && !replacement->residentAuthoritative))
		{
			markWorklistCandidate(candidateListIndex, RuntimeMutationWorklistCandidateSource_ActiveReplacement);
		}

		const bool chunkVisibleNow = IsChunkMarkedVisible(mCurrentVisibleChunkWords, mapChunk.chunkIndex);
		const bool startupVisibleValidationPending =
			mapChunk.chunkIndex < mPendingStartupVisibleChunkValidation.size() &&
			mPendingStartupVisibleChunkValidation[mapChunk.chunkIndex] != 0u;
		const bool chunkHasUnresolvedAuthoredTextures =
			chunkVisibleNow &&
			ChunkHasUnresolvedAuthoredTextures(mapChunk);
		const ResidentMapChunkRegistry::Entry* residentEntry =
			mapChunk.chunkIndex < mStaticSceneResidency.Registry().entries.size() ?
			&mStaticSceneResidency.Registry().entries[mapChunk.chunkIndex] :
			nullptr;
		if (residentEntry != nullptr &&
			residentEntry->valid &&
			chunkVisibleNow &&
			(!residentEntry->wasVisibleLastFrame || residentEntry->visibleValidationFramesRemaining > 0))
		{
			markWorklistCandidate(candidateListIndex, RuntimeMutationWorklistCandidateSource_VisibleResidentValidation);
		}
		if (chunkVisibleNow && startupVisibleValidationPending)
		{
			markWorklistCandidate(candidateListIndex, RuntimeMutationWorklistCandidateSource_StartupVisibleValidation);
		}
		if (chunkHasUnresolvedAuthoredTextures)
		{
			markWorklistCandidate(candidateListIndex, RuntimeMutationWorklistCandidateSource_UnresolvedAuthoredTextures);
		}
		if (isVisibleSuppressedStaticAnimatedChunk(mapChunk.chunkIndex))
		{
			markWorklistCandidate(candidateListIndex, RuntimeMutationWorklistCandidateSource_StaticAnimatedSuppressed);
		}
		if (replacement->deferredMaterialRefresh)
		{
			markWorklistCandidate(candidateListIndex, RuntimeMutationWorklistCandidateSource_DeferredMaterialRefresh);
		}
		if (replacement->deferredStructuralRebuild)
		{
			markWorklistCandidate(candidateListIndex, RuntimeMutationWorklistCandidateSource_DeferredStructuralRebuild);
		}
		if (mRuntimeMutation.IsSignatureWatchlistSeeded(candidateListIndex))
		{
			const uint64_t liveSignature = nri_scene::ComputeMapChunkGeometrySignature(mapChunk);
			if (liveSignature != replacement->baselineSignature)
			{
				markWorklistCandidate(candidateListIndex, RuntimeMutationWorklistCandidateSource_SignatureWatchlist);
				runtimeMutationSignatureWatchlistCandidateCount++;
			}
		}
		if (mapChunk.sectorIndex >= 0 && (unsigned)mapChunk.sectorIndex < sector.Size())
		{
			const auto& sec = sector[(unsigned)mapChunk.sectorIndex];
			if (sec.dirty != 0)
			{
				markWorklistCandidate(candidateListIndex, RuntimeMutationWorklistCandidateSource_SectorDirty);
			}
			if ((sec.exflags & SECTOREX_DRAGGED) != 0)
			{
				markWorklistCandidate(candidateListIndex, RuntimeMutationWorklistCandidateSource_Dragged);
			}
		}
		for (int sectionIndex : replacement->baseline.sectionIndices)
		{
			if ((unsigned)sectionIndex < sections.Size() && sections[sectionIndex].dirty != 0)
			{
				markWorklistCandidate(candidateListIndex, RuntimeMutationWorklistCandidateSource_SectionDirty);
				break;
			}
		}
	}

	if (runtimeMutationWorklistEnabled && !mMapWorld.chunks.empty())
	{
		const uint32_t sweepBudget = runtimeMutationSettings.worklistSweepBudget;
		const uint32_t chunkCount = (uint32_t)mMapWorld.chunks.size();
		const uint32_t sweepCount = std::min(sweepBudget, chunkCount);
		for (uint32_t sweepOffset = 0; sweepOffset < sweepCount; ++sweepOffset)
		{
			const uint32_t chunkListIndex = mRuntimeMutation.GetWorklistSweepChunkIndex(sweepOffset, chunkCount);
			if ((runtimeMutationCandidateSourceMasks[chunkListIndex] & RuntimeMutationWorklistCandidateSource_BackgroundSweep) == 0)
			{
				runtimeMutationBackgroundSweepCandidateCount++;
			}
			markWorklistCandidate(chunkListIndex, RuntimeMutationWorklistCandidateSource_BackgroundSweep);
		}
		mRuntimeMutation.AdvanceWorklistSweepCursor(sweepCount, chunkCount);
	}
	mLastPerfShellTraceStats.runtimeMutationCandidateChunks = runtimeMutationCandidateCount;
	mLastPerfShellTraceStats.runtimeMutationBackgroundSweepChunks = runtimeMutationBackgroundSweepCandidateCount;
	if (worklistValidation.enabled)
	{
		worklistValidation.candidateCount = runtimeMutationCandidateCount;
		worklistValidation.signatureWatchlistCandidateCount = runtimeMutationSignatureWatchlistCandidateCount;
		worklistValidation.backgroundSweepCandidateCount = runtimeMutationBackgroundSweepCandidateCount;
		worklistValidation.sourceMasks = runtimeMutationCandidateSourceMasks;
		worklistValidation.fullDirty.resize(mMapWorld.chunks.size(), 0u);
	}
	const auto getRuntimeMutationCandidateSourceMask = [&](size_t chunkIndex) -> uint32_t
	{
		return chunkIndex < runtimeMutationCandidateSourceMasks.size() ? runtimeMutationCandidateSourceMasks[chunkIndex] : 0u;
	};
	const bool traceStartupMutationPass =
		(bool)nri_ptloadingmutationbaseline &&
		((int)nri_ptloadingtrace >= 2 ||
			((int)nri_ptloadingtrace >= 1 && (mAllowStartupMutationRebaseline || mPendingStartupMutationRebaseline)));
	if (traceStartupMutationPass)
	{
		auto countSource = [&](uint32_t sourceBit) -> uint32_t
		{
			uint32_t count = 0;
			for (uint32_t sourceMask : runtimeMutationCandidateSourceMasks)
			{
				if ((sourceMask & sourceBit) != 0)
				{
					count++;
				}
			}
			return count;
		};
		auto countVisibleChunks = [&]() -> uint32_t
		{
			uint32_t count = 0;
			for (uint32_t word : mCurrentVisibleChunkWords)
			{
				while (word != 0)
				{
					count += word & 1u;
					word >>= 1u;
				}
			}
			return count;
		};

		mStartupMutationProbe.valid = true;
		mStartupMutationProbe.detectedMaterialOnly = false;
		mStartupMutationProbe.frameIndex = mFrameIndex;
		mStartupMutationProbe.chunkCount = (uint32_t)mMapWorld.chunks.size();
		mStartupMutationProbe.visibleChunkCount = countVisibleChunks();
		mStartupMutationProbe.candidateCount = runtimeMutationCandidateCount;
		mStartupMutationProbe.candidateActiveReplacementCount = countSource(RuntimeMutationWorklistCandidateSource_ActiveReplacement);
		mStartupMutationProbe.candidateVisibleResidentValidationCount = countSource(RuntimeMutationWorklistCandidateSource_VisibleResidentValidation);
		mStartupMutationProbe.candidateStartupVisibleValidationCount = countSource(RuntimeMutationWorklistCandidateSource_StartupVisibleValidation);
		mStartupMutationProbe.candidateUnresolvedAuthoredTextureCount = countSource(RuntimeMutationWorklistCandidateSource_UnresolvedAuthoredTextures);
		mStartupMutationProbe.candidateStaticAnimatedSuppressedCount = countSource(RuntimeMutationWorklistCandidateSource_StaticAnimatedSuppressed);
		mStartupMutationProbe.candidateSectorDirtyCount = countSource(RuntimeMutationWorklistCandidateSource_SectorDirty);
		mStartupMutationProbe.candidateSectionDirtyCount = countSource(RuntimeMutationWorklistCandidateSource_SectionDirty);
		mStartupMutationProbe.candidateDraggedCount = countSource(RuntimeMutationWorklistCandidateSource_Dragged);
		mStartupMutationProbe.candidateSignatureWatchlistCount = countSource(RuntimeMutationWorklistCandidateSource_SignatureWatchlist);
		mStartupMutationProbe.candidateBackgroundSweepCount = countSource(RuntimeMutationWorklistCandidateSource_BackgroundSweep);
		mStartupMutationProbe.candidateDeferredMaterialRefreshCount = countSource(RuntimeMutationWorklistCandidateSource_DeferredMaterialRefresh);
		mStartupMutationProbe.candidateDeferredStructuralRebuildCount = countSource(RuntimeMutationWorklistCandidateSource_DeferredStructuralRebuild);
		mStartupMutationProbe.dirtyChunkCount = 0;
		mStartupMutationProbe.startupMaterialOnlyDirtyChunkCount = 0;
		TraceStartupMutationProbe("worklist-built");
	}
	if (collectRuntimeMutationTiming)
	{
		mLastPerfShellTraceStats.runtimeMutationDiscoveryMs += std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - runtimeMutationDiscoveryStart).count();
	}
	const auto recordRuntimeMutationDirtyTier = [&](bool chunkVisibleNow, RuntimeMutationDistanceTier distanceTier, uint32_t sourceMask)
	{
		if (chunkVisibleNow)
		{
			mLastPerfShellTraceStats.runtimeMutationDirtyVisibleChunks++;
		}
		else
		{
			mLastPerfShellTraceStats.runtimeMutationDirtyInvisibleChunks++;
			switch (distanceTier)
			{
			case RuntimeMutationDistanceTier::Near:
				mLastPerfShellTraceStats.runtimeMutationDirtyNearChunks++;
				break;
			case RuntimeMutationDistanceTier::Far:
				mLastPerfShellTraceStats.runtimeMutationDirtyFarChunks++;
				break;
			default:
				mLastPerfShellTraceStats.runtimeMutationDirtyUnknownDistanceChunks++;
				break;
			}
		}
		if ((sourceMask & RuntimeMutationWorklistCandidateSource_ActiveReplacement) != 0)
		{
			mLastPerfShellTraceStats.runtimeMutationDirtyActiveReplacementChunks++;
		}
		if ((sourceMask & RuntimeMutationWorklistCandidateSource_BackgroundSweep) != 0)
		{
			mLastPerfShellTraceStats.runtimeMutationDirtyBackgroundSweepChunks++;
		}
	};
	const auto recordRuntimeMutationStructuralTier = [&](bool chunkVisibleNow, RuntimeMutationDistanceTier distanceTier, uint32_t sourceMask)
	{
		if (chunkVisibleNow)
		{
			mLastPerfShellTraceStats.runtimeMutationStructuralRebuildVisibleChunks++;
		}
		else
		{
			mLastPerfShellTraceStats.runtimeMutationStructuralRebuildInvisibleChunks++;
			switch (distanceTier)
			{
			case RuntimeMutationDistanceTier::Near:
				mLastPerfShellTraceStats.runtimeMutationStructuralRebuildNearChunks++;
				break;
			case RuntimeMutationDistanceTier::Far:
				mLastPerfShellTraceStats.runtimeMutationStructuralRebuildFarChunks++;
				break;
			default:
				mLastPerfShellTraceStats.runtimeMutationStructuralRebuildUnknownDistanceChunks++;
				break;
			}
		}
		if ((sourceMask & RuntimeMutationWorklistCandidateSource_ActiveReplacement) != 0)
		{
			mLastPerfShellTraceStats.runtimeMutationStructuralRebuildActiveReplacementChunks++;
		}
		if ((sourceMask & RuntimeMutationWorklistCandidateSource_BackgroundSweep) != 0)
		{
			mLastPerfShellTraceStats.runtimeMutationStructuralRebuildBackgroundSweepChunks++;
		}
	};
	const auto recordRuntimeMutationMaterialTier = [&](bool chunkVisibleNow, RuntimeMutationDistanceTier distanceTier, uint32_t sourceMask)
	{
		if (chunkVisibleNow)
		{
			mLastPerfShellTraceStats.runtimeMutationMaterialRefreshVisibleChunks++;
		}
		else
		{
			mLastPerfShellTraceStats.runtimeMutationMaterialRefreshInvisibleChunks++;
			switch (distanceTier)
			{
			case RuntimeMutationDistanceTier::Near:
				mLastPerfShellTraceStats.runtimeMutationMaterialRefreshNearChunks++;
				break;
			case RuntimeMutationDistanceTier::Far:
				mLastPerfShellTraceStats.runtimeMutationMaterialRefreshFarChunks++;
				break;
			default:
				mLastPerfShellTraceStats.runtimeMutationMaterialRefreshUnknownDistanceChunks++;
				break;
			}
		}
		if ((sourceMask & RuntimeMutationWorklistCandidateSource_ActiveReplacement) != 0)
		{
			mLastPerfShellTraceStats.runtimeMutationMaterialRefreshActiveReplacementChunks++;
		}
		if ((sourceMask & RuntimeMutationWorklistCandidateSource_BackgroundSweep) != 0)
		{
			mLastPerfShellTraceStats.runtimeMutationMaterialRefreshBackgroundSweepChunks++;
		}
	};
	const auto recordRuntimeMutationResidentApplyTier = [&](bool chunkVisibleNow, RuntimeMutationDistanceTier distanceTier, uint32_t sourceMask)
	{
		if (chunkVisibleNow)
		{
			mLastPerfShellTraceStats.runtimeMutationResidentApplyVisibleChunks++;
		}
		else
		{
			mLastPerfShellTraceStats.runtimeMutationResidentApplyInvisibleChunks++;
			switch (distanceTier)
			{
			case RuntimeMutationDistanceTier::Near:
				mLastPerfShellTraceStats.runtimeMutationResidentApplyNearChunks++;
				break;
			case RuntimeMutationDistanceTier::Far:
				mLastPerfShellTraceStats.runtimeMutationResidentApplyFarChunks++;
				break;
			default:
				mLastPerfShellTraceStats.runtimeMutationResidentApplyUnknownDistanceChunks++;
				break;
			}
		}
		if ((sourceMask & RuntimeMutationWorklistCandidateSource_ActiveReplacement) != 0)
		{
			mLastPerfShellTraceStats.runtimeMutationResidentApplyActiveReplacementChunks++;
		}
		if ((sourceMask & RuntimeMutationWorklistCandidateSource_BackgroundSweep) != 0)
		{
			mLastPerfShellTraceStats.runtimeMutationResidentApplyBackgroundSweepChunks++;
		}
	};
	const auto runtimeMutationBudgetStart = collectRuntimeMutationTiming ?
		std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
	const bool deferNearInvisibleStructuralRebuilds = runtimeMutationSettings.deferNearInvisibleStructuralRebuilds;
	const bool deferFarInvisibleStructuralRebuilds = runtimeMutationSettings.deferFarStructuralRebuilds;
	const uint32_t nearInvisibleStructuralBudget = runtimeMutationSettings.nearInvisibleStructuralBudget;
	const uint32_t farStructuralBudget = runtimeMutationSettings.farStructuralBudget;
	uint32_t nearInvisibleStructuralBudgetReserved = 0;
	uint32_t farStructuralBudgetReserved = 0;
	std::vector<uint8_t> nearInvisibleStructuralBudgetAllowed(mMapWorld.chunks.size(), 0u);
	std::vector<uint8_t> farStructuralBudgetAllowed(mMapWorld.chunks.size(), 0u);
	if ((deferNearInvisibleStructuralRebuilds && nearInvisibleStructuralBudget > 0) ||
		(deferFarInvisibleStructuralRebuilds && farStructuralBudget > 0))
	{
		struct DeferredStructuralBudgetCandidate
		{
			uint64_t deferredFrame = 0;
			uint32_t chunkListIndex = 0;
		};
		std::vector<DeferredStructuralBudgetCandidate> deferredNearStructuralCandidates;
		std::vector<DeferredStructuralBudgetCandidate> deferredFarStructuralCandidates;
		for (uint32_t candidateListIndex = 0; candidateListIndex < (uint32_t)mMapWorld.chunks.size(); ++candidateListIndex)
		{
			const auto* replacement = mRuntimeMutation.FindReplacement(candidateListIndex);
			if (replacement == nullptr || !replacement->deferredStructuralRebuild)
			{
				continue;
			}
			const auto& mapChunk = mMapWorld.chunks[candidateListIndex];
			const bool chunkVisibleNow = IsChunkMarkedVisible(mCurrentVisibleChunkWords, mapChunk.chunkIndex);
			if (chunkVisibleNow)
			{
				continue;
			}

			const RuntimeMutationDistanceTier distanceTier = getRuntimeMutationDistanceTier(candidateListIndex, false);
			if (distanceTier == RuntimeMutationDistanceTier::Near && deferNearInvisibleStructuralRebuilds)
			{
				deferredNearStructuralCandidates.push_back({ replacement->deferredStructuralFrame, candidateListIndex });
			}
			else if (distanceTier == RuntimeMutationDistanceTier::Far && deferFarInvisibleStructuralRebuilds)
			{
				deferredFarStructuralCandidates.push_back({ replacement->deferredStructuralFrame, candidateListIndex });
			}
		}
		const auto sortDeferredStructuralCandidates =
			[](std::vector<DeferredStructuralBudgetCandidate>& candidates)
			{
				std::sort(
					candidates.begin(),
					candidates.end(),
					[](const DeferredStructuralBudgetCandidate& left, const DeferredStructuralBudgetCandidate& right)
					{
						if (left.deferredFrame != right.deferredFrame)
						{
							return left.deferredFrame < right.deferredFrame;
						}
						return left.chunkListIndex < right.chunkListIndex;
					});
			};
		sortDeferredStructuralCandidates(deferredNearStructuralCandidates);
		sortDeferredStructuralCandidates(deferredFarStructuralCandidates);
		for (const auto& candidate : deferredNearStructuralCandidates)
		{
			if (nearInvisibleStructuralBudgetReserved >= nearInvisibleStructuralBudget)
			{
				break;
			}
			nearInvisibleStructuralBudgetAllowed[candidate.chunkListIndex] = 1u;
			nearInvisibleStructuralBudgetReserved++;
		}
		for (const auto& candidate : deferredFarStructuralCandidates)
		{
			if (farStructuralBudgetReserved >= farStructuralBudget)
			{
				break;
			}
			farStructuralBudgetAllowed[candidate.chunkListIndex] = 1u;
			farStructuralBudgetReserved++;
		}
	}
	const auto reserveInvisibleStructuralBudget =
		[&](size_t chunkIndex, RuntimeMutationDistanceTier distanceTier) -> bool
	{
		if (distanceTier == RuntimeMutationDistanceTier::Near)
		{
			if (!deferNearInvisibleStructuralRebuilds || nearInvisibleStructuralBudget == 0)
			{
				return false;
			}
			if (chunkIndex < nearInvisibleStructuralBudgetAllowed.size() &&
				nearInvisibleStructuralBudgetAllowed[chunkIndex] != 0u)
			{
				return true;
			}
			if (nearInvisibleStructuralBudgetReserved >= nearInvisibleStructuralBudget ||
				chunkIndex >= nearInvisibleStructuralBudgetAllowed.size())
			{
				return false;
			}

			nearInvisibleStructuralBudgetAllowed[chunkIndex] = 1u;
			nearInvisibleStructuralBudgetReserved++;
			return true;
		}
		if (distanceTier == RuntimeMutationDistanceTier::Far)
		{
			if (!deferFarInvisibleStructuralRebuilds || farStructuralBudget == 0)
			{
				return false;
			}
			if (chunkIndex < farStructuralBudgetAllowed.size() &&
				farStructuralBudgetAllowed[chunkIndex] != 0u)
			{
				return true;
			}
			if (farStructuralBudgetReserved >= farStructuralBudget ||
				chunkIndex >= farStructuralBudgetAllowed.size())
			{
				return false;
			}

			farStructuralBudgetAllowed[chunkIndex] = 1u;
			farStructuralBudgetReserved++;
			return true;
		}

		return false;
	};
	const bool deferNearInvisibleMaterialRefreshes = runtimeMutationSettings.deferNearInvisibleMaterialRefreshes;
	const uint32_t nearInvisibleMaterialBudget = runtimeMutationSettings.nearInvisibleMaterialBudget;
	uint32_t nearInvisibleMaterialBudgetReserved = 0;
	std::vector<uint8_t> nearInvisibleMaterialBudgetAllowed(mMapWorld.chunks.size(), 0u);
	if (deferNearInvisibleMaterialRefreshes && nearInvisibleMaterialBudget > 0)
	{
		struct DeferredMaterialBudgetCandidate
		{
			uint64_t deferredFrame = 0;
			uint32_t chunkListIndex = 0;
		};
		std::vector<DeferredMaterialBudgetCandidate> deferredNearMaterialCandidates;
		for (uint32_t candidateListIndex = 0; candidateListIndex < (uint32_t)mMapWorld.chunks.size(); ++candidateListIndex)
		{
			const auto* replacement = mRuntimeMutation.FindReplacement(candidateListIndex);
			if (replacement == nullptr || !replacement->deferredMaterialRefresh)
			{
				continue;
			}
			const auto& mapChunk = mMapWorld.chunks[candidateListIndex];
			const bool chunkVisibleNow = IsChunkMarkedVisible(mCurrentVisibleChunkWords, mapChunk.chunkIndex);
			if (chunkVisibleNow ||
				getRuntimeMutationDistanceTier(candidateListIndex, false) != RuntimeMutationDistanceTier::Near)
			{
				continue;
			}

			deferredNearMaterialCandidates.push_back({ replacement->deferredMaterialFrame, candidateListIndex });
		}
		std::sort(
			deferredNearMaterialCandidates.begin(),
			deferredNearMaterialCandidates.end(),
			[](const DeferredMaterialBudgetCandidate& left, const DeferredMaterialBudgetCandidate& right)
			{
				if (left.deferredFrame != right.deferredFrame)
				{
					return left.deferredFrame < right.deferredFrame;
				}
				return left.chunkListIndex < right.chunkListIndex;
			});
		for (const auto& candidate : deferredNearMaterialCandidates)
		{
			if (nearInvisibleMaterialBudgetReserved >= nearInvisibleMaterialBudget)
			{
				break;
			}
			nearInvisibleMaterialBudgetAllowed[candidate.chunkListIndex] = 1u;
			nearInvisibleMaterialBudgetReserved++;
		}
	}
	const auto reserveNearInvisibleMaterialBudget =
		[&](size_t chunkIndex) -> bool
	{
		if (!deferNearInvisibleMaterialRefreshes || nearInvisibleMaterialBudget == 0)
		{
			return false;
		}
		if (chunkIndex < nearInvisibleMaterialBudgetAllowed.size() &&
			nearInvisibleMaterialBudgetAllowed[chunkIndex] != 0u)
		{
			return true;
		}
		if (nearInvisibleMaterialBudgetReserved >= nearInvisibleMaterialBudget ||
			chunkIndex >= nearInvisibleMaterialBudgetAllowed.size())
		{
			return false;
		}

		nearInvisibleMaterialBudgetAllowed[chunkIndex] = 1u;
		nearInvisibleMaterialBudgetReserved++;
		return true;
	};
	const auto recordDeferredMaterialTier =
		[&](RuntimeMutationDistanceTier distanceTier, bool coalesced)
	{
		if (coalesced)
		{
			mLastPerfShellTraceStats.runtimeMutationMaterialRefreshDeferredCoalescedChunks++;
		}
		else
		{
			mLastPerfShellTraceStats.runtimeMutationMaterialRefreshDeferredChunks++;
		}
		if (distanceTier == RuntimeMutationDistanceTier::Near)
		{
			if (coalesced)
			{
				mLastPerfShellTraceStats.runtimeMutationMaterialRefreshDeferredNearCoalescedChunks++;
			}
			else
			{
				mLastPerfShellTraceStats.runtimeMutationMaterialRefreshDeferredNearChunks++;
			}
		}
	};
	const auto recordFlushedDeferredMaterialTier =
		[&](RuntimeMutationDistanceTier distanceTier)
	{
		mLastPerfShellTraceStats.runtimeMutationMaterialRefreshDeferredFlushedChunks++;
		if (distanceTier == RuntimeMutationDistanceTier::Near)
		{
			mLastPerfShellTraceStats.runtimeMutationMaterialRefreshDeferredNearFlushedChunks++;
		}
	};
	const auto recordDeferredStructuralTier =
		[&](RuntimeMutationDistanceTier distanceTier, bool coalesced)
	{
		if (coalesced)
		{
			mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredCoalescedChunks++;
		}
		else
		{
			mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredChunks++;
		}
		switch (distanceTier)
		{
		case RuntimeMutationDistanceTier::Near:
			if (coalesced)
			{
				mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredNearCoalescedChunks++;
			}
			else
			{
				mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredNearChunks++;
			}
			break;
		case RuntimeMutationDistanceTier::Far:
			if (coalesced)
			{
				mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredFarCoalescedChunks++;
			}
			else
			{
				mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredFarChunks++;
			}
			break;
		default:
			break;
		}
	};
	const auto recordFlushedDeferredStructuralTier =
		[&](RuntimeMutationDistanceTier distanceTier)
	{
		mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredFlushedChunks++;
		switch (distanceTier)
		{
		case RuntimeMutationDistanceTier::Near:
			mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredNearFlushedChunks++;
			break;
		case RuntimeMutationDistanceTier::Far:
			mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredFarFlushedChunks++;
			break;
		default:
			break;
		}
	};
	double ignoredRuntimeMutationDistanceTierMs = 0.0;
	if (collectRuntimeMutationTiming)
	{
		mLastPerfShellTraceStats.runtimeMutationBudgetMs += std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - runtimeMutationBudgetStart).count();
	}

	for (size_t chunkIndex = 0; chunkIndex < mMapWorld.chunks.size(); ++chunkIndex)
	{
		const auto& mapChunk = mMapWorld.chunks[chunkIndex];
		if (mMapMoverRigidRoute.ShouldBypassExactChunk(mapChunk.chunkIndex))
		{
			continue;
		}
		auto* replacementPtr = mRuntimeMutation.FindReplacement((uint32_t)chunkIndex);
		if (replacementPtr == nullptr)
		{
			continue;
		}
		auto& replacement = *replacementPtr;
		const bool chunkVisibleNow = IsChunkMarkedVisible(mCurrentVisibleChunkWords, mapChunk.chunkIndex);
		bool startupVisibleValidationPending =
			mapChunk.chunkIndex < mPendingStartupVisibleChunkValidation.size() &&
			mPendingStartupVisibleChunkValidation[mapChunk.chunkIndex] != 0u;
		if (!chunkVisibleNow && startupVisibleValidationPending)
		{
			mPendingStartupVisibleChunkValidation[mapChunk.chunkIndex] = 0u;
			startupVisibleValidationPending = false;
		}
		const bool chunkHasUnresolvedAuthoredTextures =
			chunkVisibleNow &&
			ChunkHasUnresolvedAuthoredTextures(mapChunk);
		ResidentMapChunkRegistry::Entry* residentEntry =
			mapChunk.chunkIndex < mStaticSceneResidency.Registry().entries.size() ?
			&mStaticSceneResidency.Registry().entries[mapChunk.chunkIndex] :
			nullptr;
		if (residentEntry != nullptr && residentEntry->valid)
		{
			if (!chunkVisibleNow)
			{
				if (mapChunk.chunkIndex < mPendingStartupVisibleChunkValidation.size())
				{
					mPendingStartupVisibleChunkValidation[mapChunk.chunkIndex] = 0u;
				}
				residentEntry->wasVisibleLastFrame = false;
				residentEntry->visibleValidationFramesRemaining = 0;
				residentEntry->visibleValidationTraceEmitted = false;
			}
			else
			{
				if (!residentEntry->wasVisibleLastFrame)
				{
					residentEntry->visibleValidationFramesRemaining = kVisibleResidentValidationWindow;
				}
				if (startupVisibleValidationPending)
				{
					residentEntry->visibleValidationFramesRemaining = std::max<uint8_t>(
						residentEntry->visibleValidationFramesRemaining,
						kStartupVisibleResidentValidationWindow);
				}
				if (chunkHasUnresolvedAuthoredTextures)
				{
					residentEntry->visibleValidationFramesRemaining = std::max<uint8_t>(
						residentEntry->visibleValidationFramesRemaining,
						kVisibleResidentUnresolvedTextureValidationWindow);
				}
				residentEntry->wasVisibleLastFrame = true;
			}
		}
		const bool processChunk =
			!runtimeMutationWorklistEnabled ||
			worklistValidation.enabled ||
			mMapMotionHistory.NeedsSettle(mapChunk.chunkIndex) ||
			(chunkIndex < runtimeMutationCandidateSourceMasks.size() &&
				runtimeMutationCandidateSourceMasks[chunkIndex] != 0);
		if (!processChunk)
		{
			continue;
		}
		const uint32_t runtimeMutationCandidateSourceMask = getRuntimeMutationCandidateSourceMask(chunkIndex);
		const RuntimeMutationDistanceTier runtimeMutationDistanceTier =
			getRuntimeMutationDistanceTier((uint32_t)chunkIndex, chunkVisibleNow);

		const uint32_t previousReasonMask = replacement.reasonMask;
		nri_scene::PTMapChunkMutationAnalysis analysis = {};
		const bool analyzed = [&]()
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationAnalyzeMs);
			mLastPerfShellTraceStats.runtimeMutationAnalyzedChunks++;
			return nri_scene::AnalyzeMapChunkMutation(mapChunk, replacement.baseline, analysis);
		}();
		if (!analyzed)
		{
			replacement.active = false;
			replacement.reasonMask = nri_scene::PTMapChunkMutationReason_None;
			replacement.sectionDirtyCount = 0;
			replacement.stableMutationFrameCount = 0;
			replacement.sectorDirty = false;
			replacement.dragged = false;
			replacement.blindSpot = false;
			replacement.excludeStaticChunk = false;
			replacement.staticAnimatedReplacement = false;
			replacement.animationOnlyRefreshed = false;
			replacement.animatedMaterialSignature = 0;
			mRuntimeMutation.TraceChunk(mapChunk, replacement, ShouldEmitTemporalTraceLogs(), (int)nri_ptmutationtracechunk, (int)nri_ptmutationtracesector);
			mRuntimeMutation.ClearReplacementPayload(replacement, replacement.residentAuthoritative);
			continue;
		}

		replacement.liveSignature = analysis.signature;
		uint32_t normalizedReasonMask = analysis.reasonMask;
		uint32_t normalizedSectionDirtyCount = analysis.sectionDirtyCount;
		bool normalizedSectorDirty = analysis.sectorDirty;
		bool normalizedDragged = analysis.dragged;
		const uint32_t residentNoiseReasonMask =
			nri_scene::PTMapChunkMutationReason_SectorDirty |
			nri_scene::PTMapChunkMutationReason_SectionDirty |
			nri_scene::PTMapChunkMutationReason_Dragged;
		if (replacement.residentAuthoritative &&
			!analysis.signatureChanged &&
			analysis.signature == replacement.baselineSignature &&
			(normalizedReasonMask & ~residentNoiseReasonMask) == 0)
		{
			normalizedReasonMask = nri_scene::PTMapChunkMutationReason_None;
			normalizedSectionDirtyCount = 0;
			normalizedSectorDirty = false;
			normalizedDragged = false;
		}
		const bool motionSettleRequested = mMapMotionHistory.NeedsSettle(mapChunk.chunkIndex);
		if (motionSettleRequested)
		{
			// Reuse the structural resident refresh lane for the one required
			// post-motion upload. Keep the structural reason even when a material
			// change coincides with settlement; otherwise the material-only fast
			// path may preserve the stale resident vertex payload. History consumes
			// this only after QueueSubmit.
			normalizedReasonMask |= nri_scene::PTMapChunkMutationReason_SectionDirty;
			normalizedSectionDirtyCount = std::max(normalizedSectionDirtyCount, 1u);
		}
		replacement.reasonMask = normalizedReasonMask;
		replacement.sectionDirtyCount = normalizedSectionDirtyCount;
		replacement.sectorDirty = normalizedSectorDirty;
		replacement.dragged = normalizedDragged;
		replacement.staticAnimatedReplacement = false;
		replacement.blindSpot = normalizedReasonMask != nri_scene::PTMapChunkMutationReason_None && !analysis.signatureChanged;
		if (normalizedReasonMask != nri_scene::PTMapChunkMutationReason_None &&
			!chunkVisibleNow &&
			chunkIndex < runtimeMutationCandidateSourceMasks.size() &&
			(runtimeMutationCandidateSourceMasks[chunkIndex] & RuntimeMutationWorklistCandidateSource_BackgroundSweep) != 0)
		{
			mRuntimeMutation.SeedSignatureWatchlist((uint32_t)chunkIndex);
		}
		if (worklistValidation.enabled && chunkIndex < worklistValidation.sourceMasks.size())
		{
			const bool fullScanSelectedChunk = normalizedReasonMask != nri_scene::PTMapChunkMutationReason_None;
			if (fullScanSelectedChunk)
			{
				worklistValidation.fullDirty[chunkIndex] = 1u;
				worklistValidation.fullDirtyCount++;
				if (worklistValidation.sourceMasks[chunkIndex] == 0)
				{
					worklistValidation.falseNegativeCount++;
					if (normalizedReasonMask < worklistValidation.falseNegativeReasonMaskCounts.size())
					{
						worklistValidation.falseNegativeReasonMaskCounts[normalizedReasonMask]++;
					}
					if (mRuntimeMutation.SeedSignatureWatchlist((uint32_t)chunkIndex))
					{
						worklistValidation.signatureWatchlistSeedCount++;
					}
					if (worklistValidation.verbosity >= 2 && worklistValidation.falseNegativeTraceCount < 16u)
					{
						Printf(
							"PERF pt mutation worklist miss NRI: frame=%llu chunk=%u sector=%d reason_mask=0x%x reasons=%s source_mask=0x%x sources=%s previous_reason_mask=0x%x active=%u valid=%u resident_authoritative=%u visible=%u startup_visible=%u unresolved_textures=%u visible_suppressed_anim=%u signature_changed=%u\n",
							(unsigned long long)mFrameIndex,
							mapChunk.chunkIndex,
							mapChunk.sectorIndex,
							normalizedReasonMask,
							GetRuntimeMapMutationReasonSummary(normalizedReasonMask).c_str(),
							worklistValidation.sourceMasks[chunkIndex],
							GetRuntimeMutationWorklistCandidateSourceSummary(worklistValidation.sourceMasks[chunkIndex]).c_str(),
							previousReasonMask,
							replacement.active ? 1u : 0u,
							replacement.valid ? 1u : 0u,
							replacement.residentAuthoritative ? 1u : 0u,
							chunkVisibleNow ? 1u : 0u,
							startupVisibleValidationPending ? 1u : 0u,
							chunkHasUnresolvedAuthoredTextures ? 1u : 0u,
							isVisibleSuppressedStaticAnimatedChunk(mapChunk.chunkIndex) ? 1u : 0u,
							analysis.signatureChanged ? 1u : 0u);
						worklistValidation.falseNegativeTraceCount++;
					}
				}
			}
		}
		// Section dirty alone is too broad for PT runtime replacement because
		// the raster path can mark transient warped sections dirty during draw
		// prep without producing a stable gameplay map mutation. Keep explicit
		// forced invalidation for sector-dirty, but do not let the sticky
		// dragged-sector ownership bit force perpetual rebuilds once PT already
		// has a valid replacement baseline for the chunk.
		const bool forceTopologyInvalidation =
			(normalizedReasonMask & nri_scene::PTMapChunkMutationReason_SectorDirty) != 0;
		RuntimeMutationTraceAction chunkTraceAction = RuntimeMutationTraceAction::None;
		bool chunkTraceResidentMaterialDirty = false;
		bool chunkTraceResidentGeometryDirty = false;
		bool chunkTraceRecoveredEmpty = false;
		uint32_t chunkTraceSurfaceCount = replacement.surfaceCount;
		uint32_t chunkTraceTriangleCount = replacement.triangleCount;
		uint32_t chunkTraceMaterialCount = (uint32_t)replacement.materialBridge.materials.size();
		bool sectorDirtyTruthCaptured = false;
		const uint32_t previousReplacementSurfaceCount = replacement.surfaceCount;
		const uint32_t previousReplacementTriangleCount = replacement.triangleCount;
		auto emitChunkTrace = [&]()
		{
			recordRuntimeMutationTopEntry(
				mapChunk.chunkIndex,
				mapChunk.sectorIndex,
				normalizedReasonMask,
				replacement.sectionDirtyCount,
				chunkTraceSurfaceCount,
				chunkTraceTriangleCount,
				chunkTraceMaterialCount,
				chunkTraceAction,
				forceTopologyInvalidation,
				chunkTraceResidentMaterialDirty,
				chunkTraceResidentGeometryDirty,
				chunkTraceRecoveredEmpty);
		};
		mRuntimeMutation.NoteMutationReasonMask(normalizedReasonMask);
		const uint32_t startupMaterialOnlyReasonMask =
			nri_scene::PTMapChunkMutationReason_SectorMaterial |
			nri_scene::PTMapChunkMutationReason_WallMaterial;
		if (mAllowStartupMutationRebaseline &&
			mFrameIndex <= mStartupMutationRebaselineDeadlineFrame &&
			(normalizedReasonMask & startupMaterialOnlyReasonMask) != 0 &&
			(normalizedReasonMask & ~startupMaterialOnlyReasonMask) == 0)
		{
			startupMaterialOnlyMutationDetected = true;
		}

		const bool useStaticAnimatedReplacement =
			normalizedReasonMask == nri_scene::PTMapChunkMutationReason_None &&
			isVisibleSuppressedStaticAnimatedChunk(mapChunk.chunkIndex);
		if (normalizedReasonMask == nri_scene::PTMapChunkMutationReason_None &&
			!useStaticAnimatedReplacement &&
			replacement.residentAuthoritative &&
			chunkVisibleNow &&
			residentEntry != nullptr &&
			residentEntry->valid &&
			residentEntry->visibleValidationFramesRemaining > 0)
		{
			uint64_t liveVisibleGeometrySignature = 0;
			uint64_t liveVisibleMaterialSignature = 0;
			nri_scene::SceneView liveVisibleChunkView;
			nri_scene::PTMapWorldStats ignoredVisibleStats = {};
			nri_scene::PTMapBuildOptions mapBuildOptions = {};
			if (nri_scene::BuildLiveMapChunkSceneView(mapChunk, liveVisibleChunkView, &ignoredVisibleStats, mapBuildOptions))
			{
				if (nri_ptceilingnudge)
				{
					NudgeMapCeilingSections(liveVisibleChunkView, (float)nri_ptceilingnudgedistance);
				}
				liveVisibleGeometrySignature = ComputeExactGeometrySignature(liveVisibleChunkView);
				liveVisibleMaterialSignature = ComputeAnimatedMaterialSignature(liveVisibleChunkView);
				if (liveVisibleGeometrySignature != residentEntry->exactGeometrySignature)
				{
					normalizedReasonMask |= nri_scene::PTMapChunkMutationReason_SectionDirty;
				}
				else if (liveVisibleMaterialSignature != residentEntry->animatedMaterialSignature)
				{
					normalizedReasonMask |= nri_scene::PTMapChunkMutationReason_SectorMaterial;
				}
				else if (!chunkHasUnresolvedAuthoredTextures)
				{
					if (startupVisibleValidationPending &&
						mapChunk.chunkIndex < mPendingStartupVisibleChunkValidation.size())
					{
						mPendingStartupVisibleChunkValidation[mapChunk.chunkIndex] = 0u;
					}
					residentEntry->visibleValidationFramesRemaining = 0;
				}
			}

			if (residentEntry->visibleValidationFramesRemaining > 0 && !chunkHasUnresolvedAuthoredTextures)
			{
				residentEntry->visibleValidationFramesRemaining--;
			}
			if (normalizedReasonMask != nri_scene::PTMapChunkMutationReason_None &&
				ShouldTracePtPerf() &&
				!residentEntry->visibleValidationTraceEmitted)
			{
				Printf("NRI PT visible resident validation: chunk=%u sector=%d reason_mask=0x%x unresolved_textures=%s startup_pending=%s validation_frames=%u static_geom_sig=0x%llx live_geom_sig=0x%llx static_mat_sig=0x%llx live_mat_sig=0x%llx\n",
					mapChunk.chunkIndex,
					mapChunk.sectorIndex,
					normalizedReasonMask,
					YesNo(chunkHasUnresolvedAuthoredTextures),
					YesNo(startupVisibleValidationPending),
					(uint32_t)residentEntry->visibleValidationFramesRemaining,
					(unsigned long long)residentEntry->exactGeometrySignature,
					(unsigned long long)liveVisibleGeometrySignature,
					(unsigned long long)residentEntry->animatedMaterialSignature,
					(unsigned long long)liveVisibleMaterialSignature);
				PrintMapChunkCompare((int32_t)mapChunk.chunkIndex);
				residentEntry->visibleValidationTraceEmitted = true;
			}
		}
		if (normalizedReasonMask == nri_scene::PTMapChunkMutationReason_None && !useStaticAnimatedReplacement)
		{
			replacement.active = false;
			replacement.excludeStaticChunk = false;
			replacement.staticAnimatedReplacement = false;
			replacement.animationOnlyRefreshed = false;
			if (!replacement.residentAuthoritative)
			{
				replacement.animatedMaterialSignature = 0;
			}
			replacement.stableMutationFrameCount = 0;
			chunkTraceAction = RuntimeMutationTraceAction::None;
			mRuntimeMutation.TraceChunk(mapChunk, replacement, ShouldEmitTemporalTraceLogs(), (int)nri_ptmutationtracechunk, (int)nri_ptmutationtracesector);
			emitChunkTrace();
			mRuntimeMutation.ClearReplacementPayload(replacement, true);
			continue;
		}

		if (!useStaticAnimatedReplacement)
		{
			mRuntimeMutation.NoteDirtyChunk(replacement.blindSpot);
			mLastPerfShellTraceStats.runtimeMutationDirtyChunks++;
			recordRuntimeMutationDirtyTier(chunkVisibleNow, runtimeMutationDistanceTier, runtimeMutationCandidateSourceMask);
		}

		const bool materialOnlyReplacement = IsRuntimeMutationMaterialOnlyReasonMask(normalizedReasonMask);
		nri_scene::PTMapChunkMutationAnalysis replacementDelta = {};
		const bool haveReplacementBaseline = replacement.valid || replacement.residentAuthoritative;
		const bool analyzedReplacementDelta =
			haveReplacementBaseline &&
			nri_scene::AnalyzeMapChunkMutation(mapChunk, replacement.replacementBaseline, replacementDelta);
		const uint32_t structuralReasonMask =
			nri_scene::PTMapChunkMutationReason_SectorGeometry |
			nri_scene::PTMapChunkMutationReason_WallGeometry |
			nri_scene::PTMapChunkMutationReason_SectorDirty |
			nri_scene::PTMapChunkMutationReason_SectionDirty;
		const uint32_t replacementViewReasonMask =
			nri_scene::PTMapChunkMutationReason_SectorGeometry |
			nri_scene::PTMapChunkMutationReason_SectorMaterial |
			nri_scene::PTMapChunkMutationReason_WallGeometry |
			nri_scene::PTMapChunkMutationReason_WallMaterial |
			nri_scene::PTMapChunkMutationReason_SectorDirty |
			nri_scene::PTMapChunkMutationReason_SectionDirty |
			nri_scene::PTMapChunkMutationReason_Dragged;
		const uint32_t replacementRefreshReasonMask =
			replacementDelta.reasonMask & ~nri_scene::PTMapChunkMutationReason_Dragged;
		const bool useOverlayReplacementState = replacement.valid;
		const bool replacementViewChanged =
			useOverlayReplacementState &&
			(previousReasonMask & replacementViewReasonMask) !=
			(normalizedReasonMask & replacementViewReasonMask);
		replacement.animationOnlyRefreshed = false;
		nri_scene::PTMapWorld liveWorld = {};
		nri_scene::SceneView liveChunkView;
		nri_scene::PTMapWorldStats liveStats = {};
		bool havePreparedLiveChunkView = false;
		bool attemptedResidentComparableLiveChunkView = false;
		bool haveResidentComparableLiveChunkView = false;
		nri_scene::SceneView residentComparableLiveChunkView;
		const auto prepareLiveChunkView = [&]() -> bool
		{
			if (havePreparedLiveChunkView)
			{
				return true;
			}

			nri_scene::PTMapBuildOptions mapBuildOptions = {};
			if (!nri_scene::BuildLiveMapChunkWorld(mapChunk, liveWorld, &liveStats, mapBuildOptions))
			{
				return false;
			}
			if (liveWorld.chunks.size() != 1 ||
				liveWorld.chunks[0].chunkIndex != mapChunk.chunkIndex ||
				liveWorld.chunks[0].sectorIndex != mapChunk.sectorIndex)
			{
				return false;
			}
			mMapMoverShadow.ObserveLiveChunk(
				mMapMovers,
				liveWorld,
				mFrameIndex,
				(int)nri_ptmapmovershadow);

			nri_scene::BuildMapChunkSceneView(liveWorld, liveWorld.chunks[0], liveChunkView);
			const bool exclusiveMaterialOnlyReplacement =
				materialOnlyReplacement &&
				RequiresExclusiveRuntimeMutationMaterialOnlyReasonMask(normalizedReasonMask);
			const bool blindSpotReplacementNudged = replacement.blindSpot && replacement.dragged;
			if (blindSpotReplacementNudged)
			{
				NudgeBlindSpotReplacementFlats(liveChunkView);
			}
			else if (nri_ptceilingnudge)
			{
				NudgeMapCeilingSections(liveChunkView, (float)nri_ptceilingnudgedistance);
			}
			if (materialOnlyReplacement && !exclusiveMaterialOnlyReplacement)
			{
				FilterMaterialOnlyReplacementSceneView(liveChunkView, normalizedReasonMask);
			}
			ApplyCommittedMapMotion(liveChunkView);

			havePreparedLiveChunkView = true;
			return true;
		};
		const auto tryResolveResidentChunkState =
			[&](uint32_t& outResidentChunkListIndex,
				const StaticMapSceneCache::ChunkCache*& outResidentChunkCache,
				const nri_scene::SceneView*& outResidentChunkView) -> bool
		{
			outResidentChunkListIndex = UINT32_MAX;
			outResidentChunkCache = nullptr;
			outResidentChunkView = nullptr;
			if (residentEntry == nullptr || !residentEntry->valid)
			{
				return false;
			}

			outResidentChunkListIndex = residentEntry->staticSceneChunkListIndex;
			const bool residentChunkListIndexValid =
				outResidentChunkListIndex < mStaticMapScene.chunks.size() &&
				mStaticMapScene.chunks[outResidentChunkListIndex].chunkIndex == mapChunk.chunkIndex;
			if (!residentChunkListIndexValid)
			{
				outResidentChunkListIndex = NRIStaticSceneResidency::FindPreferredStaticSceneChunkListIndex(
					mStaticMapScene,
					mStaticMapChunkAtlas,
					mapChunk.chunkIndex);
			}
			if (outResidentChunkListIndex >= mStaticMapScene.chunks.size() ||
				outResidentChunkListIndex >= mStaticMapScene.lightChunkViews.size() ||
				!mStaticMapScene.chunks[outResidentChunkListIndex].active)
			{
				return false;
			}

			outResidentChunkCache = &mStaticMapScene.chunks[outResidentChunkListIndex];
			outResidentChunkView = &mStaticMapScene.lightChunkViews[outResidentChunkListIndex];
			return true;
		};
		const auto tryMergeSectorMaterialOnlyPreparedMaterials =
			[&](const nri_scene::SceneView& preparedLiveChunkView,
				nri_scene::MaterialBridgeData& inOutPreparedLiveMaterials) -> bool
		{
			if (!IsPureSectorRuntimeMutationMaterialOnlyReasonMask(normalizedReasonMask))
			{
				return false;
			}

			uint32_t residentChunkListIndex = UINT32_MAX;
			const StaticMapSceneCache::ChunkCache* residentChunkCache = nullptr;
			const nri_scene::SceneView* residentChunkView = nullptr;
			if (!tryResolveResidentChunkState(residentChunkListIndex, residentChunkCache, residentChunkView) ||
				residentChunkCache == nullptr ||
				residentChunkView == nullptr)
			{
				return false;
			}

			nri_scene::MaterialBridgeData mergedMaterials;
			if (!TryBuildMergedSectorMaterialOnlyBridge(
				*residentChunkView,
				residentChunkCache->materialBridge,
				preparedLiveChunkView,
				inOutPreparedLiveMaterials,
				mergedMaterials))
			{
				return false;
			}

			inOutPreparedLiveMaterials = std::move(mergedMaterials);
			return true;
		};
		const auto getResidentComparableLiveChunkView =
			[&]() -> const nri_scene::SceneView*
		{
			if (attemptedResidentComparableLiveChunkView)
			{
				return haveResidentComparableLiveChunkView ? &residentComparableLiveChunkView : nullptr;
			}

			attemptedResidentComparableLiveChunkView = true;
			haveResidentComparableLiveChunkView = false;
			if (!IsPureSectorRuntimeMutationMaterialOnlyReasonMask(normalizedReasonMask))
			{
				return nullptr;
			}

			uint32_t residentChunkListIndex = UINT32_MAX;
			const StaticMapSceneCache::ChunkCache* residentChunkCache = nullptr;
			const nri_scene::SceneView* residentChunkView = nullptr;
			if (!tryResolveResidentChunkState(residentChunkListIndex, residentChunkCache, residentChunkView) ||
				residentChunkView == nullptr)
			{
				return nullptr;
			}
			// Pure sector-material chunks can still animate sector-driven wall bands.
			// Replacing the live wall set with resident walls hides those updates and
			// can make the chunk look synchronized after a single frame.
			if (SceneViewHasSectorDrivenWallBands(*residentChunkView))
			{
				return nullptr;
			}

			if (!TryBuildMergedSectorMaterialOnlySceneView(
				*residentChunkView,
				liveChunkView,
				residentComparableLiveChunkView))
			{
				return nullptr;
			}

			haveResidentComparableLiveChunkView = true;
			return &residentComparableLiveChunkView;
		};
		const auto recordPreparedLiveChunkMaterialOnlyMismatch =
			[&](const nri_scene::SceneView& preparedLiveChunkView,
				const nri_scene::MaterialBridgeData& preparedLiveMaterials,
				bool refreshPath)
		{
			const bool exclusiveMaterialOnlyReplacement =
				materialOnlyReplacement &&
				RequiresExclusiveRuntimeMutationMaterialOnlyReasonMask(normalizedReasonMask);
			if (exclusiveMaterialOnlyReplacement)
			{
				return;
			}

			const uint32_t filteredMaterialCount = (uint32_t)preparedLiveMaterials.materials.size();
			uint32_t residentChunkListIndex = UINT32_MAX;
			const StaticMapSceneCache::ChunkCache* residentChunkCache = nullptr;
			const nri_scene::SceneView* residentChunkView = nullptr;
			if (!tryResolveResidentChunkState(residentChunkListIndex, residentChunkCache, residentChunkView) ||
				residentChunkCache == nullptr ||
				residentChunkView == nullptr)
			{
				return;
			}

			const uint32_t residentMaterialCount = residentChunkCache->materialCount;
			if (filteredMaterialCount == residentMaterialCount)
			{
				return;
			}

			const uint32_t residentWallCount = (uint32_t)residentChunkView->opaqueWalls.size();
			const uint32_t residentFlatCount = (uint32_t)residentChunkView->opaqueFlats.size();
			const uint32_t filteredWallCount = (uint32_t)preparedLiveChunkView.opaqueWalls.size();
			const uint32_t filteredFlatCount = (uint32_t)preparedLiveChunkView.opaqueFlats.size();
			mLastPerfShellTraceStats.runtimeMutationMaterialOnlyMismatchCount++;
			if (refreshPath)
			{
				mLastPerfShellTraceStats.runtimeMutationMaterialOnlyMismatchRefreshCount++;
			}
			else
			{
				mLastPerfShellTraceStats.runtimeMutationMaterialOnlyMismatchRebuildCount++;
			}
			if (filteredWallCount != 0 && filteredFlatCount == 0)
			{
				mLastPerfShellTraceStats.runtimeMutationMaterialOnlyMismatchFilteredWallOnlyCount++;
			}
			else if (filteredFlatCount != 0 && filteredWallCount == 0)
			{
				mLastPerfShellTraceStats.runtimeMutationMaterialOnlyMismatchFilteredFlatOnlyCount++;
			}
			else
			{
				mLastPerfShellTraceStats.runtimeMutationMaterialOnlyMismatchFilteredMixedCount++;
			}

			if (residentWallCount != 0 && residentFlatCount == 0)
			{
				mLastPerfShellTraceStats.runtimeMutationMaterialOnlyMismatchResidentWallOnlyCount++;
			}
			else if (residentFlatCount != 0 && residentWallCount == 0)
			{
				mLastPerfShellTraceStats.runtimeMutationMaterialOnlyMismatchResidentFlatOnlyCount++;
			}
			else
			{
				mLastPerfShellTraceStats.runtimeMutationMaterialOnlyMismatchResidentMixedCount++;
			}

			recordMaterialOnlyMismatchEntry(
				mapChunk.chunkIndex,
				mapChunk.sectorIndex,
				refreshPath,
				normalizedReasonMask,
				CountSceneViewSurfaces(preparedLiveChunkView),
				filteredMaterialCount,
				residentMaterialCount,
				filteredWallCount,
				filteredFlatCount,
				residentWallCount,
				residentFlatCount);
		};
		static constexpr size_t kRuntimeMutationMaterialStateCacheCapacity = 8;
		const auto tryGetCachedRuntimeMutationMaterials =
			[&replacement](uint64_t animatedGeometrySignature,
				uint64_t animatedMaterialSignature,
				uint32_t surfaceCount,
				const nri_scene::CanonicalPTMapMaterialLayout* canonicalLayout,
				nri_scene::MaterialBridgeData& outMaterials) -> bool
		{
			auto& cache = replacement.materialStateCache;
			for (size_t index = 0; index < cache.size(); ++index)
			{
				const auto& entry = cache[index];
				if (entry.animatedGeometrySignature != animatedGeometrySignature ||
					entry.animatedMaterialSignature != animatedMaterialSignature ||
					entry.surfaceCount != surfaceCount ||
					(canonicalLayout != nullptr &&
					 (entry.canonicalMaterialStateKey != canonicalLayout->stateKey ||
					  entry.canonicalMaterialStateKeys != canonicalLayout->canonicalMaterialStateKeys)))
				{
					continue;
				}

				if (index != 0)
				{
					auto hotEntry = std::move(cache[index]);
					cache.erase(cache.begin() + (ptrdiff_t)index);
					cache.insert(cache.begin(), std::move(hotEntry));
				}
				outMaterials = cache.front().materialBridge;
				return true;
			}

			return false;
		};
		const auto storeCachedRuntimeMutationMaterials =
			[&replacement](uint64_t animatedGeometrySignature,
				uint64_t animatedMaterialSignature,
				uint32_t surfaceCount,
				const nri_scene::CanonicalPTMapMaterialLayout* canonicalLayout,
				const nri_scene::MaterialBridgeData& materials)
		{
			if (materials.materials.empty())
			{
				return;
			}

			auto& cache = replacement.materialStateCache;
			for (size_t index = 0; index < cache.size(); ++index)
			{
				auto& entry = cache[index];
				if (entry.animatedGeometrySignature != animatedGeometrySignature ||
					entry.animatedMaterialSignature != animatedMaterialSignature ||
					entry.surfaceCount != surfaceCount ||
					(canonicalLayout != nullptr &&
					 (entry.canonicalMaterialStateKey != canonicalLayout->stateKey ||
					  entry.canonicalMaterialStateKeys != canonicalLayout->canonicalMaterialStateKeys)))
				{
					continue;
				}

				entry.materialBridge = materials;
				if (index != 0)
				{
					auto hotEntry = std::move(entry);
					cache.erase(cache.begin() + (ptrdiff_t)index);
					cache.insert(cache.begin(), std::move(hotEntry));
				}
				return;
			}

			RuntimeMapMutationCache::ChunkReplacement::MaterialStateCacheEntry entry = {};
			entry.animatedGeometrySignature = animatedGeometrySignature;
			entry.animatedMaterialSignature = animatedMaterialSignature;
			entry.surfaceCount = surfaceCount;
			if (canonicalLayout != nullptr)
			{
				entry.canonicalMaterialStateKey = canonicalLayout->stateKey;
				entry.canonicalMaterialStateKeys = canonicalLayout->canonicalMaterialStateKeys;
			}
			entry.materialBridge = materials;
			cache.insert(cache.begin(), std::move(entry));
			if (cache.size() > kRuntimeMutationMaterialStateCacheCapacity)
			{
				cache.pop_back();
			}
		};
		const auto tryBuildCertifiedMaterialOnlyReplacement =
			[&](bool countAsStructuralRebuild, bool allowAnimatedMaterialState) -> bool
		{
			if (!materialOnlyReplacement && !allowAnimatedMaterialState)
			{
				return false;
			}
			if (!havePreparedLiveChunkView || liveWorld.chunks.size() != 1)
			{
				mMapMaterialOnlyRoute.NotePreflightReject(
					NRIMapMaterialOnlyRoutePreflightReject::LiveChunkNotPrepared);
				return false;
			}

			uint32_t residentChunkListIndex = UINT32_MAX;
			const StaticMapSceneCache::ChunkCache* residentChunkCache = nullptr;
			const nri_scene::SceneView* residentChunkView = nullptr;
			if (!tryResolveResidentChunkState(residentChunkListIndex, residentChunkCache, residentChunkView) ||
				residentChunkCache == nullptr || residentChunkView == nullptr ||
				residentChunkListIndex >= mStaticMapChunkAtlas.chunks.size())
			{
				mMapMaterialOnlyRoute.NotePreflightReject(
					NRIMapMaterialOnlyRoutePreflightReject::ResidentStateUnavailable);
				return false;
			}
			const auto& atlasChunk = mStaticMapChunkAtlas.chunks[residentChunkListIndex];
			if (!atlasChunk.valid || atlasChunk.materialCount == 0 ||
				residentChunkCache->materialCount != atlasChunk.materialCount ||
				residentChunkCache->accelerationStructure.accelerationStructure == nullptr)
			{
				mMapMaterialOnlyRoute.NotePreflightReject(
					NRIMapMaterialOnlyRoutePreflightReject::AtlasStateUnavailable);
				return false;
			}

			NRIMapMaterialOnlyRouteInput routeInput;
			routeInput.movers = &mMapMovers;
			routeInput.retainedWorld = &mMapWorld;
			routeInput.retainedChunk = &mapChunk;
			routeInput.retainedSceneView = residentChunkView;
			routeInput.retainedLayout = &residentChunkCache->canonicalMaterialLayout;
			routeInput.currentWorld = &liveWorld;
			routeInput.currentChunk = &liveWorld.chunks[0];
			routeInput.currentSceneView = &liveChunkView;
			routeInput.buildSerial = mMapWorld.buildSerial;
			routeInput.mapEpoch = mMapMovers.GetMapEpoch();
			routeInput.frameIndex = mFrameIndex;
			// The refresh caller supplies the engine-activity authority: no authored
			// replacement delta and no visible mutable canvas, but the resolved material
			// signature advanced. The retained static hint is intentionally not used as
			// authority because it misses runtime-resolved Build tile animations.
			routeInput.allowAnimatedMaterialState = allowAnimatedMaterialState;
			NRIMapMaterialOnlyRouteResult routeResult = mMapMaterialOnlyRoute.TryPrepare(routeInput);
			if (!routeResult.admitted)
			{
				return false;
			}

			auto& mutableResidentChunk = mStaticMapScene.chunks[residentChunkListIndex];
			if (!mutableResidentChunk.canonicalMaterialLayout.valid)
			{
				mutableResidentChunk.canonicalMaterialLayout = routeResult.retainedLayout;
			}
			const uint64_t canonicalGeometrySignature =
				ComputeAnimatedGeometrySignature(routeResult.residentOrderSceneView);
			const uint64_t canonicalMaterialSignature =
				ComputeAnimatedMaterialSignature(routeResult.residentOrderSceneView);
			const uint32_t surfaceCount = CountSceneViewSurfaces(routeResult.residentOrderSceneView);
			nri_scene::MaterialBridgeData residentOrderMaterials;
			if (tryGetCachedRuntimeMutationMaterials(
				canonicalGeometrySignature,
				canonicalMaterialSignature,
				surfaceCount,
				&routeResult.currentLayout,
				residentOrderMaterials))
			{
				mLastPerfShellTraceStats.runtimeMutationMaterialCacheHitCount++;
			}
			else
			{
				mLastPerfShellTraceStats.runtimeMutationMaterialCacheMissCount++;
				{
					Clocker clock(NriPTMaterialBuild);
					BuildMaterialsWithActorOverrides(
						routeResult.residentOrderSceneView,
						residentOrderMaterials,
						"runtime_mutation_chunk");
				}
				storeCachedRuntimeMutationMaterials(
					canonicalGeometrySignature,
					canonicalMaterialSignature,
					surfaceCount,
					&routeResult.currentLayout,
					residentOrderMaterials);
				mLastPerfShellTraceStats.runtimeMutationMaterialCacheStoreCount++;
			}
			if (residentOrderMaterials.materials.size() != atlasChunk.materialCount ||
				residentOrderMaterials.lightMetadata.size() != atlasChunk.materialCount)
			{
				return false;
			}

			nri_scene::PTMapChunkMutationBaseline liveBaseline;
			if (!nri_scene::CaptureMapChunkMutationBaseline(mapChunk, liveBaseline))
			{
				return false;
			}
			mRuntimeMutation.BuildLightIdentityOverrides(
				mMapWorld,
				mapChunk,
				liveWorld,
				liveWorld.chunks[0],
				replacement.lightIdentityOverrides);

			replacement.sceneView = std::move(routeResult.residentOrderSceneView);
			replacement.geometry = {};
			replacement.materialBridge = std::move(residentOrderMaterials);
			replacement.replacementBaseline = std::move(liveBaseline);
			replacement.exactGeometrySignature = residentChunkCache->exactGeometrySignature;
			replacement.surfaceCount = surfaceCount;
			replacement.triangleCount = atlasChunk.primitiveCount;
			replacement.animatedMaterialSignature = canonicalMaterialSignature;
			replacement.valid = true;
			replacement.active = true;
			if (!IsRuntimeMutationMaterialOnlyReasonMask(replacement.reasonMask))
			{
				// Resolved tile animation can advance without mutating authored map
				// fields. Give resident apply an explicit material-only operation kind.
				replacement.reasonMask = nri_scene::PTMapChunkMutationReason_WallMaterial;
			}
			replacement.excludeStaticChunk = false;
			replacement.staticAnimatedReplacement = useStaticAnimatedReplacement;
			replacement.certifiedResidentMaterialOnly = true;
			replacement.certifiedMaterialBuildSerial = mMapWorld.buildSerial;
			replacement.certifiedMaterialMapEpoch = mMapMovers.GetMapEpoch();
			replacement.certifiedMaterialOwnerStableId = routeResult.ownerStableId;
			replacement.certifiedMaterialLayoutKey = routeResult.layoutKey;
			replacement.certifiedMaterialStateKey = routeResult.stateKey;
			replacement.certifiedExactGeometrySignature = residentChunkCache->exactGeometrySignature;
			replacement.certifiedGeometryTopologySignature = residentChunkCache->geometryTopologySignature;
			replacement.certifiedPrimitiveLayoutSignature = residentChunkCache->primitiveLayoutSignature;
			replacement.certifiedChunkListIndex = residentChunkListIndex;
			replacement.certifiedVertexOffset = atlasChunk.vertexOffset;
			replacement.certifiedVertexCount = atlasChunk.vertexCount;
			replacement.certifiedIndexOffset = atlasChunk.indexOffset;
			replacement.certifiedIndexCount = atlasChunk.indexCount;
			replacement.certifiedPrimitiveOffset = atlasChunk.primitiveOffset;
			replacement.certifiedPrimitiveCount = atlasChunk.primitiveCount;
			replacement.certifiedMaterialOffset = atlasChunk.materialOffset;
			replacement.certifiedMaterialCount = atlasChunk.materialCount;
			if (countAsStructuralRebuild)
			{
				mRuntimeMutation.NoteRebuiltChunk();
				mLastPerfShellTraceStats.runtimeMutationRebuiltChunks++;
			}
			return true;
		};
		const auto captureSectorDirtyTruth = [&]()
		{
			if (!ShouldTracePtPerf() ||
				sectorDirtyTruthCaptured ||
				(normalizedReasonMask & nri_scene::PTMapChunkMutationReason_SectorDirty) == 0 ||
				!havePreparedLiveChunkView)
			{
				return;
			}

			uint64_t liveBaselineSignature = 0;
			nri_scene::PTMapChunkMutationBaseline truthBaseline;
			if (nri_scene::CaptureMapChunkMutationBaseline(mapChunk, truthBaseline))
			{
				liveBaselineSignature = truthBaseline.signature;
			}

			uint64_t previousGeometrySignature = 0;
			uint64_t previousMaterialSignature = 0;
			RuntimeSectorDirtyTruthTraceEntry::PreviousStateSource previousStateSource =
				RuntimeSectorDirtyTruthTraceEntry::PreviousStateSource::None;
			bool havePreviousSignatures = false;
			uint32_t previousSurfaceCount = 0;
			uint32_t previousTriangleCount = 0;
			uint32_t liveBuiltTriangleCount = liveStats.triangleCount;
			if (replacement.valid)
			{
				previousGeometrySignature = ComputeExactGeometrySignature(replacement.sceneView);
				previousMaterialSignature = replacement.animatedMaterialSignature;
				previousStateSource = RuntimeSectorDirtyTruthTraceEntry::PreviousStateSource::Replacement;
				havePreviousSignatures = true;
				previousSurfaceCount = previousReplacementSurfaceCount;
				previousTriangleCount = previousReplacementTriangleCount;
			}
			else if (residentEntry != nullptr && residentEntry->valid)
			{
				previousGeometrySignature = residentEntry->exactGeometrySignature;
				previousMaterialSignature = residentEntry->animatedMaterialSignature;
				previousStateSource = RuntimeSectorDirtyTruthTraceEntry::PreviousStateSource::Resident;
				havePreviousSignatures = true;
				previousSurfaceCount = residentEntry->materialCount;
				previousTriangleCount = residentEntry->primitiveCount;
			}

			const bool liveGeometryChanged =
				!havePreviousSignatures ||
				ComputeExactGeometrySignature(liveChunkView) != previousGeometrySignature;
			const bool liveMaterialChanged =
				!havePreviousSignatures ||
				ComputeAnimatedMaterialSignature(liveChunkView) != previousMaterialSignature;
			nri_scene::GeometryData liveTruthGeometry;
			{
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.geometryBuildRuntimeMutationTruthMs);
				nri_scene::BuildGeometry(liveChunkView, liveTruthGeometry);
			}
			liveBuiltTriangleCount = (uint32_t)liveTruthGeometry.primitives.size();
			mLastPerfShellTraceStats.geometryBuildRuntimeMutationTruthCalls++;
			mLastPerfShellTraceStats.geometryBuildRuntimeMutationPrimitives += liveBuiltTriangleCount;
			if (forceTopologyInvalidation &&
				previousStateSource == RuntimeSectorDirtyTruthTraceEntry::PreviousStateSource::Resident &&
				!liveGeometryChanged &&
				!liveMaterialChanged)
			{
				mLastPerfShellTraceStats.runtimeMutationResidentNoopCandidateCount++;
				mLastPerfShellTraceStats.runtimeMutationResidentNoopCandidateReasonMaskOr |= normalizedReasonMask;
			}

			recordSectorDirtyTruthEntry(
				mapChunk.chunkIndex,
				mapChunk.sectorIndex,
				normalizedReasonMask,
				previousStateSource,
				forceTopologyInvalidation,
				liveBaselineSignature != replacement.replacementBaseline.signature,
				liveGeometryChanged,
				liveMaterialChanged,
				previousSurfaceCount,
				CountSceneViewSurfaces(liveChunkView),
				previousTriangleCount,
				liveBuiltTriangleCount);
			sectorDirtyTruthCaptured = true;
		};
		const auto rebuildReplacementFromPreparedLiveChunk = [&](bool countAsStructuralRebuild) -> bool
		{
			if (tryBuildCertifiedMaterialOnlyReplacement(countAsStructuralRebuild, false))
			{
				return true;
			}
			const bool exclusiveMaterialOnlyReplacement =
				materialOnlyReplacement &&
				RequiresExclusiveRuntimeMutationMaterialOnlyReasonMask(normalizedReasonMask);
			const bool excludeStaticChunk =
				useStaticAnimatedReplacement ||
				!materialOnlyReplacement ||
				exclusiveMaterialOnlyReplacement;
			const nri_scene::SceneView* residentComparableView = getResidentComparableLiveChunkView();
			const nri_scene::SceneView& animatedSignatureSceneView =
				residentComparableView != nullptr ? *residentComparableView : liveChunkView;
			const uint64_t liveAnimatedGeometrySignature =
				ComputeAnimatedGeometrySignature(animatedSignatureSceneView);
			const uint64_t liveAnimatedMaterialSignature =
				ComputeAnimatedMaterialSignature(animatedSignatureSceneView);
			const uint32_t liveSurfaceCount = CountSceneViewSurfaces(liveChunkView);
			mRuntimeMutation.BuildLightIdentityOverrides(
				mMapWorld,
				mapChunk,
				liveWorld,
				liveWorld.chunks[0],
				replacement.lightIdentityOverrides);

			nri_scene::GeometryData liveGeometry;
			nri_scene::MaterialBridgeData liveMaterials;
			NRISE29FloorDeformerRouteResult deformerRouteResult;
			{
				Clocker clock(NriPTGeometryBuild);
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.geometryBuildRuntimeMutationRebuildMs);
				{
					ScopedPtPerfTimer bridgeTimer(mLastPerfShellTraceStats.runtimeMutationGeometryBridgeMs);
					nri_scene::BuildGeometry(liveChunkView, liveGeometry);
				}
				{
					ScopedPtPerfTimer portalTimer(mLastPerfShellTraceStats.runtimeMutationPortalAssignMs);
					AssignGeometryPortalIndices(mMapWorld, liveGeometry);
				}
			}
			if (tryGetCachedRuntimeMutationMaterials(
				liveAnimatedGeometrySignature,
				liveAnimatedMaterialSignature,
				liveSurfaceCount,
				nullptr,
				liveMaterials))
			{
				mLastPerfShellTraceStats.runtimeMutationMaterialCacheHitCount++;
			}
			else
			{
				mLastPerfShellTraceStats.runtimeMutationMaterialCacheMissCount++;
				{
					Clocker clock(NriPTMaterialBuild);
					BuildMaterialsWithActorOverrides(liveChunkView, liveMaterials, "runtime_mutation_chunk");
				}
				storeCachedRuntimeMutationMaterials(
					liveAnimatedGeometrySignature,
					liveAnimatedMaterialSignature,
					liveSurfaceCount,
					nullptr,
					liveMaterials);
				mLastPerfShellTraceStats.runtimeMutationMaterialCacheStoreCount++;
			}
			tryMergeSectorMaterialOnlyPreparedMaterials(liveChunkView, liveMaterials);
			recordPreparedLiveChunkMaterialOnlyMismatch(liveChunkView, liveMaterials, false);
			// Canonical mapping allocates temporary graph/search state. Run it after
			// material construction so that bounded deformer work cannot perturb the
			// immediately following allocation-heavy material bridge hot path.
			{
				Clocker clock(NriPTGeometryBuild);
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.geometryBuildRuntimeMutationRebuildMs);
				ScopedPtPerfTimer deformerTimer(mLastPerfShellTraceStats.runtimeMutationDeformerCanonicalMs);
				NRISE29FloorDeformerRouteInput deformerRouteInput;
				deformerRouteInput.movers = &mMapMovers;
				deformerRouteInput.mapWorld = &mMapWorld;
				deformerRouteInput.staticScene = &mStaticMapScene;
				deformerRouteInput.atlas = &mStaticMapChunkAtlas;
				deformerRouteInput.registry = &mStaticSceneResidency.Registry();
				deformerRouteInput.mapChunk = &mapChunk;
				deformerRouteInput.exactCurrentGeometry = &liveGeometry;
				deformerRouteInput.frameIndex = mFrameIndex;
				deformerRouteInput.rayVisible = chunkVisibleNow;
				deformerRouteInput.required = chunkVisibleNow;
				deformerRouteInput.enabled = (int)nri_ptmapmovermode >= 1;
				deformerRouteResult = mSE29FloorDeformerRoute.TryCanonicalize(deformerRouteInput);
				if (deformerRouteResult.admitted)
				{
					liveGeometry = std::move(deformerRouteResult.canonicalGeometry);
				}
			}
			mLastPerfShellTraceStats.geometryBuildRuntimeMutationRebuildCalls++;
			mLastPerfShellTraceStats.geometryBuildRuntimeMutationPrimitives += (uint32_t)liveGeometry.primitives.size();
			nri_scene::PTMapChunkMutationBaseline liveBaseline;
			if (!nri_scene::CaptureMapChunkMutationBaseline(mapChunk, liveBaseline))
			{
				return false;
			}

			replacement.sceneView = liveChunkView;
			replacement.exactGeometrySignature = 0;
			replacement.geometry = std::move(liveGeometry);
			replacement.materialBridge = std::move(liveMaterials);
			replacement.replacementBaseline = std::move(liveBaseline);
			replacement.fixedLayoutDeformer = deformerRouteResult.admitted;
			replacement.fixedLayoutDeformerKey = deformerRouteResult.admitted ? deformerRouteResult.stableKey : 0;
			replacement.fixedLayoutVertexSpans.clear();
			replacement.fixedLayoutPrimitiveSpans.clear();
			for (const auto& span : deformerRouteResult.vertexSpans)
			{
				replacement.fixedLayoutVertexSpans.push_back({ span.firstElement, span.elementCount });
			}
			for (const auto& span : deformerRouteResult.primitiveSpans)
			{
				replacement.fixedLayoutPrimitiveSpans.push_back({ span.firstElement, span.elementCount });
			}
			replacement.surfaceCount = liveSurfaceCount;
			replacement.triangleCount = (uint32_t)replacement.geometry.primitives.size();
			replacement.animatedMaterialSignature = liveAnimatedMaterialSignature;
			replacement.valid = true;
			replacement.active = true;
			replacement.excludeStaticChunk = excludeStaticChunk;
			replacement.staticAnimatedReplacement = useStaticAnimatedReplacement;
			if (countAsStructuralRebuild)
			{
				mRuntimeMutation.NoteRebuiltChunk();
				mLastPerfShellTraceStats.runtimeMutationRebuiltChunks++;
			}
			return true;
		};
		const auto refreshReplacementMaterialsFromPreparedLiveChunk =
			[&](bool allowAnimatedMaterialState) -> bool
		{
			if (tryBuildCertifiedMaterialOnlyReplacement(false, allowAnimatedMaterialState))
			{
				return true;
			}
			const bool exclusiveMaterialOnlyReplacement =
				materialOnlyReplacement &&
				RequiresExclusiveRuntimeMutationMaterialOnlyReasonMask(normalizedReasonMask);
			const bool excludeStaticChunk =
				useStaticAnimatedReplacement ||
				!materialOnlyReplacement ||
				exclusiveMaterialOnlyReplacement;
			const nri_scene::SceneView* residentComparableView = getResidentComparableLiveChunkView();
			const nri_scene::SceneView& animatedSignatureSceneView =
				residentComparableView != nullptr ? *residentComparableView : liveChunkView;
			const uint64_t liveAnimatedGeometrySignature =
				ComputeAnimatedGeometrySignature(animatedSignatureSceneView);
			const uint64_t liveAnimatedMaterialSignature =
				ComputeAnimatedMaterialSignature(animatedSignatureSceneView);
			const uint32_t liveSurfaceCount = CountSceneViewSurfaces(liveChunkView);
			nri_scene::GeometryData liveGeometry;
			if (exclusiveMaterialOnlyReplacement)
			{
				Clocker clock(NriPTGeometryBuild);
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.geometryBuildRuntimeMutationMaterialOnlyMs);
				nri_scene::BuildGeometry(liveChunkView, liveGeometry);
				AssignGeometryPortalIndices(mMapWorld, liveGeometry);
				mLastPerfShellTraceStats.geometryBuildRuntimeMutationMaterialOnlyCalls++;
				mLastPerfShellTraceStats.geometryBuildRuntimeMutationPrimitives += (uint32_t)liveGeometry.primitives.size();
			}
			nri_scene::MaterialBridgeData liveMaterials;
			if (tryGetCachedRuntimeMutationMaterials(
				liveAnimatedGeometrySignature,
				liveAnimatedMaterialSignature,
				liveSurfaceCount,
				nullptr,
				liveMaterials))
			{
				mLastPerfShellTraceStats.runtimeMutationMaterialCacheHitCount++;
			}
			else
			{
				mLastPerfShellTraceStats.runtimeMutationMaterialCacheMissCount++;
				{
					Clocker clock(NriPTMaterialBuild);
					BuildMaterialsWithActorOverrides(liveChunkView, liveMaterials, "runtime_mutation_chunk");
				}
				storeCachedRuntimeMutationMaterials(
					liveAnimatedGeometrySignature,
					liveAnimatedMaterialSignature,
					liveSurfaceCount,
					nullptr,
					liveMaterials);
				mLastPerfShellTraceStats.runtimeMutationMaterialCacheStoreCount++;
			}
			tryMergeSectorMaterialOnlyPreparedMaterials(liveChunkView, liveMaterials);
			recordPreparedLiveChunkMaterialOnlyMismatch(liveChunkView, liveMaterials, true);
			nri_scene::PTMapChunkMutationBaseline liveBaseline;
			if (!nri_scene::CaptureMapChunkMutationBaseline(mapChunk, liveBaseline))
			{
				return false;
			}

			if (exclusiveMaterialOnlyReplacement)
			{
				mRuntimeMutation.BuildLightIdentityOverrides(
					mMapWorld,
					mapChunk,
					liveWorld,
					liveWorld.chunks[0],
					replacement.lightIdentityOverrides);
				replacement.geometry = std::move(liveGeometry);
				replacement.triangleCount = (uint32_t)replacement.geometry.primitives.size();
			}
			replacement.sceneView = liveChunkView;
			replacement.exactGeometrySignature = 0;
			replacement.materialBridge = std::move(liveMaterials);
			replacement.replacementBaseline = std::move(liveBaseline);
			replacement.surfaceCount = liveSurfaceCount;
			replacement.animatedMaterialSignature = liveAnimatedMaterialSignature;
			replacement.valid = true;
			replacement.active = true;
			replacement.excludeStaticChunk = excludeStaticChunk;
			replacement.staticAnimatedReplacement = useStaticAnimatedReplacement;
			return true;
		};
		const bool exclusiveMaterialOnlyReplacement =
			materialOnlyReplacement &&
			RequiresExclusiveRuntimeMutationMaterialOnlyReasonMask(normalizedReasonMask);
		const bool desiredExcludeStaticChunk =
			useStaticAnimatedReplacement ||
			!materialOnlyReplacement ||
			exclusiveMaterialOnlyReplacement;
		const bool structuralInvalid = !replacement.valid && !replacement.residentAuthoritative;
		const bool structuralReplacementDelta =
			!analyzedReplacementDelta ||
			(replacementDelta.reasonMask & structuralReasonMask) != 0;
		const bool structuralStaticAnimatedModeFlip =
			replacement.staticAnimatedReplacement != useStaticAnimatedReplacement;
		const bool structuralExcludeStaticFlip =
			useOverlayReplacementState &&
			replacement.excludeStaticChunk != desiredExcludeStaticChunk;
		// A settle request changes only the temporal half of resident vertices:
		// authored positions and exact-geometry signatures are intentionally
		// unchanged. Force that one refresh through the structural upload lane so
		// the material/animation sync-skip paths cannot retain stale prevPosition
		// data indefinitely after a mover stops.
		bool effectiveForceTopologyInvalidation =
			forceTopologyInvalidation || motionSettleRequested;
		bool effectiveStructuralReplacementDelta = structuralReplacementDelta;
		if (forceTopologyInvalidation)
		{
			mLastPerfShellTraceStats.runtimeMutationForceTopologyProofChecks++;
			const uint32_t dirtyOnlyStructuralReasonMask =
				nri_scene::PTMapChunkMutationReason_SectorDirty |
				nri_scene::PTMapChunkMutationReason_SectionDirty;
			const uint32_t replacementStructuralReasonMask =
				analyzedReplacementDelta ? (replacementDelta.reasonMask & structuralReasonMask) : structuralReasonMask;
			const bool onlyDirtyStructuralReason =
				(replacementStructuralReasonMask & ~dirtyOnlyStructuralReasonMask) == 0;
			if (!chunkVisibleNow)
			{
				const bool exactSignatureCached = replacement.exactGeometrySignature != 0;
				const bool residentAvailable =
					residentEntry != nullptr &&
					residentEntry->valid &&
					residentEntry->active &&
					residentEntry->mappedInStaticScene;
				bool exactSignatureMatch = false;
				bool animatedMaterialMatch = false;
				if (replacement.valid && residentAvailable)
				{
					if (replacement.exactGeometrySignature == 0)
					{
						replacement.exactGeometrySignature = ComputeExactGeometrySignature(replacement.sceneView);
					}
					exactSignatureMatch = replacement.exactGeometrySignature == residentEntry->exactGeometrySignature;
					animatedMaterialMatch = replacement.animatedMaterialSignature == residentEntry->animatedMaterialSignature;
				}
				const bool visibleFloor = isFlatPlaneMarkedVisible(mCurrentVisibleFlatPlaneWords, mapChunk.sectorIndex, false);
				const bool visibleCeiling = isFlatPlaneMarkedVisible(mCurrentVisibleFlatPlaneWords, mapChunk.sectorIndex, true);
				const bool hardwareCanvas =
					replacement.valid &&
					SceneViewUsesHardwareCanvasTexture(replacement.sceneView);
				const bool portalChunk = mapChunkHasPortalSurface(mapChunk);
				const bool sectorLightingCandidate =
					(normalizedReasonMask &
						(nri_scene::PTMapChunkMutationReason_SectorDirty |
							nri_scene::PTMapChunkMutationReason_SectorMaterial)) != 0;
				const bool safeResidentNoopCandidate =
					replacement.residentAuthoritative &&
					replacement.valid &&
					residentAvailable &&
					exactSignatureMatch &&
					animatedMaterialMatch &&
					replacement.surfaceCount == residentEntry->materialCount &&
					replacement.triangleCount == residentEntry->primitiveCount &&
					(uint32_t)replacement.materialBridge.materials.size() == residentEntry->materialCount &&
					!replacement.staticAnimatedReplacement &&
					!hardwareCanvas &&
					!portalChunk &&
					!sectorLightingCandidate &&
					!visibleFloor &&
					!visibleCeiling &&
					!residentEntry->hasAnimatedTextureCandidates &&
					!residentEntry->animatedRefreshSuppressed;
				recordRuntimeInvisibleProofEntry(
					mapChunk,
					normalizedReasonMask,
					runtimeMutationCandidateSourceMask,
					replacement,
					residentEntry,
					visibleFloor,
					visibleCeiling,
					exactSignatureCached,
					exactSignatureMatch,
					animatedMaterialMatch,
					hardwareCanvas,
					portalChunk,
					sectorLightingCandidate,
					safeResidentNoopCandidate);
				mLastPerfShellTraceStats.runtimeMutationForceTopologyDowngradeUnsafeReasonCount++;
				mLastPerfShellTraceStats.runtimeMutationForceTopologyDowngradeInvisibleCount++;
			}
			else if (!replacement.valid)
			{
				mLastPerfShellTraceStats.runtimeMutationForceTopologyDowngradeNoReplacementCount++;
			}
			else if (!onlyDirtyStructuralReason)
			{
				mLastPerfShellTraceStats.runtimeMutationForceTopologyDowngradeUnsafeReasonCount++;
				mLastPerfShellTraceStats.runtimeMutationForceTopologyDowngradeReasonMismatchCount++;
			}
			else if (!prepareLiveChunkView())
			{
				mLastPerfShellTraceStats.runtimeMutationForceTopologyDowngradePrepareFailedCount++;
			}
			else
			{
				const uint64_t liveExactGeometrySignature = ComputeExactGeometrySignature(liveChunkView);
				if (replacement.exactGeometrySignature == 0)
				{
					replacement.exactGeometrySignature = ComputeExactGeometrySignature(replacement.sceneView);
				}
				const uint64_t previousExactGeometrySignature = replacement.exactGeometrySignature;
				if (liveExactGeometrySignature == previousExactGeometrySignature)
				{
					const uint64_t liveAnimatedMaterialSignature = ComputeAnimatedMaterialSignature(liveChunkView);
					effectiveForceTopologyInvalidation = false;
					effectiveStructuralReplacementDelta = false;
					mLastPerfShellTraceStats.runtimeMutationForceTopologyDowngradeCount++;
					if (liveAnimatedMaterialSignature == replacement.animatedMaterialSignature)
					{
						mLastPerfShellTraceStats.runtimeMutationForceTopologyDowngradeNoopCount++;
					}
					else
					{
						mLastPerfShellTraceStats.runtimeMutationForceTopologyDowngradeMaterialOnlyCount++;
					}
				}
				else
				{
					mLastPerfShellTraceStats.runtimeMutationForceTopologyDowngradeGeometryChangedCount++;
				}
			}
		}
		const bool needsStructuralRebuild =
			structuralInvalid ||
			effectiveForceTopologyInvalidation ||
			effectiveStructuralReplacementDelta ||
			replacementViewChanged ||
			structuralExcludeStaticFlip ||
			structuralStaticAnimatedModeFlip;
		const bool hadDeferredStructuralRebuild = replacement.deferredStructuralRebuild;
		if (forceTopologyInvalidation)
		{
			mLastPerfShellTraceStats.runtimeMutationInvalidForceTopologyCount++;
		}
		if (needsStructuralRebuild)
		{
			replacement.deferredMaterialRefresh = false;
			replacement.deferredMaterialFrame = 0;
		}
		const bool activeReplacementStructuralCandidate =
			(runtimeMutationCandidateSourceMask & RuntimeMutationWorklistCandidateSource_ActiveReplacement) != 0;
		const bool deferInvisibleStructuralRebuild =
			needsStructuralRebuild &&
			!useStaticAnimatedReplacement &&
			!chunkVisibleNow &&
			!activeReplacementStructuralCandidate &&
			((runtimeMutationDistanceTier == RuntimeMutationDistanceTier::Near && deferNearInvisibleStructuralRebuilds) ||
				(runtimeMutationDistanceTier == RuntimeMutationDistanceTier::Far && deferFarInvisibleStructuralRebuilds));
		if (deferInvisibleStructuralRebuild &&
			!reserveInvisibleStructuralBudget(chunkIndex, runtimeMutationDistanceTier))
		{
			recordDeferredStructuralTier(runtimeMutationDistanceTier, hadDeferredStructuralRebuild);
			if (!hadDeferredStructuralRebuild)
			{
				replacement.deferredStructuralFrame = mFrameIndex;
			}
			replacement.deferredStructuralRebuild = true;
			replacement.animationOnlyRefreshed = false;
			mRuntimeMutation.SeedSignatureWatchlist((uint32_t)chunkIndex);
			chunkTraceAction = RuntimeMutationTraceAction::DeferredStructuralRebuild;
			mRuntimeMutation.TraceChunk(mapChunk, replacement, ShouldEmitTemporalTraceLogs(), (int)nri_ptmutationtracechunk, (int)nri_ptmutationtracesector);
			emitChunkTrace();
			continue;
		}
		if (needsStructuralRebuild && hadDeferredStructuralRebuild)
		{
			recordFlushedDeferredStructuralTier(runtimeMutationDistanceTier);
			if (!deferInvisibleStructuralRebuild)
			{
				mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredPromotedChunks++;
			}
			replacement.deferredStructuralRebuild = false;
			replacement.deferredStructuralFrame = 0;
		}

		if (needsStructuralRebuild)
		{
			const bool structuralMaterialOnly = materialOnlyReplacement;
			const bool structuralSectorMaterialOnly =
				normalizedReasonMask == nri_scene::PTMapChunkMutationReason_SectorMaterial;
			const bool structuralWallMaterialOnly =
				normalizedReasonMask == nri_scene::PTMapChunkMutationReason_WallMaterial;
			const bool structuralMixedMaterialOnly =
				structuralMaterialOnly &&
				!structuralSectorMaterialOnly &&
				!structuralWallMaterialOnly;
			const bool structuralGeometryOrDirty =
				(normalizedReasonMask & structuralReasonMask) != 0;
			uint32_t previousGeometryDirtyWallCount = 0;
			uint32_t previousGeometryDirtyFlatCount = 0;
			uint32_t previousGeometryDirtyTriangleCount = 0;
			uint32_t previousGeometryDirtyMaterialCount = 0;
			if (structuralGeometryOrDirty)
			{
				if (replacement.valid)
				{
					previousGeometryDirtyWallCount = (uint32_t)replacement.sceneView.opaqueWalls.size();
					previousGeometryDirtyFlatCount = (uint32_t)replacement.sceneView.opaqueFlats.size();
					previousGeometryDirtyTriangleCount = replacement.triangleCount;
					previousGeometryDirtyMaterialCount = (uint32_t)replacement.materialBridge.materials.size();
				}
				else
				{
					uint32_t residentChunkListIndex = UINT32_MAX;
					const StaticMapSceneCache::ChunkCache* residentChunkCache = nullptr;
					const nri_scene::SceneView* residentChunkView = nullptr;
					if (tryResolveResidentChunkState(residentChunkListIndex, residentChunkCache, residentChunkView) &&
						residentChunkCache != nullptr &&
						residentChunkView != nullptr)
					{
						previousGeometryDirtyWallCount = (uint32_t)residentChunkView->opaqueWalls.size();
						previousGeometryDirtyFlatCount = (uint32_t)residentChunkView->opaqueFlats.size();
						previousGeometryDirtyTriangleCount = residentChunkCache->primitiveCount;
						previousGeometryDirtyMaterialCount = residentChunkCache->materialCount;
					}
				}
			}
			uint32_t structuralTriggerMask = 0;
			if (structuralReplacementDelta)
			{
				structuralTriggerMask |= RuntimeStructuralRebuildTrigger_ReplacementDelta;
			}
			if (replacementViewChanged)
			{
				structuralTriggerMask |= RuntimeStructuralRebuildTrigger_ViewChanged;
			}
			if (structuralStaticAnimatedModeFlip)
			{
				structuralTriggerMask |= RuntimeStructuralRebuildTrigger_StaticAnimatedFlip;
			}
			if (structuralExcludeStaticFlip)
			{
				structuralTriggerMask |= RuntimeStructuralRebuildTrigger_ExcludeStaticFlip;
			}
			if (effectiveForceTopologyInvalidation)
			{
				structuralTriggerMask |= RuntimeStructuralRebuildTrigger_ForceTopology;
			}
			if (structuralInvalid)
			{
				structuralTriggerMask |= RuntimeStructuralRebuildTrigger_Invalid;
			}
			mLastPerfShellTraceStats.runtimeMutationStructuralRebuildChunks++;
			recordRuntimeMutationStructuralTier(chunkVisibleNow, runtimeMutationDistanceTier, runtimeMutationCandidateSourceMask);
			if (effectiveStructuralReplacementDelta)
			{
				mLastPerfShellTraceStats.runtimeMutationStructuralReplacementDeltaChunks++;
				if (analyzedReplacementDelta)
				{
					mLastPerfShellTraceStats.runtimeMutationStructuralReplacementDeltaReasonMaskOr |= replacementDelta.reasonMask;
				}
			}
			if (replacementViewChanged)
			{
				mLastPerfShellTraceStats.runtimeMutationStructuralReplacementViewChangedChunks++;
			}
			if (structuralStaticAnimatedModeFlip)
			{
				mLastPerfShellTraceStats.runtimeMutationStructuralStaticAnimatedModeFlipChunks++;
			}
			if (structuralExcludeStaticFlip)
			{
				mLastPerfShellTraceStats.runtimeMutationStructuralExcludeStaticFlipChunks++;
			}
			if (effectiveForceTopologyInvalidation)
			{
				mLastPerfShellTraceStats.runtimeMutationStructuralForcedTopologyChunks++;
			}
			if (structuralInvalid)
			{
				mLastPerfShellTraceStats.runtimeMutationStructuralInvalidChunks++;
			}
			if (structuralMaterialOnly)
			{
				mLastPerfShellTraceStats.runtimeMutationStructuralMaterialOnlyChunks++;
			}
			if (structuralSectorMaterialOnly)
			{
				mLastPerfShellTraceStats.runtimeMutationStructuralSectorMaterialOnlyChunks++;
			}
			if (structuralWallMaterialOnly)
			{
				mLastPerfShellTraceStats.runtimeMutationStructuralWallMaterialOnlyChunks++;
			}
			if (structuralMixedMaterialOnly)
			{
				mLastPerfShellTraceStats.runtimeMutationStructuralMixedMaterialOnlyChunks++;
			}
			if (structuralGeometryOrDirty)
			{
				mLastPerfShellTraceStats.runtimeMutationStructuralGeometryOrDirtyChunks++;
			}
			const bool builtChunk = [&]()
			{
				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationRebuildMs);
				ScopedPtPerfTimer structuralPerfTimer(mLastPerfShellTraceStats.runtimeMutationStructuralRebuildMs);
				ScopedPtPerfTimer structuralTierPerfTimer(
					chunkVisibleNow ?
						mLastPerfShellTraceStats.runtimeMutationStructuralRebuildVisibleMs :
						mLastPerfShellTraceStats.runtimeMutationStructuralRebuildInvisibleMs);
				double& structuralDistanceTierMs =
					chunkVisibleNow ?
						ignoredRuntimeMutationDistanceTierMs :
						(runtimeMutationDistanceTier == RuntimeMutationDistanceTier::Near ?
							mLastPerfShellTraceStats.runtimeMutationStructuralRebuildNearMs :
							(runtimeMutationDistanceTier == RuntimeMutationDistanceTier::Far ?
								mLastPerfShellTraceStats.runtimeMutationStructuralRebuildFarMs :
								mLastPerfShellTraceStats.runtimeMutationStructuralRebuildUnknownDistanceMs));
				ScopedPtPerfTimer structuralDistanceTierPerfTimer(structuralDistanceTierMs);
				if (!prepareLiveChunkView())
				{
					return false;
				}
				captureSectorDirtyTruth();
				return rebuildReplacementFromPreparedLiveChunk(true);
			}();
			if (!builtChunk && replacement.valid)
			{
				replacement.active = true;
				mRuntimeMutation.NoteHeldChunk();
				mLastPerfShellTraceStats.runtimeMutationHeldChunks++;
				chunkTraceAction = RuntimeMutationTraceAction::Held;
				recordStructuralRebuildEntry(
					mapChunk.chunkIndex,
					mapChunk.sectorIndex,
					normalizedReasonMask,
					structuralTriggerMask,
					replacement.surfaceCount,
					replacement.triangleCount,
					(uint32_t)replacement.materialBridge.materials.size(),
					chunkTraceAction,
					structuralMaterialOnly,
					structuralSectorMaterialOnly,
					structuralWallMaterialOnly,
					structuralMixedMaterialOnly,
					structuralGeometryOrDirty);
			}
			else if (!builtChunk)
			{
				replacement.active = false;
				replacement.excludeStaticChunk = false;
				replacement.staticAnimatedReplacement = false;
				replacement.animationOnlyRefreshed = false;
				replacement.animatedMaterialSignature = 0;
				replacement.stableMutationFrameCount = 0;
				mLastPerfShellTraceStats.runtimeMutationInvalidFailedCount++;
				chunkTraceAction = RuntimeMutationTraceAction::Failed;
				recordStructuralRebuildEntry(
					mapChunk.chunkIndex,
					mapChunk.sectorIndex,
					normalizedReasonMask,
					structuralTriggerMask,
					0,
					0,
					0,
					chunkTraceAction,
					structuralMaterialOnly,
					structuralSectorMaterialOnly,
					structuralWallMaterialOnly,
					structuralMixedMaterialOnly,
					structuralGeometryOrDirty);
				mRuntimeMutation.TraceChunk(mapChunk, replacement, ShouldEmitTemporalTraceLogs(), (int)nri_ptmutationtracechunk, (int)nri_ptmutationtracesector);
				emitChunkTrace();
				mRuntimeMutation.ClearReplacementPayload(replacement, replacement.residentAuthoritative);
				continue;
			}
			else
			{
				mLastPerfShellTraceStats.runtimeMutationValidStructuralCount++;
				chunkTraceAction = RuntimeMutationTraceAction::StructuralRebuild;
				chunkTraceSurfaceCount = replacement.surfaceCount;
				chunkTraceTriangleCount = replacement.triangleCount;
				chunkTraceMaterialCount = (uint32_t)replacement.materialBridge.materials.size();
				recordStructuralRebuildEntry(
					mapChunk.chunkIndex,
					mapChunk.sectorIndex,
					normalizedReasonMask,
					structuralTriggerMask,
					replacement.surfaceCount,
					replacement.triangleCount,
					(uint32_t)replacement.materialBridge.materials.size(),
					chunkTraceAction,
					structuralMaterialOnly,
					structuralSectorMaterialOnly,
					structuralWallMaterialOnly,
					structuralMixedMaterialOnly,
					structuralGeometryOrDirty);
				if (structuralGeometryOrDirty)
				{
					recordGeometryDirtyEntry(
						mapChunk.chunkIndex,
						mapChunk.sectorIndex,
						normalizedReasonMask,
						previousGeometryDirtyWallCount,
						(uint32_t)replacement.sceneView.opaqueWalls.size(),
						previousGeometryDirtyFlatCount,
						(uint32_t)replacement.sceneView.opaqueFlats.size(),
						previousGeometryDirtyTriangleCount,
						replacement.triangleCount,
						previousGeometryDirtyMaterialCount,
						(uint32_t)replacement.materialBridge.materials.size(),
						effectiveForceTopologyInvalidation);
				}
			}
		}
		else
		{
			if (hadDeferredStructuralRebuild)
			{
				replacement.deferredStructuralRebuild = false;
				replacement.deferredStructuralFrame = 0;
			}
			const bool activeReplacementMaterialCandidate =
				(runtimeMutationCandidateSourceMask & RuntimeMutationWorklistCandidateSource_ActiveReplacement) != 0;
			const bool deferNearInvisibleMaterialRefresh =
				deferNearInvisibleMaterialRefreshes &&
				materialOnlyReplacement &&
				!useStaticAnimatedReplacement &&
				!chunkVisibleNow &&
				!activeReplacementMaterialCandidate &&
				runtimeMutationDistanceTier == RuntimeMutationDistanceTier::Near;
			const bool deferFarInvisibleMaterialRefresh =
				runtimeMutationSettings.deferFarMaterialRefreshes &&
				materialOnlyReplacement &&
				!useStaticAnimatedReplacement &&
				!chunkVisibleNow &&
				runtimeMutationDistanceTier == RuntimeMutationDistanceTier::Far;
			const bool deferInvisibleMaterialRefresh =
				deferFarInvisibleMaterialRefresh ||
				(deferNearInvisibleMaterialRefresh && !reserveNearInvisibleMaterialBudget(chunkIndex));
			if (deferInvisibleMaterialRefresh)
			{
				const bool hadDeferredMaterialRefresh = replacement.deferredMaterialRefresh;
				recordDeferredMaterialTier(runtimeMutationDistanceTier, hadDeferredMaterialRefresh);
				if (!hadDeferredMaterialRefresh)
				{
					replacement.deferredMaterialFrame = mFrameIndex;
				}
				replacement.deferredMaterialRefresh = true;
				replacement.animationOnlyRefreshed = false;
				mRuntimeMutation.SeedSignatureWatchlist((uint32_t)chunkIndex);
				chunkTraceAction = RuntimeMutationTraceAction::DeferredMaterialRefresh;
				mRuntimeMutation.TraceChunk(mapChunk, replacement, ShouldEmitTemporalTraceLogs(), (int)nri_ptmutationtracechunk, (int)nri_ptmutationtracesector);
				emitChunkTrace();
				continue;
			}
			replacement.active = true;
			const bool hadDeferredMaterialRefresh = replacement.deferredMaterialRefresh;
			bool activeHardwareCanvasChunk = false;
			bool suppressedAnimatedResidentAlreadySynchronized = false;
			bool unsuppressedAnimatedResidentAlreadySynchronized = false;
			// Build tile animation can change the resolved PT texture binding
			// without mutating the authored wall/sector fields tracked above.
			const bool refreshedAnimatedChunk = [&]()
			{
				const bool forceReplacementMaterialRefresh =
					useOverlayReplacementState &&
					analyzedReplacementDelta &&
					replacementRefreshReasonMask != nri_scene::PTMapChunkMutationReason_None;
				if (!prepareLiveChunkView())
				{
					return false;
				}

				activeHardwareCanvasChunk = SceneViewUsesHardwareCanvasTexture(liveChunkView);
				const bool forceHardwareCanvasRefresh =
					activeHardwareCanvasChunk &&
					IsChunkMarkedVisible(mCurrentVisibleChunkWords, mapChunk.chunkIndex);
				const nri_scene::SceneView* residentComparableView = getResidentComparableLiveChunkView();
				const nri_scene::SceneView& animatedSignatureSceneView =
					residentComparableView != nullptr ? *residentComparableView : liveChunkView;
				const uint64_t liveAnimatedGeometrySignature =
					ComputeAnimatedGeometrySignature(animatedSignatureSceneView);
				const uint64_t liveAnimatedMaterialSignature =
					ComputeAnimatedMaterialSignature(animatedSignatureSceneView);
				captureSectorDirtyTruth();
				const bool residentAnimatedAlreadySynchronized =
					!forceReplacementMaterialRefresh &&
					!forceHardwareCanvasRefresh &&
					residentEntry != nullptr &&
					residentEntry->valid &&
					residentEntry->animatedGeometrySignature == liveAnimatedGeometrySignature &&
					residentEntry->animatedMaterialSignature == liveAnimatedMaterialSignature;
				suppressedAnimatedResidentAlreadySynchronized =
					useStaticAnimatedReplacement &&
					residentAnimatedAlreadySynchronized &&
					residentEntry->animatedRefreshSuppressed;
				unsuppressedAnimatedResidentAlreadySynchronized =
					!useStaticAnimatedReplacement &&
					residentAnimatedAlreadySynchronized;
				if (useStaticAnimatedReplacement &&
					residentEntry != nullptr &&
					residentEntry->valid &&
					residentEntry->animatedRefreshSuppressed)
				{
					residentEntry->runtimeAnimatedAttemptCount++;
					recordRuntimeAnimatedFrame(mapChunk.chunkIndex, true, false, true, false, false);
				}
				else if (unsuppressedAnimatedResidentAlreadySynchronized &&
					residentEntry != nullptr &&
					residentEntry->valid)
				{
					residentEntry->runtimeAnimatedAttemptCount++;
					recordRuntimeAnimatedFrame(mapChunk.chunkIndex, false, false, true, false, false);
				}
				if (suppressedAnimatedResidentAlreadySynchronized ||
					unsuppressedAnimatedResidentAlreadySynchronized)
				{
					return true;
				}
				if (!forceReplacementMaterialRefresh &&
					!forceHardwareCanvasRefresh &&
					liveAnimatedMaterialSignature == replacement.animatedMaterialSignature)
				{
					return true;
				}

				ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.runtimeMutationRebuildMs);
				ScopedPtPerfTimer materialRefreshPerfTimer(mLastPerfShellTraceStats.runtimeMutationMaterialRefreshMs);
				ScopedPtPerfTimer materialRefreshTierPerfTimer(
					chunkVisibleNow ?
						mLastPerfShellTraceStats.runtimeMutationMaterialRefreshVisibleMs :
						mLastPerfShellTraceStats.runtimeMutationMaterialRefreshInvisibleMs);
				double& materialRefreshDistanceTierMs =
					chunkVisibleNow ?
						ignoredRuntimeMutationDistanceTierMs :
						(runtimeMutationDistanceTier == RuntimeMutationDistanceTier::Near ?
							mLastPerfShellTraceStats.runtimeMutationMaterialRefreshNearMs :
							(runtimeMutationDistanceTier == RuntimeMutationDistanceTier::Far ?
								mLastPerfShellTraceStats.runtimeMutationMaterialRefreshFarMs :
								mLastPerfShellTraceStats.runtimeMutationMaterialRefreshUnknownDistanceMs));
				ScopedPtPerfTimer materialRefreshDistanceTierPerfTimer(materialRefreshDistanceTierMs);
				if (!refreshReplacementMaterialsFromPreparedLiveChunk(
					!forceReplacementMaterialRefresh && !forceHardwareCanvasRefresh))
				{
					return false;
				}

				mLastPerfShellTraceStats.runtimeMutationMaterialRefreshChunks++;
				if (hadDeferredMaterialRefresh)
				{
					recordFlushedDeferredMaterialTier(runtimeMutationDistanceTier);
				}
				recordRuntimeMutationMaterialTier(chunkVisibleNow, runtimeMutationDistanceTier, runtimeMutationCandidateSourceMask);
				if (forceReplacementMaterialRefresh)
				{
					mLastPerfShellTraceStats.runtimeMutationMaterialRefreshReplacementDeltaChunks++;
					mLastPerfShellTraceStats.runtimeMutationMaterialRefreshReasonMaskOr |= replacementRefreshReasonMask;
				}
				if (forceHardwareCanvasRefresh)
				{
					mLastPerfShellTraceStats.runtimeMutationMaterialRefreshHardwareCanvasChunks++;
				}
				if (!forceReplacementMaterialRefresh && !forceHardwareCanvasRefresh)
				{
					mLastPerfShellTraceStats.runtimeMutationMaterialRefreshAnimatedChunks++;
					replacement.animationOnlyRefreshed = true;
					mRuntimeMutation.NoteAnimatedRefreshChunk();
				}
				recordRuntimeAnimatedFrame(
					mapChunk.chunkIndex,
					useStaticAnimatedReplacement,
					true,
					!useStaticAnimatedReplacement,
					false,
					false);
				return true;
			}();
			if (!refreshedAnimatedChunk)
			{
				replacement.active = true;
				mRuntimeMutation.NoteHeldChunk();
				mLastPerfShellTraceStats.runtimeMutationHeldChunks++;
				chunkTraceAction = RuntimeMutationTraceAction::Held;
			}
			else
			{
				replacement.deferredMaterialRefresh = false;
				replacement.deferredMaterialFrame = 0;
				if (suppressedAnimatedResidentAlreadySynchronized ||
					unsuppressedAnimatedResidentAlreadySynchronized)
				{
					replacement.active = false;
					replacement.excludeStaticChunk = false;
					replacement.staticAnimatedReplacement = false;
					replacement.animationOnlyRefreshed = false;
					replacement.stableMutationFrameCount = 0;
					mLastPerfShellTraceStats.runtimeMutationInvalidSyncSkipCount++;
					if (residentEntry != nullptr && residentEntry->valid)
					{
						residentEntry->runtimeAnimatedSyncSkipCount++;
						if (residentEntry->mappedInStaticScene &&
							hasCachedResidentAnimatedSlice(
								residentEntry->staticSceneChunkListIndex,
								residentEntry->animatedGeometrySignature,
								residentEntry->animatedMaterialSignature))
						{
							mLastPerfShellTraceStats.staticAnimatedResidentSliceSyncSkipHitCount++;
						}
					}
					recordRuntimeAnimatedFrame(
						mapChunk.chunkIndex,
						suppressedAnimatedResidentAlreadySynchronized,
						false,
						false,
						false,
						true);
					chunkTraceAction = RuntimeMutationTraceAction::SyncSkip;
					chunkTraceSurfaceCount = CountSceneViewSurfaces(liveChunkView);
					chunkTraceTriangleCount = liveStats.triangleCount;
					chunkTraceMaterialCount = (uint32_t)replacement.materialBridge.materials.size();
					mRuntimeMutation.TraceChunk(mapChunk, replacement, ShouldEmitTemporalTraceLogs(), (int)nri_ptmutationtracechunk, (int)nri_ptmutationtracesector);
					emitChunkTrace();
					continue;
				}
				else if (replacement.valid)
				{
					mLastPerfShellTraceStats.runtimeMutationValidMaterialCount++;
					chunkTraceAction = RuntimeMutationTraceAction::MaterialRefresh;
					chunkTraceSurfaceCount = replacement.surfaceCount;
					chunkTraceTriangleCount = replacement.triangleCount;
					chunkTraceMaterialCount = (uint32_t)replacement.materialBridge.materials.size();
				}
			}
		}

		if (replacement.active &&
			SceneViewUsesHardwareCanvasTexture(replacement.sceneView))
		{
			mLastPerfShellTraceStats.runtimeMutationHardwareCanvasChunkCount++;
		}

		if (replacement.active && replacement.valid)
		{
			uint32_t residentChunkListIndex = UINT32_MAX;
			bool residentChunkMaterialDirty = false;
			bool residentChunkGeometryDirty = false;
			uint32_t residentChunkSurfaceCount = 0;
			uint32_t residentChunkTriangleCount = 0;
			uint32_t residentChunkMaterialCount = 0;
			bool residentChunkRecoveredEmpty = false;
			const uint32_t appliedReasonMask = replacement.reasonMask;
			const bool attemptedStaticAnimatedReplacement = replacement.staticAnimatedReplacement;
			const bool attemptedAnimationOnlyRefresh = replacement.animationOnlyRefreshed;
			const bool attemptedSuppressedAnimatedResidentApply =
				attemptedStaticAnimatedReplacement &&
				residentEntry != nullptr &&
				residentEntry->valid &&
				residentEntry->animatedRefreshSuppressed;
			const auto settleResidentNoopReplacement = [&]()
			{
				const nri_scene::PTMapChunkMutationBaseline appliedBaseline = replacement.replacementBaseline;
				replacement.baseline = appliedBaseline;
				replacement.replacementBaseline = appliedBaseline;
				replacement.baselineSignature = appliedBaseline.signature;
				replacement.liveSignature = appliedBaseline.signature;
				replacement.reasonMask = nri_scene::PTMapChunkMutationReason_None;
				replacement.sectionDirtyCount = 0;
				replacement.stableMutationFrameCount = 0;
				replacement.sectorDirty = false;
				replacement.dragged = false;
				replacement.blindSpot = false;
				replacement.excludeStaticChunk = false;
				replacement.staticAnimatedReplacement = false;
				replacement.active = false;
				replacement.valid = false;
				replacement.residentAuthoritative = true;
				replacement.animationOnlyRefreshed = false;
				replacement.animatedMaterialSignature =
					residentEntry != nullptr ? residentEntry->animatedMaterialSignature : replacement.animatedMaterialSignature;
				replacement.surfaceCount = 0;
				replacement.triangleCount = 0;
				mRuntimeMutation.ClearReplacementPayload(replacement, true);
				if (residentEntry != nullptr)
				{
					residentEntry->appliedBaseline = appliedBaseline;
					residentEntry->baselineSignature = appliedBaseline.signature;
					residentEntry->liveSignature = appliedBaseline.signature;
					residentEntry->visibleValidationFramesRemaining = 0;
				}
			};
			const bool residentNoopResidentAvailable =
				residentEntry != nullptr &&
				residentEntry->valid &&
				residentEntry->active &&
				residentEntry->mappedInStaticScene;
			const bool residentNoopIgnoreExcludeStatic = effectiveForceTopologyInvalidation;
			if (effectiveForceTopologyInvalidation &&
				replacement.residentAuthoritative &&
				residentEntry != nullptr &&
				residentEntry->valid &&
				replacement.valid &&
				replacement.exactGeometrySignature == 0)
			{
				replacement.exactGeometrySignature = ComputeExactGeometrySignature(replacement.sceneView);
			}
			const bool residentNoopSignatureCandidate =
				effectiveForceTopologyInvalidation &&
				replacement.residentAuthoritative &&
				residentEntry != nullptr &&
				residentEntry->valid &&
				replacement.valid &&
				replacement.exactGeometrySignature == residentEntry->exactGeometrySignature &&
				replacement.animatedMaterialSignature == residentEntry->animatedMaterialSignature;
			const bool residentNoopExactMatch =
				!motionSettleRequested &&
				replacement.residentAuthoritative &&
				residentNoopResidentAvailable &&
				replacement.valid &&
				(!replacement.excludeStaticChunk || residentNoopIgnoreExcludeStatic) &&
				replacement.surfaceCount == residentEntry->materialCount &&
				replacement.triangleCount == residentEntry->primitiveCount &&
				(uint32_t)replacement.materialBridge.materials.size() == residentEntry->materialCount &&
				replacement.exactGeometrySignature == residentEntry->exactGeometrySignature &&
				replacement.animatedMaterialSignature == residentEntry->animatedMaterialSignature;
			if (residentNoopSignatureCandidate && !residentNoopExactMatch)
			{
				if (!replacement.residentAuthoritative)
				{
					mLastPerfShellTraceStats.runtimeMutationResidentNoopBlockNotAuthoritativeCount++;
				}
				if (!residentNoopResidentAvailable)
				{
					mLastPerfShellTraceStats.runtimeMutationResidentNoopBlockResidentUnavailableCount++;
				}
				if (!replacement.valid)
				{
					mLastPerfShellTraceStats.runtimeMutationResidentNoopBlockReplacementInvalidCount++;
				}
				if (replacement.excludeStaticChunk)
				{
					mLastPerfShellTraceStats.runtimeMutationResidentNoopBlockExcludeStaticCount++;
				}
				if (residentEntry != nullptr)
				{
					if (replacement.surfaceCount != residentEntry->materialCount)
					{
						mLastPerfShellTraceStats.runtimeMutationResidentNoopBlockSurfaceCountMismatch++;
					}
					if ((uint32_t)replacement.materialBridge.materials.size() != residentEntry->materialCount)
					{
						mLastPerfShellTraceStats.runtimeMutationResidentNoopBlockMaterialCountMismatch++;
					}
					if (replacement.triangleCount != residentEntry->primitiveCount)
					{
						mLastPerfShellTraceStats.runtimeMutationResidentNoopBlockPrimitiveCountMismatch++;
					}
				}
			}
			if (residentNoopExactMatch)
			{
				mLastPerfShellTraceStats.runtimeMutationResidentNoopSkipCount++;
				mLastPerfShellTraceStats.runtimeMutationInvalidSyncSkipCount++;
				if ((attemptedStaticAnimatedReplacement || attemptedAnimationOnlyRefresh) &&
					residentEntry != nullptr &&
					residentEntry->valid)
				{
					residentEntry->runtimeAnimatedSyncSkipCount++;
					if (residentEntry->mappedInStaticScene &&
						hasCachedResidentAnimatedSlice(
							residentEntry->staticSceneChunkListIndex,
							residentEntry->animatedGeometrySignature,
							residentEntry->animatedMaterialSignature))
					{
						mLastPerfShellTraceStats.staticAnimatedResidentSliceSyncSkipHitCount++;
					}
					recordRuntimeAnimatedFrame(
						mapChunk.chunkIndex,
						attemptedSuppressedAnimatedResidentApply,
						false,
						false,
						false,
						true);
				}
				settleResidentNoopReplacement();
				chunkTraceAction = RuntimeMutationTraceAction::ResidentNoopSkip;
				chunkTraceSurfaceCount = residentEntry != nullptr ? residentEntry->materialCount : 0;
				chunkTraceTriangleCount = residentEntry != nullptr ? residentEntry->primitiveCount : 0;
				chunkTraceMaterialCount = residentEntry != nullptr ? residentEntry->materialCount : 0;
				mRuntimeMutation.TraceChunk(mapChunk, replacement, ShouldEmitTemporalTraceLogs(), (int)nri_ptmutationtracechunk, (int)nri_ptmutationtracesector);
				emitChunkTrace();
				continue;
			}
			recordRuntimeMutationResidentApplyTier(chunkVisibleNow, runtimeMutationDistanceTier, runtimeMutationCandidateSourceMask);
			RuntimeMutationResidentApplyResult residentApplyResult = {};
			const bool appliedResidentChunk = [&]()
			{
				ScopedPtPerfTimer residentApplyTierPerfTimer(
					chunkVisibleNow ?
						mLastPerfShellTraceStats.runtimeMutationResidentApplyVisibleMs :
						mLastPerfShellTraceStats.runtimeMutationResidentApplyInvisibleMs);
				double& residentApplyDistanceTierMs =
					chunkVisibleNow ?
						ignoredRuntimeMutationDistanceTierMs :
						(runtimeMutationDistanceTier == RuntimeMutationDistanceTier::Near ?
							mLastPerfShellTraceStats.runtimeMutationResidentApplyNearMs :
							(runtimeMutationDistanceTier == RuntimeMutationDistanceTier::Far ?
								mLastPerfShellTraceStats.runtimeMutationResidentApplyFarMs :
								mLastPerfShellTraceStats.runtimeMutationResidentApplyUnknownDistanceMs));
				ScopedPtPerfTimer residentApplyDistanceTierPerfTimer(residentApplyDistanceTierMs);
				return mRuntimeMutation.TryApplyResidentChunk(
					NRIRuntimeMutationSystem::BuildResidentApplyServices(*this),
					mapChunk,
					replacement,
					residentApplyResult);
			}();
			residentChunkListIndex = residentApplyResult.staticSceneChunkListIndex;
			residentChunkMaterialDirty = residentApplyResult.materialDirty;
			residentChunkGeometryDirty = residentApplyResult.geometryDirty;
			residentChunkSurfaceCount = residentApplyResult.surfaceCount;
			residentChunkTriangleCount = residentApplyResult.triangleCount;
			residentChunkMaterialCount = residentApplyResult.materialCount;
			residentChunkRecoveredEmpty = residentApplyResult.recoveredEmpty;
			if (appliedResidentChunk)
			{
				mLastPerfShellTraceStats.runtimeMutationInvalidAppliedCount++;
				mLastPerfResourceTraceStats.residentChunkBatchChunkCount++;
				if (residentChunkMaterialDirty)
				{
					mLastPerfResourceTraceStats.residentChunkBatchMaterialDirtyCount++;
				}
				if (residentChunkGeometryDirty)
				{
					mLastPerfResourceTraceStats.residentChunkBatchGeometryDirtyCount++;
				}
				if (residentChunkRecoveredEmpty)
				{
					mLastPerfResourceTraceStats.residentChunkBatchRecoverEmptyCount++;
				}
				if ((attemptedStaticAnimatedReplacement || attemptedAnimationOnlyRefresh) &&
					residentEntry != nullptr &&
					residentEntry->valid)
				{
					residentEntry->runtimeAnimatedResidentApplyCount++;
				}
				if (attemptedStaticAnimatedReplacement || attemptedAnimationOnlyRefresh)
				{
					recordRuntimeAnimatedFrame(
						mapChunk.chunkIndex,
						attemptedSuppressedAnimatedResidentApply,
						false,
						false,
						true,
						false);
				}
				if (residentEntry != nullptr)
				{
					residentEntry->visibleValidationFramesRemaining = 0;
				}
				residentStaticSceneChanged = true;
				mRuntimeMutation.NoteResidentApply(residentChunkMaterialDirty, residentChunkGeometryDirty);
				if (residentChunkMaterialDirty)
				{
					residentMaterialDirty = true;
					if (residentChunkListIndex != UINT32_MAX)
					{
						residentMaterialChunkListIndices.push_back(residentChunkListIndex);
						if (attemptedStaticAnimatedReplacement || attemptedAnimationOnlyRefresh)
						{
							animatedResidentApplyMaterialChunkListIndices.push_back(residentChunkListIndex);
						}
					}
				}
				if (residentChunkGeometryDirty)
				{
					residentGeometryDirty = true;
					if (residentChunkListIndex != UINT32_MAX)
					{
						residentGeometryChunkListIndices.push_back(residentChunkListIndex);
					}
				}

				chunkTraceAction = RuntimeMutationTraceAction::ResidentApply;
				chunkTraceResidentMaterialDirty = residentChunkMaterialDirty;
				chunkTraceResidentGeometryDirty = residentChunkGeometryDirty;
				chunkTraceRecoveredEmpty = residentChunkRecoveredEmpty;
				chunkTraceSurfaceCount = residentChunkSurfaceCount;
				chunkTraceTriangleCount = residentChunkTriangleCount;
				chunkTraceMaterialCount = residentChunkMaterialCount;
				mRuntimeMutation.TraceChunk(mapChunk, replacement, ShouldEmitTemporalTraceLogs(), (int)nri_ptmutationtracechunk, (int)nri_ptmutationtracesector);
				emitChunkTrace();
				continue;
			}

			if (appliedReasonMask != nri_scene::PTMapChunkMutationReason_None)
			{
				mRuntimeMutation.NoteResidentFallback();
			}
			mSE29FloorDeformerRoute.NoteApplyFailure(replacement.fixedLayoutDeformerKey);
			replacement.active = false;
			replacement.excludeStaticChunk = false;
			replacement.stableMutationFrameCount = 0;
			mLastPerfShellTraceStats.runtimeMutationInvalidFailedCount++;
			chunkTraceAction = RuntimeMutationTraceAction::ResidentFallback;
			chunkTraceSurfaceCount = replacement.surfaceCount;
			chunkTraceTriangleCount = replacement.triangleCount;
			chunkTraceMaterialCount = (uint32_t)replacement.materialBridge.materials.size();
			mRuntimeMutation.TraceChunk(mapChunk, replacement, ShouldEmitTemporalTraceLogs(), (int)nri_ptmutationtracechunk, (int)nri_ptmutationtracesector);
			emitChunkTrace();
			continue;
		}
	}

	if (worklistValidation.enabled)
	{
		uint32_t falsePositiveTraceCount = 0;
		for (uint32_t chunkListIndex = 0; chunkListIndex < (uint32_t)worklistValidation.sourceMasks.size(); ++chunkListIndex)
		{
			if (worklistValidation.sourceMasks[chunkListIndex] == 0 ||
				worklistValidation.fullDirty[chunkListIndex] != 0)
			{
				continue;
			}

			worklistValidation.falsePositiveCount++;
			if (worklistValidation.verbosity >= 2 && falsePositiveTraceCount < 8u)
			{
				const auto& mapChunk = mMapWorld.chunks[chunkListIndex];
				Printf(
					"PERF pt mutation worklist extra NRI: frame=%llu chunk=%u sector=%d source_mask=0x%x sources=%s\n",
					(unsigned long long)mFrameIndex,
					mapChunk.chunkIndex,
					mapChunk.sectorIndex,
					worklistValidation.sourceMasks[chunkListIndex],
					GetRuntimeMutationWorklistCandidateSourceSummary(worklistValidation.sourceMasks[chunkListIndex]).c_str());
				falsePositiveTraceCount++;
			}
		}
		if (worklistValidation.falseNegativeCount > 0 || worklistValidation.verbosity >= 2)
		{
			Printf(
				"PERF pt mutation worklist validate NRI: frame=%llu candidates=%u full_dirty=%u false_negatives=%u false_positives=%u chunks=%u signature_watch_candidates=%u signature_watch_seeds=%u signature_watch_total=%u background_sweep_candidates=%u verbosity=%d\n",
				(unsigned long long)mFrameIndex,
				worklistValidation.candidateCount,
				worklistValidation.fullDirtyCount,
				worklistValidation.falseNegativeCount,
				worklistValidation.falsePositiveCount,
				(uint32_t)mMapWorld.chunks.size(),
				worklistValidation.signatureWatchlistCandidateCount,
				worklistValidation.signatureWatchlistSeedCount,
				mRuntimeMutation.GetSignatureWatchlistSeedCount(),
				worklistValidation.backgroundSweepCandidateCount,
				worklistValidation.verbosity);
			for (uint32_t reasonMask = 0; reasonMask < (uint32_t)worklistValidation.falseNegativeReasonMaskCounts.size(); ++reasonMask)
			{
				const uint32_t count = worklistValidation.falseNegativeReasonMaskCounts[reasonMask];
				if (count == 0)
				{
					continue;
				}

				Printf(
					"PERF pt mutation worklist reason NRI: frame=%llu reason_mask=0x%x reasons=%s false_negatives=%u\n",
					(unsigned long long)mFrameIndex,
					reasonMask,
					GetRuntimeMapMutationReasonSummary(reasonMask).c_str(),
					count);
			}
		}
	}
	for (uint32_t chunkListIndex = 0; chunkListIndex < mRuntimeMutation.GetCacheChunkCount(); ++chunkListIndex)
	{
		const auto* replacement = mRuntimeMutation.FindReplacement(chunkListIndex);
		if (replacement == nullptr)
		{
			continue;
		}
		if (replacement->deferredMaterialRefresh)
		{
			mLastPerfShellTraceStats.runtimeMutationMaterialRefreshDeferredPendingChunks++;
		}
		if (replacement->deferredStructuralRebuild)
		{
			mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredPendingChunks++;
		}
	}
	for (uint32_t chunkListIndex = 0; chunkListIndex < mRuntimeMutation.GetCacheChunkCount(); ++chunkListIndex)
	{
		const auto* replacement = mRuntimeMutation.FindReplacement(chunkListIndex);
		if (replacement == nullptr)
		{
			continue;
		}
		const bool deferredMaterialRefresh = replacement->deferredMaterialRefresh;
		const bool deferredStructuralRebuild = replacement->deferredStructuralRebuild;
		if ((!deferredMaterialRefresh && !deferredStructuralRebuild) ||
			chunkListIndex >= mMapWorld.chunks.size())
		{
			continue;
		}

		const auto& mapChunk = mMapWorld.chunks[chunkListIndex];
		const bool chunkVisibleNow = IsChunkMarkedVisible(mCurrentVisibleChunkWords, mapChunk.chunkIndex);
		if (chunkVisibleNow)
		{
			continue;
		}

		switch (getRuntimeMutationDistanceTier(chunkListIndex, false))
		{
		case RuntimeMutationDistanceTier::Near:
			if (deferredMaterialRefresh)
			{
				mLastPerfShellTraceStats.runtimeMutationMaterialRefreshDeferredNearPendingChunks++;
			}
			if (deferredStructuralRebuild)
			{
				mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredNearPendingChunks++;
			}
			break;
		case RuntimeMutationDistanceTier::Far:
			if (deferredStructuralRebuild)
			{
				mLastPerfShellTraceStats.runtimeMutationStructuralRebuildDeferredFarPendingChunks++;
			}
			break;
		default:
			break;
		}
	}

	if (tracePtPerf)
	{
		for (const auto& tracker : mRuntimeRecurringChunkTrackers)
		{
			if (!tracker.valid)
			{
				continue;
			}

			mLastPerfShellTraceStats.runtimeRecurringChunkTrackedCount++;
			mLastPerfShellTraceStats.runtimeRecurringChunkVisitCount += tracker.visitCount;
			mLastPerfShellTraceStats.runtimeRecurringChunkUniqueStateCount += tracker.uniqueStateCount;
			mLastPerfShellTraceStats.runtimeRecurringChunkTransitionCount += tracker.transitionCount;
			mLastPerfShellTraceStats.runtimeRecurringChunkRepeatedStateHitCount += tracker.repeatedStateHitCount;
			mLastPerfShellTraceStats.runtimeRecurringChunkAbaRecurrenceCount += tracker.abaRecurrenceCount;
			mLastPerfShellTraceStats.runtimeRecurringChunkMaxUniqueStateCount =
				std::max(mLastPerfShellTraceStats.runtimeRecurringChunkMaxUniqueStateCount, tracker.uniqueStateCount);
			if (tracker.uniqueStateCount > 1 || tracker.repeatedStateHitCount > 0 || tracker.abaRecurrenceCount > 0)
			{
				mLastPerfShellTraceStats.runtimeRecurringChunkRecurringCount++;
			}

			RuntimeRecurringChunkTraceEntry entry = {};
			entry.valid = true;
			entry.chunkIndex = tracker.chunkIndex;
			entry.sectorIndex = tracker.sectorIndex;
			entry.lastReasonMask = tracker.lastReasonMask;
			entry.visitCount = tracker.visitCount;
			entry.uniqueStateCount = tracker.uniqueStateCount;
			entry.transitionCount = tracker.transitionCount;
			entry.repeatedStateHitCount = tracker.repeatedStateHitCount;
			entry.abaRecurrenceCount = tracker.abaRecurrenceCount;
			entry.lastWallCount = tracker.lastWallCount;
			entry.lastFlatCount = tracker.lastFlatCount;
			entry.lastTriangleCount = tracker.lastTriangleCount;
			entry.lastMaterialCount = tracker.lastMaterialCount;
			entry.previousStateSignature = tracker.previousStateSignature;
			entry.lastStateSignature = tracker.lastStateSignature;
			InsertRankedTraceEntry(
				mLastPerfShellTraceStats.runtimeRecurringChunkEntries,
				entry,
				ScoreRuntimeRecurringChunkTraceEntry);
		}
	}

	mRuntimeMutation.FinalizeFrameActive();
	if (traceStartupMutationPass)
	{
		mStartupMutationProbe.valid = true;
		mStartupMutationProbe.detectedMaterialOnly = startupMaterialOnlyMutationDetected;
		mStartupMutationProbe.frameIndex = mFrameIndex;
		mStartupMutationProbe.dirtyChunkCount = mRuntimeMutation.GetDirtyChunkCount();
		mStartupMutationProbe.startupMaterialOnlyDirtyChunkCount = mRuntimeMutation.GetStartupMaterialOnlyDirtyChunkCount();
		TraceStartupMutationProbe("mutation-pass-finalize");
	}
	if (startupMaterialOnlyMutationDetected && !mPendingStartupMutationRebaseline)
	{
		mPendingStartupMutationRebaseline = true;
		Printf("NRI PT startup mutation rebaseline queued: level=%s frame=%u dirty_material_chunks=%u replaced_chunks=%u\n",
			currentLevel != nullptr ? currentLevel->labelName.GetChars() : "(none)",
			mFrameIndex,
			mRuntimeMutation.GetStartupMaterialOnlyDirtyChunkCount(),
			mRuntimeMutation.GetDirtyChunkCount());
		TraceStartupMutationProbe("queue");
		RebuildStartupMutationBaseline();
		TraceStartupMutationProbe("queue-consume-after");
	}
	if (mAllowStartupMutationRebaseline && mFrameIndex > mStartupMutationRebaselineDeadlineFrame)
	{
		TraceStartupMutationProbe("deadline-expire");
		mAllowStartupMutationRebaseline = false;
		mPendingStartupMutationRebaseline = false;
	}
	if (collectRuntimeMutationCacheStats)
	{
		const RuntimeMutationCacheStats cacheStats = mRuntimeMutation.GatherCacheStats();
		mLastPerfShellTraceStats.runtimeMutationActiveChunkCount = cacheStats.activeChunkCount;
		mLastPerfShellTraceStats.runtimeMutationValidChunkCount = cacheStats.validChunkCount;
		mLastPerfShellTraceStats.runtimeMutationExcludedStaticChunkCount = cacheStats.excludedStaticChunkCount;
		mLastPerfShellTraceStats.runtimeMutationCachedSurfaceCount = cacheStats.cachedSurfaceCount;
		mLastPerfShellTraceStats.runtimeMutationCachedTriangleCount = cacheStats.cachedTriangleCount;
		mLastPerfShellTraceStats.runtimeMutationCachedMaterialCount = cacheStats.cachedMaterialCount;
		mLastPerfShellTraceStats.runtimeMutationCachedMaterialStateCount = cacheStats.cachedMaterialStateCount;
		mRuntimeMutation.UpdateHighWaterStats(cacheStats);
	}
	if (tracePtPerf)
	{
		for (const auto& chunk : mStaticMapScene.chunks)
		{
			mLastPerfShellTraceStats.staticAnimatedResidentSliceCacheEntryCount +=
				(uint32_t)chunk.residentMaterialSliceCache.size();
		}
		for (uint32_t chunkIndex = 0; chunkIndex < (uint32_t)mStaticSceneResidency.Registry().entries.size(); ++chunkIndex)
		{
			const auto& entry = mStaticSceneResidency.Registry().entries[chunkIndex];
			if (!entry.valid)
			{
				continue;
			}

			if (entry.animatedRefreshSuppressed)
			{
				mLastPerfShellTraceStats.runtimeAnimatedSuppressedActiveCount++;
			}
			if (chunkIndex >= runtimeAnimatedFrameStats.size())
			{
				continue;
			}

			const auto& frameEntry = runtimeAnimatedFrameStats[chunkIndex];
			if (!frameEntry.touched && !entry.animatedRefreshSuppressed)
			{
				continue;
			}

			RuntimeAnimatedChurnTraceEntry animatedEntry = {};
			animatedEntry.valid = true;
			animatedEntry.chunkIndex = chunkIndex;
			animatedEntry.sectorIndex =
				chunkIndex < mMapWorld.chunks.size() ? mMapWorld.chunks[chunkIndex].sectorIndex : -1;
			animatedEntry.suppressed = entry.animatedRefreshSuppressed || frameEntry.suppressed;
			animatedEntry.materialRefreshes = frameEntry.materialRefreshes;
			animatedEntry.runtimeAttempts = frameEntry.runtimeAttempts;
			animatedEntry.residentApplies = frameEntry.residentApplies;
			animatedEntry.syncSkips = frameEntry.syncSkips;
			InsertRankedTraceEntry(
				mLastPerfShellTraceStats.runtimeAnimatedChurnEntries,
				animatedEntry,
				ScoreRuntimeAnimatedChurnTraceEntry);
		}
	}
	mLastPerfShellTraceStats.runtimeMutationPrimitiveCount = (uint32_t)outGeometry.primitives.size();
	mLastPerfShellTraceStats.runtimeMutationMaterialCount = (uint32_t)outMaterials.materials.size();
	if (residentStaticSceneChanged)
	{
		RuntimeMutationResidentSceneRefreshRequest refreshRequest = {};
		refreshRequest.sceneChanged = true;
		refreshRequest.materialDirty = residentMaterialDirty;
		refreshRequest.geometryDirty = residentGeometryDirty;
		refreshRequest.materialChunkListIndices = &residentMaterialChunkListIndices;
		refreshRequest.animatedMaterialChunkListIndices = &animatedResidentApplyMaterialChunkListIndices;
		refreshRequest.geometryChunkListIndices = &residentGeometryChunkListIndices;
		RuntimeMutationResidentSceneRefreshResult refreshResult = {};
		const auto runtimeMutationCommitStart = collectRuntimeMutationTiming ?
			std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
		const bool committed = mRuntimeMutation.CommitResidentSceneRefresh(
				NRIRuntimeMutationSystem::BuildResidentUploadServices(*this),
				NRIRuntimeMutationSystem::BuildResidentSceneRefreshServices(*this),
				refreshRequest,
				refreshResult);
		if (collectRuntimeMutationTiming)
		{
			mLastPerfShellTraceStats.runtimeMutationCommitMs += std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - runtimeMutationCommitStart).count();
		}
		if (committed)
		{
			if (outResidentStaticSceneChanged != nullptr)
			{
				*outResidentStaticSceneChanged = refreshResult.residentStaticSceneGeometryChanged;
			}
		}
		else
		{
			outGeometry = {};
			outMaterials = {};
			if (outResidentStaticSceneChanged != nullptr)
			{
				*outResidentStaticSceneChanged = false;
			}
			return false;
		}
	}
	mMapMoverShadow.EndFrame(
		mMapMovers,
		mMapWorld,
		mFrameIndex,
		(int)nri_ptmapmovershadow);
	return !outGeometry.primitives.empty() || !outMaterials.materials.empty();
}
