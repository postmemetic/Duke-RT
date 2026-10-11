#pragma once

#include "nri_resources.h"
#include "nri_scene_lights.h"
#include "nri_runtime_mutation_worklist.h"
#include "nri_runtime_mutation_discovery.h"

#include "../scene/nri_geometry_bridge.h"
#include "../scene/nri_map_builder.h"
#include "../scene/nri_material_bridge.h"
#include "../scene/nri_scene_bridge.h"

#include <cstdint>
#include <string>
#include <vector>

class NRIRenderer;

static constexpr size_t RuntimeMutationTopTraceCount = 8;

enum class RuntimeMutationTraceAction : uint8_t
{
	None,
	StructuralRebuild,
	MaterialRefresh,
	ResidentApply,
	ResidentNoopSkip,
	ResidentFallback,
	Held,
	SyncSkip,
	DeferredMaterialRefresh,
	DeferredStructuralRebuild,
	Failed
};

struct RuntimeMutationTopTraceEntry
{
	bool valid = false;
	uint32_t score = 0;
	uint32_t chunkIndex = UINT32_MAX;
	int32_t sectorIndex = -1;
	uint32_t reasonMask = 0;
	uint32_t sectionDirtyCount = 0;
	uint32_t surfaceCount = 0;
	uint32_t triangleCount = 0;
	uint32_t materialCount = 0;
	RuntimeMutationTraceAction action = RuntimeMutationTraceAction::None;
	bool forceTopology = false;
	bool residentMaterialDirty = false;
	bool residentGeometryDirty = false;
	bool recoveredEmpty = false;
};

enum RuntimeMutationWorklistCandidateSourceBits : uint32_t
{
	RuntimeMutationWorklistCandidateSource_ActiveReplacement = 1 << 0,
	RuntimeMutationWorklistCandidateSource_VisibleResidentValidation = 1 << 1,
	RuntimeMutationWorklistCandidateSource_StartupVisibleValidation = 1 << 2,
	RuntimeMutationWorklistCandidateSource_UnresolvedAuthoredTextures = 1 << 3,
	RuntimeMutationWorklistCandidateSource_StaticAnimatedSuppressed = 1 << 4,
	RuntimeMutationWorklistCandidateSource_SectorDirty = 1 << 5,
	RuntimeMutationWorklistCandidateSource_SectionDirty = 1 << 6,
	RuntimeMutationWorklistCandidateSource_Dragged = 1 << 7,
	RuntimeMutationWorklistCandidateSource_SignatureWatchlist = 1 << 8,
	RuntimeMutationWorklistCandidateSource_BackgroundSweep = 1 << 9,
	RuntimeMutationWorklistCandidateSource_DeferredMaterialRefresh = 1 << 10,
	RuntimeMutationWorklistCandidateSource_DeferredStructuralRebuild = 1 << 11,
	RuntimeMutationWorklistCandidateSource_MotionSettle = 1 << 12,
	RuntimeMutationWorklistCandidateSource_ValidationRecovery = 1 << 13,
};

const char* GetRuntimeMutationTraceActionName(RuntimeMutationTraceAction action);
uint32_t ScoreRuntimeMutationTopTraceEntry(const RuntimeMutationTopTraceEntry& entry);
std::string GetRuntimeMapMutationReasonSummary(uint32_t reasonMask);
std::string GetRuntimeMutationWorklistCandidateSourceSummary(uint32_t sourceMask);
bool IsRuntimeMutationMaterialOnlyReasonMask(uint32_t reasonMask);
bool RequiresExclusiveRuntimeMutationMaterialOnlyReasonMask(uint32_t reasonMask);
bool IsPureSectorRuntimeMutationMaterialOnlyReasonMask(uint32_t reasonMask);

struct RuntimeMutationCacheStats
{
	uint32_t activeChunkCount = 0;
	uint32_t validChunkCount = 0;
	uint32_t excludedStaticChunkCount = 0;
	uint32_t cachedSurfaceCount = 0;
	uint32_t cachedTriangleCount = 0;
	uint32_t cachedMaterialCount = 0;
	uint32_t cachedMaterialStateCount = 0;
};

