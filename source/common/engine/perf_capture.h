#pragma once

#include <cstdint>

struct PerfCompactCaptureToken
{
	uint64_t epoch = 0;
	uint64_t presentationGeneration = 0;
	uint32_t recordIndex = 0;
	explicit operator bool() const { return epoch != 0; }
};

struct PerfCompactNriStats
{
	uint64_t frame = 0;
	uint64_t traceRendererFrame = 0;
	uint64_t traceSettingsKey = 0;
	uint64_t traceWorkloadKey = 0;
	double totalMs = 0.0, initMs = 0.0, mapMs = 0.0, stateMs = 0.0;
	double selectMs = 0.0, lightsMs = 0.0, frameGraphMs = 0.0;
	double postDiagnosticsMs = 0.0, unattributedMs = 0.0;
	double mutationMs = 0.0, mutationDiscoveryMs = 0.0, mutationBudgetMs = 0.0;
	double mutationAnalyzeMs = 0.0, mutationStructuralMs = 0.0, mutationMaterialMs = 0.0;
	double mutationResidentMs = 0.0, mutationCommitMs = 0.0;
	double mutationGeometryBuildMs = 0.0, mutationGeometryBridgeMs = 0.0;
	double mutationPortalAssignMs = 0.0, mutationDeformerCanonicalMs = 0.0;
	double mutationMaterialBuildMs = 0.0;
	double dynamicCaptureMs = 0.0, persistentBatchMs = 0.0;
	double voxelAdmissionPumpMs = 0.0, voxelBatchCacheEntryMs = 0.0, voxelBatchSortMs = 0.0;
	double voxelBatchInstanceSyncMs = 0.0, voxelBatchExistingActorMapMs = 0.0, voxelBatchActorLoopMs = 0.0;
	double voxelBatchMaterialVariantMs = 0.0, voxelBatchMeshAdmissionMs = 0.0;
	double voxelBatchMaterialBridgeMs = 0.0, voxelBatchStateMs = 0.0;
	double materialBridgeMs = 0.0, texturesMs = 0.0, bufferUploadMs = 0.0;
	double persistentVoxelAsMs = 0.0, dynamicAsMs = 0.0, worldTlasMs = 0.0;
	double sceneDataMs = 0.0, stateCommitMs = 0.0, resourceWaitMs = 0.0;
	uint64_t sceneUploadBytes = 0;
	uint32_t resourceWaitCalls = 0;
	uint32_t activePrimitives = 0, dynamicPrimitives = 0, activeMaterials = 0, sceneInstances = 0;
	uint32_t mutationStructural = 0, mutationMaterial = 0, mutationResident = 0;
	uint32_t mutationCandidates = 0, mutationAnalyzed = 0, mutationSweep = 0;
	uint32_t mutationCandidateActive = 0, mutationCandidateVisible = 0;
	uint32_t mutationCandidateStartupVisible = 0, mutationCandidateUnresolved = 0;
	uint32_t mutationCandidateStaticAnimated = 0, mutationCandidateSectorDirty = 0;
	uint32_t mutationCandidateSectionDirty = 0, mutationCandidateDragged = 0;
	uint32_t mutationCandidateSignatureWatch = 0, mutationCandidateDeferredMaterial = 0;
	uint32_t mutationCandidateDeferredStructural = 0;
	uint32_t mutationStructuralChunk[4] = { UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX };
	uint32_t mutationStructuralReason[4] = {};
	uint32_t mutationStructuralTrigger[4] = {};
	uint32_t voxelPressureReason = 0, voxelPressureEntries = 0, voxelPressureResources = 0;
	uint32_t voxelPressureFlags = 0;
	uint32_t traceRenderWidth = 0, traceRenderHeight = 0, traceOutputWidth = 0, traceOutputHeight = 0;
	uint32_t traceDispatchX = 0, traceDispatchY = 0, traceDispatchZ = 0;
	uint32_t traceLightBounces = 0, traceMirrorBounces = 0, tracePortalDepth = 0, traceEmissiveSamples = 0;
	uint32_t traceEmissiveRequestedSamples = 0, traceEmissivePrimaryBudget = 0;
	uint32_t traceIndirectSamplingRequestedMode = 0;
	uint32_t traceIndirectSamplingEffectiveMode = 0, traceIndirectSamplingActiveMode = 0;
	uint32_t traceHitDistanceReconstructionMode = 0;
	uint32_t traceRuntimeLights = 0, traceRuntimeLightTilesX = 0, traceRuntimeLightTilesY = 0;
	uint32_t traceRuntimeLightTileSize = 0, traceRuntimeLightTileIndices = 0, traceRuntimeLightMaxOccupancy = 0;
	uint32_t traceRuntimeLightShadowBudget = 0, traceRuntimeLightShadowCandidates = 0, traceRuntimeLightShadowSelected = 0;
	uint32_t traceRuntimeLightShadowOverflow = 0, traceRuntimeLightShadowTileMax = 0, traceRuntimeLightShadowSelectedTileMax = 0;
	uint64_t traceRuntimeLightShadowSelectionHash = 0;
	uint32_t traceEmissivePrimitiveCount = 0;
	double traceEmissiveTotalPower = 0.0;
	uint32_t traceFlags = 0, traceDebugMode = 0, traceBootstrapMode = 0;
	uint32_t traceUpscalerKind = 0, traceUpscalerMode = 0, traceDenoiserMode = 0;
	uint32_t traceDirectScene = 0, traceDirectional = 0, traceDirectionalShadow = 0;
	uint32_t traceSplitShadow = 0, traceFastEmissiveShadow = 0, traceVisibleChunkGate = 0;
	uint32_t traceVoxelOccurrences = 0, traceVoxelOccurrenceControl = 0;
	uint64_t traceVoxelInstancePrimitives = 0;
	bool rendered = false;
	bool valid = false;
};

