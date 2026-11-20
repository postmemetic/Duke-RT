#pragma once

#include "../scene/nri_scene_bridge.h"

#include <cstdint>
#include <vector>

class NRIPersistentVoxelResidency;

struct NRIVoxelComputePreloadSettings
{
	bool enabled = false;
	bool dryRun = true;
	int traceLevel = 0;
	bool includeRequired = true;
	bool includeOptional = false;
	bool preloadMaterials = false;
	bool strict = false;
	bool balancedOptional = false;
	int32_t balancedMaxPriority = 1;
	uint32_t balancedMinPrimitives = 100000;
	uint32_t runtimeProbeModulo = 0;
	uint32_t runtimeProbeRemainder = 0;
	uint32_t runtimeWithholdModulo = 0;
	uint32_t runtimeWithholdRemainder = 0;
	uint32_t maxMilliseconds = 0;
	uint32_t maxJobs = 0;
	uint32_t maxBlasBuilds = 0;
	uint64_t maxBytes = 0;
	uint32_t maxMaterialRows = 0;
	uint32_t watchdogMilliseconds = 0;
	uint32_t peakEstimatePercent = 175;
	uint64_t minimumLocalMemoryReserveBytes = 0;
};

struct NRIVoxelStartupGeometryInventory
{
	uint64_t requiredBytes = 0;
	uint64_t optionalPreloadBytes = 0;
	uint64_t withheldBytes = 0;
	uint64_t largestPreloadBytes = 0;
	uint64_t largestWithheldBytes = 0;
};

struct NRIVoxelComputePreloadStats
{
	bool emitted = false;
	bool enabled = false;
	bool dryRun = true;
	bool actionReady = false;
	uint32_t variants = 0;
	uint32_t required = 0;
	uint32_t optional = 0;
	uint32_t selected = 0;
	uint32_t selectedRequired = 0;
	uint32_t selectedOptional = 0;
	uint32_t skippedDisabled = 0;
	uint32_t skippedRequiredOff = 0;
	uint32_t skippedOptionalOff = 0;
	uint32_t skippedByteBudget = 0;
	uint32_t skippedJobBudget = 0;
	uint32_t skippedMaterialBudget = 0;
	uint32_t uniqueMeshes = 0;
	uint32_t uniqueMaterials = 0;
	uint32_t directOnly = 0;
	uint32_t surfaceReady = 0;
	uint32_t sourceReady = 0;
	uint32_t materialContextReady = 0;
	uint32_t meshResident = 0;
	uint32_t materialResident = 0;
	uint32_t blasReady = 0;
	uint32_t ready = 0;
	uint32_t notReady = 0;
	uint32_t materialRowsPlanned = 0;
	uint64_t estimatedGeometryBytes = 0;
	uint64_t selectedGeometryBytes = 0;
	uint64_t residentSceneBytes = 0;
	uint64_t residentAsBytes = 0;
	double planMs = 0.0;
	uint32_t rawVariants = 0;
	uint32_t rawRequired = 0;
	uint32_t rawOptional = 0;
	uint32_t rawSelected = 0;
	uint32_t rawSelectedRequired = 0;
	uint32_t rawSelectedOptional = 0;
	uint32_t rawUniqueMeshes = 0;
	uint32_t rawUniqueMaterials = 0;
	uint32_t rawSourceResident = 0;
	uint32_t rawSourceMissing = 0;
	uint32_t rawMaterialContextReady = 0;
	uint32_t rawMaterialContextMissing = 0;
	uint32_t rawMaterialRequiredKeys = 0;
	uint32_t rawMaterialOptionalKeys = 0;
	uint32_t rawMaterialSelectedKeys = 0;
	uint32_t rawMaterialActorScopedKeys = 0;
	uint32_t rawMaterialTextureRefs = 0;
	uint32_t rawCpuSurfaceReady = 0;
	uint32_t rawLegacyGpuCandidate = 0;
	uint32_t rawLegacyGpuSourceSkipped = 0;
	uint32_t rawSkippedDisabled = 0;
	uint32_t rawSkippedRequiredOff = 0;
	uint32_t rawSkippedOptionalOff = 0;
	uint32_t rawSkippedSourceMissing = 0;
	uint32_t rawSkippedMaterialMissing = 0;
	uint32_t rawSkippedByteBudget = 0;
	uint32_t rawSkippedJobBudget = 0;
	uint64_t rawEstimatedGeometryBytes = 0;
	uint64_t rawSelectedGeometryBytes = 0;
	uint32_t rawCandidateBindings = 0;
	uint32_t rawCapSkippedBindings = 0;
	uint32_t rawFailedBindings = 0;
	uint32_t rawRuntimeWithheldBindings = 0;
	uint32_t rawRuntimeWithheldUniqueMeshes = 0;
	uint64_t rawRuntimeWithheldUniqueGeometryBytes = 0;
	uint64_t rawRuntimeWithheldManifestHash = 0;
	uint32_t rawBalancedRequiredBindings = 0;
	uint32_t rawBalancedImportanceBindings = 0;
	uint32_t rawBalancedLargeBindings = 0;
	uint32_t rawBalancedFilteredOptionalBindings = 0;
	uint64_t rawBalancedRequiredGeometryBytes = 0;
	uint64_t rawBalancedImportanceGeometryBytes = 0;
	uint64_t rawBalancedLargeGeometryBytes = 0;
	uint32_t rawRuntimeProbeUniqueMeshes = 0;
	uint64_t rawRuntimeProbeDigest = 0;
	uint32_t rawSelectedUniqueSources = 0;
	uint32_t rawSelectedUniqueMeshes = 0;
	uint32_t rawSelectedUniqueMaterials = 0;
	uint32_t rawSelectedUniqueTextures = 0;
	uint64_t rawSelectedUniqueGeometryBytes = 0;
	uint64_t rawSelectedLargestUniqueGeometryBytes = 0;
	uint64_t rawSelectedUniqueSourceBytes = 0;
	NRIVoxelStartupGeometryInventory startupGeometry;
	uint64_t currentTrackedBytes = 0;
	uint64_t localMemoryBudgetBytes = 0;
	uint64_t minimumLocalMemoryReserveBytes = 0;
	uint64_t estimatedPeakAdditionalBytes = 0;
	uint64_t estimatedPeakTotalBytes = 0;
	uint64_t manifestHash = 0;
	bool memoryGuardAvailable = false;
	bool memoryGuardHit = false;
	uint32_t manifestSources = 0;
	uint32_t manifestLines = 0;
	uint32_t manifestRequests = 0;
	uint32_t manifestSkippedInactive = 0;
	uint32_t manifestSkippedSyntax = 0;
	uint32_t manifestSkippedActor = 0;
	uint32_t manifestSkippedUnsupported = 0;
	uint32_t manifestDiscovered = 0;
	uint32_t manifestUniqueRequests = 0;
	uint32_t manifestSkippedInvalid = 0;
	uint32_t manifestSkippedDuplicate = 0;
};

