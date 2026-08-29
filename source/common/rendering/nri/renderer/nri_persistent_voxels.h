#pragma once

#include "nri_voxel_admission_scheduler.h"

#include "nri_persistent_voxel_admission_index.h"
#include "nri_persistent_voxel_material_closure.h"
#include "nri_persistent_voxel_material_range_allocator.h"
#include "nri_persistent_voxel_material_upload.h"
#include "nri_frame_resources.h"
#include "nri_persistent_voxel_shared_blas.h"
#include "nri_persistent_voxel_shadow_proxy.h"
#include "nri_renderer_settings.h"
#include "nri_resources.h"
#include "nri_runtime_mutation.h"
#include "nri_scene_lights.h"
#include "nri_scene_instance_visibility.h"
#include "nri_voxel_compute_meshing.h"
#include "nri_voxel_representation_policy.h"
#include "nri_actor_occurrence_diagnostics.h"
#include "nri_actor_occurrence_ledger.h"
#include "nri_actor_occurrence_policy.h"
#include "nri_actor_workload_mask_policy.h"

#include "../scene/nri_geometry_bridge.h"
#include "../scene/nri_material_bridge.h"
#include "../scene/nri_scene_bridge.h"

#include <array>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct PersistentVoxelBatch
{
	struct ActorEntry
	{
		uint64_t identityKey = 0;
		uint64_t ownerWorldEpoch = 0;
		uint64_t ownerLifetimeGeneration = 0;
		uint64_t placementGeneration = 0;
		uint64_t placementStateHash = 0;
		uint64_t bindingGeneration = 0;
		uint64_t worldTlasPublicationHash = 0;
		uint64_t signature = 0;
		uint64_t geometrySignature = 0;
		uint64_t surfaceSignature = 0;
		uint64_t bakedSurfaceSignature = 0;
		uint64_t materialSignature = 0;
		uint64_t meshResourceKey = 0;
		uint64_t meshKeyHash = 0;
		uint64_t materialKeyHash = 0;
		uint64_t meshVariantHash = 0;
		uint64_t materialVariantHash = 0;
		uint64_t lastSeenFrame = 0;
		uint64_t retainedFrameAge = 0;
		int32_t actorIndex = -1;
		int32_t physicalSectorIndex = -1;
		int32_t sourcePicnum = -1;
		int32_t resolvedVoxelIndex = -1;
		uint32_t visibilityChunkIndex = UINT32_MAX;
		uint32_t worldTlasFrameIndex = UINT32_MAX;
		uint32_t worldTlasInstanceIndex = UINT32_MAX;
		uint32_t worldTlasOccurrenceGeneration = 0;
		bool capturedThisFrame = false;
		bool indirectOnly = false;
		bool inWorldTlasThisFrame = false;
		bool authorityCurrent = false;
		bool publicationEligible = false;
		bool pendingRemoval = false;
		bool active = true;
		uint32_t primitiveOffset = 0;
		uint32_t primitiveCount = 0;
		uint32_t indexOffset = 0;
		uint32_t indexCount = 0;
		uint32_t materialOffset = 0;
		uint32_t materialCount = 0;
		uint32_t materialRowSpan = 0;
		uint64_t materialSlotGeneration = 0;
		std::array<float, 12> instanceTransform = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
		std::array<float, 12> previousInstanceTransform = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
		nri_scene::MaterialBridgeData materialBridge;
		std::vector<SceneLightSystem::SurfaceRecord> lightRecords;
	};

	bool valid = false;
	uint64_t sourceSerial = 0;
	uint32_t surfaceCount = 0;
	uint32_t primitiveCount = 0;
	uint32_t indexCount = 0;
	uint32_t materialCount = 0;
	uint32_t activeActorCount = 0;
	uint32_t rebuildCount = 0;
	nri_scene::SceneDebugStats stats;
	nri_scene::MaterialBridgeData materialBridge;
	std::vector<ActorEntry> actors;
};

struct PersistentVoxelMeshVariantResource
{
	FVoxelModel* sourceModel = nullptr;
	uint64_t resourceKey = 0;
	uint64_t meshKeyHash = 0;
	uint64_t geometrySignature = 0;
	uint64_t geometryContentHash = 0;
	uint64_t renderPrimitiveHash = 0;
	uint64_t transformBasisSignature = 0;
	nri_scene::VoxelMeshBakeSpace meshBakeSpace = nri_scene::VoxelMeshBakeSpace::Unknown;
	uint32_t primitiveCount = 0;
	uint32_t indexCount = 0;
	uint32_t vertexCount = 0;
	uint32_t vertexOffset = 0;
	uint32_t vertexCapacity = 0;
	uint32_t indexOffset = 0;
	uint32_t indexCapacity = 0;
	uint32_t primitiveOffset = 0;
	uint32_t primitiveCapacity = 0;
	uint32_t tlasReadyFrame = 0;
	uint32_t lastDesiredMapGeneration = 0;
	uint32_t lastUsedMapGeneration = 0;
	uint32_t lastUsedFrame = 0;
	uint64_t directComputeGeneration = 0;
	uint64_t directComputeSourceArchiveSerial = 0;
	uint32_t directComputeJobId = 0;
	uint32_t directComputeRequestFrame = UINT32_MAX;
	uint32_t directComputeReadyFrame = 0;
	uint32_t directComputeBlasFrame = UINT32_MAX;
	uint32_t directComputePublishedFrame = UINT32_MAX;
	uint32_t sourceBits = 0;
	uint32_t activeActorReferences = 0;
	int32_t priority = 0;
	uint64_t residentBytes = 0;
	bool tlasPublished = false;
	bool shadowProxyPrimitiveSemanticsCertified = false;
	bool directComputePublished = false;
	NRIVoxelComputeDirectPublishOutputKind directComputeOutputKind = NRIVoxelComputeDirectPublishOutputKind::None;
	NRIVoxelComputeDirectPublishFailure directComputeFailure = NRIVoxelComputeDirectPublishFailure::None;
	bool cold = false;
	bool gpuForce = false;
	bool gpuPrefer = false;
	bool lightTemplateValid = false;
	float lightTemplateCenter[3] = {};
	float lightTemplateBoundsRadius = 0.0f;
	float lightTemplateSurfaceArea = 0.0f;
	bool boundsValid = false;
	float boundsMin[3] = {};
	float boundsMax[3] = {};
	float boundsCenterMagnitude = 0.0f;
	float boundsMaxAbs = 0.0f;
	float surfaceArea = 0.0f;
	float bakedTranslation[3] = {};
	NRIBufferResource vertexBuffer;
	NRIBufferResource indexBuffer;
	NRIAccelerationStructureResource accelerationStructure;
	NRIVoxelShadowProxyResource shadowProxy;
};

struct PersistentVoxelMaterialVariantResource
{
	uint64_t materialKeyHash = 0;
	uint64_t materialSignature = 0;
	uint64_t materialPayloadHash = 0;
	uint64_t materialBridgeBuildSerial = 0;
	uint32_t materialOffset = 0;
	uint32_t materialCount = 0;
	uint32_t materialCapacity = 0;
	uint64_t materialSlotGeneration = 0;
	uint32_t lastDesiredMapGeneration = 0;
	uint32_t lastUsedMapGeneration = 0;
	uint32_t lastUsedFrame = 0;
	uint32_t sourceBits = 0;
	uint32_t activeActorReferences = 0;
	int32_t priority = 0;
	uint64_t residentBytes = 0;
	uint64_t materialUploadHash = 0;
	bool cold = false;
	bool gpuForce = false;
	bool gpuPrefer = false;
	nri_scene::MaterialBridgeData materialBridge;
};

enum class PersistentVoxelAdmissionState : uint8_t
{
	Pending,
	DirectComputePending,
	DirectBlasPending,
	UploadingVertices,
	UploadingIndices,
	UploadingPrimitives,
	BuildingBlas,
	Ready,
	Deferred,
	Failed,
};

