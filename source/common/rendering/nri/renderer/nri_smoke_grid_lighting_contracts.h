#pragma once

#include <cstddef>
#include <cstdint>

constexpr uint32_t NRI_SMOKE_GRID_LIGHT_LOBE_COUNT = 6u;
constexpr uint32_t NRI_SMOKE_GRID_LIGHT_RECORD_WORDS = 24u;
constexpr uint32_t NRI_SMOKE_GRID_LIGHT_MAX_HISTORY = 64u;
constexpr uint32_t NRI_SMOKE_GRID_LIGHT_PROPOSAL_CAPACITY = 16u;
constexpr uint32_t NRI_SMOKE_GRID_SCATTER_PROBE_AXIS = 4u;
constexpr uint32_t NRI_SMOKE_GRID_SCATTER_PROBES_PER_BRICK = 64u;

enum class NRISmokeGridLightingPass : uint32_t
{
	Prepare = 0,
	ClearSupport,
	BuildActive,
	BuildProposals,
	Seed,
	Temporal,
	BuildLinks,
	Filter,
	SeedScattering,
	PropagateScattering,
	Count,
};

enum class NRISmokeEmissiveBackend : uint32_t
{
	Auto = 0,
	Legacy,
	World,
	Compare,
};

// The hot prefix is six RGB means (9 words) plus generation/epoch/evidence
// metadata (3 words). The cold suffix carries second moments, age, and
// optional self-shadow evidence. The complete record remains exactly 96 bytes.
struct NRISmokeGridLightRecordGpu
{
	uint32_t data[NRI_SMOKE_GRID_LIGHT_RECORD_WORDS] = {};
};

struct NRISmokeGridLightControlGpu
{
	uint32_t activeCount = 0;
	uint32_t supportCount = 0;
	uint32_t sourceCount = 0;
	uint32_t supportOnlyCount = 0;
	uint32_t duplicateCount = 0;
	uint32_t supportOverflowCount = 0;
	uint32_t scheduledCount = 0;
	uint32_t samples = 0;
	uint32_t visible = 0;
	uint32_t physicalZero = 0;
	uint32_t missing = 0;
	uint32_t structuralErrors = 0;
	uint32_t overflowRejects = 0;
	uint32_t temporalAccepted = 0;
	uint32_t temporalRejected = 0;
	uint32_t linksOpen = 0;
	uint32_t linksBlocked = 0;
	uint32_t linksStale = 0;
	uint32_t cornerAccepted = 0;
	uint32_t cornerRejected = 0;
	uint32_t filterAccepted = 0;
	uint32_t filterRejected = 0;
	uint32_t maximumAge = 0;
	uint32_t frameStamp = 0;
	uint32_t simulationEpoch = 0;
	uint32_t fieldPing = 0;
	uint32_t flags = 0;
	uint32_t proposalListsBuilt = 0;
	uint32_t proposalCandidatesTested = 0;
	uint32_t proposalCandidatesAccepted = 0;
	uint32_t proposalLocalSamples = 0;
	uint32_t proposalGlobalSamples = 0;
	uint32_t proposalFallbacks = 0;
	uint32_t proposalTruncations = 0;
	uint32_t proposalMaximumCount = 0;
	uint32_t scatterActiveCount = 0;
	uint32_t scatterSeededCount = 0;
	uint32_t scatterNonzeroSourceCount = 0;
	uint32_t scatterZeroSourceCount = 0;
	uint32_t scatterPointCells = 0;
	uint32_t scatterDirectionalCells = 0;
	uint32_t scatterEmissiveCells = 0;
	uint32_t scatterEnvironmentCells = 0;
	uint32_t scatterIterations = 0;
	uint32_t scatterFinalPing = 0;
	uint32_t scatterNeighborTests = 0;
	uint32_t scatterNeighborsAccepted = 0;
	uint32_t scatterNeighborsBlocked = 0;
	uint32_t scatterNeighborsStale = 0;
	uint32_t scatterInternalBlocked = 0;
	uint32_t scatterNanRejects = 0;
	uint32_t scatterActiveOverflow = 0;
	uint32_t scatterReconstructionAccepted = 0;
	uint32_t scatterReconstructionRejected = 0;
	uint32_t scatterSourceEnergyQ = 0;
	uint32_t scatterTransportedEnergyQ = 0;
	uint32_t scatterRemovedEnergyQ = 0;
	uint32_t scatterReceiverApplications = 0;
	uint32_t scatterExactZero = 0;
	uint32_t scatterProbeCapacity = 0;
	uint32_t scatterProbesPerBrick = 0;
	uint32_t scatterFrameStamp = 0;
	uint32_t scatterEpoch = 0;
	uint32_t scatterFlags = 0;
	uint32_t scatterPadding[3] = {};
	uint32_t selfShadowSamples = 0;
	uint32_t selfShadowSteps = 0;
	uint32_t selfShadowTruncated = 0;
	uint32_t selfShadowTransmittanceZero = 0;
	uint32_t selfShadowTransmittancePartial = 0;
	uint32_t selfShadowTransmittanceOne = 0;
	uint32_t selfShadowNanRejects = 0;
	uint32_t selfShadowHistoryAccepted = 0;
	uint32_t selfShadowHistoryRestarted = 0;
	uint32_t selfShadowMaximumAge = 0;
	uint32_t topologyMissingTlas = 0;
	uint32_t topologyAsymmetric = 0;
	uint32_t explicitZeroProbes = 0;
	uint32_t splitBlockedProbes = 0;
	uint32_t selfShadowPadding[2] = {};
	uint32_t radiancePartitionCount = 1;
	uint32_t radianceNewInvalidQuantity = 0;
	uint32_t radianceMaintenanceQuantity = 0;
	uint32_t radianceMaximumAge = 0;
	uint32_t radianceNewInvalidRequested = 0;
	uint32_t radianceNewInvalidScheduled = 0;
	uint32_t radianceNewInvalidDeferred = 0;
	uint32_t radianceMaintenanceRequested = 0;
	uint32_t radianceMaintenanceScheduled = 0;
	uint32_t radianceMaintenanceDeferred = 0;
	uint32_t radianceHistoryRetained = 0;
	uint32_t radianceHistoryMissing = 0;
	uint32_t radianceAgeOverflows = 0;
	uint32_t radianceNewInvalidTickets = 0;
	uint32_t radianceMaintenanceTickets = 0;
};