struct NRIVoxelComputePreloadClosureStats
{
	bool valid = false;
	bool strictRequested = false;
	bool dryRun = false;
	bool memoryGuardHit = false;
	uint64_t sequence = 0;
	uint64_t buildSerial = 0;
	uint64_t manifestHash = 0;
	uint32_t selectedBindings = 0;
	uint32_t admittedBindings = 0;
	uint32_t readyBindings = 0;
	uint32_t reusedBindings = 0;
	uint32_t failedBindings = 0;
	uint32_t capSkippedBindings = 0;
	uint32_t staleCancelledBindings = 0;
	uint32_t runtimeWithheldBindings = 0;
	uint32_t runtimeWithheldUniqueMeshes = 0;
	uint32_t runtimeWithheldReadyMeshes = 0;
	uint32_t runtimeWithheldReadyMaterials = 0;
	uint32_t pendingBindings = 0;
	uint32_t selectedUniqueSources = 0;
	uint32_t selectedUniqueMeshes = 0;
	uint32_t readyUniqueMeshes = 0;
	uint32_t selectedUniqueMaterials = 0;
	uint32_t readyUniqueMaterials = 0;
	uint32_t selectedUniqueTextures = 0;
	uint32_t readyUniqueTextures = 0;
	uint32_t admissionQueueCount = 0;
	uint32_t computeInFlightCount = 0;
	uint32_t blasInFlightCount = 0;
	uint64_t cpuGeometryBuilds = 0;
	uint64_t cpuGeometryUploads = 0;
	uint64_t cpuGeometryUploadBytes = 0;
	uint64_t cpuGeometryFallback = 0;
	uint64_t fullGeometryReadbackBytes = 0;
	const char* outcome = "invalid";
};

NRIVoxelComputePreloadSettings BuildNRIVoxelComputePreloadSettingsFromCVars();

// Latches a guarded selection for one map build, shared by planning and enqueue.
void SelectNRIVoxelComputePreloadForBudget(
	std::vector<nri_scene::PrecachedVoxelVariantView>& variants,
	std::vector<nri_scene::PrecachedVoxelRawManifestView>& rawVariants,
	const NRIVoxelComputePreloadSettings& settings, uint64_t buildSerial,
	uint64_t currentTrackedBytes, uint64_t localMemoryBudgetBytes);

void BuildNRIVoxelComputePreloadDirectVariants(
	const std::vector<nri_scene::PrecachedVoxelRawManifestView>& rawVariants,
	const NRIVoxelComputePreloadSettings& settings,
	std::vector<nri_scene::PrecachedVoxelVariantView>& outVariants);

NRIVoxelComputePreloadStats PlanNRIVoxelComputePreload(
	const std::vector<nri_scene::PrecachedVoxelVariantView>& variants,
	const std::vector<nri_scene::PrecachedVoxelRawManifestView>& rawVariants,
	const nri_scene::PrecachedVoxelRawManifestStats& rawManifestStats,
	const NRIPersistentVoxelResidency& residency,
	const NRIVoxelComputePreloadSettings& settings,
	const char* levelName,
	uint64_t buildSerial,
	uint32_t frameIndex,
	const char* timelineStage,
	uint64_t currentTrackedBytes = 0,
	uint64_t localMemoryBudgetBytes = 0);

bool NRICompleteEnemyPreloadFits(uint64_t build, uint64_t budget, uint64_t usage, uint64_t reserve);
const nri_scene::PrecachedVoxelVariantView* NRIFindPlannedEnemyGeometry(uint64_t build, uint64_t key);
const NRIVoxelComputePreloadStats& GetLastNRIVoxelComputePreloadStats();
bool IsNRIVoxelComputePreloadRuntimeWithheldMesh(uint64_t buildSerial, uint64_t meshResourceKey);
bool IsNRIVoxelComputePreloadRuntimeProbeMesh(uint64_t buildSerial, uint64_t meshResourceKey);
bool IsNRIVoxelComputePreloadRuntimeTailReleased(uint64_t buildSerial);
void NotifyNRIVoxelComputePreloadRuntimeTailReleased(uint64_t buildSerial, uint32_t frameIndex);
NRIVoxelComputePreloadClosureStats BuildNRIVoxelComputePreloadClosure(
	const NRIPersistentVoxelResidency& residency,
	uint64_t buildSerial);