struct PersistentVoxelAdmissionEntry
{
	uint64_t pairKey = 0;
	nri_scene::PrecachedVoxelVariantView variant;
	PersistentVoxelAdmissionState state = PersistentVoxelAdmissionState::Pending;
	uint32_t sourceBits = 0;
	int32_t priority = 0;
	int32_t admissionRank = 0;
	bool gpuForce = false;
	bool gpuPrefer = false;
	bool runtimeRequested = false;
	uint32_t retryCount = 0;
	uint32_t mapGeneration = 0;
	uint32_t firstQueuedFrame = UINT32_MAX;
	uint32_t runtimeFirstQueuedFrame = UINT32_MAX;
	uint64_t estimatedBytes = 0;
	uint64_t bytesUploaded = 0;
	bool uploadPrepared = false;
	uint32_t shaderVertexOffset = 0;
	uint32_t shaderIndexOffset = 0;
	uint32_t shaderPrimitiveOffset = 0;
	uint32_t savedVertexCursor = 0;
	uint32_t savedIndexCursor = 0;
	uint32_t savedPrimitiveCursor = 0;
	uint64_t vertexBytesUploaded = 0;
	uint64_t vertexArenaBytesUploaded = 0;
	uint64_t indexBytesUploaded = 0;
	uint64_t indexArenaBytesUploaded = 0;
	uint64_t primitiveBytesUploaded = 0;
	bool uploadSubmittedBeforeBlas = false;
	bool uploadGeometryFromCompute = false;
	bool computeGeometryFailed = false;
	bool cpuGeometryBuildCounted = false;
	bool cpuGeometryUploadCounted = false;
	uint32_t computeGeometryJobId = 0;
	bool directComputeRequested = false;
	bool directComputeFailed = false;
	uint64_t directComputeGeneration = 0;
	uint32_t directComputeJobId = 0;
	uint32_t directComputeRequestFrame = UINT32_MAX;
	NRIVoxelComputeDirectPublishOutputKind directComputeOutputKind = NRIVoxelComputeDirectPublishOutputKind::None;
	NRIVoxelComputeDirectPublishFailure directComputeFailure = NRIVoxelComputeDirectPublishFailure::None;
	uint64_t directBlasFenceValue = 0;
	uint32_t directBlasRecordedFrame = UINT32_MAX;
	bool directBlasExclusive = false;
	uint64_t schedulerTokenId = 0;
	NRIBufferResource directBlasScratchBuffer;
	nri_scene::GeometryData uploadGeometry;
	std::vector<uint32_t> uploadGpuIndices;
	std::vector<nri_scene::PrimitiveData> uploadGpuPrimitives;
	PersistentVoxelMeshVariantResource uploadMeshResource;
	PersistentVoxelMaterialVariantResource uploadMaterialResource;
	const char* lastReason = "none";
};

struct PersistentVoxelReadinessStatus
{
	const char* reason = "ready";
	bool ready = false;
	bool meshPresent = false;
	bool meshPublished = false;
	bool meshKeyMatches = false;
	bool meshCountsValid = false;
	bool meshPrivateBuffersReady = false;
	bool meshDirectBuffersReady = false;
	bool meshArenaBuffersReady = false;
	bool blasReady = false;
	bool materialPresent = false;
	bool materialPublished = false;
	bool materialKeyMatches = false;
	bool materialCountValid = false;
	bool materialBridgeReady = false;
	uint64_t meshResourceKey = 0;
	uint32_t meshVertexCount = 0;
	uint32_t meshIndexCount = 0;
	uint32_t meshPrimitiveCount = 0;
	uint32_t materialCount = 0;
	uint32_t materialBridgeCount = 0;
};

struct PersistentVoxelAdmissionStats
{
	uint32_t queued = 0;
	uint32_t ready = 0;
	uint32_t deferred = 0;
	uint32_t failed = 0;
	uint32_t enqueued = 0;
	uint32_t deduped = 0;
	uint32_t promoted = 0;
	uint32_t uploaded = 0;
	uint32_t force = 0;
	uint32_t prefer = 0;
	uint32_t runtime = 0;
	uint32_t skippedBudget = 0;
	uint32_t failedThisPump = 0;
	uint32_t statePending = 0;
	uint32_t stateDirectPending = 0;
	uint32_t stateDirectBlasPending = 0;
	uint32_t stateUploading = 0;
	uint32_t stateBuildingBlas = 0;
	uint32_t runtimePending = 0;
	uint32_t runtimeDirectPending = 0;
	uint32_t oldestQueueAgeFrames = 0;
	uint32_t oldestRuntimeAgeFrames = 0;
	uint32_t oldestDirectAgeFrames = 0;
	uint32_t directRequests = 0;
	uint32_t directReady = 0;
	uint32_t directPending = 0;
	uint32_t directFailures = 0;
	uint32_t directRejected = 0;
	uint32_t directStaleDrops = 0;
	uint32_t directReadyLatencySamples = 0;
	uint32_t directBlasLatencySamples = 0;
	uint32_t maxDirectReadyLatencyFrames = 0;
	uint32_t maxDirectBlasLatencyFrames = 0;
	uint64_t totalDirectReadyLatencyFrames = 0;
	uint64_t totalDirectBlasLatencyFrames = 0;
	uint32_t cpuGeometryAvoided = 0;
	uint32_t cpuGeometryFallback = 0;
	uint32_t materialPayloadBuilds = 0;
	uint32_t materialPayloadReuses = 0;
	uint64_t bytesPending = 0;
	uint64_t bytesUploaded = 0;
	uint64_t materialPayloadBytes = 0;
};

struct PersistentVoxelInstanceRecord
{
	uint64_t identityKey = 0;
	uint64_t ownerWorldEpoch = 0;
	uint64_t ownerLifetimeGeneration = 0;
	uint64_t placementGeneration = 0;
	uint64_t placementStateHash = 0;
	uint64_t bindingGeneration = 0;
	uint64_t signature = 0;
	uint64_t geometrySignature = 0;
	uint64_t surfaceSignature = 0;
	uint64_t bakedSurfaceSignature = 0;
	uint64_t materialSignature = 0;
	uint64_t meshKeyHash = 0;
	uint64_t materialKeyHash = 0;
	uint64_t meshVariantHash = 0;
	uint64_t materialVariantHash = 0;
	uint64_t meshResourceKey = 0;
	uint32_t primitiveCount = 0;
	uint32_t lastSeenFrame = 0;
	int32_t physicalSectorIndex = -1;
	bool authorityCurrent = false;
	bool publicationEligible = false;
	bool pendingRemoval = false;
	bool active = false;
	bool pending = false;
	std::array<float, 12> currentTransform = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
	std::array<float, 12> previousTransform = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
};

struct NRIPersistentVoxelResetServices
{
	using RetireBufferFn = void (*)(void* user, NRIBufferResource& resource);
	using RetireAccelerationStructureFn = void (*)(void* user, NRIAccelerationStructureResource& resource);
	using InvalidateSceneDataDescriptorsFn = void (*)(void* user);

	void* user = nullptr;
	RetireBufferFn retireBuffer = nullptr;
	RetireAccelerationStructureFn retireAccelerationStructure = nullptr;
	InvalidateSceneDataDescriptorsFn invalidateSceneDataDescriptors = nullptr;

	void RetireBuffer(NRIBufferResource& resource) const;
	void RetireAccelerationStructure(NRIAccelerationStructureResource& resource) const;
	void InvalidateSceneDataDescriptors() const;
};

struct NRIPersistentVoxelBatchStats;

struct NRIPersistentVoxelMaterialClosureServices
{
	using RegisterFn = bool (*)(
		void* user,
		uint64_t buildSerial,
		uint64_t materialKey,
		uint64_t validatedSignature,
		const nri_scene::MaterialBridgeData& materials,
		NRIPersistentVoxelMaterialClosureSource source,
		NRIPersistentVoxelMaterialClosureResult& outResult);
	using TryReuseFn = bool (*)(
		void* user,
		uint64_t buildSerial,
		uint64_t materialKey,
		uint64_t validatedSignature,
		NRIPersistentVoxelMaterialClosureSource source,
		nri_scene::MaterialBridgeData& outMaterials,
		NRIPersistentVoxelMaterialClosureResult& outResult);

	void* user = nullptr;
	RegisterFn registerClosure = nullptr;
	TryReuseFn tryReuse = nullptr;

	bool Register(
		uint64_t buildSerial,
		uint64_t materialKey,
		uint64_t validatedSignature,
		const nri_scene::MaterialBridgeData& materials,
		NRIPersistentVoxelMaterialClosureSource source,
		NRIPersistentVoxelMaterialClosureResult& outResult) const;
	bool TryReuse(
		uint64_t buildSerial,
		uint64_t materialKey,
		uint64_t validatedSignature,
		NRIPersistentVoxelMaterialClosureSource source,
		nri_scene::MaterialBridgeData& outMaterials,
		NRIPersistentVoxelMaterialClosureResult& outResult) const;
};

