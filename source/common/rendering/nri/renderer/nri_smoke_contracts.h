#pragma once

#include <cstddef>
#include <cstdint>

enum class NRISmokePass : uint32_t
{
	Clear = 0,
	Simulate,
	Spawn,
	Bin,
	LightDirectionalCarriers,
	EvaluateMedium,
	EvaluateGrid,
	LightPoint,
	LightDirectional,
	LightDirectTemporal,
	LightDirectSpatial,
	LightEmissiveInitial,
	LightEmissiveTemporal,
	LightEmissiveSpatial,
	LightIndirectReference,
	LightIndirectTemporal,
	LightIndirectSpatial,
	Integrate,
	ResolveVolume,
	TemporalVolume,
	Composite,
	EvaluateGridCompact,
	PromptFallback,
	AnalyticClear,
	AnalyticBuildTiles,
	AnalyticMaterialize,
	AnalyticEmissiveBuild,
	AnalyticEmissiveResolve,
	Count,
};

struct NRISmokeParticleGpu
{
	float position[3] = {};
	float radius = 0.0f;
	float velocity[3] = {};
	float age = 0.0f;
	float density = 0.0f;
	float lifetime = 0.0f;
	uint32_t styleIndex = 0;
	uint32_t epoch = 0;
	float initialDensity = 0.0f;
	float initialRadius = 0.0f;
	uint32_t serial = 0;
	uint32_t active = 0;
};

struct NRISmokeStyleGpu
{
	float albedo[3] = { 0.5f, 0.5f, 0.5f };
	float extinction = 1.0f;
	float anisotropy = 0.0f;
	float radius = 16.0f;
	float expansionVelocity = 8.0f;
	float lifetime = 3.0f;
	float density = 1.0f;
	float densityHalfLife = 1.5f;
	float riseVelocity = 8.0f;
	float velocityRandom = 2.0f;
	float velocityInherit = 0.0f;
	float buoyancy = 0.0f;
	float drag = 0.5f;
	float turbulence = 1.0f;
	float turbulenceScale = 32.0f;
	float temperature = 1.0f;
	float momentumScale = 1.0f;
	float coolingHalfLife = 2.0f;
};

enum class NRISmokeInjectionShape : uint32_t
{
	Sphere = 0,
	Rectangle = 1,
};

enum class NRISmokeInjectionSourceClass : uint32_t
{
	AmbientMap = 0,
	InteractiveActor = 1,
	InteractiveEvent = 2,
	Diagnostic = 3,
};

constexpr uint32_t NRI_SMOKE_SOURCE_METADATA_PROMPT_SLOT_SHIFT = 16u;
constexpr uint32_t NRI_SMOKE_SOURCE_METADATA_PROMPT_SLOT_MASK = 0x000f0000u;
constexpr uint32_t NRI_SMOKE_SOURCE_METADATA_PROMPT_ELIGIBLE = 0x00100000u;
constexpr uint32_t NRI_SMOKE_SOURCE_METADATA_ANALYTIC_BRIDGE = 0x00200000u;

struct NRISmokeInjectionCommandGpu
{
	float position[3] = {};
	float spawnRadius = 0.0f;
	float velocity[3] = {};
	uint32_t styleIndex = 0;
	uint32_t count = 1;
	uint32_t serial = 0;
	float densityScale = 1.0f;
	float radiusScale = 1.0f;
	float velocityCone = 0.0f;
	uint32_t epoch = 0;
	float halfAxisU[3] = {};
	uint32_t shape = static_cast<uint32_t>(NRISmokeInjectionShape::Sphere);
	float halfAxisV[3] = {};
	uint32_t sourceId = 0;
	uint32_t sourceSlot = UINT32_MAX;
	uint32_t sourceMetadata = 0;
	uint32_t rangeBegin = 0;
	uint32_t rangeCount = 0;
	uint32_t pulseIdLow = 0;
	uint32_t pulseIdHigh = 0;
};

