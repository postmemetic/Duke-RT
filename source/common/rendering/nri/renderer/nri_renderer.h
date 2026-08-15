#pragma once

#include "../nri_output.h"
#include "nri_debug_overlays.h"
#include "nri_debug_reporters.h"
#include "nri_descriptor_sets.h"
#include "nri_exposure.h"
#include "nri_frame_graph.h"
#include "nri_frame_resources.h"
#include "nri_indirect_radiance_cache.h"
#include "nri_nrd.h"
#include "nri_persistent_voxels.h"
#include "nri_pipeline_state.h"
#include "nri_renderer_context.h"
#include "nri_resources.h"
#include "nri_map_movers.h"
#include "nri_map_material_only_route.h"
#include "nri_map_mover_rigid_route.h"
#include "nri_map_mover_shadow.h"
#include "nri_se29_floor_deformer_route.h"
#include "nri_runtime_mutation.h"
#include "nri_runtime_space_link_state.h"
#include "nri_scene_data_frame_ring.h"
#include "nri_scene_frame_geometry.h"
#include "nri_surface_light_overlay.h"
#include "nri_scene_material_frame_cache.h"
#include "nri_scene_texture_frame_cache.h"
#include "nri_scene_textures.h"
#include "nri_shader_contracts.h"
#include "nri_sky_environment.h"
#include "nri_spatial_absence_gate.h"
#include "nri_spatial_absence_gpu_snapshot.h"
#include "nri_scene_lights.h"
#include "nri_surface_probe.h"
#include "nri_material_policy.h"
#include "nri_static_scene.h"
#include "nri_static_scene_diagnostics.h"
#include "nri_static_scene_geometry_upload.h"
#include "nri_trace_stats.h"
#include "nri_upscaler.h"
#include "nri_weapon_event_batch.h"
#include "nri_world_tlas_slots.h"
#include "../framegen/nri_framegen.h"

#include "../scene/nri_map_builder.h"
#include "../scene/nri_map_world.h"
#include "../scene/nri_geometry_bridge.h"
#include "../scene/nri_material_bridge.h"
#include "../scene/nri_scene_bridge.h"
#include "lightoverlay.h"

#include <chrono>
#include <cstdint>
#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class NRIRenderDevice;
class NRIPassDispatchContext;
class NRISmokeSystem;
struct MapRecord;
struct LevelTransitionInfo;
struct PathTracingActorSpriteTraceEvent;
struct PathTracingEmissiveLightEditTarget;
struct RenderSceneCompletionInputs;
struct RenderSceneDispatchInputs;
struct RenderSceneFrameBuildInputs;
struct RenderSceneFrameBuildResult;
struct RenderSceneHistorySnapshot;

struct NRIDirectionalLightState
{
	bool enabled = false;
	bool shadow = true;
	bool fromOverlay = false;
	uint32_t ruleId = 0;
	uint64_t stateHash = 0;
	float direction[3] = { 0.3f, 0.85f, -0.4f };
	float color[3] = { 1.0f, 1.0f, 1.0f };
	float angularSize = 0.03f;
};

enum class NRIPTNightVisionMode : uint32_t
{
	None = 0,
	Duke = 1
};

struct NRIPTNightVisionState
{
	NRIPTNightVisionMode mode = NRIPTNightVisionMode::None;
	bool viewEligible = false;
	bool enabled = false;
	float strength01 = 0.0f;
	float remainingSeconds = 0.0f;
};

class NRIRenderer
{
public:
	enum class MaterialBuildTraceSlot : uint32_t
	{
		DynamicLive = 0,
		SceneLightMergedDynamic,
		LocalPlayerReflection,
		DynamicWithPersistentEmissive,
		SceneLightMergedPersistent,
		CapturedScene,
		PersistentEmissiveCachePrune,
		PersistentEmissiveCacheRebuild,
		StaticMapAnimChunk,
		StaticMapChunk,
		RuntimeMutationChunk,
		ResidentRuntimeMutationChunk,
		ResidentRuntimeMutationChunkRecover,
		RuntimeSpaceLinkChunk,
		Unknown,
		Count,
	};

	using SceneBufferUploadDomain = NRISceneBufferUploadDomain;
	using SceneBufferUploadProducerStamp = NRISceneBufferUploadProducerStamp;
	using SceneBufferUploadDomainSpan = NRISceneBufferUploadDomainSpan;

	struct StateCommitDomainGenerations
	{
		uint64_t staticMap = 0;
		uint64_t runtimeMutation = 0;
		uint64_t dynamicActors = 0;
		uint64_t localPlayerReflection = 0;
		uint64_t persistentVoxels = 0;
		uint64_t materialBridge = 0;
		uint64_t textures = 0;
		uint64_t tlasInstances = 0;
		uint64_t sceneConstants = 0;
	};

	struct DynamicSceneFrameState
	{
		uint32_t spriteSurfaceCount = 0;
		uint32_t primitiveCount = 0;
		uint32_t materialCount = 0;
		uint32_t modelCount = 0;
		uint32_t unsupportedModelCount = 0;
		uint32_t localPlayerReflectionSurfaceCount = 0;
		uint32_t localPlayerReflectionPrimitiveCount = 0;
		uint32_t localPlayerReflectionMaterialCount = 0;
		uint32_t localPlayerReflectionModelCount = 0;
		uint32_t localPlayerReflectionUnsupportedModelCount = 0;
		uint32_t asBuildCount = 0;
	};

	struct PreloadMaterialStatus
	{
		bool hasStaticMaterials = false;
		bool hasVoxelMaterials = false;
		bool paletteReady = true;
		bool pending = false;
		bool failed = false;
		bool staticReady = true;
		bool voxelReady = true;
		bool submitBudgetHit = false;
		bool msBudgetHit = false;
		bool textureBudgetHit = false;
		bool byteBudgetHit = false;
		uint32_t staticMaterialCount = 0;
		uint32_t staticTexturesReady = 0;
		uint32_t staticTexturesPending = 0;
		uint32_t staticTexturesRealized = 0;
		uint64_t staticUploadBytes = 0;
		uint32_t voxelMaterialCount = 0;
		uint32_t voxelVariantResourceCount = 0;
		uint32_t voxelTexturesReady = 0;
		uint32_t voxelTexturesPending = 0;
		uint32_t voxelTexturesRealized = 0;
		uint64_t voxelUploadBytes = 0;
		uint32_t preloadSubmits = 0;
		uint32_t preloadSubmitLimit = 0;
		double realizeMs = 0.0;
	};

	struct MaterialBuildTraceEntry
	{
		uint32_t calls = 0;
		uint32_t overrideBuildCalls = 0;
		uint32_t actorOverlayRuleMapBuilds = 0;
		uint32_t actorOverlayRuleMapCacheHits = 0;
		uint32_t actorOverlayRuleMapCacheMisses = 0;
		uint32_t actorOverlayRuleCount = 0;
		uint32_t actorOverlayStampedSpriteSurfaces = 0;
		uint32_t actorOverlaySkippedNonSpriteSurfaces = 0;
		uint32_t actorOverlaySkippedNoActorSurfaces = 0;
		uint32_t fullbrightFlaggedSurfaces = 0;
		uint32_t materialCount = 0;
		uint32_t actorMaterialCount = 0;
		uint32_t textureCount = 0;
		uint32_t baseTextureCount = 0;
		uint32_t glowTextureCount = 0;
		uint32_t normalTextureCount = 0;
		uint32_t metallicTextureCount = 0;
		uint32_t roughnessTextureCount = 0;
		uint32_t emissiveTextureCount = 0;
		double overrideBuildMs = 0.0;
		double actorOverlayRuleBuildMs = 0.0;
		double actorOverlayStampMs = 0.0;
		double fullbrightFlagMs = 0.0;
		double materialBuildMs = 0.0;
		double actorOverrideApplyMs = 0.0;
	};

	static constexpr size_t MaterialBuildTraceSlotCount = (size_t)MaterialBuildTraceSlot::Count;
	static constexpr size_t SceneBufferUploadDomainCount = (size_t)SceneBufferUploadDomain::Count;
	static constexpr size_t RuntimeMutationTopTraceCount = ::RuntimeMutationTopTraceCount;
	static constexpr size_t RuntimeSectorDirtyTruthTraceCount = 8;
	static constexpr size_t RuntimeAnimatedChurnTraceCount = 4;
	static constexpr size_t RuntimeMaterialOnlyMismatchTraceCount = 8;
	static constexpr size_t RuntimeResidentBlasRecreateTraceCount = 8;
	static constexpr size_t RuntimeResidentBlasRefitRejectTraceCount = 8;
	static constexpr size_t RuntimeStructuralRebuildTraceCount = 8;
	static constexpr size_t RuntimeGeometryDirtyTraceCount = 8;
	static constexpr size_t RuntimeRecurringChunkTraceCount = 8;
	static constexpr size_t RuntimeInvisibleProofTraceCount = 8;

	using RuntimeMutationTraceAction = ::RuntimeMutationTraceAction;
	using RuntimeMutationTopTraceEntry = ::RuntimeMutationTopTraceEntry;

	struct RuntimeSectorDirtyTruthTraceEntry
	{
		enum class PreviousStateSource : uint8_t
		{
			None,
			Replacement,
			Resident,
		};

		bool valid = false;
		uint32_t score = 0;
		uint32_t chunkIndex = UINT32_MAX;
		int32_t sectorIndex = -1;
		uint32_t reasonMask = 0;
		PreviousStateSource previousStateSource = PreviousStateSource::None;
		bool forceTopology = false;
		bool baselineChanged = false;
		bool geometryChanged = false;
		bool materialChanged = false;
		uint32_t previousSurfaceCount = 0;
		uint32_t liveSurfaceCount = 0;
		uint32_t previousTriangleCount = 0;
		uint32_t liveTriangleCount = 0;
	};

	struct RuntimeAnimatedChurnTraceEntry
	{
		bool valid = false;
		uint32_t score = 0;
		uint32_t chunkIndex = UINT32_MAX;
		int32_t sectorIndex = -1;
		bool suppressed = false;
		uint32_t suppressionEmits = 0;
		uint32_t materialRefreshes = 0;
		uint32_t runtimeAttempts = 0;
		uint32_t residentApplies = 0;
		uint32_t syncSkips = 0;
	};

	struct RuntimeMaterialOnlyMismatchTraceEntry
	{
		bool valid = false;
		uint32_t score = 0;
		uint32_t chunkIndex = UINT32_MAX;
		int32_t sectorIndex = -1;
		bool refreshPath = false;
		uint32_t reasonMask = 0;
		uint32_t filteredSurfaceCount = 0;
		uint32_t filteredMaterialCount = 0;
		uint32_t residentMaterialCount = 0;
		uint32_t filteredWallCount = 0;
		uint32_t filteredFlatCount = 0;
		uint32_t residentWallCount = 0;
		uint32_t residentFlatCount = 0;
	};

	struct RuntimeResidentBlasRecreateTraceEntry
	{
		bool valid = false;
		uint32_t score = 0;
		uint32_t chunkIndex = UINT32_MAX;
		int32_t sectorIndex = -1;
		uint32_t reasonMask = 0;
		uint32_t fallbackMask = 0;
		uint32_t surfaceCount = 0;
		uint32_t triangleCount = 0;
		uint32_t materialCount = 0;
		bool forceTopology = false;
		bool recoveredEmpty = false;
		bool keptGeometrySlice = false;
		bool topologyChanged = false;
		bool hadAccelerationStructure = false;
	};

	struct RuntimeResidentBlasRefitRejectTraceEntry
	{
		bool valid = false;
		uint32_t score = 0;
		uint32_t chunkIndex = UINT32_MAX;
		int32_t sectorIndex = -1;
		uint32_t reasonMask = 0;
		uint32_t rejectMask = 0;
		uint32_t previousIndexCount = 0;
		uint32_t liveIndexCount = 0;
		uint32_t previousPrimitiveCount = 0;
		uint32_t livePrimitiveCount = 0;
		bool hadAccelerationStructure = false;
	};

	struct RuntimeStructuralRebuildTraceEntry
	{
		bool valid = false;
		uint32_t score = 0;
		uint32_t chunkIndex = UINT32_MAX;
		int32_t sectorIndex = -1;
		uint32_t reasonMask = 0;
		uint32_t triggerMask = 0;
		uint32_t surfaceCount = 0;
		uint32_t triangleCount = 0;
		uint32_t materialCount = 0;
		RuntimeMutationTraceAction action = RuntimeMutationTraceAction::None;
		bool materialOnly = false;
		bool sectorMaterialOnly = false;
		bool wallMaterialOnly = false;
		bool mixedMaterialOnly = false;
		bool geometryOrDirty = false;
	};

	struct RuntimeGeometryDirtyTraceEntry
	{
		bool valid = false;
		uint32_t score = 0;
		uint32_t chunkIndex = UINT32_MAX;
		int32_t sectorIndex = -1;
		uint32_t reasonMask = 0;
		uint32_t familyMask = 0;
		uint32_t previousWallCount = 0;
		uint32_t liveWallCount = 0;
		uint32_t previousFlatCount = 0;
		uint32_t liveFlatCount = 0;
		uint32_t previousTriangleCount = 0;
		uint32_t liveTriangleCount = 0;
		uint32_t previousMaterialCount = 0;
		uint32_t liveMaterialCount = 0;
		bool forceTopology = false;
		bool countChanged = false;
		bool wallsChanged = false;
		bool flatsChanged = false;
	};

	struct RuntimeRecurringChunkTraceEntry
	{
		bool valid = false;
		uint32_t score = 0;
		uint32_t chunkIndex = UINT32_MAX;
		int32_t sectorIndex = -1;
		uint32_t lastReasonMask = 0;
		uint32_t visitCount = 0;
		uint32_t uniqueStateCount = 0;
		uint32_t transitionCount = 0;
		uint32_t repeatedStateHitCount = 0;
		uint32_t abaRecurrenceCount = 0;
		uint32_t lastWallCount = 0;
		uint32_t lastFlatCount = 0;
		uint32_t lastTriangleCount = 0;
		uint32_t lastMaterialCount = 0;
		uint64_t previousStateSignature = 0;
		uint64_t lastStateSignature = 0;
	};

	struct RuntimeInvisibleProofTraceEntry
	{
		bool valid = false;
		uint32_t score = 0;
		uint32_t chunkIndex = UINT32_MAX;
		int32_t sectorIndex = -1;
		uint32_t reasonMask = 0;
		uint32_t sourceMask = 0;
		uint32_t previousSurfaceCount = 0;
		uint32_t previousTriangleCount = 0;
		uint32_t previousMaterialCount = 0;
		uint32_t residentPrimitiveCount = 0;
		uint32_t residentMaterialCount = 0;
		bool replacementValid = false;
		bool residentAuthoritative = false;
		bool residentAvailable = false;
		bool visibleFloor = false;
		bool visibleCeiling = false;
		bool exactSignatureCached = false;
		bool exactSignatureMatch = false;
		bool animatedMaterialMatch = false;
		bool excludeStaticChunk = false;
		bool staticAnimatedReplacement = false;
		bool hasAnimatedTextureCandidates = false;
		bool animatedRefreshSuppressed = false;
		bool hardwareCanvas = false;
		bool portalChunk = false;
		bool sectorLightingCandidate = false;
		bool safeResidentNoopCandidate = false;
	};

	struct PerfShellTraceStats
	{
		struct SceneBufferUploadDomainTraceEntry
		{
			uint64_t payloadBytes = 0;
			uint64_t vertexPayloadBytes = 0;
			uint64_t indexPayloadBytes = 0;
			uint64_t primitivePayloadBytes = 0;
			uint64_t materialPayloadBytes = 0;
			uint64_t uploadedBytes = 0;
			uint64_t primitiveUploadedBytes = 0;
			uint64_t materialUploadedBytes = 0;
			uint64_t growthRequestedBytes = 0;
			uint64_t growthAllocatedBytes = 0;
			uint64_t dirtyChangedBytes = 0;
			uint64_t dirtyUploadedBytes = 0;
			uint32_t hashChecks = 0;
			uint32_t hashMisses = 0;
			uint32_t stampChecks = 0;
			uint32_t stampMisses = 0;
			uint32_t growthEvents = 0;
			uint32_t dirtyRanges = 0;
			double waitMs = 0.0;
		};

		struct OverlayAppendSourceTraceEntry
		{
			uint64_t byteCount = 0;
			uint64_t vertexBytes = 0;
			uint64_t indexBytes = 0;
			uint64_t primitiveBytes = 0;
			uint64_t materialBytes = 0;
			uint32_t vertexCount = 0;
			uint32_t indexCount = 0;
			uint32_t primitiveCount = 0;
			uint32_t materialCount = 0;
			uint32_t geometryGrowthEvents = 0;
			uint32_t materialGrowthEvents = 0;
		};

