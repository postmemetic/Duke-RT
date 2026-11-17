#pragma once

#include <cstdint>

struct NRIActorExactBindingFacts
{
	bool voxelReady = false;
	bool demandCurrent = false;
	bool instanceCurrent = false;
	bool instancePending = false;
	uint64_t desiredMesh = 0, desiredMaterial = 0;
	uint64_t selectedMesh = 0, selectedMaterial = 0;
	uint64_t actorMaterialSignature = 0, instanceMaterialSignature = 0;
};

constexpr bool IsNRIActorBindingExact(const NRIActorExactBindingFacts& facts)
{
	// Stable enemy material keys survive palette/shade/alpha changes. A held
	// instance keeps its old signature and marks pending, so matching resource
	// keys alone cannot prove that the requested material update was published.
	return facts.voxelReady && facts.demandCurrent && facts.instanceCurrent && !facts.instancePending &&
		facts.selectedMesh == facts.desiredMesh && facts.selectedMaterial == facts.desiredMaterial &&
		facts.actorMaterialSignature == facts.instanceMaterialSignature;
}

enum class NRIActorSelection { Suppressed, ExistingDynamic, Voxel, Sprite, Unavailable };

struct NRIActorSelectionFacts
{
	bool suppressed = false;
	bool existingDynamic = false;
	bool voxelReady = false;
	bool descriptorValid = false;
	bool forceSprite = false;
};

constexpr NRIActorSelection SelectNRIActorRepresentation(NRIActorSelectionFacts facts)
{
	if (facts.suppressed) return NRIActorSelection::Suppressed;
	if (facts.existingDynamic) return NRIActorSelection::ExistingDynamic;
	// Fault injection must not hide a usable representation if the alternate
	// descriptor failed validation or belongs to a different actor lifetime.
	if (facts.voxelReady && (!facts.forceSprite || !facts.descriptorValid)) return NRIActorSelection::Voxel;
	if (facts.descriptorValid) return NRIActorSelection::Sprite;
	return NRIActorSelection::Unavailable;
}
