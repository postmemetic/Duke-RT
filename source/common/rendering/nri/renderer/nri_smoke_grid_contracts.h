#pragma once

#include <cstddef>
#include <cstdint>

constexpr uint32_t NRI_SMOKE_GRID_BRICK_AXIS = 8u;
constexpr uint32_t NRI_SMOKE_GRID_CELLS_PER_BRICK = 512u;
constexpr uint32_t NRI_SMOKE_GRID_FLAG_HASH_HEALTH = 1u;
constexpr uint32_t NRI_SMOKE_GRID_FLAG_COMPACT_DRAINED_HASH = 2u;
constexpr uint32_t NRI_SMOKE_PROMPT_FALLBACK_QUANTITY = 8u;
constexpr uint32_t NRI_SMOKE_PROMPT_LEDGER_CAPACITY = NRI_SMOKE_PROMPT_FALLBACK_QUANTITY;

enum class NRISmokePromptOutcome : uint32_t
{
	None = 0,
	Fallback = 1,
	GridNew = 2,
	GridCommitted = 3,
	InternalError = 4,
};

enum class NRISmokeGridPass : uint32_t
{
	Clear = 0,
	AllocateCommands,
	BuildDispatch,
	PrepareBricks,
	Deposit,
	ResolveDeposit,
	AllocateHalo,
	BeginRebuild,
	AdvectVelocity,
	AdvectFields,
	Rebuild,
	ValidatePrompt,
	AuthorizePrompt,
	FinalizePrompt,
};

struct NRISmokePromptOutcomeGpu
{
	uint32_t pulseIdLow = 0;
	uint32_t pulseIdHigh = 0;
	uint32_t rangeBegin = 0;
	uint32_t rangeCount = 0;
	uint32_t commandIndex = UINT32_MAX;
	uint32_t outcome = 0;
	uint32_t requestedBricks = 0;
	uint32_t admittedBricks = 0;
};

struct NRISmokePromptLedgerGpu
{
	uint32_t pulseIdLow = 0;
	uint32_t pulseIdHigh = 0;
	uint32_t rangeBegin = 0;
	uint32_t rangeCount = 0;
	uint32_t epoch = 0;
	uint32_t committed = 0;
	uint32_t padding[2] = {};
};

struct NRISmokeGridHashEntryGpu
{
	int32_t coordinate[3] = {};
	uint32_t brickIndex = UINT32_MAX;
	uint32_t generation = 0;
	uint32_t state = 0;
	uint32_t padding[2] = {};
};

struct NRISmokeGridBrickGpu
{
	int32_t coordinate[3] = {};
	uint32_t hashSlot = UINT32_MAX;
	uint32_t generation = 0;
	uint32_t state = 0;
	uint32_t idleFrames = 0;
	uint32_t flags = 0;
};