		double totalMs = 0.0;
		double initResourcesMs = 0.0;
		double mapWorldMs = 0.0;
		double updateStateMs = 0.0;
		double sceneSelectMs = 0.0;
		double sceneLightsMs = 0.0;
		double residentLightRefreshMs = 0.0;
		double emissiveUpdateMs = 0.0;
		double emissiveTlasMs = 0.0;
		double surfaceProbeMs = 0.0;
		double frameGraphMs = 0.0;
		double traceOpaqueMs = 0.0;
		double traceOpaqueReadbackMs = 0.0;
		double traceOpaqueCommandMs = 0.0;
		double traceOpaqueStatsCopyMs = 0.0;
		double postFrameDiagnosticsMs = 0.0;
		double unattributedMs = 0.0;
		double otherMs = 0.0;
		double staticSceneMs = 0.0;
		double runtimeMutationMs = 0.0;
		double runtimeMutationDiscoveryMs = 0.0;
		double runtimeMutationBudgetMs = 0.0;
		double runtimeMutationAnalyzeMs = 0.0;
		double runtimeMutationCommitMs = 0.0;
		uint32_t runtimeMutationCandidateChunks = 0;
		uint32_t runtimeMutationAnalyzedChunks = 0;
		uint32_t runtimeMutationBackgroundSweepChunks = 0;
		double runtimeMutationRebuildMs = 0.0;
		double runtimeMutationStructuralRebuildMs = 0.0;
		double runtimeMutationMaterialRefreshMs = 0.0;
		double runtimeMutationResidentApplyMs = 0.0;
		double runtimeMutationStructuralRebuildVisibleMs = 0.0;
		double runtimeMutationStructuralRebuildInvisibleMs = 0.0;
		double runtimeMutationStructuralRebuildNearMs = 0.0;
		double runtimeMutationStructuralRebuildFarMs = 0.0;
		double runtimeMutationStructuralRebuildUnknownDistanceMs = 0.0;
		double runtimeMutationMaterialRefreshVisibleMs = 0.0;
		double runtimeMutationMaterialRefreshInvisibleMs = 0.0;
		double runtimeMutationMaterialRefreshNearMs = 0.0;
		double runtimeMutationMaterialRefreshFarMs = 0.0;
		double runtimeMutationMaterialRefreshUnknownDistanceMs = 0.0;
		double runtimeMutationResidentApplyVisibleMs = 0.0;
		double runtimeMutationResidentApplyInvisibleMs = 0.0;
		double runtimeMutationResidentApplyNearMs = 0.0;
		double runtimeMutationResidentApplyFarMs = 0.0;
		double runtimeMutationResidentApplyUnknownDistanceMs = 0.0;
		float runtimeMutationNearDistance = 0.0f;
		double runtimeMutationResidentApplyLiveBuildMs = 0.0;
		double runtimeMutationResidentApplyGeometryBuildMs = 0.0;
		double runtimeMutationResidentApplyMaterialBuildMs = 0.0;
		double runtimeMutationResidentApplyBaselineCaptureMs = 0.0;
		double runtimeMutationResidentApplyAtlasMs = 0.0;
		double runtimeMutationResidentApplyAtlasBookkeepingMs = 0.0;
		double runtimeMutationResidentApplyVertexIndexCopyMs = 0.0;
		double runtimeMutationResidentApplyVertexCpuCopyMs = 0.0;
		double runtimeMutationResidentApplyIndexCpuCopyMs = 0.0;
		double runtimeMutationResidentApplyVertexStageMs = 0.0;
		double runtimeMutationResidentApplyIndexStageMs = 0.0;
		double runtimeMutationResidentApplyPrimitiveRewriteMs = 0.0;
		double runtimeMutationResidentApplyPrimitiveCpuRewriteMs = 0.0;
		double runtimeMutationResidentApplyPrimitiveStageMs = 0.0;
		double runtimeMutationResidentApplyGeometryOrderHashMs = 0.0;
		double runtimeMutationResidentApplyDownstreamBlasMs = 0.0;
		double runtimeMutationResidentApplyDownstreamBlasSetupMs = 0.0;
		double runtimeMutationResidentApplyDownstreamBlasFilterMs = 0.0;
		double runtimeMutationResidentApplyDownstreamBlasCreateMs = 0.0;
		double runtimeMutationResidentApplyDownstreamBlasScratchMs = 0.0;
		double runtimeMutationResidentApplyDownstreamBlasBarrierMs = 0.0;
		double runtimeMutationResidentApplyDownstreamBlasBuildMs = 0.0;
		double runtimeMutationAppendMs = 0.0;
		double sceneLightStaticAppendMs = 0.0;
		double sceneLightRuntimeMutationAppendMs = 0.0;
		double sceneLightCapturedAppendMs = 0.0;
		double sceneLightDynamicAppendMs = 0.0;
		double sceneLightPersistentVoxelAppendMs = 0.0;
		double sceneLightAnalyticMs = 0.0;
		double sceneLightEmissiveMs = 0.0;
		double sceneLightSectorMs = 0.0;
		double runtimeSpaceLinkMs = 0.0;
		double runtimeDebugSphereMs = 0.0;
		double runtimeDebugSphereViewMs = 0.0;
		double runtimeDebugSphereGeoMs = 0.0;
		double runtimeDebugSphereMaterialMs = 0.0;
		double runtimeDebugSphereTuneMs = 0.0;
		double overlayAssembleMs = 0.0;
		double overlayAppendMs = 0.0;
		double overlayAppendResetMs = 0.0;
		double overlayAppendSourcesMs = 0.0;
		double overlayAppendProducerStampMs = 0.0;
		double overlayAppendDynamicStampMs = 0.0;
		double overlayAppendLocalPlayerReflectionStampMs = 0.0;
		double overlayAppendBookkeepingMs = 0.0;
		double overlayRuntimeSpaceLinkMs = 0.0;
		double overlayRuntimeSpaceLinkGeometryMs = 0.0;
		double overlayRuntimeSpaceLinkMaterialMs = 0.0;
		double overlayRuntimeMutationMs = 0.0;
		double overlayRuntimeMutationGeometryMs = 0.0;
		double overlayRuntimeMutationMaterialMs = 0.0;
		double overlayDynamicMs = 0.0;
		double overlayDynamicGeometryMs = 0.0;
		double overlayDynamicMaterialMs = 0.0;
		double overlayLocalPlayerReflectionMs = 0.0;
		double overlayLocalPlayerReflectionGeometryMs = 0.0;
		double overlayLocalPlayerReflectionMaterialMs = 0.0;
		double overlayDebugSphereMs = 0.0;
		double overlayDebugSphereGeometryMs = 0.0;
		double overlayDebugSphereMaterialMs = 0.0;
		double dynamicCaptureMs = 0.0;
		double localPlayerReflectionCaptureMs = 0.0;
		double localPlayerReflectionGeometryBuildMs = 0.0;
		double localPlayerReflectionGeometryBuildWallMs = 0.0;
		double localPlayerReflectionGeometryBuildFlatMs = 0.0;
		double localPlayerReflectionGeometryBuildSpriteMs = 0.0;
		double localPlayerReflectionPortalAssignMs = 0.0;
		double localPlayerReflectionMaterialBuildMs = 0.0;
		double sceneSelectStaticMapMs = 0.0;
		double sceneSelectPersistentVoxelBatchMs = 0.0;
		double sceneSelectPersistentVoxelAdmissionPumpMs = 0.0;
		double persistentVoxelBatchCacheEntryMs = 0.0;
		double persistentVoxelBatchSortMs = 0.0;
		double persistentVoxelBatchInstanceSyncMs = 0.0;
		double persistentVoxelBatchExistingActorMapMs = 0.0;
		double persistentVoxelBatchActorLoopMs = 0.0;
		double persistentVoxelBatchMaterialVariantMs = 0.0;
		double persistentVoxelBatchMeshAdmissionMs = 0.0;
		double persistentVoxelBatchMaterialBridgeMs = 0.0;
		double persistentVoxelBatchStateMs = 0.0;
		uint32_t persistentVoxelPressureReason = 0;
		uint32_t persistentVoxelPressureFlags = 0;
		uint32_t persistentVoxelPressureAdmissionRows = 0;
		uint32_t persistentVoxelPressureResourceRows = 0;
		double sceneSelectPersistentEmissiveMs = 0.0;
		double sceneSelectDynamicMergeMs = 0.0;
		double sceneSelectDynamicMergeCopyMs = 0.0;
		double sceneSelectDynamicMergeAppendMs = 0.0;
		double sceneSelectDynamicMergeStatsMs = 0.0;
		double sceneSelectDynamicMergeGeometryMs = 0.0;
		double sceneSelectDynamicMergePortalAssignMs = 0.0;
		double sceneSelectDynamicMergeMaterialMs = 0.0;
		double sceneSelectLightMergeMs = 0.0;
		double sceneSelectStaticInstancesMs = 0.0;
		double sceneSelectMaterialBridgeMs = 0.0;
		uint32_t sceneMaterialResidentRebuilds = 0;
		uint32_t sceneMaterialResidentHits = 0;
		uint32_t sceneMaterialStaticRowsCopied = 0;
		uint32_t sceneMaterialPersistentRowsAppended = 0;
		uint32_t sceneMaterialOverlayRowsAppended = 0;
		uint32_t sceneMaterialResidentRowsReused = 0;
		double sceneSelectPaletteMs = 0.0;
		double sceneSelectTexturesMs = 0.0;
		double sceneSelectMaterialSplitMs = 0.0;
		double sceneSelectBufferUploadMs = 0.0;
		double sceneSelectBufferUploadPrimitiveRewriteMs = 0.0;
		double sceneSelectBufferUploadPrimitiveRewritePrimitiveHashMs = 0.0;
		double sceneSelectBufferUploadPrimitiveRewriteProvenanceHashMs = 0.0;
		double sceneSelectBufferUploadPrimitiveRewriteVisibilityHashMs = 0.0;
		double sceneSelectBufferUploadPrimitiveRewriteCopyMs = 0.0;
		double sceneSelectBufferUploadPrimitiveRewriteResolveMs = 0.0;
		double sceneSelectBufferUploadPrimitiveRewriteStoreMs = 0.0;
		double sceneSelectBufferUploadPayloadHashMs = 0.0;
		double sceneSelectBufferUploadDirtyRangeMs = 0.0;
		double sceneSelectBufferUploadWaitCheckMs = 0.0;
		double sceneSelectBufferUploadWaitMs = 0.0;
		double sceneSelectBufferUploadVertexMs = 0.0;
		double sceneSelectBufferUploadIndexMs = 0.0;
		double sceneSelectBufferUploadPrimitiveMs = 0.0;
		double sceneSelectBufferUploadMaterialMs = 0.0;
		double sceneSelectBufferUploadPersistentVoxelMaterialMs = 0.0;
		uint32_t sceneSelectBufferUploadWaitCount = 0;
		uint32_t sceneSelectBufferUploadVertexGrowEvents = 0;
		uint32_t sceneSelectBufferUploadIndexGrowEvents = 0;
		uint32_t sceneSelectBufferUploadPrimitiveGrowEvents = 0;
		uint32_t sceneSelectBufferUploadMaterialGrowEvents = 0;
		uint32_t sceneSelectBufferUploadGrowthEvents = 0;
		uint32_t sceneSelectBufferUploadVertexOverwriteEvents = 0;
		uint32_t sceneSelectBufferUploadIndexOverwriteEvents = 0;
		uint32_t sceneSelectBufferUploadPrimitiveOverwriteEvents = 0;
		uint32_t sceneSelectBufferUploadMaterialOverwriteEvents = 0;
		uint32_t sceneSelectBufferUploadPersistentVoxelMaterialUploads = 0;
		uint32_t sceneSelectBufferUploadPersistentVoxelMaterialBatches = 0;
		uint32_t sceneSelectBufferUploadPersistentVoxelMaterialBatchRanges = 0;
		uint32_t sceneSelectBufferUploadPersistentVoxelMaterialBatchRejects = 0;
		uint32_t sceneSelectBufferUploadPersistentVoxelMaterialBatchCopyCommands = 0;
		uint32_t sceneSelectBufferUploadPersistentVoxelMaterialBatchBarrierCommands = 0;
		uint32_t sceneSelectBufferUploadPayloadHashChecks = 0;
		uint32_t sceneSelectBufferUploadPayloadHashHits = 0;
		uint32_t sceneSelectBufferUploadPayloadHashSkips = 0;
		uint32_t sceneSelectBufferUploadPayloadHashMisses = 0;
		uint32_t sceneSelectBufferUploadPayloadHashUploads = 0;
		uint32_t sceneSelectBufferUploadPayloadHashRejectMissing = 0;
		uint32_t sceneSelectBufferUploadPayloadHashRejectSize = 0;
		uint32_t sceneSelectBufferUploadPayloadHashRejectStride = 0;
		uint32_t sceneSelectBufferUploadPayloadHashRejectForced = 0;
		uint32_t sceneSelectBufferUploadPayloadHashVertexHits = 0;
		uint32_t sceneSelectBufferUploadPayloadHashIndexHits = 0;
		uint32_t sceneSelectBufferUploadPayloadHashPrimitiveHits = 0;
		uint32_t sceneSelectBufferUploadPayloadHashMaterialHits = 0;
		uint32_t sceneSelectBufferUploadPayloadHashVertexSkips = 0;
		uint32_t sceneSelectBufferUploadPayloadHashIndexSkips = 0;
		uint32_t sceneSelectBufferUploadPayloadHashPrimitiveSkips = 0;
		uint32_t sceneSelectBufferUploadPayloadHashMaterialSkips = 0;
		uint32_t sceneSelectBufferUploadPayloadHashVertexMisses = 0;
		uint32_t sceneSelectBufferUploadPayloadHashIndexMisses = 0;
		uint32_t sceneSelectBufferUploadPayloadHashPrimitiveMisses = 0;
		uint32_t sceneSelectBufferUploadPayloadHashMaterialMisses = 0;
		uint32_t sceneSelectBufferUploadProducerStampChecks = 0;
		uint32_t sceneSelectBufferUploadProducerStampUses = 0;
		uint32_t sceneSelectBufferUploadProducerStampFallbacks = 0;
		uint32_t sceneSelectBufferUploadProducerStampRewritePrimitiveUses = 0;
		uint32_t sceneSelectBufferUploadProducerStampRewriteProvenanceUses = 0;
		uint32_t sceneSelectBufferUploadProducerStampVertexUses = 0;
		uint32_t sceneSelectBufferUploadProducerStampIndexUses = 0;
		uint32_t sceneSelectBufferUploadProducerStampPrimitiveUses = 0;
		uint32_t sceneSelectBufferUploadProducerStampMaterialUses = 0;
		uint32_t sceneSelectBufferUploadProducerStampFallbackSpans = 0;
		uint32_t sceneSelectBufferUploadProducerStampCoverageRejects = 0;
		uint32_t sceneSelectBufferUploadIdentityValidationChecks = 0;
		uint32_t sceneSelectBufferUploadIdentityValidationMismatches = 0;
		uint32_t sceneSelectBufferUploadVisibilityIdentityCacheHits = 0;
		uint32_t sceneSelectBufferUploadVisibilityIdentityCacheBuilds = 0;
		uint32_t sceneSelectBufferUploadVisibilityIdentityValidationChecks = 0;
		uint32_t sceneSelectBufferUploadVisibilityIdentityValidationMismatches = 0;
		uint32_t sceneSelectBufferUploadDirtyRangeChecks = 0;
		uint32_t sceneSelectBufferUploadDirtyRangeSkips = 0;
		uint32_t sceneSelectBufferUploadDirtyRangeForcedFull = 0;
		uint32_t sceneSelectBufferUploadDirtyRangeMissingMirror = 0;
		uint32_t sceneSelectBufferUploadDirtyRangeSizeMismatch = 0;
		uint32_t sceneSelectBufferUploadDirtyRangeSourceFull = 0;
		uint32_t sceneSelectBufferUploadDirtyRangeSourceByteScan = 0;
		uint32_t sceneSelectBufferUploadDirtyRangeSourceTyped = 0;
		uint32_t sceneSelectBufferUploadDirtyRangeRawRanges = 0;
		uint32_t sceneSelectBufferUploadDirtyRangeCoalescedRanges = 0;
		uint32_t sceneSelectBufferUploadDirtyRangeRejectedCoalesces = 0;
		uint32_t sceneSelectBufferUploadVertexDirtyRanges = 0;
		uint32_t sceneSelectBufferUploadIndexDirtyRanges = 0;
		uint32_t sceneSelectBufferUploadPrimitiveDirtyRanges = 0;
		uint32_t sceneSelectBufferUploadMaterialDirtyRanges = 0;
		uint32_t sceneSelectBufferUploadRangeUploads = 0;
		uint32_t sceneSelectBufferUploadRangeFallbacks = 0;
		uint32_t sceneSelectBufferUploadRangeFallbackFragmented = 0;
		uint32_t sceneSelectBufferUploadRangeFallbackLarge = 0;
		uint32_t sceneSelectBufferUploadPrimitiveRangeUploads = 0;
		uint32_t sceneSelectBufferUploadMaterialRangeUploads = 0;
		uint32_t sceneSelectBufferUploadVertexRangeUploads = 0;
		uint32_t sceneSelectBufferUploadIndexRangeUploads = 0;
		uint32_t sceneSelectBufferUploadPrimitiveRewriteCacheChecks = 0;
		uint32_t sceneSelectBufferUploadPrimitiveRewriteCacheHits = 0;
		uint32_t sceneSelectBufferUploadPrimitiveRewriteCacheMisses = 0;
		uint32_t sceneSelectBufferUploadPrimitiveRewriteCacheRejectInvalid = 0;
		uint32_t sceneSelectBufferUploadPrimitiveRewriteCacheRejectPrimitive = 0;
		uint32_t sceneSelectBufferUploadPrimitiveRewriteCacheRejectProvenance = 0;
		uint32_t sceneSelectBufferUploadPrimitiveRewriteCacheRejectVisibility = 0;
		uint32_t sceneSelectBufferUploadPrimitiveRewriteCacheRejectCount = 0;
		uint32_t sceneSelectBufferUploadPrimitiveRewriteResolvePrimitives = 0;
		uint32_t sceneSelectBufferUploadPrimitiveRewriteResolveMapChunk = 0;
		uint32_t sceneSelectBufferUploadPrimitiveRewriteResolveSectorFallback = 0;
		uint32_t sceneSelectBufferUploadPrimitiveRewriteResolveSectorMiss = 0;
		uint64_t sceneSelectBufferUploadVertexRequestedBytes = 0;
		uint64_t sceneSelectBufferUploadIndexRequestedBytes = 0;
		uint64_t sceneSelectBufferUploadPrimitiveRequestedBytes = 0;
		uint64_t sceneSelectBufferUploadMaterialRequestedBytes = 0;
		uint64_t sceneSelectBufferUploadPersistentVoxelMaterialRequestedBytes = 0;
		uint64_t sceneSelectBufferUploadPersistentVoxelMaterialDirtyBytes = 0;
		uint64_t sceneSelectBufferUploadVertexUploadedBytes = 0;
		uint64_t sceneSelectBufferUploadIndexUploadedBytes = 0;
		uint64_t sceneSelectBufferUploadPrimitiveUploadedBytes = 0;
		uint64_t sceneSelectBufferUploadMaterialUploadedBytes = 0;
		uint64_t sceneSelectBufferUploadPersistentVoxelMaterialUploadedBytes = 0;
		uint64_t sceneSelectBufferUploadPersistentVoxelMaterialBatchGapBytes = 0;
		uint64_t sceneSelectBufferUploadGrowthOldBytes = 0;
		uint64_t sceneSelectBufferUploadGrowthRequestedBytes = 0;
		uint64_t sceneSelectBufferUploadGrowthAllocatedBytes = 0;
		uint64_t sceneSelectBufferUploadGrowthHeadroomBytes = 0;
		uint64_t sceneSelectBufferUploadDirtyRangeChangedBytes = 0;
		uint64_t sceneSelectBufferUploadDirtyRangeUploadedBytes = 0;
		uint64_t sceneSelectBufferUploadDirtyRangeGapBytes = 0;
		uint64_t sceneSelectBufferUploadVertexDirtyChangedBytes = 0;
		uint64_t sceneSelectBufferUploadIndexDirtyChangedBytes = 0;
		uint64_t sceneSelectBufferUploadPrimitiveDirtyChangedBytes = 0;
		uint64_t sceneSelectBufferUploadMaterialDirtyChangedBytes = 0;
		uint64_t sceneSelectBufferUploadVertexDirtyUploadedBytes = 0;
		uint64_t sceneSelectBufferUploadIndexDirtyUploadedBytes = 0;
		uint64_t sceneSelectBufferUploadPrimitiveDirtyUploadedBytes = 0;
		uint64_t sceneSelectBufferUploadMaterialDirtyUploadedBytes = 0;
		uint64_t sceneSelectBufferUploadRangeUploadedBytes = 0;
		uint64_t sceneSelectBufferUploadProducerStampStampedBytes = 0;
		uint64_t sceneSelectBufferUploadProducerStampFallbackBytes = 0;
		std::array<SceneBufferUploadDomainTraceEntry, SceneBufferUploadDomainCount> sceneSelectBufferUploadDomains = {};
		double sceneSelectInstanceHandlesMs = 0.0;
		double sceneSelectTexturePrepMs = 0.0;
		double sceneSelectSurfaceLightMs = 0.0;
		double sceneSelectStateCommitMs = 0.0;
		double sceneSelectStateCommitFlagsMs = 0.0;
		double sceneSelectStateCommitDynamicStateMs = 0.0;
		double sceneSelectStateCommitGeometryStateMs = 0.0;
		double sceneSelectStateCommitStatsMs = 0.0;
		double sceneSelectStateCommitDynamicCoreMs = 0.0;
		double sceneSelectStateCommitDynamicLocalPlayerReflectionMs = 0.0;
		double sceneSelectStateCommitGeometrySelectMs = 0.0;
		double sceneSelectStateCommitGeometryStaticCopyMs = 0.0;
		double sceneSelectStateCommitGeometryAppendMs = 0.0;
		double sceneSelectStateCommitStatsBaseMs = 0.0;
		double sceneSelectStateCommitStatsPersistentVoxelMs = 0.0;
		double sceneSelectStateCommitStatsLocalPlayerReflectionMs = 0.0;
		double sceneSelectStateCommitStatsMergeMs = 0.0;
		uint32_t sceneSelectStateCommitSelectedDynamic = 0;
		uint32_t sceneSelectStateCommitActiveDynamic = 0;
		uint32_t sceneSelectStateCommitLocalPlayerReflection = 0;
		uint32_t sceneSelectStateCommitGeometryCombined = 0;
		uint32_t sceneSelectStateCommitGeometryStaticOnly = 0;
		uint32_t sceneSelectStateCommitStatsPersistentVoxel = 0;
		uint32_t sceneSelectStateCommitStatsLocalPlayerReflection = 0;
		uint32_t sceneSelectStateCommitCombinedPrimitiveCount = 0;
		uint32_t sceneSelectStateCommitCombinedMaterialCount = 0;
		uint64_t sceneSelectStateCommitGenStaticMap = 0;
		uint64_t sceneSelectStateCommitGenRuntimeMutation = 0;
		uint64_t sceneSelectStateCommitGenDynamicActors = 0;
		uint64_t sceneSelectStateCommitGenLocalPlayerReflection = 0;
		uint64_t sceneSelectStateCommitGenPersistentVoxels = 0;
		uint64_t sceneSelectStateCommitGenMaterialBridge = 0;
		uint64_t sceneSelectStateCommitGenTextures = 0;
		uint64_t sceneSelectStateCommitGenTlasInstances = 0;
		uint64_t sceneSelectStateCommitGenSceneConstants = 0;
		uint32_t sceneSelectStateCommitChangedStaticMap = 0;
		uint32_t sceneSelectStateCommitChangedRuntimeMutation = 0;
		uint32_t sceneSelectStateCommitChangedDynamicActors = 0;
		uint32_t sceneSelectStateCommitChangedLocalPlayerReflection = 0;
		uint32_t sceneSelectStateCommitChangedPersistentVoxels = 0;
		uint32_t sceneSelectStateCommitChangedMaterialBridge = 0;
		uint32_t sceneSelectStateCommitChangedTextures = 0;
		uint32_t sceneSelectStateCommitChangedTlasInstances = 0;
		uint32_t sceneSelectStateCommitChangedSceneConstants = 0;
		uint32_t sceneSelectStateCommitChangedDomainCount = 0;
		uint64_t sceneReusePresentationGeneration = 0;
		uint64_t sceneReuseSimulationGeneration = 0;
		uint64_t sceneReuseEngineGeneration = 0;
		uint64_t sceneReuseMapBuildSerial = 0;
		uint32_t sceneReuseTicksExecuted = 0;
		uint32_t sceneReuseZeroTickCandidate = 0;
		uint32_t sceneReuseSurfaceLightCalled = 0;
		uint32_t sceneReuseSurfaceLightHit = 0;
		uint32_t sceneReuseSurfaceLightCandidateHit = 0;
		uint32_t sceneReuseSurfaceLightBuild = 0;
		uint32_t sceneReuseSurfaceLightReject = 0;
		uint32_t sceneReuseSurfaceLightValidationChecked = 0;
		uint32_t sceneReuseSurfaceLightValidationMismatch = 0;
		uint64_t sceneReuseSurfaceLightKey = 0;
		uint32_t sceneReuseTextureCalled = 0;
		uint32_t sceneReuseTextureHit = 0;
		uint32_t sceneReuseTextureCandidateHit = 0;
		uint32_t sceneReuseTextureBuild = 0;
		uint32_t sceneReuseTextureReject = 0;
		uint32_t sceneReuseTextureValidationChecked = 0;
		uint32_t sceneReuseTextureValidationMismatch = 0;
		uint32_t sceneReuseTextureDynamicCount = 0;
		uint32_t sceneReuseTextureMissReasonMask = 0;
		uint64_t sceneReuseTextureKey = 0;
		double dynamicCaptureCountMs = 0.0;
		double dynamicCaptureWallsMs = 0.0;
		double dynamicCaptureFlatsMs = 0.0;
		double dynamicCaptureFacingSpritesMs = 0.0;
		double dynamicCaptureModelSpritesMs = 0.0;
		double dynamicCaptureModelClassifyMs = 0.0;
		double dynamicCaptureModelMeshMs = 0.0;
		double dynamicCaptureModelMeshBuildMs = 0.0;
		double dynamicCaptureModelSurfaceMs = 0.0;
		double dynamicCaptureModelSortMs = 0.0;
		double dynamicCaptureModelStoreMs = 0.0;
		double dynamicCaptureVoxelFrameMs = 0.0;
		double dynamicCaptureVoxelLifecycleMs = 0.0;
		double dynamicCaptureVoxelLiveEnumerationMs = 0.0;
		double dynamicCaptureVoxelReconcileMs = 0.0;
		double dynamicCaptureVoxelDuplicationAuditMs = 0.0;
		double dynamicCaptureStatsMs = 0.0;
		double persistentDynamicMs = 0.0;
		double dynamicAsMs = 0.0;
		double dynamicAsSetupMs = 0.0;
		double dynamicAsCreateMs = 0.0;
		double dynamicAsScratchMs = 0.0;
		double dynamicAsBuildMs = 0.0;
		double dynamicAsBarrierMs = 0.0;
		double persistentVoxelAsMs = 0.0;
		double persistentVoxelTlasInstanceMs = 0.0;
		double worldTlasMs = 0.0;
		double sceneDataSetMs = 0.0;
		double sceneDataSetWaitCheckMs = 0.0;
		double sceneDataSetWaitMs = 0.0;
		double sceneDataSetWaitEventMs[2] = {};
		double sceneDataSetReprojectionMs = 0.0;
		double sceneDataSetVisibleFlatPlaneMs = 0.0;
		double sceneDataSetVisibleChunkMs = 0.0;
		double sceneDataSetSceneInstanceMs = 0.0;
		double sceneDataSetPortalMs = 0.0;
		double sceneDataSetRuntimeLightHashMs = 0.0;
		double sceneDataSetRuntimeLightUploadMs = 0.0;
		double sceneDataSetRuntimeLightClusterMs = 0.0;
		double sceneDataSetEmissiveMs = 0.0;
		double sceneDataSetSectorLightMs = 0.0;
		double sceneDataSetDescriptorBuildMs = 0.0;
		double sceneDataSetDescriptorValidateMs = 0.0;
		double sceneDataSetDescriptorUpdateMs = 0.0;
		double sceneDataSetDescriptorHashMs = 0.0;
		uint32_t sceneDataSetFrameSlot = 0;
		uint32_t sceneDataSetFrameSlotCount = 0;
		uint32_t sceneDataSetFrameSlotEnabled = 0;
		uint32_t sceneDataSetFrameSlotFallbacks = 0;
		uint32_t sceneDataSetFrameSlotOverCap = 0;
		uint32_t sceneDataSetFrameSlotWaits = 0;
		uint32_t sceneDataSetFrameSlotGrows = 0;
		uint64_t sceneDataSetFrameSlotUsedBytes = 0;
		uint64_t sceneDataSetFrameSlotCapacityBytes = 0;
		uint64_t sceneDataSetFrameRingCapacityBytes = 0;
		uint64_t sceneDataSetFrameRingHighWaterBytes = 0;
		uint32_t sceneDataSetWaitCount = 0;
		const char* sceneDataSetWaitEventReason[2] = {};
		const char* sceneDataSetWaitEventBuffer[2] = {};
		uint32_t sceneDataSetDescriptorUpdateCount = 0;
		uint32_t sceneDataSetDescriptorNullCount = 0;
		uint32_t sceneDataSetDeferredDescriptorUpdateCount = 0;
		uint32_t sceneDataSetRuntimeLightUploads = 0;
		uint32_t sceneDataSetRuntimeLightCacheHits = 0;
		uint32_t sceneDataSetRuntimeLightClusterUploads = 0;
		uint32_t sceneDataSetRuntimeLightClusterCacheHits = 0;
		uint32_t sceneDataSetEmissiveUploads = 0;
		uint32_t sceneDataSetEmissiveCacheHits = 0;
		uint32_t sceneDataSetSectorLightUploads = 0;
		uint32_t sceneDataSetSectorLightCacheHits = 0;
		uint32_t sceneDataSetResourceGrowEvents = 0;
		uint32_t sceneDataSetResourceOverwriteEvents = 0;
		double sceneDataPreGrowMs = 0.0;
		double sceneDataPreGrowWaitMs = 0.0;
		uint32_t sceneDataPreGrowCalls = 0;
		uint32_t sceneDataPreGrowResourceGrowEvents = 0;
		uint64_t sceneDataPreGrowRequestedBytes = 0;
		uint64_t sceneDataPreGrowAllocatedBytes = 0;
		uint64_t sceneDataSetSceneInstanceRequestedBytes = 0;
		uint64_t sceneDataSetSceneInstanceUploadedBytes = 0;
		uint64_t sceneDataSetPortalRequestedBytes = 0;
		uint64_t sceneDataSetPortalUploadedBytes = 0;
		uint64_t sceneDataSetRuntimeLightRequestedBytes = 0;
		uint64_t sceneDataSetRuntimeLightUploadedBytes = 0;
		uint64_t sceneDataSetRuntimeLightClusterRequestedBytes = 0;
		uint64_t sceneDataSetRuntimeLightClusterUploadedBytes = 0;
		uint64_t sceneDataSetEmissiveRequestedBytes = 0;
		uint64_t sceneDataSetEmissiveUploadedBytes = 0;
		uint64_t sceneDataSetSectorLightRequestedBytes = 0;
		uint64_t sceneDataSetSectorLightUploadedBytes = 0;
		uint64_t sceneDataSnapshotGeneration = 0;
		uint64_t sceneDataDescriptorGeneration = 0;
		uint64_t sceneDataSceneInstanceHash = 0;
		uint64_t sceneDataTlasInstanceHash = 0;
		uint64_t sceneDataPortalHash = 0;
		uint32_t sceneDataSnapshotMismatchCount = 0;
		uint32_t sceneDataSceneInstanceCount = 0;
		uint32_t sceneDataTlasInstanceCount = 0;
		uint32_t sceneDataPortalCount = 0;
		double restoreStaticSceneMs = 0.0;
		double copyFinalMs = 0.0;
		double sceneTextureLookupMs = 0.0;
		double sceneTextureRealizeMs = 0.0;
		double sceneTextureDescriptorMs = 0.0;
		uint32_t sceneTextureDescriptorWrites = 0;
		uint32_t sceneTextureDescriptorSkips = 0;
		uint32_t sceneTextureDescriptorRowsWritten = 0;
		uint32_t sceneTextureStableSlotMode = 0;
		uint32_t sceneTextureSlotsLive = 0;
		uint32_t sceneTextureSlotsQuarantined = 0;
		uint32_t sceneTextureSlotsFree = 0;
		uint64_t sceneTextureSlotReuses = 0;
		uint64_t sceneTextureSlotExhaustions = 0;
		uint32_t sceneTextureStableDescriptorHits = 0;
		uint32_t sceneTextureStableDescriptorMisses = 0;
		double sceneTextureTransitionMs = 0.0;
		double actorOverrideMapBuildMs = 0.0;
		double materialBuildMs = 0.0;
		double geometryBuildDynamicLiveMs = 0.0;
		double geometryBuildLocalPlayerReflectionMs = 0.0;
		double geometryBuildMergedDynamicMs = 0.0;
		double geometryBuildCapturedMs = 0.0;
		double geometryBuildPersistentVoxelVariantMs = 0.0;
		double geometryBuildPersistentVoxelAppendMs = 0.0;
		double geometryBuildPersistentVoxelRebuildMs = 0.0;
		double geometryBuildPersistentEmissivePruneMs = 0.0;
		double geometryBuildPersistentEmissiveRebuildMs = 0.0;
		double geometryBuildStaticChunkMs = 0.0;
		double geometryBuildDebugSphereMs = 0.0;
		double geometryBuildRuntimeMutationTruthMs = 0.0;
		double geometryBuildRuntimeMutationRebuildMs = 0.0;
		double runtimeMutationGeometryBridgeMs = 0.0;
		double runtimeMutationPortalAssignMs = 0.0;
		double runtimeMutationDeformerCanonicalMs = 0.0;
		double geometryBuildRuntimeMutationMaterialOnlyMs = 0.0;
		double geometryBuildRuntimeSpaceLinkMs = 0.0;
		double geometryBuildResidentApplyMs = 0.0;
		double geometryBuildResidentRecoverMs = 0.0;
		double sceneInstanceStatsMs = 0.0;
		double persistentVoxelResourceStatsMs = 0.0;
		double persistentVoxelBatchStatsMs = 0.0;
		uint32_t runtimeMutationDirtyChunks = 0;
		uint32_t runtimeMutationRebuiltChunks = 0;
		uint32_t runtimeMutationHeldChunks = 0;
		uint32_t runtimeMutationStructuralRebuildChunks = 0;
		uint32_t runtimeMutationMaterialRefreshChunks = 0;
		uint32_t runtimeMutationCandidateVisibleChunks = 0;
		uint32_t runtimeMutationCandidateInvisibleChunks = 0;
		uint32_t runtimeMutationCandidateNearChunks = 0;
		uint32_t runtimeMutationCandidateFarChunks = 0;
		uint32_t runtimeMutationCandidateUnknownDistanceChunks = 0;
		uint32_t runtimeMutationCandidateBoundsValidChunks = 0;
		uint32_t runtimeMutationCandidateBoundsInvalidChunks = 0;
		uint32_t runtimeMutationCandidateActiveReplacementChunks = 0;
		uint32_t runtimeMutationCandidateVisibleResidentValidationChunks = 0;
		uint32_t runtimeMutationCandidateStartupVisibleValidationChunks = 0;
		uint32_t runtimeMutationCandidateUnresolvedTextureChunks = 0;
		uint32_t runtimeMutationCandidateStaticAnimatedSuppressedChunks = 0;
		uint32_t runtimeMutationCandidateSectorDirtyChunks = 0;
		uint32_t runtimeMutationCandidateSectionDirtyChunks = 0;
		uint32_t runtimeMutationCandidateDraggedChunks = 0;
		uint32_t runtimeMutationCandidateSignatureWatchChunks = 0;
		uint32_t runtimeMutationCandidateBackgroundSweepSourceChunks = 0;
		uint32_t runtimeMutationCandidateDeferredMaterialChunks = 0;
		uint32_t runtimeMutationCandidateDeferredStructuralChunks = 0;
		uint32_t runtimeMutationDirtyVisibleChunks = 0;
		uint32_t runtimeMutationDirtyInvisibleChunks = 0;
		uint32_t runtimeMutationDirtyNearChunks = 0;
		uint32_t runtimeMutationDirtyFarChunks = 0;
		uint32_t runtimeMutationDirtyUnknownDistanceChunks = 0;
		uint32_t runtimeMutationDirtyActiveReplacementChunks = 0;
		uint32_t runtimeMutationDirtyBackgroundSweepChunks = 0;
		uint32_t runtimeMutationStructuralRebuildVisibleChunks = 0;
		uint32_t runtimeMutationStructuralRebuildInvisibleChunks = 0;
		uint32_t runtimeMutationStructuralRebuildNearChunks = 0;
		uint32_t runtimeMutationStructuralRebuildFarChunks = 0;
		uint32_t runtimeMutationStructuralRebuildUnknownDistanceChunks = 0;
		uint32_t runtimeMutationStructuralRebuildActiveReplacementChunks = 0;
		uint32_t runtimeMutationStructuralRebuildBackgroundSweepChunks = 0;
		uint32_t runtimeMutationStructuralRebuildDeferredChunks = 0;
		uint32_t runtimeMutationStructuralRebuildDeferredCoalescedChunks = 0;
		uint32_t runtimeMutationStructuralRebuildDeferredFlushedChunks = 0;
		uint32_t runtimeMutationStructuralRebuildDeferredPromotedChunks = 0;
		uint32_t runtimeMutationStructuralRebuildDeferredPendingChunks = 0;
		uint32_t runtimeMutationStructuralRebuildDeferredBudget = 0;
		uint32_t runtimeMutationStructuralRebuildDeferredNearChunks = 0;
		uint32_t runtimeMutationStructuralRebuildDeferredNearCoalescedChunks = 0;
		uint32_t runtimeMutationStructuralRebuildDeferredNearFlushedChunks = 0;
		uint32_t runtimeMutationStructuralRebuildDeferredNearPendingChunks = 0;
		uint32_t runtimeMutationStructuralRebuildDeferredNearBudget = 0;
		uint32_t runtimeMutationStructuralRebuildDeferredFarChunks = 0;
		uint32_t runtimeMutationStructuralRebuildDeferredFarCoalescedChunks = 0;
		uint32_t runtimeMutationStructuralRebuildDeferredFarFlushedChunks = 0;
		uint32_t runtimeMutationStructuralRebuildDeferredFarPendingChunks = 0;
		uint32_t runtimeMutationStructuralRebuildDeferredFarBudget = 0;
		uint32_t runtimeMutationMaterialRefreshVisibleChunks = 0;
		uint32_t runtimeMutationMaterialRefreshInvisibleChunks = 0;
		uint32_t runtimeMutationMaterialRefreshNearChunks = 0;
		uint32_t runtimeMutationMaterialRefreshFarChunks = 0;
		uint32_t runtimeMutationMaterialRefreshUnknownDistanceChunks = 0;
		uint32_t runtimeMutationMaterialRefreshDeferredChunks = 0;
		uint32_t runtimeMutationMaterialRefreshDeferredCoalescedChunks = 0;
		uint32_t runtimeMutationMaterialRefreshDeferredFlushedChunks = 0;
		uint32_t runtimeMutationMaterialRefreshDeferredPendingChunks = 0;
		uint32_t runtimeMutationMaterialRefreshDeferredNearChunks = 0;
		uint32_t runtimeMutationMaterialRefreshDeferredNearCoalescedChunks = 0;
		uint32_t runtimeMutationMaterialRefreshDeferredNearFlushedChunks = 0;
		uint32_t runtimeMutationMaterialRefreshDeferredNearPendingChunks = 0;
		uint32_t runtimeMutationMaterialRefreshDeferredNearBudget = 0;
		uint32_t runtimeMutationMaterialRefreshActiveReplacementChunks = 0;
		uint32_t runtimeMutationMaterialRefreshBackgroundSweepChunks = 0;
		uint32_t runtimeMutationResidentApplyVisibleChunks = 0;
		uint32_t runtimeMutationResidentApplyInvisibleChunks = 0;
		uint32_t runtimeMutationResidentApplyNearChunks = 0;
		uint32_t runtimeMutationResidentApplyFarChunks = 0;
		uint32_t runtimeMutationResidentApplyUnknownDistanceChunks = 0;
		uint32_t runtimeMutationResidentApplyActiveReplacementChunks = 0;
		uint32_t runtimeMutationResidentApplyBackgroundSweepChunks = 0;
		uint32_t runtimeMutationMaterialRefreshAnimatedChunks = 0;
		uint32_t runtimeMutationMaterialRefreshReplacementDeltaChunks = 0;
		uint32_t runtimeMutationMaterialRefreshHardwareCanvasChunks = 0;
		uint32_t runtimeMutationStructuralReplacementDeltaChunks = 0;
		uint32_t runtimeMutationStructuralReplacementViewChangedChunks = 0;
		uint32_t runtimeMutationStructuralStaticAnimatedModeFlipChunks = 0;
		uint32_t runtimeMutationStructuralExcludeStaticFlipChunks = 0;
		uint32_t runtimeMutationStructuralForcedTopologyChunks = 0;
		uint32_t runtimeMutationStructuralInvalidChunks = 0;
		uint32_t runtimeMutationStructuralMaterialOnlyChunks = 0;
		uint32_t runtimeMutationStructuralSectorMaterialOnlyChunks = 0;
		uint32_t runtimeMutationStructuralWallMaterialOnlyChunks = 0;
		uint32_t runtimeMutationStructuralMixedMaterialOnlyChunks = 0;
		uint32_t runtimeMutationStructuralGeometryOrDirtyChunks = 0;
		uint32_t runtimeMutationGeometryDirtySectorGeometryOnlyChunks = 0;
		uint32_t runtimeMutationGeometryDirtyWallGeometryOnlyChunks = 0;
		uint32_t runtimeMutationGeometryDirtySectorWallGeometryChunks = 0;
		uint32_t runtimeMutationGeometryDirtyDirtyOnlyChunks = 0;
		uint32_t runtimeMutationGeometryDirtyGeometryDirtyMixedChunks = 0;
		uint32_t runtimeMutationGeometryDirtyForceTopologyOnlyChunks = 0;
		uint32_t runtimeMutationGeometryDirtyRealCountChangeChunks = 0;
		uint32_t runtimeMutationGeometryDirtyWallsOnlyChangedChunks = 0;
		uint32_t runtimeMutationGeometryDirtyFlatsOnlyChangedChunks = 0;
		uint32_t runtimeMutationGeometryDirtyWallsAndFlatsChangedChunks = 0;
		uint32_t runtimeRecurringChunkTrackedCount = 0;
		uint32_t runtimeRecurringChunkRecurringCount = 0;
		uint32_t runtimeRecurringChunkVisitCount = 0;
		uint32_t runtimeRecurringChunkUniqueStateCount = 0;
		uint32_t runtimeRecurringChunkTransitionCount = 0;
		uint32_t runtimeRecurringChunkRepeatedStateHitCount = 0;
		uint32_t runtimeRecurringChunkAbaRecurrenceCount = 0;
		uint32_t runtimeRecurringChunkMaxUniqueStateCount = 0;
		uint32_t runtimeMutationHardwareCanvasChunkCount = 0;
		uint32_t runtimeMutationStructuralReplacementDeltaReasonMaskOr = 0;
		uint32_t runtimeMutationMaterialRefreshReasonMaskOr = 0;
		uint32_t runtimeMutationInvalidForceTopologyCount = 0;
		uint32_t runtimeMutationForceTopologyProofChecks = 0;
		uint32_t runtimeMutationForceTopologyDowngradeCount = 0;
		uint32_t runtimeMutationForceTopologyDowngradeNoopCount = 0;
		uint32_t runtimeMutationForceTopologyDowngradeMaterialOnlyCount = 0;
		uint32_t runtimeMutationForceTopologyDowngradePrepareFailedCount = 0;
		uint32_t runtimeMutationForceTopologyDowngradeNoReplacementCount = 0;
		uint32_t runtimeMutationForceTopologyDowngradeGeometryChangedCount = 0;
		uint32_t runtimeMutationForceTopologyDowngradeUnsafeReasonCount = 0;
		uint32_t runtimeMutationForceTopologyDowngradeInvisibleCount = 0;
		uint32_t runtimeMutationForceTopologyDowngradeReasonMismatchCount = 0;
		uint32_t runtimeMutationInvalidAppliedCount = 0;
		uint32_t runtimeMutationResidentNoopSkipCount = 0;
		uint32_t runtimeMutationInvalidFailedCount = 0;
		uint32_t runtimeMutationInvalidSyncSkipCount = 0;
		uint32_t runtimeMutationResidentNoopCandidateCount = 0;
		uint32_t runtimeMutationResidentNoopCandidateReasonMaskOr = 0;
		uint32_t runtimeMutationResidentNoopBlockNotAuthoritativeCount = 0;
		uint32_t runtimeMutationResidentNoopBlockResidentUnavailableCount = 0;
		uint32_t runtimeMutationResidentNoopBlockReplacementInvalidCount = 0;
		uint32_t runtimeMutationResidentNoopBlockExcludeStaticCount = 0;
		uint32_t runtimeMutationResidentNoopBlockSurfaceCountMismatch = 0;
		uint32_t runtimeMutationResidentNoopBlockMaterialCountMismatch = 0;
		uint32_t runtimeMutationResidentNoopBlockPrimitiveCountMismatch = 0;
		uint32_t runtimeMutationValidMaterialCount = 0;
		uint32_t runtimeMutationValidStructuralCount = 0;
		uint32_t runtimeMutationResidentApplyCount = 0;
		uint32_t runtimeMutationResidentApplyMaterialOnlyCount = 0;
		uint32_t runtimeMutationResidentApplyStructuralCount = 0;
		uint32_t runtimeMutationResidentApplyFastMaterialOnlyCount = 0;
		uint32_t runtimeMutationResidentApplyCertifiedMaterialOnlyCount = 0;
		uint32_t runtimeMutationResidentApplySlowMaterialOnlyCount = 0;
		uint32_t runtimeMutationResidentApplyMaterialOnlyExclusiveCount = 0;
		uint32_t runtimeMutationResidentApplyMaterialOnlyNoResidentChunkCount = 0;
		uint32_t runtimeMutationResidentApplyMaterialOnlyInvalidReplacementCount = 0;
		uint32_t runtimeMutationResidentApplyMaterialOnlyMaterialCountMismatchCount = 0;
		uint32_t runtimeMutationResidentApplyMaterialPayloadHashCheckCount = 0;
		uint32_t runtimeMutationResidentApplyMaterialPayloadHashSkipCount = 0;
		uint32_t runtimeMutationResidentApplyMaterialPayloadHashMissCount = 0;
		uint32_t runtimeMutationResidentApplyMaterialPayloadHashRejectCount = 0;
		uint32_t runtimeMutationResidentApplyGeometryPayloadHashCheckCount = 0;
		uint32_t runtimeMutationResidentApplyGeometryPayloadHashSkipCount = 0;
		uint32_t runtimeMutationResidentApplyGeometryPayloadHashMissCount = 0;
		uint32_t runtimeMutationResidentApplyGeometryPayloadHashRejectCount = 0;
		uint32_t runtimeMutationResidentApplyGeometryPayloadHashBlasSkipCount = 0;
		uint32_t runtimeMutationResidentApplyGeometryPayloadOrderCheckCount = 0;
		uint32_t runtimeMutationResidentApplyGeometryPayloadOrderEquivalentCount = 0;
		uint32_t runtimeMutationResidentApplyGeometryPayloadOrderMissCount = 0;
		uint32_t runtimeMutationResidentApplyGeometryPayloadOrderRejectCount = 0;
		uint32_t runtimeMutationResidentApplyVertexStageRangeCount = 0;
		uint32_t runtimeMutationResidentApplyIndexStageRangeCount = 0;
		uint32_t runtimeMutationResidentApplyPrimitiveStageRangeCount = 0;
		uint64_t runtimeMutationResidentApplyVertexStageBytes = 0;
		uint64_t runtimeMutationResidentApplyIndexStageBytes = 0;
		uint64_t runtimeMutationResidentApplyPrimitiveStageBytes = 0;
		uint32_t runtimeMutationResidentApplyCoalescedStageRangeCount = 0;
		uint32_t runtimeMutationResidentApplyCoalescedStageRejectCount = 0;
		uint64_t runtimeMutationResidentApplyCoalescedStageBytes = 0;
		uint64_t runtimeMutationResidentApplyCoalescedStageGapBytes = 0;
		double runtimeMutationResidentApplyStageMapMs = 0.0;
		double runtimeMutationResidentApplyStageMemcpyMs = 0.0;
		double runtimeMutationResidentApplyStageCommandMs = 0.0;
		uint32_t runtimeMutationResidentApplyStageBatchCount = 0;
		uint32_t runtimeMutationResidentApplyStageBatchRangeCount = 0;
		uint32_t runtimeMutationResidentApplyStageCopyCommandCount = 0;
		uint32_t runtimeMutationResidentApplyStageBarrierCommandCount = 0;
		uint32_t runtimeMutationResidentApplyStageScratchGrowCount = 0;
		uint64_t runtimeMutationResidentApplyStageScratchGrowBytes = 0;
		uint32_t runtimeMutationResidentApplyPreserveGeometryCount = 0;
		uint32_t runtimeMutationResidentApplyPreserveIndexCount = 0;
		uint32_t runtimeMutationResidentApplyPreservePrimitiveCount = 0;
		uint32_t runtimeMutationResidentApplyBlasReuseCount = 0;
		uint32_t runtimeMutationResidentApplyBlasUpdateCount = 0;
		uint32_t runtimeMutationResidentApplyBlasRefitOnlyCount = 0;
		uint32_t runtimeMutationResidentApplyBlasRefitProbeCount = 0;
		uint32_t runtimeMutationResidentApplyBlasRefitRejectNoPreviousAsCount = 0;
		uint32_t runtimeMutationResidentApplyBlasRefitRejectIndexCountMismatchCount = 0;
		uint32_t runtimeMutationResidentApplyBlasRefitRejectPrimitiveCountMismatchCount = 0;
		uint32_t runtimeMutationResidentApplyBlasRefitRejectZeroIndexCount = 0;
		uint32_t runtimeMutationResidentApplyBlasRefitRejectZeroPrimitiveCount = 0;
		uint32_t runtimeMutationResidentApplyBlasRecreateCount = 0;
		uint32_t runtimeMutationResidentApplyBlasRecreateNoPreviousAsCount = 0;
		uint32_t runtimeMutationResidentApplyBlasRecreateRecoveredEmptyCount = 0;
		uint32_t runtimeMutationResidentApplyBlasRecreateSliceMovedCount = 0;
		uint32_t runtimeMutationResidentApplyBlasRecreateTopologyChangedCount = 0;
		uint32_t runtimeMutationResidentApplyBlasRecreateForceTopologyCount = 0;
		uint32_t runtimeMutationResidentApplyBlasScratchQueryCount = 0;
		uint32_t runtimeMutationResidentApplyBlasScratchCacheHitCount = 0;
		uint32_t runtimeMutationResidentApplyBlasScratchCacheMissCount = 0;
		uint32_t runtimeMutationResidentApplyBlasScratchGrowCount = 0;
		uint32_t runtimeMutationResidentApplyBlasBuildCommandCount = 0;
		uint32_t runtimeMutationResidentApplyBlasScratchBarrierCount = 0;
		uint32_t runtimeMutationResidentApplyKeepGeometrySliceCount = 0;
		uint32_t runtimeMutationResidentApplyKeepMaterialSliceCount = 0;
		uint32_t runtimeMutationResidentApplyEmptyRemovalCount = 0;
		uint32_t runtimeMutationResidentApplyRecoverAttemptCount = 0;
		uint32_t runtimeMutationResidentApplyRecoverSuccessCount = 0;
		uint32_t runtimeMutationResidentApplyAtlasGrowCount = 0;
		uint32_t runtimeMutationMaterialOnlyMismatchCount = 0;
		uint32_t runtimeMutationMaterialOnlyMismatchRefreshCount = 0;
		uint32_t runtimeMutationMaterialOnlyMismatchRebuildCount = 0;
		uint32_t runtimeMutationMaterialOnlyMismatchFilteredWallOnlyCount = 0;
		uint32_t runtimeMutationMaterialOnlyMismatchFilteredFlatOnlyCount = 0;
		uint32_t runtimeMutationMaterialOnlyMismatchFilteredMixedCount = 0;
		uint32_t runtimeMutationMaterialOnlyMismatchResidentWallOnlyCount = 0;
		uint32_t runtimeMutationMaterialOnlyMismatchResidentFlatOnlyCount = 0;
		uint32_t runtimeMutationMaterialOnlyMismatchResidentMixedCount = 0;
		uint32_t runtimeAnimatedSuppressedActiveCount = 0;
		uint32_t runtimeAnimatedSuppressionEmitCount = 0;
		uint32_t runtimeAnimatedUniqueTouchedCount = 0;
		uint32_t runtimeAnimatedMaterialRefreshCount = 0;
		uint32_t runtimeAnimatedAttemptCount = 0;
		uint32_t runtimeAnimatedSuppressedAttemptCount = 0;
		uint32_t runtimeAnimatedUnsuppressedAttemptCount = 0;
		uint32_t runtimeAnimatedResidentApplyCount = 0;
		uint32_t runtimeAnimatedSuppressedResidentApplyCount = 0;
		uint32_t runtimeAnimatedUnsuppressedResidentApplyCount = 0;
		uint32_t runtimeAnimatedSyncSkipCount = 0;
		uint32_t runtimeMutationActiveChunkCount = 0;
		uint32_t runtimeMutationValidChunkCount = 0;
		uint32_t runtimeMutationExcludedStaticChunkCount = 0;
		uint32_t runtimeMutationCachedSurfaceCount = 0;
		uint32_t runtimeMutationCachedTriangleCount = 0;
		uint32_t runtimeMutationCachedMaterialCount = 0;
		uint32_t runtimeMutationCachedMaterialStateCount = 0;
		uint32_t runtimeMutationMaterialCacheHitCount = 0;
		uint32_t runtimeMutationMaterialCacheMissCount = 0;
		uint32_t runtimeMutationMaterialCacheStoreCount = 0;
		uint32_t staticAnimatedResidentSliceCacheEntryCount = 0;
		uint32_t staticAnimatedResidentSliceCacheHitCount = 0;
		uint32_t staticAnimatedResidentSliceCacheMissCount = 0;
		uint32_t staticAnimatedResidentSliceCacheStoreCount = 0;
		uint32_t staticAnimatedResidentSliceApplyHitCount = 0;
		uint32_t staticAnimatedResidentSliceApplyMissCount = 0;
		uint32_t staticAnimatedResidentSliceSyncSkipHitCount = 0;
		uint32_t staticAnimatedResidentGpuPayloadCacheHitCount = 0;
		uint32_t staticAnimatedResidentGpuPayloadCacheMissCount = 0;
		uint32_t staticAnimatedResidentGpuPayloadCacheStoreCount = 0;
		uint32_t runtimeMutationPrimitiveCount = 0;
		uint32_t runtimeMutationMaterialCount = 0;
		uint32_t sceneLightSurfaceRecordCount = 0;
		uint32_t sceneLightStaticRecordCount = 0;
		uint32_t sceneLightRuntimeMutationRecordCount = 0;
		uint32_t sceneLightDynamicRecordCount = 0;
		uint32_t sceneLightCapturedRecordCount = 0;
		uint32_t sceneLightPersistentVoxelRecordCount = 0;
		uint32_t persistentVoxelOnboardingCandidateCount = 0;
		uint32_t persistentVoxelOnboardingAdmittedCount = 0;
		uint32_t persistentVoxelOnboardingDeferredCount = 0;
		uint32_t persistentVoxelOnboardingActorBudgetHits = 0;
		uint32_t persistentVoxelOnboardingPrimitiveBudgetHits = 0;
		uint32_t persistentVoxelOnboardingByteBudgetHits = 0;
		uint32_t persistentVoxelOnboardingTextureBudgetHits = 0;
		uint32_t persistentVoxelOnboardingAdmissionPendingCount = 0;
		uint32_t persistentVoxelOnboardingTexturePrewarmDeferredCount = 0;
		uint32_t persistentVoxelOnboardingMaterialInvalidCount = 0;
		uint32_t persistentVoxelOnboardingBudgetDeferredCount = 0;
		uint64_t persistentVoxelOnboardingEstimatedBytes = 0;
		uint64_t persistentVoxelOnboardingAdmittedBytes = 0;
		uint64_t persistentVoxelOnboardingDeferredBytes = 0;
		uint64_t persistentVoxelOnboardingByteBudget = 0;
		uint32_t persistentVoxelTexturePrewarmQueuedCount = 0;
		uint32_t persistentVoxelTexturePrewarmProcessedCount = 0;
		uint32_t persistentVoxelTexturePrewarmDeferredCount = 0;
		uint32_t persistentVoxelTexturePrewarmHitCount = 0;
		uint32_t persistentVoxelTexturePrewarmMissCount = 0;
		uint64_t persistentVoxelTexturePrewarmEstimatedBytes = 0;
		uint64_t persistentVoxelTexturePrewarmProcessedBytes = 0;
		uint64_t persistentVoxelTexturePrewarmDeferredBytes = 0;
		uint64_t persistentVoxelTexturePrewarmByteBudget = 0;
		double persistentVoxelTexturePrewarmMs = 0.0;
		uint32_t runtimeSpaceLinkPrimitiveCount = 0;
		uint32_t runtimeSpaceLinkMaterialCount = 0;
		uint32_t runtimeDebugSphereCount = 0;
		uint32_t runtimeDebugSphereLongitudeSegments = 0;
		uint32_t runtimeDebugSphereLatitudeSegments = 0;
		uint32_t runtimeDebugSpherePrimitiveCount = 0;
		uint32_t runtimeDebugSphereMaterialCount = 0;
		uint32_t overlayPrimitiveCount = 0;
		uint32_t overlayMaterialCount = 0;
		uint32_t overlayRuntimeSpaceLinkPrimitiveCount = 0;
		uint32_t overlayRuntimeSpaceLinkMaterialCount = 0;
		uint32_t overlayRuntimeMutationPrimitiveCount = 0;
		uint32_t overlayRuntimeMutationMaterialCount = 0;
		uint32_t overlayDynamicPrimitiveCount = 0;
		uint32_t overlayDynamicMaterialCount = 0;
		uint32_t overlayLocalPlayerReflectionPrimitiveCount = 0;
		uint32_t overlayLocalPlayerReflectionMaterialCount = 0;
		uint32_t overlayDebugSpherePrimitiveCount = 0;
		uint32_t overlayDebugSphereMaterialCount = 0;
		uint32_t overlayPersistentVoxelActorCount = 0;
		uint32_t overlayPersistentVoxelPrimitiveCount = 0;
		uint32_t overlayPersistentVoxelMaterialCount = 0;
		OverlayAppendSourceTraceEntry overlayRuntimeSpaceLinkAppend = {};
		OverlayAppendSourceTraceEntry overlayRuntimeMutationAppend = {};
		OverlayAppendSourceTraceEntry overlayDynamicAppend = {};
		OverlayAppendSourceTraceEntry overlayLocalPlayerReflectionAppend = {};
		OverlayAppendSourceTraceEntry overlayDebugSphereAppend = {};
		OverlayAppendSourceTraceEntry overlayPersistentVoxelAppend = {};
		uint32_t localPlayerReflectionCaptureRawFacingSprites = 0;
		uint32_t localPlayerReflectionCaptureRawVoxelSprites = 0;
		uint32_t localPlayerReflectionCaptureSurfaces = 0;
		uint32_t localPlayerReflectionCaptureMatchingActorSurfaces = 0;
		uint32_t localPlayerReflectionCaptureOtherActorSurfaces = 0;
		uint32_t localPlayerReflectionCaptureActorlessSurfaces = 0;
		uint32_t localPlayerReflectionCaptureFilteredSurfaces = 0;
		uint32_t localPlayerReflectionGeometryWallSurfaces = 0;
		uint32_t localPlayerReflectionGeometryFlatSurfaces = 0;
		uint32_t localPlayerReflectionGeometrySpriteSurfaces = 0;
		uint32_t localPlayerReflectionGeometryIndexedSurfaces = 0;
		uint32_t localPlayerReflectionGeometryTriangleFanSurfaces = 0;
		uint32_t localPlayerReflectionGeometrySpriteStripSurfaces = 0;
		uint32_t localPlayerReflectionGeometrySkippedSurfaces = 0;
		uint32_t localPlayerReflectionGeometrySourceVertices = 0;
		uint32_t localPlayerReflectionGeometrySourceIndices = 0;
		uint32_t localPlayerReflectionGeometryVertexGrowths = 0;
		uint32_t localPlayerReflectionGeometryIndexGrowths = 0;
		uint32_t localPlayerReflectionGeometryPrimitiveGrowths = 0;
		uint32_t localPlayerReflectionGeometryProvenanceGrowths = 0;
		uint32_t dynamicCaptureCalls = 0;
		uint32_t dynamicCaptureWallSurfaces = 0;
		uint32_t dynamicCaptureFlatSurfaces = 0;
		uint32_t dynamicCaptureSpriteSurfaces = 0;
		uint32_t dynamicCaptureVoxelProxySurfaces = 0;
		uint32_t dynamicCaptureUnsupportedModelSurfaces = 0;
		uint32_t dynamicCaptureVoxelCacheStores = 0;
		uint32_t dynamicCaptureVoxelCacheRebuilds = 0;
		uint32_t dynamicCaptureVoxelCacheDeferred = 0;
		uint32_t dynamicCaptureVoxelMeshBuilds = 0;
		uint32_t dynamicCaptureVoxelMeshDeferred = 0;
		uint32_t dynamicCaptureVoxelMeshHits = 0;
		uint32_t dynamicCaptureVoxelMeshMisses = 0;
		uint32_t dynamicCaptureVoxelMeshInvalid = 0;
		uint32_t dynamicCaptureVoxelCanonicalSurfaceBuilds = 0;
		uint32_t dynamicCaptureVoxelCanonicalSurfaceHits = 0;
		uint32_t dynamicCaptureVoxelCanonicalSurfaceInvalid = 0;
		uint32_t dynamicCaptureVoxelDuplicationAuditCalls = 0;
		uint32_t dynamicCaptureVoxelDuplicationAuditEntriesScanned = 0;
		uint32_t dynamicCaptureVoxelDuplicationAuditTemporaryContainersBuilt = 0;
		uint32_t dynamicCaptureVoxelMaintenanceCalls = 0;
		uint32_t dynamicCaptureVoxelMaintenanceSimulationSkips = 0;
		uint32_t dynamicCaptureVoxelMaintenanceLegacyReconciles = 0;
		uint32_t dynamicCaptureVoxelMaintenanceDeltaReconciles = 0;
		uint32_t dynamicCaptureVoxelMaintenanceReasonMask = 0;
		uint32_t dynamicCaptureVoxelMaintenanceLiveActorsEnumerated = 0;
		uint32_t dynamicCaptureVoxelMaintenanceCacheEntriesScanned = 0;
		uint32_t dynamicCaptureVoxelMaintenanceRemovals = 0;
		uint32_t dynamicCaptureVoxelMaintenanceTransformSyncs = 0;
		uint32_t dynamicCaptureVoxelLifecycleEventsApplied = 0;
		uint32_t dynamicCaptureVoxelLifecycleEventsDiscarded = 0;
		uint32_t dynamicCaptureVoxelLifecycleInsertEvents = 0;
		uint32_t dynamicCaptureVoxelLifecycleRemoveEvents = 0;
		uint32_t dynamicCaptureVoxelLifecycleStatEvents = 0;
		uint32_t dynamicCaptureVoxelLifecycleResetEvents = 0;
		uint32_t dynamicCaptureVoxelLifecycleOverflows = 0;
		uint32_t dynamicCaptureVoxelLifecycleRemovalEntriesMarked = 0;
		uint32_t dynamicCaptureModelActorCandidates = 0;
		uint32_t dynamicCaptureModelActorSorted = 0;
		uint32_t dynamicCaptureModelActorSortSkipped = 0;
		uint32_t dynamicCaptureModelScratchReuses = 0;
		uint32_t dynamicCaptureModelScratchGrows = 0;
		uint32_t dynamicCaptureModelScratchFallbacks = 0;
		uint32_t dynamicCaptureModelBudgetTruncations = 0;
		uint32_t dynamicCaptureModelSurfaceBuilds = 0;
		uint32_t voxelCacheActorEntries = 0;
		uint32_t voxelCacheActorSurfaces = 0;
		uint32_t voxelCacheUniqueMeshKeys = 0;
		uint32_t voxelCacheUniqueMaterialKeys = 0;
		uint32_t voxelCacheLocalSpaceSurfaces = 0;
		uint32_t voxelCacheBakedTransformSurfaces = 0;
		uint32_t voxelCacheUnknownSpaceSurfaces = 0;
		uint32_t voxelCacheTransformKeyedSurfaces = 0;
		uint32_t voxelCacheUniqueTransformBases = 0;
		uint32_t voxelCacheInvariantWarnings = 0;
		uint32_t voxelCacheActorPrimitives = 0;
		uint64_t voxelCacheDuplicatedVertexBytes = 0;
		uint64_t voxelCacheDuplicatedIndexBytes = 0;
		uint64_t voxelCacheDuplicatedPrimitiveBytes = 0;
		uint64_t voxelCacheDuplicatedTotalBytes = 0;
		uint32_t voxelCacheDuplicateTopCount = 0;
		uint32_t dynamicAsPrimitiveCount = 0;
		uint32_t dynamicAsVertexCount = 0;
		uint32_t dynamicAsIndexCount = 0;
		uint32_t dynamicAsRuntimeSpaceLinkPrimitives = 0;
		uint32_t dynamicAsRuntimeMutationPrimitives = 0;
		uint32_t dynamicAsDynamicPrimitives = 0;
		uint32_t dynamicAsLocalPlayerReflectionPrimitives = 0;
		uint32_t dynamicAsDebugSpherePrimitives = 0;
		uint32_t dynamicAsCreateCalls = 0;
		uint32_t dynamicAsReuseCount = 0;
		uint32_t dynamicAsScratchQueries = 0;
		uint32_t dynamicAsScratchGrowCount = 0;
		uint64_t dynamicAsRuntimeSpaceLinkBytes = 0;
		uint64_t dynamicAsRuntimeMutationBytes = 0;
		uint64_t dynamicAsDynamicBytes = 0;
		uint64_t dynamicAsLocalPlayerReflectionBytes = 0;
		uint64_t dynamicAsDebugSphereBytes = 0;
		uint64_t dynamicAsScratchRequestedBytes = 0;
		uint64_t dynamicAsMemoryBytes = 0;
		uint32_t dynamicOverlayBlasDomainCount = 0;
		uint32_t dynamicOverlayBlasVertexCount = 0;
		uint32_t dynamicOverlayBlasIndexCount = 0;
		uint32_t dynamicOverlayBlasPrimitiveCount = 0;
		uint32_t dynamicOverlayBlasMaterialCount = 0;
		uint32_t dynamicOverlayBlasEligibleDomains = 0;
		uint32_t dynamicOverlayBlasEligiblePrimitives = 0;
		uint32_t dynamicOverlayBlasFallbackDomains = 0;
		uint32_t dynamicOverlayBlasFallbackPrimitives = 0;
		uint32_t dynamicOverlayBlasRejectDisabled = 0;
		uint32_t dynamicOverlayBlasRejectStaticOverlay = 0;
		uint32_t dynamicOverlayBlasRejectRuntimeSpaceLink = 0;
		uint32_t dynamicOverlayBlasRejectRuntimeMutation = 0;
		uint32_t dynamicOverlayBlasRejectLocalPlayerReflection = 0;
		uint32_t dynamicOverlayBlasRejectRuntimeDebugSphere = 0;
		uint32_t dynamicOverlayBlasRejectSurfaceLightOverlay = 0;
		uint32_t dynamicOverlayBlasRejectMaterialBase = 0;
		uint32_t dynamicOverlayBlasBuildAttempts = 0;
		uint32_t dynamicOverlayBlasBuildSuccesses = 0;
		uint32_t dynamicOverlayBlasCacheHits = 0;
		uint32_t dynamicOverlayBlasCacheMisses = 0;
		uint32_t dynamicOverlayBlasRoutedInstances = 0;
		uint32_t dynamicOverlayBlasMonolithicRefs = 0;
		uint32_t dynamicOverlayBlasBuildBudget = 0;
		bool dynamicOverlayBlasBuildEnabled = false;
		bool dynamicOverlayBlasRouteEnabled = false;
		uint32_t filterCandidateOccurrences = 0;
		uint32_t filterCandidateCertifiedOccurrences = 0;
		uint32_t filterCandidateCertifiedPrimitives = 0;
		uint32_t filterCandidateRejectEmpty = 0;
		uint32_t filterCandidateRejectRange = 0;
		uint32_t filterCandidateRejectMixed = 0;
		bool filterCandidateEnabled = false;
		uint32_t persistentVoxelAsCalls = 0;
		uint32_t persistentVoxelAsBuilds = 0;
		uint32_t persistentVoxelAsUniqueMeshBuilds = 0;
		uint32_t persistentVoxelAsInstances = 0;
		uint32_t persistentVoxelSharedMeshResources = 0;
		uint32_t persistentVoxelTlasInstances = 0;
		uint32_t persistentVoxelInstanceTransformUpdates = 0;
		uint32_t persistentVoxelBatchSerialFastPathCount = 0;
		uint32_t persistentVoxelBakedFallbackInstances = 0;
		uint32_t persistentVoxelBatchActorCount = 0;
		uint32_t persistentVoxelInstanceRecordCount = 0;
		uint32_t persistentVoxelPendingInstanceCount = 0;
		uint32_t persistentVoxelMaterialVariantResourceCount = 0;
		uint32_t persistentVoxelZeroRefMeshResourceCount = 0;
		uint32_t persistentVoxelZeroRefMaterialResourceCount = 0;
		uint32_t persistentVoxelAdmissionQueueCount = 0;
		uint64_t persistentVoxelResidentResourceBytes = 0;
		uint64_t persistentVoxelZeroRefResourceBytes = 0;
		uint32_t worldTlasBuildCalls = 0;
		uint32_t worldTlasExactReuseCalls = 0;
		uint32_t worldTlasUpdateCalls = 0;
		uint32_t worldTlasUpdateReasonMask = 0;
		uint32_t worldTlasUpdateDirtyRangeCount = 0;
		uint32_t worldTlasUpdateDirtyInstanceCount = 0;
		uint64_t worldTlasUpdateDirtyBytes = 0;
		uint32_t worldTlasBlasOverrideUpdateCalls = 0;
		uint32_t worldTlasFullBuildCalls = 0;
		uint32_t worldTlasFullBuildReasonMask = 0;
		uint32_t worldTlasFullBuildChangeReasonMask = 0;
		uint32_t worldTlasFullBuildUpdateRejectReasonMask = 0;
		uint32_t worldTlasFullBuildUpdateGateReasonMask = 0;
		uint32_t worldTlasFullBuildDestinationReuseCalls = 0;
		uint32_t worldTlasFullBuildDestinationCreateCalls = 0;
		uint32_t worldTlasFullBuildGrowthCalls = 0;
		uint32_t worldTlasFullBuildGrowthReasonMask = 0;
		uint32_t worldTlasFullBuildReuseRejectReasonMask = 0;
		uint32_t worldTlasFullBuildReuseRuntimeFallbacks = 0;
		uint32_t worldTlasSameCommandResourceRotations = 0;
		uint64_t worldTlasBlasContentGeneration = 0;
		uint32_t worldTlasInstanceCount = 0;
		double worldTlasRetireMs = 0.0;
		double worldTlasInstanceUploadMs = 0.0;
		double worldTlasCreateMs = 0.0;
		double worldTlasMemoryMs = 0.0;
		double worldTlasScratchMs = 0.0;
		double worldTlasDescriptorMs = 0.0;
		double worldTlasBuildMs = 0.0;
		double worldTlasUpdateMs = 0.0;
		double worldTlasBarrierMs = 0.0;
		uint32_t worldTlasCreateCalls = 0;
		uint32_t worldTlasScratchQueries = 0;
		uint32_t worldTlasScratchGrowCount = 0;
		uint32_t worldTlasDescriptorCreateCalls = 0;
		uint32_t worldTlasBarrierCount = 0;
		double worldTlasPreGrowMs = 0.0;
		double worldTlasPreGrowWaitMs = 0.0;
		uint32_t worldTlasPreGrowCalls = 0;
		uint32_t worldTlasPreGrowInstanceCount = 0;
		uint32_t worldTlasPreGrowInstanceGrowCount = 0;
		uint32_t worldTlasPreGrowScratchGrowCount = 0;
		uint32_t worldTlasPreGrowDeferredAsSlots = 0;
		uint32_t worldTlasPreGrowDeferredInstanceSlots = 0;
		uint32_t worldTlasPreGrowDeferredScratchSlots = 0;
		uint64_t worldTlasPreGrowInstanceRequestedBytes = 0;
		uint64_t worldTlasPreGrowInstanceAllocatedBytes = 0;
		uint64_t worldTlasPreGrowScratchRequestedBytes = 0;
		uint64_t worldTlasPreGrowBuildScratchRequestedBytes = 0;
		uint64_t worldTlasPreGrowUpdateScratchRequestedBytes = 0;
		uint64_t worldTlasPreGrowScratchAllocatedBytes = 0;
		uint64_t worldTlasScratchRequestedBytes = 0;
		uint64_t worldTlasBuildScratchRequestedBytes = 0;
		uint64_t worldTlasUpdateScratchRequestedBytes = 0;
		uint64_t worldTlasScratchAllocatedBytes = 0;
		uint64_t worldTlasMemoryBytes = 0;
		uint32_t asWorldTlasObjects = 0;
		uint32_t asWorldTlasEntries = 0;
		uint32_t asWorldTlasMaskAllWorkloadsRefs = 0;
		uint32_t asWorldTlasMaskOtherRefs = 0;
		uint32_t asEmissiveTlasObjects = 0;
		uint32_t asEmissiveTlasEntries = 0;
		uint32_t asBlasStatic = 0;
		uint32_t asBlasDynamic = 0;
		uint32_t asBlasVoxelUnique = 0;
		uint32_t asBlasVoxelActor = 0;
		uint32_t asEntriesStatic = 0;
		uint32_t asEntriesDynamic = 0;
		uint32_t asEntriesVoxel = 0;
		uint32_t asSceneRecords = 0;
		uint32_t asVoxelUniqueGeometryKeys = 0;
		uint32_t asVoxelActorInstances = 0;
		uint32_t asVoxelSharedBlasRefs = 0;
		uint32_t asStaticUniqueGeometrySignatures = 0;
		uint32_t asStaticSegmentBlas = 0;
		uint32_t asStaticChunkOwnedBlas = 0;
		uint32_t asStaticSegmentCandidateChunks = 0;
		uint32_t asStaticSegmentUniqueGeometrySignatures = 0;
		uint32_t asStaticSegmentDuplicateKeys = 0;
		uint32_t asStaticSegmentDuplicateRefs = 0;
		uint32_t asStaticSegmentPortalChunks = 0;
		uint32_t asStaticSegmentLocalSpaceChunks = 0;
		uint32_t asStaticSegmentAnimatedChunks = 0;
		uint32_t asStaticSegmentAtlasEligibleChunks = 0;
		uint32_t asStaticSegmentRegistryMappedChunks = 0;
		uint32_t asStaticSegmentCandidateSurfaces = 0;
		uint32_t asStaticSegmentWallCandidates = 0;
		uint32_t asStaticSegmentFloorCandidates = 0;
		uint32_t asStaticSegmentCeilingCandidates = 0;
		uint32_t asStaticSegmentPortalCandidates = 0;
		uint32_t asStaticSegmentLocalSpaceSurfaces = 0;
		uint32_t asStaticSegmentAnimatedSurfaces = 0;
		uint32_t asStaticSegmentMaterialRiskSurfaces = 0;
		uint32_t asStaticSegmentContiguousChunkSurfaces = 0;
		uint32_t asStaticSegmentCacheCandidates = 0;
		uint32_t asStaticSegmentCacheEntries = 0;
		uint32_t asStaticSegmentCacheHits = 0;
		uint32_t asStaticSegmentCacheMisses = 0;
		uint32_t asStaticSegmentCacheDuplicateRefs = 0;
		uint32_t asStaticSegmentCacheResidentBlas = 0;
		uint32_t asStaticSegmentCacheBuildsThisFrame = 0;
		uint32_t asStaticSegmentCacheBuildsLastRebuild = 0;
		uint32_t asStaticSegmentCacheInvalidations = 0;
		uint64_t asStaticSegmentCacheResidentBytes = 0;
		bool asStaticSegmentCacheBlasBuildEnabled = false;
		uint32_t asStaticSegmentRouteRouted = 0;
		uint32_t asStaticSegmentRouteChunkFallback = 0;
		uint32_t asStaticSegmentRouteRejectDisabled = 0;
		uint32_t asStaticSegmentRouteRejectMissingCache = 0;
		uint32_t asStaticSegmentRouteRejectMissingBlas = 0;
		uint32_t asStaticSegmentRouteSegmentBlasRefs = 0;
		uint32_t asStaticSegmentRouteChunkBlasRefs = 0;
		bool successDiagnosticsBasicCollected = false;
		bool successDiagnosticsInstanceCompositionCollected = false;
		bool successDiagnosticsPersistentVoxelStatusCollected = false;
		bool successDiagnosticsAsSummaryCollected = false;
		bool successDiagnosticsDeepSceneAuditCollected = false;
		uint32_t successDiagnosticsDeepSceneAuditCacheHits = 0;
		uint32_t successDiagnosticsDeepSceneAuditRebuilds = 0;
		uint32_t successDiagnosticsInstanceRowsScanned = 0;
		uint32_t successDiagnosticsPersistentStatusCalls = 0;
		uint32_t successDiagnosticsStaticChunkRowsScanned = 0;
		uint32_t successDiagnosticsStaticChunkRowsIncrementallyUpdated = 0;
		uint32_t successDiagnosticsStaticSurfaceRowsScanned = 0;
		uint32_t successDiagnosticsStaticSurfaceRowsIncrementallyUpdated = 0;
		uint32_t successDiagnosticsRegistryRowsScanned = 0;
		uint32_t successDiagnosticsTemporaryContainersBuilt = 0;
		uint32_t emissiveAsRecords = 0;
		bool emissiveAsEnabled = false;
		uint32_t emissiveAsRecordsStatic = 0;
		uint32_t emissiveAsRecordsDynamic = 0;
		uint32_t emissiveAsRecordsPersistentVoxel = 0;
		uint32_t emissiveAsStaticRecordMatchedChunks = 0;
		uint32_t emissiveAsStaticRecordUnmatchedChunks = 0;
		uint32_t emissiveAsDynamicRecordCount = 0;
		uint32_t emissiveAsPersistentVoxelIgnoredRecords = 0;
		uint32_t emissiveAsStaticChunkRefs = 0;
		uint32_t emissiveAsDynamicAggregateRefs = 0;
		uint32_t emissiveAsMaskAllWorkloadsRefs = 0;
		uint32_t emissiveAsMaskOtherRefs = 0;
		uint32_t emissiveAsPayloadCacheHits = 0;
		uint32_t emissiveAsPayloadCacheMisses = 0;
		uint32_t emissiveSamplingSurfaceStatic = 0;
		uint32_t emissiveSamplingSurfaceCaptured = 0;
		uint32_t emissiveSamplingSurfaceRuntimeMutation = 0;
		uint32_t emissiveSamplingSurfaceDynamic = 0;
		uint32_t emissiveSamplingSurfacePersistentVoxel = 0;
		uint32_t emissiveSamplingOutputStaticRecords = 0;
		uint32_t emissiveSamplingOutputDynamicRecords = 0;
		uint32_t emissiveSamplingOutputPersistentVoxelRecords = 0;
		uint32_t emissiveSamplingSkippedPersistentVoxelSurfaces = 0;
		uint32_t asDynamicUniqueGeometrySignatures = 0;
		uint32_t asBlasCacheHits = 0;
		uint32_t asBlasBuiltThisFrame = 0;
		uint32_t asMonolithicDynamicBlasBuilds = 0;
		uint32_t sceneRecordAuditRecords = 0;
		uint32_t sceneRecordAuditStatic = 0;
		uint32_t sceneRecordAuditDynamic = 0;
		uint32_t sceneRecordAuditPersistentVoxel = 0;
		uint32_t sceneRecordAuditInvalidSource = 0;
		uint32_t sceneRecordAuditVisibilityChunked = 0;
		uint32_t sceneRecordAuditLegacyCompatible = 0;
		uint32_t sceneRecordAuditMaterialIndirection = 0;
		uint32_t hitMetadataAuditRecords = 0;
		uint32_t hitMetadataPrimitiveBaseMismatches = 0;
		uint32_t hitMetadataMaterialBaseMismatches = 0;
		uint32_t hitMetadataLegacyPrimitiveOffsetMatches = 0;
		uint32_t hitMetadataPersistentMaterialBaseRecords = 0;
		uint32_t voxelSharedBlasActiveActors = 0;
		uint32_t voxelSharedBlasUniqueDesiredKeys = 0;
		uint32_t voxelSharedBlasResidentAssets = 0;
		uint32_t voxelSharedBlasQueuedAssets = 0;
		uint32_t voxelSharedBlasEligibleBuildKeys = 0;
		uint32_t voxelSharedBlasBuildAttempts = 0;
		uint32_t voxelSharedBlasBuildSuccesses = 0;
		uint32_t voxelSharedBlasBuildFailures = 0;
		uint32_t voxelSharedBlasCacheHits = 0;
		uint32_t voxelSharedBlasCacheMisses = 0;
		uint32_t voxelSharedBlasActorRefs = 0;
		uint32_t voxelSharedBlasRoutedLegacy = 0;
		uint32_t voxelSharedBlasRoutedShared = 0;
		uint32_t voxelSharedBlasFallbackLastValid = 0;
		uint32_t voxelSharedBlasActiveReferencedAssets = 0;
		uint32_t voxelSharedBlasUnreferencedResidentAssets = 0;
		uint64_t voxelSharedBlasResidentBytes = 0;
		uint64_t voxelSharedBlasActiveReferencedBytes = 0;
		uint64_t voxelSharedBlasUnreferencedResidentBytes = 0;
		uint32_t voxelSharedBlasRouteEligibleActors = 0;
		uint32_t voxelSharedBlasRouteRejectMissingResident = 0;
		uint32_t voxelSharedBlasRouteRejectNonLocal = 0;
		uint32_t voxelSharedBlasRouteRejectTransformKeyed = 0;
		uint32_t voxelSharedBlasRouteRejectInvalidMaterial = 0;
		uint32_t voxelSharedBlasRouteRejectInvalidTransform = 0;
		uint32_t voxelSharedBlasRouteRejectGeometryMismatch = 0;
		uint32_t voxelSharedBlasRejectMissingKey = 0;
		uint32_t voxelSharedBlasRejectDisabled = 0;
		uint32_t voxelSharedBlasRejectNonLocal = 0;
		uint32_t voxelSharedBlasRejectTransformKeyed = 0;
		uint32_t voxelSharedBlasRejectMissingBuffers = 0;
		uint32_t voxelSharedBlasRejectInvalidCounts = 0;
		uint32_t voxelSharedBlasRejectBuildBudget = 0;
		uint32_t voxelSharedBlasRejectGeometryMismatch = 0;
		uint32_t voxelLocalShareProfileActiveActors = 0;
		uint32_t voxelLocalShareProfileLocalSpaceActors = 0;
		uint32_t voxelLocalShareProfileBakedTransformActors = 0;
		uint32_t voxelLocalShareProfileUnknownSpaceActors = 0;
		uint32_t voxelLocalShareProfileTransformKeyedActors = 0;
		uint32_t voxelLocalShareProfileLocalIdentityTransformActors = 0;
		uint32_t voxelLocalShareProfileLocalNonIdentityTransformActors = 0;
		uint32_t voxelLocalShareProfileShareableLocalActors = 0;
		uint32_t voxelLocalShareProfileShareableUniqueKeys = 0;
		uint32_t voxelLocalShareProfileShareableDuplicateActorRefs = 0;
		uint32_t voxelLocalShareProfileShareableSingleActorKeys = 0;
		uint32_t voxelLocalShareProfileShareableMultiActorKeys = 0;
		uint32_t voxelLocalShareProfileResidentShareableKeys = 0;
		uint32_t voxelLocalShareProfileEligibleNotResidentActors = 0;
		uint32_t voxelLocalShareProfileRejectMissingMesh = 0;
		uint32_t voxelLocalShareProfileRejectNonLocal = 0;
		uint32_t voxelLocalShareProfileRejectTransformKeyed = 0;
		uint32_t voxelLocalShareProfileRejectMissingBuffers = 0;
		uint32_t voxelLocalShareProfileRejectInvalidCounts = 0;
		uint32_t voxelLocalShareProfileRejectInvalidMaterial = 0;
		uint32_t voxelLocalShareProfileRejectInvalidTransform = 0;
		uint32_t voxelLocalShareProfileRejectGeometryMismatch = 0;
		uint32_t voxelSharedKeyAuditActors = 0;
		uint32_t voxelSharedKeyAuditKeys = 0;
		uint32_t voxelSharedKeyAuditSafeKeys = 0;
		uint32_t voxelSharedKeyAuditUnsafeKeys = 0;
		uint32_t voxelSharedKeyAuditGeometryMismatchKeys = 0;
		uint32_t voxelSharedKeyAuditCountMismatchKeys = 0;
		uint32_t voxelSharedKeyAuditMaterialVariantKeys = 0;
		uint32_t voxelSharedKeyAuditMaterialCountMismatchKeys = 0;
		uint32_t voxelSharedKeyAuditSourcePicnumAliasKeys = 0;
		uint32_t voxelSharedKeyAuditVoxelIndexAliasKeys = 0;
		uint32_t voxelSharedKeyAuditSourceStateAliasActorRefs = 0;
		uint32_t voxelSharedKeyAuditBakeSpaceMismatchKeys = 0;
		uint32_t voxelSharedKeyAuditTransformBasisMismatchKeys = 0;
		uint32_t voxelSharedKeyAuditLocalShareableUnsafeKeys = 0;
		uint32_t voxelLocalSpaceInvariantLocalActors = 0;
		uint32_t voxelLocalSpaceInvariantLocalIdentityTransformActors = 0;
		uint32_t voxelLocalSpaceInvariantLocalNonIdentityTransformActors = 0;
		uint32_t voxelLocalSpaceInvariantSuspiciousWorldBoundsActors = 0;
		uint32_t voxelLocalSpaceInvariantMissingBoundsActors = 0;
		uint32_t voxelLocalSpaceInvariantInvalidTransformActors = 0;
		uint32_t voxelLocalSpaceInvariantBakedFallbackActors = 0;
		uint32_t voxelLocalSpaceInvariantUnknownSpaceActors = 0;
		float voxelLocalSpaceInvariantMaxBoundsCenterMagnitude = 0.0f;
		float voxelLocalSpaceInvariantMaxBoundsAbs = 0.0f;
		uint32_t sceneDataSetCalls = 0;
		uint32_t sceneTextureCacheCount = 0;
		uint32_t sceneTextureCacheMisses = 0;
		uint32_t sceneTextureCacheInserts = 0;
		uint32_t sceneTextureTransitionCount = 0;
		uint32_t sceneTextureRequestedCount = 0;
		uint32_t sceneTextureReferencedActorMaterialCount = 0;
		uint32_t sceneTextureReferencedBaseCount = 0;
		uint32_t sceneTextureReferencedGlowCount = 0;
		uint32_t sceneTextureReferencedNormalCount = 0;
		uint32_t sceneTextureReferencedMetallicCount = 0;
		uint32_t sceneTextureReferencedRoughnessCount = 0;
		uint32_t sceneTextureReferencedEmissiveCount = 0;
		uint32_t materialBuildCalls = 0;
		uint32_t actorOverrideMapBuildCalls = 0;
		uint32_t actorOverflowMaterialCount = 0;
		uint32_t actorOverflowBaseClampCount = 0;
		uint32_t actorOverflowNormalClampCount = 0;
		uint32_t actorOverflowMetallicClampCount = 0;
		uint32_t actorOverflowRoughnessClampCount = 0;
		uint32_t actorOverflowEmissiveClampCount = 0;
		uint32_t actorOverflowTraceOmittedCount = 0;
		uint32_t persistentDynamicActorSurfaceCount = 0;
		uint32_t persistentDynamicNonActorSurfaceCount = 0;
		uint32_t persistentDynamicWallSurfaceCount = 0;
		uint32_t persistentDynamicFlatSurfaceCount = 0;
		uint32_t persistentDynamicSpriteSurfaceCount = 0;
		uint32_t persistentDynamicHighWaterSurfaceCount = 0;
		uint32_t persistentDynamicHighWaterPrimitiveCount = 0;
		uint32_t persistentDynamicHighWaterMaterialCount = 0;
		uint32_t persistentDynamicHighWaterActorSurfaceCount = 0;
		uint32_t persistentDynamicHighWaterSpriteSurfaceCount = 0;
		uint32_t dynamicMergeLiveSurfaceCount = 0;
		uint32_t dynamicMergePersistentCacheSurfaceCount = 0;
		uint32_t dynamicMergeAppendedPersistentSurfaceCount = 0;
		uint32_t dynamicMergeDuplicatePersistentSurfaceCount = 0;
		uint32_t dynamicMergeAppendedPersistentPrimitiveCount = 0;
		uint32_t dynamicMergeAppendedPersistentMaterialCount = 0;
		uint32_t dynamicMergeDeltaAppendAttempts = 0;
		uint32_t dynamicMergeDeltaAppendUsed = 0;
		uint32_t dynamicMergeDeltaAppendFallbacks = 0;
		uint32_t dynamicMergeDeltaAppendFallbackNonZeroSurfaces = 0;
		uint32_t sceneLightSpriteTileRuleCount = 0;
		uint32_t sceneLightSpriteRecordCandidateScans = 0;
		uint32_t sceneLightActorOverlayRuleCount = 0;
		uint32_t sceneLightActorOverlaySurfaceLookups = 0;
		uint32_t sceneLightActorOverlayFullRecordScans = 0;
		uint32_t sceneLightActorOverlaySurfaceCandidateScans = 0;
		uint32_t sceneLightActorOverlayIndexedCandidateCount = 0;
		uint32_t sceneLightTopologyKeyCount = 0;
		uint32_t sceneLightTopologyRebuildCount = 0;
		uint32_t sceneLightPropertyOnlyUpdateCount = 0;
		uint32_t sceneLightTopologySortSkippedCount = 0;
		uint32_t sceneLightTopologyAddedKeyCount = 0;
		uint32_t sceneLightTopologyRemovedKeyCount = 0;
		uint32_t sceneLightTopologyReboundKeyCount = 0;
		uint32_t sceneLightSoftLightCount = 0;
		uint32_t sceneLightSurvivingIndexChangeCount = 0;
		uint32_t sceneLightSurvivingSoftIndexChangeCount = 0;
		uint64_t sceneLightOrderedStableKeyHash = 0;
		double sceneLightTopologySortMs = 0.0;
		uint32_t traceOpaqueDispatchX = 0;
		uint32_t traceOpaqueDispatchY = 0;
		uint32_t traceOpaqueDispatchZ = 0;
		uint64_t traceRendererFrame = 0;
		uint64_t traceSettingsKey = 0;
		uint64_t traceWorkloadKey = 0;
		uint32_t traceRenderWidth = 0;
		uint32_t traceRenderHeight = 0;
		uint32_t traceOutputWidth = 0;
		uint32_t traceOutputHeight = 0;
		uint32_t traceLightBounceCount = 0;
		uint32_t traceMirrorBounceCount = 0;
		uint32_t tracePortalDepth = 0;
		uint32_t traceEmissiveSampleCount = 0;
		uint32_t traceEmissiveRequestedSampleCount = 0;
		uint32_t traceEmissivePrimarySampleBudget = 0;
		uint32_t traceIndirectSamplingRequestedMode = 0;
		uint32_t traceIndirectSamplingEffectiveMode = 0;
		uint32_t traceIndirectSamplingActiveMode = 0;
		uint32_t traceHitDistanceReconstructionMode = 0;
		uint32_t traceRuntimeLightCount = 0;
		uint32_t traceRuntimeLightTileCountX = 0;
		uint32_t traceRuntimeLightTileCountY = 0;
		uint32_t traceRuntimeLightTileSize = 0;
		uint32_t traceRuntimeLightTileIndexCount = 0;
		uint32_t traceRuntimeLightMaxTileOccupancy = 0;
		uint32_t traceEmissivePrimitiveCount = 0;
		double traceEmissiveTotalPower = 0.0;
		uint32_t traceFlags = 0;
		uint32_t traceDebugMode = 0;
		uint32_t traceBootstrapMode = 0;
		uint32_t traceUpscalerKind = 0;
		uint32_t traceUpscalerMode = 0;
		uint32_t traceDenoiserMode = 0;
		uint32_t traceDirectScene = 0;
		uint32_t traceDirectional = 0;
		uint32_t traceDirectionalShadow = 0;
		uint32_t traceSplitShadow = 0;
		uint32_t traceFastEmissiveShadow = 0;
		uint32_t traceVisibleChunkGate = 0;
		uint32_t traceVoxelOccurrenceControl = 0;
		uint32_t activePrimitiveCount = 0;
		uint32_t dynamicPrimitiveCount = 0;
		uint32_t activeMaterialCount = 0;
		uint32_t sceneInstanceCount = 0;
		uint32_t sceneInstanceStaticCount = 0;
		uint32_t sceneInstanceDynamicCount = 0;
		uint32_t sceneInstancePersistentVoxelCount = 0;
		uint32_t persistentVoxelMeshVariantResourceCount = 0;
		uint32_t persistentVoxelInstanceActiveCount = 0;
		uint32_t persistentVoxelInstancePrimitiveCount = 0;
		uint32_t persistentVoxelInstanceMaterialCount = 0;
		uint32_t persistentVoxelInstanceMinPrimitiveCount = 0;
		uint32_t persistentVoxelInstanceMaxPrimitiveCount = 0;
		uint32_t geometryBuildDynamicLivePrimitives = 0;
		uint32_t geometryBuildPersistentVoxelVariantCalls = 0;
		uint32_t geometryBuildPersistentVoxelVariantPrimitives = 0;
		uint32_t geometryBuildStaticChunkCalls = 0;
		uint32_t geometryBuildStaticChunkPrimitives = 0;
		uint32_t geometryBuildRuntimeMutationTruthCalls = 0;
		uint32_t geometryBuildRuntimeMutationRebuildCalls = 0;
		uint32_t geometryBuildRuntimeMutationMaterialOnlyCalls = 0;
		uint32_t geometryBuildRuntimeMutationPrimitives = 0;
		uint32_t geometryBuildRuntimeSpaceLinkCalls = 0;
		uint32_t geometryBuildRuntimeSpaceLinkPrimitives = 0;
		uint32_t geometryBuildResidentApplyCalls = 0;
		uint32_t geometryBuildResidentRecoverCalls = 0;
		uint32_t geometryBuildResidentPrimitives = 0;
		uint32_t dynamicVoxelEscapeActorCount = 0;
		uint32_t dynamicVoxelEscapeEligibleActorCount = 0;
		uint32_t dynamicVoxelEscapeForcedActorCount = 0;
		uint32_t dynamicVoxelEscapePrimitiveCount = 0;
		uint64_t dynamicVoxelEscapeVertexBytes = 0;
		uint64_t dynamicVoxelEscapeIndexBytes = 0;
		uint64_t dynamicVoxelEscapePrimitiveBytes = 0;
		uint64_t dynamicVoxelEscapeMaterialBytes = 0;
		uint64_t dynamicVoxelEscapeTotalBytes = 0;
		uint32_t dynamicVoxelExpectedEscapeActorCount = 0;
		uint32_t dynamicVoxelUnexpectedEscapeActorCount = 0;
		uint32_t dynamicVoxelExpectedEscapePrimitiveCount = 0;
		uint32_t dynamicVoxelUnexpectedEscapePrimitiveCount = 0;
		uint64_t dynamicVoxelExpectedEscapeTotalBytes = 0;
		uint64_t dynamicVoxelUnexpectedEscapeTotalBytes = 0;
		uint32_t dynamicVoxelEscapeTopCount = 0;
		uint32_t dynamicVoxelUnexpectedEscapeTopCount = 0;
		bool usedStaticMapScene = false;
		bool usedDynamicOverlay = false;
		bool usedPersistentDynamicEmissiveCache = false;
		std::string sceneTextureReason;
		std::array<MaterialBuildTraceEntry, MaterialBuildTraceSlotCount> materialBuildByLabel = {};
		std::array<RuntimeMutationTopTraceEntry, RuntimeMutationTopTraceCount> runtimeMutationTopEntries = {};
		std::array<RuntimeSectorDirtyTruthTraceEntry, RuntimeSectorDirtyTruthTraceCount> runtimeSectorDirtyTruthEntries = {};
		std::array<RuntimeAnimatedChurnTraceEntry, RuntimeAnimatedChurnTraceCount> runtimeAnimatedChurnEntries = {};
		std::array<RuntimeMaterialOnlyMismatchTraceEntry, RuntimeMaterialOnlyMismatchTraceCount> runtimeMaterialOnlyMismatchEntries = {};
		std::array<RuntimeResidentBlasRecreateTraceEntry, RuntimeResidentBlasRecreateTraceCount> runtimeResidentBlasRecreateEntries = {};
		std::array<RuntimeResidentBlasRefitRejectTraceEntry, RuntimeResidentBlasRefitRejectTraceCount> runtimeResidentBlasRefitRejectEntries = {};
		std::array<RuntimeStructuralRebuildTraceEntry, RuntimeStructuralRebuildTraceCount> runtimeStructuralRebuildEntries = {};
		std::array<RuntimeGeometryDirtyTraceEntry, RuntimeGeometryDirtyTraceCount> runtimeGeometryDirtyEntries = {};
		std::array<RuntimeRecurringChunkTraceEntry, RuntimeRecurringChunkTraceCount> runtimeRecurringChunkEntries = {};
		std::array<RuntimeInvisibleProofTraceEntry, RuntimeInvisibleProofTraceCount> runtimeInvisibleProofEntries = {};
		std::array<nri_scene::VoxelDuplicateVariantTraceEntry, nri_scene::VoxelDuplicateVariantTraceCount> voxelCacheDuplicateTopEntries = {};
		std::array<nri_scene::DynamicVoxelEscapeTraceEntry, nri_scene::DynamicVoxelEscapeTraceCount> dynamicVoxelEscapeTopEntries = {};
		std::array<nri_scene::DynamicVoxelEscapeTraceEntry, nri_scene::DynamicVoxelEscapeTraceCount> dynamicVoxelUnexpectedEscapeTopEntries = {};
	};

