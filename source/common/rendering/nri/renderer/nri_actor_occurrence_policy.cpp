#include "nri_actor_occurrence_policy.h"

#include "../scene/nri_scene_bridge.h"
#include "../scene/nri_geometry_bridge.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
	constexpr float kSpatialEpsilon = 0.001f;

	bool IsChunkMarked(const std::vector<uint32_t>& words, uint32_t chunkIndex)
	{
		const uint32_t wordIndex = chunkIndex >> 5u;
		return wordIndex < words.size() &&
			(words[wordIndex] & (1u << (chunkIndex & 31u))) != 0u;
	}

	bool PointInside(const float point[3], const NRISpatialAbsenceConflictRecord& conflict)
	{
		for (uint32_t axis = 0; axis < 3u; ++axis)
		{
			if (!std::isfinite(point[axis]) ||
				point[axis] < conflict.overlapMin[axis] - kSpatialEpsilon ||
				point[axis] > conflict.overlapMax[axis] + kSpatialEpsilon)
			{
				return false;
			}
		}
		return true;
	}

	bool BoundsOverlap(
		const NRIActorOccurrencePolicyCandidate& candidate,
		const NRISpatialAbsenceConflictRecord& conflict)
	{
		if (!candidate.boundsValid)
		{
			return false;
		}
		for (uint32_t axis = 0; axis < 3u; ++axis)
		{
			if (!std::isfinite(candidate.boundsMin[axis]) ||
				!std::isfinite(candidate.boundsMax[axis]) ||
				candidate.boundsMax[axis] < conflict.overlapMin[axis] - kSpatialEpsilon ||
				candidate.boundsMin[axis] > conflict.overlapMax[axis] + kSpatialEpsilon)
			{
				return false;
			}
		}
		return true;
	}

	bool CandidateIntersectsCensusSphere(
		const NRIActorOccurrencePolicyCandidate& candidate,
		const float center[3],
		float radius)
	{
		if (!candidate.boundsValid || !std::isfinite(radius) || radius <= 0.0f)
		{
			return false;
		}
		float distanceSquared = 0.0f;
		for (uint32_t axis = 0; axis < 3u; ++axis)
		{
			if (!std::isfinite(center[axis]) ||
				!std::isfinite(candidate.boundsMin[axis]) ||
				!std::isfinite(candidate.boundsMax[axis]))
			{
				return false;
			}
			const float distance = center[axis] < candidate.boundsMin[axis] ?
				candidate.boundsMin[axis] - center[axis] :
				(center[axis] > candidate.boundsMax[axis] ? center[axis] - candidate.boundsMax[axis] : 0.0f);
			distanceSquared += distance * distance;
		}
		return distanceSquared <= radius * radius;
	}

	struct NRIActorOccurrencePolicyFacts
	{
		NRIActorOccurrencePolicyContext context;
		NRIActorOccurrencePolicyCandidate candidate;
		nri_scene::PersistentVoxelActorAuthorityView authority;
		bool authorityFound = false;
	};

	NRIActorOccurrencePolicyDecision EvaluateFacts(const NRIActorOccurrencePolicyFacts& facts)
	{
		NRIActorOccurrencePolicyDecision decision = {};
		decision.evaluated = true;
		decision.identityKey = facts.candidate.identityKey;
		decision.actorIndex = facts.candidate.actorIndex;
		decision.authorityFound = facts.authorityFound;
		decision.identityCurrent = facts.authority.identityCurrent;
		decision.live = facts.authority.live;
		decision.pendingRemoval = facts.authority.pendingRemoval;
		decision.actorPositionSynchronized = facts.authority.actorPositionSynchronized;
		decision.capturedThisFrame = facts.candidate.capturedThisFrame;
		decision.lifecycleGeneration = facts.authority.lifecycleGeneration;
		decision.physicalSectorIndex = facts.authority.physicalSectorIndex;
		std::memcpy(decision.actorScenePosition, facts.authority.actorScenePosition, sizeof(decision.actorScenePosition));

		auto keep = [&](NRIActorOccurrencePolicyReason reason)
		{
			decision.reason = reason;
			return decision;
		};
		if (!facts.context.enabled) return keep(NRIActorOccurrencePolicyReason::Disabled);
		if (!facts.context.logicalMainRoot) return keep(NRIActorOccurrencePolicyReason::NonMainRoot);
		if (facts.context.contextRiskFlags != NRI_ACTOR_CONTEXT_RISK_NONE)
			return keep(NRIActorOccurrencePolicyReason::AlternateRayContext);
		if (!facts.candidate.active || facts.candidate.requestedWorkloadMask == 0u)
			return keep(NRIActorOccurrencePolicyReason::InactiveOccurrence);
		if (!facts.candidate.uniqueActiveOccurrence)
			return keep(NRIActorOccurrencePolicyReason::DuplicateOccurrence);
		if (!facts.authorityFound) return keep(NRIActorOccurrencePolicyReason::MissingAuthority);
		// Actor lifetime zero is valid for the first serialized map actor. The
		// nonzero world epoch is the ABA-safe validity discriminator.
		if (!facts.authority.identityCurrent || !facts.authority.live || facts.authority.pendingRemoval ||
			facts.authority.ownerWorldEpoch == 0u)
			return keep(NRIActorOccurrencePolicyReason::StaleLifecycle);
		if (!facts.authority.actorPositionSynchronized)
			return keep(NRIActorOccurrencePolicyReason::StaleTransform);

		const nri_scene::PTMapWorld* mapWorld = facts.context.mapWorld;
		const NRISpatialAbsenceSnapshot* snapshot = facts.context.spatialSnapshot;
		if (mapWorld == nullptr || !mapWorld->valid || mapWorld->buildSerial == 0u)
			return keep(NRIActorOccurrencePolicyReason::MissingWorld);
		if (snapshot == nullptr || !snapshot->HasNegativeAuthority() ||
			snapshot->frameIndex != facts.context.frameIndex ||
			snapshot->worldGeneration != mapWorld->buildSerial)
			return keep(NRIActorOccurrencePolicyReason::IncompleteCensus);

		decision.spatialEvidenceComplete = true;
		decision.rootSectorIndex = snapshot->authoritativeRootSector;
		decision.rootLocalSpaceIndex = snapshot->rootLocalSpaceIndex;
		if (facts.authority.physicalSectorIndex < 0 ||
			(size_t)facts.authority.physicalSectorIndex >= mapWorld->sectorChunkLookup.size())
			return keep(NRIActorOccurrencePolicyReason::MissingSector);
		decision.physicalChunkIndex = mapWorld->sectorChunkLookup[(size_t)facts.authority.physicalSectorIndex];
		decision.physicalLocalSpaceIndex =
			nri_scene::FindMapWorldLocalSpaceIndex(*mapWorld, decision.physicalChunkIndex);
		if (decision.physicalChunkIndex == UINT32_MAX || decision.physicalLocalSpaceIndex < 0 ||
			decision.rootLocalSpaceIndex < 0)
			return keep(NRIActorOccurrencePolicyReason::MissingSector);
		if (decision.physicalLocalSpaceIndex != decision.rootLocalSpaceIndex)
			return keep(NRIActorOccurrencePolicyReason::DifferentLocalSpace);

		decision.ownerSectorReachedBy360 = std::binary_search(
			snapshot->reachedSectorIndices.begin(), snapshot->reachedSectorIndices.end(),
			(uint32_t)facts.authority.physicalSectorIndex);
		decision.ownerChunkNegative = IsChunkMarked(snapshot->negativeChunkWords, decision.physicalChunkIndex);
		decision.insideCensusSphere = CandidateIntersectsCensusSphere(
			facts.candidate, snapshot->center, snapshot->guardRadius);
		if (decision.ownerSectorReachedBy360)
			return keep(NRIActorOccurrencePolicyReason::OwnerReached);
		if (!decision.ownerChunkNegative)
			return keep(NRIActorOccurrencePolicyReason::OwnerNotNegative);

		const NRISpatialAbsenceConflictRecord* reachedConflict = nullptr;
		const NRISpatialAbsenceConflictRecord* fallbackConflict = nullptr;
		bool duplicateConflict = false;
		for (const NRISpatialAbsenceConflictRecord& conflict : snapshot->conflicts)
		{
			if (conflict.decision != NRISpatialAbsenceConflictDecision::Certified ||
				conflict.negativeChunk != decision.physicalChunkIndex)
			{
				continue;
			}
			if (fallbackConflict != nullptr &&
				fallbackConflict->positiveChunk == conflict.positiveChunk)
			{
				duplicateConflict = true;
				continue;
			}
			if (fallbackConflict == nullptr)
			{
				fallbackConflict = &conflict;
			}
			const bool positiveReached = conflict.positiveSector >= 0 &&
				std::binary_search(
					snapshot->reachedSectorIndices.begin(), snapshot->reachedSectorIndices.end(),
					(uint32_t)conflict.positiveSector);
			// Prefer a root-local witness for diagnostics, then any reached
			// positive owner. Both are stronger than the bounded negative-owner
			// fallback used for an uncaptured actor inside the census sphere.
			if (positiveReached && (reachedConflict == nullptr ||
				conflict.positiveSector == snapshot->authoritativeRootSector))
			{
				reachedConflict = &conflict;
			}
		}
		const NRISpatialAbsenceConflictRecord* matchedConflict = reachedConflict;
		if (matchedConflict == nullptr && !facts.candidate.capturedThisFrame &&
			decision.insideCensusSphere)
		{
			matchedConflict = fallbackConflict;
		}
		if (matchedConflict == nullptr)
			return keep(NRIActorOccurrencePolicyReason::NoCertifiedConflict);
		if (duplicateConflict)
			return keep(NRIActorOccurrencePolicyReason::AmbiguousConflict);

		decision.conflictPositiveChunk = matchedConflict->positiveChunk;
		decision.conflictNegativeChunk = matchedConflict->negativeChunk;
		// The conflict footprint proves that the two owner sectors are mutually
		// exclusive, but it is only a geometry-overlap sample. An actor elsewhere
		// in the same certified-negative owner can still protrude into the current
		// root (ItemIntrusion). Preserve these facts for diagnostics, but do not
		// make them prerequisites for whole-owner occurrence suppression.
		decision.boundsOverlapConflict = BoundsOverlap(facts.candidate, *matchedConflict);
		decision.actorPositionInsideConflict = PointInside(facts.authority.actorScenePosition, *matchedConflict);
		decision.suppressedWorkloadMask = facts.candidate.requestedWorkloadMask;
		decision.suppress = true;
		decision.reason = NRIActorOccurrencePolicyReason::CertifiedNegativeWholeOccurrence;
		return decision;
	}
}

