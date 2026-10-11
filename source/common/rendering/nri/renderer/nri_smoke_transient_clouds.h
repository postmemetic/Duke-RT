#pragma once

#include <array>
#include <cstdint>
#include <vector>

enum class NRISmokeTransientClass : uint32_t
{
	Explosion = 0u,
	TrailChunk,
	FirePacket,
	Muzzle,
	Impact,
	Diagnostic,
};

enum class NRISmokeTransientLightRefresh : uint32_t
{
	Frozen = 0u,
	Slow = 1u,
};

// CPU scheduling interest only; this deliberately does not change GPU layouts.
enum class NRISmokeTransientInterest : uint32_t
{
	Hot = 0u,
	Warm,
	Dormant,
};

// Values intentionally match nri_ptsmokeworkprofile.
enum class NRISmokeTransientQuality : uint32_t
{
	Reference = 0u,
	High = 1u,
	Medium = 2u,
	Low = 3u,
};

enum NRISmokeTransientGroupFlags : uint32_t
{
	NRISmokeTransientGroupFlagActive = 1u,
	NRISmokeTransientGroupFlagFullLightAllowed = 2u,
	NRISmokeTransientGroupFlagFallbackLight = 4u,
	NRISmokeTransientGroupFlagSlowRefresh = 8u,
};

// A shaped lobe is an admission record, not a count of source particles. Optical
// amount is carried by opticalWeight and remains invariant if a batch is reduced.
struct NRISmokeTransientLobeRequest
{
	float position[3] = {};
	float initialRadius = 0.0f;
	float velocity[3] = {};
	float initialDensity = 0.0f;
	float halfAxisU[3] = {};
	uint32_t shape = 0u;
	float halfAxisV[3] = {};
	float opticalWeight = 0.0f;
	float expansionVelocity = 0.0f;
	float densityHalfLife = 0.0f;
	float lifetimeSeconds = 0.0f;
	uint32_t styleIndex = 0u;
	uint32_t sourceId = 0u;
	uint32_t epoch = 0u;
	double authoredGameplaySeconds = 0.0;
	float maximumLatencySeconds = 0.0f;
	uint64_t sourceEventSerial = 0u;
	uint64_t replacementKey = 0u;
	uint32_t batchIndex = 0u;
	uint32_t batchCount = 1u;
	float groupLifetimeSeconds = 0.0f;
	float lobeDelaySeconds = 0.0f;
	float densityAttackSeconds = 0.0f;
	float densitySustainSeconds = 0.0f;
	float densityReleaseSeconds = 0.0f;
	float radiusExponent = 1.0f;
	float intrinsicEmission = 0.0f;
	float emissionHalfLife = 0.25f;
	float corePlateau = 0.58f;
	float edgeErosion = 0.12f;
	float noiseScale = 0.035f;
	float noiseStrength = 0.18f;
	uint32_t deterministicSeed = 0u;
	NRISmokeTransientClass transientClass = NRISmokeTransientClass::Diagnostic;
	NRISmokeTransientLightRefresh lightRefresh = NRISmokeTransientLightRefresh::Frozen;
	bool burstBorrow = false;
};

// Semantic emitter input. One input is one group; optical amount and requested
// spatial complexity are deliberately independent.
struct NRISmokeTransientGroupShapeInput
{
	float position[3] = {};
	float velocity[3] = {};
	float up[3] = { 0.0f, 1.0f, 0.0f };
	float trailAxis[3] = {};
	float trailSpan = 0.0f;
	float initialRadius = 0.0f;
	float initialDensity = 0.0f;
	float opticalAmount = 0.0f;
	float expansionVelocity = 0.0f;
	float densityHalfLife = 0.0f;
	float lobeLifetimeSeconds = 0.0f;
	float groupLifetimeSeconds = 0.0f;
	float maximumLatencySeconds = 0.0f;
	float densityAttackSeconds = 0.0f;
	float densitySustainSeconds = 0.0f;
	float densityReleaseSeconds = 0.0f;
	float radiusExponent = 1.0f;
	float intrinsicEmission = 0.0f;
	float emissionHalfLife = 0.25f;
	float clusterSpread = 0.55f;
	float lobeRadiusMinScale = 0.72f;
	float lobeRadiusMaxScale = 1.28f;
	float riseVelocity = 0.0f;
	float curlVelocity = 0.0f;
	// Positive delay on FirePacket feeds compact births over requestedCount * step.
	// Builder/reduction retain that cadence span and fit local fades before group end.
	float lobeDelayStepSeconds = 0.0f;
	float corePlateau = 0.58f;
	float edgeErosion = 0.12f;
	float noiseScale = 0.035f;
	float noiseStrength = 0.18f;
	float halfAxisU[3] = {};
	uint32_t shape = 0u;
	float halfAxisV[3] = {};
	uint32_t requestedLobeCount = 0u;
	uint32_t styleIndex = 0u;
	uint32_t sourceId = 0u;
	uint32_t epoch = 0u;
	double authoredGameplaySeconds = 0.0;
	uint64_t sourceEventSerial = 0u;
	uint64_t replacementKey = 0u;
	uint32_t deterministicSeed = 0u;
	NRISmokeTransientClass transientClass = NRISmokeTransientClass::Diagnostic;
	NRISmokeTransientLightRefresh lightRefresh = NRISmokeTransientLightRefresh::Frozen;
	bool burstBorrow = false;
};

