#include "nri_material_bridge.h"
#include "nri_voxel_palette_policy.h"
#include "../renderer/nri_cvars.h"

#include "nri_hash.h"
#include "nri_texture_signature.h"

#include "palette.h"
#include "tiletexture.h"
#include "textures.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <mutex>
#include <unordered_map>


namespace
{
	using namespace nri_scene;

	enum MaterialClass : uint32_t
	{
		MaterialClass_DefaultDiffuse = 0,
		MaterialClass_UnstableDiffuse = 1,
		MaterialClass_Emissive = 2,
		MaterialClass_SpecularSpecial = 3,
	};

	float ComputeLightLevel(const MaterialRef&)
	{
		return 1.0f;
	}

	float ComputeRoughnessHint(const MaterialRef& materialRef, float lightLevel)
	{
		const uint32_t flags = materialRef.flags;
		if ((flags & MaterialFlag_Mirror) != 0)
		{
			return 0.02f;
		}

		if ((flags & MaterialFlag_Portal) != 0)
		{
			return 0.04f;
		}

		if ((flags & MaterialFlag_Fullbright) != 0)
		{
			if ((flags & MaterialFlag_Sprite) != 0)
			{
				return 0.45f;
			}
			if ((flags & MaterialFlag_Flat) != 0)
			{
				return 0.60f;
			}
			return 0.55f;
		}

		float roughness = 0.45f;
		if ((flags & MaterialFlag_Sprite) != 0)
		{
			roughness = 0.85f;
		}
		else if ((flags & MaterialFlag_Flat) != 0)
		{
			roughness = 0.65f;
		}
		else if ((flags & MaterialFlag_Indexed) != 0)
		{
			roughness = 0.55f;
		}

		if ((flags & MaterialFlag_OneWay) != 0)
		{
			roughness = std::max(roughness, 0.70f);
		}

		if (materialRef.alpha < 0.999f)
		{
			roughness = std::max(roughness, 0.85f);
		}

		if (lightLevel < 0.25f)
		{
			roughness = std::min(1.0f, roughness + 0.05f);
		}

		return clamp(roughness, 0.02f, 1.0f);
	}

	bool IsPlainMirrorMaterial(const MaterialRef& materialRef)
	{
		return (materialRef.flags & MaterialFlag_PlainMirror) != 0;
	}

	float ComputeMetalnessHint(const MaterialRef& materialRef)
	{
		if (IsPlainMirrorMaterial(materialRef))
		{
			return 1.0f;
		}

		// Build surfaces are overwhelmingly non-metallic. Keep this explicit and conservative.
		return 0.0f;
	}

	uint32_t ComputeMaterialClass(const MaterialRef& materialRef)
	{
		const uint32_t flags = materialRef.flags;
		if (IsPlainMirrorMaterial(materialRef))
		{
			return MaterialClass_DefaultDiffuse;
		}

		if ((flags & (MaterialFlag_Mirror | MaterialFlag_Portal)) != 0)
		{
			return MaterialClass_SpecularSpecial;
		}

		if ((flags & (MaterialFlag_Sprite | MaterialFlag_Indexed | MaterialFlag_OneWay | MaterialFlag_AlphaClip)) != 0 || materialRef.alpha < 0.999f)
		{
			return MaterialClass_UnstableDiffuse;
		}

		return MaterialClass_DefaultDiffuse;
	}

	uint64_t MakeTextureKey(FGameTexture* texture, bool indexed)
	{
		return (uint64_t)(uintptr_t)texture ^ (indexed ? 1ull : 0ull);
	}

	uint64_t MakeTextureKey(FTexture* texture, bool indexed)
	{
		return ((uint64_t)(uintptr_t)texture ^ (indexed ? 1ull : 0ull)) ^ 0x4000000000000000ull;
	}

	IndexedTexturePayloadCache gIndexedTexturePayloadCache;
	std::mutex gIndexedTexturePayloadCacheMutex;

	bool TryBuildSharedTextureContentKey(FGameTexture* gameTexture, FTexture* baseTexture, bool indexed, uint64_t& outKey, uint32_t& outWidth, uint32_t& outHeight)
	{
		outKey = 0;
		outWidth = 0;
		outHeight = 0;

		TextureSignatureRequest request = {};
		request.contentKind = indexed ? TextureSignatureContentKind::Indexed : TextureSignatureContentKind::ProcessedBGRA;
		request.translation = 0;
		request.flags = TextureSignatureRequestFlag_None;

		TextureSignature signature = {};
		bool success = false;
		if (gameTexture != nullptr)
		{
			success = TryBuildTextureSignature(gameTexture, request, signature);
		}
		else if (baseTexture != nullptr)
		{
			success = TryBuildImageTextureSignature(baseTexture, request, signature);
		}

		if (!success || !signature.valid || !signature.persistentEligible || signature.width == 0 || signature.height == 0)
		{
			return false;
		}

		outKey = signature.key;
		outWidth = signature.width;
		outHeight = signature.height;
		return true;
	}