struct RuntimeMutationResidentElementSpan
{
	uint32_t firstElement = 0;
	uint32_t elementCount = 0;
};

struct RuntimeMapMutationCache
{
	struct ChunkReplacement
	{
		struct MaterialStateCacheEntry
		{
			uint64_t animatedGeometrySignature = 0;
			uint64_t animatedMaterialSignature = 0;
			uint32_t surfaceCount = 0;
			uint64_t canonicalMaterialStateKey = 0;
			std::vector<uint64_t> canonicalMaterialStateKeys;
			nri_scene::MaterialBridgeData materialBridge;
		};

		nri_scene::PTMapChunkMutationBaseline baseline;
		nri_scene::PTMapChunkMutationBaseline replacementBaseline;
		uint64_t baselineSignature = 0;
		uint64_t liveSignature = 0;
		uint64_t exactGeometrySignature = 0;
		uint64_t animatedMaterialSignature = 0;
		uint64_t lastTraceSignature = UINT64_MAX;
		uint64_t lastTraceAnimatedMaterialSignature = UINT64_MAX;
		uint32_t reasonMask = 0;
		uint32_t sectionDirtyCount = 0;
		uint32_t stableMutationFrameCount = 0;
		uint32_t lastTraceReasonMask = UINT32_MAX;
		uint32_t traceCount = 0;
		bool active = false;
		bool valid = false;
		bool residentAuthoritative = false;
		bool sectorDirty = false;
		bool dragged = false;
		bool blindSpot = false;
		bool excludeStaticChunk = false;
		bool staticAnimatedReplacement = false;
		bool lastTraceActive = false;
		bool lastTraceBlindSpot = false;
		bool animationOnlyRefreshed = false;
		bool lastTraceAnimationOnlyRefreshed = false;
		bool lastTraceStaticAnimatedReplacement = false;
		bool deferredMaterialRefresh = false;
		uint64_t deferredMaterialFrame = 0;
		bool deferredStructuralRebuild = false;
		uint64_t deferredStructuralFrame = 0;
		bool fixedLayoutDeformer = false;
		uint64_t fixedLayoutDeformerKey = 0;
		std::vector<RuntimeMutationResidentElementSpan> fixedLayoutVertexSpans;
		std::vector<RuntimeMutationResidentElementSpan> fixedLayoutPrimitiveSpans;
		// Proof produced by the canonical material-state route. This is the only
		// way an otherwise-exclusive wall-material replacement may reuse resident
		// geometry. Resident apply revalidates every captured atlas fact before
		// consulting this bit.
		bool certifiedResidentMaterialOnly = false;
		uint64_t certifiedMaterialBuildSerial = 0;
		uint64_t certifiedMaterialMapEpoch = 0;
		uint64_t certifiedMaterialOwnerStableId = UINT64_MAX;
		uint64_t certifiedMaterialLayoutKey = 0;
		uint64_t certifiedMaterialStateKey = 0;
		uint64_t certifiedExactGeometrySignature = 0;
		uint64_t certifiedGeometryTopologySignature = 0;
		uint64_t certifiedPrimitiveLayoutSignature = 0;
		uint32_t certifiedChunkListIndex = UINT32_MAX;
		uint32_t certifiedVertexOffset = 0;
		uint32_t certifiedVertexCount = 0;
		uint32_t certifiedIndexOffset = 0;
		uint32_t certifiedIndexCount = 0;
		uint32_t certifiedPrimitiveOffset = 0;
		uint32_t certifiedPrimitiveCount = 0;
		uint32_t certifiedMaterialOffset = 0;
		uint32_t certifiedMaterialCount = 0;
		uint32_t surfaceCount = 0;
		uint32_t triangleCount = 0;
		SceneLightSystem::SurfaceIdentityOverrides lightIdentityOverrides;
		nri_scene::SceneView sceneView;
		nri_scene::GeometryData geometry;
		nri_scene::MaterialBridgeData materialBridge;
		std::vector<MaterialStateCacheEntry> materialStateCache;
	};

