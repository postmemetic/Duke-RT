#pragma once

#include "nri_scene_surface_types.h"

#include "flatvertices.h"
#include "hw_drawinfo.h"
#include "hw_drawstructs.h"

#include <array>
#include <cstdint>
#include <vector>

class FGameTexture;
class FVoxelModel;

namespace nri_scene
{
bool TryGetAverageTextureColor(FGameTexture* texture, float* outColor);
void ResetAverageTextureColorCache();
void Copy3(const float* source, float* destination);

static constexpr uint32_t VoxelDuplicateVariantTraceCount = 8;
static constexpr uint32_t DynamicVoxelEscapeTraceCount = 8;

enum class VoxelMeshBakeSpace : uint8_t
{
	Unknown = 0,
	LocalSpace,
	BakedTransform,
};

struct VoxelDuplicateVariantTraceEntry
{
	bool valid = false;
	uint64_t meshKeyHash = 0;
	uint64_t exampleBasisSignature = 0;
	int32_t sourcePicnum = -1;
	uint32_t actorCount = 0;
	uint32_t persistentActorCount = 0;
	uint32_t uniqueBasisSignatureCount = 0;
	uint32_t transformKeyedActorCount = 0;
	VoxelMeshBakeSpace bakeSpace = VoxelMeshBakeSpace::Unknown;
	uint32_t primitiveCountPerActor = 0;
	uint32_t totalDuplicatedPrimitives = 0;
	uint64_t duplicatedBytes = 0;
};

enum class DynamicVoxelEscapeReason : uint8_t
{
	Unknown = 0,
	VariantPending,
	MaterialPending,
	ActorBudget,
	BuildBudget,
	UnsupportedTransform,
	NonLocalSpace,
	CameraOrWeaponSpecial,
	LifecycleTransient,
	NotCacheable,
	ValidationQuarantine,
	FallbackDisabled,
	MissingSurface,
};

const char* GetDynamicVoxelEscapeReasonName(DynamicVoxelEscapeReason reason);

struct DynamicVoxelEscapeTraceEntry
{
	bool valid = false;
	DynamicVoxelEscapeReason reason = DynamicVoxelEscapeReason::Unknown;
	int32_t actorIndex = -1;
	int32_t statnum = -1;
	int32_t sourcePicnum = -1;
	int32_t resolvedVoxelIndex = -1;
	uint64_t meshVariantHash = 0;
	uint64_t materialVariantHash = 0;
	uint32_t primitiveCount = 0;
	uint64_t vertexBytes = 0;
	uint64_t indexBytes = 0;
	uint64_t primitiveBytes = 0;
	uint64_t materialBytes = 0;
	uint64_t totalBytes = 0;
	bool persistentReady = false;
	bool hasCachedSurface = false;
};

enum class PTSkyMode : uint32_t
{
	None = 0,
	SolidColor,
	Cubemap,
};

enum class PTSkySourceType : uint32_t
{
	None = 0,
	Wall,
	Flat,
	Portal,
};

struct PTSkyDescriptor
{
	PTSkyMode mode = PTSkyMode::None;
	PTSkySourceType sourceType = PTSkySourceType::None;
	FGameTexture* texture = nullptr;
	uint32_t faceMask = 0;
	uint32_t priority = 0;
	bool flipTop = false;
	bool isThreeFace = false;
};

struct SceneDebugStats
{
	unsigned int totalDrawItems = 0;
	unsigned int wallDrawItems = 0;
	unsigned int flatDrawItems = 0;
	unsigned int spriteDrawItems = 0;
	unsigned int translucentDrawItems = 0;
	unsigned int triangleEstimate = 0;
	unsigned int materialRefs = 0;
	unsigned int mirrorSurfaces = 0;
	unsigned int skySurfaces = 0;
	unsigned int portalViews = 0;
	unsigned int portalCapturesSkipped = 0;
	unsigned int modelDrawItems = 0;
	unsigned int voxelProxyDrawItems = 0;
	unsigned int unsupportedModelDrawItems = 0;
	unsigned int voxelStableCandidates = 0;
	unsigned int voxelStableUncacheable = 0;
	unsigned int voxelStableSignatureHits = 0;
	unsigned int voxelStableSignatureMisses = 0;
	unsigned int voxelStableSignatureChanges = 0;
	unsigned int voxelStableSplitStable = 0;
	unsigned int voxelStableSplitLive = 0;
	unsigned int voxelCacheEntries = 0;
	unsigned int voxelCacheSurfaceHits = 0;
	unsigned int voxelCacheSurfaceStores = 0;
	unsigned int voxelCacheSurfaceRebuilds = 0;
	unsigned int voxelCacheTransformRebakes = 0;
	unsigned int voxelCacheSurfaceRemoves = 0;
	unsigned int voxelCacheNotCaptured = 0;
	unsigned int voxelCacheDeferred = 0;
	unsigned int voxelCachePrimitives = 0;
	unsigned int voxelCacheActorSurfaces = 0;
	unsigned int voxelCacheUniqueMeshKeys = 0;
	unsigned int voxelCacheUniqueMaterialKeys = 0;
	unsigned int voxelCacheLocalSpaceSurfaces = 0;
	unsigned int voxelCacheBakedTransformSurfaces = 0;
	unsigned int voxelCacheUnknownSpaceSurfaces = 0;
	unsigned int voxelCacheTransformKeyedSurfaces = 0;
	unsigned int voxelCacheUniqueTransformBases = 0;
	unsigned int voxelCacheInvariantWarnings = 0;
	uint64_t voxelCacheDuplicatedVertexBytes = 0;
	uint64_t voxelCacheDuplicatedIndexBytes = 0;
	uint64_t voxelCacheDuplicatedPrimitiveBytes = 0;
	uint64_t voxelCacheDuplicatedTotalBytes = 0;
	unsigned int voxelCacheDuplicateTopCount = 0;
	std::array<VoxelDuplicateVariantTraceEntry, VoxelDuplicateVariantTraceCount> voxelCacheDuplicateTopEntries = {};
	unsigned int dynamicVoxelEscapeActorCount = 0;
	unsigned int dynamicVoxelEscapeEligibleActorCount = 0;
	unsigned int dynamicVoxelEscapeForcedActorCount = 0;
	unsigned int dynamicVoxelEscapePrimitiveCount = 0;
	uint64_t dynamicVoxelEscapeVertexBytes = 0;
	uint64_t dynamicVoxelEscapeIndexBytes = 0;
	uint64_t dynamicVoxelEscapePrimitiveBytes = 0;
	uint64_t dynamicVoxelEscapeMaterialBytes = 0;
	uint64_t dynamicVoxelEscapeTotalBytes = 0;
	unsigned int dynamicVoxelExpectedEscapeActorCount = 0;
	unsigned int dynamicVoxelUnexpectedEscapeActorCount = 0;
	unsigned int dynamicVoxelExpectedEscapePrimitiveCount = 0;
	unsigned int dynamicVoxelUnexpectedEscapePrimitiveCount = 0;
	uint64_t dynamicVoxelExpectedEscapeTotalBytes = 0;
	uint64_t dynamicVoxelUnexpectedEscapeTotalBytes = 0;
	unsigned int dynamicVoxelEscapeTopCount = 0;
	std::array<DynamicVoxelEscapeTraceEntry, DynamicVoxelEscapeTraceCount> dynamicVoxelEscapeTopEntries = {};
	unsigned int dynamicVoxelUnexpectedEscapeTopCount = 0;
	std::array<DynamicVoxelEscapeTraceEntry, DynamicVoxelEscapeTraceCount> dynamicVoxelUnexpectedEscapeTopEntries = {};
};

struct SkyPerfStats
{
	uint32_t updateCalls = 0;
	uint32_t wallUpdateCalls = 0;
	uint32_t flatUpdateCalls = 0;
	uint32_t portalUpdateCalls = 0;
	uint32_t inspectCalls = 0;
	uint32_t inspectCubemapCandidates = 0;
	uint32_t inspectSolidCandidates = 0;
	uint32_t inspectFaceWalks = 0;
	uint32_t averageColorBaseCalls = 0;
	uint32_t averageColorRecursiveCalls = 0;
	uint32_t recursiveSkyboxFaceSamples = 0;
	uint64_t averageColorPixels = 0;
	uint64_t updateTimeUs = 0;
	uint64_t inspectTimeUs = 0;
	uint64_t averageColorTimeUs = 0;
};

struct DynamicCapturePerfStats
{
	uint32_t calls = 0;
	uint32_t wallSurfaces = 0;
	uint32_t flatSurfaces = 0;
	uint32_t spriteSurfaces = 0;
	uint32_t voxelProxySurfaces = 0;
	uint32_t unsupportedModelSurfaces = 0;
	uint32_t voxelCacheStores = 0;
	uint32_t voxelCacheRebuilds = 0;
	uint32_t voxelCacheDeferred = 0;
	uint32_t voxelMeshCacheBuilds = 0;
	uint32_t voxelMeshCacheDeferred = 0;
	uint32_t voxelMeshCacheHits = 0;
	uint32_t voxelMeshCacheMisses = 0;
	uint32_t voxelMeshCacheInvalid = 0;
	uint32_t voxelCanonicalSurfaceBuilds = 0;
	uint32_t voxelCanonicalSurfaceHits = 0;
	uint32_t voxelCanonicalSurfaceInvalid = 0;
	uint32_t voxelDuplicationAuditCalls = 0;
	uint32_t voxelDuplicationAuditEntriesScanned = 0;
	uint32_t voxelDuplicationAuditTemporaryContainersBuilt = 0;
	uint32_t voxelMaintenanceCalls = 0;
	uint32_t voxelMaintenanceSimulationSkips = 0;
	uint32_t voxelMaintenanceLegacyReconciles = 0;
	uint32_t voxelMaintenanceDeltaReconciles = 0;
	uint32_t voxelMaintenanceReasonMask = 0;
	uint32_t voxelMaintenanceLiveActorsEnumerated = 0;
	uint32_t voxelMaintenanceCacheEntriesScanned = 0;
	uint32_t voxelMaintenanceRemovals = 0;
	uint32_t voxelMaintenanceTransformSyncs = 0;
	uint32_t voxelLifecycleEventsApplied = 0;
	uint32_t voxelLifecycleEventsDiscarded = 0;
	uint32_t voxelLifecycleInsertEvents = 0;
	uint32_t voxelLifecycleRemoveEvents = 0;
	uint32_t voxelLifecycleStatEvents = 0;
	uint32_t voxelLifecycleResetEvents = 0;
	uint32_t voxelLifecycleOverflows = 0;
	uint32_t voxelLifecycleRemovalEntriesMarked = 0;
	uint32_t modelActorCandidates = 0;
	uint32_t modelActorSorted = 0;
	uint32_t modelActorSortSkipped = 0;
	uint32_t modelScratchReuses = 0;
	uint32_t modelScratchGrows = 0;
	uint32_t modelScratchFallbacks = 0;
	uint32_t modelBudgetTruncations = 0;
	uint32_t modelSurfaceBuilds = 0;
	double countMs = 0.0;
	double wallsMs = 0.0;
	double flatsMs = 0.0;
	double facingSpritesMs = 0.0;
	double modelSpritesMs = 0.0;
	double modelClassifyMs = 0.0;
	double modelMeshMs = 0.0;
	double modelMeshBuildMs = 0.0;
	double modelSurfaceMs = 0.0;
	double modelSortMs = 0.0;
	double modelStoreMs = 0.0;
	double voxelFrameMs = 0.0;
	double voxelLifecycleMs = 0.0;
	double voxelMaintenanceLiveEnumerationMs = 0.0;
	double voxelMaintenanceReconcileMs = 0.0;
	double voxelDuplicationAuditMs = 0.0;
	double statsMs = 0.0;
};

struct VoxelMeshPrecacheStats
{
	uint32_t textureCandidates = 0;
	uint32_t actorCandidates = 0;
	uint32_t modelCandidates = 0;
	uint32_t meshVariantCandidates = 0;
	uint32_t meshHits = 0;
	uint32_t meshBuilds = 0;
	uint32_t meshInvalid = 0;
	uint32_t meshSkipped = 0;
	uint32_t meshVariantHits = 0;
	uint32_t meshVariantBuilds = 0;
	uint32_t meshVariantInvalid = 0;
	uint32_t vertices = 0;
	uint32_t indices = 0;
	uint32_t primitives = 0;
	uint32_t variantPrimitives = 0;
	double buildMs = 0.0;
};

struct SceneView
{
	HWDrawInfo* drawInfo = nullptr;
	std::vector<SurfaceRef> opaqueWalls;
	std::vector<SurfaceRef> opaqueFlats;
	std::vector<SurfaceRef> opaqueSprites;
	uint32_t primitiveFlags = PrimitiveFlag_None;
	SceneDebugStats stats;
	PTSkyDescriptor sky;
	float skyColor[3] = { 0.38f, 0.48f, 0.65f };
	float groundColor[3] = { 0.08f, 0.08f, 0.08f };
};

struct PersistentVoxelCacheEntryView
{
	uint64_t identityKey = 0;
	uint64_t ownerWorldEpoch = 0;
	uint64_t ownerLifetimeGeneration = 0;
	uint64_t placementGeneration = 0;
	uint64_t placementStateHash = 0;
	uint64_t signature = 0;
	uint64_t geometrySignature = 0;
	uint64_t surfaceSignature = 0;
	uint64_t bakedSurfaceSignature = 0;
	uint64_t materialSignature = 0;
	uint64_t transformBasisSignature = 0;
	uint64_t meshKeyHash = 0;
	uint64_t materialKeyHash = 0;
	uint64_t geometryContentHash = 0;
	uint64_t renderPrimitiveHash = 0;
	uint64_t meshVariantHash = 0;
	uint64_t materialVariantHash = 0;
	VoxelMeshBakeSpace meshBakeSpace = VoxelMeshBakeSpace::Unknown;
	int32_t actorIndex = -1;
	int32_t physicalSectorIndex = -1;
	int32_t sourcePicnum = -1;
	int32_t resolvedVoxelIndex = -1;
	uint32_t primitiveCount = 0;
	uint64_t lastSeenFrame = 0;
	uint64_t retainedFrameAge = 0;
	bool capturedThisFrame = false;
	bool indirectOnly = false;
	bool authorityCurrent = false;
	bool publicationEligible = false;
	bool pendingRemoval = false;
	float instanceTransform[12] = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
	float currentTranslation[3] = {};
	float bakedTranslation[3] = {};
	FVoxelModel* model = nullptr;
	const SurfaceRef* surface = nullptr;
	const SurfaceRef* lightSurface = nullptr;
	SurfaceRef materialSurface;
	bool sharedVariantSurface = false;
	bool desiredPending = false;
	bool directOnlyAdmission = false;
};

struct PersistentVoxelActorCacheStats
{
	uint32_t entries = 0;
	uint32_t readyEntries = 0;
	uint32_t capturedThisFrame = 0;
	uint32_t primitiveCount = 0;
	uint64_t serial = 0;
	uint64_t frame = 0;
};

// Immutable engine-owned authority for one retained voxel occurrence.
struct PersistentVoxelActorAuthorityView
{
	uint64_t identityKey = 0;
	uint64_t ownerWorldEpoch = 0;
	uint64_t ownerLifetimeGeneration = 0;
	uint64_t placementGeneration = 0;
	uint64_t placementStateHash = 0;
	uint64_t lifecycleGeneration = 0;
	int32_t actorIndex = -1;
	int32_t physicalSectorIndex = -1;
	uint64_t lastSeenFrame = 0;
	uint64_t retainedFrameAge = 0;
	bool identityCurrent = false;
	bool live = false;
	bool authorityCurrent = false;
	bool publicationEligible = false;
	bool pendingRemoval = false;
	bool capturedThisFrame = false;
	bool actorPositionSynchronized = false;
	float actorScenePosition[3] = {};
	float cachedActorScenePosition[3] = {};
	float authoritativeInstanceTransform[12] = {
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f
	};
};

struct PrecachedVoxelVariantView
{
	uint64_t meshKeyHash = 0;
	uint64_t materialKeyHash = 0;
	uint64_t geometrySignature = 0;
	uint64_t geometryContentHash = 0;
	uint64_t renderPrimitiveHash = 0;
	uint64_t meshVariantHash = 0;
	uint64_t materialVariantHash = 0;
	uint32_t sourceBits = 0;
	int32_t priority = 0;
	int32_t admissionRank = 0;
	int32_t sourcePicnum = -1;
	int32_t resolvedVoxelIndex = -1;
	uint32_t primitiveCount = 0;
	bool gpuForce = false;
	bool gpuPrefer = false;
	FVoxelModel* model = nullptr;
	const SurfaceRef* surface = nullptr;
	SurfaceRef materialSurface;
	MaterialRef material;
	bool directOnlyAdmission = false;
	bool preloadGeometry = true;
};

struct PrecachedVoxelRawManifestStats
{
	uint32_t discovered = 0;
	uint32_t uniqueRequests = 0;
	uint32_t actorCandidates = 0;
	uint32_t skippedInvalid = 0;
	uint32_t skippedDuplicate = 0;
	uint32_t manifestSources = 0;
	uint32_t manifestLines = 0;
	uint32_t manifestRequests = 0;
	uint32_t manifestSkippedInactive = 0;
	uint32_t manifestSkippedSyntax = 0;
	uint32_t manifestSkippedActor = 0;
	uint32_t manifestSkippedUnsupported = 0;
};

struct PrecachedVoxelRawManifestView
{
	uint64_t meshKeyHash = 0;
	uint64_t materialKeyHash = 0;
	uint64_t geometryContentHash = 0;
	uint64_t renderPrimitiveHash = 0;
	uint64_t meshVariantHash = 0;
	uint64_t materialVariantHash = 0;
	uint32_t sourceBits = 0;
	int32_t priority = 0;
	int32_t admissionRank = 0;
	int32_t sourcePicnum = -1;
	int32_t resolvedVoxelIndex = -1;
	uint32_t primitiveCount = 0;
	uint32_t rawSlabCount = 0;
	uint32_t rawColorRunCount = 0;
	uint64_t rawBytes = 0;
	bool gpuForce = false;
	bool gpuPrefer = false;
	bool legacyGpuCandidate = false;
	bool rawSourceResident = false;
	bool rawStatsReady = false;
	bool materialContextReady = false;
	bool cpuSurfaceReady = false;
	FVoxelModel* model = nullptr;
	MaterialRef material;
};

static constexpr uint32_t PrecachedVoxelSourceBit_MountedVoxelPreload = 1u << 3;
static constexpr uint32_t PrecachedVoxelSourceBit_MountedPreloadMap = 1u << 6;

enum class DynamicVoxelCaptureMode : uint8_t
{
	Authoritative,
	ReadOnlyCache,
	Transient,
};

struct ActorSpriteSceneCaptureResult
{
	bool capturedFallbackScene = false;
	bool currentVoxel = false;
};

SceneDebugStats CollectDebugStats(HWDrawInfo& di);
MaterialRef MakeMaterialRef(FGameTexture* texture, int palette, int shade, float alpha, uint32_t extraFlags);
void UpdateSceneSky(SceneView& outView, FGameTexture* texture, uint32_t fallbackColor, PTSkySourceType sourceType);
void ResetSkyPerfStats();
SkyPerfStats ConsumeSkyPerfStats();
DynamicCapturePerfStats ConsumeDynamicCapturePerfStats();
bool CaptureDynamicScene(HWDrawInfo& di, SceneView& outView, DynamicVoxelCaptureMode voxelCaptureMode = DynamicVoxelCaptureMode::Authoritative);
ActorSpriteSceneCaptureResult CaptureActorSpriteScene(
	HWDrawInfo& di,
	int32_t actorIndex,
	bool residentVoxelReady,
	SceneView& outView);
ActorSpriteSceneCaptureResult CaptureActorVoxelSprite(
	HWDrawInfo& di,
	HWSprite& sprite,
	bool residentVoxelReady,
	SceneView& outView);
bool CaptureScene(HWDrawInfo& di, SceneView& outView);
bool BuildPersistentVoxelCacheSceneView(SceneView& outView);
bool BuildPersistentVoxelCacheEntries(std::vector<PersistentVoxelCacheEntryView>& outEntries);
bool GetPersistentVoxelActorAuthority(
	uint64_t identityKey,
	int32_t actorIndex,
	PersistentVoxelActorAuthorityView& outAuthority);
PersistentVoxelActorCacheStats GetPersistentVoxelActorCacheStats();
bool BuildPrecachedVoxelVariantViews(std::vector<PrecachedVoxelVariantView>& outEntries);
bool BuildPrecachedVoxelRawManifestViews(std::vector<PrecachedVoxelRawManifestView>& outEntries, PrecachedVoxelRawManifestStats* outStats = nullptr);
uint64_t GetPersistentVoxelCacheSerial();
void ResetPersistentVoxelActorCache(const char* reason);
void SetPersistentVoxelActorStartupTransientMode(bool active, const char* reason);
bool PrecacheVoxelModelCpuMesh(FVoxelModel* model, VoxelMeshPrecacheStats* stats = nullptr);
bool PrecacheVoxelTextureCpuMesh(FTextureID texid, VoxelMeshPrecacheStats* stats = nullptr);
void PreloadLiveActorVoxelRawSources();
void PrecacheLiveActorVoxelMeshes(VoxelMeshPrecacheStats* stats = nullptr);
void PrintAndResetLoadingWarmupStats(const char* phase);
}
