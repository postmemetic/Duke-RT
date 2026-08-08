#include "nri_actor_occurrence_diagnostics.h"
#include "nri_actor_occurrence_ledger.h"

#include "../scene/nri_scene_bridge.h"

#include "printf.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace
{
	constexpr float kTransformEpsilon = 0.001f;

	bool SameTransform(const std::array<float, 12>& left, const float right[12])
	{
		for (uint32_t index = 0; index < 12u; ++index)
		{
			if (!std::isfinite(left[index]) || !std::isfinite(right[index]) ||
				std::abs(left[index] - right[index]) > kTransformEpsilon)
			{
				return false;
			}
		}
		return true;
	}

	bool IsChunkMarked(const std::vector<uint32_t>& words, uint32_t chunkIndex)
	{
		const uint32_t wordIndex = chunkIndex >> 5u;
		return wordIndex < words.size() &&
			(words[wordIndex] & (1u << (chunkIndex & 31u))) != 0u;
	}

	bool BoundsInside(const NRIActorOccurrence& occurrence, const NRISpatialAbsenceConflictRecord& conflict)
	{
		if (!occurrence.boundsValid)
		{
			return false;
		}
		constexpr float Epsilon = 0.001f;
		for (uint32_t axis = 0; axis < 3u; ++axis)
		{
			if (!std::isfinite(occurrence.boundsMin[axis]) || !std::isfinite(occurrence.boundsMax[axis]) ||
				occurrence.boundsMin[axis] < conflict.overlapMin[axis] - Epsilon ||
				occurrence.boundsMax[axis] > conflict.overlapMax[axis] + Epsilon)
			{
				return false;
			}
		}
		return true;
	}

	bool BoundsOverlap(const NRIActorOccurrence& occurrence, const NRISpatialAbsenceConflictRecord& conflict)
	{
		if (!occurrence.boundsValid)
		{
			return false;
		}
		constexpr float Epsilon = 0.001f;
		for (uint32_t axis = 0; axis < 3u; ++axis)
		{
			if (occurrence.boundsMax[axis] < conflict.overlapMin[axis] - Epsilon ||
				occurrence.boundsMin[axis] > conflict.overlapMax[axis] + Epsilon)
			{
				return false;
			}
		}
		return true;
	}

	bool PointInside(const float point[3], const NRISpatialAbsenceConflictRecord& conflict)
	{
		constexpr float Epsilon = 0.001f;
		for (uint32_t axis = 0; axis < 3u; ++axis)
		{
			if (!std::isfinite(point[axis]) || point[axis] < conflict.overlapMin[axis] - Epsilon ||
				point[axis] > conflict.overlapMax[axis] + Epsilon)
			{
				return false;
			}
		}
		return true;
	}

	bool MasksOverlap(uint32_t left, uint32_t right)
	{
		return (left & right) != 0u;
	}

	bool OwnerStampValid(uint64_t worldEpoch, uint64_t lifetimeGeneration)
	{
		(void)lifetimeGeneration;
		return worldEpoch != 0;
	}

	bool IsRetirementReason(NRIActorOccurrenceLedgerReason reason)
	{
		return reason == NRIActorOccurrenceLedgerReason::StaleAuthority ||
			reason == NRIActorOccurrenceLedgerReason::PendingRemoval ||
			reason == NRIActorOccurrenceLedgerReason::PublicationIneligible;
	}

	NRIActorOccurrenceClassification ClassifyFrame(NRIActorOccurrenceFrame& frame)
	{
		uint32_t invariantFlags = frame.invariantFlags;
		if (!frame.authorityFound || frame.identityKey == 0 ||
			(!OwnerStampValid(frame.ownerWorldEpoch, frame.ownerLifetimeGeneration) &&
			 frame.lifecycleGeneration == 0))
		{
			invariantFlags |= NRI_ACTOR_OCCURRENCE_INVARIANT_MISSING_AUTHORITY;
		}
		if (!frame.identityCurrent || !frame.live || frame.pendingRemoval)
		{
			invariantFlags |= NRI_ACTOR_OCCURRENCE_INVARIANT_STALE_LIFECYCLE;
		}
		if (frame.authorityFound && frame.live && !frame.actorPositionSynchronized)
		{
			invariantFlags |= NRI_ACTOR_OCCURRENCE_INVARIANT_STALE_TRANSFORM;
		}
		if (frame.physicalSectorIndex < 0 || frame.physicalChunkIndex == UINT32_MAX ||
			frame.physicalLocalSpaceIndex < 0)
		{
			invariantFlags |= NRI_ACTOR_OCCURRENCE_INVARIANT_MISSING_SECTOR;
		}

		std::vector<const NRIActorOccurrence*> exact;
		std::vector<const NRIActorOccurrence*> proxies;
		std::vector<const NRIActorOccurrence*> dynamic;
		bool staleTransform = false;
		bool staleBinding = false;
		bool missingBounds = false;
		for (const NRIActorOccurrence& occurrence : frame.occurrences)
		{
			if (occurrence.identityKey == 0 ||
				(!OwnerStampValid(occurrence.ownerWorldEpoch, occurrence.ownerLifetimeGeneration) &&
				 occurrence.lifecycleGeneration == 0))
			{
				invariantFlags |= NRI_ACTOR_OCCURRENCE_INVARIANT_MISSING_IDENTITY;
			}
			if (OwnerStampValid(frame.ownerWorldEpoch, frame.ownerLifetimeGeneration) &&
				OwnerStampValid(occurrence.ownerWorldEpoch, occurrence.ownerLifetimeGeneration) &&
				(frame.ownerWorldEpoch != occurrence.ownerWorldEpoch ||
				 frame.ownerLifetimeGeneration != occurrence.ownerLifetimeGeneration))
			{
				invariantFlags |= NRI_ACTOR_OCCURRENCE_INVARIANT_STALE_OWNER;
			}
			if (frame.placementGeneration != 0 && occurrence.placementGeneration != 0 &&
				(frame.placementGeneration != occurrence.placementGeneration ||
				 (frame.placementStateHash != 0 && occurrence.placementStateHash != 0 &&
				  frame.placementStateHash != occurrence.placementStateHash)))
			{
				invariantFlags |= NRI_ACTOR_OCCURRENCE_INVARIANT_STALE_PLACEMENT;
			}
			if ((frame.bindingGeneration != 0 && occurrence.bindingGeneration != 0 &&
				 frame.bindingGeneration != occurrence.bindingGeneration) ||
				(frame.publicationHash != 0 && occurrence.publicationHash != 0 &&
				 frame.publicationHash != occurrence.publicationHash))
			{
				staleBinding = true;
			}
			if (!occurrence.authorityCurrent || !occurrence.publicationEligible ||
				occurrence.pendingRemoval || occurrence.ledgerReason != NRIActorOccurrenceLedgerReason::Current)
			{
				invariantFlags |= NRI_ACTOR_OCCURRENCE_INVARIANT_LEDGER_REJECTED;
			}
			if (occurrence.role == NRIActorOccurrenceRole::Exact)
			{
				exact.push_back(&occurrence);
				staleTransform = staleTransform || !SameTransform(occurrence.transform, frame.authoritativeTransform);
				staleBinding = staleBinding ||
					occurrence.occurrenceGeneration == 0 ||
					occurrence.occurrenceGeneration != occurrence.expectedOccurrenceGeneration;
				missingBounds = missingBounds || !occurrence.boundsValid;
			}
			else if (occurrence.role == NRIActorOccurrenceRole::ShadowProxy)
			{
				proxies.push_back(&occurrence);
			}
			else
			{
				dynamic.push_back(&occurrence);
			}
		}
		if (exact.size() > 1u)
		{
			invariantFlags |= NRI_ACTOR_OCCURRENCE_INVARIANT_DUPLICATE;
		}
		for (size_t left = 0; left < exact.size(); ++left)
		{
			for (size_t right = left + 1u; right < exact.size(); ++right)
			{
				if (MasksOverlap(exact[left]->workloadMask, exact[right]->workloadMask))
				{
					invariantFlags |= NRI_ACTOR_OCCURRENCE_INVARIANT_MASK_OVERLAP;
				}
			}
			for (const NRIActorOccurrence* proxy : proxies)
			{
				if (MasksOverlap(exact[left]->workloadMask, proxy->workloadMask))
				{
					invariantFlags |= NRI_ACTOR_OCCURRENCE_INVARIANT_MASK_OVERLAP;
				}
			}
		}
		if (!exact.empty() && !dynamic.empty())
		{
			invariantFlags |= NRI_ACTOR_OCCURRENCE_INVARIANT_DYNAMIC_DUPLICATE;
		}
		if (staleTransform)
		{
			invariantFlags |= NRI_ACTOR_OCCURRENCE_INVARIANT_STALE_TRANSFORM;
		}
		if (staleBinding)
		{
			invariantFlags |= NRI_ACTOR_OCCURRENCE_INVARIANT_STALE_BINDING;
		}
		if (missingBounds)
		{
			invariantFlags |= NRI_ACTOR_OCCURRENCE_INVARIANT_MISSING_BOUNDS;
		}
		frame.invariantFlags = invariantFlags;

		const bool candidatesRetired = std::all_of(
			frame.candidates.begin(), frame.candidates.end(),
			[](const NRIActorOccurrenceCandidate& candidate)
			{
				return !candidate.admitted &&
					(IsRetirementReason(candidate.ledgerReason) ||
					 !candidate.authorityCurrent || !candidate.publicationEligible || candidate.pendingRemoval);
			});
		const bool retirementProof = frame.worldTlasCommitted && frame.authorityFound &&
			OwnerStampValid(frame.ownerWorldEpoch, frame.ownerLifetimeGeneration) &&
			frame.occurrences.empty() && candidatesRetired &&
			(IsRetirementReason(frame.ledgerReason) || !frame.authorityCurrent ||
			 !frame.publicationEligible || frame.pendingRemoval);
		if (retirementProof)
		{
			return NRIActorOccurrenceClassification::RetiredIneligible;
		}

		const uint32_t incompleteFlags =
			NRI_ACTOR_OCCURRENCE_INVARIANT_MISSING_IDENTITY |
			NRI_ACTOR_OCCURRENCE_INVARIANT_MISSING_AUTHORITY |
			NRI_ACTOR_OCCURRENCE_INVARIANT_MISSING_SECTOR |
			NRI_ACTOR_OCCURRENCE_INVARIANT_MISSING_BOUNDS |
			NRI_ACTOR_OCCURRENCE_INVARIANT_INCOMPLETE_CENSUS;
		const bool suppressionCandidateValid =
			frame.candidates.size() == 1u &&
			frame.candidates.front().publishReady &&
			frame.candidates.front().suppressionAuthorized &&
			!frame.candidates.front().admitted &&
			frame.candidates.front().suppressedWorkloadMask != 0u;
		const bool suppressionProof =
			frame.suppressionAuthorized && frame.suppressedWorkloadMask != 0u &&
			suppressionCandidateValid && exact.empty() && proxies.empty() && dynamic.empty();
		if ((invariantFlags & incompleteFlags) != 0u || (exact.empty() && !suppressionProof))
		{
			return NRIActorOccurrenceClassification::EvidenceIncomplete;
		}

		const bool staleLifecycle = (invariantFlags &
			(NRI_ACTOR_OCCURRENCE_INVARIANT_STALE_LIFECYCLE |
			 NRI_ACTOR_OCCURRENCE_INVARIANT_STALE_OWNER |
			 NRI_ACTOR_OCCURRENCE_INVARIANT_LEDGER_REJECTED)) != 0u;
		const bool duplicate = (invariantFlags &
			(NRI_ACTOR_OCCURRENCE_INVARIANT_DUPLICATE |
			 NRI_ACTOR_OCCURRENCE_INVARIANT_MASK_OVERLAP |
			 NRI_ACTOR_OCCURRENCE_INVARIANT_DYNAMIC_DUPLICATE)) != 0u;
		const bool stalePublication = (invariantFlags &
			(NRI_ACTOR_OCCURRENCE_INVARIANT_STALE_TRANSFORM |
			 NRI_ACTOR_OCCURRENCE_INVARIANT_STALE_BINDING |
			 NRI_ACTOR_OCCURRENCE_INVARIANT_STALE_PLACEMENT)) != 0u;
		const bool wrongLocality = frame.spatialEvidenceComplete && frame.ownerChunkNegative &&
			!frame.ownerSectorReachedBy360 && frame.boundsOverlapConflict &&
			std::any_of(exact.begin(), exact.end(), [](const NRIActorOccurrence* occurrence)
			{
				return (occurrence->workloadMask & 1u) != 0u;
			});
		const bool wrongLocalitySuppressed = suppressionProof && frame.spatialEvidenceComplete &&
			frame.ownerChunkNegative && !frame.ownerSectorReachedBy360 &&
			frame.boundsOverlapConflict && frame.actorPositionInsideConflict;
		const uint32_t provenClasses = (staleLifecycle ? 1u : 0u) + (duplicate ? 1u : 0u) +
			(stalePublication ? 1u : 0u) + (wrongLocality ? 1u : 0u) +
			(wrongLocalitySuppressed ? 1u : 0u);
		if (provenClasses > 1u)
		{
			return NRIActorOccurrenceClassification::MixedInvariantFailure;
		}
		if (staleLifecycle)
		{
			return NRIActorOccurrenceClassification::StaleLifecycle;
		}
		if (duplicate)
		{
			return NRIActorOccurrenceClassification::DuplicateOccurrence;
		}
		if (stalePublication)
		{
			return NRIActorOccurrenceClassification::StaleTransform;
		}
		if (wrongLocality)
		{
			return NRIActorOccurrenceClassification::WrongLocalitySingleCurrent;
		}
		if (wrongLocalitySuppressed)
		{
			return NRIActorOccurrenceClassification::WrongLocalitySuppressed;
		}
		return NRIActorOccurrenceClassification::CurrentLegitimate;
	}
}

