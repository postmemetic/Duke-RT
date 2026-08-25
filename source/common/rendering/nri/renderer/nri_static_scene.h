#pragma once

#include "../scene/nri_map_material_layout.h"

#include "nri_frame_resources.h"
#include "nri_resources.h"
#include "nri_runtime_mutation.h"
#include "../scene/nri_geometry_bridge.h"
#include "../scene/nri_map_builder.h"
#include "../scene/nri_material_bridge.h"
#include "../scene/nri_scene_bridge.h"

#include <cstdint>
#include <vector>

struct StaticMapChunkAtlas
{
	struct FreeRange
	{
		uint32_t offset = 0;
		uint32_t count = 0;
	};

	struct ChunkEntry
	{
		uint32_t chunkIndex = UINT32_MAX;
		uint32_t staticSceneChunkListIndex = UINT32_MAX;
		uint32_t vertexOffset = 0;
		uint32_t vertexCount = 0;
		uint32_t indexOffset = 0;
		uint32_t indexCount = 0;
		uint32_t primitiveOffset = 0;
		uint32_t primitiveCount = 0;
		uint32_t materialOffset = 0;
		uint32_t materialCount = 0;
		bool valid = false;
	};

	bool valid = false;
	uint64_t buildSerial = 0;
	uint32_t chunkCount = 0;
	uint32_t vertexCount = 0;
	uint32_t indexCount = 0;
	uint32_t primitiveCount = 0;
	uint32_t materialCount = 0;
	uint32_t vertexCapacity = 0;
	uint32_t indexCapacity = 0;
	uint32_t primitiveCapacity = 0;
	uint32_t materialCapacity = 0;
	std::vector<ChunkEntry> chunks;
	std::vector<FreeRange> freeVertexRanges;
	std::vector<FreeRange> freeIndexRanges;
	std::vector<FreeRange> freePrimitiveRanges;
	std::vector<FreeRange> freeMaterialRanges;
};

struct StaticGeometrySegmentKey
{
	uint64_t geometrySignature = 0;
	uint32_t vertexOffset = 0;
	uint32_t vertexCount = 0;
	uint32_t indexOffset = 0;
	uint32_t indexCount = 0;
	uint32_t primitiveOffset = 0;
	uint32_t primitiveCount = 0;
	uint32_t materialOffset = 0;
	uint32_t materialCount = 0;
	uint32_t sourceChunkIndex = UINT32_MAX;
};

struct StaticMapSegmentBlasCache
{
	struct RouteStats
	{
		uint32_t routedSegment = 0;
		uint32_t routedChunkFallback = 0;
		uint32_t rejectDisabled = 0;
		uint32_t rejectMissingCache = 0;
		uint32_t rejectMissingBlas = 0;
		uint32_t segmentBlasRefs = 0;
		uint32_t chunkBlasRefs = 0;
	};

	struct Entry
	{
		StaticGeometrySegmentKey key = {};
		uint32_t firstChunkListIndex = UINT32_MAX;
		uint32_t refCount = 0;
		uint32_t sourceChunkRefs = 0;
		uint64_t scratchSize = 0;
		NRIAccelerationStructureResource accelerationStructure;
	};

	bool valid = false;
	bool blasBuildEnabled = false;
	uint64_t buildSerial = 0;
	uint32_t candidateCount = 0;
	uint32_t entryCount = 0;
	uint32_t cacheHits = 0;
	uint32_t cacheMisses = 0;
	uint32_t duplicateRefs = 0;
	uint32_t residentBlasCount = 0;
	uint32_t buildsThisFrame = 0;
	uint32_t invalidations = 0;
	uint64_t residentMemoryBytes = 0;
	RouteStats routeStats = {};
	std::vector<Entry> entries;
};

struct StaticMapSceneCache
{
	struct ChunkCache
	{
		struct ResidentMaterialSliceCacheEntry
		{
			uint64_t animatedGeometrySignature = 0;
			uint64_t animatedMaterialSignature = 0;
			uint64_t materialBridgeHash = 0;
			uint64_t actorOverrideHash = 0;
			uint64_t emissiveOverrideHash = 0;
			uint64_t textureSlotRevision = 0;
			bool stableTextureSlots = false;
			uint32_t materialCount = 0;
			nri_scene::MaterialBridgeData remappedMaterialBridge;
			std::vector<nri_scene::MaterialData> gpuMaterials;
		};