struct NRIPersistentVoxelPreloadServices
{
	using PumpAdmissionQueueFn = bool (*)(void* user, const char* phase);
	using PumpComputeJobsFn = void (*)(void* user, uint32_t frameIndex);
	using EnsureBatchFn = bool (*)(void* user, NRIPersistentVoxelBatchStats* outStats);
	using WarmSharedBlasFn = bool (*)(void* user, const std::vector<nri_scene::PrecachedVoxelVariantView>& variants, uint32_t frameIndex);
	using IsSubmitBudgetHitFn = bool (*)(void* user);
	using BuildMaterialsFn = void (*)(void* user, nri_scene::SceneView& sceneView, nri_scene::MaterialBridgeData& materials, const char* label);
	using PrewarmTextureFn = bool (*)(void* user, const nri_scene::TextureUpload& upload);

	void* user = nullptr;
	PumpAdmissionQueueFn pumpAdmissionQueue = nullptr;
	PumpComputeJobsFn pumpComputeJobs = nullptr;
	EnsureBatchFn ensureBatch = nullptr;
	WarmSharedBlasFn warmSharedBlas = nullptr;
	IsSubmitBudgetHitFn isSubmitBudgetHit = nullptr;
	BuildMaterialsFn buildMaterials = nullptr;
	PrewarmTextureFn prewarmTexture = nullptr;
	NRIPersistentVoxelMaterialClosureServices materialClosure;

	bool PumpAdmissionQueue(const char* phase) const;
	void PumpComputeJobs(uint32_t frameIndex) const;
	bool EnsureBatch(NRIPersistentVoxelBatchStats* outStats = nullptr) const;
	bool WarmSharedBlas(const std::vector<nri_scene::PrecachedVoxelVariantView>& variants, uint32_t frameIndex) const;
	bool IsSubmitBudgetHit() const;
	void BuildMaterials(nri_scene::SceneView& sceneView, nri_scene::MaterialBridgeData& materials, const char* label) const;
	bool PrewarmTexture(const nri_scene::TextureUpload& upload) const;
};

struct NRIPersistentVoxelAdmissionServices
{
	using AdmitVariantResourceFn = bool (*)(
		void* user,
		PersistentVoxelAdmissionEntry& entry,
		uint64_t byteBudget,
		uint32_t& blasBudget,
		uint64_t& outUploadBytes,
		bool& outReusedMesh,
		bool& outReusedMaterial,
		bool& outInProgress,
		bool isolateBlasBuild,
		const char*& outFailureReason,
		PersistentVoxelAdmissionStats* outStats);
	using SubmitWaitAndRestartFn = bool (*)(void* user, const char* reason);
	using IsSubmitBudgetHitFn = bool (*)(void* user);
	using GetRecordingCommandFenceValueFn = uint64_t (*)(void* user);
	using IsCommandFenceValueCompleteFn = bool (*)(void* user, uint64_t fenceValue);
	using RetireBufferFn = void (*)(void* user, NRIBufferResource& resource);
	using RetireAccelerationStructureFn = void (*)(void* user, NRIAccelerationStructureResource& resource);
	using BuildMaterialsFn = void (*)(void* user, nri_scene::SceneView& sceneView, nri_scene::MaterialBridgeData& materials, const char* label);
	using PrewarmTextureFn = bool (*)(void* user, const nri_scene::TextureUpload& upload);
	using AssignGeometryPortalIndicesFn = void (*)(void* user, nri_scene::GeometryData& geometry);
	using CreateStructuredBufferNoUploadFn = bool (*)(void* user, NRIBufferResource& resource, uint64_t size, uint32_t stride, nri::BufferUsageBits usage);
	using EnsureArenaBufferFn = bool (*)(void* user, NRIBufferResource& resource, uint64_t requiredSize, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after);
	using StageBufferCopyRangeFn = bool (*)(void* user, NRIBufferResource& resource, uint64_t byteOffset, const void* data, uint64_t size, nri::AccessStage after, int uploadKind);
	using NoteBufferUploadFn = void (*)(void* user, int uploadKind, uint64_t size, const char* reason);
	using BuildBottomLevelFn = bool (*)(
		void* user,
		const NRIBufferResource& vertexBuffer,
		const NRIBufferResource& indexBuffer,
		uint32_t vertexOffset,
		uint32_t vertexCount,
		uint32_t indexOffset,
		uint32_t indexCount,
		uint32_t primitiveCount,
		NRIAccelerationStructureResource& outAccelerationStructure,
		NRIBufferResource* buildScratchBuffer);
	using BarrierBuildInputsFn = bool (*)(void* user, const NRIBufferResource& vertexBuffer, const NRIBufferResource& indexBuffer);
	using BarrierComputeToBuildInputsFn = bool (*)(void* user, const NRIBufferResource& vertexBuffer, const NRIBufferResource& indexBuffer);

	void* user = nullptr;
	AdmitVariantResourceFn admitVariantResource = nullptr;
	SubmitWaitAndRestartFn submitWaitAndRestart = nullptr;
	IsSubmitBudgetHitFn isSubmitBudgetHit = nullptr;
	GetRecordingCommandFenceValueFn getRecordingCommandFenceValue = nullptr;
	IsCommandFenceValueCompleteFn isCommandFenceValueComplete = nullptr;
	RetireBufferFn retireBuffer = nullptr;
	RetireAccelerationStructureFn retireAccelerationStructure = nullptr;
	BuildMaterialsFn buildMaterials = nullptr;
	PrewarmTextureFn prewarmTexture = nullptr;
	NRIPersistentVoxelMaterialClosureServices materialClosure;
	AssignGeometryPortalIndicesFn assignGeometryPortalIndices = nullptr;
	CreateStructuredBufferNoUploadFn createStructuredBufferNoUpload = nullptr;
	EnsureArenaBufferFn ensureArenaBuffer = nullptr;
	StageBufferCopyRangeFn stageBufferCopyRange = nullptr;
	NoteBufferUploadFn noteBufferUpload = nullptr;
	BuildBottomLevelFn buildBottomLevel = nullptr;
	BarrierBuildInputsFn barrierBuildInputs = nullptr;
	BarrierComputeToBuildInputsFn barrierComputeToBuildInputs = nullptr;

	bool AdmitVariantResource(
		PersistentVoxelAdmissionEntry& entry,
		uint64_t byteBudget,
		uint32_t& blasBudget,
		uint64_t& outUploadBytes,
		bool& outReusedMesh,
		bool& outReusedMaterial,
		bool& outInProgress,
		bool isolateBlasBuild,
		const char*& outFailureReason,
		PersistentVoxelAdmissionStats* outStats) const;
	bool SubmitWaitAndRestart(const char* reason) const;
	bool IsSubmitBudgetHit() const;
	void RetireBuffer(NRIBufferResource& resource) const;
	void RetireAccelerationStructure(NRIAccelerationStructureResource& resource) const;
	void BuildMaterials(nri_scene::SceneView& sceneView, nri_scene::MaterialBridgeData& materials, const char* label) const;
	bool PrewarmTexture(const nri_scene::TextureUpload& upload) const;
	void AssignGeometryPortalIndices(nri_scene::GeometryData& geometry) const;
	bool CreateStructuredBufferNoUpload(NRIBufferResource& resource, uint64_t size, uint32_t stride, nri::BufferUsageBits usage) const;
	bool EnsureArenaBuffer(NRIBufferResource& resource, uint64_t requiredSize, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after) const;
	bool StageBufferCopyRange(NRIBufferResource& resource, uint64_t byteOffset, const void* data, uint64_t size, nri::AccessStage after, int uploadKind) const;
	void NoteBufferUpload(int uploadKind, uint64_t size, const char* reason) const;
	bool BuildBottomLevel(
		const NRIBufferResource& vertexBuffer,
		const NRIBufferResource& indexBuffer,
		uint32_t vertexOffset,
		uint32_t vertexCount,
		uint32_t indexOffset,
		uint32_t indexCount,
		uint32_t primitiveCount,
		NRIAccelerationStructureResource& outAccelerationStructure,
		NRIBufferResource* buildScratchBuffer = nullptr) const;
	uint64_t GetRecordingCommandFenceValue() const;
	bool IsCommandFenceValueComplete(uint64_t fenceValue) const;
	bool BarrierBuildInputs(const NRIBufferResource& vertexBuffer, const NRIBufferResource& indexBuffer) const;
	bool BarrierComputeToBuildInputs(const NRIBufferResource& vertexBuffer, const NRIBufferResource& indexBuffer) const;
};