NRIActorOccurrenceCensus::NRIActorOccurrenceCensus(
	const NRIActorOccurrenceTraceConfig& config,
	uint32_t frameIndex)
	: mConfig(config)
{
	mFrame.enabled = config.enabled && config.targetActorIndex >= 0;
	mFrame.frameIndex = frameIndex;
	mFrame.targetActorIndex = config.targetActorIndex;
}

bool NRIActorOccurrenceCensus::Targets(int32_t actorIndex) const
{
	return mFrame.enabled && actorIndex == mFrame.targetActorIndex;
}

void NRIActorOccurrenceCensus::ResolveAuthority(uint64_t identityKey, int32_t actorIndex)
{
	if (!Targets(actorIndex))
	{
		return;
	}
	nri_scene::PersistentVoxelActorAuthorityView authority;
	mFrame.authorityFound = nri_scene::GetPersistentVoxelActorAuthority(identityKey, actorIndex, authority);
	if (!mFrame.authorityFound || !authority.identityCurrent)
	{
		return;
	}
	mFrame.identityKey = authority.identityKey;
	mFrame.lifecycleGeneration = authority.lifecycleGeneration;
	mFrame.identityCurrent = authority.identityCurrent;
	mFrame.live = authority.live;
	mFrame.pendingRemoval = authority.pendingRemoval;
	mFrame.actorPositionSynchronized = authority.actorPositionSynchronized;
	mFrame.capturedThisFrame = authority.capturedThisFrame;
	mFrame.physicalSectorIndex = authority.physicalSectorIndex;
	std::memcpy(mFrame.actorScenePosition, authority.actorScenePosition, sizeof(mFrame.actorScenePosition));
	std::memcpy(mFrame.authoritativeTransform, authority.authoritativeInstanceTransform, sizeof(mFrame.authoritativeTransform));

	const nri_scene::PTMapWorld* mapWorld = mConfig.mapWorld;
	const NRISpatialAbsenceSnapshot* snapshot = mConfig.spatialSnapshot;
	if (mapWorld == nullptr || snapshot == nullptr || !mapWorld->valid ||
		snapshot->frameIndex != mFrame.frameIndex || snapshot->worldGeneration != mapWorld->buildSerial ||
		authority.physicalSectorIndex < 0 || (size_t)authority.physicalSectorIndex >= mapWorld->sectorChunkLookup.size())
	{
		mFrame.invariantFlags |= NRI_ACTOR_OCCURRENCE_INVARIANT_INCOMPLETE_CENSUS;
		return;
	}
	mFrame.physicalChunkIndex = mapWorld->sectorChunkLookup[(size_t)authority.physicalSectorIndex];
	mFrame.physicalLocalSpaceIndex = nri_scene::FindMapWorldLocalSpaceIndex(*mapWorld, mFrame.physicalChunkIndex);
	mFrame.rootSectorIndex = snapshot->authoritativeRootSector;
	mFrame.rootLocalSpaceIndex = snapshot->rootLocalSpaceIndex;
	mFrame.ownerSectorReachedBy360 = std::binary_search(
		snapshot->reachedSectorIndices.begin(), snapshot->reachedSectorIndices.end(),
		(uint32_t)authority.physicalSectorIndex);
	mFrame.ownerChunkNegative = IsChunkMarked(snapshot->negativeChunkWords, mFrame.physicalChunkIndex);
	for (const NRISpatialAbsenceConflictRecord& conflict : snapshot->conflicts)
	{
		if (conflict.decision != NRISpatialAbsenceConflictDecision::Certified ||
			conflict.negativeChunk != mFrame.physicalChunkIndex ||
			conflict.positiveSector != snapshot->authoritativeRootSector)
		{
			continue;
		}
		mFrame.spatialEvidenceComplete = snapshot->HasNegativeAuthority();
		mFrame.conflictPositiveChunk = conflict.positiveChunk;
		mFrame.conflictNegativeChunk = conflict.negativeChunk;
		break;
	}
}