	bool ResolveIndexedTexturePayload(
		FTexture* texture,
		uint64_t sourceSignature,
		uint32_t sourceWidth,
		uint32_t sourceHeight,
		TextureUpload& upload,
		MaterialBridgeBuildStats& stats)
	{
		{
			std::lock_guard<std::mutex> lock(gIndexedTexturePayloadCacheMutex);
			IndexedTexturePayloadCacheEntry cached = {};
			if (gIndexedTexturePayloadCache.FindSource(
				sourceSignature, sourceWidth, sourceHeight, cached))
			{
				upload.key = cached.contentKey;
				upload.width = cached.width;
				upload.height = cached.height;
				upload.pixels = cached.pixels;
				stats.indexedPayloadReuses++;
				return true;
			}
		}

		const auto start = std::chrono::steady_clock::now();
		FTextureBuffer texBuffer = texture->CreateTexBuffer(0, CTF_Indexed);
		if (texBuffer.mBuffer == nullptr || texBuffer.mWidth <= 0 || texBuffer.mHeight <= 0)
		{
			stats.indexedPayloadMs += std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - start).count();
			return false;
		}

		std::vector<uint8_t> realizedPixels(
			texBuffer.mBuffer,
			texBuffer.mBuffer + (size_t)texBuffer.mWidth * (size_t)texBuffer.mHeight);
		IndexedTexturePayloadCacheEntry cached = {};
		{
			std::lock_guard<std::mutex> lock(gIndexedTexturePayloadCacheMutex);
			cached = gIndexedTexturePayloadCache.ResolveSource(
				sourceSignature,
				(uint32_t)texBuffer.mWidth,
				(uint32_t)texBuffer.mHeight,
				std::move(realizedPixels));
		}
		upload.key = cached.contentKey;
		upload.width = cached.width;
		upload.height = cached.height;
		upload.pixels = cached.pixels;
		stats.indexedPayloadRealizations++;
		stats.indexedPayloadBytes += upload.pixels.size();
		stats.indexedPayloadMs += std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - start).count();
		return true;
	}

	TextureUpload BuildTextureUpload(
		FGameTexture* gameTexture,
		FTexture* texture,
		bool indexed,
		MaterialBridgeBuildStats& stats)
	{
		TextureUpload upload = {};
		upload.indexed = indexed;
		upload.sourceTexture = texture;
		upload.key = gameTexture != nullptr ? MakeTextureKey(gameTexture, indexed) : MakeTextureKey(texture, indexed);

		if (texture == nullptr)
		{
			return upload;
		}

		uint64_t sharedKey = 0;
		uint32_t sharedWidth = 0;
		uint32_t sharedHeight = 0;
		const bool hasSharedKey = TryBuildSharedTextureContentKey(gameTexture, texture, indexed, sharedKey, sharedWidth, sharedHeight);

		const bool deferRealization = hasSharedKey && !indexed;
		if (indexed && hasSharedKey)
		{
			ResolveIndexedTexturePayload(
				texture, sharedKey, sharedWidth, sharedHeight, upload, stats);
		}
		else if (deferRealization)
		{
			upload.width = sharedWidth;
			upload.height = sharedHeight;
			upload.key = sharedKey;
		}
		else if (indexed)
		{
			const auto start = std::chrono::steady_clock::now();
			FTextureBuffer texBuffer = texture->CreateTexBuffer(0, CTF_Indexed);
			if (texBuffer.mBuffer != nullptr && texBuffer.mWidth > 0 && texBuffer.mHeight > 0)
			{
				upload.width = (uint32_t)texBuffer.mWidth;
				upload.height = (uint32_t)texBuffer.mHeight;
				upload.pixels.assign(texBuffer.mBuffer, texBuffer.mBuffer + (size_t)texBuffer.mWidth * (size_t)texBuffer.mHeight);
				upload.key = Fnv1a64(upload.pixels.data(), upload.pixels.size());
				stats.indexedPayloadRealizations++;
				stats.indexedPayloadBytes += upload.pixels.size();
			}
			stats.indexedPayloadMs += std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - start).count();
		}
		else
		{
			FTextureBuffer texBuffer = texture->CreateTexBuffer(0, CTF_ProcessData);
			if (texBuffer.mBuffer != nullptr && texBuffer.mWidth > 0 && texBuffer.mHeight > 0)
			{
				upload.width = (uint32_t)texBuffer.mWidth;
				upload.height = (uint32_t)texBuffer.mHeight;
				upload.pixels.assign(texBuffer.mBuffer, texBuffer.mBuffer + (size_t)texBuffer.mWidth * (size_t)texBuffer.mHeight * 4u);
				upload.key = Fnv1a64(upload.pixels.data(), upload.pixels.size());
			}
		}

		if (hasSharedKey && !indexed && upload.width != 0 && upload.height != 0)
		{
			upload.width = sharedWidth;
			upload.height = sharedHeight;
			upload.key = sharedKey;
		}
		else if ((!indexed || !hasSharedKey) && upload.width != 0 && upload.height != 0)
		{
			upload.key ^= ((uint64_t)upload.width << 32) | (uint64_t)upload.height;
			upload.key ^= indexed ? (1ull << 63) : 0ull;
		}

		return upload;
	}

	TextureUpload BuildTextureUpload(FTexture* texture, bool indexed, MaterialBridgeBuildStats& stats)
	{
		return BuildTextureUpload(nullptr, texture, indexed, stats);
	}

	TextureUpload BuildTextureUpload(FGameTexture* texture, bool indexed, MaterialBridgeBuildStats& stats)
	{
		return texture != nullptr ? BuildTextureUpload(texture, texture->GetTexture(), indexed, stats) : TextureUpload{};
	}

	uint32_t EnsureTextureUploadIndex(FGameTexture* texture, bool indexed, std::unordered_map<uint64_t, uint32_t>& textureLookup, MaterialBridgeData& outMaterials)
	{
		const uint64_t textureKey = MakeTextureKey(texture, indexed);
		auto it = textureLookup.find(textureKey);
		if (it == textureLookup.end())
		{
			const uint32_t textureIndex = (uint32_t)outMaterials.textures.size();
			textureLookup.emplace(textureKey, textureIndex);
			outMaterials.textures.push_back(BuildTextureUpload(texture, indexed, outMaterials.buildStats));
			return textureIndex;
		}

		return it->second;
	}

	uint32_t EnsureTextureUploadIndex(FTexture* texture, bool indexed, std::unordered_map<uint64_t, uint32_t>& textureLookup, MaterialBridgeData& outMaterials)
	{
		const uint64_t textureKey = MakeTextureKey(texture, indexed);
		auto it = textureLookup.find(textureKey);
		if (it == textureLookup.end())
		{
			const uint32_t textureIndex = (uint32_t)outMaterials.textures.size();
			textureLookup.emplace(textureKey, textureIndex);
			outMaterials.textures.push_back(BuildTextureUpload(texture, indexed, outMaterials.buildStats));
			return textureIndex;
		}

		return it->second;
	}

	uint64_t ComputeMaterialKey(const MaterialRef& materialRef, const MaterialLightingMetadata& metadata)
	{
		uint64_t key = 1469598103934665603ull;
		Fnv1a64Append(key, &metadata.textureContentKey, sizeof(metadata.textureContentKey));
		Fnv1a64Append(key, &metadata.glowmapContentKey, sizeof(metadata.glowmapContentKey));
		Fnv1a64Append(key, &metadata.normalContentKey, sizeof(metadata.normalContentKey));
		Fnv1a64Append(key, &metadata.metallicContentKey, sizeof(metadata.metallicContentKey));
		Fnv1a64Append(key, &metadata.roughnessContentKey, sizeof(metadata.roughnessContentKey));
		Fnv1a64Append(key, &metadata.textureId, sizeof(metadata.textureId));
		Fnv1a64Append(key, &metadata.paletteIndex, sizeof(metadata.paletteIndex));
		Fnv1a64Append(key, &metadata.materialFlags, sizeof(metadata.materialFlags));
		Fnv1a64Append(key, &metadata.lightingFlags, sizeof(metadata.lightingFlags));
		Fnv1a64Append(key, &metadata.emissiveStableFrames, sizeof(metadata.emissiveStableFrames));
		Fnv1a64Append(key, &materialRef.alpha, sizeof(materialRef.alpha));
		return key;
	}

	float ComputeGlowEmissiveIntensity(const MaterialLightingMetadata& metadata)
	{
		float intensity = 0.0f;
		if ((metadata.lightingFlags & MaterialLightingFlag_TextureGlowing) != 0)
		{
			intensity = std::max(intensity, 1.25f);
		}
		if ((metadata.lightingFlags & MaterialLightingFlag_TextureAutoGlowing) != 0)
		{
			intensity = std::max(intensity, 1.10f);
		}
		if ((metadata.lightingFlags & MaterialLightingFlag_HasGlowmap) != 0)
		{
			intensity = std::max(intensity, 1.50f);
		}
		return intensity;
	}

	float GetVisibleFullbrightBoost()
	{
		return std::clamp((float)nri_ptfullbrightboost, 0.50f, 8.00f);
	}

	float GetVoxelEmissionBoostScale()
	{
		return 1.0f + std::max((float)nri_voxelemissionboost, 0.0f);
	}

	MaterialLightingMetadata BuildMaterialLightingMetadata(
		const SurfaceRef& surface,
		const MaterialData& material,
		uint64_t textureContentKey,
		uint32_t glowmapTextureIndex,
		uint64_t glowmapContentKey,
		uint32_t normalTextureIndex,
		uint64_t normalContentKey,
		uint32_t metallicTextureIndex,
		uint64_t metallicContentKey,
		uint32_t roughnessTextureIndex,
		uint64_t roughnessContentKey)
	{
		const MaterialRef& materialRef = surface.material;
		const bool inheritedEmissiveSource = materialRef.emissiveSourceTexture != nullptr && materialRef.emissiveSourceTexture != materialRef.texture;
		FGameTexture* lightingTexture = inheritedEmissiveSource ? materialRef.emissiveSourceTexture : materialRef.texture;
		FGameTexture* averageTexture = materialRef.texture != nullptr ? materialRef.texture : lightingTexture;

		MaterialLightingMetadata metadata = {};
		metadata.texture = lightingTexture != nullptr ? lightingTexture : materialRef.texture;
		metadata.textureContentKey = textureContentKey;
		metadata.textureIndex = material.textureIndex;
		metadata.glowmapTextureIndex = glowmapTextureIndex;
		metadata.normalTextureIndex = normalTextureIndex;
		metadata.metallicTextureIndex = metallicTextureIndex;
		metadata.roughnessTextureIndex = roughnessTextureIndex;
		metadata.paletteIndex = material.paletteIndex;
		metadata.materialFlags = material.flags;
		metadata.sourceType = surface.provenance.sourceType;
		metadata.sectorIndex = surface.provenance.sectorIndex;
		metadata.actorIndex = surface.provenance.actorIndex;
		metadata.actorOverlayRuleCount = std::min(surface.provenance.actorOverlayRuleCount, MaxActorOverlayRuleIdsPerSurface);
		for (uint32_t ruleIndex = 0; ruleIndex < metadata.actorOverlayRuleCount; ++ruleIndex)
		{
			metadata.actorOverlayRuleIds[ruleIndex] = surface.provenance.actorOverlayRuleIds[ruleIndex];
		}
		metadata.shade = materialRef.shade;
		metadata.alpha = material.alpha;
		metadata.lightLevel = material.lightLevel;
		metadata.materialClass = material.materialClass;
		metadata.normalContentKey = normalContentKey;
		metadata.metallicContentKey = metallicContentKey;
		metadata.roughnessContentKey = roughnessContentKey;

		const bool materialFullbright = (material.flags & MaterialFlag_Fullbright) != 0;
		if (materialFullbright)
		{
			metadata.lightingFlags |= MaterialLightingFlag_MaterialFullbright;
		}

		if (lightingTexture != nullptr)
		{
			const FTextureID lightingTextureId = lightingTexture->GetID();
			metadata.textureId = lightingTextureId.isValid() ? (uint32_t)lightingTextureId.GetIndex() : 0u;
			if (lightingTexture->isFullbright())
			{
				metadata.lightingFlags |= MaterialLightingFlag_TextureFullbright;
			}
			if (lightingTexture->isGlowing())
			{
				metadata.lightingFlags |= MaterialLightingFlag_TextureGlowing;
				lightingTexture->GetGlowColor(metadata.glowColor);
			}
			if (lightingTexture->isAutoGlowing())
			{
				metadata.lightingFlags |= MaterialLightingFlag_TextureAutoGlowing;
			}
			if (lightingTexture->GetGlowmap() != nullptr)
			{
				metadata.lightingFlags |= MaterialLightingFlag_HasGlowmap;
				metadata.glowmapContentKey = glowmapContentKey;
			}

			if (!TryGetAverageTextureColor(averageTexture, metadata.averageColor))
			{
				metadata.averageColor[0] = 1.0f;
				metadata.averageColor[1] = 1.0f;
				metadata.averageColor[2] = 1.0f;
			}
			if ((metadata.lightingFlags & MaterialLightingFlag_TextureGlowing) == 0 &&
				(metadata.lightingFlags & MaterialLightingFlag_HasGlowmap) != 0)
			{
				metadata.glowColor[0] = metadata.averageColor[0];
				metadata.glowColor[1] = metadata.averageColor[1];
				metadata.glowColor[2] = metadata.averageColor[2];
			}
		}
		if (materialRef.texture != nullptr)
		{
			const FTextureID baseTextureId = materialRef.texture->GetID();
			metadata.baseTextureId = baseTextureId.isValid() ? (uint32_t)baseTextureId.GetIndex() : 0u;
		}

		const bool sampledEmissive =
			(metadata.lightingFlags & MaterialLightingFlag_MaterialFullbright) != 0 ||
			(metadata.lightingFlags & MaterialLightingFlag_TextureFullbright) != 0;
		if (sampledEmissive)
		{
			metadata.emissiveMode = MaterialEmissiveMode_UseBaseTexture;
			metadata.emissiveTextureIndex = material.textureIndex;
			metadata.emissiveIntensity = 1.0f;
			metadata.emissiveMaskScale = 1.0f;
			metadata.visibleFullbrightBoost = GetVisibleFullbrightBoost();
			metadata.emissiveColor[0] = 1.0f;
			metadata.emissiveColor[1] = 1.0f;
			metadata.emissiveColor[2] = 1.0f;
		}
		else
		{
			const float glowIntensity = ComputeGlowEmissiveIntensity(metadata);
			if (glowIntensity > 0.0f)
			{
				metadata.emissiveMode = glowmapTextureIndex != UINT32_MAX ? MaterialEmissiveMode_UseGlowmapTexture : MaterialEmissiveMode_UseConstantColor;
				metadata.emissiveTextureIndex = glowmapTextureIndex;
				metadata.emissiveIntensity = glowIntensity;
				metadata.emissiveMaskScale = 1.0f;
				metadata.emissiveColor[0] = metadata.glowColor[0] > 0.0f ? metadata.glowColor[0] : metadata.averageColor[0];
				metadata.emissiveColor[1] = metadata.glowColor[1] > 0.0f ? metadata.glowColor[1] : metadata.averageColor[1];
				metadata.emissiveColor[2] = metadata.glowColor[2] > 0.0f ? metadata.glowColor[2] : metadata.averageColor[2];
				if (inheritedEmissiveSource && (metadata.lightingFlags & MaterialLightingFlag_HasGlowmap) != 0)
				{
					metadata.emissiveIntensity *= GetVoxelEmissionBoostScale();
				}
			}
		}

		metadata.materialKey = ComputeMaterialKey(surface.material, metadata);
		return metadata;
	}

	void ApplyVoxelPalettePolicyRow(
		const SurfaceRef& surface,
		uint32_t paletteIndex,
		MaterialData& material,
		MaterialLightingMetadata& metadata)
	{
		const VoxelPalettePolicyDocument* policy = surface.material.voxelPalettePolicy.get();
		if (policy == nullptr || paletteIndex >= policy->compiled.size())
		{
			return;
		}

		const uint8_t policyFlags = policy->compiled[paletteIndex];
		constexpr uint32_t inheritedLightingMask =
			MaterialLightingFlag_MaterialFullbright |
			MaterialLightingFlag_TextureFullbright |
			MaterialLightingFlag_TextureGlowing |
			MaterialLightingFlag_TextureAutoGlowing |
			MaterialLightingFlag_HasGlowmap |
			MaterialLightingFlag_NoShadowReceive |
			MaterialLightingFlag_NoShadowCast;
		material.flags &= ~(MaterialFlag_Fullbright | MaterialFlag_TintEmission);
		material.lightingFlags &= ~inheritedLightingMask;
		material.emissiveTextureIndex = UINT32_MAX;
		material.emissiveColor[0] = 0.0f;
		material.emissiveColor[1] = 0.0f;
		material.emissiveColor[2] = 0.0f;
		material.emissiveIntensity = 0.0f;
		material.emissiveMaskScale = 0.0f;
		material.emissiveMode = MaterialEmissiveMode_None;
		material.emissiveReserved = 1.0f;

		metadata.materialFlags &= ~(MaterialFlag_Fullbright | MaterialFlag_TintEmission);
		metadata.lightingFlags &= ~inheritedLightingMask;
		metadata.glowmapTextureIndex = UINT32_MAX;
		metadata.glowmapContentKey = 0;
		metadata.emissiveTextureIndex = UINT32_MAX;
		metadata.emissiveColor[0] = 0.0f;
		metadata.emissiveColor[1] = 0.0f;
		metadata.emissiveColor[2] = 0.0f;
		metadata.emissiveIntensity = 0.0f;
		metadata.emissiveMaskScale = 0.0f;
		metadata.emissiveMode = MaterialEmissiveMode_None;
		metadata.visibleFullbrightBoost = 1.0f;
		metadata.voxelPalettePolicyApplied = true;
		metadata.voxelPaletteIndex = paletteIndex;
		metadata.voxelPalettePolicyFlags = policyFlags;

		if ((policyFlags & VoxelPalettePolicyFlag_EmissionEnabled) != 0)
		{
			const float emissionScale = (float)std::clamp(policy->emissionScale, 0.0, 65536.0) * GetVoxelEmissionBoostScale();
			material.flags |= MaterialFlag_TintEmission;
			material.emissiveTextureIndex = material.textureIndex;
			material.emissiveColor[0] = 1.0f;
			material.emissiveColor[1] = 1.0f;
			material.emissiveColor[2] = 1.0f;
			material.emissiveIntensity = emissionScale;
			material.emissiveMaskScale = 1.0f;
			material.emissiveMode = MaterialEmissiveMode_UseBaseTexture;

			metadata.materialFlags |= MaterialFlag_TintEmission;
			metadata.emissiveTextureIndex = material.textureIndex;
			metadata.emissiveColor[0] = 1.0f;
			metadata.emissiveColor[1] = 1.0f;
			metadata.emissiveColor[2] = 1.0f;
			metadata.emissiveIntensity = emissionScale;
			metadata.emissiveMaskScale = 1.0f;
			metadata.emissiveMode = MaterialEmissiveMode_UseBaseTexture;
		}

		if ((policyFlags & VoxelPalettePolicyFlag_Fullbright) != 0)
		{
			material.flags |= MaterialFlag_Fullbright;
			material.lightingFlags |= MaterialLightingFlag_MaterialFullbright;
			material.lightLevel = 1.0f;
			material.emissiveReserved = GetVisibleFullbrightBoost();
			metadata.materialFlags |= MaterialFlag_Fullbright;
			metadata.lightingFlags |= MaterialLightingFlag_MaterialFullbright;
			metadata.lightLevel = 1.0f;
			metadata.visibleFullbrightBoost = GetVisibleFullbrightBoost();
		}
		if ((policyFlags & VoxelPalettePolicyFlag_NoShadowCast) != 0)
		{
			material.lightingFlags |= MaterialLightingFlag_NoShadowCast;
			metadata.lightingFlags |= MaterialLightingFlag_NoShadowCast;
		}
		if ((policyFlags & VoxelPalettePolicyFlag_NoShadowReceive) != 0)
		{
			material.lightingFlags |= MaterialLightingFlag_NoShadowReceive;
			metadata.lightingFlags |= MaterialLightingFlag_NoShadowReceive;
		}

		metadata.materialKey = HashCombine64(metadata.materialKey, surface.material.voxelPalettePolicyContentKey);
		metadata.materialKey = HashCombine64(metadata.materialKey, ((uint64_t)paletteIndex << 8u) | policyFlags);
	}

	void AppendSurfaceMaterial(const SurfaceRef& surface, std::unordered_map<uint64_t, uint32_t>& textureLookup, MaterialBridgeData& outMaterials)
	{
		const MaterialRef& materialRef = surface.material;
		MaterialData material = {};
		material.flags = materialRef.flags;
		material.paletteIndex = (uint32_t)clamp(materialRef.palette, 0, MAXPALOOKUPS - 1);
		material.lightLevel = ComputeLightLevel(materialRef);
		material.alpha = materialRef.alpha;
		material.roughnessHint = ComputeRoughnessHint(materialRef, material.lightLevel);
		material.metalnessHint = ComputeMetalnessHint(materialRef);
		material.materialClass = ComputeMaterialClass(materialRef);

		const bool indexed = (materialRef.flags & MaterialFlag_Indexed) != 0;
		material.textureIndex = EnsureTextureUploadIndex(materialRef.texture, indexed, textureLookup, outMaterials);

		material.sectorIndex = surface.provenance.sectorIndex >= 0 ? (uint32_t)surface.provenance.sectorIndex : UINT32_MAX;
		outMaterials.materials.push_back(material);
		uint32_t glowmapTextureIndex = UINT32_MAX;
		uint64_t glowmapContentKey = 0;
		uint32_t normalTextureIndex = UINT32_MAX;
		uint64_t normalContentKey = 0;
		uint32_t metallicTextureIndex = UINT32_MAX;
		uint64_t metallicContentKey = 0;
		uint32_t roughnessTextureIndex = UINT32_MAX;
		uint64_t roughnessContentKey = 0;
		const bool inheritedEmissiveSource = materialRef.emissiveSourceTexture != nullptr && materialRef.emissiveSourceTexture != materialRef.texture;
		FGameTexture* lightingTexture = inheritedEmissiveSource ? materialRef.emissiveSourceTexture : materialRef.texture;
		const bool useAuxiliaryMaps = !IsPlainMirrorMaterial(materialRef);
		if (!inheritedEmissiveSource && materialRef.texture != nullptr && materialRef.texture->GetGlowmap() != nullptr)
		{
			glowmapTextureIndex = EnsureTextureUploadIndex(materialRef.texture->GetGlowmap(), false, textureLookup, outMaterials);
			glowmapContentKey = outMaterials.textures[glowmapTextureIndex].key;
		}
		else if (inheritedEmissiveSource && lightingTexture != nullptr && lightingTexture->GetGlowmap() != nullptr)
		{
			glowmapTextureIndex = material.textureIndex;
			glowmapContentKey = outMaterials.textures[material.textureIndex].key;
		}
		if (useAuxiliaryMaps && materialRef.texture != nullptr && materialRef.texture->GetMetallic() != nullptr)
		{
			metallicTextureIndex = EnsureTextureUploadIndex(materialRef.texture->GetMetallic(), false, textureLookup, outMaterials);
			metallicContentKey = outMaterials.textures[metallicTextureIndex].key;
			outMaterials.materials.back().metallicTextureIndex = metallicTextureIndex;
		}
		if (useAuxiliaryMaps && materialRef.texture != nullptr && materialRef.texture->GetNormalmap() != nullptr)
		{
			normalTextureIndex = EnsureTextureUploadIndex(materialRef.texture->GetNormalmap(), false, textureLookup, outMaterials);
			normalContentKey = outMaterials.textures[normalTextureIndex].key;
			outMaterials.materials.back().normalTextureIndex = normalTextureIndex;
		}
		if (useAuxiliaryMaps && materialRef.texture != nullptr && materialRef.texture->GetRoughness() != nullptr)
		{
			roughnessTextureIndex = EnsureTextureUploadIndex(materialRef.texture->GetRoughness(), false, textureLookup, outMaterials);
			roughnessContentKey = outMaterials.textures[roughnessTextureIndex].key;
			outMaterials.materials.back().roughnessTextureIndex = roughnessTextureIndex;
		}

		MaterialLightingMetadata metadata = BuildMaterialLightingMetadata(
			surface,
			outMaterials.materials.back(),
			outMaterials.textures[material.textureIndex].key,
			glowmapTextureIndex,
			glowmapContentKey,
			normalTextureIndex,
			normalContentKey,
			metallicTextureIndex,
			metallicContentKey,
			roughnessTextureIndex,
			roughnessContentKey);
		outMaterials.materials.back().lightingFlags = metadata.lightingFlags;
		outMaterials.materials.back().emissiveReserved = metadata.visibleFullbrightBoost;
		outMaterials.lightMetadata.push_back(metadata);

		if (surface.material.voxelPalettePolicy != nullptr && surface.materialRowSpan == VoxelPalettePolicyEntryCount)
		{
			const MaterialData baseMaterial = outMaterials.materials.back();
			const MaterialLightingMetadata baseMetadata = outMaterials.lightMetadata.back();
			const size_t baseRow = outMaterials.materials.size() - 1u;
			for (uint32_t paletteIndex = 0; paletteIndex < VoxelPalettePolicyEntryCount; ++paletteIndex)
			{
				MaterialData rowMaterial = baseMaterial;
				MaterialLightingMetadata rowMetadata = baseMetadata;
				ApplyVoxelPalettePolicyRow(surface, paletteIndex, rowMaterial, rowMetadata);
				if (paletteIndex == 0)
				{
					outMaterials.materials[baseRow] = rowMaterial;
					outMaterials.lightMetadata[baseRow] = rowMetadata;
				}
				else
				{
					outMaterials.materials.push_back(rowMaterial);
					outMaterials.lightMetadata.push_back(rowMetadata);
				}
			}
		}
	}

	struct PaletteLookupCache
	{
		uint64_t sourceSignature = 0;
		ImmutableBytePayload payload;
	};

	PaletteLookupCache gPaletteLookupCache;
	std::mutex gPaletteLookupCacheMutex;

	uint64_t BuildPaletteSourceSignature()
	{
		uint64_t signature = 1469598103934665603ull;
		for (uint32_t colorIndex = 0; colorIndex < 256; ++colorIndex)
		{
			const PalEntry color = GPalette.BaseColors[colorIndex];
			Fnv1a64Append(signature, &color.b, sizeof(color.b));
			Fnv1a64Append(signature, &color.g, sizeof(color.g));
			Fnv1a64Append(signature, &color.r, sizeof(color.r));
		}
		for (uint32_t paletteIndex = 0; paletteIndex < MAXPALOOKUPS; ++paletteIndex)
		{
			const uint8_t* table = lookups.getTable((int)paletteIndex);
			if (table != nullptr)
			{
				Fnv1a64Append(signature, table, 256);
			}
			else
			{
				for (uint32_t colorIndex = 0; colorIndex < 256; ++colorIndex)
				{
					const uint8_t identity = (uint8_t)colorIndex;
					Fnv1a64Append(signature, &identity, sizeof(identity));
				}
			}
		}
		return signature;
	}

	bool PalettePayloadMatchesLiveSamples(const ImmutableBytePayload& payload)
	{
		if (payload.size() != (size_t)256 * MAXPALOOKUPS * 4u)
		{
			return false;
		}
		for (uint32_t sample = 0; sample < 32; ++sample)
		{
			const uint32_t paletteIndex = (sample * 67u) & 255u;
			const uint32_t colorIndex = (sample * 149u) & 255u;
			const uint8_t* table = lookups.getTable((int)paletteIndex);
			const uint8_t remappedIndex = table != nullptr ? table[colorIndex] : (uint8_t)colorIndex;
			const PalEntry color = GPalette.BaseColors[remappedIndex];
			const size_t pixelIndex = ((size_t)paletteIndex * 256u + colorIndex) * 4u;
			const uint8_t* bytes = payload.data();
			if (bytes[pixelIndex + 0] != color.b ||
				bytes[pixelIndex + 1] != color.g ||
				bytes[pixelIndex + 2] != color.r ||
				bytes[pixelIndex + 3] != 255)
			{
				return false;
			}
		}
		return true;
	}

	bool BuildPaletteLookup(MaterialBridgeData& outMaterials)
	{
		outMaterials.paletteWidth = 256;
		outMaterials.paletteHeight = MAXPALOOKUPS;
		const uint64_t sourceSignature = BuildPaletteSourceSignature();
		std::lock_guard<std::mutex> lock(gPaletteLookupCacheMutex);
		if (gPaletteLookupCache.sourceSignature == sourceSignature &&
			PalettePayloadMatchesLiveSamples(gPaletteLookupCache.payload))
		{
			outMaterials.paletteLookup = gPaletteLookupCache.payload;
			return false;
		}

		std::vector<uint8_t> paletteLookup(
			(size_t)outMaterials.paletteWidth * (size_t)outMaterials.paletteHeight * 4u);

		for (uint32_t paletteIndex = 0; paletteIndex < outMaterials.paletteHeight; ++paletteIndex)
		{
			const uint8_t* table = lookups.getTable((int)paletteIndex);
			for (uint32_t colorIndex = 0; colorIndex < outMaterials.paletteWidth; ++colorIndex)
			{
				const uint8_t remappedIndex = table != nullptr ? table[colorIndex] : (uint8_t)colorIndex;
				const PalEntry color = GPalette.BaseColors[remappedIndex];
				const size_t pixelIndex = ((size_t)paletteIndex * outMaterials.paletteWidth + colorIndex) * 4u;
				paletteLookup[pixelIndex + 0] = color.b;
				paletteLookup[pixelIndex + 1] = color.g;
				paletteLookup[pixelIndex + 2] = color.r;
				paletteLookup[pixelIndex + 3] = 255;
			}
		}
		const uint64_t payloadSignature = Fnv1a64(paletteLookup.data(), paletteLookup.size());
		gPaletteLookupCache.sourceSignature = sourceSignature;
		gPaletteLookupCache.payload.Assign(std::move(paletteLookup), payloadSignature);
		outMaterials.paletteLookup = gPaletteLookupCache.payload;
		return true;
	}
}

