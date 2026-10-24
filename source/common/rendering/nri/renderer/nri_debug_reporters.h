#pragma once

#include "nri_frame_resources.h"
#include "nri_resources.h"

#include <cstdint>
#include <string>
#include <vector>

struct NRIBufferStatusSnapshot
{
	const char* label = "";
	uint64_t usedBytes = 0;
	uint64_t capacityBytes = 0;
	uint64_t usedItems = 0;
	uint64_t capacityItems = 0;
	uint32_t uploadCount = 0;
	uint32_t growthCount = 0;
	uint32_t overwriteCount = 0;
	uint64_t bytesUploadedLastFrame = 0;
	uint32_t growEventsLastFrame = 0;
	uint32_t overwriteEventsLastFrame = 0;
	uint64_t peakUsedBytes = 0;
};

struct NRISceneBufferStatusSnapshot
{
	uint64_t totalUsedBytes = 0;
	uint64_t totalCapacityBytes = 0;
	uint64_t lastFrameUploadBytes = 0;
	uint32_t lastFrameGrowEvents = 0;
	uint32_t lastFrameOverwriteEvents = 0;
	std::vector<NRIBufferStatusSnapshot> buffers;
};

struct NRISelfTestRouteSnapshot
{
	const char* routeName = "unknown";
	const char* presenterName = "unknown";
	const char* ownerName = "unknown";
	const char* passes = "unknown";
	bool denoiserRun = false;
	bool upscalerRun = false;
	bool exposureRun = false;
};

struct NRISelfTestSummarySnapshot
{
	uint32_t traceFrameIndex = 0;
	uint32_t engineFrameIndex = 0;
	const char* mapName = "none";
	const char* levelName = "none";
	const char* graphicsApiName = "unknown";
	bool worldActive = false;
	bool menuActive = false;
	bool gameplayFrame = false;
	bool portal = false;
	int drawmode = 0;
	NRISelfTestRouteSnapshot route;
	int debugMode = 0;
	const char* presentKind = "unknown";
	uint32_t renderWidth = 0;
	uint32_t renderHeight = 0;
	uint32_t outputWidth = 0;
	uint32_t outputHeight = 0;
	uint32_t swapchainFormat = 0;
	bool hdr = false;
	uint32_t primitiveCount = 0;
	uint32_t materialCount = 0;
	uint32_t sceneInstanceCount = 0;
	uint32_t staticInstanceCount = 0;
	uint32_t dynamicInstanceCount = 0;
	uint32_t persistentVoxelInstanceCount = 0;
	uint32_t emissiveInstanceCount = 0;
	uint32_t staticSceneUploadThisFrame = 0;
	uint32_t staticSceneAsBuildThisFrame = 0;
	uint32_t runtimeVoxelOnboardingAdmitted = 0;
	uint32_t runtimeVoxelTexturePrewarmDeferred = 0;
	uint32_t vertexCount = 0;
	uint32_t indexCount = 0;
	uint64_t vertexBytes = 0;
	uint64_t indexBytes = 0;
	uint64_t primitiveBytes = 0;
	uint64_t materialBytes = 0;
	uint64_t instanceBytes = 0;
	uint64_t sceneSignature = 0;
	uint64_t materialSignature = 0;
	uint64_t instanceSignature = 0;
	uint64_t skySignature = 0;
	const char* skyMode = "unknown";
	const char* skySource = "unknown";
	uint64_t skyKey = 0;
	float skyBrightness = 0.0f;
	const char* skyAction = "unknown";
	bool autoExposure = false;
	bool exposureTexture = false;
	float exposure = 1.0f;
	float targetExposure = 1.0f;
	float adaptedExposure = 1.0f;
	float meteredLogLuminance = 0.0f;
	bool exposureStatsValid = false;
	uint64_t exposureStatsFrame = 0;
	bool finalValid = false;
	const char* exposureReason = "unknown";
};

struct NRITextureStatusSnapshot
{
	const char* slotName = "unknown";
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t access = 0;
	uint32_t layout = 0;
	uint32_t stages = 0;
};