// Returns initialized records, or zero for invalid input. A smaller output
// capacity reduces spatial complexity deterministically while preserving the
// complete opticalAmount across the returned batch.
uint32_t NRIBuildSmokeTransientLobes(const NRISmokeTransientGroupShapeInput& input,
	NRISmokeTransientLobeRequest* output, uint32_t outputCapacity);

struct NRISmokeTransientLobeGpu
{
	float position[3] = {};
	float radius = 0.0f;
	float halfAxisU[3] = {};
	uint32_t shape = 0u;
	float halfAxisV[3] = {};
	uint32_t styleIndex = 0u;
	float densityScale = 0.0f;
	float emissionScale = 0.0f;
	uint32_t groupSlot = UINT32_MAX;
	uint32_t groupGeneration = 0u;
	uint32_t epoch = 0u;
	uint32_t flags = 0u;
	uint32_t deterministicSeed = 0u;
	uint32_t transientClass = 0u;
	float corePlateau = 0.58f;
	float edgeErosion = 0.12f;
	float noiseScale = 0.035f;
	float noiseStrength = 0.18f;
};

static_assert(sizeof(NRISmokeTransientLobeGpu) == 96u,
	"transient lobe GPU records must retain an explicit six-register layout");

struct NRISmokeTransientGroupGpu
{
	float boundsMin[3] = {};
	float ageSeconds = 0.0f;
	float boundsMax[3] = {};
	float groupLifetimeSeconds = 0.0f;
	uint32_t firstLobe = 0u;
	uint32_t lobeCount = 0u;
	uint32_t slot = UINT32_MAX;
	uint32_t generation = 0u;
	uint32_t epoch = 0u;
	uint32_t flags = 0u;
	uint32_t anchorCount = 0u;
	uint32_t samplesPerAnchor = 0u;
	float center[3] = {};
	float refreshIntervalSeconds = 0.0f;
	uint32_t sourceId = 0u;
	uint32_t transientClass = 0u;
	uint32_t requiredAnchorMask = 0u;
	uint32_t reserved = 0u;
};

static_assert(sizeof(NRISmokeTransientGroupGpu) == 96u,
	"transient group GPU records must retain an explicit six-register layout");

struct NRISmokeTransientProfile
{
	uint32_t maximumActiveGroups = 64u;
	uint32_t maximumActiveLobes = 256u;
	uint32_t maximumLobesPerGroup = 12u;
	uint32_t minimumReducedLobes = 4u;
	uint32_t maximumFullLightBuilds = 8u;
	uint32_t anchorsPerGroup = 4u;
	uint32_t samplesPerAnchor = 2u;
	float fireRefreshSeconds = 0.5f;
	bool allowSlowFireRefresh = true;
	bool enabled = true;
};

struct NRISmokeTransientLightBudgetMetadata
{
	uint32_t maximumFullBuildsPerFrame = 0u;
	uint32_t anchorsPerGroup = 0u;
	uint32_t samplesPerAnchor = 0u;
	uint32_t maximumPointLightsPerAnchor = 4u;
	uint32_t maximumDirectionalLightsPerAnchor = 1u;
	uint32_t maximumVisibilityQueriesPerGroup = 0u;
	uint32_t maximumVisibilityQueriesPerFrame = 0u;
	float fireRefreshSeconds = 0.0f;
	bool slowFireRefreshEnabled = false;
};

struct NRISmokeTransientHandle
{
	uint32_t slot = UINT32_MAX;
	uint32_t generation = 0u;
	uint32_t epoch = 0u;
};

