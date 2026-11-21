#include "nri_material_bridge.h"
#include "nri_material_color_audit.h"
#include "nri_material_average_color_cache.h"
#include "nri_mutation_pipeline_audit.h"
#include "nri_scene_product_layout.h"
#include "nri_material_palette.h"
#include "../../../engine/perf_audit.h"
#include "nri_sector_color_policy.h"
#include "nri_voxel_palette_policy.h"
#include "../renderer/nri_cvars.h"

#include "nri_hash.h"
#include "nri_indexed_mip_chain.h"
#include "nri_texture_signature.h"

#include "palette.h"
#include "tiletexture.h"
#include "textures.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <unordered_map>

FGameTexture* FindHightileForRT(FTextureID texid, int palnum, int& outTintFlags, uint32_t& outTintRGB); // core/textures/hightile.cpp


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

	struct IndexedMipVariantIdentity
	{
		uint64_t rawContentKey = 0;
		uint64_t palettePayloadSignature = 0;
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t paletteIndex = 0;
		uint32_t alphaPolicy = 0;
		uint32_t algorithmVersion = PaletteAwareIndexedMipAlgorithmVersion;

		bool operator==(const IndexedMipVariantIdentity& other) const
		{
			return rawContentKey == other.rawContentKey &&
				palettePayloadSignature == other.palettePayloadSignature &&
				width == other.width &&
				height == other.height &&
				paletteIndex == other.paletteIndex &&
				alphaPolicy == other.alphaPolicy &&
				algorithmVersion == other.algorithmVersion;
		}
	};

	struct IndexedMipVariantIdentityHash
	{
		size_t operator()(const IndexedMipVariantIdentity& identity) const
		{
			return (size_t)MakePaletteAwareIndexedMipKey(
				identity.rawContentKey,
				identity.palettePayloadSignature,
				identity.width,
				identity.height,
				identity.paletteIndex,
				identity.alphaPolicy != 0);
		}
	};

	struct IndexedMipVariantCacheEntry
	{
		uint64_t contentKey = 0;
		uint32_t mipCount = 1;
		ImmutableBytePayload pixels;
	};

	std::unordered_map<IndexedMipVariantIdentity, IndexedMipVariantCacheEntry, IndexedMipVariantIdentityHash> gIndexedMipVariantCache;
	std::mutex gIndexedMipVariantCacheMutex;

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

	bool ResolvePaletteAwareIndexedMipVariant(
		TextureUpload& upload,
		uint32_t paletteIndex,
		bool alphaClip,
		MaterialBridgeData& outMaterials)
	{
		const auto start = std::chrono::steady_clock::now();
		const size_t mip0Size = (size_t)upload.width * upload.height;
		if (!upload.indexed || upload.width == 0 || upload.height == 0 ||
			upload.pixels.size() < mip0Size ||
			outMaterials.paletteWidth < 256u ||
			paletteIndex >= outMaterials.paletteHeight ||
			outMaterials.paletteLookup.size() <
				(size_t)outMaterials.paletteWidth * outMaterials.paletteHeight * 4u)
		{
			outMaterials.buildStats.indexedMipMs += std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - start).count();
			return false;
		}

		const IndexedMipVariantIdentity identity = {
			upload.key,
			outMaterials.paletteLookup.signature(),
			upload.width,
			upload.height,
			paletteIndex,
			alphaClip ? 1u : 0u,
			PaletteAwareIndexedMipAlgorithmVersion,
		};
		IndexedMipVariantCacheEntry cached = {};
		{
			std::lock_guard<std::mutex> lock(gIndexedMipVariantCacheMutex);
			const auto found = gIndexedMipVariantCache.find(identity);
			if (found != gIndexedMipVariantCache.end())
			{
				cached = found->second;
			}
		}
		if (!cached.pixels.empty())
		{
			upload.key = cached.contentKey;
			upload.mipCount = cached.mipCount;
			upload.pixels = cached.pixels;
			outMaterials.buildStats.indexedMipVariants++;
			outMaterials.buildStats.indexedMipReuses++;
			outMaterials.buildStats.indexedMipVariantBytes += upload.pixels.size();
			outMaterials.buildStats.indexedMipMs += std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - start).count();
			return true;
		}

		const uint8_t* paletteRow = outMaterials.paletteLookup.data() +
			(size_t)paletteIndex * outMaterials.paletteWidth * 4u;
		PaletteAwareIndexedMipChain chain = {};
		if (!BuildPaletteAwareIndexedMipChain(
			upload.pixels.data(),
			upload.width,
			upload.height,
			paletteRow,
			outMaterials.paletteWidth,
			alphaClip,
			chain))
		{
			outMaterials.buildStats.indexedMipMs += std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - start).count();
			return false;
		}

		IndexedMipVariantCacheEntry generated = {};
		generated.contentKey = MakePaletteAwareIndexedMipKey(
			identity.rawContentKey,
			identity.palettePayloadSignature,
			identity.width,
			identity.height,
			identity.paletteIndex,
			alphaClip);
		generated.mipCount = chain.mipCount;
		generated.pixels.Assign(std::move(chain.pixels), generated.contentKey);

		bool built = false;
		{
			std::lock_guard<std::mutex> lock(gIndexedMipVariantCacheMutex);
			const auto found = gIndexedMipVariantCache.find(identity);
			if (found != gIndexedMipVariantCache.end())
			{
				cached = found->second;
			}
			else
			{
				gIndexedMipVariantCache.emplace(identity, generated);
				cached = generated;
				built = true;
			}
		}

		upload.key = cached.contentKey;
		upload.mipCount = cached.mipCount;
		upload.pixels = cached.pixels;
		outMaterials.buildStats.indexedMipVariants++;
		outMaterials.buildStats.indexedMipBuilds += built ? 1u : 0u;
		outMaterials.buildStats.indexedMipReuses += built ? 0u : 1u;
		outMaterials.buildStats.indexedMipVariantBytes += upload.pixels.size();
		outMaterials.buildStats.indexedMipMs += std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - start).count();
		return true;
	}

	uint32_t EnsureTextureUploadIndex(
		FGameTexture* texture,
		bool indexed,
		uint32_t paletteIndex,
		uint32_t materialFlags,
		bool allowPaletteMips,
		std::unordered_map<uint64_t, uint32_t>& textureLookup,
		MaterialBridgeData& outMaterials)
	{
		material_color_audit::Add(material_color_audit::Counter::TextureLookupRequests);
		const bool paletteMipped =
			allowPaletteMips && indexed && (materialFlags & MaterialFlag_PointSampled) == 0;
		const bool alphaClip = (materialFlags & MaterialFlag_AlphaClip) != 0;
		const uint64_t sourceLookupKey = MakeTextureKey(texture, indexed);
		const uint64_t textureKey = paletteMipped ?
			MakePaletteAwareIndexedMipKey(
				sourceLookupKey,
				outMaterials.paletteLookup.signature(),
				0,
				0,
				paletteIndex,
				alphaClip) :
			sourceLookupKey;
		auto it = textureLookup.find(textureKey);
		if (it == textureLookup.end())
		{
			const uint32_t textureIndex = (uint32_t)outMaterials.textures.size();
			const size_t previousBuckets = material_color_audit::Current != nullptr ? textureLookup.bucket_count() : 0;
			textureLookup.emplace(textureKey, textureIndex);
			if (material_color_audit::Current != nullptr && textureLookup.bucket_count() != previousBuckets)
				material_color_audit::Add(material_color_audit::Counter::TextureLookupBucketGrowths);
			TextureUpload upload = BuildTextureUpload(texture, indexed, outMaterials.buildStats);
			if (paletteMipped)
			{
				ResolvePaletteAwareIndexedMipVariant(upload, paletteIndex, alphaClip, outMaterials);
			}
			outMaterials.textures.push_back(std::move(upload));
			return textureIndex;
		}

		return it->second;
	}

	uint32_t EnsureTextureUploadIndex(FTexture* texture, bool indexed, std::unordered_map<uint64_t, uint32_t>& textureLookup, MaterialBridgeData& outMaterials)
	{
		material_color_audit::Add(material_color_audit::Counter::TextureLookupRequests);
		const uint64_t textureKey = MakeTextureKey(texture, indexed);
		auto it = textureLookup.find(textureKey);
		if (it == textureLookup.end())
		{
			const uint32_t textureIndex = (uint32_t)outMaterials.textures.size();
			const size_t previousBuckets = material_color_audit::Current != nullptr ? textureLookup.bucket_count() : 0;
			textureLookup.emplace(textureKey, textureIndex);
			if (material_color_audit::Current != nullptr && textureLookup.bucket_count() != previousBuckets)
				material_color_audit::Add(material_color_audit::Counter::TextureLookupBucketGrowths);
			outMaterials.textures.push_back(BuildTextureUpload(texture, indexed, outMaterials.buildStats));
			return textureIndex;
		}

		return it->second;
	}

	// DEF replacements carry effects Raze applies in its GL shader (main.fp:
	// ApplyTextureManipulation for `tint`, TEXF_Detailmap for `detail`). The RT
	// shaders have neither, so they are baked into the uploaded BGRA pixels here.
	// The RT also has no specular input, so a DEF specular map becomes roughness.
	struct ReplacementBake
	{
		int tintFlags = -1;
		uint32_t tintRGB = 0xffffff;
		FTexture* detail = nullptr;
		float detailScale[2] = { 1.0f, 1.0f };
		bool specularToRoughness = false;

		bool active() const { return tintFlags != -1 || detail != nullptr || specularToRoughness; }

		uint64_t key() const
		{
			uint64_t k = HashCombine64(0x52544842414b45ull, (uint64_t)(uint32_t)tintFlags << 32 | tintRGB);
			k = HashCombine64(k, (uint64_t)(uintptr_t)detail);
			uint32_t sx, sy;
			memcpy(&sx, &detailScale[0], 4);
			memcpy(&sy, &detailScale[1], 4);
			k = HashCombine64(k, (uint64_t)sx << 32 | sy);
			return HashCombine64(k, specularToRoughness ? 1u : 0u);
		}
	};

	// Raze main.fp ApplyTextureManipulation, with the uniforms PreBindTexture
	// derives from a tint (basepal tint left at white). c is RGB in [0,1].
	void ApplyTint(float c[3], int flags, uint32_t tintRGB)
	{
		float mod[3] = { 1.0f, 1.0f, 1.0f };
		if (flags & TINTF_COLORIZE)
		{
			mod[0] = ((tintRGB >> 16) & 255) / 64.0f;
			mod[1] = ((tintRGB >> 8) & 255) / 64.0f;
			mod[2] = (tintRGB & 255) / 64.0f;
		}
		const float desat = (flags & TINTF_GRAYSCALE) ? 1.0f : 0.0f;
		const float gray = c[0] * 0.3f + c[1] * 0.56f + c[2] * 0.14f;
		for (int i = 0; i < 3; i++)
		{
			c[i] += (gray - c[i]) * desat;
			if (flags & TINTF_INVERT) c[i] = 1.0f - c[i];
			c[i] = clamp(c[i] * mod[i], 0.0f, 1.0f);
		}

		const int blend = (flags & TINTF_BLENDMASK) ? ((((flags & TINTF_BLENDMASK) >> 6) + 1) & 7) : 0;
		if (blend == 0) return;
		float t[3], tc[3];
		for (int i = 0; i < 3; i++) { t[i] = mod[i] * 255.0f; tc[i] = c[i] * 255.0f; }
		float out[3];
		switch (blend)
		{
		default: // alpha blend; the blend alpha is the desaturation factor, as in Raze
			for (int i = 0; i < 3; i++) out[i] = tc[i] * (1.0f - desat) + t[i] * desat;
			break;
		case 2: // screen; Raze swaps the r/b tint channels here, kept for parity
			out[2] = 255.0f - (((255.0f - tc[2]) * (255.0f - t[0])) / 256.0f);
			out[1] = 255.0f - (((255.0f - tc[1]) * (255.0f - t[1])) / 256.0f);
			out[0] = 255.0f - (((255.0f - tc[0]) * (255.0f - t[2])) / 256.0f);
			break;
		case 3: // overlay
			for (int i = 0; i < 3; i++)
				out[i] = tc[i] < 128.0f ? (tc[i] * t[i]) / 128.0f : 255.0f - (((255.0f - tc[i]) * (255.0f - t[i])) / 128.0f);
			break;
		case 4: // hardlight
			for (int i = 0; i < 3; i++)
				out[i] = t[i] < 128.0f ? (tc[i] * t[i]) / 128.0f : 255.0f - (((255.0f - tc[i]) * (255.0f - t[i])) / 128.0f);
			break;
		}
		for (int i = 0; i < 3; i++) c[i] = out[i] / 255.0f;
	}

	void ApplyBake(uint8_t* bgra, uint32_t width, uint32_t height, const ReplacementBake& bake)
	{
		std::vector<uint8_t> detail;
		int dw = 0, dh = 0;
		if (bake.detail != nullptr)
		{
			FTextureBuffer db = bake.detail->CreateTexBuffer(0, CTF_ProcessData);
			if (db.mBuffer != nullptr && db.mWidth > 0 && db.mHeight > 0)
			{
				dw = db.mWidth;
				dh = db.mHeight;
				detail.assign(db.mBuffer, db.mBuffer + (size_t)dw * dh * 4u);
			}
		}

		for (uint32_t y = 0; y < height; y++)
		{
			for (uint32_t x = 0; x < width; x++)
			{
				uint8_t* p = bgra + ((size_t)y * width + x) * 4u;
				float c[3] = { p[2] / 255.0f, p[1] / 255.0f, p[0] / 255.0f };

				if (bake.specularToRoughness)
				{
					const float r = clamp(1.0f - (c[0] * 0.3f + c[1] * 0.56f + c[2] * 0.14f), 0.02f, 1.0f);
					c[0] = c[1] = c[2] = r;
					p[3] = 255;
				}
				if (!detail.empty())
				{
					// main.fp: Base.rgb *= texture(detail, uv * scale) * 2 (bias 2, from PreBindTexture)
					const float u = (x + 0.5f) / width * bake.detailScale[0] * dw - 0.5f;
					const float v = (y + 0.5f) / height * bake.detailScale[1] * dh - 0.5f;
					const float fu = floorf(u), fv = floorf(v);
					const float ax = u - fu, ay = v - fv;
					auto texel = [&](int tx, int ty, int ch)
					{
						tx = ((tx % dw) + dw) % dw;
						ty = ((ty % dh) + dh) % dh;
						return detail[((size_t)ty * dw + tx) * 4u + ch] / 255.0f;
					};
					for (int i = 0; i < 3; i++)
					{
						const int ch = 2 - i; // BGRA
						const float d =
							(texel((int)fu, (int)fv, ch) * (1 - ax) + texel((int)fu + 1, (int)fv, ch) * ax) * (1 - ay) +
							(texel((int)fu, (int)fv + 1, ch) * (1 - ax) + texel((int)fu + 1, (int)fv + 1, ch) * ax) * ay;
						c[i] = clamp(c[i] * d * 2.0f, 0.0f, 1.0f);
					}
				}
				if (bake.tintFlags != -1)
				{
					ApplyTint(c, bake.tintFlags, bake.tintRGB);
				}

				p[2] = (uint8_t)(c[0] * 255.0f + 0.5f);
				p[1] = (uint8_t)(c[1] * 255.0f + 0.5f);
				p[0] = (uint8_t)(c[2] * 255.0f + 0.5f);
			}
		}
	}

	uint32_t ComputeFullMipCount(uint32_t width, uint32_t height)
	{
		uint32_t count = 1;
		while (width > 1 || height > 1)
		{
			width = (std::max)(1u, width / 2u);
			height = (std::max)(1u, height / 2u);
			count++;
		}
		return count;
	}

	// Appends mips 1..mipCount-1 after mip 0 (tightly packed BGRA, the layout
	// BuildSceneTextureSubresources expects; level sizes halve with max(1, n / 2)).
	// 2x2 box filter weighted by alpha, so fully transparent texels (often black)
	// don't darken the edges of cut-out textures like fences.
	void AppendBgraMipChain(std::vector<uint8_t>& pixels, uint32_t width, uint32_t height, uint32_t mipCount)
	{
		size_t srcOffset = 0;
		uint32_t srcWidth = width;
		uint32_t srcHeight = height;
		for (uint32_t mip = 1; mip < mipCount; mip++)
		{
			const uint32_t dstWidth = (std::max)(1u, srcWidth / 2u);
			const uint32_t dstHeight = (std::max)(1u, srcHeight / 2u);
			const size_t dstOffset = pixels.size();
			pixels.resize(dstOffset + (size_t)dstWidth * dstHeight * 4u);
			for (uint32_t y = 0; y < dstHeight; y++)
			{
				for (uint32_t x = 0; x < dstWidth; x++)
				{
					float weighted[3] = { 0.0f, 0.0f, 0.0f };
					float plain[3] = { 0.0f, 0.0f, 0.0f };
					float alphaSum = 0.0f;
					for (uint32_t dy = 0; dy < 2; dy++)
					{
						for (uint32_t dx = 0; dx < 2; dx++)
						{
							const uint32_t sx = (std::min)(x * 2u + dx, srcWidth - 1u);
							const uint32_t sy = (std::min)(y * 2u + dy, srcHeight - 1u);
							const uint8_t* s = &pixels[srcOffset + ((size_t)sy * srcWidth + sx) * 4u];
							const float a = s[3] / 255.0f;
							for (int c = 0; c < 3; c++)
							{
								weighted[c] += s[c] * a;
								plain[c] += s[c];
							}
							alphaSum += a;
						}
					}
					uint8_t* d = &pixels[dstOffset + ((size_t)y * dstWidth + x) * 4u];
					for (int c = 0; c < 3; c++)
					{
						const float v = alphaSum > 0.0f ? weighted[c] / alphaSum : plain[c] * 0.25f;
						d[c] = (uint8_t)clamp(v + 0.5f, 0.0f, 255.0f);
					}
					d[3] = (uint8_t)clamp(alphaSum * 0.25f * 255.0f + 0.5f, 0.0f, 255.0f);
				}
			}
			srcOffset = dstOffset;
			srcWidth = dstWidth;
			srcHeight = dstHeight;
		}
	}
}