		uint32_t chunkIndex = UINT32_MAX;
		uint32_t vertexOffset = 0;
		uint32_t vertexCount = 0;
		uint32_t indexOffset = 0;
		uint32_t indexCount = 0;
		uint32_t primitiveOffset = 0;
		uint32_t primitiveCount = 0;
		uint32_t materialOffset = 0;
		uint32_t materialCount = 0;
		uint64_t geometryTopologySignature = 0;
		uint64_t primitiveLayoutSignature = 0;
		uint64_t exactGeometrySignature = 0;
		uint64_t geometryPayloadHash = 0;
		uint64_t animatedMaterialSignature = 0;
		uint64_t animatedGeometrySignature = 0;
		bool active = true;
		bool blasUpdateEligible = false;
		uint32_t lastResidentBlasReasonMask = 0;
		uint32_t lastResidentBlasSurfaceCount = 0;
		uint32_t lastResidentBlasTriangleCount = 0;
		uint32_t lastResidentBlasMaterialCount = 0;
		bool lastResidentBlasForceTopology = false;
		bool lastResidentBlasRecoveredEmpty = false;
		bool lastResidentBlasKeptGeometrySlice = false;
		bool lastResidentBlasTopologyChanged = false;
		nri::AccelerationStructure* residentBlasScratchSizeCacheKey = nullptr;
		uint64_t residentBlasBuildScratchSize = 0;
		uint64_t residentBlasUpdateScratchSize = 0;
		nri::Buffer* residentBlasVertexBuffer = nullptr;
		nri::Buffer* residentBlasIndexBuffer = nullptr;
		uint32_t residentBlasVertexNum = 0;
		uint64_t residentBlasIndexOffset = 0;
		uint32_t residentBlasIndexNum = 0;
		uint64_t fixedLayoutDeformerKey = 0;
		uint32_t fixedLayoutVertexSpanCount = 0;
		uint32_t fixedLayoutPrimitiveSpanCount = 0;
		uint64_t fixedLayoutVertexBytes = 0;
		uint64_t fixedLayoutPrimitiveBytes = 0;
		bool hasAnimatedTextureCandidates = false;
		bool animatedRefreshSuppressed = false;
		nri_scene::CanonicalPTMapMaterialLayout canonicalMaterialLayout;
		nri_scene::MaterialBridgeData materialBridge;
		std::vector<ResidentMaterialSliceCacheEntry> residentMaterialSliceCache;
		NRIAccelerationStructureResource accelerationStructure;
	};

	bool valid = false;
	bool texturesResident = false;
	bool buffersResident = false;
	bool accelerationResident = false;
	uint64_t buildSerial = 0;
	uint64_t materialGeneration = 0;
	bool gpuMaterialsUseStableTextureSlots = false;
	uint32_t sceneBuildCount = 0;
	uint32_t gpuUploadCount = 0;
	uint32_t accelerationBuildCount = 0;
	uint32_t animatedCandidateChunkCount = 0;
	uint32_t animatedRefreshCount = 0;
	uint32_t animatedRefreshUploadCount = 0;
	uint32_t animatedGeometryFallbackCount = 0;
	uint32_t animatedRefreshSuppressedChunkCount = 0;
	uint32_t reuseCount = 0;
	nri_scene::SceneView sceneView;
	std::vector<nri_scene::SceneView> lightChunkViews;
	nri_scene::GeometryData geometry;
	nri_scene::MaterialBridgeData materialBridge;
	std::vector<nri_scene::MaterialData> gpuMaterials;
	std::vector<ChunkCache> chunks;
	StaticMapSegmentBlasCache segmentBlasCache;
	uint32_t tlasInstanceCount = 0;
};