struct PerfCompactBoundaryStats
{
	uint64_t frame = 0;
	double waitMs = 0.0, waitPresentMs = 0.0, acquireMs = 0.0, submitMs = 0.0, presentMs = 0.0;
	bool pathTraced = false;
	bool presentOk = false;
	bool valid = false;
};

struct PerfCompactOuterFrame
{
	uint64_t traceFrame = 0, presentationGeneration = 0, simulationGeneration = 0, engineGeneration = 0;
	int gametic = 0;
	double startFrameMs = 0.0, tryMs = 0.0, tryTracedMs = 0.0;
	double displayMs = 0.0, displayBeginMs = 0.0, displayRenderMs = 0.0;
	double displayOverlayMs = 0.0, displayUpdateMs = 0.0;
	double startTicMs = 0.0, musicMs = 0.0, frameMs = 0.0;
	double nriTotalMs = 0.0, nriInitializeMs = 0.0, nriFrameResourcesMs = 0.0;
	double nriUpdateStateMs = 0.0, nriSceneCaptureMs = 0.0, nriGeometryBuildMs = 0.0;
	double nriMaterialBuildMs = 0.0, nriSceneTexturesMs = 0.0, nriSceneBuffersMs = 0.0;
	double nriAccelerationMs = 0.0, nriFrameGraphMs = 0.0, nriTraceMs = 0.0;
	double nriDenoiseMs = 0.0, nriComposeMs = 0.0, nriUpscaleMs = 0.0, nriFinalMs = 0.0;
	int realtics = 0, availabletics = 0, counts = 0, ticks = 0, waitLoops = 0;
	bool doWait = false, zeroReturn = false, waitReturn = false, pausedReturn = false;
	bool fixedSimulationReturn = false;
	uint32_t fixedSimulationSuppressedTailTicks = 0;
	bool displaySkipped = false, levelRendered = false, stateIsLevel = false, nriActive = false;
};

struct PerfCompactGpuTiming
{
	double segmentMs = 0.0, sceneMs = 0.0, traceMs = 0.0, traceDispatchMs = 0.0, denoiseMs = 0.0;
	double compositionMs = 0.0, upscaleMs = 0.0, finalMs = 0.0;
	double smokeSimulationMs = 0.0, smokeVolumeMs = 0.0;
	double smokeGridAllocateMs = 0.0, smokeGridInitializeMs = 0.0, smokeGridDepositMs = 0.0;
	double smokeGridHaloMs = 0.0, smokeGridSimulateMs = 0.0, smokeGridRebuildMs = 0.0;
	double smokeDormantArchiveMs = 0.0, smokeDormantPromoteMs = 0.0, smokeDormantEvolveMs = 0.0;
	double smokeWorldActiveMs = 0.0, smokeWorldLinkMs = 0.0, smokeWorldProposalMs = 0.0;
	double smokeWorldSeedMs = 0.0, smokeWorldTemporalMs = 0.0, smokeWorldFilterMs = 0.0;
	double smokeWorldScatterMs = 0.0, smokeCarrierMs = 0.0, smokeViewPrepareMs = 0.0;
	double smokeMaterializeMs = 0.0, smokeAnalyticMaterializeMs = 0.0;
	double smokeViewPointMs = 0.0, smokeViewDirectionalMs = 0.0;
	double smokeViewDirectReuseMs = 0.0, smokeViewEmissiveMs = 0.0, smokeViewIndirectMs = 0.0;
	double smokeAnalyticEmissiveBuildMs = 0.0, smokeAnalyticEmissiveApplyMs = 0.0;
	double smokeIntegrateMs = 0.0, smokeReconstructionMs = 0.0;
	uint32_t segmentCount = 0, invalidPairs = 0, droppedScopes = 0;