namespace nri_scene
{
bool RealizeTextureUploadPayload(const TextureUpload& upload, std::vector<uint8_t>& outPixels, uint32_t& outWidth, uint32_t& outHeight)
{
	outPixels.clear();
	outWidth = 0;
	outHeight = 0;

	if (upload.sourceTexture == nullptr)
	{
		return false;
	}

	if (upload.indexed)
	{
		FTextureBuffer texBuffer = upload.sourceTexture->CreateTexBuffer(0, CTF_Indexed);
		if (texBuffer.mBuffer == nullptr || texBuffer.mWidth <= 0 || texBuffer.mHeight <= 0)
		{
			return false;
		}

		outWidth = (uint32_t)texBuffer.mWidth;
		outHeight = (uint32_t)texBuffer.mHeight;
		outPixels.assign(texBuffer.mBuffer, texBuffer.mBuffer + (size_t)texBuffer.mWidth * (size_t)texBuffer.mHeight);
		return !outPixels.empty();
	}

	FTextureBuffer texBuffer = upload.sourceTexture->CreateTexBuffer(0, CTF_ProcessData);
	if (texBuffer.mBuffer == nullptr || texBuffer.mWidth <= 0 || texBuffer.mHeight <= 0)
	{
		return false;
	}

	outWidth = (uint32_t)texBuffer.mWidth;
	outHeight = (uint32_t)texBuffer.mHeight;
	outPixels.assign(texBuffer.mBuffer, texBuffer.mBuffer + (size_t)texBuffer.mWidth * (size_t)texBuffer.mHeight * 4u);
	return !outPixels.empty();
}

uint64_t EstimateMaterialBridgeBytes(const MaterialBridgeData& materials)
{
	uint64_t uniquePayloadBytes = materials.paletteLookup.uniqueStorageBytes();
	for (const TextureUpload& texture : materials.textures)
	{
		uniquePayloadBytes += texture.pixels.uniqueStorageBytes();
	}
	// Shared palette/indexed bytes are owned by the process-wide immutable caches.
	// Charging them to every material resource would multiply residency pressure by
	// the variant count. This estimate represents incremental bridge ownership.
	return
		(uint64_t)materials.materials.size() * sizeof(MaterialData) +
		(uint64_t)materials.lightMetadata.size() * sizeof(MaterialLightingMetadata) +
		(uint64_t)materials.textures.size() * sizeof(TextureUpload) +
		uniquePayloadBytes;
}

void ClearMaterialBridgeRetainingCapacity(MaterialBridgeData& materials)
{
	materials.materials.clear();
	materials.lightMetadata.clear();
	materials.textures.clear();
	materials.paletteLookup.clear();
	materials.paletteWidth = 256;
	materials.paletteHeight = 256;
	materials.buildStats = {};
}

void AppendMaterialBridge(const MaterialBridgeData& source, MaterialBridgeData& destination)
{
	std::unordered_map<uint64_t, uint32_t> textureLookup;
	textureLookup.reserve(destination.textures.size() + source.textures.size());
	for (uint32_t i = 0; i < (uint32_t)destination.textures.size(); ++i)
	{
		textureLookup.emplace(destination.textures[i].key, i);
	}
	AppendMaterialBridge(source, destination, textureLookup);
}

void AppendMaterialBridge(
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

void BuildMaterials(const SceneView& sceneView, MaterialBridgeData& outMaterials)
{
	outMaterials = {};
	std::unordered_map<uint64_t, uint32_t> textureLookup;
	const auto materialRowsStart = std::chrono::steady_clock::now();

	for (const SurfaceRef& wall : sceneView.opaqueWalls)
	{
		AppendSurfaceMaterial(wall, textureLookup, outMaterials);
	}

	for (const SurfaceRef& flat : sceneView.opaqueFlats)
	{
		AppendSurfaceMaterial(flat, textureLookup, outMaterials);
	}

	for (const SurfaceRef& sprite : sceneView.opaqueSprites)
	{
		AppendSurfaceMaterial(sprite, textureLookup, outMaterials);
	}
	outMaterials.buildStats.materialRowsMs = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - materialRowsStart).count();

	const auto paletteStart = std::chrono::steady_clock::now();
	const bool paletteBuilt = BuildPaletteLookup(outMaterials);
	outMaterials.buildStats.paletteMs = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - paletteStart).count();
	outMaterials.buildStats.paletteBuilds = paletteBuilt ? 1u : 0u;
	outMaterials.buildStats.paletteReuses = paletteBuilt ? 0u : 1u;
	outMaterials.buildStats.paletteBytesBuilt = paletteBuilt ? outMaterials.paletteLookup.size() : 0u;
}
}