struct ResidentMapChunkRegistry
{
	struct Entry
	{
		uint32_t chunkIndex = UINT32_MAX;
		uint32_t staticSceneChunkListIndex = UINT32_MAX;
		uint32_t vertexOffset = 0;
		uint32_t vertexCount = 0;
		uint32_t indexOffset = 0;
		uint32_t indexCount = 0;
		uint32_t primitiveOffset = 0;
		uint32_t primitiveCount = 0;
		uint32_t materialOffset = 0;
		uint32_t materialCount = 0;
		uint64_t geometryTopologySignature = 0;
		uint64_t baselineSignature = 0;
		uint64_t liveSignature = 0;
		uint64_t exactGeometrySignature = 0;
		uint64_t animatedMaterialSignature = 0;
		uint64_t materialPayloadHash = 0;
		uint64_t geometryPayloadHash = 0;
		uint64_t animatedGeometrySignature = 0;
		bool valid = false;
		bool active = false;
		bool mappedInStaticScene = false;
		bool accelerationResident = false;
		bool hasAnimatedTextureCandidates = false;
		bool animatedRefreshSuppressed = false;
		bool wasVisibleLastFrame = false;
		bool visibleValidationTraceEmitted = false;
		uint8_t visibleValidationFramesRemaining = 0;
		uint32_t animatedSuppressionEmitCount = 0;
		uint32_t runtimeAnimatedAttemptCount = 0;
		uint32_t runtimeAnimatedResidentApplyCount = 0;
		uint32_t runtimeAnimatedSyncSkipCount = 0;
		nri_scene::PTMapChunkMutationBaseline appliedBaseline;
	};

	bool valid = false;
	uint64_t buildSerial = 0;
	uint32_t chunkCount = 0;
	uint32_t activeChunkCount = 0;
	uint32_t mappedChunkCount = 0;
	uint32_t accelerationResidentChunkCount = 0;
	uint32_t animatedCandidateChunkCount = 0;
	uint32_t animatedRefreshSuppressedChunkCount = 0;
	std::vector<Entry> entries;
};

struct StaticMapSceneResources;

struct NRIStaticSceneRegistrySyncInput
{
	const nri_scene::PTMapWorld* mapWorld = nullptr;
	const StaticMapSceneCache* staticScene = nullptr;
	const StaticMapChunkAtlas* atlas = nullptr;
	const std::vector<RuntimeMutationResidentReplacementInfo>* replacements = nullptr;
	uint64_t (*hashResidentMaterialPayload)(const nri_scene::MaterialBridgeData& materials) = nullptr;
};

struct NRIPreservedStaticMapSkyState;

struct NRIStaticSceneCacheBuildServices
{
	void* user = nullptr;
	void (*resetMutationCacheForStaticSceneBuild)(void* user, uint32_t chunkCount) = nullptr;
	void (*initializeStaticChunkReplacement)(void* user, const nri_scene::PTMapChunk& chunk) = nullptr;
	void (*buildMaterialsWithActorOverrides)(void* user, nri_scene::SceneView& sceneView, nri_scene::MaterialBridgeData& materials, const char* label) = nullptr;
	void (*applyCommittedMapMotion)(void* user, nri_scene::SceneView& sceneView) = nullptr;
	bool (*chunkHasAnimatedStaticMapSurfaceCandidates)(void* user, const nri_scene::PTMapWorld& mapWorld, const nri_scene::PTMapChunk& chunk) = nullptr;
	double* geometryBuildStaticChunkMs = nullptr;
	uint32_t* geometryBuildStaticChunkCalls = nullptr;
	uint32_t* geometryBuildStaticChunkPrimitives = nullptr;
	bool ceilingNudge = false;
	float ceilingNudgeDistance = 0.0f;
};

struct NRIStaticSceneAnimatedMaterialRefreshInput
{
	const nri_scene::PTMapWorld* mapWorld = nullptr;
	StaticMapSceneCache* staticScene = nullptr;
	StaticMapChunkAtlas* atlas = nullptr;
	ResidentMapChunkRegistry* registry = nullptr;
	const nri_scene::SceneView* preservedSkyView = nullptr;
	const std::vector<uint32_t>* visibleChunkWords = nullptr;
	uint32_t* runtimeAnimatedSuppressionEmitCount = nullptr;
	bool traceStats = false;
	bool traceMaterialBridgeFailures = false;
};