	double SmokeDetailTotalMs() const
	{
		return smokeGridAllocateMs + smokeGridInitializeMs + smokeGridDepositMs + smokeGridHaloMs +
			smokeGridSimulateMs + smokeGridRebuildMs + smokeDormantArchiveMs +
			smokeDormantPromoteMs + smokeDormantEvolveMs + smokeWorldActiveMs + smokeWorldLinkMs +
			smokeWorldProposalMs + smokeWorldSeedMs + smokeWorldTemporalMs + smokeWorldFilterMs +
			smokeWorldScatterMs + smokeCarrierMs + smokeViewPrepareMs + smokeMaterializeMs +
			smokeAnalyticMaterializeMs +
			smokeViewPointMs + smokeViewDirectionalMs + smokeViewDirectReuseMs + smokeViewEmissiveMs +
			smokeViewIndirectMs + smokeIntegrateMs + smokeReconstructionMs;
	}
};

enum class PerfCompactFirstUseDomain : uint32_t
{
	Actor = 1,
	Material = 2,
	Texture = 3,
	Voxel = 4,
	Arena = 5,
	Acceleration = 6,
	Descriptor = 7,
	Ingest = 8,
	Compute = 9,
};

enum class PerfCompactFirstUseStage : uint32_t
{
	Request = 1,
	IndexedPayload = 2,
	ContentHash = 3,
	AverageColor = 4,
	Palette = 5,
	MaterialRows = 6,
	TextureResource = 7,
	UploadRecord = 8,
	UploadSubmit = 9,
	UploadComplete = 10,
	ArenaGrowth = 11,
	ArenaCopy = 12,
	BlasObject = 13,
	BlasSubmit = 14,
	BlasComplete = 15,
	TlasPublish = 16,
	DescriptorWrite = 17,
	Publication = 18,
};

enum class PerfCompactFirstUseState : uint32_t
{
	Instant = 0,
	Pending = 1,
	Fallback = 2,
	Ready = 3,
	Cancelled = 4,
	Failed = 5,
};

enum PerfCompactFirstUseFlags : uint32_t
{
	PerfCompactFirstUseBegin = 1u << 0,
	PerfCompactFirstUseEnd = 1u << 1,
};

// A fixed-size, deferred-output event record. Callers should retain the returned
// event ID while first-use work spans frames and close it with an End record.
struct PerfCompactFirstUseRecord
{
	uint64_t eventId = 0;
	uint64_t actorLifecycleKey = 0;
	uint64_t sourceKey = 0;
	uint64_t meshKey = 0;
	uint64_t materialKey = 0;
	uint64_t validatedSignature = 0;
	uint64_t textureKey = 0;
	uint64_t rendererFrame = 0;
	uint64_t producerFrame = 0;
	uint64_t submittedFence = 0;
	uint64_t publicationFrame = 0;
	uint64_t bytes = 0;
	double cpuMs = 0.0;
	uint32_t queuedSlot = UINT32_MAX;
	uint32_t count = 1;
	PerfCompactFirstUseDomain domain = PerfCompactFirstUseDomain::Actor;
	PerfCompactFirstUseStage stage = PerfCompactFirstUseStage::Request;
	PerfCompactFirstUseState state = PerfCompactFirstUseState::Instant;
	uint32_t flags = 0;
};

void PerfCompactCaptureBeginOuterFrame(uint64_t presentationGeneration);
void PerfCompactCaptureFlushIfReady();
bool PerfCompactCaptureTimingActive();
bool PerfCompactCaptureReadbackDrainActive();
PerfCompactCaptureToken PerfCompactCaptureGetCurrentToken();
void PerfCompactCaptureNoteNri(const PerfCompactCaptureToken& token, const PerfCompactNriStats& stats);
void PerfCompactCaptureNoteBoundary(const PerfCompactCaptureToken& token, const PerfCompactBoundaryStats& stats);
void PerfCompactCaptureExpectGpuSegment(const PerfCompactCaptureToken& token);
void PerfCompactCaptureResolveGpuSegment(const PerfCompactCaptureToken& token, const PerfCompactGpuTiming& timing);
uint64_t PerfCompactCaptureNoteFirstUse(const PerfCompactFirstUseRecord& record);
void PerfCompactCaptureEndOuterFrame(const PerfCompactOuterFrame& frame);
void PerfCompactCaptureAbort(const char* reason);