void NRIActorOccurrenceCensus::RecordCandidate(const NRIActorOccurrenceCandidate& candidate)
{
	if (!Targets(candidate.actorIndex))
	{
		return;
	}
	mFrame.candidates.push_back(candidate);
	if (OwnerStampValid(candidate.ownerWorldEpoch, candidate.ownerLifetimeGeneration) &&
		(!OwnerStampValid(mFrame.ownerWorldEpoch, mFrame.ownerLifetimeGeneration) ||
		 candidate.identityKey == mFrame.identityKey))
	{
		mFrame.ownerWorldEpoch = candidate.ownerWorldEpoch;
		mFrame.ownerLifetimeGeneration = candidate.ownerLifetimeGeneration;
		mFrame.placementGeneration = candidate.placementGeneration;
		mFrame.placementStateHash = candidate.placementStateHash;
		mFrame.bindingGeneration = candidate.bindingGeneration;
		mFrame.publicationHash = candidate.publicationHash;
		mFrame.physicalSectorIndex = candidate.physicalSectorIndex;
		mFrame.authorityCurrent = candidate.authorityCurrent;
		mFrame.publicationEligible = candidate.publicationEligible;
		mFrame.pendingRemoval = candidate.pendingRemoval;
		mFrame.ledgerReason = candidate.ledgerReason;
	}
	if (!mFrame.authorityFound || !mFrame.identityCurrent || candidate.identityKey == mFrame.identityKey)
	{
		ResolveAuthority(candidate.identityKey, candidate.actorIndex);
	}
}

