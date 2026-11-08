#include "nri_scene_material_texture_slots.h"

#include <algorithm>

namespace
{
uint32_t ResolveRequired(
	const NRISceneTextureSlotTable& slotTable,
	const nri_scene::MaterialBridgeData& materials,
	uint32_t textureIndex,
	NRISceneMaterialTextureSlotResolveStats& stats)
{
	if (textureIndex >= materials.textures.size())
	{
		stats.requiredFallbacks++;
		return 0;
	}
	const NRISceneTextureSlotHandle handle = slotTable.Lookup(materials.textures[textureIndex].key);
	if (!handle)
	{
		stats.requiredFallbacks++;
		return 0;
	}
	return handle.slot;
}

uint32_t ResolveOptional(
	const NRISceneTextureSlotTable& slotTable,
	const nri_scene::MaterialBridgeData& materials,
	uint32_t textureIndex,
	NRISceneMaterialTextureSlotResolveStats& stats)
{
	if (textureIndex == UINT32_MAX)
		return UINT32_MAX;
	if (textureIndex >= materials.textures.size())
	{
		stats.optionalFallbacks++;
		return UINT32_MAX;
	}
	const NRISceneTextureSlotHandle handle = slotTable.Lookup(materials.textures[textureIndex].key);
	if (!handle)
	{
		stats.optionalFallbacks++;
		return UINT32_MAX;
	}
	return handle.slot;
}
}

void NRIResolveSceneMaterialTextureSlots(
	const NRISceneTextureSlotTable& slotTable,
	const nri_scene::MaterialBridgeData& materials,
	std::vector<nri_scene::MaterialData>& gpuMaterials,
	NRISceneMaterialTextureSlotResolveStats* outStats)
{
	NRISceneMaterialTextureSlotResolveStats stats = {};
	for (nri_scene::MaterialData& material : gpuMaterials)
	{
		material.textureIndex = ResolveRequired(slotTable, materials, material.textureIndex, stats);
		material.normalTextureIndex = ResolveOptional(slotTable, materials, material.normalTextureIndex, stats);
		material.metallicTextureIndex = ResolveOptional(slotTable, materials, material.metallicTextureIndex, stats);
		material.roughnessTextureIndex = ResolveOptional(slotTable, materials, material.roughnessTextureIndex, stats);
		material.emissiveTextureIndex = ResolveOptional(slotTable, materials, material.emissiveTextureIndex, stats);
		stats.materialRows++;
	}

	if (outStats != nullptr)
	{
		*outStats = stats;
	}
}

void NRIResolveSceneMaterialBridgeTextureSlots(
	const NRISceneTextureSlotTable& slotTable,
	nri_scene::MaterialBridgeData& materials,
	uint32_t firstMaterialRow,
	NRISceneMaterialTextureSlotResolveStats* outStats)
{
	NRIResolveSceneMaterialBridgeTextureSlotRange(
		slotTable, materials, firstMaterialRow, (uint32_t)materials.materials.size(), outStats);
}

void NRIResolveSceneMaterialBridgeTextureSlotRange(
	const NRISceneTextureSlotTable& slotTable,
	nri_scene::MaterialBridgeData& materials,
	uint32_t firstMaterialRow,
	uint32_t endMaterialRow,
	NRISceneMaterialTextureSlotResolveStats* outStats)
{
	NRISceneMaterialTextureSlotResolveStats stats = {};
	const size_t end = std::min<size_t>(endMaterialRow, materials.materials.size());
	for (uint32_t row = firstMaterialRow; row < end; ++row)
	{
		nri_scene::MaterialData& material = materials.materials[row];
		material.textureIndex = ResolveRequired(slotTable, materials, material.textureIndex, stats);
		material.normalTextureIndex = ResolveOptional(slotTable, materials, material.normalTextureIndex, stats);
		material.metallicTextureIndex = ResolveOptional(slotTable, materials, material.metallicTextureIndex, stats);
		material.roughnessTextureIndex = ResolveOptional(slotTable, materials, material.roughnessTextureIndex, stats);
		material.emissiveTextureIndex = ResolveOptional(slotTable, materials, material.emissiveTextureIndex, stats);
		if (row < materials.lightMetadata.size())
		{
			nri_scene::MaterialLightingMetadata& metadata = materials.lightMetadata[row];
			metadata.textureIndex = ResolveRequired(slotTable, materials, metadata.textureIndex, stats);
			metadata.glowmapTextureIndex = ResolveOptional(slotTable, materials, metadata.glowmapTextureIndex, stats);
			metadata.normalTextureIndex = ResolveOptional(slotTable, materials, metadata.normalTextureIndex, stats);
			metadata.metallicTextureIndex = ResolveOptional(slotTable, materials, metadata.metallicTextureIndex, stats);
			metadata.roughnessTextureIndex = ResolveOptional(slotTable, materials, metadata.roughnessTextureIndex, stats);
			metadata.emissiveTextureIndex = ResolveOptional(slotTable, materials, metadata.emissiveTextureIndex, stats);
		}
		stats.materialRows++;
	}
	if (outStats != nullptr)
		*outStats = stats;
}
