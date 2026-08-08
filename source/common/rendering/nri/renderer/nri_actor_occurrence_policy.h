#pragma once

#include "nri_spatial_absence_gate.h"

#include <cstdint>
#include <string>
#include <unordered_set>

namespace nri_scene { struct GeometryData; }

enum class NRIActorOccurrencePolicyReason : uint32_t
{
	Disabled = 0,
	NonMainRoot,
	AlternateRayContext,
	InactiveOccurrence,
	DuplicateOccurrence,
	MissingAuthority,
	StaleLifecycle,
	StaleTransform,
	MissingWorld,
	IncompleteCensus,
	MissingSector,
	DifferentLocalSpace,
	OwnerReached,
	OwnerNotNegative,
	NoCertifiedConflict,
	AmbiguousConflict,
	OutsideConflict,
	CertifiedNegativeWholeOccurrence,
};

enum NRIActorOccurrenceContextRiskFlags : uint32_t
{
	NRI_ACTOR_CONTEXT_RISK_NONE = 0,
	NRI_ACTOR_CONTEXT_RISK_NON_PLAYER_CAMERA = 1u << 0,
	NRI_ACTOR_CONTEXT_RISK_MULTIPLE_ROOTS = 1u << 1,
	NRI_ACTOR_CONTEXT_RISK_PORTAL_GRAPH = 1u << 2,
	NRI_ACTOR_CONTEXT_RISK_REACHED_MIRROR = 1u << 3,
	NRI_ACTOR_CONTEXT_RISK_RUNTIME_LINK = 1u << 4,
};

struct NRIActorOccurrencePolicyContext
{
	bool enabled = false;
	bool logicalMainRoot = false;
	uint32_t contextRiskFlags = NRI_ACTOR_CONTEXT_RISK_NONE;
	uint32_t frameIndex = 0;
	const nri_scene::PTMapWorld* mapWorld = nullptr;
	const NRISpatialAbsenceSnapshot* spatialSnapshot = nullptr;
};

struct NRIActorOccurrencePolicyCandidate
{
	uint64_t identityKey = 0;
	int32_t actorIndex = -1;
	uint32_t requestedWorkloadMask = 0;
	bool active = false;
	bool uniqueActiveOccurrence = false;
	bool boundsValid = false;
	float boundsMin[3] = {};
	float boundsMax[3] = {};
};

// Immutable frame-local answer for one live actor occurrence. Suppression only
// controls publication into the current logical-player TLAS/light workload;
// it never retires the actor, its cache entry, or its reusable BLAS.
struct NRIActorOccurrencePolicyDecision
{
	NRIActorOccurrencePolicyReason reason = NRIActorOccurrencePolicyReason::Disabled;
	bool evaluated = false;
	bool suppress = false;
	bool authorityFound = false;
	bool identityCurrent = false;
	bool live = false;
	bool pendingRemoval = false;
	bool actorPositionSynchronized = false;
	bool spatialEvidenceComplete = false;
	bool ownerSectorReachedBy360 = false;
	bool ownerChunkNegative = false;
	bool boundsOverlapConflict = false;
	bool actorPositionInsideConflict = false;
	uint32_t suppressedWorkloadMask = 0;
	uint64_t identityKey = 0;
	uint64_t lifecycleGeneration = 0;
	int32_t actorIndex = -1;
	int32_t physicalSectorIndex = -1;
	uint32_t physicalChunkIndex = UINT32_MAX;
	int32_t physicalLocalSpaceIndex = -1;
	int32_t rootSectorIndex = -1;
	int32_t rootLocalSpaceIndex = -1;
	uint32_t conflictPositiveChunk = UINT32_MAX;
	uint32_t conflictNegativeChunk = UINT32_MAX;
	float actorScenePosition[3] = {};
};

NRIActorOccurrencePolicyDecision EvaluateNRIActorOccurrencePolicy(
	const NRIActorOccurrencePolicyContext& context,
	const NRIActorOccurrencePolicyCandidate& candidate);

const char* GetNRIActorOccurrencePolicyReasonName(NRIActorOccurrencePolicyReason reason);
bool RunNRIActorOccurrencePolicySelfTests(std::string* failureReason = nullptr);

// Copies source geometry only when at least one primitive belongs to a
// suppressed actor, then removes those actor primitives before aggregate BLAS
// construction. Vertices and material slots stay resident/reusable.
uint32_t FilterNRIActorOccurrenceGeometry(
	const nri_scene::GeometryData& source,
	const std::unordered_set<int32_t>& suppressedActorIndices,
	nri_scene::GeometryData& destination);