	struct PerfResourceTraceStats
	{
		uint32_t waitCalls = 0;
		double waitMs = 0.0;
		uint32_t residentChunkWriteWaitCalls = 0;
		double residentChunkWriteWaitMs = 0.0;
		uint32_t residentChunkBlasRebuildWaitCalls = 0;
		double residentChunkBlasRebuildWaitMs = 0.0;
		uint32_t sceneDataUploadWaitCalls = 0;
		double sceneDataUploadWaitMs = 0.0;
		uint32_t sceneBufferUploadWaitCalls = 0;
		double sceneBufferUploadWaitMs = 0.0;
		uint32_t emissiveSamplingUploadWaitCalls = 0;
		double emissiveSamplingUploadWaitMs = 0.0;
		uint32_t worldTlasInstanceUploadWaitCalls = 0;
		double worldTlasInstanceUploadWaitMs = 0.0;
		uint32_t worldTlasScratchResizeWaitCalls = 0;
		double worldTlasScratchResizeWaitMs = 0.0;
		uint32_t emissiveTlasInstanceUploadWaitCalls = 0;
		double emissiveTlasInstanceUploadWaitMs = 0.0;
		uint32_t emissiveTlasScratchResizeWaitCalls = 0;
		double emissiveTlasScratchResizeWaitMs = 0.0;
		uint32_t otherWaitCalls = 0;
		double otherWaitMs = 0.0;
		uint32_t growEvents = 0;
		uint32_t overwriteEvents = 0;
		uint32_t sceneUploadCalls = 0;
		uint32_t sceneDynamicUploadCalls = 0;
		uint32_t sceneResidentChunkUploadCalls = 0;
		uint32_t scenePersistentVoxelUploadCalls = 0;
		uint32_t scenePersistentVoxelVariantUploadCalls = 0;
		uint32_t sceneStaticRefreshUploadCalls = 0;
		uint32_t sceneOtherUploadCalls = 0;
		uint32_t sceneDataUploadCalls = 0;
		uint32_t emissiveUploadCalls = 0;
		uint32_t residentChunkBatchChunkCount = 0;
		uint32_t residentChunkBatchGeometryDirtyCount = 0;
		uint32_t residentChunkBatchMaterialDirtyCount = 0;
		uint32_t residentChunkBatchRecoverEmptyCount = 0;
		uint32_t residentChunkBatchMaterialFallbackCount = 0;
		uint32_t residentChunkBatchBlasRebuildCount = 0;
		uint64_t sceneUploadBytes = 0;
		uint64_t sceneDynamicUploadBytes = 0;
		uint64_t sceneResidentChunkUploadBytes = 0;
		uint64_t scenePersistentVoxelUploadBytes = 0;
		uint64_t scenePersistentVoxelVariantUploadBytes = 0;
		uint64_t sceneStaticRefreshUploadBytes = 0;
		uint64_t sceneOtherUploadBytes = 0;
		uint64_t sceneVertexUploadBytes = 0;
		uint64_t sceneIndexUploadBytes = 0;
		uint64_t scenePrimitiveUploadBytes = 0;
		uint64_t sceneMaterialUploadBytes = 0;
		uint64_t sceneDataUploadBytes = 0;
		uint64_t emissiveUploadBytes = 0;
		uint64_t residentChunkBatchVertexBytes = 0;
		uint64_t residentChunkBatchIndexBytes = 0;
		uint64_t residentChunkBatchPrimitiveBytes = 0;
		uint64_t residentChunkBatchMaterialBytes = 0;
	};

