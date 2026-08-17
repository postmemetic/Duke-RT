#pragma once

#include "nri_scene_bridge.h"

#include <initializer_list>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nri_scene
{
class ImmutableBytePayload
{
public:
	ImmutableBytePayload() = default;
	ImmutableBytePayload(std::initializer_list<uint8_t> bytes) { Assign(std::vector<uint8_t>(bytes)); }
	ImmutableBytePayload(const std::vector<uint8_t>& bytes) { Assign(std::vector<uint8_t>(bytes)); }
	ImmutableBytePayload(std::vector<uint8_t>&& bytes) { Assign(std::move(bytes)); }

	ImmutableBytePayload& operator=(std::initializer_list<uint8_t> bytes)
	{
		Assign(std::vector<uint8_t>(bytes));
		return *this;
	}
	ImmutableBytePayload& operator=(const std::vector<uint8_t>& bytes)
	{
		Assign(std::vector<uint8_t>(bytes));
		return *this;
	}
	ImmutableBytePayload& operator=(std::vector<uint8_t>&& bytes)
	{
		Assign(std::move(bytes));
		return *this;
	}

	void Assign(std::vector<uint8_t>&& bytes, uint64_t signature = 0)
	{
		mBytes = bytes.empty() ? nullptr : std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
		mSignature = mBytes != nullptr ? signature : 0;
	}

	template <typename Iterator>
	void assign(Iterator first, Iterator last)
	{
		Assign(std::vector<uint8_t>(first, last));
	}

	void clear()
	{
		mBytes.reset();
		mSignature = 0;
	}
	void reserve(size_t) {}

	bool empty() const { return mBytes == nullptr || mBytes->empty(); }
	size_t size() const { return mBytes != nullptr ? mBytes->size() : 0; }
	size_t capacity() const { return size(); }
	const uint8_t* data() const { return mBytes != nullptr ? mBytes->data() : nullptr; }
	uint64_t signature() const { return mSignature; }
	uint64_t uniqueStorageBytes() const
	{
		return mBytes != nullptr && mBytes.use_count() == 1 ? (uint64_t)mBytes->size() : 0;
	}

	bool operator==(const ImmutableBytePayload& other) const
	{
		return mBytes == other.mBytes || Bytes() == other.Bytes();
	}
	bool operator!=(const ImmutableBytePayload& other) const { return !(*this == other); }
	bool operator==(const std::vector<uint8_t>& other) const { return Bytes() == other; }
	bool operator!=(const std::vector<uint8_t>& other) const { return !(*this == other); }

private:
	const std::vector<uint8_t>& Bytes() const
	{
		static const std::vector<uint8_t> empty;
		return mBytes != nullptr ? *mBytes : empty;
	}

	std::shared_ptr<const std::vector<uint8_t>> mBytes;
	uint64_t mSignature = 0;
};

struct IndexedTexturePayloadCacheEntry
{
	uint64_t sourceSignature = 0;
	uint64_t contentKey = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	ImmutableBytePayload pixels;
};

class IndexedTexturePayloadCache
{
public:
	bool FindSource(
		uint64_t sourceSignature,
		uint32_t width,
		uint32_t height,
		IndexedTexturePayloadCacheEntry& outEntry) const
	{
		const auto found = mSources.find(sourceSignature);
		if (found == mSources.end() || found->second.width != width || found->second.height != height)
		{
			return false;
		}
		outEntry = found->second;
		return true;
	}

	IndexedTexturePayloadCacheEntry ResolveSource(
		uint64_t sourceSignature,
		uint32_t width,
		uint32_t height,
		std::vector<uint8_t>&& realizedPixels)
	{
		IndexedTexturePayloadCacheEntry existing = {};
		if (FindSource(sourceSignature, width, height, existing))
		{
			return existing;
		}

		uint64_t contentKey = HashBytes(realizedPixels.data(), realizedPixels.size());
		contentKey ^= ((uint64_t)width << 32) | (uint64_t)height;
		contentKey ^= 1ull << 63;
		uint64_t collisionIndex = 0;
		for (;;)
		{
			const auto content = mContent.find(contentKey);
			if (content == mContent.end())
			{
				ImmutableBytePayload payload;
				payload.Assign(std::move(realizedPixels), contentKey);
				mContent.emplace(contentKey, payload);
				existing = { sourceSignature, contentKey, width, height, payload };
				break;
			}
			if (content->second == realizedPixels)
			{
				existing = { sourceSignature, contentKey, width, height, content->second };
				break;
			}
			contentKey = CombineHash(contentKey, ++collisionIndex);
		}
		mSources[sourceSignature] = existing;
		return existing;
	}

	size_t SourceCount() const { return mSources.size(); }
	size_t ContentCount() const { return mContent.size(); }
	uint64_t ContentBytes() const
	{
		uint64_t bytes = 0;
		for (const auto& pair : mContent)
		{
			bytes += pair.second.size();
		}
		return bytes;
	}

private:
	static uint64_t HashBytes(const uint8_t* bytes, size_t size)
	{
		uint64_t hash = 1469598103934665603ull;
		for (size_t index = 0; index < size; ++index)
		{
			hash ^= bytes[index];
			hash *= 1099511628211ull;
		}
		return hash;
	}

	static uint64_t CombineHash(uint64_t hash, uint64_t value)
	{
		return hash ^ (value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2));
	}

	std::unordered_map<uint64_t, IndexedTexturePayloadCacheEntry> mSources;
	std::unordered_map<uint64_t, ImmutableBytePayload> mContent;
};