struct NRISmokeControlGpu
{
	uint32_t writeCursor = 0;
	uint32_t activeApprox = 0;
	uint32_t liveEvictions = 0;
	uint32_t columnOverflow = 0;
	uint32_t epoch = 0;
	uint32_t spawned = 0;
	uint32_t expired = 0;
	uint32_t wideParticlesProjected = 0;
	uint32_t lightCandidatesTested = 0;
	uint32_t lightDistanceRejected = 0;
	uint32_t lightShadowRays = 0;
	uint32_t lightShadowVisible = 0;
	uint32_t lightShadowOccluded = 0;
	uint32_t lightSoftSamples = 0;
	uint32_t lightRadianceClamps = 0;
	uint32_t wideGlobalDrops = 0;
	uint32_t filterCandidateHits = 0;
	uint32_t filterAlphaRejects = 0;
	uint32_t filterNoShadowRejects = 0;
	uint32_t filterOneWayRejects = 0;
	uint32_t filterReflectionRejects = 0;
	uint32_t filterPortalContinuations = 0;
	uint32_t filterAcceptedBlockers = 0;
	uint32_t filterMisses = 0;
	uint32_t filterSkipLimitExits = 0;
	uint32_t filterContinuationLimitExits = 0;
	uint32_t filterResourceDowngrades = 0;
	uint32_t fineColumnReferences = 0;
	uint32_t wideCellReferences = 0;
	uint32_t referenceInvalidLinks = 0;
	uint32_t referenceTraversalLimitExits = 0;
	uint32_t globalDepthReferences = 0;
	uint32_t fineTierParticles = 0;
	uint32_t wideTierParticles = 0;
	uint32_t globalTierParticles = 0;
	uint32_t fineOccupiedCells = 0;
	uint32_t wideOccupiedCells = 0;
	uint32_t globalOccupiedSlices = 0;
	uint32_t fineMaximumCellReferences = 0;
	uint32_t wideMaximumCellReferences = 0;
	uint32_t globalMaximumCellReferences = 0;
	uint32_t emissiveInnerRisSets = 0;
	uint32_t emissiveInnerPointProposals = 0;
	uint32_t emissiveInnerZeroProposals = 0;
	uint32_t emissiveInnerRisRejects = 0;
	uint32_t emissiveInnerSelections = 0;
	uint32_t emissiveInnerVisibilityRays = 0;
	uint32_t emissiveInnerSourceVisibilityRays = 0;
	uint32_t emissiveInnerVisibilityVisible = 0;
	uint32_t emissiveInnerBlockerReceiverImmediate = 0;
	uint32_t emissiveInnerBlockerReceiverCell = 0;
	uint32_t emissiveInnerBlockerEmitterCell = 0;
	uint32_t emissiveInnerBlockerInterior = 0;
	uint32_t emissiveInnerSourceSelections = 0;
	uint32_t emissiveInnerSourceOverflow = 0;
	// Exact-candidate smoke visibility telemetry. These fields are only written
	// while an explicit diagnostic emissive candidate is selected.
	uint32_t emissiveTargetVisibilityRays = 0;
	uint32_t emissiveTargetVisibilityVisible = 0;
	uint32_t emissiveTargetBlockerExact = 0;
	uint32_t emissiveTargetBlockerRange = 0;
	uint32_t emissiveTargetBlockerOther = 0;
	uint32_t emissiveTargetWitnessClaim = 0;
	uint32_t emissiveTargetWitnessCandidate = 0xffffffffu;
	uint32_t emissiveTargetWitnessRelation = 0;
	uint32_t emissiveTargetWitnessSamplePrimitive = 0xffffffffu;
	uint32_t emissiveTargetWitnessSampleMaterial = 0xffffffffu;
	uint32_t emissiveTargetWitnessBlockerDataSource = 0xffffffffu;
	uint32_t emissiveTargetWitnessBlockerInstance = 0xffffffffu;
	uint32_t emissiveTargetWitnessBlockerPrimitive = 0xffffffffu;
	uint32_t emissiveTargetWitnessBlockerMaterial = 0xffffffffu;
	uint32_t emissiveTargetWitnessDistanceBits = 0;
	uint32_t maximumDepthSpan = 0;
	uint32_t depthSpanOne = 0;
	uint32_t depthSpanTwoToFour = 0;
	uint32_t depthSpanFiveToSixteen = 0;
	uint32_t depthSpanOverSixteen = 0;
	uint32_t maximumCandidatesPerFroxel = 0;
	uint32_t occupiedCount = 0;
	uint32_t occupiedOverflow = 0;
	uint32_t mediumCandidateTests = 0;
	uint32_t pointFroxelsProcessed = 0;
	uint32_t directionalFroxelsProcessed = 0;
	uint32_t directionalSamples = 0;
	uint32_t directionalShadowRays = 0;
	uint32_t directionalShadowVisible = 0;
	uint32_t directionalShadowOccluded = 0;
	uint32_t directionalRadianceClamps = 0;
	uint32_t emissiveFroxelsProcessed = 0;
	uint32_t emissiveSamples = 0;
	uint32_t emissiveCandidateMisses = 0;
	uint32_t emissiveDistanceRejected = 0;
	uint32_t emissiveFacingRejected = 0;
	uint32_t emissiveShadowRays = 0;
	uint32_t emissiveShadowVisible = 0;
	uint32_t emissiveShadowOccluded = 0;
	uint32_t emissiveContributed = 0;
	uint32_t emissiveRadianceClamps = 0;
	uint32_t emissiveReservoirInitial = 0;
	uint32_t emissiveReservoirInvalid = 0;
	uint32_t emissiveTemporalAccepted = 0;
	uint32_t emissiveTemporalRejected = 0;
	uint32_t emissiveSpatialAccepted = 0;
	uint32_t emissiveSpatialRejected = 0;
	uint32_t emissiveFinalEvaluations = 0;
	uint32_t emissiveSourceClamps = 0;
	uint32_t emissiveRemovedEnergy = 0;
	uint32_t emissiveMaximumAge = 0;
	uint32_t emissiveReferenceSamples = 0;
	uint32_t emissiveReferenceRays = 0;
	uint32_t emissiveIdentityRejects = 0;
	uint32_t indirectFroxelsProcessed = 0;
	uint32_t indirectLocalityRays = 0;
	uint32_t indirectLocalityAgreement = 0;
	uint32_t indirectLocalityOneSided = 0;
	uint32_t indirectLocalityMismatch = 0;
	uint32_t indirectLocalityInvalid = 0;
	uint32_t indirectReferenceRays = 0;
	uint32_t indirectReferenceHits = 0;
	uint32_t indirectReferenceMisses = 0;
	uint32_t indirectSectorContributions = 0;
	uint32_t indirectSkyContributions = 0;
	uint32_t indirectEmissionContributions = 0;
	uint32_t indirectRadianceClamps = 0;
	uint32_t indirectNanRejects = 0;
	uint32_t indirectTemporalAccepted = 0;
	uint32_t indirectTemporalRejected = 0;
	uint32_t indirectSpatialAccepted = 0;
	uint32_t indirectSpatialRejected = 0;
	uint32_t indirectCacheMaximumAge = 0;
	uint32_t indirectCacheClamps = 0;
	uint32_t indirectCacheResolved = 0;
	uint32_t directReceiverSamples = 0;
	uint32_t directFractionalVisibility = 0;
	uint32_t directVisibilityZero = 0;
	uint32_t directVisibilityOne = 0;
	uint32_t directTemporalAccepted = 0;
	uint32_t directTemporalRejected = 0;
	uint32_t directSpatialAccepted = 0;
	uint32_t directSpatialRejected = 0;
	uint32_t directHistoryMaximumAge = 0;
	uint32_t directHistoryResolved = 0;
	uint32_t directHistoryClamps = 0;
	uint32_t directNanRejects = 0;
	uint32_t analyticLightBuildEvents = 0;
	uint32_t analyticLightAnchorsBuilt = 0;
	uint32_t analyticLightAnchorsValid = 0;
	uint32_t analyticLightAnchorsInvalid = 0;
	uint32_t analyticLightSamplesRequested = 0;
	uint32_t analyticLightSamplesExecuted = 0;
	uint32_t analyticLightEvaluations = 0;
	uint32_t analyticLightBuildVisibilityRays = 0;
	uint32_t analyticLightGridSeedHits = 0;
	uint32_t analyticLightGridSeedMisses = 0;
	uint32_t analyticLightApplyFroxelsTested = 0;
	uint32_t analyticLightApplyFroxelsApplied = 0;
	uint32_t analyticLightCarrierContributions = 0;
	uint32_t analyticLightAnchorBlendTaps = 0;
	uint32_t analyticLightGroupCacheHits = 0;
	uint32_t analyticLightMissingGroupRecords = 0;
	uint32_t analyticLightIdentityRejects = 0;
	uint32_t analyticLightApplyVisibilityRays = 0;
};