void NRIActorOccurrenceCensus::RecordPolicyDecision(const NRIActorOccurrencePolicyDecision& decision)
{
	if (!Targets(decision.actorIndex))
	{
		return;
	}
	mFrame.authorityFound = decision.authorityFound;
	mFrame.identityCurrent = decision.identityCurrent;
	mFrame.live = decision.live;
	mFrame.pendingRemoval = decision.pendingRemoval;
	mFrame.actorPositionSynchronized = decision.actorPositionSynchronized;
	mFrame.spatialEvidenceComplete = decision.spatialEvidenceComplete;
	mFrame.ownerSectorReachedBy360 = decision.ownerSectorReachedBy360;
	mFrame.ownerChunkNegative = decision.ownerChunkNegative;
	mFrame.boundsOverlapConflict = decision.boundsOverlapConflict;
	mFrame.actorPositionInsideConflict = decision.actorPositionInsideConflict;
	mFrame.suppressionAuthorized = decision.suppress;
	mFrame.suppressedWorkloadMask = decision.suppressedWorkloadMask;
	mFrame.identityKey = decision.identityKey;
	mFrame.lifecycleGeneration = decision.lifecycleGeneration;
	mFrame.physicalSectorIndex = decision.physicalSectorIndex;
	mFrame.physicalChunkIndex = decision.physicalChunkIndex;
	mFrame.physicalLocalSpaceIndex = decision.physicalLocalSpaceIndex;
	mFrame.rootSectorIndex = decision.rootSectorIndex;
	mFrame.rootLocalSpaceIndex = decision.rootLocalSpaceIndex;
	mFrame.conflictPositiveChunk = decision.conflictPositiveChunk;
	mFrame.conflictNegativeChunk = decision.conflictNegativeChunk;
	std::memcpy(mFrame.actorScenePosition, decision.actorScenePosition, sizeof(mFrame.actorScenePosition));
}

void NRIActorOccurrenceCensus::RecordOccurrence(const NRIActorOccurrence& occurrence)
{
	if (!Targets(occurrence.actorIndex))
	{
		return;
	}
	mFrame.occurrences.push_back(occurrence);
	if (OwnerStampValid(occurrence.ownerWorldEpoch, occurrence.ownerLifetimeGeneration) &&
		(!OwnerStampValid(mFrame.ownerWorldEpoch, mFrame.ownerLifetimeGeneration) ||
		 occurrence.identityKey == mFrame.identityKey))
	{
		mFrame.ownerWorldEpoch = occurrence.ownerWorldEpoch;
		mFrame.ownerLifetimeGeneration = occurrence.ownerLifetimeGeneration;
		mFrame.placementGeneration = occurrence.placementGeneration;
		mFrame.placementStateHash = occurrence.placementStateHash;
		mFrame.bindingGeneration = occurrence.bindingGeneration;
		mFrame.publicationHash = occurrence.publicationHash;
		mFrame.physicalSectorIndex = occurrence.physicalSectorIndex;
		mFrame.authorityCurrent = occurrence.authorityCurrent;
		mFrame.publicationEligible = occurrence.publicationEligible;
		mFrame.pendingRemoval = occurrence.pendingRemoval;
		mFrame.ledgerReason = occurrence.ledgerReason;
	}
	if (occurrence.role == NRIActorOccurrenceRole::Exact && mConfig.spatialSnapshot != nullptr)
	{
		for (const NRISpatialAbsenceConflictRecord& conflict : mConfig.spatialSnapshot->conflicts)
		{
			if (conflict.decision == NRISpatialAbsenceConflictDecision::Certified &&
				conflict.negativeChunk == mFrame.physicalChunkIndex &&
				conflict.positiveSector == mFrame.rootSectorIndex &&
				BoundsOverlap(occurrence, conflict))
			{
				mFrame.boundsOverlapConflict = true;
				mFrame.completeBoundsInsideConflict = BoundsInside(occurrence, conflict);
				mFrame.actorPositionInsideConflict = PointInside(mFrame.actorScenePosition, conflict);
				break;
			}
		}
	}
}

NRIActorOccurrenceFrame NRIActorOccurrenceCensus::FinishPersistent()
{
	if (mFrame.enabled && mFrame.candidates.empty())
	{
		mFrame.invariantFlags |= NRI_ACTOR_OCCURRENCE_INVARIANT_MISSING_AUTHORITY;
	}
	return mFrame;
}

void AppendNRIActorDynamicOccurrences(
	NRIActorOccurrenceFrame& frame,
	const nri_scene::GeometryData* dynamicGeometry,
	uint32_t tlasInstanceIndex,
	uint32_t workloadMask)
{
	if (!frame.enabled || dynamicGeometry == nullptr)
	{
		return;
	}
	const size_t count = std::min(dynamicGeometry->primitives.size(), dynamicGeometry->primitiveProvenance.size());
	bool matched = false;
	float boundsMin[3] = {
		std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()
	};
	float boundsMax[3] = {
		-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()
	};
	for (size_t primitiveIndex = 0; primitiveIndex < count; ++primitiveIndex)
	{
		if (dynamicGeometry->primitiveProvenance[primitiveIndex].actorIndex != frame.targetActorIndex)
		{
			continue;
		}
		matched = true;
		for (uint32_t corner = 0; corner < 3u; ++corner)
		{
			const uint32_t vertexIndex = dynamicGeometry->primitives[primitiveIndex].indices[corner];
			if (vertexIndex >= dynamicGeometry->vertices.size())
			{
				continue;
			}
			for (uint32_t axis = 0; axis < 3u; ++axis)
			{
				const float value = dynamicGeometry->vertices[vertexIndex].position[axis];
				boundsMin[axis] = std::min(boundsMin[axis], value);
				boundsMax[axis] = std::max(boundsMax[axis], value);
			}
		}
	}
	if (!matched)
	{
		return;
	}
	NRIActorOccurrence occurrence;
	occurrence.role = NRIActorOccurrenceRole::DynamicAggregate;
	occurrence.identityKey = frame.identityKey;
	occurrence.lifecycleGeneration = frame.lifecycleGeneration;
	occurrence.ownerWorldEpoch = frame.ownerWorldEpoch;
	occurrence.ownerLifetimeGeneration = frame.ownerLifetimeGeneration;
	occurrence.placementGeneration = frame.placementGeneration;
	occurrence.placementStateHash = frame.placementStateHash;
	occurrence.bindingGeneration = frame.bindingGeneration;
	occurrence.publicationHash = frame.publicationHash;
	occurrence.actorIndex = frame.targetActorIndex;
	occurrence.physicalSectorIndex = frame.physicalSectorIndex;
	occurrence.tlasInstanceIndex = tlasInstanceIndex;
	occurrence.workloadMask = workloadMask;
	occurrence.authorityCurrent = frame.authorityCurrent;
	occurrence.publicationEligible = frame.publicationEligible;
	occurrence.pendingRemoval = frame.pendingRemoval;
	occurrence.ledgerReason = frame.ledgerReason;
	occurrence.boundsValid = tlasInstanceIndex != UINT32_MAX;
	std::memcpy(occurrence.boundsMin, boundsMin, sizeof(boundsMin));
	std::memcpy(occurrence.boundsMax, boundsMax, sizeof(boundsMax));
	frame.occurrences.push_back(occurrence);
}