NRIActorOccurrencePolicyDecision EvaluateNRIActorOccurrencePolicy(
	const NRIActorOccurrencePolicyContext& context,
	const NRIActorOccurrencePolicyCandidate& candidate)
{
	NRIActorOccurrencePolicyFacts facts = {};
	facts.context = context;
	facts.candidate = candidate;
	facts.authorityFound = nri_scene::GetPersistentVoxelActorAuthority(
		candidate.identityKey, candidate.actorIndex, facts.authority);
	return EvaluateFacts(facts);
}

const char* GetNRIActorOccurrencePolicyReasonName(NRIActorOccurrencePolicyReason reason)
{
	switch (reason)
	{
	case NRIActorOccurrencePolicyReason::Disabled: return "disabled";
	case NRIActorOccurrencePolicyReason::NonMainRoot: return "non-main-root";
	case NRIActorOccurrencePolicyReason::AlternateRayContext: return "alternate-ray-context";
	case NRIActorOccurrencePolicyReason::InactiveOccurrence: return "inactive-occurrence";
	case NRIActorOccurrencePolicyReason::DuplicateOccurrence: return "duplicate-occurrence";
	case NRIActorOccurrencePolicyReason::MissingAuthority: return "missing-authority";
	case NRIActorOccurrencePolicyReason::StaleLifecycle: return "stale-lifecycle";
	case NRIActorOccurrencePolicyReason::StaleTransform: return "stale-transform";
	case NRIActorOccurrencePolicyReason::MissingWorld: return "missing-world";
	case NRIActorOccurrencePolicyReason::IncompleteCensus: return "incomplete-census";
	case NRIActorOccurrencePolicyReason::MissingSector: return "missing-sector";
	case NRIActorOccurrencePolicyReason::DifferentLocalSpace: return "different-local-space";
	case NRIActorOccurrencePolicyReason::OwnerReached: return "owner-reached";
	case NRIActorOccurrencePolicyReason::OwnerNotNegative: return "owner-not-negative";
	case NRIActorOccurrencePolicyReason::NoCertifiedConflict: return "no-certified-conflict";
	case NRIActorOccurrencePolicyReason::AmbiguousConflict: return "ambiguous-conflict";
	case NRIActorOccurrencePolicyReason::OutsideConflict: return "outside-conflict";
	case NRIActorOccurrencePolicyReason::CertifiedNegativeWholeOccurrence: return "certified-negative-whole-occurrence";
	default: return "unknown";
	}
}