struct NRISmokeIndirectCacheGpu
{
	float radiance[3] = {};
	float sigmaT = 0.0f;
	float worldPosition[3] = {};
	uint32_t metadata = 0;
};

struct NRISmokeDirectCacheGpu
{
	float radiance[3] = {};
	float sigmaT = 0.0f;
	float worldPosition[3] = {};
	uint32_t metadata = 0;
	float mediumTransmittance = 1.0f;
	uint32_t mediumMetadata = 0;
};

struct NRISmokeEmissiveStorageGpu
{
	uint32_t data[12] = {};
};

struct NRISmokeAnalyticEmissiveStorageGpu
{
	// Four packed directional-lobe anchor records per analytic event are split
	// evenly across the two persistent analytic emissive storage banks.
	uint32_t data[16] = {};
};

static_assert(sizeof(NRISmokeParticleGpu) == 64);
static_assert(sizeof(NRISmokeStyleGpu) == 80);
static_assert(sizeof(NRISmokeInjectionCommandGpu) == 112);
static_assert(offsetof(NRISmokeInjectionCommandGpu, halfAxisU) == 56);
static_assert(offsetof(NRISmokeInjectionCommandGpu, shape) == 68);
static_assert(offsetof(NRISmokeInjectionCommandGpu, halfAxisV) == 72);
static_assert(offsetof(NRISmokeInjectionCommandGpu, sourceId) == 84);
static_assert(offsetof(NRISmokeInjectionCommandGpu, sourceSlot) == 88);
static_assert(offsetof(NRISmokeInjectionCommandGpu, sourceMetadata) == 92);
static_assert(offsetof(NRISmokeInjectionCommandGpu, rangeBegin) == 96);
static_assert(offsetof(NRISmokeInjectionCommandGpu, rangeCount) == 100);
static_assert(offsetof(NRISmokeInjectionCommandGpu, pulseIdLow) == 104);
static_assert(offsetof(NRISmokeInjectionCommandGpu, pulseIdHigh) == 108);
static_assert(sizeof(NRISmokeControlGpu) == 640);
static_assert(sizeof(NRISmokeIndirectCacheGpu) == 32);
static_assert(sizeof(NRISmokeDirectCacheGpu) == 40);
static_assert(sizeof(NRISmokeEmissiveStorageGpu) == 48);
static_assert(sizeof(NRISmokeAnalyticEmissiveStorageGpu) == 64);