struct NRIStaticSceneAnimatedMaterialRefreshServices
{
	void* user = nullptr;
	bool (*refreshAnimatedBindingsForStaticMapChunk)(void* user, const nri_scene::PTMapWorld& mapWorld, const nri_scene::PTMapChunk& chunk, nri_scene::SceneView& ioChunkView) = nullptr;
	void (*buildMaterialsWithActorOverrides)(void* user, nri_scene::SceneView& sceneView, nri_scene::MaterialBridgeData& materials, const char* label) = nullptr;
	bool (*ensurePaletteTexture)(void* user, const nri_scene::MaterialBridgeData& materials) = nullptr;
	bool (*ensureSceneTextures)(void* user, const nri_scene::SceneView& sceneView, const nri_scene::MaterialBridgeData& materials, std::vector<nri_scene::MaterialData>& gpuMaterials, bool preserveExistingSky, const char* reason) = nullptr;
	bool (*uploadStaticMaterialAtlas)(void* user) = nullptr;
	bool (*recoverStaticScene)(void* user, const char* reason) = nullptr;
	void (*syncResidentRegistry)(void* user) = nullptr;
	void (*markUploadedStaticMapSceneLastFrame)(void* user) = nullptr;
};

struct NRIStaticSceneMaterialLightingRefreshInput
{
	StaticMapSceneCache* staticScene = nullptr;
};

struct NRIStaticSceneMaterialLightingRefreshServices
{
	void* user = nullptr;
	bool (*ensurePaletteTexture)(void* user, const nri_scene::MaterialBridgeData& materials) = nullptr;
	bool (*ensureSceneTextures)(void* user, const nri_scene::SceneView& sceneView, const nri_scene::MaterialBridgeData& materials, std::vector<nri_scene::MaterialData>& gpuMaterials, bool preserveExistingSky, const char* reason) = nullptr;
	bool (*uploadStaticMaterialAtlas)(void* user) = nullptr;
};

struct NRIStaticMapInstanceBuildInput
{
	const nri_scene::PTMapWorld* mapWorld = nullptr;
	const StaticMapSceneCache* staticScene = nullptr;
	const StaticMapChunkAtlas* atlas = nullptr;
	const ResidentMapChunkRegistry* registry = nullptr;
	StaticMapSegmentBlasCache::RouteStats* segmentRouteStats = nullptr;
};

struct NRIStaticMapInstanceBuildServices
{
	void* user = nullptr;
	uint64_t (*getAccelerationStructureHandle)(void* user, const NRIAccelerationStructureResource& accelerationStructure) = nullptr;
};

struct NRIStaticMapBlasBuildInput
{
	uint32_t chunkListIndex = UINT32_MAX;
	uint32_t vertexCount = 0;
	uint64_t indexOffsetBytes = 0;
	uint32_t indexCount = 0;
};

struct NRIStaticSceneAccelerationBuildServices
{
	void* user = nullptr;
	uint64_t (*getAccelerationStructureHandle)(void* user, const NRIAccelerationStructureResource& accelerationStructure) = nullptr;
	void (*waitForCommandsTracked)(void* user) = nullptr;
	void (*destroyBufferResource)(void* user, NRIBufferResource& resource) = nullptr;
	void (*destroyAccelerationStructureResource)(void* user, NRIAccelerationStructureResource& resource) = nullptr;
	bool (*createBottomLevelAccelerationStructure)(void* user, const nri::AccelerationStructureDesc& desc, NRIAccelerationStructureResource& outAccelerationStructure) = nullptr;
	uint64_t (*getAccelerationStructureBuildScratchBufferSize)(void* user, const NRIAccelerationStructureResource& accelerationStructure) = nullptr;
	bool (*createScratchBuffer)(void* user, NRIBufferResource& scratchBuffer, uint64_t scratchSize) = nullptr;
	void (*cmdBuildBottomLevelAccelerationStructure)(void* user, const nri::BuildBottomLevelAccelerationStructureDesc& build) = nullptr;
	void (*cmdScratchReuseBarrier)(void* user, NRIBufferResource& scratchBuffer) = nullptr;
	void (*cmdAccelerationReadBarriers)(void* user, const std::vector<NRIAccelerationStructureResource*>& accelerationStructures) = nullptr;
	bool (*buildTopLevelAccelerationStructure)(void* user, const std::vector<nri::TopLevelInstance>& instances, StaticMapSceneResources& staticResources, bool updateLiveState) = nullptr;
};