bool RunNRIActorOccurrencePolicySelfTests(std::string* failureReason)
{
	auto fail = [&](const char* reason)
	{
		if (failureReason != nullptr) *failureReason = reason;
		return false;
	};
	auto makeFacts = []()
	{
		static nri_scene::PTMapWorld mapWorld;
		static NRISpatialAbsenceSnapshot snapshot;
		mapWorld.Reset();
		mapWorld.valid = true;
		mapWorld.buildSerial = 41u;
		mapWorld.sectorChunkLookup = { 0u, 1u };
		mapWorld.chunks.resize(2u);
		mapWorld.chunks[0].chunkIndex = 0u;
		mapWorld.chunks[0].sectorIndex = 0;
		mapWorld.chunks[0].localSpaceIndex = 0u;
		mapWorld.chunks[1].chunkIndex = 1u;
		mapWorld.chunks[1].sectorIndex = 1;
		mapWorld.chunks[1].localSpaceIndex = 0u;
		mapWorld.localSpaces.resize(1u);
		mapWorld.localSpaces[0].localSpaceIndex = 0u;
		mapWorld.localSpaces[0].anchorSectorIndex = 0;
		mapWorld.localSpaces[0].chunkCount = 2u;
		snapshot = {};
		snapshot.valid = true;
		snapshot.worldGeneration = 41u;
		snapshot.frameIndex = 9u;
		snapshot.certifiedCount = 1u;
		snapshot.authoritativeRootSector = 0;
		snapshot.rootLocalSpaceIndex = 0;
		snapshot.reachedSectorIndices = { 0u };
		snapshot.reachedChunkWords = { 1u << 0u };
		snapshot.negativeChunkWords = { 1u << 1u };
		// Policy tests consume an already-authorized census snapshot. The
		// spatial owner independently tests and publishes this seal after full
		// serialized-payload validation.
		snapshot.gpuRecords.resize(1u);
		snapshot.gpuRecords[0].flags = NRI_SPATIAL_ABSENCE_GPU_RAY_QUERY_VALIDATED;
		NRISpatialAbsenceConflictRecord conflict;
		conflict.decision = NRISpatialAbsenceConflictDecision::Certified;
		conflict.positiveChunk = 0u;
		conflict.negativeChunk = 1u;
		conflict.positiveSector = 0;
		conflict.negativeSector = 1;
		conflict.overlapMin[0] = conflict.overlapMin[1] = conflict.overlapMin[2] = -2.0f;
		conflict.overlapMax[0] = conflict.overlapMax[1] = conflict.overlapMax[2] = 2.0f;
		snapshot.conflicts = { conflict };

		NRIActorOccurrencePolicyFacts facts;
		facts.context.enabled = true;
		facts.context.logicalMainRoot = true;
		facts.context.frameIndex = 9u;
		facts.context.mapWorld = &mapWorld;
		facts.context.spatialSnapshot = &snapshot;
		facts.candidate.identityKey = 11u;
		facts.candidate.actorIndex = 7;
		facts.candidate.requestedWorkloadMask = 0xffu;
		facts.candidate.active = true;
		facts.candidate.uniqueActiveOccurrence = true;
		facts.candidate.capturedThisFrame = false;
		facts.candidate.boundsValid = true;
		facts.candidate.boundsMin[0] = facts.candidate.boundsMin[1] = facts.candidate.boundsMin[2] = -1.0f;
		facts.candidate.boundsMax[0] = facts.candidate.boundsMax[1] = facts.candidate.boundsMax[2] = 1.0f;
		facts.authorityFound = true;
		facts.authority.identityKey = 11u;
		facts.authority.ownerWorldEpoch = 3u;
		facts.authority.ownerLifetimeGeneration = 7u;
		facts.authority.lifecycleGeneration = 7u;
		facts.authority.actorIndex = 7;
		facts.authority.physicalSectorIndex = 1;
		facts.authority.identityCurrent = true;
		facts.authority.live = true;
		facts.authority.actorPositionSynchronized = true;
		return facts;
	};

	NRIActorOccurrencePolicyFacts facts = makeFacts();
	NRIActorOccurrencePolicyDecision decision = EvaluateFacts(facts);
	if (!decision.suppress || decision.reason != NRIActorOccurrencePolicyReason::CertifiedNegativeWholeOccurrence ||
		decision.suppressedWorkloadMask != 0xffu)
		return fail("certified negative occurrence was not suppressed");

	facts = makeFacts();
	facts.authority.ownerLifetimeGeneration = 0u;
	facts.authority.lifecycleGeneration = 0u;
	decision = EvaluateFacts(facts);
	if (!decision.suppress || decision.reason != NRIActorOccurrencePolicyReason::CertifiedNegativeWholeOccurrence)
		return fail("valid lifetime generation zero was rejected");

	facts = makeFacts();
	const_cast<NRISpatialAbsenceSnapshot*>(facts.context.spatialSnapshot)->reachedSectorIndices.push_back(1u);
	decision = EvaluateFacts(facts);
	if (decision.suppress || decision.reason != NRIActorOccurrencePolicyReason::OwnerReached)
		return fail("reached owner did not fail open");

	facts = makeFacts();
	facts.candidate.uniqueActiveOccurrence = false;
	decision = EvaluateFacts(facts);
	if (decision.suppress || decision.reason != NRIActorOccurrencePolicyReason::DuplicateOccurrence)
		return fail("duplicate occurrence did not fail open");

	facts = makeFacts();
	facts.context.logicalMainRoot = false;
	decision = EvaluateFacts(facts);
	if (decision.suppress || decision.reason != NRIActorOccurrencePolicyReason::NonMainRoot)
		return fail("alternate root did not fail open");

	facts = makeFacts();
	facts.context.contextRiskFlags = NRI_ACTOR_CONTEXT_RISK_PORTAL_GRAPH;
	decision = EvaluateFacts(facts);
	if (decision.suppress || decision.reason != NRIActorOccurrencePolicyReason::AlternateRayContext)
		return fail("alternate ray context did not fail open");

	facts = makeFacts();
	facts.authority.actorScenePosition[0] = 10.0f;
	facts.candidate.boundsMin[0] = 9.0f;
	facts.candidate.boundsMax[0] = 11.0f;
	decision = EvaluateFacts(facts);
	if (!decision.suppress || decision.reason != NRIActorOccurrencePolicyReason::CertifiedNegativeWholeOccurrence ||
		decision.actorPositionInsideConflict || decision.boundsOverlapConflict)
		return fail("certified negative owner incorrectly required actor/footprint overlap");

	facts = makeFacts();
	const_cast<NRISpatialAbsenceSnapshot*>(facts.context.spatialSnapshot)->conflicts.clear();
	decision = EvaluateFacts(facts);
	if (decision.suppress || decision.reason != NRIActorOccurrencePolicyReason::NoCertifiedConflict)
		return fail("negative owner without a certified reached conflict did not fail open");

	facts = makeFacts();
	auto* adjacentRootSnapshot = const_cast<NRISpatialAbsenceSnapshot*>(facts.context.spatialSnapshot);
	auto* adjacentRootWorld = const_cast<nri_scene::PTMapWorld*>(facts.context.mapWorld);
	adjacentRootWorld->sectorChunkLookup.push_back(2u);
	adjacentRootWorld->chunks.resize(3u);
	adjacentRootWorld->chunks[2].chunkIndex = 2u;
	adjacentRootWorld->chunks[2].sectorIndex = 2;
	adjacentRootWorld->chunks[2].localSpaceIndex = 0u;
	adjacentRootWorld->localSpaces[0].chunkCount = 3u;
	adjacentRootSnapshot->authoritativeRootSector = 2;
	adjacentRootSnapshot->reachedSectorIndices = { 0u, 2u };
	NRISpatialAbsenceConflictRecord rootConflict = adjacentRootSnapshot->conflicts.front();
	rootConflict.positiveChunk = 2u;
	rootConflict.positiveSector = 2;
	adjacentRootSnapshot->conflicts.push_back(rootConflict);
	decision = EvaluateFacts(facts);
	if (!decision.suppress || decision.reason != NRIActorOccurrencePolicyReason::CertifiedNegativeWholeOccurrence ||
		decision.conflictPositiveChunk != 2u)
		return fail("negative owner with multiple reached witnesses was not suppressed from an adjacent root");

	facts = makeFacts();
	auto* unreachedSnapshot = const_cast<NRISpatialAbsenceSnapshot*>(facts.context.spatialSnapshot);
	auto* unreachedWorld = const_cast<nri_scene::PTMapWorld*>(facts.context.mapWorld);
	unreachedWorld->sectorChunkLookup.push_back(2u);
	unreachedWorld->chunks.resize(3u);
	unreachedWorld->chunks[2].chunkIndex = 2u;
	unreachedWorld->chunks[2].sectorIndex = 2;
	unreachedWorld->chunks[2].localSpaceIndex = 0u;
	unreachedWorld->localSpaces[0].chunkCount = 3u;
	unreachedSnapshot->authoritativeRootSector = 2;
	unreachedSnapshot->reachedSectorIndices = { 2u };
	decision = EvaluateFacts(facts);
	if (decision.suppress || decision.reason != NRIActorOccurrencePolicyReason::NoCertifiedConflict)
		return fail("unreached positive witness authorized whole-occurrence suppression");

	facts = makeFacts();
	auto* sphereFallbackSnapshot = const_cast<NRISpatialAbsenceSnapshot*>(facts.context.spatialSnapshot);
	auto* sphereFallbackWorld = const_cast<nri_scene::PTMapWorld*>(facts.context.mapWorld);
	sphereFallbackWorld->sectorChunkLookup.push_back(2u);
	sphereFallbackWorld->chunks.resize(3u);
	sphereFallbackWorld->chunks[2].chunkIndex = 2u;
	sphereFallbackWorld->chunks[2].sectorIndex = 2;
	sphereFallbackWorld->chunks[2].localSpaceIndex = 0u;
	sphereFallbackWorld->localSpaces[0].chunkCount = 3u;
	sphereFallbackSnapshot->authoritativeRootSector = 2;
	sphereFallbackSnapshot->reachedSectorIndices = { 2u };
	sphereFallbackSnapshot->guardRadius = 8.0f;
	decision = EvaluateFacts(facts);
	if (!decision.suppress || decision.reason != NRIActorOccurrencePolicyReason::CertifiedNegativeWholeOccurrence ||
		!decision.insideCensusSphere)
		return fail("uncaptured negative owner inside the census sphere did not use its certified overlap witness");

	facts.candidate.capturedThisFrame = true;
	decision = EvaluateFacts(facts);
	if (decision.suppress || decision.reason != NRIActorOccurrencePolicyReason::NoCertifiedConflict)
		return fail("hardware-captured actor used the bounded negative-owner fallback");

	facts.candidate.capturedThisFrame = false;
	facts.candidate.boundsMin[0] = facts.candidate.boundsMin[1] = facts.candidate.boundsMin[2] = 20.0f;
	facts.candidate.boundsMax[0] = facts.candidate.boundsMax[1] = facts.candidate.boundsMax[2] = 22.0f;
	decision = EvaluateFacts(facts);
	if (decision.suppress || decision.reason != NRIActorOccurrencePolicyReason::NoCertifiedConflict ||
		decision.insideCensusSphere)
		return fail("actor outside the census sphere used the bounded negative-owner fallback");

	facts.candidate.boundsValid = false;
	decision = EvaluateFacts(facts);
	if (decision.suppress || decision.reason != NRIActorOccurrencePolicyReason::NoCertifiedConflict ||
		decision.insideCensusSphere)
		return fail("actor with invalid bounds used the bounded negative-owner fallback");

	facts = makeFacts();
	auto* ambiguousSnapshot = const_cast<NRISpatialAbsenceSnapshot*>(facts.context.spatialSnapshot);
	const NRISpatialAbsenceConflictRecord duplicateConflict = ambiguousSnapshot->conflicts.front();
	ambiguousSnapshot->conflicts.push_back(duplicateConflict);
	decision = EvaluateFacts(facts);
	if (decision.suppress || decision.reason != NRIActorOccurrencePolicyReason::AmbiguousConflict)
		return fail("ambiguous owner conflicts did not fail open");

	nri_scene::GeometryData sourceGeometry;
	sourceGeometry.vertices.resize(4u);
	for (uint32_t primitiveIndex = 0u; primitiveIndex < 2u; ++primitiveIndex)
	{
		nri_scene::PrimitiveData primitive;
		primitive.indices[0] = 0u;
		primitive.indices[1] = primitiveIndex + 1u;
		primitive.indices[2] = primitiveIndex + 2u;
		sourceGeometry.primitives.push_back(primitive);
		sourceGeometry.indices.push_back(primitive.indices[0]);
		sourceGeometry.indices.push_back(primitive.indices[1]);
		sourceGeometry.indices.push_back(primitive.indices[2]);
		nri_scene::SurfaceProvenance provenance;
		provenance.actorIndex = primitiveIndex == 0u ? 7 : 8;
		sourceGeometry.primitiveProvenance.push_back(provenance);
	}
	nri_scene::GeometryData filteredGeometry;
	const std::unordered_set<int32_t> suppressedActors = { 7 };
	if (FilterNRIActorOccurrenceGeometry(sourceGeometry, suppressedActors, filteredGeometry) != 1u ||
		filteredGeometry.primitives.size() != 1u || filteredGeometry.indices.size() != 3u ||
		filteredGeometry.primitiveProvenance.size() != 1u ||
		filteredGeometry.primitiveProvenance.front().actorIndex != 8)
		return fail("aggregate actor geometry suppression was not atomic");

	return true;
}

