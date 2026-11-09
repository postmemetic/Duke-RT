#include "nri_material_payload_semantics.h"

#include <algorithm>
#include <cstring>
#include <tuple>

namespace nri_scene
{
namespace
{
auto MetadataValues(const MaterialLightingMetadata& m)
{
	return std::tie(m.texture, m.materialKey, m.textureContentKey, m.glowmapContentKey,
		m.normalContentKey, m.metallicContentKey, m.roughnessContentKey, m.textureId,
		m.baseTextureId, m.authoredTextureId, m.textureIndex, m.glowmapTextureIndex,
		m.normalTextureIndex, m.metallicTextureIndex, m.roughnessTextureIndex,
		m.emissiveTextureIndex, m.paletteIndex, m.materialFlags, m.lightingFlags,
		m.materialClass, m.emissiveMode, m.emissiveStableFrames, m.voxelPaletteIndex,
		m.voxelPalettePolicyFlags, m.voxelPalettePolicyApplied, m.sourceType,
		m.sectorIndex, m.actorOverlayRuleCount, m.shade, m.alpha, m.lightLevel,
		m.emissiveIntensity, m.emissiveMaskScale, m.visibleFullbrightBoost);
}

struct Hasher
{
	uint64_t value = 14695981039346656037ull, bytes = 0;
	void Bytes(const void* data, size_t size)
	{
		const auto* p = static_cast<const uint8_t*>(data);
		for (size_t i = 0; i < size; ++i) value = (value ^ p[i]) * 1099511628211ull;
		bytes += size;
	}
	template<class T> void Add(const T& v) { Bytes(&v, sizeof(v)); }
	void Add(float v) { if (v == 0.0f) v = 0.0f; Bytes(&v, sizeof(v)); }
};
}

uint64_t HashMaterialPayload(const MaterialBridgeData& bridge, uint64_t* bytesHashed)
{
	MaterialPayloadHashWork work;
	const auto hash = HashMaterialPayloadWithWork(bridge, false, work);
	if (bytesHashed) *bytesHashed = work.bytesHashed;
	return hash;
}

uint64_t HashMaterialPayloadWithWork(const MaterialBridgeData& bridge, bool useDigests,
	MaterialPayloadHashWork& work)
{
	work = {};
	Hasher h;
	auto immutableBytes = [&](const ImmutableBytePayload& bytes)
	{
		if (!useDigests)
		{
			h.Bytes(bytes.data(), bytes.size());
			work.immutableBytesScanned += bytes.size();
			return;
		}
		const auto digest = bytes.ContentDigest();
		h.Add(digest.value);
		work.immutableBytesScanned += digest.bytesScanned;
		if (!bytes.empty())
		{
			if (digest.computed) ++work.digestMisses;
			else { ++work.digestHits; work.immutableBytesAvoided += bytes.size(); }
		}
	};
	h.Add(bridge.materials.size());
	h.Bytes(bridge.materials.data(), bridge.materials.size() * sizeof(MaterialData));
	h.Add(bridge.lightMetadata.size());
	for (const auto& m : bridge.lightMetadata)
	{
		std::apply([&](const auto&... fields) { (h.Add(fields), ...); }, MetadataValues(m));
		for (auto v : m.actorOverlayRuleIds) h.Add(v);
		for (auto v : m.averageColor) h.Add(v);
		for (auto v : m.glowColor) h.Add(v);
		for (auto v : m.emissiveColor) h.Add(v);
	}
	h.Add(bridge.textures.size());
	for (const auto& t : bridge.textures)
	{
		h.Add(t.key); h.Add(t.width); h.Add(t.height); h.Add(t.mipCount);
		h.Add(t.indexed); h.Add(t.sourceTexture); h.Add(t.pixels.size());
		immutableBytes(t.pixels);
	}
	h.Add(bridge.paletteWidth); h.Add(bridge.paletteHeight); h.Add(bridge.paletteLookup.size());
	immutableBytes(bridge.paletteLookup);
	work.bytesHashed = h.bytes + (useDigests ? work.immutableBytesScanned : 0);
	return h.value;
}

bool MaterialPayloadsEquivalent(const MaterialBridgeData& a, const MaterialBridgeData& b)
{
	if (a.materials.size() != b.materials.size() || a.lightMetadata.size() != b.lightMetadata.size() ||
		a.textures.size() != b.textures.size() || a.paletteWidth != b.paletteWidth || a.paletteHeight != b.paletteHeight ||
		a.paletteLookup != b.paletteLookup) return false;
	if (!a.materials.empty() && std::memcmp(a.materials.data(), b.materials.data(), a.materials.size() * sizeof(MaterialData)))
		return false;
	for (size_t i = 0; i < a.lightMetadata.size(); ++i)
	{
		const auto& x = a.lightMetadata[i]; const auto& y = b.lightMetadata[i];
		if (MetadataValues(x) != MetadataValues(y) ||
			!std::equal(std::begin(x.actorOverlayRuleIds), std::end(x.actorOverlayRuleIds), std::begin(y.actorOverlayRuleIds)) ||
			!std::equal(std::begin(x.averageColor), std::end(x.averageColor), std::begin(y.averageColor)) ||
			!std::equal(std::begin(x.glowColor), std::end(x.glowColor), std::begin(y.glowColor)) ||
			!std::equal(std::begin(x.emissiveColor), std::end(x.emissiveColor), std::begin(y.emissiveColor))) return false;
	}
	for (size_t i = 0; i < a.textures.size(); ++i)
	{
		const auto& x = a.textures[i]; const auto& y = b.textures[i];
		if (std::tie(x.key, x.width, x.height, x.mipCount, x.indexed, x.sourceTexture) !=
			std::tie(y.key, y.width, y.height, y.mipCount, y.indexed, y.sourceTexture) || x.pixels != y.pixels) return false;
	}
	return true;
}
}
