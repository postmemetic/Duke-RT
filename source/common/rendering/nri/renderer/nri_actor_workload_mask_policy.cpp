#include "nri_actor_workload_mask_policy.h"

#include "nri_tlas_masks.h"

NRIActorWorkloadMaskDecision EvaluateNRIActorWorkloadMaskPolicy(
	const NRIActorWorkloadMaskFacts& facts)
{
	NRIActorWorkloadMaskDecision decision = {};
	decision.requestedMask = facts.requestedMask;
	decision.publishedMask = facts.requestedMask;

	auto reject = [&](NRIActorWorkloadMaskReason reason)
	{
		decision.reason = reason;
		return decision;
	};

	switch (facts.occurrenceKind)
	{
	case NRIActorWorkloadOccurrenceKind::DedicatedPersistentActor:
		break;
	case NRIActorWorkloadOccurrenceKind::DynamicAggregate:
		return reject(NRIActorWorkloadMaskReason::DynamicAggregate);
	case NRIActorWorkloadOccurrenceKind::MixedAggregate:
		return reject(NRIActorWorkloadMaskReason::MixedAggregate);
	default:
		return reject(NRIActorWorkloadMaskReason::AmbiguousOccurrence);
	}

	if (facts.actorIndex < 0)
	{
		return reject(NRIActorWorkloadMaskReason::MissingActor);
	}
	if (!facts.finalActorNoShadowCastIntent)
	{
		return reject(NRIActorWorkloadMaskReason::MissingFinalNoShadowIntent);
	}
	if (facts.materialSignature == 0 ||
		facts.expectedMaterialSignature == 0 ||
		facts.materialSignature != facts.expectedMaterialSignature ||
		facts.materialClosureGeneration == 0 ||
		facts.expectedMaterialClosureGeneration == 0 ||
		facts.materialClosureGeneration != facts.expectedMaterialClosureGeneration)
	{
		return reject(NRIActorWorkloadMaskReason::StaleMaterialClosure);
	}
	if (facts.materialSlotGeneration == 0 ||
		facts.expectedMaterialSlotGeneration == 0 ||
		facts.materialSlotGeneration != facts.expectedMaterialSlotGeneration)
	{
		return reject(NRIActorWorkloadMaskReason::StaleMaterialSlot);
	}
	if (facts.bindingGeneration == 0 ||
		facts.expectedBindingGeneration == 0 ||
		facts.bindingGeneration != facts.expectedBindingGeneration)
	{
		return reject(NRIActorWorkloadMaskReason::StaleBinding);
	}
	// Palette policy remains authoritative even when all of its rows happen to
	// select identical flags. It is not an occurrence-wide visibility contract.
	if (facts.anyVoxelPalettePolicyApplied)
	{
		return reject(NRIActorWorkloadMaskReason::VoxelPalettePolicy);
	}
	if (facts.effectiveMaterialRowSpan != 1u ||
		facts.declaredMaterialRowCount != 1u ||
		facts.materialRowCount != 1u ||
		facts.metadataRowCount != 1u)
	{
		return reject(NRIActorWorkloadMaskReason::NonUniformMaterialTopology);
	}
	if (!facts.singleActorProvenance)
	{
		return reject(NRIActorWorkloadMaskReason::MixedActorProvenance);
	}
	if (!facts.allRowsFinalNoShadowCast)
	{
		return reject(NRIActorWorkloadMaskReason::FinalNoShadowCastMissing);
	}

	constexpr uint32_t CandidateRemovalMask = NRI_TLAS_MASK_SHADOW | NRI_TLAS_MASK_GI;
	decision.reason = NRIActorWorkloadMaskReason::Certified;
	decision.publishedMask = facts.requestedMask & ~CandidateRemovalMask;
	decision.removedMask = facts.requestedMask & CandidateRemovalMask;
	decision.certified = true;
	return decision;
}

const char* GetNRIActorWorkloadMaskReasonName(NRIActorWorkloadMaskReason reason)
{
	switch (reason)
	{
	case NRIActorWorkloadMaskReason::AmbiguousOccurrence: return "ambiguous-occurrence";
	case NRIActorWorkloadMaskReason::DynamicAggregate: return "dynamic-aggregate";
	case NRIActorWorkloadMaskReason::MixedAggregate: return "mixed-aggregate";
	case NRIActorWorkloadMaskReason::MissingActor: return "missing-actor";
	case NRIActorWorkloadMaskReason::MissingFinalNoShadowIntent: return "missing-final-no-shadow-intent";
	case NRIActorWorkloadMaskReason::StaleMaterialClosure: return "stale-material-closure";
	case NRIActorWorkloadMaskReason::StaleMaterialSlot: return "stale-material-slot";
	case NRIActorWorkloadMaskReason::StaleBinding: return "stale-binding";
	case NRIActorWorkloadMaskReason::VoxelPalettePolicy: return "voxel-palette-policy";
	case NRIActorWorkloadMaskReason::NonUniformMaterialTopology: return "non-uniform-material-topology";
	case NRIActorWorkloadMaskReason::MixedActorProvenance: return "mixed-actor-provenance";
	case NRIActorWorkloadMaskReason::FinalNoShadowCastMissing: return "final-no-shadow-cast-missing";
	case NRIActorWorkloadMaskReason::Certified: return "certified";
	default: return "unknown";
	}
}
