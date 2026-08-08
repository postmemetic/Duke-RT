#pragma once

#include "nri_spatial_absence_gate.h"
#include "nri_actor_occurrence_ledger.h"
#include "nri_actor_occurrence_policy.h"

#include "../scene/nri_geometry_bridge.h"
#include "../scene/nri_map_world.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

enum class NRIActorOccurrenceRole : uint32_t
{
	Exact = 0,
	ShadowProxy,
	DynamicAggregate,
};

enum class NRIActorOccurrenceClassification : uint32_t
{
	EvidenceIncomplete = 0,
	StaleLifecycle,
	DuplicateOccurrence,
	StaleTransform,
	WrongLocalitySingleCurrent,
	WrongLocalitySuppressed,
	CurrentLegitimate,
	MixedInvariantFailure,
	RetiredIneligible,
};

enum NRIActorOccurrenceInvariantFlags : uint32_t
{
	NRI_ACTOR_OCCURRENCE_INVARIANT_NONE = 0,
	NRI_ACTOR_OCCURRENCE_INVARIANT_MISSING_IDENTITY = 1u << 0,
	NRI_ACTOR_OCCURRENCE_INVARIANT_MISSING_AUTHORITY = 1u << 1,
	NRI_ACTOR_OCCURRENCE_INVARIANT_STALE_LIFECYCLE = 1u << 2,
	NRI_ACTOR_OCCURRENCE_INVARIANT_DUPLICATE = 1u << 3,
	NRI_ACTOR_OCCURRENCE_INVARIANT_MASK_OVERLAP = 1u << 4,
	NRI_ACTOR_OCCURRENCE_INVARIANT_STALE_TRANSFORM = 1u << 5,
	NRI_ACTOR_OCCURRENCE_INVARIANT_STALE_BINDING = 1u << 6,
	NRI_ACTOR_OCCURRENCE_INVARIANT_MISSING_SECTOR = 1u << 7,
	NRI_ACTOR_OCCURRENCE_INVARIANT_MISSING_BOUNDS = 1u << 8,
	NRI_ACTOR_OCCURRENCE_INVARIANT_INCOMPLETE_CENSUS = 1u << 9,
	NRI_ACTOR_OCCURRENCE_INVARIANT_DYNAMIC_DUPLICATE = 1u << 10,
	NRI_ACTOR_OCCURRENCE_INVARIANT_STALE_OWNER = 1u << 11,
	NRI_ACTOR_OCCURRENCE_INVARIANT_STALE_PLACEMENT = 1u << 12,
	NRI_ACTOR_OCCURRENCE_INVARIANT_LEDGER_REJECTED = 1u << 13,
};

struct NRIActorOccurrenceTraceConfig
{
	bool enabled = false;
	int32_t targetActorIndex = -1;
	const nri_scene::PTMapWorld* mapWorld = nullptr;
	const NRISpatialAbsenceSnapshot* spatialSnapshot = nullptr;
};

struct NRIActorOccurrenceCandidate
{
	uint64_t identityKey = 0;
	uint64_t lifecycleGeneration = 0;
	uint64_t ownerWorldEpoch = 0;
	uint64_t ownerLifetimeGeneration = 0;
	uint64_t placementGeneration = 0;
	uint64_t placementStateHash = 0;
	uint64_t bindingGeneration = 0;
	uint64_t publicationHash = 0;
	uint64_t meshResourceKey = 0;
	uint64_t meshKeyHash = 0;
	uint64_t materialKeyHash = 0;
	int32_t actorIndex = -1;
	int32_t physicalSectorIndex = -1;
	bool active = false;
	bool capturedThisFrame = false;
	bool admitted = false;
	bool publishReady = false;
	bool authorityCurrent = true;
	bool publicationEligible = true;
	bool pendingRemoval = false;
	bool suppressionAuthorized = false;
	uint32_t suppressedWorkloadMask = 0;
	NRIActorOccurrenceLedgerReason ledgerReason = NRIActorOccurrenceLedgerReason::Current;
	std::string reason;
};