struct NRISmokeConstants
{
	uint32_t pass = 0;
	uint32_t frameIndex = 0;
	uint32_t simulationEpoch = 0;
	uint32_t particleCapacity = 0;

	uint32_t commandCount = 0;
	uint32_t styleCount = 0;
	uint32_t froxelWidth = 0;
	uint32_t froxelHeight = 0;

	uint32_t froxelDepth = 0;
	// 8-bit RGB channels packed over [0, 8]. The obsolete column-capacity
	// constant was retired when smoke moved to fixed depth-local tier caps.
	uint32_t directionalColorPacked = 0;
	uint32_t renderWidth = 0;
	uint32_t renderHeight = 0;

	uint32_t outputWidth = 0;
	uint32_t outputHeight = 0;
	uint32_t debugMode = 0;
	uint32_t flags = 0;

	float deltaTime = 0.0f;
	// Dedicated volume-indirect scale. This preserves the established prefix
	// layout by assigning a previously unused scalar to its first owner.
	float indirectScale = 0.0f;
	float froxelMaxDistance = 0.0f;
	float depthExponent = 1.0f;

	float densityScale = 1.0f;
	float radianceScale = 1.0f;
	float tanHalfFovX = 1.0f;
	float tanHalfFovY = 1.0f;

	float cameraPosition[3] = {};
	float timeScale = 1.0f;

	float cameraForward[3] = {};
	float directionalDirectionX = 0.0f;

	float cameraRight[3] = {};
	float directionalDirectionY = 0.0f;

	float cameraUp[3] = {};
	float directionalDirectionZ = 0.0f;

	float wind[3] = {};
	float directionalAngularSize = 0.03f;

	uint32_t lightMode = 0;
	uint32_t lightSamples = 1;
	uint32_t maxLightCandidates = 8;
	uint32_t runtimeLightCount = 0;

	uint32_t runtimeLightTileCountX = 0;
	uint32_t runtimeLightTileCountY = 0;
	// bit 0: point, bit 1: directional, bit 2: directional shadow,
	// bit 3: emissive.
	uint32_t lightSourceFlags = 1;
	// bit 0: filtered visibility effective, bit 1: filtered resources ready,
	// bit 2: TLAS ready, bit 3: filtered visibility requested,
	// bits 4..7: smoke-world emissive point-candidate count,
	// bits 8..15: portal traversal depth,
	// bits 16..31: session-only exact emissive candidate index + 1.
	uint32_t filteredVisibilityEnabled = 0;

	float currentJitter[2] = {};

	// Four packed words are the remaining safe D3D12 root-signature budget
	// after the smoke layout's six descriptor tables. Words 0..2 contain
	// half2 transfer values; word 3 contains two RGB555 grades over [0, 2].
	uint32_t visuals[4] = {};
};

static_assert(sizeof(NRISmokeConstants) == 232, "NRISmokeConstants must match SmokeConstants.hlsli");
static_assert(offsetof(NRISmokeConstants, cameraPosition) == 96, "NRISmokeConstants camera offset must match HLSL");
static_assert(offsetof(NRISmokeConstants, lightMode) == 176, "NRISmokeConstants lighting offset must match HLSL");
static_assert(offsetof(NRISmokeConstants, runtimeLightTileCountX) == 192, "NRISmokeConstants runtime-light tile offset must match HLSL");
static_assert(offsetof(NRISmokeConstants, currentJitter) == 208, "NRISmokeConstants jitter offset must match HLSL");
static_assert(offsetof(NRISmokeConstants, visuals) == 216, "NRISmokeConstants visual offset must match HLSL");