enum class NRISmokeTransientDropReason : uint32_t
{
	None = 0u,
	NotPrepared,
	Disabled,
	InvalidRequest,
	StaleEpoch,
	ExpiredOnArrival,
	StaleOnArrival,
	GroupCapacity,
	LobeCapacity,
};

struct NRISmokeTransientAdmission
{
	NRISmokeTransientHandle handle = {};
	NRISmokeTransientDropReason dropReason = NRISmokeTransientDropReason::NotPrepared;
	uint32_t admittedLobes = 0u;
	uint32_t requestedLobes = 0u;
	bool Accepted() const { return dropReason == NRISmokeTransientDropReason::None; }
};

struct NRISmokeTransientSnapshot
{
	uint64_t groupsRequested = 0u;
	uint64_t groupsAdmitted = 0u;
	uint64_t groupsExpired = 0u;
	uint64_t groupsRejected = 0u;
	uint64_t lobesRequested = 0u;
	uint64_t lobesAdmitted = 0u;
	uint64_t lobesExpired = 0u;
	uint64_t droppedNotPrepared = 0u;
	uint64_t droppedDisabled = 0u;
	uint64_t droppedInvalidRequest = 0u;
	uint64_t droppedStaleEpoch = 0u;
	uint64_t droppedExpiredOnArrival = 0u;
	uint64_t droppedStaleOnArrival = 0u;
	uint64_t droppedGroupCapacity = 0u;
	uint64_t droppedLobeCapacity = 0u;
	uint64_t replacements = 0u;
	uint64_t replacementRetirements = 0u;
	uint64_t deterministicallyReducedGroups = 0u;
	uint64_t deterministicallyReducedLobes = 0u;
	uint32_t epoch = 0u;
	uint32_t maximumActiveGroups = 0u;
	uint32_t maximumActiveLobes = 0u;
	uint32_t activeGroups = 0u;
	uint32_t activeLobes = 0u;
	uint32_t visibleGroups = 0u;
	uint32_t visibleLobes = 0u;
	uint32_t groupHighWater = 0u;
	uint32_t lobeHighWater = 0u;
	uint32_t visibleLobeHighWater = 0u;
	uint32_t oldestActiveAgeMilliseconds = 0u;
	uint32_t fullLightFreshRequestedThisFrame = 0u;
	uint32_t fullLightFreshScheduledThisFrame = 0u;
	uint32_t fullLightFreshDeferredThisFrame = 0u;
	uint32_t fullLightRefreshRequestedThisFrame = 0u;
	uint32_t fullLightRefreshScheduledThisFrame = 0u;
	uint32_t fullLightRefreshDeferredThisFrame = 0u;
	uint32_t fullLightAllowedGroups = 0u;
	uint32_t fallbackLightGroups = 0u;
	uint32_t lightAnchorsScheduledThisFrame = 0u;
	uint32_t lightSamplesScheduledThisFrame = 0u;
	uint32_t lightVisibilityQueriesScheduledThisFrame = 0u;
	uint32_t lightInterestDeferredGroups = 0u;
	uint64_t allocatedGroupBytes = 0u;
	uint64_t allocatedLobeBytes = 0u;
};

// Owns immediate group/lobe admission and absolute gameplay-time state. CPU
// reservation and current GPU visibility are intentionally independent.
class NRISmokeTransientClouds
{
public:
	static constexpr uint32_t FixedGroupCapacity = 64u;
	static constexpr uint32_t FixedLobeCapacity = 256u;
	static constexpr uint32_t FixedMaximumLobesPerGroup = 16u;

	static NRISmokeTransientProfile ProfileForQuality(uint32_t workProfile);
	static uint32_t DefaultLobeCountForQuality(uint32_t workProfile,
		NRISmokeTransientClass transientClass);