struct NRIPersistentVoxelAccelerationBuildStats
{
	uint32_t calls = 0;
	uint32_t builds = 0;
	uint32_t uniqueMeshBuilds = 0;
	uint32_t instances = 0;
	NRIVoxelShadowProxyBuildStats shadowProxy;
};

struct NRIPersistentVoxelAccelerationServices
{
	using BuildBottomLevelFn = bool (*)(
		void* user,
		const NRIBufferResource& vertexBuffer,
		const NRIBufferResource& indexBuffer,
		uint32_t vertexOffset,
		uint32_t vertexCount,
		uint32_t indexOffset,
		uint32_t indexCount,
		uint32_t primitiveCount,
		NRIAccelerationStructureResource& outAccelerationStructure);
	using BarrierBuildInputsFn = bool (*)(void* user, const NRIBufferResource& vertexBuffer, const NRIBufferResource& indexBuffer);
	using EnsureStructuredBufferFn = bool (*)(void* user, NRIBufferResource& resource, const void* data, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after, const char* reason, int uploadKind);

	void* user = nullptr;
	BuildBottomLevelFn buildBottomLevel = nullptr;
	BarrierBuildInputsFn barrierBuildInputs = nullptr;
	EnsureStructuredBufferFn ensureStructuredBuffer = nullptr;

	bool BuildBottomLevel(
		const NRIBufferResource& vertexBuffer,
		const NRIBufferResource& indexBuffer,
		uint32_t vertexOffset,
		uint32_t vertexCount,
		uint32_t indexOffset,
		uint32_t indexCount,
		uint32_t primitiveCount,
		NRIAccelerationStructureResource& outAccelerationStructure) const;
	bool BarrierBuildInputs(const NRIBufferResource& vertexBuffer, const NRIBufferResource& indexBuffer) const;
	bool EnsureStructuredBuffer(NRIBufferResource& resource, const void* data, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after, const char* reason, int uploadKind) const;
};

struct NRIPersistentVoxelDescriptorSnapshot
{
	nri::Descriptor* vertex = nullptr;
	nri::Descriptor* index = nullptr;
	nri::Descriptor* primitive = nullptr;
	nri::Descriptor* material = nullptr;
	uint32_t primitiveCount = 0;
	uint32_t materialCount = 0;
};

struct NRIPersistentVoxelDestroyServices
{
	using DestroyBufferFn = void (*)(void* user, NRIBufferResource& resource);

	void* user = nullptr;
	DestroyBufferFn destroyBuffer = nullptr;

	void DestroyBuffer(NRIBufferResource& resource) const;
};

struct NRIPersistentVoxelLightAppendStats
{
	uint32_t activationAnchors = 0;
	uint32_t appendedActors = 0;
	uint32_t skippedActors = 0;
	uint32_t appendedRecords = 0;
	uint32_t skippedRecords = 0;
};

struct NRIPersistentVoxelMemoryUsage
{
	uint64_t sceneBufferBytes = 0;
	uint64_t accelerationStructureBytes = 0;
	uint64_t arenaVertexCommittedBytes = 0;
	uint64_t arenaIndexCommittedBytes = 0;
	uint64_t arenaPrimitiveCommittedBytes = 0;
	uint64_t arenaMaterialCommittedBytes = 0;
	uint64_t arenaVertexUsedBytes = 0;
	uint64_t arenaIndexUsedBytes = 0;
	uint64_t arenaPrimitiveUsedBytes = 0;
	uint64_t arenaMaterialUsedBytes = 0;
	uint64_t privateVertexBytes = 0;
	uint64_t privateIndexBytes = 0;
	uint64_t directBlasBytes = 0;
	uint64_t sharedBlasBytes = 0;
	uint64_t shadowProxyVertexBytes = 0;
	uint64_t shadowProxyIndexBytes = 0;
	uint64_t shadowProxyBlasBytes = 0;
	uint64_t materialLogicalBytes = 0;
	uint64_t admissionTransientBufferBytes = 0;
	uint64_t admissionTransientAsBytes = 0;
	uint64_t admissionCpuGeometryBytes = 0;
};

struct NRIPersistentVoxelStatusSnapshot
{
	uint32_t meshVariantResourceCount = 0;
	uint32_t materialVariantResourceCount = 0;
	uint32_t batchActorCount = 0;
	uint32_t instanceRecordCount = 0;
	uint32_t admissionQueueCount = 0;
	uint32_t pendingInstanceCount = 0;
	uint64_t residentResourceBytes = 0;
	uint64_t zeroRefResourceBytes = 0;
	uint32_t zeroRefMeshResourceCount = 0;
	uint32_t zeroRefMaterialResourceCount = 0;
	uint32_t shadowProxyResidentCount = 0;
	uint32_t shadowProxyFailedCount = 0;
	uint64_t shadowProxyPrimitiveCount = 0;
	uint64_t shadowProxyResidentBytes = 0;
	uint32_t activeInstanceCount = 0;
	uint32_t instancePrimitiveCount = 0;
	uint32_t instanceMaterialCount = 0;
	uint32_t instanceMinPrimitiveCount = 0;
	uint32_t instanceMaxPrimitiveCount = 0;
	uint32_t requiredAdmissionPendingCount = 0;
	uint32_t requiredAdmissionReadyCount = 0;
	uint32_t optionalAdmissionPendingCount = 0;
	uint32_t failedAdmissionCount = 0;
	uint32_t computeInFlightCount = 0;
	uint32_t blasInFlightCount = 0;
	uint64_t cpuGeometryBuildCount = 0;
	uint64_t cpuGeometryUploadCount = 0;
	uint64_t cpuGeometryUploadBytes = 0;
	uint64_t cpuGeometryFallbackCount = 0;
	uint32_t residencyGeneration = 0;
	uint64_t residencyBuildSerial = 0;
	uint32_t lastDesiredResidencyCount = 0;
	uint32_t lastDesiredPreloadCount = 0;
	uint32_t lastDesiredActorCount = 0;
	uint32_t lastCpuReadyCount = 0;
	uint32_t lastGpuReadyCount = 0;
	uint32_t lastRetainedCount = 0;
	uint32_t lastQueuedCount = 0;
	uint64_t lastQueuedUploadBytes = 0;
	uint32_t lastMeshReadyCount = 0;
	uint32_t lastMaterialReadyCount = 0;
	uint32_t lastBlasReadyCount = 0;
	uint32_t lastMeshMissingCount = 0;
	uint32_t lastMaterialOnlyCount = 0;
	uint32_t lastBlasOnlyCount = 0;
	uint32_t lastColdMeshCount = 0;
	uint32_t lastColdMaterialCount = 0;
	uint64_t lastColdPrimitiveCount = 0;
	uint32_t lastForcedCount = 0;
	uint32_t lastPreferredCount = 0;
};

struct NRIPersistentVoxelOverlayStats
{
	uint32_t actorCount = 0;
	uint32_t primitiveCount = 0;
	uint32_t materialCount = 0;
	uint32_t indexCount = 0;
	uint64_t byteCount = 0;
};

struct NRIPersistentVoxelMaintenanceStats
{
	uint32_t registryEntries = 0;
	uint32_t activeEntries = 0;
	uint32_t requiredReadyEntries = 0;
	uint32_t optionalReadyEntries = 0;
	uint32_t failedEntries = 0;
	uint32_t entriesScannedLastPump = 0;
	uint32_t pressureEntriesScannedLast = 0;
	uint32_t pressureResourceRowsScannedLast = 0;
	bool pumpFastReturnLast = false;
	bool pressureEvaluatedLast = false;
	bool pressureSkippedLast = false;
	bool pressureProtectionBlockedLast = false;
	uint32_t pressureEvaluationReasonMaskLast = 0;
	uint64_t pumpCalls = 0;
	uint64_t pumpFastReturns = 0;
	uint64_t entriesScanned = 0;
	uint64_t pressureEntriesScanned = 0;
	uint64_t pressureResourceRowsScanned = 0;
	uint64_t pressureEvaluations = 0;
	uint64_t pressureNoops = 0;
	uint64_t pressureSkips = 0;
	uint64_t pressureSafetyAudits = 0;
	uint64_t pressureMembershipChanges = 0;
	uint64_t pressureMembershipInvalidations = 0;
	uint64_t pressureProtectionBlockedEvaluations = 0;
	uint64_t memorySnapshotRebuilds = 0;
	uint64_t memorySnapshotHits = 0;
	uint64_t resourceStatusRebuilds = 0;
	uint64_t resourceStatusHits = 0;
};