	std::vector<ChunkReplacement> chunks;
};

struct RuntimeMutationResidentApplyMode
{
	bool materialOnlyReplacement = false;
	bool exclusiveMaterialOnlyReplacement = false;
	bool fastResidentMaterialOnlyUpdate = false;
	bool certifiedResidentMaterialOnlyUpdate = false;
};

struct RuntimeMutationResidentApplyModeStats
{
	uint32_t materialOnlyCount = 0;
	uint32_t fastMaterialOnlyCount = 0;
	uint32_t slowMaterialOnlyCount = 0;
	uint32_t materialOnlyExclusiveCount = 0;
	uint32_t certifiedMaterialOnlyCount = 0;
	uint32_t materialOnlyNoResidentChunkCount = 0;
	uint32_t materialOnlyInvalidReplacementCount = 0;
	uint32_t materialOnlyMaterialCountMismatchCount = 0;
	uint32_t structuralCount = 0;
};

struct RuntimeMutationResidentUploadRange
{
	int uploadKind = 0;
	uint64_t byteOffset = 0;
	uint64_t size = 0;
	uint64_t dirtySize = 0;
};

struct NRIRuntimeMutationResidentUploadServices
{
	using StageGeometryRangesFn = bool (*)(void* user, const std::vector<RuntimeMutationResidentUploadRange>& ranges);
	using NoteUploadRangeFn = void (*)(void* user, int uploadKind, uint64_t size);
	using NoteCoalescedRangeFn = void (*)(void* user, const RuntimeMutationResidentUploadRange& range);
	using NoteCoalescedRejectFn = void (*)(void* user);

	void* user = nullptr;
	StageGeometryRangesFn stageGeometryRanges = nullptr;
	NoteUploadRangeFn noteUploadRange = nullptr;
	NoteCoalescedRangeFn noteCoalescedRange = nullptr;
	NoteCoalescedRejectFn noteCoalescedReject = nullptr;

	bool StageGeometryRanges(const std::vector<RuntimeMutationResidentUploadRange>& ranges) const;
	void NoteUploadRange(int uploadKind, uint64_t size) const;
	void NoteCoalescedRange(const RuntimeMutationResidentUploadRange& range) const;
	void NoteCoalescedReject() const;
};

struct RuntimeMutationResidentApplyResult
{
	uint32_t staticSceneChunkListIndex = UINT32_MAX;
	bool materialDirty = false;
	bool geometryDirty = false;
	uint32_t surfaceCount = 0;
	uint32_t triangleCount = 0;
	uint32_t materialCount = 0;
	bool recoveredEmpty = false;
};

struct RuntimeMutationResidentReplacementInfo
{
	uint32_t chunkListIndex = UINT32_MAX;
	nri_scene::PTMapChunkMutationBaseline baseline;
	uint64_t baselineSignature = 0;
	uint64_t liveSignature = 0;
};

struct NRIRuntimeMutationResidentApplyServices
{
	using TryApplyChunkFn = bool (*)(
		void* user,
		const nri_scene::PTMapChunk& mapChunk,
		RuntimeMapMutationCache::ChunkReplacement& replacement,
		RuntimeMutationResidentApplyResult& outResult);

	void* user = nullptr;
	TryApplyChunkFn tryApplyChunk = nullptr;

	bool TryApplyChunk(
		const nri_scene::PTMapChunk& mapChunk,
		RuntimeMapMutationCache::ChunkReplacement& replacement,
		RuntimeMutationResidentApplyResult& outResult) const;
};

struct NRIRuntimeMutationOverlayServices
{
	using BuildOverlayFn = bool (*)(
		void* user,
		nri_scene::GeometryData& outGeometry,
		nri_scene::MaterialBridgeData& outMaterials,
		bool* outResidentStaticSceneChanged);

	void* user = nullptr;
	BuildOverlayFn buildOverlay = nullptr;