	void BeginFrame(double gameplayTimeSeconds, uint32_t maximumActiveLobes,
		const NRISmokeTransientProfile& profile);
	NRISmokeTransientAdmission Admit(const NRISmokeTransientLobeRequest& request);
	NRISmokeTransientAdmission AdmitLatest(const NRISmokeTransientLobeRequest& request);
	NRISmokeTransientAdmission AdmitBatch(const NRISmokeTransientLobeRequest* requests,
		uint32_t count);
	// A retained event has already passed arrival validation. Reentry preserves
	// authored age, including expired members of a still-living group; it never
	// re-applies the source's initial-arrival latency gate.
	NRISmokeTransientAdmission AdmitRetainedBatch(
		const NRISmokeTransientLobeRequest* requests, uint32_t count,
		uint32_t maximumLobes);
	static bool ValidateBatch(const NRISmokeTransientLobeRequest* requests,
		uint32_t count);
	bool Release(const NRISmokeTransientHandle& handle);
	bool SetInterest(const NRISmokeTransientHandle& handle,
		NRISmokeTransientInterest interest);
	// Acknowledges that the currently permitted full-light builds were actually
	// recorded. This is submission state only; GPU cache validity stays GPU-owned.
	void CommitLightDispatchSchedule();
	// Explicit light-family/policy invalidation. Moving lights and camera motion do
	// not call this; cache identity remains camera-independent.
	void InvalidateLighting();
	bool RetireLatest(uint64_t replacementKey);
	bool IsLive(const NRISmokeTransientHandle& handle) const;
	void Reset(uint32_t epoch);

	const std::vector<NRISmokeTransientLobeGpu>& GetGpuLobes() const { return mGpuLobes; }
	const std::vector<NRISmokeTransientGroupGpu>& GetGpuGroups() const { return mGpuGroups; }
	const NRISmokeTransientSnapshot& GetSnapshot() const { return mSnapshot; }
	const NRISmokeTransientLightBudgetMetadata& GetLightBudgetMetadata() const
	{
		return mLightBudget;
	}

private:
	struct LobeSlot
	{
		NRISmokeTransientLobeRequest request = {};
		uint32_t generation = 0u;
		uint32_t groupSlot = UINT32_MAX;
		bool active = false;
	};
	struct GroupSlot
	{
		std::array<uint32_t, FixedMaximumLobesPerGroup> lobes = {};
		uint32_t lobeCount = 0u;
		uint32_t generation = 0u;
		uint32_t epoch = 0u;
		uint32_t sourceId = 0u;
		uint64_t sourceEventSerial = 0u;
		uint64_t replacementKey = 0u;
		double authoredGameplaySeconds = 0.0;
		double lastFullLightScheduleSeconds = -1.0;
		uint64_t admissionOrdinal = 0u;
		uint64_t admittedFrame = 0u;
		uint64_t fullLightScheduledFrame = 0u;
		float lifetimeSeconds = 0.0f;
		uint16_t shapeRevision = 1u;
		uint16_t lightingRevision = 1u;
		NRISmokeTransientClass transientClass = NRISmokeTransientClass::Diagnostic;
		NRISmokeTransientLightRefresh lightRefresh = NRISmokeTransientLightRefresh::Frozen;
		NRISmokeTransientInterest interest = NRISmokeTransientInterest::Hot;
		bool needsInitialLight = true;
		bool fullLightAllowed = false;
		bool active = false;
	};

	void Refresh();
	void RebuildLightSchedule();
	void InvalidateLightingState();
	void RetireGroup(uint32_t groupSlot, bool expired);
	NRISmokeTransientAdmission Drop(NRISmokeTransientDropReason reason,
		uint32_t requestedLobes);
	static bool Valid(const NRISmokeTransientLobeRequest& request);
	static bool ValidBatchIdentity(const NRISmokeTransientLobeRequest* requests,
		uint32_t count);
	NRISmokeTransientAdmission AdmitBatchWithLimits(
		const NRISmokeTransientLobeRequest* requests, uint32_t count,
		uint32_t maximumLobes, uint32_t minimumLobes, bool retained);
	bool Visible(const LobeSlot& lobe) const;

	std::array<GroupSlot, FixedGroupCapacity> mGroups = {};
	std::array<LobeSlot, FixedLobeCapacity> mLobes = {};
	std::vector<NRISmokeTransientLobeGpu> mGpuLobes;
	std::vector<NRISmokeTransientGroupGpu> mGpuGroups;
	NRISmokeTransientSnapshot mSnapshot = {};
	NRISmokeTransientProfile mProfile = {};
	NRISmokeTransientLightBudgetMetadata mLightBudget = {};
	NRISmokeTransientDropReason mLastDropReason = NRISmokeTransientDropReason::None;
	double mGameplayTimeSeconds = 0.0;
	uint64_t mFrameSerial = 0u;
	uint64_t mAdmissionOrdinal = 0u;
	uint16_t mLightingRevision = 1u;
	bool mHasProfile = false;
	bool mPrepared = false;
};