struct NRISmokeGridControlGpu
{
	uint32_t activeCountA = 0;
	uint32_t activeCountB = 0;
	uint32_t residentCount = 0;
	uint32_t freeCount = 0;
	uint32_t allocated = 0;
	uint32_t reclaimed = 0;
	uint32_t allocationFailures = 0;
	uint32_t probeFailures = 0;
	uint32_t maximumProbe = 0;
	uint32_t commandsProcessed = 0;
	uint32_t requestedMassQ = 0;
	uint32_t depositedMassQ = 0;
	uint32_t rejectedMassQ = 0;
	uint32_t saturatedDeposits = 0;
	uint32_t haloAllocations = 0;
	uint32_t occupiedBricks = 0;
	uint32_t emptyBricks = 0;
	uint32_t cflClamps = 0;
	uint32_t backtraceClamps = 0;
	uint32_t nanRejects = 0;
	uint32_t vorticityClamps = 0;
	uint32_t fieldHashLo = 0;
	uint32_t fieldHashHi = 0;
	uint32_t depositionCells = 0;
	uint32_t depositionRejected = 0;
	uint32_t generation = 0;
	uint32_t frameStamp = 0;
	uint32_t brickCapacity = 0;
	uint32_t hashCapacity = 0;
	uint32_t cellCapacity = 0;
	uint32_t activePing = 0;
	uint32_t fieldPing = 0;
	uint32_t cellSizeBits = 0;
	uint32_t admissionSourceCount = 0;
	uint32_t admissionRequested = 0;
	uint32_t admissionExisting = 0;
	uint32_t admissionAdmitted = 0;
	uint32_t admissionRejected = 0;
	uint32_t admissionCapacityRejected = 0;
	uint32_t admissionProbeRejected = 0;
	uint32_t admissionInvalidRejected = 0;
	uint32_t admissionFootprintCulled = 0;
	uint32_t hashEmpty = 0;
	uint32_t hashClaimed = 0;
	uint32_t hashResident = 0;
	uint32_t hashNew = 0;
	uint32_t hashTombstone = 0;
	uint32_t hashInvalidState = 0;
	uint32_t hashInvalidMapping = 0;
	uint32_t controlProbeTotal = 0;
	uint32_t controlProbeBin1 = 0;
	uint32_t controlProbeBin2To4 = 0;
	uint32_t controlProbeBin5To8 = 0;
	uint32_t controlProbeBin9To16 = 0;
	uint32_t controlProbeBin17To24 = 0;
	uint32_t lookupProbeTotal = 0;
	uint32_t insertionProbeTotal = 0;
	uint32_t lookupProbeLimitFailures = 0;
	uint32_t insertionProbeLimitFailures = 0;
	uint32_t insertionCapacityFailures = 0;
	uint32_t insertionActiveFailures = 0;
	uint32_t reclaimInvalidMappingFailures = 0;
	uint32_t hashRebuildAttempts = 0;
	uint32_t hashRebuildSuccesses = 0;
	uint32_t hashRebuildFailures = 0;
	uint32_t firstUseCoreCapacity = 0;
	uint32_t borrowedResident = 0;
	uint32_t borrowedAllocations = 0;
	uint32_t borrowedReturns = 0;
	uint32_t borrowedPromotions = 0;
	uint32_t borrowedReclaims = 0;
	uint32_t firstUseReplacementAdmissions = 0;
	uint32_t firstUseBlockedNoBorrowed = 0;
	uint32_t firstUseBlockedVisible = 0;
	uint32_t firstUseBlockedProbe = 0;
	uint32_t firstUseBlockedInvalid = 0;
	uint32_t firstUseCapacityFailures = 0;
};

struct NRISmokeGridSourceStatsGpu
{
	uint32_t sourceId = 0;
	uint32_t sourceClass = 0;
	uint32_t priority = 0;
	uint32_t commands = 0;
	uint32_t requestedBricks = 0;
	uint32_t existingHits = 0;
	uint32_t admittedNew = 0;
	uint32_t rejectedCapacity = 0;
	uint32_t rejectedProbe = 0;
	uint32_t rejectedInvalid = 0;
	uint32_t depositionCells = 0;
	uint32_t footprintCulled = 0;
	uint32_t requestedMassQ = 0;
	uint32_t depositedMassQ = 0;
	uint32_t rejectedMassQ = 0;
	uint32_t admittedKeyHash = 0;
};

struct NRISmokeGridDispatchGpu
{
	uint32_t x = 0;
	uint32_t y = 1;
	uint32_t z = 1;
};

struct NRISmokeGridConstants
{
	uint32_t pass = 0;
	uint32_t frameIndex = 0;
	uint32_t simulationEpoch = 0;
	uint32_t commandCount = 0;

	uint32_t styleCount = 0;
	uint32_t brickCapacity = 0;
	uint32_t hashCapacity = 0;
	uint32_t cellCapacity = 0;

	uint32_t activePing = 0;
	uint32_t fieldPing = 0;
	uint32_t flags = 0;
	uint32_t representation = 0;

	float cellSize = 8.0f;
	float deltaTime = 0.0f;
	float timeScale = 1.0f;
	float maxBacktrace = 32.0f;

	float wind[3] = {};
	float buoyancy = 1.0f;

	float velocityDamping = 0.15f;
	float windCoupling = 0.5f;
	float densityHalfLifeScale = 1.0f;
	float coolingScale = 1.0f;

	float maxVelocity = 128.0f;
	float activeThreshold = 0.0001f;
	uint32_t reclaimGrace = 120;
	float massQuantization = 4096.0f;

	float momentumQuantization = 256.0f;
	float curlTime = 0.0f;
	float curlEvolution = 0.0f;
	float vorticityConfinement = 0.0f;
};

static_assert(sizeof(NRISmokeGridHashEntryGpu) == 32);
static_assert(sizeof(NRISmokeGridBrickGpu) == 32);
static_assert(sizeof(NRISmokeGridControlGpu) == 308);
static_assert(sizeof(NRISmokeGridSourceStatsGpu) == 64);
static_assert(sizeof(NRISmokeGridDispatchGpu) == 12);
static_assert(sizeof(NRISmokeGridConstants) == 128);
static_assert(sizeof(NRISmokePromptOutcomeGpu) == 32);
static_assert(sizeof(NRISmokePromptLedgerGpu) == 32);
