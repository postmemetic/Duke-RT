#include "nri_static_material_slices.h"

#include <algorithm>
#include <cstring>

namespace nri_static_material_slices
{
namespace
{
	template <typename T> bool BitsEqual(const T& lhs, const T& rhs)
	{
		return std::memcmp(&lhs, &rhs, sizeof(T)) == 0;
	}

	bool TextureEqual(const nri_scene::TextureUpload& lhs, const nri_scene::TextureUpload& rhs)
	{
		return lhs.key == rhs.key && lhs.width == rhs.width && lhs.height == rhs.height &&
			lhs.mipCount == rhs.mipCount && lhs.indexed == rhs.indexed &&
			lhs.sourceTexture == rhs.sourceTexture && lhs.pixels == rhs.pixels;
	}

	bool MetadataEqual(const nri_scene::MaterialLightingMetadata& lhs, const nri_scene::MaterialLightingMetadata& rhs)
	{
		// Compare all fields, including float bits, without comparing struct padding.
#define SAME_FIELD(name) if (!BitsEqual(lhs.name, rhs.name)) return false
		SAME_FIELD(texture); SAME_FIELD(materialKey); SAME_FIELD(textureContentKey);
		SAME_FIELD(glowmapContentKey); SAME_FIELD(normalContentKey); SAME_FIELD(metallicContentKey);
		SAME_FIELD(roughnessContentKey); SAME_FIELD(textureId); SAME_FIELD(baseTextureId);
		SAME_FIELD(textureIndex); SAME_FIELD(glowmapTextureIndex); SAME_FIELD(normalTextureIndex);
		SAME_FIELD(metallicTextureIndex); SAME_FIELD(roughnessTextureIndex); SAME_FIELD(emissiveTextureIndex);
		SAME_FIELD(paletteIndex); SAME_FIELD(materialFlags); SAME_FIELD(lightingFlags);
		SAME_FIELD(materialClass); SAME_FIELD(emissiveMode); SAME_FIELD(emissiveStableFrames);
		SAME_FIELD(voxelPaletteIndex); SAME_FIELD(voxelPalettePolicyFlags); SAME_FIELD(voxelPalettePolicyApplied);
		SAME_FIELD(sourceType); SAME_FIELD(sectorIndex); SAME_FIELD(actorIndex);
		SAME_FIELD(actorOverlayRuleCount); SAME_FIELD(actorOverlayRuleIds); SAME_FIELD(shade);
		SAME_FIELD(alpha); SAME_FIELD(lightLevel); SAME_FIELD(averageColor); SAME_FIELD(glowColor);
		SAME_FIELD(emissiveColor); SAME_FIELD(emissiveIntensity); SAME_FIELD(emissiveMaskScale);
		SAME_FIELD(visibleFullbrightBoost);
#undef SAME_FIELD
		return true;
	}

	bool MaterialReferencesEqual(const nri_scene::MaterialData& lhs, const nri_scene::MaterialData& rhs)
	{
		return lhs.textureIndex == rhs.textureIndex && lhs.normalTextureIndex == rhs.normalTextureIndex &&
			lhs.metallicTextureIndex == rhs.metallicTextureIndex && lhs.roughnessTextureIndex == rhs.roughnessTextureIndex &&
			lhs.emissiveTextureIndex == rhs.emissiveTextureIndex;
	}

	bool MetadataReferencesEqual(const nri_scene::MaterialLightingMetadata& lhs, const nri_scene::MaterialLightingMetadata& rhs)
	{
		return lhs.textureIndex == rhs.textureIndex && lhs.glowmapTextureIndex == rhs.glowmapTextureIndex &&
			lhs.normalTextureIndex == rhs.normalTextureIndex && lhs.metallicTextureIndex == rhs.metallicTextureIndex &&
			lhs.roughnessTextureIndex == rhs.roughnessTextureIndex && lhs.emissiveTextureIndex == rhs.emissiveTextureIndex &&
			lhs.sourceType == rhs.sourceType && lhs.sectorIndex == rhs.sectorIndex && lhs.actorIndex == rhs.actorIndex;
	}

