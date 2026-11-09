#pragma once
#include "nri_scene_material_texture_slots.h"

// Independent original scalar lookup path, retained for opt-in CPU validation.
void NRIResolveSceneMaterialTextureSlotsReference(const NRISceneTextureSlotTable& slotTable,
 const nri_scene::MaterialBridgeData& materials, std::vector<nri_scene::MaterialData>& gpuMaterials,
 NRISceneMaterialTextureSlotResolveStats* outStats = nullptr);
void NRIResolveSceneMaterialBridgeTextureSlotsReference(const NRISceneTextureSlotTable& slotTable,
 nri_scene::MaterialBridgeData& materials, uint32_t firstMaterialRow,
 NRISceneMaterialTextureSlotResolveStats* outStats = nullptr);
void NRIResolveSceneMaterialBridgeTextureSlotRangeReference(const NRISceneTextureSlotTable& slotTable,
 nri_scene::MaterialBridgeData& materials, uint32_t firstMaterialRow, uint32_t endMaterialRow,
 NRISceneMaterialTextureSlotResolveStats* outStats = nullptr);
