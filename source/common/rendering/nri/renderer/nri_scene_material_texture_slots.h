#pragma once

#include "nri_scene_texture_slot_table.h"
#include "../scene/nri_material_bridge.h"

#include <cstdint>
#include <vector>

struct NRISceneMaterialTextureSlotResolveStats
{
	uint32_t materialRows = 0;
	uint64_t keyLookups = 0;
	// Finite source indices may reference a later appended texture suffix.
	uint64_t sourceIndexLimit = 0;
	uint32_t requiredFallbacks = 0;
	uint32_t optionalFallbacks = 0;
};

void NRIResolveSceneMaterialTextureSlots(
	const NRISceneTextureSlotTable& slotTable,
	const nri_scene::MaterialBridgeData& materials,
	std::vector<nri_scene::MaterialData>& gpuMaterials,
	NRISceneMaterialTextureSlotResolveStats* outStats = nullptr);

void NRIResolveSceneMaterialBridgeTextureSlots(
	const NRISceneTextureSlotTable& slotTable,
	nri_scene::MaterialBridgeData& materials,
	uint32_t firstMaterialRow,
	NRISceneMaterialTextureSlotResolveStats* outStats = nullptr);

// Resolves clean bridge indices in [firstMaterialRow, endMaterialRow). Other
// rows may already contain resolved slot indices and must not be visited again.
void NRIResolveSceneMaterialBridgeTextureSlotRange(
	const NRISceneTextureSlotTable& slotTable,
	nri_scene::MaterialBridgeData& materials,
	uint32_t firstMaterialRow,
	uint32_t endMaterialRow,
	NRISceneMaterialTextureSlotResolveStats* outStats = nullptr);

// Owned dense lookup scratch. Prepare checks actual lookup outputs, including
// quarantine/reactivation, rather than trusting a slot revision alone.
class NRISceneMaterialTextureSlotRemap
{
public:
 bool Prepare(const NRISceneTextureSlotTable& slotTable, const nri_scene::MaterialBridgeData& materials);
 uint32_t Required(uint32_t index, NRISceneMaterialTextureSlotResolveStats& stats) const;
 uint32_t Optional(uint32_t index, NRISceneMaterialTextureSlotResolveStats& stats) const;
 size_t FirstChangedIndex() const { return mFirstChangedIndex; }
 size_t CapacityBytes() const { return mSlots.capacity() * sizeof(uint32_t); }
 void Reset() { std::vector<uint32_t>{}.swap(mSlots); mPrepared = false; mFirstChangedIndex = 0; }
private:
 std::vector<uint32_t> mSlots;
 size_t mFirstChangedIndex = 0;
 bool mPrepared = false;
};

// The remap must have been prepared from this exact bridge's texture list.
// Clean ranges only; previously resolved rows must not be resolved twice.
void NRIResolveSceneMaterialBridgeTextureSlotRangeWithRemap(
 const NRISceneMaterialTextureSlotRemap& remap, nri_scene::MaterialBridgeData& materials,
 uint32_t firstMaterialRow, uint32_t endMaterialRow,
 NRISceneMaterialTextureSlotResolveStats* outStats = nullptr);