struct NRIActorOccurrence
{
	NRIActorOccurrenceRole role = NRIActorOccurrenceRole::Exact;
	uint64_t identityKey = 0;
	uint64_t lifecycleGeneration = 0;
	uint64_t ownerWorldEpoch = 0;
	uint64_t ownerLifetimeGeneration = 0;
	uint64_t placementGeneration = 0;
	uint64_t placementStateHash = 0;
	uint64_t bindingGeneration = 0;
	uint64_t publicationHash = 0;
	uint64_t meshResourceKey = 0;
	uint64_t meshKeyHash = 0;
	uint64_t blasHandle = 0;
	int32_t actorIndex = -1;
	int32_t physicalSectorIndex = -1;
	uint32_t tlasInstanceIndex = UINT32_MAX;
	uint32_t occurrenceGeneration = 0;
	uint32_t expectedOccurrenceGeneration = 0;
	uint32_t workloadMask = 0;
	bool authorityCurrent = true;
	bool publicationEligible = true;
	bool pendingRemoval = false;
	NRIActorOccurrenceLedgerReason ledgerReason = NRIActorOccurrenceLedgerReason::Current;
	bool capturedThisFrame = false;
	bool boundsValid = false;
	std::array<float, 12> transform = {
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f
	};
	float boundsMin[3] = {};
	float boundsMax[3] = {};
};

struct NRIActorOccurrenceFrame
{
	bool enabled = false;
	bool finalized = false;
	bool worldTlasCommitted = false;
	bool authorityFound = false;
	bool identityCurrent = false;
	bool live = false;
	bool pendingRemoval = false;
	bool actorPositionSynchronized = false;
	bool capturedThisFrame = false;
	bool spatialEvidenceComplete = false;
	bool ownerSectorReachedBy360 = false;
	bool ownerChunkNegative = false;
	bool completeBoundsInsideConflict = false;
	bool boundsOverlapConflict = false;
	bool actorPositionInsideConflict = false;
	bool suppressionAuthorized = false;
	uint32_t suppressedWorkloadMask = 0;
	uint32_t frameIndex = 0;
	uint32_t invariantFlags = NRI_ACTOR_OCCURRENCE_INVARIANT_NONE;
	uint64_t identityKey = 0;
	uint64_t lifecycleGeneration = 0;
	uint64_t ownerWorldEpoch = 0;
	uint64_t ownerLifetimeGeneration = 0;
	uint64_t placementGeneration = 0;
	uint64_t placementStateHash = 0;
	uint64_t bindingGeneration = 0;
	uint64_t publicationHash = 0;
	int32_t targetActorIndex = -1;
	int32_t physicalSectorIndex = -1;
	uint32_t physicalChunkIndex = UINT32_MAX;
	int32_t physicalLocalSpaceIndex = -1;
	int32_t rootSectorIndex = -1;
	int32_t rootLocalSpaceIndex = -1;
	bool authorityCurrent = true;
	bool publicationEligible = true;
	NRIActorOccurrenceLedgerReason ledgerReason = NRIActorOccurrenceLedgerReason::Current;
	uint32_t conflictPositiveChunk = UINT32_MAX;
	uint32_t conflictNegativeChunk = UINT32_MAX;
	float actorScenePosition[3] = {};
	float authoritativeTransform[12] = {
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f
	};
	NRIActorOccurrenceClassification classification = NRIActorOccurrenceClassification::EvidenceIncomplete;
	std::vector<NRIActorOccurrenceCandidate> candidates;
	std::vector<NRIActorOccurrence> occurrences;
};

class NRIActorOccurrenceCensus
{
public:
	NRIActorOccurrenceCensus(const NRIActorOccurrenceTraceConfig& config, uint32_t frameIndex);
	bool Targets(int32_t actorIndex) const;
	void RecordCandidate(const NRIActorOccurrenceCandidate& candidate);
	void RecordPolicyDecision(const NRIActorOccurrencePolicyDecision& decision);
	void RecordOccurrence(const NRIActorOccurrence& occurrence);
	NRIActorOccurrenceFrame FinishPersistent();

private:
	void ResolveAuthority(uint64_t identityKey, int32_t actorIndex);
	NRIActorOccurrenceTraceConfig mConfig;
	NRIActorOccurrenceFrame mFrame;
};

void AppendNRIActorDynamicOccurrences(
	NRIActorOccurrenceFrame& frame,
	const nri_scene::GeometryData* dynamicGeometry,
	uint32_t tlasInstanceIndex,
	uint32_t workloadMask);
bool ComputeNRIActorOccurrenceWorldBounds(
	const std::array<float, 12>& transform,
	const float localBoundsMin[3],
	const float localBoundsMax[3],
	float worldBoundsMin[3],
	float worldBoundsMax[3]);
void FinalizeNRIActorOccurrenceFrame(NRIActorOccurrenceFrame& frame, bool worldTlasCommitted);
void TraceNRIActorOccurrenceFrame(const NRIActorOccurrenceFrame& frame);
bool RunNRIActorOccurrenceSelfTests(std::string* failureReason = nullptr);

const char* GetNRIActorOccurrenceRoleName(NRIActorOccurrenceRole role);
const char* GetNRIActorOccurrenceClassificationName(NRIActorOccurrenceClassification classification);