struct NRIPersistentVoxelMaterialWarmupStats
{
	uint32_t textureRequests = 0;
	uint32_t textureHits = 0;
	uint32_t textureMisses = 0;
	uint32_t textureInserts = 0;
	uint64_t estimatedBytes = 0;
	double realizeMs = 0.0;
	bool pending = false;
	bool textureBudgetHit = false;
	bool byteBudgetHit = false;
	bool msBudgetHit = false;
};

struct NRIPersistentVoxelMaterialWarmupResult
{
	bool hasMaterials = false;
	bool paletteReady = true;
	uint32_t materialCount = 0;
	uint32_t variantResourceCount = 0;
	bool pending = false;
	NRIPersistentVoxelMaterialWarmupStats textureStats = {};
};

struct NRIPersistentVoxelMaterialWarmupServices
{
	using EnsurePaletteFn = bool (*)(void* user, const nri_scene::MaterialBridgeData& materials);
	using WarmTexturesFn = bool (*)(void* user, const nri_scene::MaterialBridgeData& materials, NRIPersistentVoxelMaterialWarmupStats& stats);

	void* user = nullptr;
	EnsurePaletteFn ensurePalette = nullptr;
	WarmTexturesFn warmTextures = nullptr;

	bool EnsurePalette(const nri_scene::MaterialBridgeData& materials) const;
	bool WarmTextures(const nri_scene::MaterialBridgeData& materials, NRIPersistentVoxelMaterialWarmupStats& stats) const;
};

struct NRIPersistentVoxelMaterialUploadStats
{
	uint32_t uploads = 0;
	uint32_t layoutInvalidatedResources = 0;
	uint32_t activeValidatedResources = 0;
	uint32_t activeHashMisses = 0;
	uint32_t actorMaterialRebinds = 0;
	uint32_t batchRejects = 0;
	uint64_t requestedBytes = 0;
	uint64_t dirtyBytes = 0;
	uint64_t uploadedBytes = 0;
	uint64_t batchGapBytes = 0;
	uint64_t domainPayloadBytes = 0;
	uint64_t domainMaterialPayloadBytes = 0;
	uint64_t domainUploadedBytes = 0;
	uint64_t domainMaterialUploadedBytes = 0;
	uint32_t domainHashChecks = 0;
	uint32_t domainHashMisses = 0;
};

struct NRIPersistentVoxelMaterialPreloadStats
{
	bool attempted = false;
	uint32_t variants = 0;
	uint32_t directOnlyVariants = 0;
	uint32_t candidates = 0;
	uint32_t uniqueMaterials = 0;
	uint32_t selected = 0;
	uint32_t reused = 0;
	uint32_t built = 0;
	uint32_t failed = 0;
	uint32_t skippedInvalid = 0;
	uint32_t skippedMaterialBudget = 0;
	uint32_t actorScopedMaterials = 0;
	uint32_t materialRows = 0;
	uint32_t textureRequests = 0;
	uint64_t materialBytes = 0;
	uint64_t textureBytes = 0;
	double buildMs = 0.0;
};

struct NRIPersistentVoxelBatchStats
{
	double persistentVoxelBatchCacheEntryMs = 0.0;
	double persistentVoxelBatchSortMs = 0.0;
	double persistentVoxelBatchInstanceSyncMs = 0.0;
	double persistentVoxelBatchExistingActorMapMs = 0.0;
	double persistentVoxelBatchActorLoopMs = 0.0;
	double persistentVoxelBatchMaterialVariantMs = 0.0;
	double persistentVoxelBatchMeshAdmissionMs = 0.0;
	double persistentVoxelBatchMaterialBridgeMs = 0.0;
	double persistentVoxelBatchStateMs = 0.0;
	double geometryBuildPersistentVoxelVariantMs = 0.0;
	double geometryBuildPersistentVoxelAppendMs = 0.0;
	double geometryBuildPersistentVoxelRebuildMs = 0.0;
	double persistentVoxelTexturePrewarmMs = 0.0;
	uint32_t geometryBuildPersistentVoxelVariantCalls = 0;
	uint32_t geometryBuildPersistentVoxelVariantPrimitives = 0;
	uint32_t persistentVoxelTexturePrewarmHitCount = 0;
	uint32_t persistentVoxelTexturePrewarmQueuedCount = 0;
	uint32_t persistentVoxelTexturePrewarmMissCount = 0;
	uint32_t persistentVoxelTexturePrewarmDeferredCount = 0;
	uint32_t persistentVoxelTexturePrewarmProcessedCount = 0;
	uint64_t persistentVoxelTexturePrewarmByteBudget = 0;
	uint64_t persistentVoxelTexturePrewarmEstimatedBytes = 0;
	uint64_t persistentVoxelTexturePrewarmDeferredBytes = 0;
	uint64_t persistentVoxelTexturePrewarmProcessedBytes = 0;
	uint32_t persistentVoxelOnboardingCandidateCount = 0;
	uint32_t persistentVoxelOnboardingDeferredCount = 0;
	uint32_t persistentVoxelOnboardingPrimitiveBudgetHits = 0;
	uint32_t persistentVoxelOnboardingByteBudgetHits = 0;
	uint32_t persistentVoxelOnboardingActorBudgetHits = 0;
	uint32_t persistentVoxelOnboardingAdmittedCount = 0;
	uint32_t persistentVoxelOnboardingTextureBudgetHits = 0;
	uint32_t persistentVoxelOnboardingAdmissionPendingCount = 0;
	uint32_t persistentVoxelOnboardingTexturePrewarmDeferredCount = 0;
	uint32_t persistentVoxelOnboardingMaterialInvalidCount = 0;
	uint32_t persistentVoxelOnboardingBudgetDeferredCount = 0;
	uint64_t persistentVoxelOnboardingEstimatedBytes = 0;
	uint64_t persistentVoxelOnboardingDeferredBytes = 0;
	uint64_t persistentVoxelOnboardingAdmittedBytes = 0;
	uint64_t persistentVoxelOnboardingByteBudget = 0;
	uint32_t persistentVoxelInstanceTransformUpdates = 0;
	uint32_t persistentVoxelBatchSerialFastPathCount = 0;
};

struct NRIPersistentVoxelBatchServices
{
	using BuildMaterialsFn = void (*)(void* user, nri_scene::SceneView& sceneView, nri_scene::MaterialBridgeData& materials, const char* label);
	using IsTextureCachedFn = bool (*)(void* user, const nri_scene::TextureUpload& upload);
	using PrewarmTextureFn = bool (*)(void* user, const nri_scene::TextureUpload& upload, double* outMs);
	using AssignGeometryPortalIndicesFn = void (*)(void* user, nri_scene::GeometryData& geometry);
	using EnsureStructuredBufferFn = bool (*)(void* user, NRIBufferResource& resource, const void* data, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after, const char* reason, int uploadKind);
	using EnsureArenaBufferFn = bool (*)(void* user, NRIBufferResource& resource, uint64_t requiredSize, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after);
	using StageBufferCopyRangeFn = bool (*)(void* user, NRIBufferResource& resource, uint64_t byteOffset, const void* data, uint64_t size, nri::AccessStage after, int uploadKind);
	using NoteBufferUploadFn = void (*)(void* user, int uploadKind, uint64_t size, const char* reason);
	using RetireAccelerationStructureFn = void (*)(void* user, NRIAccelerationStructureResource& resource);
	using MaterialWouldEmitFn = bool (*)(void* user, const nri_scene::MaterialLightingMetadata& metadata);
	using BuildSurfaceRecordFn = SceneLightSystem::SurfaceRecord (*)(
		void* user,
		const nri_scene::SurfaceRef& surface,
		const nri_scene::MaterialBridgeData& materials,
		SceneLightRecordSource source,
		uint32_t materialIndex,
		uint32_t primitiveIndex);

	void* user = nullptr;
	BuildMaterialsFn buildMaterials = nullptr;
	IsTextureCachedFn isTextureCached = nullptr;
	PrewarmTextureFn prewarmTexture = nullptr;
	AssignGeometryPortalIndicesFn assignGeometryPortalIndices = nullptr;
	EnsureStructuredBufferFn ensureStructuredBuffer = nullptr;
	EnsureArenaBufferFn ensureArenaBuffer = nullptr;
	StageBufferCopyRangeFn stageBufferCopyRange = nullptr;
	NoteBufferUploadFn noteBufferUpload = nullptr;
	RetireAccelerationStructureFn retireAccelerationStructure = nullptr;
	MaterialWouldEmitFn materialWouldEmit = nullptr;
	BuildSurfaceRecordFn buildSurfaceRecord = nullptr;
	NRIPersistentVoxelMaterialClosureServices materialClosure;