struct NRIStaticSceneLiveAccelerationBuildInput
{
	const nri_scene::PTMapWorld* mapWorld = nullptr;
	StaticMapSceneCache* staticScene = nullptr;
	StaticMapChunkAtlas* atlas = nullptr;
	const ResidentMapChunkRegistry* registry = nullptr;
	NRIBufferResource* staticVertexBuffer = nullptr;
	NRIBufferResource* staticIndexBuffer = nullptr;
	NRIBufferResource* staticPrimitiveBuffer = nullptr;
	NRIBufferResource* staticMaterialBuffer = nullptr;
	NRIBufferResource* emissiveTlasInstanceBuffer = nullptr;
	NRIBufferResource* sceneInstanceBuffer = nullptr;
	NRIBufferResource* scratchBuffer = nullptr;
	NRIBufferResource* emissiveTopLevelScratchBuffer = nullptr;
	NRIAccelerationStructureResource* emissiveTopLevelAS = nullptr;
	bool hasWorldTlasFrameSlotResources = false;
	uint64_t* staticAccelerationBuildSerial = nullptr;
};

struct NRIStaticSceneLiveAccelerationBuildServices
{
	void* user = nullptr;
	uint64_t (*getAccelerationStructureHandle)(void* user, const NRIAccelerationStructureResource& accelerationStructure) = nullptr;
	bool (*hasAnyDynamicBottomLevelAS)(void* user) = nullptr;
	void (*waitForCommandsTracked)(void* user) = nullptr;
	void (*destroyBufferResource)(void* user, NRIBufferResource& resource) = nullptr;
	void (*destroyAccelerationStructureResource)(void* user, NRIAccelerationStructureResource& resource) = nullptr;
	void (*destroyDynamicBottomLevelAccelerationStructures)(void* user) = nullptr;
	void (*destroyWorldTlasFrameSlots)(void* user) = nullptr;
	void (*resetPersistentVoxelsForStaticAccelerationRebuild)(void* user) = nullptr;
	bool (*createBottomLevelAccelerationStructure)(void* user, const nri::AccelerationStructureDesc& desc, NRIAccelerationStructureResource& outAccelerationStructure) = nullptr;
	uint64_t (*getAccelerationStructureBuildScratchBufferSize)(void* user, const NRIAccelerationStructureResource& accelerationStructure) = nullptr;
	bool (*createScratchBuffer)(void* user, NRIBufferResource& scratchBuffer, uint64_t scratchSize) = nullptr;
	void (*cmdBuildBottomLevelAccelerationStructure)(void* user, const nri::BuildBottomLevelAccelerationStructureDesc& build) = nullptr;
	void (*cmdScratchReuseBarrier)(void* user, NRIBufferResource& scratchBuffer) = nullptr;
	void (*cmdAccelerationReadBarriers)(void* user, const std::vector<NRIAccelerationStructureResource*>& accelerationStructures) = nullptr;
	bool (*buildTopLevelAccelerationStructure)(void* user, const std::vector<nri::TopLevelInstance>& instances) = nullptr;
	bool (*updateSceneDataSet)(void* user, const std::vector<SceneInstanceData>& sceneInstances) = nullptr;
};

struct NRIStaticSceneLiveCacheDestroyInput
{
	StaticMapSceneCache* staticScene = nullptr;
	StaticMapChunkAtlas* atlas = nullptr;
	NRIBufferResource* staticVertexBuffer = nullptr;
	NRIBufferResource* staticIndexBuffer = nullptr;
	NRIBufferResource* staticPrimitiveBuffer = nullptr;
	NRIBufferResource* staticMaterialBuffer = nullptr;
	uint32_t* boundStaticPrimitiveCount = nullptr;
	uint32_t* boundDynamicPrimitiveCount = nullptr;
	uint32_t* boundStaticMaterialCount = nullptr;
	uint32_t* boundDynamicMaterialCount = nullptr;
	uint32_t* boundPortalCount = nullptr;
};

struct NRIStaticSceneLiveCacheDestroyServices
{
	void* user = nullptr;
	void (*waitForCommandsTracked)(void* user) = nullptr;
	void (*destroyBufferResource)(void* user, NRIBufferResource& resource) = nullptr;
	void (*destroyAccelerationStructureResource)(void* user, NRIAccelerationStructureResource& resource) = nullptr;
	void (*resetPersistentDynamicEmissiveCache)(void* user) = nullptr;
	void (*resetSceneFrameGeometry)(void* user) = nullptr;
	void (*resetRuntimeMutationCacheAndFrameForStaticScene)(void* user) = nullptr;
	void (*resetResidentMapChunkRegistry)(void* user) = nullptr;
};