bool ComputeNRIActorOccurrenceWorldBounds(
	const std::array<float, 12>& transform,
	const float localBoundsMin[3],
	const float localBoundsMax[3],
	float worldBoundsMin[3],
	float worldBoundsMax[3])
{
	for (uint32_t axis = 0; axis < 3u; ++axis)
	{
		worldBoundsMin[axis] = std::numeric_limits<float>::max();
		worldBoundsMax[axis] = -std::numeric_limits<float>::max();
		if (!std::isfinite(localBoundsMin[axis]) || !std::isfinite(localBoundsMax[axis]) ||
			localBoundsMin[axis] > localBoundsMax[axis])
		{
			return false;
		}
	}
	for (uint32_t corner = 0; corner < 8u; ++corner)
	{
		const float local[3] = {
			(corner & 1u) != 0u ? localBoundsMax[0] : localBoundsMin[0],
			(corner & 2u) != 0u ? localBoundsMax[1] : localBoundsMin[1],
			(corner & 4u) != 0u ? localBoundsMax[2] : localBoundsMin[2]
		};
		const float world[3] = {
			transform[0] * local[0] + transform[1] * local[1] + transform[2] * local[2] + transform[3],
			transform[4] * local[0] + transform[5] * local[1] + transform[6] * local[2] + transform[7],
			transform[8] * local[0] + transform[9] * local[1] + transform[10] * local[2] + transform[11]
		};
		for (uint32_t axis = 0; axis < 3u; ++axis)
		{
			if (!std::isfinite(world[axis]))
			{
				return false;
			}
			worldBoundsMin[axis] = std::min(worldBoundsMin[axis], world[axis]);
			worldBoundsMax[axis] = std::max(worldBoundsMax[axis], world[axis]);
		}
	}
	return true;
}

void FinalizeNRIActorOccurrenceFrame(NRIActorOccurrenceFrame& frame, bool worldTlasCommitted)
{
	if (!frame.enabled)
	{
		return;
	}
	frame.worldTlasCommitted = worldTlasCommitted;
	if (!worldTlasCommitted)
	{
		frame.invariantFlags |= NRI_ACTOR_OCCURRENCE_INVARIANT_INCOMPLETE_CENSUS;
	}
	frame.classification = ClassifyFrame(frame);
	frame.finalized = true;
}

const char* GetNRIActorOccurrenceRoleName(NRIActorOccurrenceRole role)
{
	switch (role)
	{
	case NRIActorOccurrenceRole::Exact: return "exact";
	case NRIActorOccurrenceRole::ShadowProxy: return "shadow-proxy";
	case NRIActorOccurrenceRole::DynamicAggregate: return "dynamic-aggregate";
	default: return "unknown";
	}
}

const char* GetNRIActorOccurrenceClassificationName(NRIActorOccurrenceClassification classification)
{
	switch (classification)
	{
	case NRIActorOccurrenceClassification::EvidenceIncomplete: return "evidence-incomplete";
	case NRIActorOccurrenceClassification::StaleLifecycle: return "stale-lifecycle";
	case NRIActorOccurrenceClassification::DuplicateOccurrence: return "duplicate-occurrence";
	case NRIActorOccurrenceClassification::StaleTransform: return "stale-transform";
	case NRIActorOccurrenceClassification::WrongLocalitySingleCurrent: return "wrong-locality-single-current";
	case NRIActorOccurrenceClassification::WrongLocalitySuppressed: return "wrong-locality-suppressed";
	case NRIActorOccurrenceClassification::CurrentLegitimate: return "current-legitimate";
	case NRIActorOccurrenceClassification::MixedInvariantFailure: return "mixed-invariant-failure";
	case NRIActorOccurrenceClassification::RetiredIneligible: return "retired-ineligible";
	default: return "unknown";
	}
}

