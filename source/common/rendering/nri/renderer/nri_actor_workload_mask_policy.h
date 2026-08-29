#pragma once

#include <cstdint>

enum class NRIActorWorkloadOccurrenceKind : uint8_t
{
	Unknown = 0,
	DedicatedPersistentActor,
	DynamicAggregate,
	MixedAggregate,
};

enum class NRIActorWorkloadMaskReason : uint8_t
{
	AmbiguousOccurrence = 0,
	DynamicAggregate,
	MixedAggregate,
	MissingActor,
	MissingFinalNoShadowIntent,
	StaleMaterialClosure,
	StaleMaterialSlot,
	StaleBinding,
	VoxelPalettePolicy,
	NonUniformMaterialTopology,
	MixedActorProvenance,
	FinalNoShadowCastMissing,
	Certified,
	Count,
};

struct NRIActorWorkloadMaskFacts
{
	uint32_t requestedMask = 0;
	NRIActorWorkloadOccurrenceKind occurrenceKind = NRIActorWorkloadOccurrenceKind::Unknown;
	int32_t actorIndex = -1;
	bool finalActorNoShadowCastIntent = false;
	uint64_t materialSignature = 0;
	uint64_t expectedMaterialSignature = 0;
	uint64_t materialClosureGeneration = 0;
	uint64_t expectedMaterialClosureGeneration = 0;
	uint64_t materialSlotGeneration = 0;
	uint64_t expectedMaterialSlotGeneration = 0;
	uint64_t bindingGeneration = 0;
	uint64_t expectedBindingGeneration = 0;
	uint32_t effectiveMaterialRowSpan = 0;
	uint32_t declaredMaterialRowCount = 0;
	uint32_t materialRowCount = 0;
	uint32_t metadataRowCount = 0;
	bool singleActorProvenance = false;
	bool anyVoxelPalettePolicyApplied = false;
	bool allRowsFinalNoShadowCast = false;
};

struct NRIActorWorkloadMaskDecision
{
	NRIActorWorkloadMaskReason reason = NRIActorWorkloadMaskReason::AmbiguousOccurrence;
	uint32_t requestedMask = 0;
	uint32_t publishedMask = 0;
	uint32_t removedMask = 0;
	bool certified = false;
};

NRIActorWorkloadMaskDecision EvaluateNRIActorWorkloadMaskPolicy(
	const NRIActorWorkloadMaskFacts& facts);
const char* GetNRIActorWorkloadMaskReasonName(NRIActorWorkloadMaskReason reason);
