#include "nri_material_bridge_append_reference.h"

namespace nri_scene
{
void AppendMaterialBridgeReference(const MaterialBridgeData& source, MaterialBridgeData& destination)
{
	std::unordered_map<uint64_t, uint32_t> textureLookup;
	textureLookup.reserve(destination.textures.size() + source.textures.size());
	for (uint32_t i = 0; i < (uint32_t)destination.textures.size(); ++i)
	{
		textureLookup.emplace(destination.textures[i].key, i);
	}
	AppendMaterialBridgeReference(source, destination, textureLookup);
}

void AppendMaterialBridgeReference(
	const MaterialBridgeData& source,
	MaterialBridgeData& destination,
	std::unordered_map<uint64_t, uint32_t>& textureLookup)
{

	auto remapTextureIndex = [&source, &destination, &textureLookup](uint32_t textureIndex) -> uint32_t
	{
		if (textureIndex == UINT32_MAX)
		{
			return UINT32_MAX;
		}
		if (textureIndex >= source.textures.size())
		{
			return textureIndex;
		}

		const auto& texture = source.textures[textureIndex];
		auto it = textureLookup.find(texture.key);
		if (it == textureLookup.end())
		{
			const uint32_t newIndex = (uint32_t)destination.textures.size();
			textureLookup.emplace(texture.key, newIndex);
			destination.textures.push_back(texture);
			return newIndex;
		}

		return it->second;
	};

	for (size_t materialIndex = 0; materialIndex < source.materials.size(); ++materialIndex)
	{
		const auto& material = source.materials[materialIndex];
		MaterialData copy = material;
		const bool hasLightMetadata = materialIndex < source.lightMetadata.size();
		copy.textureIndex = remapTextureIndex(material.textureIndex);
		copy.normalTextureIndex = remapTextureIndex(material.normalTextureIndex);
		copy.metallicTextureIndex = remapTextureIndex(material.metallicTextureIndex);
		copy.roughnessTextureIndex = remapTextureIndex(material.roughnessTextureIndex);
		copy.emissiveTextureIndex = remapTextureIndex(material.emissiveTextureIndex);

		destination.materials.push_back(copy);
		if (hasLightMetadata)
		{
			MaterialLightingMetadata metadata = source.lightMetadata[materialIndex];
			metadata.textureIndex = remapTextureIndex(metadata.textureIndex);
			metadata.glowmapTextureIndex = remapTextureIndex(metadata.glowmapTextureIndex);
			metadata.normalTextureIndex = remapTextureIndex(metadata.normalTextureIndex);
			metadata.metallicTextureIndex = remapTextureIndex(metadata.metallicTextureIndex);
			metadata.roughnessTextureIndex = remapTextureIndex(metadata.roughnessTextureIndex);
			metadata.emissiveTextureIndex = remapTextureIndex(metadata.emissiveTextureIndex);
			destination.lightMetadata.push_back(metadata);
		}
	}

	if (destination.paletteLookup.empty())
	{
		destination.paletteLookup = source.paletteLookup;
		destination.paletteWidth = source.paletteWidth;
		destination.paletteHeight = source.paletteHeight;
	}
}

}