	static constexpr uint32_t TraceShaderScalarStatCount = NRI_TRACE_SHADER_SCALAR_STAT_COUNT;
	static constexpr uint32_t TraceShaderInstanceBucketCount = NRI_TRACE_SHADER_INSTANCE_BUCKET_COUNT;
	static constexpr uint32_t TraceShaderRayKindCount = NRI_TRACE_SHADER_RAY_KIND_COUNT;
	static constexpr uint32_t TraceShaderInstanceCommittedBase = NRI_TRACE_SHADER_INSTANCE_COMMITTED_BASE;
	static constexpr uint32_t TraceShaderInstanceAcceptedBase = NRI_TRACE_SHADER_INSTANCE_ACCEPTED_BASE;
	static constexpr uint32_t TraceShaderInstanceKindCommittedBase = NRI_TRACE_SHADER_INSTANCE_KIND_COMMITTED_BASE;
	static constexpr uint32_t TraceShaderStatCount = NRI_TRACE_SHADER_STAT_COUNT;
	static constexpr uint32_t TraceShaderHotInstanceCount = NRI_TRACE_SHADER_HOT_INSTANCE_COUNT;
	using PerfTraceShaderHotInstance = NRITraceShaderHotInstance;
	using PerfTraceShaderStats = NRITraceShaderStatsSnapshot;