struct NRITemporalStatusSnapshot
{
	int debugMode = 0;
	const char* requestedMainUpscaler = "unknown";
	const char* resolvedMainUpscaler = "unknown";
	const char* requestedPostSharpen = "unknown";
	const char* resolvedPostSharpen = "unknown";
	bool taa = false;
	bool guiCapture = false;
	int lastDebugMode = 0;
	const char* lastMainUpscaler = "unknown";
	const char* lastPostSharpen = "unknown";
	bool resetHistory = false;
	bool previousCamera = false;
	NRITextureStatusSnapshot historyInput;
	NRITextureStatusSnapshot historyOutput;
	const char* presentSlotName = "unknown";
	const char* upscaledSlotName = "unknown";
	bool useUpscaled = false;
	const char* historyDomain = "unknown";
	const char* presentDomain = "unknown";
	float temporalExposure = 1.0f;
	float presentExposure = 1.0f;
	float exposureStops = 0.0f;
	float resetThresholdStops = 0.0f;
	bool autoExposure = false;
	bool exposureTexture = false;
	bool taaApply = false;
};

struct NRITemporalTraceSnapshot
{
	const char* stage = "unknown";
	uint32_t frameIndex = 0;
	int debugMode = 0;
	const char* resolvedMainUpscaler = "unknown";
	const char* resolvedPostSharpen = "unknown";
	bool runAppTaa = false;
	bool guiCapture = false;
	const char* primaryDomain = "unknown";
	const char* secondaryDomain = "unknown";
	float temporalExposure = 1.0f;
	float primaryPresentExposure = 1.0f;
	float secondaryPresentExposure = 1.0f;
	bool resetHistory = false;
	const char* resetReason = "none";
	bool previousCamera = false;
	NRITextureStatusSnapshot historyInput;
	NRITextureStatusSnapshot historyOutput;
	NRITextureStatusSnapshot primary;
	NRITextureStatusSnapshot secondary;
	bool useUpscaled = false;
};

struct NRIPortalTraversalStatusSnapshot
{
	bool available = false;
	uint32_t depth = 0;
	uint32_t reflective = 0;
	uint32_t transfer = 0;
	uint32_t runtimeBound = 0;
	uint32_t hittableSurfaces = 0;
	uint32_t pendingPlanePortals = 0;
};

struct NRIResidentMapChunkRegistryStatusSnapshot
{
	bool available = false;
	uint64_t buildSerial = 0;
	uint32_t chunkCount = 0;
	uint32_t activeChunkCount = 0;
	uint32_t mappedChunkCount = 0;
	uint32_t accelerationResidentChunkCount = 0;
	uint32_t animatedCandidateChunkCount = 0;
	uint32_t animatedRefreshSuppressedChunkCount = 0;
	uint32_t mapWorldChunkCount = 0;
	uint32_t boundsValidCount = 0;
	uint32_t boundsInvalidCount = 0;
	float nearDistance = 0.0f;
	uint32_t visibleCount = 0;
	uint32_t invisibleNearCount = 0;
	uint32_t invisibleFarCount = 0;
	uint32_t invisibleUnknownCount = 0;
	uint32_t sampleChunkIndex = UINT32_MAX;
	float sampleCenter[3] = {};
	float sampleRadius = 0.0f;
	float sampleDistance = 0.0f;
	const char* sampleTier = "none";
};

