#pragma once

#include "nri_nrd.h"
#include "nri_smoke_visuals.h"

#include <array>
#include <cstdint>

struct NRITraceSettings
{
	uint32_t lightBounceCount = 4;
	uint32_t mirrorBounceCount = 8;
	uint32_t portalDepth = 8;
	uint32_t emissiveSampleCount = 4;
	uint32_t emissiveRequestedSampleCount = 4;
	uint32_t emissivePrimarySampleBudget = 0;
	uint32_t indirectSamplingMode = 0;
};

constexpr uint32_t NRIResolveIndirectSamplingMode(int32_t requested)
{
	return requested > 0 ? 1u : 0u;
}

static_assert(NRIResolveIndirectSamplingMode(0) == 0u);
static_assert(NRIResolveIndirectSamplingMode(1) == 1u);
static_assert(NRIResolveIndirectSamplingMode(7) == 1u);
static_assert(NRIResolveIndirectSamplingMode(-1) == 0u);

constexpr uint32_t NRIResolvePrimaryEmissiveSampleCount(uint32_t requested, uint32_t budget)
{
	requested = requested < 1u ? 1u : (requested > 4u ? 4u : requested);
	budget = budget > 4u ? 4u : budget;
	return budget == 0u || budget >= requested ? requested : budget;
}

static_assert(NRIResolvePrimaryEmissiveSampleCount(4u, 0u) == 4u);
static_assert(NRIResolvePrimaryEmissiveSampleCount(4u, 2u) == 2u);
static_assert(NRIResolvePrimaryEmissiveSampleCount(4u, 1u) == 1u);
static_assert(NRIResolvePrimaryEmissiveSampleCount(2u, 4u) == 2u);

struct NRIDenoiserSettings
{
	NRINrdDenoiserMode denoiserMode = NRINrdDenoiserMode::Reblur;
	uint32_t maxAccumulatedFrameNum = 0;
	uint32_t maxFastAccumulatedFrameNum = 0;
	uint32_t maxStabilizedFrameNum = 0;
	uint32_t sigmaMaxStabilizedFrameNum = 0;
	uint32_t hitDistanceReconstructionMode = 0;
	uint32_t inputSplitMode = 0;
	float fastHistoryClampingSigmaScale = 1.0f;
	float diffusePrepassBlurRadius = 0.0f;
	float specularPrepassBlurRadius = 0.0f;
	float minBlurRadius = 0.0f;
	float maxBlurRadius = 0.0f;
	float sigmaPlaneDistanceSensitivity = 0.001f;
	bool enableAntiFirefly = true;
	bool enableValidation = false;
};

struct NRIPersistentVoxelSettings
{
	uint32_t buildActors = 0;
	uint32_t buildPrimitives = 0;
	uint64_t buildBytes = 0;
	uint32_t texturePrewarms = 0;
	uint64_t textureBytes = 0;
	uint32_t runtimeBudgetMode = 0;
	uint32_t admissionLoadVariants = 0;
	uint64_t admissionLoadBytes = 0;
	uint32_t admissionRuntimeVariants = 0;
	uint64_t admissionRuntimeBytes = 0;
	uint32_t admissionGraceFrames = 0;
	uint32_t admissionGraceVariants = 0;
	uint32_t preloadReadyGraceFrames = 0;
	uint64_t admitMaxBytesLoading = 0;
	uint64_t admitMaxBytesRuntime = 0;
	uint32_t admitMaxMsLoading = 0;
	uint32_t admitMaxMsRuntime = 0;
	uint32_t admitMaxBlasLoading = 0;
	uint32_t admitMaxBlasRuntime = 0;
	uint32_t admitMaxBlasPrimitives = 0;
	int32_t admitIsolateBlasPrimitives = 0;
	uint32_t computeMaxJobs = 0;
	uint64_t residentMaxBytes = 0;
	uint64_t residentMinHeadroomBytes = 0;
	uint32_t residentMaxColdMaps = 0;
	uint32_t pressureSafetyAuditFrames = 0;
	bool trimColdOnLoading = false;
	bool sharedBlasBuildEnabled = false;
	uint32_t sharedBlasBuildsPerFrame = 0;
	bool sharedBlasLoadingWarmupEnabled = false;
	bool sharedBlasRouteEnabled = false;
	bool shadowProxyBuildEnabled = false;
	bool shadowProxyRouteEnabled = false;
	uint32_t shadowProxyBuildsPerFrame = 0;
	uint32_t shadowProxyTransitionsPerFrame = 0;
	bool transformKeyed = false;
	bool diagnosticsEnabled = false;
	std::array<int32_t, 3> excludeIndices = { -1, -1, -1 };
	uint32_t excludeMinPrimitives = 0;
	bool omitTlasOccurrences = false;
};