	struct MemoryTelemetry
	{
		uint64_t frameTextureBytes = 0;
		uint64_t sceneTextureBytes = 0;
		uint64_t skyTextureBytes = 0;
		uint64_t sceneBufferBytes = 0;
		uint64_t accelerationStructureBytes = 0;
		uint64_t totalTrackedBytes = 0;
		uint32_t renderWidth = 0;
		uint32_t renderHeight = 0;
		uint32_t outputWidth = 0;
		uint32_t outputHeight = 0;
	};

	struct LevelTransitionSnapshot
	{
		bool mapWorldValid = false;
		uint64_t mapWorldBuildSerial = 0;
		uint32_t mapWorldChunkCount = 0;
		uint32_t mapWorldSurfaceCount = 0;
		bool staticSceneValid = false;
		bool staticSceneTexturesResident = false;
		bool staticSceneBuffersResident = false;
		bool staticSceneAccelerationResident = false;
		uint64_t staticSceneBuildSerial = 0;
		uint32_t staticSceneChunkCount = 0;
		uint32_t staticSceneMaterialCount = 0;
		uint32_t textureCacheCount = 0;
		uint32_t skyTextureCacheCount = 0;
		uint32_t runtimeMutationChunkCount = 0;
		uint32_t runtimeMutationActiveChunkCount = 0;
		uint32_t runtimeMutationValidChunkCount = 0;
		bool residentChunkRegistryValid = false;
		uint32_t residentChunkRegistryEntryCount = 0;
		uint32_t residentChunkRegistryChunkCount = 0;
		uint32_t residentChunkRegistryActiveChunkCount = 0;
		uint32_t residentChunkRegistryMappedChunkCount = 0;
		uint32_t residentChunkRegistryAccelerationResidentChunkCount = 0;
		bool pendingStaticMapLightingInvalidation = false;
		bool surfaceProbeValid = false;
		bool surfaceProbeHit = false;
		int32_t surfaceProbeWallIndex = -1;
		int32_t surfaceProbeMapChunkIndex = -1;
		uint32_t transientMuzzleFlashSlotCount = 0;
		uint32_t transientMuzzleFlashActiveCount = 0;
		uint32_t analyticLightCount = 0;
		uint32_t manualLightCount = 0;
		uint32_t emissiveSurfaceCount = 0;
		uint32_t activeSectorLightCount = 0;
		uint32_t runtimeDebugSphereCount = 0;
		uint32_t runtimeTestLightCount = 0;
		uint32_t persistentVoxelMeshResources = 0;
		uint32_t persistentVoxelMaterialResources = 0;
		uint32_t persistentVoxelBatchActors = 0;
		uint32_t persistentVoxelActiveInstances = 0;
		uint32_t persistentVoxelInstanceRecords = 0;
		uint32_t persistentVoxelAdmissionQueue = 0;
		uint32_t persistentVoxelRequiredAdmissionPending = 0;
		uint32_t persistentVoxelRequiredAdmissionReady = 0;
		uint32_t persistentVoxelOptionalAdmissionPending = 0;
		uint32_t persistentVoxelFailedAdmission = 0;
		uint64_t persistentVoxelResidentBytes = 0;
		uint64_t persistentVoxelZeroRefBytes = 0;
		uint32_t persistentVoxelColdMeshes = 0;
		uint32_t persistentVoxelColdMaterials = 0;
		uint64_t persistentVoxelColdPrimitives = 0;
		uint32_t persistentVoxelResidencyGeneration = 0;
		uint64_t persistentVoxelResidencyBuildSerial = 0;
		uint32_t persistentVoxelLastDesired = 0;
		uint32_t persistentVoxelLastDesiredPreload = 0;
		uint32_t persistentVoxelLastDesiredActor = 0;
		uint32_t persistentVoxelLastGpuReady = 0;
		uint32_t persistentVoxelLastRetained = 0;
		uint32_t persistentVoxelLastQueued = 0;
		uint64_t persistentVoxelLastQueuedBytes = 0;
		uint32_t persistentVoxelLastMeshMissing = 0;
		uint32_t persistentVoxelLastMaterialOnly = 0;
		uint32_t persistentVoxelLastBlasOnly = 0;
		uint32_t persistentVoxelLastForced = 0;
		uint32_t persistentVoxelLastPreferred = 0;
		uint64_t sceneInstanceBufferBytes = 0;
		uint64_t visibleChunkBufferBytes = 0;
		uint64_t visibleFlatBufferBytes = 0;
		uint64_t reprojectionBufferBytes = 0;
		uint64_t dynamicScratchBufferBytes = 0;
		uint64_t worldTlasScratchBufferBytes = 0;
	};