struct NRISurfaceProbeStatusSnapshot
{
	bool recorded = false;
	bool hit = false;
	const char* sourceName = "unknown";
	const char* drawListName = "unknown";
	const char* ownerName = "unknown";
	const char* dataSourceName = "unknown";
	int32_t chunkIndex = -1;
	const char* gateVisible = "no";
	const char* flatDrawlistVisible = "n/a";
	const char* staticResident = "no";
	const char* staticTlasInstanced = "no";
	const char* staticProbeIncluded = "no";
	const char* chunkReplaced = "no";
	std::string chunkReasons = "none";
	uint32_t sectionDirtyCount = 0;
	const char* sectorDirty = "no";
	const char* dragged = "no";
	const char* blindSpot = "no";
	uint32_t replacementSurfaceCount = 0;
	uint32_t replacementTriangleCount = 0;
	int32_t localSpaceIndex = -1;
	int32_t portalGraphIndex = -1;
	int32_t sectorIndex = -1;
	int32_t wallIndex = -1;
	int32_t nextSectorIndex = -1;
	int32_t actorIndex = -1;
	uint32_t cstat = 0;
	uint32_t primitiveIndex = UINT32_MAX;
	uint32_t materialIndex = UINT32_MAX;
	uint32_t textureId = 0;
	uint32_t baseTextureId = 0;
	float distance = 0.0f;
	float position[3] = {};
	uint32_t primitiveFlags = 0;
	const char* indexed = "no";
	const char* fullbright = "no";
	const char* flat = "no";
	const char* sprite = "no";
	const char* mirror = "no";
	const char* sky = "no";
	const char* portal = "no";
	const char* facingBillboard = "no";
	const char* pointSampled = "no";
	const char* textureFullbright = "no";
	const char* textureGlowing = "no";
	const char* textureAutoGlow = "no";
	const char* hasGlowmap = "no";
	const char* hasNormalMap = "no";
	const char* hasMetallicMap = "no";
	const char* hasRoughnessMap = "no";
	uint32_t normalTextureIndex = 0;
	uint32_t metallicTextureIndex = 0;
	uint32_t roughnessTextureIndex = 0;
	float metalnessHint = 0.0f;
	float roughnessHint = 0.45f;
	uint32_t materialClass = 0;
	const char* emissiveModeName = "none";
	uint32_t emissiveTextureIndex = 0;
	const char* lightSurface = "no";
	uint32_t lightMaterialIndex = 0;
	const char* emissiveSurface = "no";
	uint32_t emissivePrimitiveMatchCount = 0;
	const char* emissiveHit = "no";
	uint32_t emissiveSourceFlags = 0;
	uint32_t emissiveSourceRuleId = 0;
	uint32_t emissiveOverrideRuleId = 0;
	int32_t emissiveSectorIndex = -1;
	float sectorResponseScale = 1.0f;
	float sectorReachScale = 1.0f;
	const char* sectorResponseApplied = "no";
	float emissivePrimitiveArea = 0.0f;
	float emissivePowerEstimate = 0.0f;
	float emissiveSelectionWeight = 0.0f;
	float emissiveSelectionPdf = 0.0f;
	float emissiveIntensity = 0.0f;
	const char* materialResponse = "no";
	float materialResponseScale = 1.0f;
	float lightLevel = 0.0f;
	float alpha = 1.0f;
	float averageColor[3] = {};
	float emissiveColor[3] = {};
	float glowColor[3] = {};
};

struct NRIMapChunkPortalDumpRow
{
	uint32_t portalIndex = UINT32_MAX;
	uint32_t sourceSurfaceIndex = UINT32_MAX;
	int32_t sourceSectorIndex = -1;
	int32_t sourceWallIndex = -1;
	int32_t sourcePlane = -1;
	uint32_t targetCount = 0;
	const char* runtimeBound = "no";
	float delta[3] = {};
};

struct NRIMapChunkSurfaceDumpRow
{
	uint32_t surfaceIndex = UINT32_MAX;
	const char* kindName = "unknown";
	const char* sourceName = "unknown";
	int32_t sectionIndex = -1;
	int32_t sectorIndex = -1;
	int32_t wallIndex = -1;
	int32_t nextSectorIndex = -1;
	int32_t actorIndex = -1;
	uint32_t cstat = 0;
	uint32_t flags = 0;
	const char* flat = "no";
	const char* sprite = "no";
	const char* mirror = "no";
	const char* sky = "no";
	const char* portal = "no";
	const char* oneWay = "no";
	const char* facingBillboard = "no";
	const char* pointSampled = "no";
	uint32_t textureId = 0;
	int palette = 0;
	int shade = 0;
	float alpha = 1.0f;
	uint32_t vertexCount = 0;
	uint32_t triangleCount = 0;
};