struct NRIStaticSceneEnsureInput
{
	const nri_scene::PTMapWorld* mapWorld = nullptr;
	StaticMapSceneCache* staticScene = nullptr;
	StaticMapChunkAtlas* atlas = nullptr;
	NRIPreservedStaticMapSkyState* preservedSkyState = nullptr;
	uint64_t* staticAccelerationBuildSerial = nullptr;
	bool* uploadedStaticMapSceneLastFrame = nullptr;
	bool* builtStaticMapSceneASLastFrame = nullptr;
	uint32_t frameIndex = 0;
	bool traceSceneStats = false;
	bool tracePtPerf = false;
	bool traceSkyPerf = false;
};

struct NRIStaticSceneEnsureServices
{
	void* user = nullptr;
	NRIStaticSceneCacheBuildServices cacheBuildServices = {};
	void (*destroyStaticMapSceneCache)(void* user, const char* reason) = nullptr;
	bool (*refreshStaticMapAnimatedMaterials)(void* user) = nullptr;
	bool (*ensurePaletteTexture)(void* user, const nri_scene::MaterialBridgeData& materials) = nullptr;
	bool (*ensureSceneTextures)(
		void* user,
		const nri_scene::SceneView& sceneView,
		const nri_scene::MaterialBridgeData& materials,
		std::vector<nri_scene::MaterialData>& gpuMaterials,
		bool preserveExistingSky,
		const char* reason) = nullptr;
	bool (*uploadStaticMapChunkAtlas)(void* user) = nullptr;
	bool (*buildStaticMapAccelerationStructures)(void* user) = nullptr;
	void (*syncResidentRegistryFromStaticScene)(void* user) = nullptr;
	void (*noteResidentStaticSceneTextureBuild)(void* user) = nullptr;
	bool (*finalizeStaticMapSceneBuildCommands)(void* user) = nullptr;
};

struct NRIStaticSceneResourceDestroyServices
{
	void* user = nullptr;
	void (*waitForCommandsTracked)(void* user) = nullptr;
	void (*destroyBufferResource)(void* user, NRIBufferResource& resource) = nullptr;
	void (*destroyAccelerationStructureResource)(void* user, NRIAccelerationStructureResource& resource) = nullptr;
	void (*resetSceneFrameGeometry)(void* user) = nullptr;
};

namespace nri_static_scene
{
	void InitializeStaticMapSceneCacheBuild(
		const nri_scene::PTMapWorld& mapWorld,
		const NRIPreservedStaticMapSkyState* preservedSkyState,
		const NRIStaticSceneCacheBuildServices& services,
		StaticMapSceneCache& outStaticScene);

	void AppendStaticMapSceneCacheChunk(
		const nri_scene::PTMapWorld& mapWorld,
		const nri_scene::PTMapChunk& chunk,
		const nri_scene::SceneView* preservedSkyView,
		const NRIStaticSceneCacheBuildServices& services,
		StaticMapSceneCache& outStaticScene);

	bool BuildStaticMapSceneCache(
		const nri_scene::PTMapWorld& mapWorld,
		const NRIPreservedStaticMapSkyState* preservedSkyState,
		const NRIStaticSceneCacheBuildServices& services,
		StaticMapSceneCache& outStaticScene);

	bool RebuildResidentStaticMaterialBridgeFromChunks(
		StaticMapSceneCache& staticScene,
		const StaticMapChunkAtlas& atlas,
		bool traceFailures);

bool RefreshStaticMapAnimatedMaterials(
	const NRIStaticSceneAnimatedMaterialRefreshInput& input,
	const NRIStaticSceneAnimatedMaterialRefreshServices& services);
bool RefreshStaticMapSceneMaterialLighting(
	const NRIStaticSceneMaterialLightingRefreshInput& input,
	const NRIStaticSceneMaterialLightingRefreshServices& services);

	void BuildStaticMapInstances(
		const NRIStaticMapInstanceBuildInput& input,
		const NRIStaticMapInstanceBuildServices& services,
		std::vector<nri::TopLevelInstance>& outTlasInstances,
		std::vector<SceneInstanceData>& outSceneInstances);