	explicit NRIRenderer(NRIRenderDevice* frameBuffer);
	~NRIRenderer();

	bool Initialize();
	void Shutdown();
	bool RenderScene(HWDrawInfo& di, int drawmode, bool portal);
	bool PreloadLevelScene(uint32_t outputWidth, uint32_t outputHeight, uint32_t targetWidth, uint32_t targetHeight, bool frameTargetUsed, bool standaloneContextUsed);
	bool HasMaterialPreloadPending() const { return mPreloadMaterialStatus.pending; }
	const PreloadMaterialStatus& GetPreloadMaterialStatus() const { return mPreloadMaterialStatus; }
	void ResetHistory();
	void RequestAutoExposureReset(const char* reason);
	LevelTransitionSnapshot BuildLevelTransitionSnapshot() const;
	void TraceStartupMutationProbe(const char* event) const;
	void OnLevelFirstFrameRelease();
	void OnLevelUnloadBegin(const LevelTransitionInfo& info);
	void OnLevelUnloadComplete(const LevelTransitionInfo& info);
	void OnLevelLoadBegin(const LevelTransitionInfo& info);
	void NotifyCameraCut(const char* reason);
	void SetGuiCaptureState(bool active);
	void PrintStatus();
	void PrintSwapChainRenderConfig() const;
	void PrintSceneBufferStatus() const;
	void PrintSceneLightDump(float radius, uint32_t limit) const;
	bool AddRuntimePointLight(const float position[3], const float color[3], float intensity, float radius, uint32_t& outId);
	bool UpdateRuntimePointLight(uint32_t id, const float position[3], const float color[3], float intensity, float radius);
	bool RemoveRuntimePointLight(uint32_t id);
	void ClearRuntimePointLights();
	void PrintRuntimePointLights() const;
	void PrintRuntimeLightClusterStatus() const;
	uint32_t GetRuntimePointLightCount() const;
	bool AddRuntimeDebugSphere(const float center[3], float diameter, float metalness, float roughness, uint32_t& outId);
	bool RemoveRuntimeDebugSphere(uint32_t id);
	void ClearRuntimeDebugSpheres();
	void PrintRuntimeDebugSpheres() const;
	uint32_t GetRuntimeDebugSphereCount() const;
	bool AddSpriteTileLightHeuristic(uint32_t textureId, const float color[3], float intensity, float radius, uint32_t flickerFrames, uint32_t& outRuleId);
	void ClearSpriteTileLightHeuristics();
	void PrintSpriteTileLightHeuristics() const;
	bool AddTextureEmissiveHeuristic(uint32_t textureId, uint32_t emissiveMode, float intensityScale, const float* emissiveColor, bool hasExplicitColor, uint32_t& outRuleId);
	void ClearTextureEmissiveHeuristics();
	void PrintTextureEmissiveHeuristics() const;
	void NotifyGlowControlChange();
	void NotifyMaterialLightingCalibrationChange();
	void NotifyAnalyticLightSettingsChange();
	void NotifyDebugSphereTessellationChange();
	void PrintEmissiveSurfaceDump(float radius, uint32_t limit) const;
	void PrintSectorLightDump(float radius, uint32_t limit) const;
	void PrintSurfaceProbeStatus() const;
	NRISurfaceProbeStatusSnapshot BuildSurfaceProbeStatusSnapshot() const;
	bool BuildEmissiveLightEditTarget(PathTracingEmissiveLightEditTarget& outTarget) const;
	bool BuildSurfaceLightEditTarget(PathTracingEmissiveLightEditTarget& outTarget) const;
	bool ProjectEditorLineToScreen(const float renderStart[3], const float renderEnd[3],
		float outStart[2], float outEnd[2]) const;
	void PrintMapChunkDump(int32_t chunkIndex) const;
	NRIMapChunkDumpSnapshot BuildMapChunkDumpSnapshot(int32_t chunkIndex) const;
	void PrintMapChunkCompare(int32_t chunkIndex) const;
	NRIMapChunkCompareSnapshot BuildMapChunkCompareSnapshot(int32_t chunkIndex) const;
	void TraceActorSpriteEvent(const PathTracingActorSpriteTraceEvent& event);
	bool IsPathTracingSupported() const { return mPathTracingSupported; }
	uint32_t GetLastCompletedFrameIndex() const;
	bool RefreshPathTracingAvailability();
	const char* GetAvailabilityReason() const;
	const PerfShellTraceStats& GetLastPerfShellTraceStats() const { return mLastPerfShellTraceStats; }
	const PerfResourceTraceStats& GetLastPerfResourceTraceStats() const { return mLastPerfResourceTraceStats; }
	const PerfTraceShaderStats& GetLastPerfTraceShaderStats() const { return mLastPerfTraceShaderStats; }
	const NRIVoxelRepresentationSnapshot& GetVoxelRepresentationSnapshot() const { return mVoxelRepresentationPolicy.GetSnapshot(); }
	NRISE29FloorDeformerRouteFrameStats GetSE29FloorDeformerRouteFrameStats() const;
	NRIMapMaterialOnlyRouteFrameStats GetMapMaterialOnlyRouteFrameStats() const;
	nri_scene::PTMapMaterialStateVariantStats GetMapMaterialVariantStats() const;
	MemoryTelemetry GetMemoryTelemetry() const;
	static const char* GetMaterialBuildTraceSlotName(MaterialBuildTraceSlot slot);
	enum class FrameTextureSlot : uint32_t
	{
		ViewZ,
		Motion,
		NormalRoughness,
		BaseColorMetalness,
		UnfilteredDiffuse,
		UnfilteredSpecular,
		UnfilteredPenumbra,
		DenoisedDiffuse,
		DenoisedSpecular,
		DenoisedShadow,
		Composed,
		TraceTransparentOutput,
		DirectLighting,
		DirectEmission,
		TaaHistoryPing,
		TaaHistoryPong,
		Validation,
		SrInput,
		RrInput,
		UpscalerDepth,
		RrGuideDiffuseAlbedo,
		RrGuideSpecularAlbedo,
		RrGuideSpecularHitDistance,
		RrGuideNormalRoughness,
		SmokeVolumeCurrent,
		SmokeVolumeCurrentMeta,
		SmokeVolumeHistoryPing,
		SmokeVolumeHistoryPong,
		SmokeVolumeMetaPing,
		SmokeVolumeMetaPong,
		RrVolumeInput,
		VendorOutput,
		PostSharpenOutput,
		PostVolumeOutput,
		PostBloomOutput,
		BloomPyramid0,
		BloomPyramid1,
		BloomPyramid2,
		BloomPyramid3,
		BloomPyramid4,
		BloomPyramid5,
		BloomPyramid6,
		BloomPyramid7,
		Final,
		Count
	};

	enum class ExposureDomain : uint32_t
	{
		SceneHDR,
		PreExposedHDR,
		DisplayMappedOutput
	};

	static constexpr size_t BloomDescriptorSetCount = 18;

	struct ExposureRoute
	{
		ExposureDomain inputDomain = ExposureDomain::SceneHDR;
		float temporalExposure = 1.0f;
		float presentExposure = 1.0f;
	};

	enum class PipelineSlot : uint32_t
	{
		TraceOpaque,
		TraceOpaqueCache,
		Composition,
		TraceTransparent,
		ExposureHistogramClear,
		ExposureHistogramBuild,
		ExposureResolve,
		Taa,
		RawPresent,
		FinalPresent,
		DlssSrBefore,
		DlssBefore,
		DlssAfter,
		Final,
		BloomCopy,
		VoxelComputeCount,
		VoxelComputeEmit,
		VoxelComputeClassify,
		VoxelComputeScan,
		VoxelComputeEmitParallel,
		VoxelComputeFinalize,
		BloomDownsample,
		BloomUpsample,
		BloomComposite,
		Count
	};

	uint64_t GetRecordingFrameFenceValue() const;
	bool IsFrameFenceValueComplete(uint64_t fenceValue) const;
	uint64_t GetRecordingCommandFenceValue() const;
	bool IsCommandFenceValueComplete(uint64_t fenceValue) const;
	void PrintSmokeStatus() const;
	void ResetSmoke(const char* reason = "console");
	void QueueSyntheticSmoke();

private:
	friend class NRIExposurePassAccess;
	friend class NRIAccelerationStructureManager;
	friend class NRIDescriptorSetManager;
	friend class NRIFrameResources;
	friend NRIIndirectRadianceCacheServices BuildNRIIndirectRadianceCacheServices(NRIRenderer& renderer);
	friend class NRIPipelineStateManager;
	friend class NRISmokeSystem;
	friend class NRIPreloadCoordinator;
	friend class NRIPersistentVoxelServiceFactory;
	friend class NRISceneUploadManager;
	friend NRIRuntimeMutationResidentUploadServices BuildNRIRuntimeMutationResidentUploadServices(NRIRenderer& renderer);
	friend NRIRuntimeMutationOverlayServices BuildNRIRuntimeMutationOverlayServices(NRIRenderer& renderer);
	friend NRIRuntimeMutationResidentApplyServices BuildNRIRuntimeMutationResidentApplyServices(NRIRenderer& renderer);
	friend NRIRuntimeMutationResidentSceneRefreshServices BuildNRIRuntimeMutationResidentSceneRefreshServices(NRIRenderer& renderer);
	friend bool EnsureNRIRendererAutoExposureResources(NRIRenderer& renderer, const NRIAutoExposureSettings& settings);
	bool PumpPersistentVoxelBlasCompaction(uint64_t buildSerial);
	void ResetPersistentVoxelBlasCompaction();
	struct PersistentVoxelBlasCompactionState
	{
		enum class Stage : uint8_t
		{
			Idle,
			QueryPending,
			CopyPending,
			Complete,
			Failed
		};
		Stage stage = Stage::Idle;
		uint64_t buildSerial = 0;
		uint64_t queryFence = 0;
		uint64_t copyFence = 0;
		uint64_t originalBytes = 0;
		uint64_t compactedBytes = 0;
		nri::QueryPool* queryPool = nullptr;
		NRIBufferResource readbackBuffer;
		std::vector<NRIAccelerationStructureResource*> sources;
		std::vector<NRIAccelerationStructureResource> destinations;
	};
	PersistentVoxelBlasCompactionState mPersistentVoxelBlasCompaction;
	friend void DestroyNRIRendererAutoExposureResources(NRIRenderer& renderer);
	friend bool UpdateNRIRendererAutoExposureDescriptorSets(NRIRenderer& renderer, uint32_t sourceSlot);
	friend bool DispatchNRIRendererAutoExposure(NRIRenderer& renderer, uint32_t sourceSlot);
	friend void CopyNRIRendererAutoExposureStatsForReadback(NRIRenderer& renderer, uint64_t frameNumber);
	friend void ReadbackNRIRendererAutoExposureStats(NRIRenderer& renderer);
	friend void DispatchNRIVoxelComputeMeshingDiagnostics(NRIRenderer& renderer, uint64_t frameNumber);
	friend void DestroyNRIVoxelComputeMeshingDiagnostics(NRIRenderer& renderer);

	NRIResourceContext BuildResourceContext() const;
	NRIResourceServices BuildResourceServices();
	NRIRendererFrameContext BuildFrameContext(int drawmode, bool portal, int debugMode, bool preserveHistory) const;
	NRIPassDispatchContext BuildPassDispatchContext(bool mainViewEligible = false);

	RenderSceneHistorySnapshot CaptureRenderSceneHistorySnapshot(bool preserveHistory) const;
	void RestoreRenderSceneHistorySnapshot(const RenderSceneHistorySnapshot& snapshot);
	bool EnsureRenderSceneFrameResources(const NRIRendererFrameContext& frameContext, bool preserveHistory, const RenderSceneHistorySnapshot& history);
	bool BeginRenderSceneFrame(HWDrawInfo& di, const NRIRendererFrameContext& frameContext, bool preserveHistory, const RenderSceneHistorySnapshot& history);
	bool RenderSimpleBootstrapView(bool preserveHistory, const RenderSceneHistorySnapshot& history);
	bool BuildRenderSceneFrame(HWDrawInfo& di, const RenderSceneFrameBuildInputs& inputs, const RenderSceneHistorySnapshot& history, RenderSceneFrameBuildResult& frame);
	bool DispatchSelectedRenderScene(const RenderSceneDispatchInputs& inputs);
	void LogRenderSceneFailureReasons(bool paletteReady, bool texturesReady, bool buffersReady, bool accelerationReady, bool dispatched, bool bootstrapCapturedView);
	void CommitRenderSceneResult(const RenderSceneCompletionInputs& inputs, const RenderSceneHistorySnapshot& history);
	void RecordRenderSceneSuccessStats(const RenderSceneCompletionInputs& inputs);
	void EmitRenderSceneTemporalTrace(uint32_t traceFrameIndex);

	using SceneBufferDebugStats = ::SceneBufferDebugStats;

	struct SelectPrimitiveRewriteCache
	{
		bool valid = false;
		uint64_t primitivePayloadHash = 0;
		uint64_t primitiveProvenanceHash = 0;
		uint64_t visibilityIdentityHash = 0;
		uint64_t primitiveCount = 0;
		std::vector<nri_scene::PrimitiveData> primitives;
	};

	struct PrimitiveVisibilityIdentityCache
	{
		bool valid = false;
		bool mapValid = false;
		uint64_t mapBuildSerial = 0;
		uint32_t chunkCount = 0;
		uint32_t statsChunkCount = 0;
		uint64_t identity = 0;
	};

	struct DynamicOverlayBlasAsset
	{
		uint64_t key = 0;
		uint32_t vertexCount = 0;
		uint32_t indexCount = 0;
		uint32_t primitiveCount = 0;
		uint64_t lastUsedFrame = 0;
		NRIBufferResource vertexBuffer = {};
		NRIBufferResource indexBuffer = {};
		NRIAccelerationStructureResource accelerationStructure = {};
		SceneBufferDebugStats vertexStats = { "DynamicOverlayBLASVertex" };
		SceneBufferDebugStats indexStats = { "DynamicOverlayBLASIndex" };
	};

	struct DynamicOverlayBlasRoute
	{
		struct Occurrence
		{
			const NRIAccelerationStructureResource* accelerationStructure = nullptr;
			SceneBufferUploadDomainSpan span = {};
		};

		std::vector<Occurrence> occurrences;
		bool routeAllOverlay = false;
	};

	struct SelectedDynamicOverlayBlasOccurrence
	{
		const NRIAccelerationStructureResource* accelerationStructure = nullptr;
		uint32_t sceneInstanceIndex = UINT32_MAX;
		uint32_t primitiveOffset = 0;
		uint32_t primitiveCount = 0;
	};

	using SceneUploadDirtyRange = ::SceneUploadDirtyRange;

	using RuntimePointLightGpuData = NRIRuntimePointLightGpuData;

	using RuntimeLightTileHeaderGpuData = NRIRuntimeLightTileHeaderGpuData;

	using EmissivePrimitiveHeaderGpuData = NRIEmissivePrimitiveHeaderGpuData;
	using EmissivePrimitiveGpuData = NRIEmissivePrimitiveGpuData;
	using EmissiveMaterialResponseGpuData = NRIEmissiveMaterialResponseGpuData;
	using EmissivePrimitiveDebugRecord = NRIEmissivePrimitiveDebugRecord;
	using EmissiveSamplingBuildContext = SceneLightSystem::EmissiveSamplingBuildContext;

	using SectorLightHeaderGpuData = NRISectorLightHeaderGpuData;
	using SectorLightGpuData = NRISectorLightGpuData;

	using StaticMapChunkAtlas = ::StaticMapChunkAtlas;

	using PersistentDynamicEmissiveCache = SceneLightSystem::PersistentDynamicEmissiveCache;
	using PersistentDynamicEmissiveHighWaterStats = SceneLightSystem::PersistentDynamicEmissiveHighWaterStats;

	using PersistentVoxelBatch = ::PersistentVoxelBatch;

	using PersistentVoxelMeshVariantResource = ::PersistentVoxelMeshVariantResource;
	using PersistentVoxelMaterialVariantResource = ::PersistentVoxelMaterialVariantResource;
	using PersistentVoxelAdmissionState = ::PersistentVoxelAdmissionState;
	using PersistentVoxelAdmissionEntry = ::PersistentVoxelAdmissionEntry;
	using PersistentVoxelReadinessStatus = ::PersistentVoxelReadinessStatus;
	using PersistentVoxelAdmissionStats = ::PersistentVoxelAdmissionStats;
	using PersistentVoxelInstanceRecord = ::PersistentVoxelInstanceRecord;

	using ActorSpriteDebugStats = SceneLightSystem::ActorSpriteDebugStats;
	using PersistentDynamicSurfaceStats = SceneLightSystem::PersistentDynamicSurfaceStats;

	using RuntimeMutationCacheStats = ::RuntimeMutationCacheStats;

	struct DescriptorCoherencyDebugStats
	{
		uint64_t actorMaterialBuilds = 0;
		uint64_t sceneTextureSetUpdates = 0;
		uint64_t sceneDataSetUpdates = 0;
		uint64_t lastMaterialBridgeHash = 0;
		uint64_t lastActorSpriteMaterialHash = 0;
		uint64_t lastSceneTextureDescriptorHash = 0;
		uint64_t lastSceneDataDescriptorHash = 0;
		uint32_t lastMaterialCount = 0;
		uint32_t lastTextureCount = 0;
		uint32_t lastActorSpriteSurfaceCount = 0;
		uint32_t lastActorSpriteActorCount = 0;
		uint32_t lastSceneTextureDescriptorCount = 0;
		uint32_t lastSceneDataDescriptorCount = 0;
		uint32_t lastSceneTextureQueuedFrameIndex = 0;
		uint32_t lastSceneDataQueuedFrameIndex = 0;
		uint32_t lastSceneTextureOutstandingQueuedFrames = 0;
		uint32_t lastSceneDataOutstandingQueuedFrames = 0;
		uint64_t lastSceneTextureQueuedFrameFence = 0;
		uint64_t lastSceneDataQueuedFrameFence = 0;
		uint64_t lastSceneTextureSubmittedFence = 0;
		uint64_t lastSceneDataSubmittedFence = 0;
		std::string lastMaterialBuildLabel;
		std::string lastSceneTextureReason;
		std::string lastSceneDataReason;
	};

	using RuntimeMapMutationCache = ::RuntimeMapMutationCache;
	using RuntimeMutationResidentApplyMode = ::RuntimeMutationResidentApplyMode;

	struct StartupMutationProbeState
	{
		bool valid = false;
		bool detectedMaterialOnly = false;
		uint64_t frameIndex = 0;
		uint32_t chunkCount = 0;
		uint32_t visibleChunkCount = 0;
		uint32_t candidateCount = 0;
		uint32_t candidateActiveReplacementCount = 0;
		uint32_t candidateVisibleResidentValidationCount = 0;
		uint32_t candidateStartupVisibleValidationCount = 0;
		uint32_t candidateUnresolvedAuthoredTextureCount = 0;
		uint32_t candidateStaticAnimatedSuppressedCount = 0;
		uint32_t candidateSectorDirtyCount = 0;
		uint32_t candidateSectionDirtyCount = 0;
		uint32_t candidateDraggedCount = 0;
		uint32_t candidateSignatureWatchlistCount = 0;
		uint32_t candidateBackgroundSweepCount = 0;
		uint32_t candidateDeferredMaterialRefreshCount = 0;
		uint32_t candidateDeferredStructuralRebuildCount = 0;
		uint32_t dirtyChunkCount = 0;
		uint32_t startupMaterialOnlyDirtyChunkCount = 0;
	};

	struct ResidentBufferUploadScratch
	{
		NRIBufferResource buffer;
		uint64_t cursor = 0;
		bool copySourceActive = false;
	};

	struct ResidentUploadScratchFrame
	{
		uint64_t frameIndex = UINT64_MAX;
		ResidentBufferUploadScratch vertex;
		ResidentBufferUploadScratch index;
		ResidentBufferUploadScratch primitive;
		ResidentBufferUploadScratch material;
		std::vector<NRIBufferResource> retiredBuffers;
		std::vector<NRIAccelerationStructureResource> retiredAccelerationStructures;
	};