struct NRIMapChunkDumpSnapshot
{
	bool mapWorldValid = false;
	bool chunkResolved = false;
	bool chunkInRange = false;
	bool usedProbeFallback = false;
	int32_t requestedChunkIndex = -1;
	uint32_t chunkRange = 0;
	int32_t chunkIndex = -1;
	int32_t sectorIndex = -1;
	uint32_t localSpaceIndex = UINT32_MAX;
	uint32_t surfaceCount = 0;
	uint32_t triangleCount = 0;
	uint32_t portalSurfaceCount = 0;
	uint32_t skySurfaceCount = 0;
	uint32_t sourcePortalCount = 0;
	const char* gateVisible = "no";
	const char* residentStatic = "no";
	const char* staticTlasInstanced = "no";
	const char* staticProbeIncluded = "no";
	const char* runtimeReplaced = "no";
	std::string replacementReasons = "none";
	uint32_t sectionDirtyCount = 0;
	const char* sectorDirty = "no";
	const char* dragged = "no";
	const char* blindSpot = "no";
	uint32_t replacementSurfaceCount = 0;
	uint32_t replacementTriangleCount = 0;
	uint32_t duplicateChunkSlotCount = 0;
	uint32_t preferredChunkListIndex = UINT32_MAX;
	bool hasStaticChunk = false;
	uint32_t staticPrimitiveOffset = 0;
	uint32_t staticPrimitiveCount = 0;
	uint32_t staticMaterialOffset = 0;
	uint32_t staticMaterialCount = 0;
	const char* staticAsReady = "no";
	std::vector<NRIMapChunkPortalDumpRow> portals;
	std::vector<NRIMapChunkSurfaceDumpRow> surfaces;
};

struct NRIMapChunkCompareMatchRow
{
	uint32_t staticSurfaceIndex = UINT32_MAX;
	uint32_t liveSurfaceIndex = UINT32_MAX;
	const char* kindName = "unknown";
	const char* sourceName = "unknown";
	int32_t sectorIndex = -1;
	int32_t wallIndex = -1;
	int32_t sectionIndex = -1;
	int32_t nextSectorIndex = -1;
	uint32_t cstat = 0;
	float delta[3] = {};
	float deviationFromMean = 0.0f;
	float areaRatio = 1.0f;
	float normalDot = 1.0f;
	uint32_t staticTextureId = 0;
	uint32_t liveTextureId = 0;
	uint32_t staticFlags = 0;
	uint32_t liveFlags = 0;
};

struct NRIMapChunkCompareSeamRow
{
	uint32_t staticSurfaceIndex = UINT32_MAX;
	uint32_t liveSurfaceIndex = UINT32_MAX;
	const char* kindName = "unknown";
	int32_t wallIndex = -1;
	int32_t nextSectorIndex = -1;
	int32_t adjacentChunkIndex = -1;
	const char* adjacentReplaced = "no";
	float delta[3] = {};
	float deviationFromMean = 0.0f;
	float areaRatio = 1.0f;
	float normalDot = 1.0f;
	const char* seamOutlier = "no";
};

struct NRIMapChunkCompareUnmatchedRow
{
	uint32_t surfaceIndex = UINT32_MAX;
	const char* kindName = "unknown";
	const char* sourceName = "unknown";
	int32_t sectorIndex = -1;
	int32_t wallIndex = -1;
	int32_t sectionIndex = -1;
	int32_t nextSectorIndex = -1;
	uint32_t cstat = 0;
	uint32_t textureId = 0;
	uint32_t flags = 0;
	uint32_t vertexCount = 0;
	uint32_t triangleCount = 0;
};