struct NRIRuntimeMutationSettings
{
	bool worklistEnabled = true;
	uint32_t worklistSweepBudget = 32;
	bool deferFarMaterialRefreshes = true;
	bool deferNearInvisibleMaterialRefreshes = true;
	uint32_t nearInvisibleMaterialBudget = 4;
	bool deferFarStructuralRebuilds = true;
	uint32_t farStructuralBudget = 2;
	bool deferNearInvisibleStructuralRebuilds = true;
	uint32_t nearInvisibleStructuralBudget = 2;
	float nearDistance = 1024.0f;
};

struct NRISmokeSettings
{
	NRISmokeVisualSettings visuals = {};
	bool enabled = true;
	bool readback = false;
	bool viewCompare = false;
	uint32_t viewRoute = 0;
	uint32_t workProfile = 2;
	uint32_t quality = 2;
	uint32_t particleCapacity = 8192;
	uint32_t froxelPixelSize = 16;
	uint32_t froxelDepth = 48;
	uint32_t columnCapacity = 64;
	uint32_t simulationRate = 60;
	uint32_t maxSubsteps = 4;
	bool pointLights = true;
	bool directionalLight = true;
	bool emissiveLights = true;
	uint32_t emissiveReuseMode = 1;
	bool emissiveReference = false;
	uint32_t emissivePointCandidates = 4;
	int32_t emissiveCandidateTarget = -1;
	uint32_t emissiveBackend = 2;
	bool emissiveWorldFilter = false;
	bool emissiveLocalProposals = true;
	uint32_t emissiveWorldDebug = 0;
	uint32_t worldRadiancePartitions = 1;
	uint32_t worldRadianceNewCells = 8192;
	uint32_t worldRadianceMaintenanceCells = 32768;
	uint32_t worldRadianceMaximumAge = 16;
	bool emissiveLegacyGatherDisabled = false;
	bool emissiveQuarterKey = false;
	float emissiveSourceClamp = 32.0f;
	uint32_t directReuseMode = 2;
	uint32_t directReferenceMode = 1;
	bool volumeHistory = true;
	uint32_t dlrrMode = 1;
	bool indirect = false;
	uint32_t indirectCacheMode = 3;
	bool multipleScatter = false;
	float multipleScatterScale = 1.0f;
	uint32_t multipleScatterIterations = 0;
	uint32_t multipleScatterDebug = 0;
	bool selfShadow = false;
	uint32_t selfShadowDebug = 0;
	uint32_t lightMode = 3;
	uint32_t lightSamples = 4;
	uint32_t maxLightCandidates = 8;
	bool filteredVisibility = true;
	uint32_t debugMode = 0;
	uint32_t traceMode = 0;
	float froxelMaxDistance = 4096.0f;
	float timeScale = 1.0f;
	float wind[3] = { 5.0f, 20.0f, 5.0f };
	float densityScale = 5.0f;
	float radianceScale = 1.0f;
	float indirectScale = 1.0f;
	uint32_t representation = 1;
	uint32_t gridBrickCapacity = 512;
	bool dormantGrid = true;
	float gridCellSize = 8.0f;
	float gridBuoyancy = 1.0f;
	float gridVelocityDamping = 0.15f;
	float gridWindCoupling = 0.5f;
	float gridDensityHalfLifeScale = 1.0f;
	float gridCoolingScale = 1.0f;
	float gridMaxVelocity = 128.0f;
	float gridMaxBacktrace = 32.0f;
	float gridActiveThreshold = 0.0001f;
	uint32_t gridReclaimGrace = 120;
};

NRITraceSettings BuildNRITraceSettingsFromCVars();
NRIDenoiserSettings BuildNRIDenoiserSettingsFromCVars(uint32_t indirectSamplingMode = 0);
NRIPersistentVoxelSettings BuildNRIPersistentVoxelSettingsFromCVars();
NRIRuntimeMutationSettings BuildNRIRuntimeMutationSettingsFromCVars();
NRISmokeSettings BuildNRISmokeSettingsFromCVars();