uint32_t FilterNRIActorOccurrenceGeometry(
	const nri_scene::GeometryData& source,
	const std::unordered_set<int32_t>& suppressedActorIndices,
	nri_scene::GeometryData& destination)
{
	if (suppressedActorIndices.empty())
	{
		return 0u;
	}
	const size_t pairedCount = std::min(source.primitives.size(), source.primitiveProvenance.size());
	uint32_t removedCount = 0u;
	for (size_t primitiveIndex = 0; primitiveIndex < pairedCount; ++primitiveIndex)
	{
		const int32_t actorIndex = source.primitiveProvenance[primitiveIndex].actorIndex;
		if (actorIndex >= 0 && suppressedActorIndices.find(actorIndex) != suppressedActorIndices.end())
		{
			removedCount++;
		}
	}
	if (removedCount == 0u)
	{
		return 0u;
	}

	destination.vertices = source.vertices;
	destination.indices.clear();
	destination.primitives.clear();
	destination.primitiveProvenance.clear();
	destination.indices.reserve((source.primitives.size() - removedCount) * 3u);
	destination.primitives.reserve(source.primitives.size() - removedCount);
	destination.primitiveProvenance.reserve(source.primitives.size() - removedCount);
	for (size_t primitiveIndex = 0; primitiveIndex < source.primitives.size(); ++primitiveIndex)
	{
		const bool paired = primitiveIndex < source.primitiveProvenance.size();
		const int32_t actorIndex = paired ? source.primitiveProvenance[primitiveIndex].actorIndex : -1;
		if (actorIndex >= 0 && suppressedActorIndices.find(actorIndex) != suppressedActorIndices.end())
		{
			continue;
		}
		const nri_scene::PrimitiveData& primitive = source.primitives[primitiveIndex];
		destination.primitives.push_back(primitive);
		if (paired)
		{
			destination.primitiveProvenance.push_back(source.primitiveProvenance[primitiveIndex]);
		}
		destination.indices.push_back(primitive.indices[0]);
		destination.indices.push_back(primitive.indices[1]);
		destination.indices.push_back(primitive.indices[2]);
	}
	return removedCount;
}