struct NRISmokeGridLightProposalGpu
{
	uint32_t candidateIndices[NRI_SMOKE_GRID_LIGHT_PROPOSAL_CAPACITY] = {};
	uint32_t count = 0;
	uint32_t brickGeneration = 0;
	uint32_t simulationEpoch = 0;
	uint32_t frameStamp = 0;
};

struct NRISmokeGridLightSupportStampGpu
{
	uint32_t brickGeneration = 0;
	uint32_t frameStamp = 0;
};

struct NRISmokeGridScatterMetadataGpu
{
	uint32_t brickGeneration = 0;
	uint32_t simulationEpoch = 0;
	uint32_t frameStamp = 0;
	uint32_t flags = 0;
	uint32_t historyBlock = 0;
	uint32_t historyCount = 0;
	uint32_t transmittanceQ = 0;
	uint32_t reserved = 0;
};

struct NRISmokeGridLightingStatusSnapshot
{
	bool requested = false;
	bool initialized = false;
	bool resourcesReady = false;
	bool gpuStatsValid = false;
	uint64_t gpuRendererFrame = UINT64_MAX;
	bool filterRequested = false;
	bool filterAllocated = false;
	bool multipleScatterRequested = false;
	bool multipleScatterAllocated = false;
	bool multipleScatterEffective = false;
	bool selfShadowRequested = false;
	bool selfShadowAllocated = false;
	bool selfShadowEffective = false;
	uint32_t requestedBackend = 0;
	uint32_t effectiveBackend = 0;
	uint32_t cellCapacity = 0;
	uint32_t fieldPing = 0;
	uint32_t simulationEpoch = 0;
	uint32_t scatterProbeCapacity = 0;
	uint32_t scatterIterations = 0;
	uint32_t scatterFinalPing = 0;
	uint32_t emissivePointCandidatesRequested = 1;
	uint32_t emissivePointCandidatesEffective = 1;
	int32_t emissiveCandidateTarget = -1;
	uint32_t radiancePartitionCount = 1;
	uint32_t radianceNewInvalidQuantity = 0;
	uint32_t radianceMaintenanceQuantity = 0;
	uint32_t radianceMaximumAge = 0;
	bool radianceWorkLimited = false;
	uint32_t lastUpdatedFrame = UINT32_MAX;
	uint64_t fieldBytes = 0;
	uint64_t workBytes = 0;
	uint64_t linkBytes = 0;
	uint64_t proposalBytes = 0;
	uint64_t scatterSeedBytes = 0;
	uint64_t scatterBounceBytes = 0;
	uint64_t scatterMetadataBytes = 0;
	uint64_t scatterActiveBytes = 0;
	uint64_t scatterBytes = 0;
	uint64_t selfShadowFieldBytes = 0;
	uint64_t filterBytes = 0;
	uint64_t totalBytes = 0;
	uint64_t controlReadbackBytes = 0;
	NRISmokeGridLightControlGpu gpu = {};
	const char* authority = "disabled";
	const char* failureReason = "not-requested";
	const char* filterDecision = "not-requested";
	const char* proposalDecision = "global-cdf/no-measured-starvation";
	const char* scatterDecision = "disabled/phase12d-pending";
	const char* selfShadowDecision = "disabled";
};

static_assert(sizeof(NRISmokeGridLightRecordGpu) == 96);
static_assert(sizeof(NRISmokeGridLightControlGpu) == 392);
static_assert(sizeof(NRISmokeGridLightProposalGpu) == 80);
static_assert(sizeof(NRISmokeGridLightSupportStampGpu) == 8);
static_assert(sizeof(NRISmokeGridScatterMetadataGpu) == 32);