void TraceNRIActorOccurrenceFrame(const NRIActorOccurrenceFrame& frame)
{
	if (!frame.enabled || !frame.finalized)
	{
		return;
	}
	Printf("NRI PT actor occurrence census: frame=%u target_actor=%d actor_key=0x%llx lifecycle=%llu authority=%u live=%u pending_removal=%u position_sync=%u physical_sector=%d physical_chunk=%u physical_local_space=%d root_sector=%d root_local_space=%d owner_reached=%u owner_negative=%u conflict_positive=%u conflict_negative=%u bounds_inside=%u bounds_overlap=%u actor_inside=%u candidates=%u occurrences=%u committed=%u invariant=0x%x classification=%s owner_world_epoch=%llu owner_lifetime=%llu placement_generation=%llu placement_state_hash=0x%llx binding_generation=%llu publication_hash=0x%llx authority_current=%u publication_eligible=%u ledger_reason=%s\n",
		frame.frameIndex, frame.targetActorIndex,
		(unsigned long long)frame.identityKey, (unsigned long long)frame.lifecycleGeneration,
		frame.authorityFound ? 1u : 0u, frame.live ? 1u : 0u,
		frame.pendingRemoval ? 1u : 0u, frame.actorPositionSynchronized ? 1u : 0u,
		frame.physicalSectorIndex, frame.physicalChunkIndex, frame.physicalLocalSpaceIndex,
		frame.rootSectorIndex, frame.rootLocalSpaceIndex,
		frame.ownerSectorReachedBy360 ? 1u : 0u, frame.ownerChunkNegative ? 1u : 0u,
		frame.conflictPositiveChunk, frame.conflictNegativeChunk,
		frame.completeBoundsInsideConflict ? 1u : 0u,
		frame.boundsOverlapConflict ? 1u : 0u,
		frame.actorPositionInsideConflict ? 1u : 0u,
		(uint32_t)frame.candidates.size(), (uint32_t)frame.occurrences.size(),
		frame.worldTlasCommitted ? 1u : 0u, frame.invariantFlags,
		GetNRIActorOccurrenceClassificationName(frame.classification),
		(unsigned long long)frame.ownerWorldEpoch,
		(unsigned long long)frame.ownerLifetimeGeneration,
		(unsigned long long)frame.placementGeneration,
		(unsigned long long)frame.placementStateHash,
		(unsigned long long)frame.bindingGeneration,
		(unsigned long long)frame.publicationHash,
		frame.authorityCurrent ? 1u : 0u,
		frame.publicationEligible ? 1u : 0u,
		GetNRIActorOccurrenceLedgerReasonName(frame.ledgerReason));
	for (const NRIActorOccurrenceCandidate& candidate : frame.candidates)
	{
		Printf("NRI PT actor occurrence candidate: frame=%u actor=%d actor_key=0x%llx lifecycle=%llu mesh_resource=0x%llx mesh_key=0x%llx material_key=0x%llx captured=%u active=%u admitted=%u publish_ready=%u suppression_authorized=%u suppressed_mask=0x%x reason=%s owner_world_epoch=%llu owner_lifetime=%llu placement_generation=%llu placement_state_hash=0x%llx binding_generation=%llu publication_hash=0x%llx physical_sector=%d authority_current=%u publication_eligible=%u pending_removal=%u ledger_reason=%s\n",
			frame.frameIndex, candidate.actorIndex,
			(unsigned long long)candidate.identityKey,
			(unsigned long long)candidate.lifecycleGeneration,
			(unsigned long long)candidate.meshResourceKey,
			(unsigned long long)candidate.meshKeyHash,
			(unsigned long long)candidate.materialKeyHash,
			candidate.capturedThisFrame ? 1u : 0u, candidate.active ? 1u : 0u,
			candidate.admitted ? 1u : 0u, candidate.publishReady ? 1u : 0u,
			candidate.suppressionAuthorized ? 1u : 0u,
			candidate.suppressedWorkloadMask, candidate.reason.c_str(),
			(unsigned long long)candidate.ownerWorldEpoch,
			(unsigned long long)candidate.ownerLifetimeGeneration,
			(unsigned long long)candidate.placementGeneration,
			(unsigned long long)candidate.placementStateHash,
			(unsigned long long)candidate.bindingGeneration,
			(unsigned long long)candidate.publicationHash,
			candidate.physicalSectorIndex,
			candidate.authorityCurrent ? 1u : 0u,
			candidate.publicationEligible ? 1u : 0u,
			candidate.pendingRemoval ? 1u : 0u,
			GetNRIActorOccurrenceLedgerReasonName(candidate.ledgerReason));
	}
	for (const NRIActorOccurrence& occurrence : frame.occurrences)
	{
		Printf("NRI PT actor occurrence: frame=%u actor=%d actor_key=0x%llx lifecycle=%llu role=%s mesh_resource=0x%llx mesh_key=0x%llx blas=0x%llx instance=%u generation=0x%x expected_generation=0x%x mask=0x%x captured=%u bounds_valid=%u transform=(%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f) bounds_min=(%.6f,%.6f,%.6f) bounds_max=(%.6f,%.6f,%.6f) owner_world_epoch=%llu owner_lifetime=%llu placement_generation=%llu placement_state_hash=0x%llx binding_generation=%llu publication_hash=0x%llx physical_sector=%d authority_current=%u publication_eligible=%u pending_removal=%u ledger_reason=%s\n",
			frame.frameIndex, occurrence.actorIndex,
			(unsigned long long)occurrence.identityKey,
			(unsigned long long)occurrence.lifecycleGeneration,
			GetNRIActorOccurrenceRoleName(occurrence.role),
			(unsigned long long)occurrence.meshResourceKey,
			(unsigned long long)occurrence.meshKeyHash,
			(unsigned long long)occurrence.blasHandle,
			occurrence.tlasInstanceIndex, occurrence.occurrenceGeneration,
			occurrence.expectedOccurrenceGeneration, occurrence.workloadMask,
			occurrence.capturedThisFrame ? 1u : 0u, occurrence.boundsValid ? 1u : 0u,
			occurrence.transform[0], occurrence.transform[1], occurrence.transform[2], occurrence.transform[3],
			occurrence.transform[4], occurrence.transform[5], occurrence.transform[6], occurrence.transform[7],
			occurrence.transform[8], occurrence.transform[9], occurrence.transform[10], occurrence.transform[11],
			occurrence.boundsMin[0], occurrence.boundsMin[1], occurrence.boundsMin[2],
			occurrence.boundsMax[0], occurrence.boundsMax[1], occurrence.boundsMax[2],
			(unsigned long long)occurrence.ownerWorldEpoch,
			(unsigned long long)occurrence.ownerLifetimeGeneration,
			(unsigned long long)occurrence.placementGeneration,
			(unsigned long long)occurrence.placementStateHash,
			(unsigned long long)occurrence.bindingGeneration,
			(unsigned long long)occurrence.publicationHash,
			occurrence.physicalSectorIndex,
			occurrence.authorityCurrent ? 1u : 0u,
			occurrence.publicationEligible ? 1u : 0u,
			occurrence.pendingRemoval ? 1u : 0u,
			GetNRIActorOccurrenceLedgerReasonName(occurrence.ledgerReason));
	}
}