struct NRIMapChunkCompareSnapshot
{
	bool mapWorldValid = false;
	bool chunkResolved = false;
	bool chunkInRange = false;
	bool liveBuildSucceeded = false;
	int32_t requestedChunkIndex = -1;
	int32_t chunkIndex = -1;
	uint32_t chunkRange = 0;
	int32_t sectorIndex = -1;
	uint32_t staticSurfaceCount = 0;
	uint32_t liveSurfaceCount = 0;
	uint32_t matchedCount = 0;
	uint32_t unmatchedStaticCount = 0;
	uint32_t unmatchedLiveCount = 0;
	std::string replacementReasons = "none";
	const char* dragged = "no";
	const char* replacementActive = "no";
	float meanDelta[3] = {};
	uint32_t within1 = 0;
	uint32_t within4 = 0;
	uint32_t areaOutlierCount = 0;
	uint32_t normalOutlierCount = 0;
	uint32_t materialDiffCount = 0;
	const char* likelyCoherent = "no";
	uint32_t liveTriangleCount = 0;
	uint32_t seamSurfaceCount = 0;
	uint32_t seamOutlierCount = 0;
	uint32_t seamAgainstStaticCount = 0;
	uint32_t seamAgainstReplacedCount = 0;
	std::vector<NRIMapChunkCompareMatchRow> matchRows;
	std::vector<NRIMapChunkCompareSeamRow> seamRows;
	std::vector<NRIMapChunkCompareUnmatchedRow> unmatchedStaticRows;
	std::vector<NRIMapChunkCompareUnmatchedRow> unmatchedLiveRows;
};

struct NRIActorSpriteMaterialTraceRow
{
	uint32_t frameIndex = 0;
	std::string label = "unlabeled";
	int32_t actorIndex = -1;
	const char* sourceName = "unknown";
	uint32_t materialIndex = UINT32_MAX;
	uint32_t textureId = 0;
	uint32_t textureIndex = UINT32_MAX;
	uint32_t emissiveMode = 0;
	uint32_t emissiveTextureIndex = UINT32_MAX;
	uint32_t paletteIndex = 0;
	uint32_t materialFlags = 0;
	uint32_t lightingFlags = 0;
	uint64_t materialKey = 0;
	const void* texture = nullptr;
};

struct NRIActorSpriteMaterialTraceSnapshot
{
	bool emitSummary = false;
	uint32_t frameIndex = 0;
	std::string label = "unlabeled";
	uint32_t materialCount = 0;
	uint32_t textureCount = 0;
	uint32_t actorSurfaceCount = 0;
	uint32_t actorCount = 0;
	uint64_t bridgeHash = 0;
	uint64_t actorHash = 0;
	uint32_t queuedFrameIndex = 0;
	uint32_t outstandingQueuedFrames = 0;
	std::vector<NRIActorSpriteMaterialTraceRow> rows;
};

class NRIRendererDiagnostics
{
public:
	void ResetSelfTestRouteSnapshot();
	void SetSelfTestRouteSnapshot(const char* routeName, const char* presenterName, const char* ownerName, const char* passes, bool denoiserRun, bool upscalerRun, bool exposureRun);
	const NRISelfTestRouteSnapshot& GetSelfTestRouteSnapshot() const { return mSelfTestRoute; }
	void EmitSelfTestSummary(const NRISelfTestSummarySnapshot& snapshot) const;

private:
	NRISelfTestRouteSnapshot mSelfTestRoute = {};
};

NRIBufferStatusSnapshot BuildNRIBufferStatusSnapshot(const NRIBufferResource& resource, const SceneBufferDebugStats& stats);
void PrintNRISceneBufferStatusSnapshot(const NRISceneBufferStatusSnapshot& snapshot);
void PrintNRITemporalStatusSnapshot(const NRITemporalStatusSnapshot& snapshot);
void PrintNRITemporalTraceSnapshot(const NRITemporalTraceSnapshot& snapshot);
void PrintNRIPortalTraversalStatusSnapshot(const NRIPortalTraversalStatusSnapshot& snapshot);
void PrintNRIResidentMapChunkRegistryStatusSnapshot(const NRIResidentMapChunkRegistryStatusSnapshot& snapshot);
void PrintNRISurfaceProbeStatusSnapshot(const NRISurfaceProbeStatusSnapshot& snapshot);
void PrintNRIMapChunkDumpSnapshot(const NRIMapChunkDumpSnapshot& snapshot);
void PrintNRIMapChunkCompareSnapshot(const NRIMapChunkCompareSnapshot& snapshot);
void PrintNRIActorSpriteMaterialTraceSnapshot(const NRIActorSpriteMaterialTraceSnapshot& snapshot);