	void BuildMaterials(nri_scene::SceneView& sceneView, nri_scene::MaterialBridgeData& materials, const char* label) const;
	bool IsTextureCached(const nri_scene::TextureUpload& upload) const;
	bool PrewarmTexture(const nri_scene::TextureUpload& upload, double* outMs) const;
	void AssignGeometryPortalIndices(nri_scene::GeometryData& geometry) const;
	bool EnsureStructuredBuffer(NRIBufferResource& resource, const void* data, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after, const char* reason, int uploadKind) const;
	bool EnsureArenaBuffer(NRIBufferResource& resource, uint64_t requiredSize, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after) const;
	bool StageBufferCopyRange(NRIBufferResource& resource, uint64_t byteOffset, const void* data, uint64_t size, nri::AccessStage after, int uploadKind) const;
	void NoteBufferUpload(int uploadKind, uint64_t size, const char* reason) const;
	void RetireAccelerationStructure(NRIAccelerationStructureResource& resource) const;
	bool MaterialWouldEmit(const nri_scene::MaterialLightingMetadata& metadata) const;
	SceneLightSystem::SurfaceRecord BuildSurfaceRecord(
		const nri_scene::SurfaceRef& surface,
		const nri_scene::MaterialBridgeData& materials,
		SceneLightRecordSource source,
		uint32_t materialIndex,
		uint32_t primitiveIndex) const;
};

struct NRIPersistentVoxelMaterialUploadServices
{
	using EnsureMaterialArenaBufferFn = bool (*)(void* user, NRIBufferResource& resource, uint64_t sizeBytes);
	using StageMaterialRangesFn = bool (*)(
		void* user,
		const NRIBufferResource& targetBuffer,
		const std::vector<RuntimeMutationResidentUploadRange>& ranges,
		const uint8_t* data,
		uint64_t availableBytes);
	using NoteMaterialUploadFn = void (*)(void* user, uint64_t sizeBytes);

	void* user = nullptr;
	EnsureMaterialArenaBufferFn ensureMaterialArenaBuffer = nullptr;
	StageMaterialRangesFn stageMaterialRanges = nullptr;
	NoteMaterialUploadFn noteMaterialUpload = nullptr;

	bool EnsureMaterialArenaBuffer(NRIBufferResource& resource, uint64_t sizeBytes) const;
	bool StageMaterialRanges(
		const NRIBufferResource& targetBuffer,
		const std::vector<RuntimeMutationResidentUploadRange>& ranges,
		const uint8_t* data,
		uint64_t availableBytes) const;
	void NoteMaterialUpload(uint64_t sizeBytes) const;
};

struct NRIPersistentVoxelTlasServices
{
	using GetAccelerationStructureHandleFn = uint64_t (*)(void* user, const NRIAccelerationStructureResource& resource);
	using EvaluateRepresentationFn = NRIVoxelRepresentationDecision (*)(void* user, const NRIVoxelRepresentationFacts& facts);
	using HasFinalActorNoShadowCastIntentFn = bool (*)(void* user, int32_t actorIndex);

	void* user = nullptr;
	GetAccelerationStructureHandleFn getAccelerationStructureHandle = nullptr;
	EvaluateRepresentationFn evaluateRepresentation = nullptr;
	HasFinalActorNoShadowCastIntentFn hasFinalActorNoShadowCastIntent = nullptr;
	NRIActorOccurrenceTraceConfig occurrenceTrace;
	NRIActorOccurrencePolicyContext occurrencePolicy;
	const std::vector<nri_scene::MaterialData>* gpuMaterials = nullptr;
	bool actorWorkloadMaskDiagnosticsEnabled = false;
	uint32_t actorWorkloadMaskDiagnosticLimit = 0;

	uint64_t GetAccelerationStructureHandle(const NRIAccelerationStructureResource& resource) const;
	NRIVoxelRepresentationDecision EvaluateRepresentation(const NRIVoxelRepresentationFacts& facts) const;
	bool HasFinalActorNoShadowCastIntent(int32_t actorIndex) const;
};

struct NRIPersistentVoxelTlasBuildStats
{
	uint32_t sharedMeshResourceCount = 0;
	uint32_t instanceCount = 0;
	uint64_t instancePrimitiveCount = 0;
	uint32_t bakedFallbackInstanceCount = 0;
	uint32_t shadowProxyInstanceCount = 0;
	uint64_t shadowProxyPrimitiveCount = 0;
	uint64_t exactShadowPrimitiveCountRemoved = 0;
	uint32_t actorOccurrenceSuppressedCount = 0;
	uint32_t actorWorkloadMaskCandidateCount = 0;
	uint32_t actorWorkloadMaskIntentCount = 0;
	uint32_t actorWorkloadMaskCertifiedCount = 0;
	uint32_t actorWorkloadMaskDiagnosticRows = 0;
	uint64_t actorWorkloadMaskCandidatePrimitiveCount = 0;
	uint64_t actorWorkloadMaskCertifiedPrimitiveCount = 0;
	std::array<uint32_t, (uint32_t)NRIActorWorkloadMaskReason::Count> actorWorkloadMaskReasonCounts = {};
	std::vector<int32_t> suppressedActorIndices;
	NRIActorOccurrenceFrame occurrenceFrame;
};

struct NRIPersistentVoxelPreloadStatus
{
	bool gpuLoadingEnabled = false;
	bool hasCacheEntries = false;
	bool batchReady = true;
	uint32_t requiredPending = 0;
	uint32_t requiredReady = 0;
	uint32_t optionalPending = 0;
	uint32_t failed = 0;
	uint32_t batchReadyActors = 0;
	uint32_t batchPendingActors = 0;
	uint32_t deferredTexturePrewarm = 0;
	uint32_t deferredOnboarding = 0;
};