	bool BuildStaticMapBlasBuildInputs(
		const StaticMapSceneCache& staticScene,
		const StaticMapChunkAtlas& atlas,
		std::vector<NRIStaticMapBlasBuildInput>& outBuildInputs);

	nri::BottomLevelGeometryDesc BuildStaticMapBlasGeometryDesc(
		const NRIStaticMapBlasBuildInput& buildInput,
		nri::Buffer* vertexBuffer,
		nri::Buffer* indexBuffer);

	bool BuildStaticMapAccelerationStructures(
		const nri_scene::PTMapWorld* mapWorld,
		StaticMapSceneCache& staticScene,
		StaticMapSceneResources& staticResources,
		const NRIStaticSceneAccelerationBuildServices& services,
		bool updateLiveState);

	bool BuildLiveStaticMapAccelerationStructures(
		const NRIStaticSceneLiveAccelerationBuildInput& input,
		const NRIStaticSceneLiveAccelerationBuildServices& services);

	void DestroyLiveStaticMapSceneCache(
		const NRIStaticSceneLiveCacheDestroyInput& input,
		const NRIStaticSceneLiveCacheDestroyServices& services,
		bool waitForCommands);

	bool EnsureStaticMapScene(
		const NRIStaticSceneEnsureInput& input,
		const NRIStaticSceneEnsureServices& services);

	void DestroyStaticMapSceneResources(
		StaticMapSceneCache& staticScene,
		StaticMapSceneResources& staticResources,
		const NRIStaticSceneResourceDestroyServices& services,
		bool waitForCommands,
		bool resetSceneFrameGeometry);

	void PrintStaticMapSceneStatus(
		const StaticMapSceneCache& staticScene,
		bool usedStaticMapSceneLastFrame,
		bool uploadedStaticMapSceneLastFrame,
		bool builtStaticMapSceneASLastFrame);
}

class NRIStaticSceneResidency
{
public:
	struct ChunkDiagnosticFacts
	{
		bool residentStatic = false;
		bool staticTlasInstanced = false;
		bool staticProbeIncluded = false;
		uint32_t duplicateChunkSlotCount = 0;
		uint32_t preferredChunkListIndex = UINT32_MAX;
		bool hasStaticChunk = false;
		uint32_t staticPrimitiveOffset = 0;
		uint32_t staticPrimitiveCount = 0;
		uint32_t staticMaterialOffset = 0;
		uint32_t staticMaterialCount = 0;
		bool staticAsReady = false;
	};

	ResidentMapChunkRegistry& Registry() { return mResidentMapChunkRegistry; }
	const ResidentMapChunkRegistry& Registry() const { return mResidentMapChunkRegistry; }
	void ResetResidentMapChunkRegistry() { mResidentMapChunkRegistry = {}; }
	void SyncResidentMapChunkRegistryFromStaticScene(const NRIStaticSceneRegistrySyncInput& input);

	static uint32_t GetStaticSceneChunkSlotPreference(
		const StaticMapSceneCache& staticScene,
		const StaticMapChunkAtlas& atlas,
		uint32_t chunkListIndex);
	static uint32_t FindPreferredStaticSceneChunkListIndex(
		const StaticMapSceneCache& staticScene,
		const StaticMapChunkAtlas& atlas,
		uint32_t chunkIndex);
	static uint32_t CountStaticSceneChunkSlots(
		const StaticMapSceneCache& staticScene,
		uint32_t chunkIndex);
	static ChunkDiagnosticFacts BuildChunkDiagnosticFacts(
		const StaticMapSceneCache& staticScene,
		const StaticMapChunkAtlas& atlas,
		uint32_t chunkIndex);

private:
	ResidentMapChunkRegistry mResidentMapChunkRegistry;
};

struct StaticMapSceneResources
{
	NRIBufferResource vertexBuffer;
	NRIBufferResource indexBuffer;
	NRIBufferResource primitiveBuffer;
	NRIBufferResource materialBuffer;
	StaticMapChunkAtlas chunkAtlas;
	NRIBufferResource tlasInstanceBuffer;
	NRIBufferResource scratchBuffer;
	NRIBufferResource topLevelScratchBuffer;
	NRIAccelerationStructureResource topLevelAS;
	uint64_t accelerationBuildSerial = 0;
	uint32_t tlasInstanceCount = 0;
	std::vector<SceneInstanceData> sceneInstances;
};