enum MaterialLightingFlags : uint32_t
{
	MaterialLightingFlag_None = 0,
	MaterialLightingFlag_MaterialFullbright = 1u << 0,
	MaterialLightingFlag_TextureFullbright = 1u << 1,
	MaterialLightingFlag_TextureGlowing = 1u << 2,
	MaterialLightingFlag_TextureAutoGlowing = 1u << 3,
	MaterialLightingFlag_HasGlowmap = 1u << 4,
	MaterialLightingFlag_NoShadowReceive = 1u << 5,
	MaterialLightingFlag_NoShadowCast = 1u << 6,
	MaterialLightingFlag_SmokeForeground = 1u << 7,
};

enum MaterialEmissiveMode : uint32_t
{
	MaterialEmissiveMode_None = 0,
	MaterialEmissiveMode_UseBaseTexture = 1,
	MaterialEmissiveMode_UseConstantColor = 2,
	MaterialEmissiveMode_UseGlowmapTexture = 3,
	MaterialEmissiveMode_UseAlbedo = MaterialEmissiveMode_UseBaseTexture,
};

struct MaterialData
{
	uint32_t textureIndex = 0;
	uint32_t paletteIndex = 0;
	uint32_t flags = 0;
	uint32_t materialClass = 0;
	uint32_t lightingFlags = 0;
	uint32_t normalTextureIndex = UINT32_MAX;
	uint32_t metallicTextureIndex = UINT32_MAX;
	uint32_t roughnessTextureIndex = UINT32_MAX;
	uint32_t sectorIndex = UINT32_MAX;
	uint32_t emissiveTextureIndex = UINT32_MAX;
	float lightLevel = 1.0f;
	float alpha = 1.0f;
	float roughnessHint = 0.45f;
	float metalnessHint = 0.0f;
	float emissiveColor[3] = {};
	float emissiveIntensity = 0.0f;
	float emissiveMaskScale = 0.0f;
	uint32_t emissiveMode = MaterialEmissiveMode_None;
	float emissiveReserved = 0.0f;
};

struct MaterialLightingMetadata
{
	FGameTexture* texture = nullptr;
	uint64_t materialKey = 0;
	uint64_t textureContentKey = 0;
	uint64_t glowmapContentKey = 0;
	uint64_t normalContentKey = 0;
	uint64_t metallicContentKey = 0;
	uint64_t roughnessContentKey = 0;
	uint32_t textureId = 0;
	uint32_t baseTextureId = 0;
	uint32_t textureIndex = 0;
	uint32_t glowmapTextureIndex = UINT32_MAX;
	uint32_t normalTextureIndex = UINT32_MAX;
	uint32_t metallicTextureIndex = UINT32_MAX;
	uint32_t roughnessTextureIndex = UINT32_MAX;
	uint32_t emissiveTextureIndex = UINT32_MAX;
	uint32_t paletteIndex = 0;
	uint32_t materialFlags = 0;
	uint32_t lightingFlags = 0;
	uint32_t materialClass = 0;
	uint32_t emissiveMode = MaterialEmissiveMode_None;
	uint32_t emissiveStableFrames = 0;
	uint32_t voxelPaletteIndex = UINT32_MAX;
	uint32_t voxelPalettePolicyFlags = 0;
	bool voxelPalettePolicyApplied = false;
	SurfaceSourceType sourceType = {};
	int32_t sectorIndex = -1;
	int32_t actorIndex = -1;
	uint32_t actorOverlayRuleCount = 0;
	uint32_t actorOverlayRuleIds[MaxActorOverlayRuleIdsPerSurface] = {};
	int32_t shade = 0;
	float alpha = 1.0f;
	float lightLevel = 1.0f;
	float averageColor[3] = { 1.0f, 1.0f, 1.0f };
	float glowColor[3] = {};
	float emissiveColor[3] = {};
	float emissiveIntensity = 0.0f;
	float emissiveMaskScale = 0.0f;
	float visibleFullbrightBoost = 1.0f;
};

struct TextureUpload
{
	uint64_t key = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	bool indexed = false;
	FTexture* sourceTexture = nullptr;
	ImmutableBytePayload pixels;
};

struct MaterialBridgeBuildStats
{
	double materialRowsMs = 0.0;
	double paletteMs = 0.0;
	double indexedPayloadMs = 0.0;
	uint64_t paletteBytesBuilt = 0;
	uint64_t indexedPayloadBytes = 0;
	uint32_t paletteBuilds = 0;
	uint32_t paletteReuses = 0;
	uint32_t indexedPayloadRealizations = 0;
	uint32_t indexedPayloadReuses = 0;
};

struct MaterialBridgeData
{
	std::vector<MaterialData> materials;
	std::vector<MaterialLightingMetadata> lightMetadata;
	std::vector<TextureUpload> textures;
	ImmutableBytePayload paletteLookup;
	uint32_t paletteWidth = 256;
	uint32_t paletteHeight = 256;
	MaterialBridgeBuildStats buildStats;
};

void BuildMaterials(const SceneView& sceneView, MaterialBridgeData& outMaterials);
uint64_t EstimateMaterialBridgeBytes(const MaterialBridgeData& materials);
void ClearMaterialBridgeRetainingCapacity(MaterialBridgeData& materials);
void AppendMaterialBridge(const MaterialBridgeData& source, MaterialBridgeData& destination);
void AppendMaterialBridge(
	const MaterialBridgeData& source,
	MaterialBridgeData& destination,
	std::unordered_map<uint64_t, uint32_t>& textureLookup);
bool RealizeTextureUploadPayload(const TextureUpload& upload, std::vector<uint8_t>& outPixels, uint32_t& outWidth, uint32_t& outHeight);
}