class NRIPersistentVoxelResidency
{
public:
	void Reset(const char* reason, bool clearSharedResources, bool traceReset, const NRIPersistentVoxelResetServices& services);
	void ResetLevelSchedulingState(const char* reason, bool traceReset, const NRIPersistentVoxelResetServices& services);
	bool CompactMaterialRangesForQuiescentLevelTransition(const char* reason, bool traceEnabled);
	bool SyncMapGeneration(uint64_t buildSerial, const char* reason, bool traceEnabled, const NRIPersistentVoxelResetServices& services);
	void ReconcileResidency(
		const std::vector<nri_scene::PrecachedVoxelVariantView>& variants,
		const std::vector<nri_scene::PersistentVoxelCacheEntryView>& cacheEntries,
		uint64_t buildSerial,
		const char* levelName,
		uint32_t frameIndex,
		const NRIPersistentVoxelSettings& settings,
		int loadingTraceLevel,
		const NRIPersistentVoxelResetServices& services);
	bool EnqueueAdmission(
		const nri_scene::PrecachedVoxelVariantView& variant,
		bool runtimeRequested,
		const char* sourceLabel,
		uint64_t buildSerial,
		const NRIPersistentVoxelSettings& settings,
		int loadingTraceLevel,
		bool voxelStatsEnabled,
		const NRIPersistentVoxelResetServices& services);
	bool PreloadVariantResources(
		const std::vector<nri_scene::PrecachedVoxelVariantView>& variants,
		uint64_t buildSerial,
		uint32_t frameIndex,
		const NRIPersistentVoxelSettings& settings,
		int loadingTraceLevel,
		bool voxelStatsEnabled,
		const NRIPersistentVoxelResetServices& resetServices,
		const NRIPersistentVoxelPreloadServices& preloadServices);
	bool PreSizeDirectGeometryArenas(
		uint64_t buildSerial,
		uint64_t uniqueGeometryBytes,
		uint64_t plannedRuntimeTailBytes,
		uint64_t largestKnownGeometryBytes,
		int loadingTraceLevel,
		const NRIPersistentVoxelAdmissionServices& services);
	bool PreloadResources(
		const std::vector<nri_scene::PrecachedVoxelVariantView>& variants,
		const std::vector<nri_scene::PersistentVoxelCacheEntryView>& cacheEntries,
		bool hasCacheEntries,
		bool gpuLoadingEnabled,
		bool preloadMaterialPayloads,
		uint32_t preloadMaterialMaxRows,
		uint64_t buildSerial,
		const char* levelName,
		uint32_t frameIndex,
		const NRIPersistentVoxelSettings& settings,
		int loadingTraceLevel,
		bool voxelStatsEnabled,
		const NRIPersistentVoxelResetServices& resetServices,
		const NRIPersistentVoxelPreloadServices& preloadServices);
	void ApplyPressurePolicy(
		const char* phase,
		uint32_t frameIndex,
		const NRIPersistentVoxelSettings& settings,
		uint64_t totalTrackedBytes,
		uint64_t adapterLocalBudget,
		bool traceEnabled,
		const NRIPersistentVoxelResetServices& services);
	void ArmPostLoadAdmissionGrace(uint32_t frameIndex, const NRIPersistentVoxelSettings& settings, int loadingTraceLevel);
	bool PumpAdmissionQueue(
		const char* phase,
		uint64_t buildSerial,
		uint32_t frameIndex,
		const NRIPersistentVoxelSettings& settings,
		uint64_t totalTrackedBytes,
		uint64_t adapterLocalBudget,
		int loadingTraceLevel,
		bool voxelStatsEnabled,
		const NRIPersistentVoxelResetServices& resetServices,
		const NRIPersistentVoxelAdmissionServices& admissionServices);
	bool AdmitVariantResource(
		PersistentVoxelAdmissionEntry& entry,
		uint64_t byteBudget,
		uint32_t& blasBudget,
		uint64_t& outUploadBytes,
		bool& outReusedMesh,
		bool& outReusedMaterial,
		bool& outInProgress,
		bool isolateBlasBuild,
		const char*& outFailureReason,
		PersistentVoxelAdmissionStats* outStats,
		uint32_t frameIndex,
		int loadingTraceLevel,
		bool voxelStatsEnabled,
		const NRIPersistentVoxelAdmissionServices& services);
	bool EnsureBatch(
		uint64_t buildSerial,
		uint32_t frameIndex,
		const NRIPersistentVoxelSettings& settings,
		int loadingTraceLevel,
		bool voxelStatsEnabled,
		const NRIPersistentVoxelResetServices& resetServices,
		const NRIPersistentVoxelBatchServices& batchServices,
		NRIPersistentVoxelBatchStats& outStats);
	bool BuildAccelerationStructures(
		uint32_t frameIndex,
		const NRIPersistentVoxelSettings& settings,
		bool voxelStatsEnabled,
		const NRIPersistentVoxelResetServices& resetServices,
		const NRIPersistentVoxelAccelerationServices& accelerationServices,
		NRIPersistentVoxelAccelerationBuildStats& outStats);
	bool WarmSharedBlasForLoading(
		const std::vector<nri_scene::PrecachedVoxelVariantView>& variants,
		uint32_t frameIndex,
		const NRIPersistentVoxelSettings& settings,
		int loadingTraceLevel,
		bool voxelStatsEnabled,
		const NRIPersistentVoxelResetServices& resetServices,
		const NRIPersistentVoxelAccelerationServices& accelerationServices);
	bool PreloadMaterialPayloads(
		const std::vector<nri_scene::PrecachedVoxelVariantView>& variants,
		uint64_t buildSerial,
		const char* levelName,
		uint32_t frameIndex,
		uint32_t maxMaterialRows,
		int loadingTraceLevel,
		bool voxelStatsEnabled,
		const NRIPersistentVoxelPreloadServices& preloadServices,
		NRIPersistentVoxelMaterialPreloadStats& outStats);
	NRIPersistentVoxelDescriptorSnapshot BuildDescriptorSnapshot(
		const NRIBufferResource& fallbackVertexBuffer,
		const NRIBufferResource& fallbackIndexBuffer,
		const NRIBufferResource& fallbackPrimitiveBuffer,
		const NRIBufferResource& fallbackMaterialBuffer) const;
	uint32_t BoundPrimitiveCount() const;
	uint32_t BoundMaterialCount() const;
	void DestroyArenaBuffers(const NRIPersistentVoxelDestroyServices& services);
	NRIPersistentVoxelLightAppendStats AppendSceneLights(SceneLightSystem& sceneLights, uint32_t frameIndex, bool voxelStatsEnabled) const;
	NRIPersistentVoxelMemoryUsage GetMemoryUsage() const;
	void CollectResidentAccelerationStructures(std::vector<NRIAccelerationStructureResource*>& outResources);
	const NRIPersistentVoxelSharedBlasFrameStats& GetSharedBlasFrameStats() const;
	NRIPersistentVoxelStatusSnapshot BuildStatusSnapshot() const;
	void FillResourceStatusSnapshot(NRIPersistentVoxelStatusSnapshot& snapshot) const;
	void FillBatchStatusSnapshot(NRIPersistentVoxelStatusSnapshot& snapshot) const;
	NRIPersistentVoxelOverlayStats BuildOverlayStats() const;
	const NRIPersistentVoxelMaintenanceStats& GetMaintenanceStats() const { return maintenanceStats; }
	bool HasValidBatch() const;
	bool HasRenderableOverlay() const;
	bool HasResidentIndirectOnlyActor(int32_t actorIndex) const;
	uint32_t EvaluateActorOccurrencePolicies(
		uint32_t frameIndex,
		const NRIActorOccurrencePolicyContext& context);
	const std::unordered_set<int32_t>* GetSuppressedActorIndices(uint32_t frameIndex) const;
	bool HasOverlayPreparationEligibleActor(const NRIPersistentVoxelSettings& settings) const;
	bool IsIndirectOnlyActorTlasAppendEligible(
		int32_t actorIndex,
		uint32_t frameIndex,
		const NRIPersistentVoxelSettings& settings,
		const NRIPersistentVoxelTlasServices& services) const;
	bool HasPreloadPending() const;
	NRIPersistentVoxelPreloadStatus BuildPreloadStatusSnapshot() const;
	uint32_t OverlayMaterialCount() const;
	const nri_scene::MaterialBridgeData& MaterialBridge() const { return batch.materialBridge; }
	uint64_t MaterialPublicationGeneration() const { return batchMaterialPublicationGeneration; }
	const NRIPersistentVoxelMaterialRangeStats& MaterialRangeStats() const { return materialRangeAllocator.Stats(); }
	uint32_t EstimatePrimitiveCountForInstanceOffset(uint32_t primitiveOffset) const;
	nri_scene::SceneDebugStats BuildOverlayDebugStats() const;
	uint64_t BuildSceneGenerationHash() const;
	void RebuildBatchMaterialBridge(PersistentVoxelBatch& targetBatch);
	bool PublishCanonicalMaterialResource(
		PersistentVoxelMaterialVariantResource& candidate,
		bool& outReused);
	void RecomputeBatchState(PersistentVoxelBatch& targetBatch) const;
	void RefreshActiveResourceReferences(uint32_t frameIndex);
	void ClearActorInstances(const NRIPersistentVoxelResetServices& services);
	bool ValidateActorGeometry(
		uint64_t identityKey,
		uint64_t surfaceSignature,
		const nri_scene::GeometryData& actorGeometry,
		uint32_t materialCount,
		uint32_t frameIndex,
		bool voxelStatsEnabled);
	void AppendMaterialBridgeTo(nri_scene::MaterialBridgeData& destination) const;
	bool WarmMaterialResources(
		const NRIPersistentVoxelMaterialWarmupServices& services,
		NRIPersistentVoxelMaterialWarmupResult& outResult) const;
	bool UploadArenaMaterialBuffers(
		const std::vector<nri_scene::MaterialData>& materials,
		const NRIPersistentVoxelMaterialUploadServices& services,
		uint32_t frameIndex,
		bool validateActiveMaterialPayloads,
		bool voxelStatsEnabled,
		NRIPersistentVoxelMaterialUploadStats& outStats);
	bool AppendTlasInstances(
		std::vector<nri::TopLevelInstance>& instances,
		std::vector<SceneInstanceData>& sceneInstances,
		uint32_t frameIndex,
		const NRIPersistentVoxelSettings& settings,
		const NRISceneInstanceVisibilityContext& visibilityContext,
		bool voxelStatsEnabled,
		const NRIPersistentVoxelTlasServices& services,
		NRIPersistentVoxelTlasBuildStats& outStats);
	void CommitWorldTlasFrame(uint32_t frameIndex);
	void DiscardAdmissionEntry(PersistentVoxelAdmissionEntry& entry, const NRIPersistentVoxelResetServices& services);
	PersistentVoxelReadinessStatus GetSharedVariantReadiness(uint64_t meshResourceKey, uint64_t materialKeyHash) const;
	bool AppendMaterialTextureKeys(uint64_t materialKeyHash, std::vector<uint64_t>& outKeys) const;
	bool IsSharedVariantReady(uint64_t meshResourceKey, uint64_t materialKeyHash) const;
	bool IsRequiredAdmission(const PersistentVoxelAdmissionEntry& entry) const;
	NRIPersistentVoxelAdmissionBucket GetAdmissionBucket(const PersistentVoxelAdmissionEntry& entry) const;
	void RebuildAdmissionIndex(bool reactivateStaleReady);
	void MarkMaintenanceMutation();
	void CountAdmissionWork(uint32_t& requiredPending, uint32_t& requiredReady, uint32_t& optionalPending, uint32_t& failed) const;
	void TraceReadiness(
		const char* event,
		const char* phase,
		const PersistentVoxelAdmissionEntry* entry,
		uint64_t meshResourceKey,
		uint64_t materialKeyHash,
		const PersistentVoxelReadinessStatus& status,
		bool traceEnabled) const;
	bool IsActorTlasAppendEligible(
		const PersistentVoxelBatch::ActorEntry& actor,
		uint32_t frameIndex,
		const NRIPersistentVoxelSettings& settings,
		const NRIPersistentVoxelTlasServices& services) const;
	bool IsActorOverlayPreparationEligible(
		const PersistentVoxelBatch::ActorEntry& actor,
		const NRIPersistentVoxelSettings& settings) const;

