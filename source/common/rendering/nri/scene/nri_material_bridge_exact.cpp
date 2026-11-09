#include "nri_material_bridge_exact.h"
#include <cstring>

namespace nri_scene
{
namespace
{
template <typename T> bool SameBits(const T& a, const T& b)
{ return std::memcmp(&a, &b, sizeof(T)) == 0; }
bool Same(const MaterialLightingMetadata& a, const MaterialLightingMetadata& b)
{
	return a.texture == b.texture &&
		a.materialKey == b.materialKey &&
		a.textureContentKey == b.textureContentKey &&
		a.glowmapContentKey == b.glowmapContentKey &&
		a.normalContentKey == b.normalContentKey &&
		a.metallicContentKey == b.metallicContentKey &&
		a.roughnessContentKey == b.roughnessContentKey &&
		a.textureId == b.textureId &&
		a.baseTextureId == b.baseTextureId &&
		a.authoredTextureId == b.authoredTextureId &&
		a.textureIndex == b.textureIndex &&
		a.glowmapTextureIndex == b.glowmapTextureIndex &&
		a.normalTextureIndex == b.normalTextureIndex &&
		a.metallicTextureIndex == b.metallicTextureIndex &&
		a.roughnessTextureIndex == b.roughnessTextureIndex &&
		a.emissiveTextureIndex == b.emissiveTextureIndex &&
		a.paletteIndex == b.paletteIndex &&
		a.materialFlags == b.materialFlags &&
		a.lightingFlags == b.lightingFlags &&
		a.materialClass == b.materialClass &&
		a.emissiveMode == b.emissiveMode &&
		a.emissiveStableFrames == b.emissiveStableFrames &&
		a.voxelPaletteIndex == b.voxelPaletteIndex &&
		a.voxelPalettePolicyFlags == b.voxelPalettePolicyFlags &&
		a.voxelPalettePolicyApplied == b.voxelPalettePolicyApplied &&
		a.sourceType == b.sourceType &&
		a.sectorIndex == b.sectorIndex &&
		a.actorIndex == b.actorIndex &&
		a.actorOverlayRuleCount == b.actorOverlayRuleCount &&
		SameBits(a.actorOverlayRuleIds, b.actorOverlayRuleIds) &&
		a.shade == b.shade &&
		SameBits(a.alpha, b.alpha) &&
		SameBits(a.lightLevel, b.lightLevel) &&
		SameBits(a.averageColor, b.averageColor) &&
		SameBits(a.glowColor, b.glowColor) &&
		SameBits(a.emissiveColor, b.emissiveColor) &&
		SameBits(a.emissiveIntensity, b.emissiveIntensity) &&
		SameBits(a.emissiveMaskScale, b.emissiveMaskScale) &&
		SameBits(a.visibleFullbrightBoost, b.visibleFullbrightBoost);
}
bool Same(const MaterialBridgeBuildStats& a, const MaterialBridgeBuildStats& b)
{
	return SameBits(a.materialRowsMs, b.materialRowsMs) &&
		SameBits(a.paletteMs, b.paletteMs) &&
		SameBits(a.indexedPayloadMs, b.indexedPayloadMs) &&
		SameBits(a.indexedMipMs, b.indexedMipMs) &&
		a.paletteBytesBuilt == b.paletteBytesBuilt &&
		a.indexedPayloadBytes == b.indexedPayloadBytes &&
		a.indexedMipVariantBytes == b.indexedMipVariantBytes &&
		a.paletteBuilds == b.paletteBuilds &&
		a.paletteReuses == b.paletteReuses &&
		a.indexedPayloadRealizations == b.indexedPayloadRealizations &&
		a.indexedPayloadReuses == b.indexedPayloadReuses &&
		a.indexedMipVariants == b.indexedMipVariants &&
		a.indexedMipBuilds == b.indexedMipBuilds &&
		a.indexedMipReuses == b.indexedMipReuses;
}
}
bool MaterialBridgesExactlyEqual(const MaterialBridgeData& a, const MaterialBridgeData& b)
{
	if (a.materials.size() != b.materials.size() || a.lightMetadata.size() != b.lightMetadata.size() ||
		a.textures.size() != b.textures.size() || a.paletteWidth != b.paletteWidth || a.paletteHeight != b.paletteHeight ||
		a.paletteLookup.signature() != b.paletteLookup.signature() || a.paletteLookup != b.paletteLookup ||
		!Same(a.buildStats, b.buildStats)) return false;
	if (!a.materials.empty() && std::memcmp(a.materials.data(), b.materials.data(), a.materials.size() * sizeof(MaterialData))) return false;
	for (size_t i = 0; i < a.lightMetadata.size(); ++i)
		if (!Same(a.lightMetadata[i], b.lightMetadata[i])) return false;
	for (size_t i = 0; i < a.textures.size(); ++i)
	{
		const auto& x = a.textures[i]; const auto& y = b.textures[i];
		if (x.key != y.key || x.width != y.width || x.height != y.height || x.mipCount != y.mipCount ||
			x.indexed != y.indexed || x.sourceTexture != y.sourceTexture ||
			x.pixels.signature() != y.pixels.signature() || x.pixels != y.pixels) return false;
	}
	return true;
}
}