	bool BuildOverlay(
		nri_scene::GeometryData& outGeometry,
		nri_scene::MaterialBridgeData& outMaterials,
		bool* outResidentStaticSceneChanged) const;
};

struct RuntimeMutationResidentSceneRefreshRequest
{
	bool sceneChanged = false;
	bool materialDirty = false;
	bool geometryDirty = false;
	std::vector<uint32_t>* materialChunkListIndices = nullptr;
	std::vector<uint32_t>* animatedMaterialChunkListIndices = nullptr;
	std::vector<uint32_t>* geometryChunkListIndices = nullptr;
};

struct RuntimeMutationResidentSceneRefreshResult
{
	bool residentStaticSceneGeometryChanged = false;
	const char* failureReason = nullptr;
};

struct NRIRuntimeMutationFrameOutput
{
	nri_scene::GeometryData geometry;
	nri_scene::MaterialBridgeData materialBridge;
	bool hasOverlay = false;
	bool residentStaticSceneChanged = false;
};

struct NRIRuntimeMutationResidentSceneRefreshServices
{
	using RefreshMaterialSlicesFn = bool (*)(void* user, const std::vector<uint32_t>& chunkListIndices, const std::vector<uint32_t>& animatedChunkListIndices);
	using NoteMaterialFallbackFn = void (*)(void* user, uint32_t chunkCount);
	using RebuildMaterialStateFn = bool (*)(void* user, const char* reason);
	using RebuildChunkBlasesFn = bool (*)(void* user, const std::vector<uint32_t>& chunkListIndices);
	using NoteBlasRebuildFn = void (*)(void* user, uint32_t chunkCount);
	using CommitSuccessFn = void (*)(void* user, const std::vector<uint32_t>& geometryChunkListIndices, bool materialDirty);
	using RecoverFailureFn = bool (*)(void* user, const char* reason);

	void* user = nullptr;
	RefreshMaterialSlicesFn refreshMaterialSlices = nullptr;
	NoteMaterialFallbackFn noteMaterialFallback = nullptr;
	RebuildMaterialStateFn rebuildMaterialState = nullptr;
	RebuildChunkBlasesFn rebuildChunkBlases = nullptr;
	NoteBlasRebuildFn noteBlasRebuild = nullptr;
	CommitSuccessFn commitSuccess = nullptr;
	RecoverFailureFn recoverFailure = nullptr;

	bool RefreshMaterialSlices(const std::vector<uint32_t>& chunkListIndices, const std::vector<uint32_t>& animatedChunkListIndices) const;
	void NoteMaterialFallback(uint32_t chunkCount) const;
	bool RebuildMaterialState(const char* reason) const;
	bool RebuildChunkBlases(const std::vector<uint32_t>& chunkListIndices) const;
	void NoteBlasRebuild(uint32_t chunkCount) const;
	void CommitSuccess(const std::vector<uint32_t>& geometryChunkListIndices, bool materialDirty) const;
	bool RecoverFailure(const char* reason) const;
};

struct RuntimeMapMutationFrameState
{
	bool active = false;
	uint32_t dirtyChunkCount = 0;
	uint32_t residentAppliedChunkCount = 0;
	uint32_t residentGeometryChunkCount = 0;
	uint32_t residentMaterialChunkCount = 0;
	uint32_t residentAtlasGrowCount = 0;
	uint32_t residentFallbackChunkCount = 0;
	uint32_t rebuiltChunkCount = 0;
	uint32_t heldChunkCount = 0;
	uint32_t blindSpotChunkCount = 0;
	uint32_t sectorGeometryChunkCount = 0;
	uint32_t sectorMaterialChunkCount = 0;
	uint32_t wallGeometryChunkCount = 0;
	uint32_t wallMaterialChunkCount = 0;
	uint32_t sectorDirtyChunkCount = 0;
	uint32_t sectionDirtyChunkCount = 0;
	uint32_t draggedChunkCount = 0;
	uint32_t animatedRefreshChunkCount = 0;
	uint32_t replacementSurfaceCount = 0;
	uint32_t replacementTriangleCount = 0;
	uint32_t materialCount = 0;
};