	bool IsPostLoadAdmissionGraceActive(uint32_t frameIndex) const;

	NRIBufferResource vertexBuffer;
	NRIBufferResource indexBuffer;
	NRIBufferResource primitiveBuffer;
	NRIBufferResource materialBuffer;
	PersistentVoxelBatch batch = {};
	std::unordered_map<uint64_t, PersistentVoxelMeshVariantResource> meshVariantResources;
	std::unordered_map<uint64_t, PersistentVoxelMaterialVariantResource> materialVariantResources;
	std::unordered_map<uint64_t, PersistentVoxelInstanceRecord> instances;
	std::unordered_map<uint64_t, uint64_t> actorRejectedSignatures;
	std::unordered_map<uint64_t, PersistentVoxelAdmissionEntry> admissionQueue;
	std::unordered_map<uint64_t, uint32_t> activeMeshReferences;
	std::unordered_map<uint64_t, uint32_t> activeMaterialReferences;
	uint32_t actorOccurrencePolicyFrameIndex = UINT32_MAX;
	std::unordered_map<int32_t, NRIActorOccurrencePolicyDecision> actorOccurrencePolicyDecisions;
	std::unordered_set<int32_t> suppressedActorIndices;
	NRIPersistentVoxelAdmissionIndex admissionIndex;
	uint64_t maintenanceMutationGeneration = 1;
	mutable uint64_t cachedMemoryUsageGeneration = 0;
	mutable NRIPersistentVoxelMemoryUsage cachedMemoryUsage = {};
	mutable uint64_t cachedResourceStatusGeneration = 0;
	mutable NRIPersistentVoxelStatusSnapshot cachedResourceStatus = {};
	uint64_t pressureEvaluationGeneration = 0;
	uint64_t pressureSettingsSignature = 0;
	uint64_t pressureAdapterBudget = 0;
	uint32_t pressureEvaluationFrame = 0;
	bool pressureEvaluationValid = false;
	bool pressureProtectionBlocked = false;
	mutable NRIPersistentVoxelMaintenanceStats maintenanceStats = {};
	std::unordered_set<uint64_t> publishedMeshKeys;
	std::unordered_set<uint64_t> publishedMaterialKeys;
	std::unordered_set<uint64_t> dirtyMaterialResourceKeys;
	NRIVoxelAdmissionScheduler admissionScheduler = NRIVoxelAdmissionScheduler(NRIVoxelAdmissionLimits{});
	NRIPersistentVoxelSharedBlasCache sharedBlasCache;
	uint32_t arenaVertexCursor = 0;
	uint32_t arenaIndexCursor = 0;
	uint32_t arenaPrimitiveCursor = 0;
	NRIPersistentVoxelMaterialRangeAllocator materialRangeAllocator;
	NRIPersistentVoxelMaterialUploadMirror materialUploadMirror;
	uint64_t arenaPresizeBuildSerial = 0;
	uint64_t blasPolicyTraceBuildSerial = 0;
	uint64_t materialResourceGeneration = 1;
	uint64_t batchMaterialResourceGeneration = 0;
	uint64_t batchMaterialPublicationGeneration = 1;
	uint64_t materialRangeCompactions = 0;
	uint64_t materialRangeCompactedRows = 0;
	uint64_t uploadedMaterialResourceGeneration = 0;
	uint64_t uploadedMaterialPublicationGeneration = 0;
	uint32_t committedWorldTlasFrameIndex = UINT32_MAX;
	NRIActorOccurrenceLedger actorOccurrenceLedger;
	uint32_t pendingMaterialLayoutInvalidatedResources = 0;
	uint32_t pendingMaterialActorRebinds = 0;
	std::vector<uint64_t> uploadedMaterialTextureKeys;
	uint32_t residencyMapGeneration = 0;
	uint64_t residencyLastBuildSerial = 0;
	uint32_t lastDesiredResidencyCount = 0;
	uint32_t lastDesiredPreloadCount = 0;
	uint32_t lastDesiredActorCount = 0;
	uint32_t lastCpuReadyCount = 0;
	uint32_t lastGpuReadyCount = 0;
	uint32_t lastRetainedCount = 0;
	uint32_t lastQueuedCount = 0;
	uint64_t lastQueuedUploadBytes = 0;
	uint32_t lastMeshReadyCount = 0;
	uint32_t lastMaterialReadyCount = 0;
	uint32_t lastBlasReadyCount = 0;
	uint32_t lastMeshMissingCount = 0;
	uint32_t lastMaterialOnlyCount = 0;
	uint32_t lastBlasOnlyCount = 0;
	uint32_t lastColdMeshCount = 0;
	uint32_t lastColdMaterialCount = 0;
	uint64_t lastColdPrimitiveCount = 0;
	uint32_t lastForcedCount = 0;
	uint32_t lastPreferredCount = 0;
	bool loadingWarmupActive = false;
	bool preloadPending = false;
	bool preloadMaterialClosureCached = false;
	uint64_t preloadMaterialClosureBuildSerial = 0;
	uint64_t preloadMaterialClosureSignature = 0;
	uint64_t preloadMaterialClosureCacheHits = 0;
	uint32_t postLoadAdmissionGraceEndFrame = 0;
	uint32_t postLoadAdmissionGraceMapGeneration = 0;
	NRIPersistentVoxelPreloadStatus lastPreloadStatus = {};
	uint64_t cumulativeCpuGeometryBuildCount = 0;
	uint64_t cumulativeCpuGeometryUploadCount = 0;
	uint64_t cumulativeCpuGeometryUploadBytes = 0;
	uint64_t cumulativeCpuGeometryFallbackCount = 0;
};

const char* GetPersistentVoxelBakeSpaceName(nri_scene::VoxelMeshBakeSpace bakeSpace);
void CopyPersistentVoxelInstanceTransform(const float source[12], std::array<float, 12>& target);
bool SamePersistentVoxelInstanceTransform(const std::array<float, 12>& left, const float right[12]);
void FillPersistentVoxelInstanceTransform(const float currentTranslation[3], const float bakedTranslation[3], std::array<float, 12>& target);
void FillPersistentVoxelActorInstanceTransform(const nri_scene::PersistentVoxelCacheEntryView& cacheEntry, const PersistentVoxelMeshVariantResource& meshResource, std::array<float, 12>& target);
uint64_t BuildPersistentVoxelVariantMeshResourceKey(const nri_scene::PrecachedVoxelVariantView& variant);
uint64_t EstimatePersistentVoxelActorUploadBytes(const nri_scene::PersistentVoxelCacheEntryView& cacheEntry);
bool IsPersistentVoxelMeshResourceTransformKeyed(const nri_scene::PersistentVoxelCacheEntryView& cacheEntry, const NRIPersistentVoxelSettings& settings);
uint64_t BuildPersistentVoxelMeshResourceKey(const nri_scene::PersistentVoxelCacheEntryView& cacheEntry, const NRIPersistentVoxelSettings& settings);
uint32_t ResolvePersistentVoxelActorVisibilityChunk(const nri_scene::PersistentVoxelCacheEntryView& cacheEntry);