	struct SceneDataDescriptorSnapshot
	{
		nri::DescriptorSet* sceneDataSet = nullptr;
		NRIBufferResource sceneInstanceBuffer;
		SceneBufferDebugStats sceneInstanceStats = { "SceneDataSnapshotSceneInstance" };
		uint64_t retireFenceValue = 0;
		uint64_t publishedMapEpoch = 0;
		uint64_t publishedBuildEpoch = 0;
		bool descriptorsInitialized = false;
	};

	using RuntimeMutationResidentUploadRange = ::RuntimeMutationResidentUploadRange;
	using RuntimeMapMutationFrameState = ::RuntimeMapMutationFrameState;

	using SceneInstanceData = ::SceneInstanceData;

	using StaticMapSceneResources = ::StaticMapSceneResources;

	using SceneUploadBufferRingSlot = ::SceneUploadBufferRingSlot;
	using SceneDataFrameSlot = ::NRISceneDataFrameSlot;

	enum SceneDataBufferMask : uint32_t
	{
		SceneDataBufferMask_None = 0,
		SceneDataBufferMask_Static = 1 << 0,
		SceneDataBufferMask_Dynamic = 1 << 1,
	};

	bool UseFallbackSceneTextures(bool preserveExistingSky, const char* reason = nullptr);
	bool EnsurePaletteTexture(const nri_scene::MaterialBridgeData& materials);
	uint32_t FindSceneTextureCacheIndex(uint64_t key) const;
	bool EnsureSceneTextureCacheEntry(const nri_scene::TextureUpload& upload, double* outRealizeMs = nullptr);
	bool EnsureSceneTextures(const nri_scene::SceneView& sceneView, const nri_scene::MaterialBridgeData& materials, std::vector<nri_scene::MaterialData>& outGpuMaterials, bool preserveExistingSky, const char* reason = nullptr, const NRISceneTextureFrameReuseInputs* reuseInputs = nullptr, NRISceneTextureMissPolicy missPolicy = NRISceneTextureMissPolicy::Synchronous, std::vector<uint32_t>* outDeferredMaterialIndices = nullptr);
	void ResolveSceneMaterialTextureSlots(const nri_scene::MaterialBridgeData& materials, std::vector<nri_scene::MaterialData>& gpuMaterials) const;
	bool EnsureSkyTexture(const nri_scene::SceneView& sceneView, bool preserveExistingSky);
	bool EnsureStaticMapScene();
	void ResetResidentMapChunkRegistry();
	void SyncResidentMapChunkRegistryFromStaticScene();
	bool EnsureResidentStaticMapChunkAtlasBufferCapacity(const StaticMapChunkAtlas& atlas);
	bool RefreshResidentStaticMaterialSlices(
		const std::vector<uint32_t>& chunkListIndices,
		const char* reason,
		const std::vector<uint32_t>* animatedApplyChunkListIndices = nullptr);
	bool RefreshStaticMapAnimatedMaterials();
	bool UploadSceneBuffers(
		const nri_scene::GeometryData& geometry,
		const std::vector<nri_scene::MaterialData>& materials,
		const std::vector<SceneBufferUploadDomainSpan>* domainSpans = nullptr);
	bool UploadSceneBuffers(
		SceneUploadBufferRingSlot& uploadSlot,
		const nri_scene::GeometryData& geometry,
		const std::vector<nri_scene::MaterialData>& materials,
		const std::vector<SceneBufferUploadDomainSpan>* domainSpans = nullptr);
	bool BuildStaticMapAccelerationStructures();
	bool BuildStaticMapAccelerationStructures(
		StaticMapSceneCache& staticScene,
		StaticMapSceneResources& staticResources,
		bool updateLiveState);
	bool BuildTopLevelAccelerationStructure(const std::vector<nri::TopLevelInstance>& instances, uint32_t sceneBufferMask);
	bool BuildTopLevelAccelerationStructure(
		const std::vector<nri::TopLevelInstance>& instances,
		uint32_t sceneBufferMask,
		const std::vector<SceneInstanceData>& sceneInstances);
	bool BuildTopLevelAccelerationStructure(
		const std::vector<nri::TopLevelInstance>& instances,
		uint32_t sceneBufferMask,
		NRIAccelerationStructureResource& topLevelAS,
		NRIBufferResource& tlasInstanceBuffer,
		NRIBufferResource& topLevelScratchBuffer,
		const NRIBufferResource* staticVertexBuffer,
		const NRIBufferResource* staticIndexBuffer,
		uint32_t* outTlasInstanceCount,
		bool updateLiveState,
		bool tlasInstanceWritesQuiesced);
	bool EnsureTopLevelAccelerationStructureCapacity(uint32_t instanceCount);
	bool BuildEmissiveTopLevelAccelerationStructure();
	bool BuildDynamicAccelerationStructure(const nri_scene::GeometryData& geometry);
	bool BuildDynamicAccelerationStructure(
		const nri_scene::GeometryData& geometry,
		uint32_t indexOffset,
		uint32_t indexCount,
		uint32_t primitiveCount,
		NRIAccelerationStructureResource& outAccelerationStructure,
		bool updateDynamicPerfStats);
	bool BuildDynamicOverlayBlasRoute(
		const nri_scene::GeometryData& geometry,
		const std::vector<SceneBufferUploadDomainSpan>& uploadSpans,
		DynamicOverlayBlasRoute& outRoute);
	void ResetDynamicOverlayBlasCache();
	bool BuildBottomLevelAccelerationStructure(
		const NRIBufferResource& vertexBuffer,
		const NRIBufferResource& indexBuffer,
		uint32_t vertexOffset,
		uint32_t vertexCount,
		uint32_t indexOffset,
		uint32_t indexCount,
		uint32_t primitiveCount,
		NRIAccelerationStructureResource& outAccelerationStructure,
		bool updateDynamicPerfStats,
		NRIBufferResource* buildScratchBuffer = nullptr,
		nri::AccelerationStructureBits buildFlags = nri::AccelerationStructureBits::PREFER_FAST_BUILD);
	void NoteWorldBlasContentChanged();
	bool PreloadStaticMapResources();
	bool PreloadPersistentVoxelResources();
	bool PreloadMaterialResources();
	bool PreGrowLevelSceneResourcesForLoading();
	bool EnsurePersistentVoxelBatch();
	bool UploadPersistentVoxelArenaMaterialBuffers(
		const std::vector<nri_scene::MaterialData>& materials,
		bool validateActiveMaterialPayloads = false);
	void InvalidateRuntimeLightSceneData();
	bool RefreshResidentStaticSceneDataSet();
	void NoteResidentStaticAtlasGrow();
	bool BuildRuntimeMapMutationOverlay(nri_scene::GeometryData& outGeometry, nri_scene::MaterialBridgeData& outMaterials, bool* outResidentStaticSceneChanged = nullptr);
	bool TryApplyRuntimeMutationChunkToResidentScene(
		const nri_scene::PTMapChunk& mapChunk,
		RuntimeMapMutationCache::ChunkReplacement& replacement,
		RuntimeMutationResidentApplyResult& outResult);
	bool RebuildResidentStaticMaterialState(const char* reason);
	bool RebuildResidentStaticMapChunkBlases(const std::vector<uint32_t>& chunkListIndices);
	bool BuildRuntimeSpaceLinkOverlay(HWDrawInfo& di, nri_scene::GeometryData& outGeometry, nri_scene::MaterialBridgeData& outMaterials);
	SceneLightSystem::RuntimeLightClusterBuildInput BuildRuntimeLightClusterInput() const;
	void TraceEmissiveSectorResponseChange();
	bool UpdateEmissiveSamplingBuffers(const EmissiveSamplingBuildContext& context, bool* ioWaitedForWrites = nullptr, bool allowSceneDataFrameSlot = false);
	void UpdateBoundSectorLightingState();
	void TraceActorSpriteMaterialAssignments(const nri_scene::SceneView& sceneView, const nri_scene::MaterialBridgeData& outMaterials, const char* traceLabel);
	void TraceSharedDescriptorRewrite(const char* setName, const char* reason, uint64_t descriptorHash, uint32_t descriptorCount, bool sceneTextureSet);
	uint32_t CountPotentialOutstandingQueuedFrames() const;
	uint32_t GetCurrentQueuedFrameIndex() const;
	SceneUploadBufferRingSlot& GetCurrentSceneUploadBufferRingSlot();
	const SceneUploadBufferRingSlot* GetCurrentSceneUploadBufferRingSlot() const;
	SceneDataFrameSlot& GetCurrentSceneDataFrameSlot();
	const SceneDataFrameSlot* GetCurrentSceneDataFrameSlot() const;
	bool ShouldUseSceneDataFrameRing() const;
	uint64_t GetSceneDataFrameRingCapacityBytes() const;
	void NoteSceneDataFrameRingTelemetry(const SceneDataFrameSlot* slot, bool enabled, bool fallback, bool overCap);
	NRIBufferResource& GetCurrentDynamicVertexBuffer();
	NRIBufferResource& GetCurrentDynamicIndexBuffer();
	NRIBufferResource& GetCurrentDynamicPrimitiveBuffer();
	NRIBufferResource& GetCurrentDynamicMaterialBuffer();
	NRIAccelerationStructureResource& GetCurrentDynamicBottomLevelAS();
	NRIBufferResource& GetCurrentTlasInstanceBuffer();
	NRIWorldTlasFrameSlot& GetCurrentWorldTlasFrameSlot();
	const NRIBufferResource& GetCurrentDynamicVertexBuffer() const;
	const NRIBufferResource& GetCurrentDynamicIndexBuffer() const;
	const NRIBufferResource& GetCurrentDynamicPrimitiveBuffer() const;
	const NRIBufferResource& GetCurrentDynamicMaterialBuffer() const;
	const NRIAccelerationStructureResource* GetCurrentDynamicBottomLevelAS() const;
	const NRIWorldTlasFrameSlot* GetCurrentWorldTlasFrameSlot() const;
	bool HasAnyDynamicBottomLevelAS() const;
	void DestroyDynamicBottomLevelAccelerationStructures();
	void DestroyWorldTlasFrameSlots();
	ResidentUploadScratchFrame& GetResidentUploadScratchFrame();
	void ResetResidentUploadScratchFrame(const char* reason);
	nri::DescriptorSet* GetCurrentSceneTextureSet() const;
	nri::DescriptorSet* GetCurrentSceneDataSet() const;
	bool IsCurrentSceneDataDescriptorsInitialized() const;
	void SetCurrentSceneDataDescriptorsInitialized(bool value);
	void BuildStaticMapInstances(std::vector<nri::TopLevelInstance>& outTlasInstances, std::vector<SceneInstanceData>& outSceneInstances) const;
	void BuildStaticMapInstances(const StaticMapSceneCache& staticScene, std::vector<nri::TopLevelInstance>& outTlasInstances, std::vector<SceneInstanceData>& outSceneInstances) const;
	void BuildStaticMapInstances(const StaticMapSceneCache& staticScene, const StaticMapChunkAtlas& atlas, std::vector<nri::TopLevelInstance>& outTlasInstances, std::vector<SceneInstanceData>& outSceneInstances) const;
	bool RestoreStaticTopLevelScene();
	bool ShouldRunAppTaaForFrameGraph(NRIMainUpscalerKind kind) const;
	void RefreshMapWorld();
	bool ApplyStartupMapWorldCorrectionIfNeeded(const char* trigger);
	void RebuildStartupMutationBaseline();
	bool CheckPathTracingSupport();
	void UpdatePerFrameState(HWDrawInfo& di, bool logicalMainView);
	void UpdateNightVisionState();
	void ResetSceneBufferFrameStats();
	void LogBridgeStats(const nri_scene::SceneDebugStats& stats);
	void PrintMapWorldStatus() const;
	void PrintPortalTraversalStatus() const;
	void PrintStaticMapSceneStatus() const;
	void PrintResidentMapChunkRegistryStatus() const;
	void PrintDynamicSceneStatus() const;
	void PrintTemporalStatus() const;
	void PrintRuntimeSpaceLinkStatus() const;
	void RequestHistoryReset(const char* reason, bool clearPreviousCameraState = false, bool clearRuntimeChunkTranslationHistory = false);
	void NoteLightHistoryChange(const char* reason);
	void ArmTemporalTraceBudget(const char* reason);
	void TraceTemporalState(const char* stage, NRIMainUpscalerKind resolvedMainUpscaler, NRIPostSharpenKind resolvedPostSharpen, bool runAppTaa, FrameTextureSlot primarySlot, FrameTextureSlot secondarySlot) const;
	ExposureDomain ResolveFrameTextureExposureDomain(FrameTextureSlot slot, NRIMainUpscalerKind mainKind, NRIPostSharpenKind postSharpenKind) const;
	ExposureRoute ResolveExposureRoute(FrameTextureSlot inputSlot, const NRIPTOutputPolicy& outputPolicy, NRIMainUpscalerKind mainKind, NRIPostSharpenKind postSharpenKind) const;
	const char* GetExposureDomainName(ExposureDomain domain) const;
	void ResetSelfTestRouteSnapshot();
	void SetSelfTestRouteSnapshot(const char* routeName, const char* presenterName, const char* ownerName, const char* passes, bool denoiserRun, bool upscalerRun, bool exposureRun);
	void EmitSelfTestSummary(uint32_t traceFrameIndex, int drawmode, bool portal) const;
	void TraceRuntimeLinkEvents(HWDrawInfo& di);
	void TraceSkyState(const nri_scene::SceneView& sceneView, const char* action, uint64_t resolvedKey);
	void UpdateSurfaceProbe(const nri_scene::GeometryData& geometry, const nri_scene::MaterialBridgeData* materials, bool allowLogging);
	NRISurfaceProbeEmissiveDiagnostics BuildSurfaceProbeEmissiveDiagnostics(const NRISurfaceProbeResult& probe) const;
	bool BuildSurfaceLightOverlay(
		nri_scene::SceneView& outSceneView,
		nri_scene::GeometryData& outGeometry,
		nri_scene::MaterialBridgeData& outMaterials,
		bool allowReuse,
		bool validateReuse);
	void RefreshSceneLightSystem(
		bool usedStaticMapScene,
		const nri_scene::SceneView* capturedSceneView,
		const nri_scene::MaterialBridgeData* capturedMaterials,
		const nri_scene::SceneView* dynamicSceneView,
		const nri_scene::MaterialBridgeData* dynamicMaterials,
		const nri_scene::SceneView* surfaceLightSceneView,
		const nri_scene::MaterialBridgeData* surfaceLightMaterials,
		bool appendPersistentVoxelSceneLights,
		const TArray<PathTracingWeaponLightEvent>* weaponEvents);
	void ResetMuzzleFlashOverlayState(const char* reason);
	void ResetPersistentDynamicEmissiveCache();
	SceneLightSystem::PersistentDynamicEmissiveCacheBuildServices BuildPersistentDynamicEmissiveCacheServices();
	void BuildMaterialsWithActorOverrides(nri_scene::SceneView& sceneView, nri_scene::MaterialBridgeData& outMaterials, const char* traceLabel = nullptr);
	void ApplyEmissiveMaterialOverrides(const nri_scene::MaterialBridgeData& materials, std::vector<nri_scene::MaterialData>& inOutGpuMaterials) const;
	void ApplyActorShadowMaterialOverrides(const nri_scene::MaterialBridgeData& materials, std::vector<nri_scene::MaterialData>& inOutGpuMaterials);
	uint64_t ComputeChunkActorOverrideHash(const nri_scene::MaterialBridgeData& materials);
	uint64_t ComputeChunkEmissiveOverrideHash(const nri_scene::MaterialBridgeData& materials) const;
	void QueueStaticMapSceneLightingInvalidation();
	void InvalidateStaticMapSceneForMaterialLighting();
	static MaterialBuildTraceSlot ResolveMaterialBuildTraceSlot(const char* traceLabel);
	const nri_material_policy::ActorMaterialOverrideMap& GetActorMaterialOverrideMapForFrame(MaterialBuildTraceSlot traceSlot = MaterialBuildTraceSlot::Unknown);
	void LogFallback(const char* reason);
	void CopyFinalToActiveTarget();
	void UpdateFrameGenerationFrameDesc();
	void UpdateFrameGenerationHistoryPolicy(int debugMode, const NRIFrameGenerationPolicy& frameGenPolicy, bool preserveHistory);
	bool HasFrameGenerationCadenceBreak() const;
	void NoteSuccessfulRealFrame();
	void CopyTexture(NRITextureResource& source, NRITextureResource& destination);
	void CopyTextureToActiveTarget(NRITextureResource& source);

	void DestroyCachedTextures();
	void DestroySceneBuffers();
	void DestroyAccelerationStructures();
	void DestroyStaticMapSceneCache(const char* reason = nullptr);
	void DestroyStaticMapSceneResources(StaticMapSceneCache& staticScene, StaticMapSceneResources& staticResources, bool waitForCommands);
	void DestroyBufferResource(NRIBufferResource& resource);
	void DestroyAccelerationStructureResource(NRIAccelerationStructureResource& resource);
	const NRIBufferResource& GetActiveVertexBuffer() const;
	const NRIBufferResource& GetActiveIndexBuffer() const;
	const NRIBufferResource& GetActivePrimitiveBuffer() const;
	const NRIBufferResource& GetActiveMaterialBuffer() const;
	bool BindSceneRootDescriptors();

	bool CreateStructuredBuffer(NRIBufferResource& resource, const void* data, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after);
	bool EnsureStructuredBuffer(NRIBufferResource& resource, SceneBufferDebugStats& stats, const void* data, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after, bool writesQuiesced = false, const char* waitReason = nullptr);
	bool UpdateStructuredBufferRange(NRIBufferResource& resource, uint64_t byteOffset, const void* data, uint64_t size, nri::AccessStage after);
	bool CreateBufferWithoutView(NRIBufferResource& resource, uint64_t size, uint32_t stride, nri::BufferUsageBits usage);
	bool CreateBufferWithoutViewAtLocation(NRIBufferResource& resource, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::MemoryLocation memoryLocation);
	bool EnsureResidentArenaBuffer(NRIBufferResource& resource, uint64_t requiredSize, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after);
	bool EnsureResidentUploadScratchBuffer(ResidentBufferUploadScratch& scratch, ResidentUploadScratchFrame& frameScratch, uint64_t requiredSize);
	bool EnsureResidentStructuredBuffer(NRIBufferResource& resource, SceneBufferDebugStats& stats, const void* data, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after, const char* waitReason, int uploadKind);
	bool StageResidentBufferCopyRange(NRIBufferResource& resource, uint64_t byteOffset, const void* data, uint64_t size, nri::AccessStage after, int uploadKind);
	NRIStaticSceneGeometryUploadServices BuildStaticSceneGeometryUploadServices();
	bool StageRuntimeMutationResidentGeometryUploadRanges(const std::vector<RuntimeMutationResidentUploadRange>& ranges);
	bool StageResidentMaterialUploadRanges(
		const NRIBufferResource& targetBuffer,
		const std::vector<RuntimeMutationResidentUploadRange>& ranges,
		const uint8_t* data,
		uint64_t availableBytes,
		uint32_t& batchCount,
		uint32_t& batchRangeCount,
		uint32_t& barrierCommandCount,
		uint32_t& copyCommandCount);
	void RefreshStateCommitCombinedGeometryStaticPrefixForResidentUpdate(const std::vector<uint32_t>& changedGeometryChunkListIndices);
	void RetireResidentBufferResource(NRIBufferResource& resource);
	void RetireResidentAccelerationStructure(NRIAccelerationStructureResource& resource);
	void RetireTopLevelAccelerationStructure(NRIAccelerationStructureResource& resource);
	bool UpdateSceneTextureSet(const std::vector<nri::Descriptor*>& descriptors, const char* reason = nullptr);
	bool EnsureAutoExposureResources(const NRIAutoExposureSettings& settings);
	void DestroyAutoExposureResources();
	bool UpdateAutoExposureDescriptorSets(FrameTextureSlot sourceSlot);
	bool DispatchAutoExposure(FrameTextureSlot sourceSlot);
	void CopyAutoExposureStatsForReadback(uint64_t frameNumber);
	void ReadbackAutoExposureStats();
	void PrepareSceneTextureInputsForCompute();
	void ResetPerfTraceStats();
	void WaitForCommandsTracked(const char* reason = nullptr);
	void ReleaseWorldAccelerationBuildScratch(const char* reason = nullptr);
	void NotePerfBufferUpload(const SceneBufferDebugStats* stats, uint64_t size, bool growth, const char* reason, int uploadKind);
	NRITextureResource& GetFrameTexture(FrameTextureSlot slot) { return mFrameTextures[(size_t)slot]; }
	const NRITextureResource& GetFrameTexture(FrameTextureSlot slot) const { return mFrameTextures[(size_t)slot]; }
	nri::Pipeline* GetPipeline(PipelineSlot slot) const { return mPipelines[(size_t)slot]; }
	NRIMainUpscalerKind GetSelectedMainUpscalerKind() const;
	NRIMainUpscalerKind ResolveMainUpscalerKind(bool logFallback);
	NRIMainUpscalerKind GetResolvedMainUpscalerKindForStatus() const;
	NRIPostSharpenKind GetSelectedPostSharpenKind() const;
	NRIPostSharpenKind ResolvePostSharpenKind(bool logFallback);
	NRIPostSharpenKind GetResolvedPostSharpenKindForStatus() const;
	nri::UpscalerMode GetSelectedUpscalerMode() const;
	bool IsMainUpscalerSupported(NRIMainUpscalerKind kind) const;
	bool IsPostSharpenSupported(NRIPostSharpenKind kind) const;
	void FillMatrix(float* outMatrix, const VSMatrix& matrix) const;
	const char* GetFrameTextureSlotName(FrameTextureSlot slot) const;

	NRIRenderDevice* mFrameBuffer = nullptr;
	std::unique_ptr<NRISmokeSystem> mSmoke;
	NRIWeaponEventBatch mWeaponEventBatch;
	nri::PipelineLayout* mPipelineLayout = nullptr;
	nri::PipelineLayout* mIndirectRadianceCachePipelineLayout = nullptr;
	nri::PipelineLayout* mTaaPipelineLayout = nullptr;
	nri::PipelineLayout* mPresentPipelineLayout = nullptr;
	nri::PipelineLayout* mExposurePipelineLayout = nullptr;
	nri::PipelineLayout* mBloomPipelineLayout = nullptr;
	nri::PipelineLayout* mVoxelComputePipelineLayout = nullptr;
	std::array<nri::Pipeline*, (size_t)PipelineSlot::Count> mPipelines = {};
	nri::DescriptorSet* mSamplerSet = nullptr;
	std::vector<nri::DescriptorSet*> mSceneTextureSets;
	std::vector<uint64_t> mSceneTextureKeyScratch;
	std::vector<uint64_t> mSceneTextureSetHashes;
	std::vector<uint8_t> mSceneTextureSetHashValid;
	bool mSceneTextureStableSlotsActive = false;
	std::vector<nri::DescriptorSet*> mSceneDataSets;
	std::vector<SceneDataDescriptorSnapshot> mSceneDataSnapshots;
	nri::DescriptorSet* mActiveSceneDataSet = nullptr;
	SceneDataDescriptorSnapshot* mActiveSceneDataSnapshot = nullptr;
	uint64_t mActiveSceneDataSetFrameIndex = UINT64_MAX;
	uint32_t mSceneDataSnapshotCursor = 0;
	nri::DescriptorSet* mFrameTextureSet = nullptr;
	nri::DescriptorSet* mOutputSet = nullptr;
	nri::DescriptorSet* mCompositionFrameTextureSet = nullptr;
	nri::DescriptorSet* mCompositionOutputSet = nullptr;
	nri::DescriptorSet* mUpscalerPrepassFrameTextureSet = nullptr;
	nri::DescriptorSet* mUpscalerPrepassOutputSet = nullptr;
	nri::DescriptorSet* mTaaFrameTextureSet = nullptr;
	nri::DescriptorSet* mTaaOutputSet = nullptr;
	nri::DescriptorSet* mRawPresentFrameTextureSet = nullptr;
	nri::DescriptorSet* mRawPresentOutputSet = nullptr;
	nri::DescriptorSet* mFinalPresentFrameTextureSet = nullptr;
	nri::DescriptorSet* mFinalPresentOutputSet = nullptr;
	std::array<nri::DescriptorSet*, BloomDescriptorSetCount> mBloomInputSets = {};
	std::array<nri::DescriptorSet*, BloomDescriptorSetCount> mBloomOutputSets = {};
	std::array<nri::DescriptorSet*, 2> mExposureInputSets = {};
	std::array<nri::DescriptorSet*, 2> mExposureOutputSets = {};
	std::array<nri::DescriptorSet*, 4> mVoxelComputeInputSets = {};
	std::array<nri::DescriptorSet*, 4> mVoxelComputeOutputSets = {};
	FrameTextureSlot mAutoExposureInputSourceSlot = FrameTextureSlot::Count;