namespace nri_scene
{
	struct TextureUploadTransform
	{
		ReplacementBake bake;
	};
}

namespace
{
	// DEF replacement uploads (bake and/or mip chain) are deferred like ordinary
	// true-color textures: the key identifies the result without decoding, and
	// RealizeTextureUploadPayload does the work only on a GPU texture cache miss.
	// (Doing it per BuildMaterials call, which runs often, made HRP scenes crawl.)
	uint32_t EnsureTransformedTextureUploadIndex(FTexture* texture, const ReplacementBake& bake, bool generateMips, std::unordered_map<uint64_t, uint32_t>& textureLookup, MaterialBridgeData& outMaterials)
	{
		const uint64_t lookupKey = HashCombine64(MakeTextureKey(texture, false) ^ bake.key(), generateMips ? 0x4d4950u : 0u);
		auto it = textureLookup.find(lookupKey);
		if (it != textureLookup.end())
		{
			return it->second;
		}

		const uint32_t textureIndex = (uint32_t)outMaterials.textures.size();
		textureLookup.emplace(lookupKey, textureIndex);
		TextureUpload upload = {};
		upload.sourceTexture = texture;
		upload.key = lookupKey;
		upload.indexed = false;
		if (texture != nullptr && texture->GetWidth() > 0 && texture->GetHeight() > 0)
		{
			upload.width = (uint32_t)texture->GetWidth();
			upload.height = (uint32_t)texture->GetHeight();
			upload.mipCount = generateMips ? ComputeFullMipCount(upload.width, upload.height) : 1u;
		}
		auto transform = std::make_shared<TextureUploadTransform>();
		transform->bake = bake;
		upload.transform = std::move(transform);
		outMaterials.textures.push_back(std::move(upload));
		return textureIndex;
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
		Fnv1a64Append(key, &metadata.authoredTextureId, sizeof(metadata.authoredTextureId));
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
		metadata.paletteIndex = (uint32_t)clamp(materialRef.palette, 0, MAXPALOOKUPS - 1);
		metadata.materialFlags = material.flags;
		metadata.authoredTextureId = materialRef.authoredTextureId >= 0 ? (uint32_t)materialRef.authoredTextureId : UINT32_MAX;
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
			// A chosen glow layer counts too: a DEF replacement's glow map on a tile
			// that has no Duke-RT glow overlay of its own.
			if (lightingTexture->GetGlowmap() != nullptr || glowmapTextureIndex != UINT32_MAX)
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
		material.paletteIndex = ResolveSectorRenderPalette((uint32_t)clamp(materialRef.palette, 0, MAXPALOOKUPS - 1),
			surface.provenance.sourceType, surface.provenance.sectorIndex, materialRef.flags);
		material.lightLevel = ComputeLightLevel(materialRef);
		material.alpha = materialRef.alpha;
		material.roughnessHint = ComputeRoughnessHint(materialRef, material.lightLevel);
		material.metalnessHint = ComputeMetalnessHint(materialRef);
		material.materialClass = ComputeMaterialClass(materialRef);

		bool indexed = (materialRef.flags & MaterialFlag_Indexed) != 0;
		// Voxel palette policies expand this surface into many material rows which
		// currently share one texture index. Keep that texture single-mip until the
		// policy rows can own palette-specific indexed mip variants.
		const bool allowPaletteMips = materialRef.voxelPalettePolicy == nullptr;
		// DEF `texture` replacement: upload the true-color image instead of the ART
		// tile. UVs are normalized to the tile, so any resolution maps 1:1. Lighting
		// rules below still key off the original tile (materialRef.texture).
		FGameTexture* baseColorTexture = materialRef.texture;
		FGameTexture* hightile = nullptr;
		ReplacementBake bake;
		if (indexed && allowPaletteMips && materialRef.texture != nullptr)
		{
			if (FGameTexture* replacement = FindHightileForRT(materialRef.texture->GetID(), (int)material.paletteIndex, bake.tintFlags, bake.tintRGB))
			{
				baseColorTexture = replacement;
				hightile = replacement;
				indexed = false;
				material.flags &= ~MaterialFlag_Indexed;
				if (replacement->GetDetailmap() != nullptr)
				{
					bake.detail = replacement->GetDetailmap();
					bake.detailScale[0] = replacement->GetDetailScale().X;
					bake.detailScale[1] = replacement->GetDetailScale().Y;
				}
			}
		}
		// Each auxiliary layer comes from the DEF replacement when it has one, else
		// from the tile's own Duke-RT material overlays. UVs are normalized to the
		// tile, so overlays authored for the ART tile line up with any replacement.
		// (All-or-nothing selection dropped e.g. Duke-RT glow maps on tiles whose
		// HRP replacement only adds normal/specular layers.)
		FTexture* const tileGlow = materialRef.texture != nullptr ? materialRef.texture->GetGlowmap() : nullptr;
		FTexture* const tileMetallic = materialRef.texture != nullptr ? materialRef.texture->GetMetallic() : nullptr;
		FTexture* const tileNormal = materialRef.texture != nullptr ? materialRef.texture->GetNormalmap() : nullptr;
		FTexture* const tileRoughness = materialRef.texture != nullptr ? materialRef.texture->GetRoughness() : nullptr;
		// nri_hightiledeflayers picks the winner when both sources have a layer.
		const bool preferDef = nri_hightiledeflayers;
		auto pick = [preferDef](FTexture* fromDef, FTexture* fromTile) -> FTexture*
		{
			if (preferDef) return fromDef != nullptr ? fromDef : fromTile;
			return fromTile != nullptr ? fromTile : fromDef;
		};
		FTexture* const glowLayer = pick(hightile != nullptr ? hightile->GetGlowmap() : nullptr, tileGlow);
		FTexture* const metallicLayer = pick(hightile != nullptr ? hightile->GetMetallic() : nullptr, tileMetallic);
		FTexture* const normalLayer = pick(hightile != nullptr ? hightile->GetNormalmap() : nullptr, tileNormal);
		// Roughness: authored roughness (DEF or Duke-RT, per the cvar), else a DEF
		// specular map converted (1 - luminance) as the last resort.
		FTexture* const roughnessLayer = pick(hightile != nullptr ? hightile->GetRoughness() : nullptr, tileRoughness);
		FTexture* const specularForRoughness = roughnessLayer == nullptr && hightile != nullptr ? hightile->GetSpecularmap() : nullptr;
		// Replacements get a mip chain (indexed tiles get palette-aware mips); the
		// primary hit picks a LOD from the texture's own mip count.
		material.textureIndex = hightile != nullptr ?
			EnsureTransformedTextureUploadIndex(baseColorTexture->GetTexture(), bake, (material.flags & MaterialFlag_PointSampled) == 0, textureLookup, outMaterials) :
			EnsureTextureUploadIndex(
				baseColorTexture,
				indexed,
				material.paletteIndex,
				material.flags,
				allowPaletteMips,
				textureLookup,
				outMaterials);

		material.sectorIndex = surface.provenance.sectorIndex >= 0 ? (uint32_t)surface.provenance.sectorIndex : UINT32_MAX;
		material.wallIndex = surface.provenance.wallIndex >= 0 ? (uint32_t)surface.provenance.wallIndex : UINT32_MAX;
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
		if (!inheritedEmissiveSource && glowLayer != nullptr)
		{
			glowmapTextureIndex = EnsureTextureUploadIndex(glowLayer, false, textureLookup, outMaterials);
			glowmapContentKey = outMaterials.textures[glowmapTextureIndex].key;
		}
		else if (inheritedEmissiveSource && lightingTexture != nullptr && lightingTexture->GetGlowmap() != nullptr)
		{
			glowmapTextureIndex = material.textureIndex;
			glowmapContentKey = outMaterials.textures[material.textureIndex].key;
		}
		if (useAuxiliaryMaps && metallicLayer != nullptr)
		{
			metallicTextureIndex = EnsureTextureUploadIndex(metallicLayer, false, textureLookup, outMaterials);
			metallicContentKey = outMaterials.textures[metallicTextureIndex].key;
			outMaterials.materials.back().metallicTextureIndex = metallicTextureIndex;
		}
		if (useAuxiliaryMaps && normalLayer != nullptr)
		{
			normalTextureIndex = EnsureTextureUploadIndex(normalLayer, false, textureLookup, outMaterials);
			normalContentKey = outMaterials.textures[normalTextureIndex].key;
			outMaterials.materials.back().normalTextureIndex = normalTextureIndex;
		}
		if (useAuxiliaryMaps && roughnessLayer != nullptr)
		{
			roughnessTextureIndex = EnsureTextureUploadIndex(roughnessLayer, false, textureLookup, outMaterials);
			roughnessContentKey = outMaterials.textures[roughnessTextureIndex].key;
			outMaterials.materials.back().roughnessTextureIndex = roughnessTextureIndex;
		}
		else if (useAuxiliaryMaps && specularForRoughness != nullptr)
		{
			// DEF specular map, from a replacement only (Duke-RT's own specular overlays
			// stay unused as before): roughness = 1 - specular luminance.
			ReplacementBake specBake;
			specBake.specularToRoughness = true;
			roughnessTextureIndex = EnsureTransformedTextureUploadIndex(specularForRoughness, specBake, false, textureLookup, outMaterials);
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
			PerfAuditAdd(PerfAuditMetric::VoxelPolicyRowsBuilt, VoxelPalettePolicyEntryCount);
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


}

namespace nri_scene
{
bool RealizeTextureUploadPayload(const TextureUpload& upload, std::vector<uint8_t>& outPixels, uint32_t& outWidth, uint32_t& outHeight)
{
	outPixels.clear();
	outWidth = 0;
	outHeight = 0;

	if (upload.transform != nullptr)
	{
		// DEF replacement: decode, bake (tint/detail/specular->roughness), mips.
		if (upload.sourceTexture == nullptr)
		{
			return false;
		}
		FTextureBuffer texBuffer = upload.sourceTexture->CreateTexBuffer(0, CTF_ProcessData);
		if (texBuffer.mBuffer == nullptr || texBuffer.mWidth <= 0 || texBuffer.mHeight <= 0)
		{
			return false;
		}
		const uint32_t width = (uint32_t)texBuffer.mWidth;
		const uint32_t height = (uint32_t)texBuffer.mHeight;
		// The uploader sizes the mip chain from upload.width/height/mipCount.
		if (upload.mipCount > 1 && (width != upload.width || height != upload.height))
		{
			return false;
		}
		outPixels.assign(texBuffer.mBuffer, texBuffer.mBuffer + (size_t)width * height * 4u);
		if (upload.transform->bake.active())
		{
			ApplyBake(outPixels.data(), width, height, upload.transform->bake);
		}
		if (upload.mipCount > 1)
		{
			AppendBgraMipChain(outPixels, width, height, upload.mipCount);
		}
		outWidth = width;
		outHeight = height;
		return !outPixels.empty();
	}

	// Palette-aware mip variants are immutable packed payloads. Recreating only
	// mip 0 from the source texture would silently change the upload contract.
	if (upload.mipCount > 1)
	{
		return false;
	}

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


void BuildMaterials(const SceneView& sceneView, MaterialBridgeData& outMaterials, MaterialProductLayout* layout)
{
	mutation_audit::Scope category(mutation_audit::Category::Material);
	material_color_audit::Scope colorAudit("direct", true);
	ScopedMaterialAverageColorCache averageColorCache;
	const size_t previousMaterials = material_color_audit::Current != nullptr ? outMaterials.materials.capacity() : 0;
	const size_t previousMetadata = material_color_audit::Current != nullptr ? outMaterials.lightMetadata.capacity() : 0;
	const size_t previousTextures = material_color_audit::Current != nullptr ? outMaterials.textures.capacity() : 0;
	ClearMaterialBridgeRetainingCapacity(outMaterials);
	const auto paletteStart = std::chrono::steady_clock::now();
	const bool paletteBuilt = ResolveMaterialPalette(outMaterials);
	outMaterials.buildStats.paletteMs = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - paletteStart).count();
	outMaterials.buildStats.paletteBuilds = paletteBuilt ? 1u : 0u;
	outMaterials.buildStats.paletteReuses = paletteBuilt ? 0u : 1u;
	outMaterials.buildStats.paletteBytesBuilt = paletteBuilt ? outMaterials.paletteLookup.size() : 0u;

	std::unordered_map<uint64_t, uint32_t> textureLookup;
	const auto materialRowsStart = std::chrono::steady_clock::now();
	if (layout != nullptr) layout->Clear();
	const auto endGroup = [&](size_t index)
	{
		if (layout == nullptr) return;
		auto& group = layout->groups[index];
		group.materials.count = outMaterials.materials.size() - group.materials.first;
		group.textures.count = outMaterials.textures.size() - group.textures.first;
		if (index + 1 < layout->groups.size())
		{
			layout->groups[index + 1].materials.first = outMaterials.materials.size();
			layout->groups[index + 1].textures.first = outMaterials.textures.size();
		}
	};

	for (const SurfaceRef& wall : sceneView.opaqueWalls)
	{
		AppendSurfaceMaterial(wall, textureLookup, outMaterials);
	}
	endGroup(0);

	for (const SurfaceRef& flat : sceneView.opaqueFlats)
	{
		AppendSurfaceMaterial(flat, textureLookup, outMaterials);
	}
	endGroup(1);

	for (const SurfaceRef& sprite : sceneView.opaqueSprites)
	{
		AppendSurfaceMaterial(sprite, textureLookup, outMaterials);
	}
	endGroup(2);
	if (layout != nullptr)
	{
		layout->textureLookupKeys.resize(outMaterials.textures.size());
		for (const auto& entry : textureLookup) layout->textureLookupKeys[entry.second] = entry.first;
		layout->valid = true;
	}
	if (material_color_audit::Current != nullptr)
	{
		using namespace material_color_audit;
		Add(Counter::TextureLookupEntries, textureLookup.size());
		Add(Counter::MaterialVectorGrowths, outMaterials.materials.capacity() > previousMaterials);
		Add(Counter::MetadataVectorGrowths, outMaterials.lightMetadata.capacity() > previousMetadata);
		Add(Counter::TextureVectorGrowths, outMaterials.textures.capacity() > previousTextures);
		Add(Counter::CapacityGrowthBytes, (outMaterials.materials.capacity() - previousMaterials) * sizeof(MaterialData) +
			(outMaterials.lightMetadata.capacity() - previousMetadata) * sizeof(MaterialLightingMetadata) +
			(outMaterials.textures.capacity() - previousTextures) * sizeof(TextureUpload));
	}
	outMaterials.buildStats.materialRowsMs = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - materialRowsStart).count();
}
}