class NRIRuntimeMutationSystem
{
public:
	struct ChunkDiagnosticFacts
	{
		bool hasReplacement = false;
		bool active = false;
		bool valid = false;
		bool sectorDirty = false;
		bool dragged = false;
		bool blindSpot = false;
		uint32_t reasonMask = 0;
		std::string reasonSummary = "none";
		uint32_t sectionDirtyCount = 0;
		uint32_t surfaceCount = 0;
		uint32_t triangleCount = 0;
	};

	RuntimeMutationCacheStats GatherCacheStats() const;
	static NRIRuntimeMutationResidentUploadServices BuildResidentUploadServices(NRIRenderer& renderer);
	static NRIRuntimeMutationOverlayServices BuildOverlayServices(NRIRenderer& renderer);
	static NRIRuntimeMutationResidentApplyServices BuildResidentApplyServices(NRIRenderer& renderer);
	static NRIRuntimeMutationResidentSceneRefreshServices BuildResidentSceneRefreshServices(NRIRenderer& renderer);
	bool IsCacheEmpty() const;
	uint32_t GetCacheChunkCount() const;
	uint64_t BuildFrameGenerationHash(bool hasRuntimeMutationOverlay) const;
	ChunkDiagnosticFacts BuildChunkDiagnosticFacts(uint32_t chunkIndex) const;
	void CollectResidentReplacementInfo(
		uint32_t chunkCount,
		std::vector<RuntimeMutationResidentReplacementInfo>& outReplacements) const;
	const RuntimeMapMutationCache::ChunkReplacement* FindReplacement(uint32_t chunkIndex) const;
	RuntimeMapMutationCache::ChunkReplacement* FindReplacementForUpdate(uint32_t chunkIndex);
	bool IsReplacementActive(uint32_t chunkIndex) const;
	bool IsReplacementActiveAndValid(uint32_t chunkIndex) const;
	uint32_t AppendSceneLightRecords(SceneLightSystem& sceneLights) const;
	void BuildLightIdentityOverrides(
		const nri_scene::PTMapWorld& staticWorld,
		const nri_scene::PTMapChunk& staticChunk,
		const nri_scene::PTMapWorld& liveWorld,
		const nri_scene::PTMapChunk& liveChunk,
		SceneLightSystem::SurfaceIdentityOverrides& outOverrides) const;
	void PrintStatus() const;
	void ClearReplacementPayload(RuntimeMapMutationCache::ChunkReplacement& replacement, bool clearMaterialStateCache);
	void TraceChunk(
		const nri_scene::PTMapChunk& mapChunk,
		RuntimeMapMutationCache::ChunkReplacement& replacement,
		bool traceEnabled,
		int traceChunkIndex,
		int traceSectorIndex);
	void ClearResidentGeometryUploadRanges();
	bool QueueResidentGeometryUploadRange(
		int uploadKind,
		uint64_t byteOffset,
		uint64_t size,
		const NRIRuntimeMutationResidentUploadServices& services);
	bool FlushResidentGeometryUploadRanges(const NRIRuntimeMutationResidentUploadServices& services);
	bool TryApplyResidentChunk(
		const NRIRuntimeMutationResidentApplyServices& services,
		const nri_scene::PTMapChunk& mapChunk,
		RuntimeMapMutationCache::ChunkReplacement& replacement,
		RuntimeMutationResidentApplyResult& outResult);
	bool BuildOverlay(
		const NRIRuntimeMutationOverlayServices& services,
		nri_scene::GeometryData& outGeometry,
		nri_scene::MaterialBridgeData& outMaterials,
		bool* outResidentStaticSceneChanged = nullptr);
	bool BuildFrameOverlay(
		const NRIRuntimeMutationOverlayServices& services,
		NRIRuntimeMutationFrameOutput& outFrame);
	bool CommitResidentSceneRefresh(
		const NRIRuntimeMutationResidentUploadServices& uploadServices,
		const NRIRuntimeMutationResidentSceneRefreshServices& refreshServices,
		const RuntimeMutationResidentSceneRefreshRequest& request,
		RuntimeMutationResidentSceneRefreshResult& outResult);
	void NoteResidentAtlasGrow();
	void ResetCacheAndFrame();
	void ResetCacheForStaticSceneBuild(uint32_t chunkCount);
	void InitializeStaticChunkReplacement(const nri_scene::PTMapChunk& chunk);
	void ResetLevelLifecycleState();
	void ResetForMapWorldBuildFailure();
	void ResetLevelHighWaterStats();
	void BeginFrameState();
	void PrepareStartupBaseline(uint64_t buildSerial, uint32_t chunkCount);
	bool CanApplyStartupCorrection(uint32_t chunkCount) const;
	void ResetWorklist();
	void BeginWorklistFrame(uint32_t chunkCount);
	bool MarkWorklistCandidate(uint32_t chunkIndex, uint32_t sourceMask);
	uint32_t GetWorklistCandidateSourceMask(uint32_t chunkIndex) const;
	const std::vector<uint32_t>& GetWorklistCandidateSourceMasks() const;
	const std::vector<uint32_t>& GetWorklistCandidates() const;
	void ResetFrameState();
	void ResetHighWaterStats();
	void PrepareSignatureWatchlist(uint64_t buildSerial, uint32_t chunkCount);
	bool HasCacheChunkCount(uint32_t chunkCount) const;
	void EnsureSignatureWatchlist(uint64_t buildSerial, uint32_t chunkCount);
	bool SeedSignatureWatchlist(uint32_t chunkIndex);
	bool IsSignatureWatchlistSeeded(uint32_t chunkIndex) const;
	uint32_t GetSignatureWatchlistSeedCount() const;
	const std::vector<uint32_t>& GetSignatureWatchlistChunks() const { return signatureWatchlistChunks; }
	uint32_t GetWorklistSweepChunkIndex(uint32_t sweepOffset, uint32_t chunkCount) const;
	void AdvanceWorklistSweepCursor(uint32_t sweepCount, uint32_t chunkCount);
	void FinalizeFrameActive();
	bool HasStartupMaterialOnlyMutation() const;
	uint32_t GetDirtyChunkCount() const;
	uint32_t GetStartupMaterialOnlyDirtyChunkCount() const;
	void MarkFrameInactive();
	void NoteMutationReasonMask(uint32_t reasonMask);
	void NoteDirtyChunk(bool blindSpot);
	void NoteRebuiltChunk();
	void NoteHeldChunk();
	void NoteAnimatedRefreshChunk();
	void NoteResidentApply(bool materialDirty, bool geometryDirty);
	void NoteResidentFallback();
	void UpdateHighWaterStats(const RuntimeMutationCacheStats& cacheStats);
	RuntimeMutationResidentApplyMode ClassifyResidentApplyMode(
		const RuntimeMapMutationCache::ChunkReplacement& replacement,
		bool hasResidentChunk,
		bool hasResolvedAtlasChunk,
		uint32_t resolvedAtlasMaterialCount) const;
	RuntimeMutationResidentApplyModeStats BuildResidentApplyModeStats(
		const RuntimeMutationResidentApplyMode& mode,
		const RuntimeMapMutationCache::ChunkReplacement& replacement,
		bool hasResidentChunk,
		bool hasResolvedAtlasChunk,
		uint32_t resolvedAtlasMaterialCount) const;

	RuntimeMapMutationCache cache;
	RuntimeMapMutationFrameState lastFrame = {};
	RuntimeMutationCacheStats cacheHighWaterStats = {};
	std::vector<RuntimeMutationResidentUploadRange> residentGeometryUploadRanges;
	NRIRuntimeMutationWorklist worklist;
	NRIRuntimeMutationDiscovery discovery;
	std::vector<uint8_t> signatureWatchlist;
	std::vector<uint32_t> signatureWatchlistChunks;
	uint64_t signatureWatchlistBuildSerial = 0;
	uint32_t worklistSweepCursor = 0;
};