	NRITextureResource* GetActiveSkyTexture() { return mSkyEnvironment.ActiveTexture(); }
	const NRITextureResource* GetActiveSkyTexture() const { return mSkyEnvironment.ActiveTexture(); }

	NRISceneTextureResidency mSceneTextures;
	NRISceneTextureFrameCache mSceneTextureFrameCache;
	NRIMaterialTextureWarmupCursor mStaticPreloadMaterialCursor = {};
	NRIMaterialTextureWarmupCursor mVoxelPreloadMaterialCursor = {};
	PreloadMaterialStatus mPreloadMaterialStatus = {};
	std::array<NRITextureResource, (size_t)FrameTextureSlot::Count> mFrameTextures = {};
	NRIExposureController mExposure;

	NRIBufferResource mVertexBuffer;
	NRIBufferResource mIndexBuffer;
	NRIBufferResource mPrimitiveBuffer;
	NRIBufferResource mMaterialBuffer;
	NRIBufferResource mStaticVertexBuffer;
	NRIBufferResource mStaticIndexBuffer;
	NRIBufferResource mStaticPrimitiveBuffer;
	NRIBufferResource mStaticMaterialBuffer;
	NRIWorldTlasFrameSlots mWorldTlasFrameSlots;
	NRIBufferResource mSceneInstanceBuffer;
	NRIBufferResource mPortalBuffer;
	NRIBufferResource mRuntimeLightBuffer;
	NRIBufferResource mRuntimeLightTileHeaderBuffer;
	NRIBufferResource mRuntimeLightTileIndexBuffer;
	NRIBufferResource mEmissivePrimitiveHeaderBuffer;
	NRIBufferResource mEmissivePrimitiveBuffer;
	NRIBufferResource mEmissivePrimitiveCdfBuffer;
	NRIBufferResource mEmissiveMaterialResponseBuffer;
	NRIBufferResource mEmissiveTlasInstanceBuffer;
	NRIBufferResource mSectorLightHeaderBuffer;
	NRIBufferResource mSectorLightBuffer;
	NRIBufferResource mReprojectionBuffer;
	NRIBufferResource mVisibleChunkBuffer;
	NRIBufferResource mVisibleFlatPlaneBuffer;
	NRIBufferResource mSpatialAbsenceBuffer;
	NRIBufferResource mSpatialAbsenceTypedBuffer;
	NRITraceShaderStats mTraceShaderStats;
	NRIIndirectRadianceCache mIndirectRadianceCache;
	NRIIndirectRadianceCacheTelemetrySnapshot mLastIndirectRadianceCacheTelemetry = {};
	NRIBufferResource mScratchBuffer;
	NRIBufferResource mResidentStaticBlasScratchBuffer;
	NRIBufferResource mEmissiveTopLevelScratchBuffer;
	SelectPrimitiveRewriteCache mSelectPrimitiveRewriteCache = {};
	PrimitiveVisibilityIdentityCache mPrimitiveVisibilityIdentityCache = {};
	NRISceneUploadProducerGenerations mSceneUploadProducerGenerations;
	NRISceneUploadIdentityValidator mSceneUploadIdentityValidator;
	std::vector<nri_scene::MaterialData> mSelectCapturedGpuMaterialScratch;
	std::vector<nri_scene::MaterialData> mSelectDynamicGpuMaterialScratch;
	std::vector<nri_scene::MaterialData> mSelectPersistentVoxelGpuMaterialScratch;
	std::vector<nri_scene::MaterialData> mSelectCombinedGpuMaterialScratch;
	std::vector<nri_scene::MaterialData> mSelectRefreshedCombinedGpuMaterialScratch;
	std::vector<uint32_t> mSelectDeferredTextureMaterialIndexScratch;
	nri_scene::GeometryData mSelectLocalPlayerReflectionGeometryScratch;
	nri_scene::GeometryData mSelectOverlayGeometryScratch;
	nri_scene::MaterialBridgeData mSelectOverlayMaterialBridgeScratch;
	NRISceneMaterialFrameCache mSceneMaterialFrameCache;
	NRISceneFrameGeometry mSceneFrameGeometry;
	std::vector<nri::TopLevelInstance> mSelectTopLevelInstanceScratch;
	std::vector<SceneInstanceData> mSelectSceneInstanceScratch;
	std::vector<nri::TopLevelInstance> mSelectCapturedTopLevelInstanceScratch;
	std::vector<SceneInstanceData> mSelectCapturedSceneInstanceScratch;
	std::vector<SceneUploadBufferRingSlot> mSceneUploadBufferRing;
	std::vector<SceneDataFrameSlot> mSceneDataFrameRing;
	uint64_t mSceneDataFrameRingHighWaterBytes = 0;
	uint32_t mSceneDataFrameRingFallbackCount = 0;
	uint32_t mSceneDataFrameRingOverCapCount = 0;
	uint32_t mSceneDataFrameRingSlotWaitCount = 0;
	uint32_t mSceneDataFrameRingDisabledFrameIndex = UINT32_MAX;
	uint32_t mSceneDataFrameRingOverCapFrameIndex = UINT32_MAX;
	std::vector<SceneUploadDirtyRange> mSceneUploadPrimitiveDirtyRangeScratch;
	std::vector<SceneUploadDirtyRange> mSceneUploadMaterialDirtyRangeScratch;
	std::vector<SceneUploadDirtyRange> mSceneUploadVertexDirtyRangeScratch;
	std::vector<SceneUploadDirtyRange> mSceneUploadIndexDirtyRangeScratch;
	std::vector<DynamicOverlayBlasAsset> mDynamicOverlayBlasAssets;
	std::vector<SelectedDynamicOverlayBlasOccurrence> mSelectedDynamicOverlayBlasOccurrences;
	std::vector<nri_scene::SceneVertex> mDynamicOverlayBlasVertexScratch;
	std::vector<uint32_t> mDynamicOverlayBlasIndexScratch;
	std::array<ResidentUploadScratchFrame, 3> mResidentUploadScratchFrames = {};
	std::vector<uint32_t> mResidentStaticBlasActiveChunkListIndices;
	std::vector<nri::BufferBarrierDesc> mResidentStaticBlasBarriers;
	SceneBufferDebugStats mVertexBufferStats = { "Vertex" };
	SceneBufferDebugStats mIndexBufferStats = { "Index" };
	SceneBufferDebugStats mPrimitiveBufferStats = { "Primitive" };
	SceneBufferDebugStats mMaterialBufferStats = { "Material" };
	SceneBufferDebugStats mSceneInstanceBufferStats = { "SceneInstance" };
	SceneBufferDebugStats mPortalBufferStats = { "Portal" };
	SceneBufferDebugStats mRuntimeLightBufferStats = { "RuntimeLight" };
	SceneBufferDebugStats mRuntimeLightTileHeaderBufferStats = { "RuntimeLightTileHeader" };
	SceneBufferDebugStats mRuntimeLightTileIndexBufferStats = { "RuntimeLightTileIndex" };
	SceneBufferDebugStats mEmissivePrimitiveHeaderBufferStats = { "EmissivePrimitiveHeader" };
	SceneBufferDebugStats mEmissivePrimitiveBufferStats = { "EmissivePrimitive" };
	SceneBufferDebugStats mEmissivePrimitiveCdfBufferStats = { "EmissivePrimitiveCdf" };
	SceneBufferDebugStats mEmissiveMaterialResponseBufferStats = { "EmissiveMaterialResponse" };
	SceneBufferDebugStats mEmissiveTlasInstanceBufferStats = { "EmissiveTLASInstance" };
	SceneBufferDebugStats mSectorLightHeaderBufferStats = { "SectorLightHeader" };
	SceneBufferDebugStats mSectorLightBufferStats = { "SectorLight" };
	SceneBufferDebugStats mReprojectionBufferStats = { "Reprojection" };
	SceneBufferDebugStats mVisibleChunkBufferStats = { "VisibleChunk" };
	SceneBufferDebugStats mVisibleFlatPlaneBufferStats = { "VisibleFlatPlane" };
	SceneBufferDebugStats mSpatialAbsenceBufferStats = { "SpatialAbsence" };
	SceneBufferDebugStats mSpatialAbsenceTypedBufferStats = { "SpatialAbsenceTyped" };
	PerfShellTraceStats mLastPerfShellTraceStats = {};
	PerfResourceTraceStats mLastPerfResourceTraceStats = {};
	PerfTraceShaderStats mLastPerfTraceShaderStats = {};
	uint64_t mPendingAutoExposureStatsFrame = 0;

	NRIAccelerationStructureResource mEmissiveTopLevelAS;

	NRISkyEnvironment mSkyEnvironment;
	NRINrdContext mNrd;
	NRIUpscalerContext mUpscaler;
	nri_scene::PTMapWorld mMapWorld;
	StaticMapSceneCache mStaticMapScene;
	StaticMapChunkAtlas mStaticMapChunkAtlas = {};
	NRIStaticSceneResidency mStaticSceneResidency;
	NRIStaticSceneDiagnosticsCache mStaticSceneDiagnostics;
	NRIMapMoverSystem mMapMovers;
	NRIMapMoverShadow mMapMoverShadow;
	NRISpatialAbsenceGate mSpatialAbsenceGate;
	NRISpatialAbsenceGpuSnapshot mSpatialAbsenceGpuSnapshot;
	uint32_t mSpatialAbsenceFormat = 0;
	uint32_t mSpatialAbsenceRayQueryCandidateInstanceCount = 0;
	NRIMapMoverRigidRoute mMapMoverRigidRoute;
	NRISE29FloorDeformerRoute mSE29FloorDeformerRoute;
	NRIMapMaterialOnlyRoute mMapMaterialOnlyRoute;
	NRIRuntimeMutationSystem mRuntimeMutation;
	NRISurfaceLightOverlayCache mSurfaceLightOverlayCache;
	DynamicSceneFrameState mDynamicSceneLastFrame = {};
	NRIPersistentVoxelResidency mPersistentVoxels;
	NRIVoxelRepresentationPolicy mVoxelRepresentationPolicy;
	StateCommitDomainGenerations mLastStateCommitDomainGenerations = {};
	bool mHasLastStateCommitDomainGenerations = false;
	nri_material_policy::ActorMaterialOverrideCache mActorMaterialOverrideCache = {};
	nri_material_policy::ActorOverlayMaterialRuleCache mActorOverlayMaterialRuleCache = {};
	DescriptorCoherencyDebugStats mDescriptorCoherencyDebugStats = {};
	RuntimeSpaceLinkFrameState mRuntimeSpaceLinkLastFrame = {};
	RuntimeLinkTraceState mLastRuntimeLinkTraceState = {};
	std::vector<RuntimeChunkTranslationState> mRuntimeChunkTranslationHistory;
	struct RuntimeRecurringChunkTracker
	{
		bool valid = false;
		uint32_t chunkIndex = UINT32_MAX;
		int32_t sectorIndex = -1;
		uint32_t lastReasonMask = 0;
		uint32_t visitCount = 0;
		uint32_t uniqueStateCount = 0;
		uint32_t transitionCount = 0;
		uint32_t repeatedStateHitCount = 0;
		uint32_t abaRecurrenceCount = 0;
		uint32_t lastWallCount = 0;
		uint32_t lastFlatCount = 0;
		uint32_t lastTriangleCount = 0;
		uint32_t lastMaterialCount = 0;
		uint64_t previousStateSignature = 0;
		uint64_t lastStateSignature = 0;
		std::array<uint64_t, 8> seenStateSignatures = {};
	};
	uint64_t mRuntimeRecurringChunkTrackerBuildSerial = 0;
	std::vector<RuntimeRecurringChunkTracker> mRuntimeRecurringChunkTrackers;
	nri_scene::SceneDebugStats mLastStats = {};
	NRIRendererDiagnostics mDiagnostics;
	NRIDebugOverlaySystem mDebugOverlays;
	SceneLightSystem mSceneLights;
	NRIDirectionalLightState mDirectionalLightState = {};
	NRIPTNightVisionState mNightVisionState = {};
	std::array<nri::Descriptor*, NRI_SCENE_DATA_DESCRIPTOR_NUM> mSceneDataDescriptors = {};
	// Retain the descriptor identities used by the current queued-frame scene
	// texture set so focused GPU workloads can bind the same resident textures
	// without duplicating or re-uploading texture payloads.
	std::vector<nri::Descriptor*> mCurrentSceneTextureDescriptors;
	std::array<nri::Descriptor*, 14> mFrameInputDescriptors = {};
	std::array<nri::Descriptor*, 15> mOutputDescriptors = {};
	std::vector<SceneInstanceData> mBoundSceneInstances;
	std::vector<uint32_t> mCurrentVisibleChunkWords;
	std::vector<uint32_t> mCurrentVisibleFlatPlaneWords;
	uint32_t mLastResolvedLightOverlayGeneration = 0;
	uint32_t mFrameIndex = 0;
	uint32_t mLastCompletedFrameIndex = ~0u;
	uint64_t mFrameGenerationFrameId = 0;
	uint32_t mRenderWidth = 0;
	uint32_t mRenderHeight = 0;
	uint32_t mOutputWidth = 0;
	uint32_t mOutputHeight = 0;
	uint32_t mTargetWidth = 0;
	uint32_t mTargetHeight = 0;
	int32_t mSceneLeft = 0;
	int32_t mSceneTop = 0;
	nri::Format mFinalSceneFormat = nri::Format::UNKNOWN;
	float mCurrentCameraPos[3] = {};
	float mCurrentCameraForward[3] = {};
	float mCurrentCameraRight[3] = {};
	float mCurrentCameraUp[3] = {};
	float mPreviousCameraPos[3] = {};
	float mPreviousCameraForward[3] = {};
	float mPreviousCameraRight[3] = {};
	float mPreviousCameraUp[3] = {};
	float mCurrentTanHalfFovX = 1.0f;
	float mCurrentTanHalfFovY = 1.0f;
	float mPreviousTanHalfFovX = 1.0f;
	float mPreviousTanHalfFovY = 1.0f;
	float mCurrentJitter[2] = {};
	float mPreviousJitter[2] = {};
	float mCurrentViewToClip[16] = {};
	float mPreviousViewToClip[16] = {};
	float mCurrentWorldToView[16] = {};
	float mPreviousWorldToView[16] = {};
	float mSkyColor[3] = { 0.38f, 0.48f, 0.65f };
	float mGroundColor[3] = { 0.08f, 0.08f, 0.08f };
	bool mHasLoggedStats = false;
	bool mHasPreviousCameraState = false;
	bool mHasFrameGenerationRealFrameTime = false;
	bool mHasPendingFrameGenerationRealFrameTime = false;
	bool mHasFrameGenerationTimestamp = false;
	bool mHasFrameGenerationConfigState = false;
	bool mHasDirectionalLightState = false;
	bool mPathTracingSupported = true;
	bool mGuiCaptureActive = false;
	bool mHasOutputPolicyState = false;
	bool mHasRuntimeLinkTraceState = false;
	NRISurfaceProbeFrameState mSurfaceProbeFrame = {};
	bool mResetHistory = true;
	std::string mLastHistoryResetReason = "startup";
	float mLastFrameGenerationRealFrameTimeMs = 0.0f;
	float mPendingFrameGenerationRealFrameTimeMs = 0.0f;
	std::chrono::steady_clock::time_point mLastFrameGenerationTimestamp = {};
	std::chrono::steady_clock::time_point mPendingFrameGenerationTimestamp = {};
	bool mLastFrameGenerationRequestedEnabled = false;
	NRIFrameGenerationProvider mLastFrameGenerationRequestedProvider = NRIFrameGenerationProvider::Off;
	NRIFrameGenerationUiMode mLastFrameGenerationResolvedUiMode = NRIFrameGenerationUiMode::Auto;
	bool mUseUpscaledInFinal = false;
	bool mLastTemporalAppTaaEnabled = false;
	bool mLastTemporalDenoiseEnabled = false;
	bool mHasTemporalExposureState = false;
	bool mHasAutoExposureSettingsState = false;
	bool mUseDenoisedCompositionInputs = false;
	bool mUseSplitShadowDenoiser = false;
	bool mHasLoggedFallback = false;
	bool mUsedStaticMapSceneLastFrame = false;
	bool mUsedDynamicSceneLastFrame = false;
	bool mGpuSceneHasDynamicOverlay = false;
	bool mUploadedStaticMapSceneLastFrame = false;
	bool mBuiltStaticMapSceneASLastFrame = false;
	bool mBuiltDynamicSceneASLastFrame = false;
	bool mPendingStaticMapLightingInvalidation = false;
	bool mAllowStartupMapWorldCorrection = false;
	bool mAllowStartupMutationRebaseline = false;
	bool mPendingStartupMutationRebaseline = false;
	StartupMutationProbeState mStartupMutationProbe = {};
	std::vector<uint8_t> mPendingStartupVisibleChunkValidation;
	uint64_t mObservedMapWorldBuildSerial = 0;
	uint64_t mStaticAccelerationBuildSerial = 0;
	uint32_t mStartupMapWorldCorrectionDeadlineFrame = 0;
	uint32_t mStartupMutationRebaselineDeadlineFrame = 0;
	uint32_t mActiveTlasInstanceCount = 0;
	uint32_t mBoundStaticPrimitiveCount = 0;
	uint32_t mBoundDynamicPrimitiveCount = 0;
	uint32_t mBoundStaticMaterialCount = 0;
	uint32_t mBoundDynamicMaterialCount = 0;
	uint32_t mBoundPortalCount = 0;
	uint32_t mBoundRuntimeLightCount = 0;
	uint32_t mBoundRuntimeLightTileCountX = 0;
	uint32_t mBoundRuntimeLightTileCountY = 0;
	uint32_t mBoundRuntimeLightTileSize = 0;
	uint32_t mBoundRuntimeLightTileIndexCount = 0;
	uint32_t mBoundRuntimeLightMaxTileOccupancy = 0;
	std::vector<uint8_t> mSceneDataDescriptorsInitialized;
	std::vector<uint64_t> mSceneDataDescriptorMapEpochs;
	std::vector<uint64_t> mSceneDataDescriptorBuildEpochs;
	bool mRuntimeLightPayloadCacheValid = false;
	uint64_t mRuntimeLightPayloadHash = 0;
	bool mRuntimeLightClusterCacheValid = false;
	uint64_t mRuntimeLightClusterPayloadHash = 0;
	uint64_t mRuntimeLightClusterCameraHash = 0;
	bool mRuntimeLightSceneDataDirty = false;
	bool mSceneInstancePayloadCacheValid = false;
	uint64_t mSceneInstancePayloadHash = 0;
	uint32_t mSceneInstancePayloadCount = 0;
	bool mPortalPayloadCacheValid = false;
	uint64_t mPortalPayloadHash = 0;
	uint64_t mPortalPayloadBuildSerial = 0;
	uint32_t mPortalPayloadCount = 0;
	uint64_t mSceneDataSnapshotGenerationCounter = 0;
	uint64_t mSceneDataSnapshotGeneration = 0;
	uint64_t mSceneDataSnapshotFrameIndex = UINT64_MAX;
	uint64_t mSceneDataSnapshotSceneInstanceHash = 0;
	uint64_t mSceneDataSnapshotTlasInstanceHash = 0;
	uint64_t mSceneDataSnapshotPortalHash = 0;
	uint32_t mSceneDataSnapshotQueuedFrameIndex = UINT32_MAX;
	uint32_t mSceneDataSnapshotSceneInstanceCount = 0;
	uint32_t mSceneDataSnapshotTlasInstanceCount = 0;
	uint32_t mSceneDataSnapshotPortalCount = 0;
	uint64_t mSceneDataDescriptorGeneration = 0;
	uint64_t mLastWorldTlasInstancePayloadHash = 0;
	uint64_t mLastWorldTlasSceneInstancePayloadHash = 0;
	uint64_t mLastWorldTlasInstanceFrameIndex = UINT64_MAX;
	uint32_t mLastWorldTlasInstanceCount = 0;
	uint64_t mWorldBlasContentGeneration = 1;
	uint32_t mBoundEmissivePrimitiveCount = 0;
	uint32_t mBoundEmissiveDominantPrimitive = UINT32_MAX;
	uint32_t mBoundEmissiveDominantTile = 0;
	uint32_t mBoundEmissiveDominantFlags = 0;
	uint32_t mBoundEmissiveDominantDataSource = 0;
	bool mEmissiveSamplingPayloadCacheValid = false;
	uint64_t mEmissiveSamplingPayloadHash = 0;
	bool mEmissiveStabilityTraceValid = false;
	uint64_t mEmissiveStabilityTopologyEpoch = 0;
	uint64_t mEmissiveStabilityDistributionEpoch = 0;
	uint64_t mEmissiveStabilityTopologyHash = 0;
	uint64_t mEmissiveStabilityOrderedKeyHash = 0;
	uint64_t mEmissiveStabilityLivePowerHash = 0;
	uint64_t mEmissiveStabilityProposalWeightHash = 0;
	uint64_t mEmissiveStabilityCdfHash = 0;
	std::vector<uint64_t> mEmissiveStabilityOrderedKeys;
	std::vector<float> mEmissiveStabilityCdf;
	bool mEmissiveSectorResponsePayloadCacheValid = false;
	uint64_t mEmissiveSectorResponsePayloadHash = 0;
	uint32_t mEmissiveTlasInstanceCount = 0;
	uint32_t mEmissiveTlasStaticInstanceCount = 0;
	uint32_t mEmissiveTlasDynamicInstanceCount = 0;
	uint32_t mEmissiveTlasBuildCount = 0;
	bool mEmissiveTlasInstancePayloadCacheValid = false;
	uint64_t mEmissiveTlasInstancePayloadHash = 0;
	float mBoundEmissiveTotalPower = 0.0f;
	float mBoundEmissiveDominantPower = 0.0f;
	std::vector<EmissivePrimitiveDebugRecord> mBoundEmissivePrimitiveRecords;
	bool mSectorLightingPayloadCacheValid = false;
	uint64_t mSectorLightingPayloadHash = 0;
	uint32_t mBoundSectorLightSectorCount = 0;
	uint32_t mBoundSectorLightActiveCount = 0;
	uint32_t mBoundSectorLightPulsingCount = 0;
	uint32_t mBoundSectorLightDominantSector = UINT32_MAX;
	float mBoundSectorLightDominantContribution = 0.0f;
	NRISurfaceProbeTracker mSurfaceProbe;
	int mLastDebugMode = -1;
	int mLastMainUpscalerRequest = -1;
	int mLastPostSharpenRequest = -1;
	NRIPTOutputMode mLastOutputRequestedMode = NRIPTOutputMode::SDR;
	NRIPTOutputMode mLastOutputResolvedMode = NRIPTOutputMode::SDR;
	float mLastTemporalExposure = 1.0f;
	NRIAutoExposureSettings mLastAutoExposureSettings = {};
	NRIMainUpscalerKind mLastMainUpscalerResolved = NRIMainUpscalerKind::Off;
	NRIPostSharpenKind mLastPostSharpenResolved = NRIPostSharpenKind::Off;
	NRIMainUpscalerKind mLastTemporalHistoryMainUpscaler = NRIMainUpscalerKind::Off;
	NRIPostSharpenKind mLastTemporalPostSharpen = NRIPostSharpenKind::Off;
	FrameTextureSlot mHistoryInputSlot = FrameTextureSlot::TaaHistoryPing;
	FrameTextureSlot mHistoryOutputSlot = FrameTextureSlot::TaaHistoryPong;
	FrameTextureSlot mUpscaledInputSlot = FrameTextureSlot::PostSharpenOutput;
};