	bool TextureTableEqual(const nri_scene::MaterialBridgeData& lhs, const nri_scene::MaterialBridgeData& rhs)
	{
		if (lhs.textures.size() != rhs.textures.size() || lhs.paletteWidth != rhs.paletteWidth ||
			lhs.paletteHeight != rhs.paletteHeight || lhs.paletteLookup != rhs.paletteLookup)
		{
			return false;
		}
		for (size_t index = 0; index < lhs.textures.size(); ++index)
		{
			if (!TextureEqual(lhs.textures[index], rhs.textures[index])) return false;
		}
		return true;
	}
}

void UpdateCandidate(std::vector<uint32_t>& candidates, uint32_t chunkListIndex, bool enabled)
{
	const auto found = std::lower_bound(candidates.begin(), candidates.end(), chunkListIndex);
	if (enabled && (found == candidates.end() || *found != chunkListIndex)) candidates.insert(found, chunkListIndex);
	else if (!enabled && found != candidates.end() && *found == chunkListIndex) candidates.erase(found);
}

bool LayoutCompatible(const nri_scene::MaterialBridgeData& previous, const nri_scene::MaterialBridgeData& current)
{
	if (previous.materials.size() != current.materials.size() || previous.lightMetadata.size() != previous.materials.size() ||
		current.lightMetadata.size() != current.materials.size() || !TextureTableEqual(previous, current))
	{
		return false;
	}
	for (size_t index = 0; index < previous.materials.size(); ++index)
	{
		if (!MaterialReferencesEqual(previous.materials[index], current.materials[index]) ||
			!MetadataReferencesEqual(previous.lightMetadata[index], current.lightMetadata[index])) return false;
	}
	return true;
}

bool BridgeEqual(const nri_scene::MaterialBridgeData& lhs, const nri_scene::MaterialBridgeData& rhs)
{
	if (lhs.materials.size() != rhs.materials.size() || lhs.lightMetadata.size() != rhs.lightMetadata.size() ||
		!TextureTableEqual(lhs, rhs)) return false;
	if (!lhs.materials.empty() && std::memcmp(lhs.materials.data(), rhs.materials.data(),
		lhs.materials.size() * sizeof(nri_scene::MaterialData)) != 0) return false;
	for (size_t index = 0; index < lhs.lightMetadata.size(); ++index)
	{
		if (!MetadataEqual(lhs.lightMetadata[index], rhs.lightMetadata[index])) return false;
	}
	return true;
}

bool Patch(
	const nri_scene::MaterialBridgeData& previous,
	const nri_scene::MaterialBridgeData& current,
	uint32_t offset,
	uint32_t count,
	nri_scene::MaterialBridgeData& destination,
	const std::unordered_map<uint64_t, uint32_t>& textureSlots,
	nri_scene::MaterialBridgeData& scratch)
{
	if (current.materials.size() != count || !LayoutCompatible(previous, current) ||
		offset > destination.materials.size() || count > destination.materials.size() - offset ||
		offset > destination.lightMetadata.size() || count > destination.lightMetadata.size() - offset)
	{
		return false;
	}
	scratch.materials.clear();
	scratch.lightMetadata.clear();
	scratch.materials.reserve(count);
	scratch.lightMetadata.reserve(count);
	const auto remap = [&](uint32_t& index)
	{
		// Preserve AppendMaterialBridge's sentinel and out-of-table behavior.
		if (index == UINT32_MAX || index >= current.textures.size()) return true;
		const auto found = textureSlots.find(current.textures[index].key);
		if (found == textureSlots.end() || found->second >= destination.textures.size() ||
			destination.textures[found->second].key != current.textures[index].key) return false;
		index = found->second;
		return true;
	};
	for (uint32_t index = 0; index < count; ++index)
	{
		auto row = current.materials[index];
		auto metadata = current.lightMetadata[index];
		if (!remap(row.textureIndex) || !remap(row.normalTextureIndex) || !remap(row.metallicTextureIndex) ||
			!remap(row.roughnessTextureIndex) || !remap(row.emissiveTextureIndex) ||
			!remap(metadata.textureIndex) || !remap(metadata.glowmapTextureIndex) || !remap(metadata.normalTextureIndex) ||
			!remap(metadata.metallicTextureIndex) || !remap(metadata.roughnessTextureIndex) || !remap(metadata.emissiveTextureIndex)) return false;
		scratch.materials.push_back(row);
		scratch.lightMetadata.push_back(metadata);
	}
	std::copy(scratch.materials.begin(), scratch.materials.end(), destination.materials.begin() + offset);
	std::copy(scratch.lightMetadata.begin(), scratch.lightMetadata.end(), destination.lightMetadata.begin() + offset);
	return true;
}
}
