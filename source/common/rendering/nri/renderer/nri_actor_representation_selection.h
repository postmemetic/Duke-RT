#pragma once

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
