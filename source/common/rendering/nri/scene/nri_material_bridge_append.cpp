#include "nri_material_bridge.h"
#include "nri_material_bridge_append_reference.h"
#include "nri_material_bridge_exact.h"
#include "../renderer/nri_cvars.h"
#include "perf_audit.h"
#include "printf.h"

namespace nri_scene
{
void AppendMaterialBridge(const MaterialBridgeData& source, MaterialBridgeData& destination)
{
 std::vector<uint64_t> scratch;
 AppendMaterialBridge(source, destination, scratch);
}
void AppendMaterialBridge(const MaterialBridgeData& source, MaterialBridgeData& destination,
 std::vector<uint64_t>& scratch)
{
 std::unordered_map<uint64_t, uint32_t> lookup;
 lookup.reserve(destination.textures.size() + source.textures.size());
 for (uint32_t i = 0; i < destination.textures.size(); ++i) lookup.emplace(destination.textures[i].key, i);
 AppendMaterialBridge(source, destination, lookup, scratch);
}
void AppendMaterialBridge(const MaterialBridgeData& source, MaterialBridgeData& destination,
 std::unordered_map<uint64_t, uint32_t>& lookup)
{
 std::vector<uint64_t> scratch;
 AppendMaterialBridge(source, destination, lookup, scratch);
}
void AppendMaterialBridge(
	const MaterialBridgeData& source,
	MaterialBridgeData& destination,
	std::unordered_map<uint64_t, uint32_t>& textureLookup,
	std::vector<uint64_t>& textureRemap)
{

	PerfAuditScope auditAppend(PerfAuditMetric::TextureResolveBridgeAppendNs);
	PerfAuditAdd(PerfAuditMetric::TextureResolveBridgeAppendCalls);
	PerfAuditAdd(PerfAuditMetric::TextureResolveBridgeAppendRows, source.materials.size());
	const bool validate = (bool)nri_pttextureresolvevalidate;
	MaterialBridgeData reference;
	std::unordered_map<uint64_t, uint32_t> referenceLookup;
	if (validate)
	{
		PerfAuditScope auditReference(PerfAuditMetric::TextureResolveValidationNs);
		reference = destination;
		referenceLookup = textureLookup;
		AppendMaterialBridgeReference(source, reference, referenceLookup);
	}
	textureRemap.assign(source.materials.empty() ? 0 : source.textures.size(), UINT64_MAX);
	uint64_t textureLookups = 0;
	auto remapTextureIndex = [&source, &destination, &textureLookup, &textureLookups, &textureRemap](uint32_t textureIndex) -> uint32_t
	{
		if (textureIndex == UINT32_MAX)
		{
			return UINT32_MAX;
		}
		if (textureIndex >= source.textures.size())
		{
			return textureIndex;
		}

		if (textureRemap[textureIndex] != UINT64_MAX) return (uint32_t)textureRemap[textureIndex];
		const auto& texture = source.textures[textureIndex];
		++textureLookups;
		auto it = textureLookup.find(texture.key);
		if (it == textureLookup.end())
		{
			const uint32_t newIndex = (uint32_t)destination.textures.size();
			textureLookup.emplace(texture.key, newIndex);
			destination.textures.push_back(texture);
			textureRemap[textureIndex] = newIndex;
			return newIndex;
		}

		textureRemap[textureIndex] = it->second;
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

	PerfAuditAdd(PerfAuditMetric::TextureResolveBridgeAppendKeyLookups, textureLookups);
	if (destination.paletteLookup.empty())
	{
		destination.paletteLookup = source.paletteLookup;
		destination.paletteWidth = source.paletteWidth;
		destination.paletteHeight = source.paletteHeight;
	}
	if (validate)
	{
		PerfAuditScope auditReference(PerfAuditMetric::TextureResolveValidationNs);
		PerfAuditAdd(PerfAuditMetric::TextureResolveBridgeAppendValidationChecks);
		if (!MaterialBridgesExactlyEqual(destination, reference) || textureLookup != referenceLookup)
		{
			PerfAuditAdd(PerfAuditMetric::TextureResolveValidationMismatches);
			PerfAuditAdd(PerfAuditMetric::TextureResolveQuarantines);
			Printf("NRI PT texture resolve validation: operation=append mismatch=1 action=reference-for-operation\n");
			destination = std::move(reference);
			textureLookup = std::move(referenceLookup);
		}
	}
}
}