bool RunNRIActorOccurrenceSelfTests(std::string* failureReason)
{
	auto fail = [&](const char* reason)
	{
		if (failureReason != nullptr) *failureReason = reason;
		return false;
	};
	auto baseFrame = []()
	{
		NRIActorOccurrenceFrame frame;
		frame.enabled = true;
		frame.authorityFound = true;
		frame.identityCurrent = true;
		frame.live = true;
		frame.actorPositionSynchronized = true;
		frame.identityKey = 11;
		frame.lifecycleGeneration = 7;
		frame.ownerWorldEpoch = 3;
		frame.ownerLifetimeGeneration = 7;
		frame.placementGeneration = 12;
		frame.placementStateHash = 0x1234;
		frame.bindingGeneration = 0x5678;
		frame.publicationHash = 0x9abc;
		frame.targetActorIndex = 7;
		frame.physicalSectorIndex = 2;
		frame.physicalChunkIndex = 2;
		frame.physicalLocalSpaceIndex = 0;
		frame.worldTlasCommitted = true;
		NRIActorOccurrence occurrence;
		occurrence.identityKey = 11;
		occurrence.lifecycleGeneration = 7;
		occurrence.ownerWorldEpoch = frame.ownerWorldEpoch;
		occurrence.ownerLifetimeGeneration = frame.ownerLifetimeGeneration;
		occurrence.placementGeneration = frame.placementGeneration;
		occurrence.placementStateHash = frame.placementStateHash;
		occurrence.bindingGeneration = frame.bindingGeneration;
		occurrence.publicationHash = frame.publicationHash;
		occurrence.actorIndex = 7;
		occurrence.physicalSectorIndex = frame.physicalSectorIndex;
		occurrence.tlasInstanceIndex = 3;
		occurrence.occurrenceGeneration = 5;
		occurrence.expectedOccurrenceGeneration = 5;
		occurrence.workloadMask = 1;
		occurrence.boundsValid = true;
		frame.occurrences.push_back(occurrence);
		return frame;
	};

	NRIActorOccurrenceFrame current = baseFrame();
	current.classification = ClassifyFrame(current);
	if (current.classification != NRIActorOccurrenceClassification::CurrentLegitimate)
		return fail("current occurrence did not classify legitimate");

	NRIActorOccurrenceFrame zeroLifetime = baseFrame();
	zeroLifetime.lifecycleGeneration = 0;
	zeroLifetime.ownerLifetimeGeneration = 0;
	zeroLifetime.occurrences.front().lifecycleGeneration = 0;
	zeroLifetime.occurrences.front().ownerLifetimeGeneration = 0;
	zeroLifetime.classification = ClassifyFrame(zeroLifetime);
	if (zeroLifetime.classification != NRIActorOccurrenceClassification::CurrentLegitimate)
		return fail("valid lifetime generation zero was rejected");

	NRIActorOccurrenceFrame duplicate = baseFrame();
	duplicate.occurrences.push_back(duplicate.occurrences.front());
	duplicate.classification = ClassifyFrame(duplicate);
	if (duplicate.classification != NRIActorOccurrenceClassification::DuplicateOccurrence)
		return fail("duplicate occurrence was not detected");

	NRIActorOccurrenceFrame stale = baseFrame();
	stale.occurrences.front().occurrenceGeneration = 4;
	stale.classification = ClassifyFrame(stale);
	if (stale.classification != NRIActorOccurrenceClassification::StaleTransform)
		return fail("stale binding generation was not detected");

	NRIActorOccurrenceFrame ownerAba = baseFrame();
	ownerAba.occurrences.front().ownerWorldEpoch--;
	ownerAba.classification = ClassifyFrame(ownerAba);
	if (ownerAba.classification != NRIActorOccurrenceClassification::StaleLifecycle)
		return fail("old-world owner ABA was not detected");

	NRIActorOccurrenceFrame placementMismatch = baseFrame();
	placementMismatch.occurrences.front().placementGeneration--;
	placementMismatch.classification = ClassifyFrame(placementMismatch);
	if (placementMismatch.classification != NRIActorOccurrenceClassification::StaleTransform)
		return fail("old placement generation was not detected");

	NRIActorOccurrenceFrame incomplete = baseFrame();
	incomplete.identityKey = 0;
	incomplete.classification = ClassifyFrame(incomplete);
	if (incomplete.classification != NRIActorOccurrenceClassification::EvidenceIncomplete)
		return fail("missing identity did not fail open");

	NRIActorOccurrenceFrame legalProxy = baseFrame();
	legalProxy.occurrences.front().workloadMask = 0x7;
	NRIActorOccurrence proxy = legalProxy.occurrences.front();
	proxy.role = NRIActorOccurrenceRole::ShadowProxy;
	proxy.workloadMask = 0x8;
	legalProxy.occurrences.push_back(proxy);
	legalProxy.classification = ClassifyFrame(legalProxy);
	if (legalProxy.classification != NRIActorOccurrenceClassification::CurrentLegitimate)
		return fail("disjoint shadow proxy was treated as duplicate");

	NRIActorOccurrenceFrame wrongLocality = baseFrame();
	wrongLocality.spatialEvidenceComplete = true;
	wrongLocality.ownerChunkNegative = true;
	wrongLocality.ownerSectorReachedBy360 = false;
	wrongLocality.boundsOverlapConflict = true;
	wrongLocality.classification = ClassifyFrame(wrongLocality);
	if (wrongLocality.classification != NRIActorOccurrenceClassification::WrongLocalitySingleCurrent)
		return fail("single current negative-locality occurrence was not classified");

	NRIActorOccurrenceFrame mixed = wrongLocality;
	mixed.occurrences.push_back(mixed.occurrences.front());
	mixed.classification = ClassifyFrame(mixed);
	if (mixed.classification != NRIActorOccurrenceClassification::MixedInvariantFailure)
		return fail("mixed duplicate and locality failure was not classified");

	NRIActorOccurrenceFrame suppressed = baseFrame();
	suppressed.occurrences.clear();
	suppressed.spatialEvidenceComplete = true;
	suppressed.ownerChunkNegative = true;
	suppressed.ownerSectorReachedBy360 = false;
	suppressed.boundsOverlapConflict = true;
	suppressed.actorPositionInsideConflict = true;
	suppressed.suppressionAuthorized = true;
	suppressed.suppressedWorkloadMask = 0xffu;
	NRIActorOccurrenceCandidate suppressedCandidate;
	suppressedCandidate.identityKey = suppressed.identityKey;
	suppressedCandidate.lifecycleGeneration = suppressed.lifecycleGeneration;
	suppressedCandidate.actorIndex = suppressed.targetActorIndex;
	suppressedCandidate.active = true;
	suppressedCandidate.publishReady = true;
	suppressedCandidate.suppressionAuthorized = true;
	suppressedCandidate.suppressedWorkloadMask = 0xffu;
	suppressedCandidate.reason = "certified-negative-whole-occurrence";
	suppressed.candidates.push_back(suppressedCandidate);
	suppressed.classification = ClassifyFrame(suppressed);
	if (suppressed.classification != NRIActorOccurrenceClassification::WrongLocalitySuppressed)
		return fail("authorized whole occurrence suppression was not classified");

	NRIActorOccurrenceFrame unauthorized = suppressed;
	unauthorized.suppressionAuthorized = false;
	unauthorized.candidates.front().suppressionAuthorized = false;
	unauthorized.classification = ClassifyFrame(unauthorized);
	if (unauthorized.classification != NRIActorOccurrenceClassification::EvidenceIncomplete)
		return fail("occurrence disappearance without policy proof did not fail open");

	auto retirementFrame = [&baseFrame](NRIActorOccurrenceLedgerReason reason)
	{
		NRIActorOccurrenceFrame frame = baseFrame();
		frame.occurrences.clear();
		frame.authorityCurrent = reason != NRIActorOccurrenceLedgerReason::StaleAuthority;
		frame.publicationEligible = false;
		frame.pendingRemoval = reason == NRIActorOccurrenceLedgerReason::PendingRemoval;
		frame.ledgerReason = reason;
		NRIActorOccurrenceCandidate candidate;
		candidate.identityKey = frame.identityKey;
		candidate.lifecycleGeneration = frame.lifecycleGeneration;
		candidate.ownerWorldEpoch = frame.ownerWorldEpoch;
		candidate.ownerLifetimeGeneration = frame.ownerLifetimeGeneration;
		candidate.placementGeneration = frame.placementGeneration;
		candidate.placementStateHash = frame.placementStateHash;
		candidate.bindingGeneration = frame.bindingGeneration;
		candidate.actorIndex = frame.targetActorIndex;
		candidate.physicalSectorIndex = frame.physicalSectorIndex;
		candidate.active = true;
		candidate.publishReady = false;
		candidate.authorityCurrent = frame.authorityCurrent;
		candidate.publicationEligible = false;
		candidate.pendingRemoval = frame.pendingRemoval;
		candidate.ledgerReason = reason;
		candidate.reason = GetNRIActorOccurrenceLedgerReasonName(reason);
		frame.candidates.push_back(candidate);
		frame.classification = ClassifyFrame(frame);
		return frame;
	};
	if (retirementFrame(NRIActorOccurrenceLedgerReason::PendingRemoval).classification !=
		NRIActorOccurrenceClassification::RetiredIneligible)
		return fail("pending-removal whole-role retirement was not accepted");
	if (retirementFrame(NRIActorOccurrenceLedgerReason::PublicationIneligible).classification !=
		NRIActorOccurrenceClassification::RetiredIneligible)
		return fail("publication-ineligible whole-role retirement was not accepted");

	std::string policyFailure;
	if (!RunNRIActorOccurrencePolicySelfTests(&policyFailure))
	{
		if (failureReason != nullptr) *failureReason = "policy: " + policyFailure;
		return false;
	}

	std::string ledgerFailure;
	if (!RunNRIActorOccurrenceLedgerSelfTests(&ledgerFailure))
	{
		if (failureReason != nullptr) *failureReason = "ledger: " + ledgerFailure;
		return false;
	}

	auto publicationFacts = []()
	{
		NRIActorOccurrencePublicationFacts facts;
		facts.owner = { 5u, 19u };
		facts.placementGeneration = 8u;
		facts.placementStateHash = 0x1111u;
		facts.bindingGeneration = 0x2222u;
		facts.physicalSectorIndex = 7;
		facts.roleMask = NRI_ACTOR_OCCURRENCE_ROLE_EXACT |
			NRI_ACTOR_OCCURRENCE_ROLE_SHADOW_PROXY |
			NRI_ACTOR_OCCURRENCE_ROLE_EMISSIVE_SURFACE |
			NRI_ACTOR_OCCURRENCE_ROLE_LIGHT_ANCHOR;
		facts.exactWorkloadMask = 0x7fu;
		facts.proxyWorkloadMask = 0x80u;
		facts.exactBlasHandle = 0x3333u;
		facts.proxyBlasHandle = 0x4444u;
		facts.authorityCurrent = true;
		facts.publicationEligible = true;
		return facts;
	};
	NRIActorOccurrenceLedger ledger;
	NRIActorOccurrencePublicationFacts oldOwner = publicationFacts();
	ledger.BeginFrame(20u);
	const NRIActorOccurrenceLedgerDecision oldOwnerDecision = ledger.CommitPublication(oldOwner);
	ledger.CommitFrame(20u);
	NRIActorOccurrencePublicationFacts newOwner = oldOwner;
	newOwner.owner.worldEpoch++;
	ledger.BeginFrame(21u);
	const NRIActorOccurrenceLedgerDecision newOwnerDecision = ledger.CommitPublication(newOwner);
	ledger.CommitFrame(21u);
	if (!newOwnerDecision.eligible || ledger.IsCommitted(
		21u, oldOwner.owner, oldOwner.placementGeneration, oldOwner.bindingGeneration,
		oldOwnerDecision.publicationHash, NRI_ACTOR_OCCURRENCE_ROLE_EXACT))
		return fail("old owner remained committed after world-epoch ABA");

	NRIActorOccurrencePublicationFacts movedOwner = newOwner;
	movedOwner.placementGeneration++;
	movedOwner.placementStateHash++;
	ledger.BeginFrame(22u);
	const NRIActorOccurrenceLedgerDecision movedDecision = ledger.CommitPublication(movedOwner);
	ledger.CommitFrame(22u);
	if (!movedDecision.eligible || ledger.IsCommitted(
		22u, newOwner.owner, newOwner.placementGeneration, newOwner.bindingGeneration,
		newOwnerDecision.publicationHash, NRI_ACTOR_OCCURRENCE_ROLE_EXACT))
		return fail("old placement remained committed after authoritative movement");

	for (NRIActorOccurrenceLedgerReason reason : {
		NRIActorOccurrenceLedgerReason::PendingRemoval,
		NRIActorOccurrenceLedgerReason::PublicationIneligible })
	{
		NRIActorOccurrencePublicationFacts retired = movedOwner;
		retired.pendingRemoval = reason == NRIActorOccurrenceLedgerReason::PendingRemoval;
		retired.publicationEligible = reason != NRIActorOccurrenceLedgerReason::PublicationIneligible;
		ledger.BeginFrame(reason == NRIActorOccurrenceLedgerReason::PendingRemoval ? 23u : 24u);
		const NRIActorOccurrenceLedgerDecision retiredDecision = ledger.CommitPublication(retired);
		const uint32_t retiredFrame = reason == NRIActorOccurrenceLedgerReason::PendingRemoval ? 23u : 24u;
		ledger.CommitFrame(retiredFrame);
		if (retiredDecision.eligible || retiredDecision.reason != reason || ledger.IsCommitted(
			retiredFrame, retired.owner, retired.placementGeneration, retired.bindingGeneration,
			movedDecision.publicationHash,
			NRI_ACTOR_OCCURRENCE_ROLE_EXACT | NRI_ACTOR_OCCURRENCE_ROLE_SHADOW_PROXY |
				NRI_ACTOR_OCCURRENCE_ROLE_EMISSIVE_SURFACE |
				NRI_ACTOR_OCCURRENCE_ROLE_LIGHT_ANCHOR))
			return fail("ineligible owner retained an actor-derived publication role");
	}

	return true;
}
