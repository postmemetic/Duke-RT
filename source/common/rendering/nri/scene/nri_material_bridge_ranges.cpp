#include "nri_material_bridge_ranges.h"

#include <algorithm>
#include <cstring>
#include <tuple>

namespace nri_scene
{
namespace
{
bool EqualMetadata(const MaterialLightingMetadata& a, const MaterialLightingMetadata& b)
{
	return std::tie(a.texture, a.materialKey, a.textureContentKey, a.glowmapContentKey,
		a.normalContentKey, a.metallicContentKey, a.roughnessContentKey, a.textureId,
		a.baseTextureId, a.authoredTextureId, a.textureIndex, a.glowmapTextureIndex,
		a.normalTextureIndex, a.metallicTextureIndex, a.roughnessTextureIndex,
		a.emissiveTextureIndex, a.paletteIndex, a.materialFlags, a.lightingFlags,
		a.materialClass, a.emissiveMode, a.emissiveStableFrames, a.voxelPaletteIndex,
		a.voxelPalettePolicyFlags, a.voxelPalettePolicyApplied, a.sourceType,
		a.sectorIndex, a.actorIndex, a.actorOverlayRuleCount, a.shade, a.alpha,
		a.lightLevel, a.emissiveIntensity, a.emissiveMaskScale, a.visibleFullbrightBoost) ==
		std::tie(b.texture, b.materialKey, b.textureContentKey, b.glowmapContentKey,
		b.normalContentKey, b.metallicContentKey, b.roughnessContentKey, b.textureId,
		b.baseTextureId, b.authoredTextureId, b.textureIndex, b.glowmapTextureIndex,
		b.normalTextureIndex, b.metallicTextureIndex, b.roughnessTextureIndex,
		b.emissiveTextureIndex, b.paletteIndex, b.materialFlags, b.lightingFlags,
		b.materialClass, b.emissiveMode, b.emissiveStableFrames, b.voxelPaletteIndex,
		b.voxelPalettePolicyFlags, b.voxelPalettePolicyApplied, b.sourceType,
		b.sectorIndex, b.actorIndex, b.actorOverlayRuleCount, b.shade, b.alpha,
		b.lightLevel, b.emissiveIntensity, b.emissiveMaskScale, b.visibleFullbrightBoost) &&
		std::equal(std::begin(a.actorOverlayRuleIds), std::end(a.actorOverlayRuleIds), std::begin(b.actorOverlayRuleIds)) &&
		std::equal(std::begin(a.averageColor), std::end(a.averageColor), std::begin(b.averageColor)) &&
		std::equal(std::begin(a.glowColor), std::end(a.glowColor), std::begin(b.glowColor)) &&
		std::equal(std::begin(a.emissiveColor), std::end(a.emissiveColor), std::begin(b.emissiveColor));
}
}

bool MaterialBridgesEquivalent(const MaterialBridgeData& a, const MaterialBridgeData& b)
{
	if (a.materials.size() != b.materials.size() || a.lightMetadata.size() != b.lightMetadata.size() ||
		a.textures.size() != b.textures.size() || a.paletteWidth != b.paletteWidth || a.paletteHeight != b.paletteHeight ||
		a.paletteLookup != b.paletteLookup) return false;
	if (!a.materials.empty() && std::memcmp(a.materials.data(), b.materials.data(), a.materials.size() * sizeof(MaterialData)) != 0)
		return false;
	for (size_t row = 0; row < a.lightMetadata.size(); ++row)
		if (!EqualMetadata(a.lightMetadata[row], b.lightMetadata[row])) return false;
	for (size_t index = 0; index < a.textures.size(); ++index)
	{
		const auto& x = a.textures[index];
		const auto& y = b.textures[index];
		if (std::tie(x.key, x.width, x.height, x.mipCount, x.indexed, x.sourceTexture) !=
			std::tie(y.key, y.width, y.height, y.mipCount, y.indexed, y.sourceTexture) || x.pixels != y.pixels)
			return false;
	}
	return true;
}

void MergeMaterialRowRanges(std::vector<MaterialRowRange>& ranges, uint32_t rowCount)
{
	std::sort(ranges.begin(), ranges.end(), [](const MaterialRowRange& a, const MaterialRowRange& b) { return a.first < b.first; });
	size_t output = 0;
	for (auto range : ranges)
	{
		if (range.first >= rowCount || range.count == 0) continue;
		range.count = std::min(range.count, rowCount - range.first);
		if (output && range.first <= ranges[output - 1].first + ranges[output - 1].count)
		{
			auto& previous = ranges[output - 1];
			previous.count = std::max(previous.count, range.first + range.count - previous.first);
		}
		else ranges[output++] = range;
	}
	ranges.resize(output);
}

MaterialBridgeTextureLayoutWork CollectMaterialBridgeTextureLayout(
	const MaterialBridgeData& source,
	MaterialBridgeData& destination,
	std::unordered_map<uint64_t, uint32_t>& textureLookup, bool singleTextureFast)
{
	MaterialBridgeTextureLayoutWork work;
	auto visit = [&](uint32_t index)
	{
		if (index >= source.textures.size()) return;
		const auto& texture = source.textures[index];
		if (textureLookup.emplace(texture.key, (uint32_t)destination.textures.size()).second)
			destination.textures.push_back(texture);
	};
	if (singleTextureFast && source.textures.size() == 1 && &source != &destination)
	{
		// There is only one possible namespace entry. Preserve the original row
		// domain and reference rules, then stop after its first reference.
		work.singleTextureFast = true;
		for (size_t row = 0; row < source.materials.size(); ++row)
		{
			work.rowsVisited = row + 1;
			const auto& material = source.materials[row];
			bool referenced = material.textureIndex == 0 || material.normalTextureIndex == 0 ||
				material.metallicTextureIndex == 0 || material.roughnessTextureIndex == 0 ||
				material.emissiveTextureIndex == 0;
			if (!referenced && row < source.lightMetadata.size())
			{
				const auto& metadata = source.lightMetadata[row];
				referenced = metadata.textureIndex == 0 || metadata.glowmapTextureIndex == 0 ||
					metadata.normalTextureIndex == 0 || metadata.metallicTextureIndex == 0 ||
					metadata.roughnessTextureIndex == 0 || metadata.emissiveTextureIndex == 0;
			}
			if (referenced)
			{
				visit(0);
				work.singleTextureFound = true;
				break;
			}
		}
	}
	else
	{
		for (size_t row = 0; row < source.materials.size(); ++row)
		{
			const auto& material = source.materials[row];
			visit(material.textureIndex);
			visit(material.normalTextureIndex);
			visit(material.metallicTextureIndex);
			visit(material.roughnessTextureIndex);
			visit(material.emissiveTextureIndex);
			if (row < source.lightMetadata.size())
			{
				const auto& metadata = source.lightMetadata[row];
				visit(metadata.textureIndex);
				visit(metadata.glowmapTextureIndex);
				visit(metadata.normalTextureIndex);
				visit(metadata.metallicTextureIndex);
				visit(metadata.roughnessTextureIndex);
				visit(metadata.emissiveTextureIndex);
			}
		}
		work.rowsVisited = source.materials.size();
	}
	if (destination.paletteLookup.empty())
	{
		destination.paletteLookup = source.paletteLookup;
		destination.paletteWidth = source.paletteWidth;
		destination.paletteHeight = source.paletteHeight;
	}
	return work;
}

bool CopyMaterialBridgeRowRange(
	const MaterialBridgeData& source,
	MaterialRowRange range,
	MaterialBridgeData& destination,
	uint32_t destinationFirst,
	const std::unordered_map<uint64_t, uint32_t>& textureLookup)
{
	if (range.first > source.materials.size() || range.count > source.materials.size() - range.first ||
		range.first > source.lightMetadata.size() || range.count > source.lightMetadata.size() - range.first ||
		destinationFirst > destination.materials.size() || range.count > destination.materials.size() - destinationFirst ||
		destinationFirst > destination.lightMetadata.size() || range.count > destination.lightMetadata.size() - destinationFirst)
		return false;
	auto remap = [&](uint32_t index)
	{
		if (index >= source.textures.size()) return index;
		const auto found = textureLookup.find(source.textures[index].key);
		return found != textureLookup.end() ? found->second : index;
	};
	for (uint32_t index = 0; index < range.count; ++index)
	{
		auto material = source.materials[range.first + index];
		auto metadata = source.lightMetadata[range.first + index];
		material.textureIndex = remap(material.textureIndex);
		material.normalTextureIndex = remap(material.normalTextureIndex);
		material.metallicTextureIndex = remap(material.metallicTextureIndex);
		material.roughnessTextureIndex = remap(material.roughnessTextureIndex);
		material.emissiveTextureIndex = remap(material.emissiveTextureIndex);
		metadata.textureIndex = remap(metadata.textureIndex);
		metadata.glowmapTextureIndex = remap(metadata.glowmapTextureIndex);
		metadata.normalTextureIndex = remap(metadata.normalTextureIndex);
		metadata.metallicTextureIndex = remap(metadata.metallicTextureIndex);
		metadata.roughnessTextureIndex = remap(metadata.roughnessTextureIndex);
		metadata.emissiveTextureIndex = remap(metadata.emissiveTextureIndex);
		destination.materials[destinationFirst + index] = material;
		destination.lightMetadata[destinationFirst + index] = metadata;
	}
	return true;
}
}
