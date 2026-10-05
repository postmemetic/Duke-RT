#include "nri_texture_signature.h"

#include "nri_hash.h"
#include "nri_scene_texture_utils.h"

#include "image.h"
#include "skyboxtexture.h"
#include "textures.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
	using namespace nri_scene;

	constexpr int kMaxTextureSignatureDepth = 4;

	FTexture* TryResolveBaseTextureUnchecked(FGameTexture* texture)
	{
		if (texture == nullptr)
		{
			return nullptr;
		}

		FTexture* baseTexture = nullptr;
#ifdef _WIN32
		__try
#endif
		{
			baseTexture = texture->GetTexture();
		}
#ifdef _WIN32
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			baseTexture = nullptr;
		}
#endif

		return baseTexture;
	}

	FTexture* TryResolveBaseTexture(FGameTexture* texture)
	{
		return IsUsableGameTexturePointer(texture) ? TryResolveBaseTextureUnchecked(texture) : nullptr;
	}

	bool IsTexturePersistentSignatureEligibleUnchecked(FGameTexture* texture, FTexture* baseTexture)
	{
		if (texture == nullptr)
		{
			return false;
		}

		// Keep wrapper-level mutable cases explicit so callers do not accidentally
		// inherit persistence just because the wrapped base exposes an image ID.
		if (texture->isHardwareCanvas() || texture->isSoftwareCanvas() || texture->GetUseType() == ETextureType::SWCanvas)
		{
			return false;
		}

		return IsTexturePersistentSignatureEligible(baseTexture);
	}

	FGameTexture* TryGetSkyFace(FSkyBox* skybox, int index)
	{
		if (skybox == nullptr || index < 0 || index >= 6)
		{
			return nullptr;
		}

		FGameTexture* face = nullptr;
#ifdef _WIN32
		__try
#endif
		{
			face = skybox->GetSkyFace(index);
		}
#ifdef _WIN32
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			face = nullptr;
		}
#endif

		return IsUsableGameTexturePointer(face) ? face : nullptr;
	}

	FGameTexture* TryGetSkyPrevious(FSkyBox* skybox)
	{
		if (skybox == nullptr)
		{
			return nullptr;
		}

		FGameTexture* previous = nullptr;
#ifdef _WIN32
		__try
#endif
		{
			previous = skybox->previous;
		}
#ifdef _WIN32
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			previous = nullptr;
		}
#endif

		return IsUsableGameTexturePointer(previous) ? previous : nullptr;
	}

	uint64_t BuildRequestSalt(const TextureSignatureRequest& request)
	{
		uint64_t salt = 1469598103934665603ull;
		salt = HashCombine64(salt, (uint64_t)(uint8_t)request.contentKind);
		salt = HashCombine64(salt, (uint64_t)(uint32_t)std::max(request.translation, 0));
		salt = HashCombine64(salt, (uint64_t)request.flags);
		salt = HashCombine64(salt, (uint64_t)request.scaler);
		salt = HashCombine64(salt, (uint64_t)request.scaleFactor);
		return salt;
	}

	bool TryBuildSkyboxTextureSignatureImpl(FSkyBox* skybox, const TextureSignatureRequest& request, TextureSignature& outSignature, int depth, bool includeFlipTopInKey);
	bool TryBuildTextureSignatureImpl(FGameTexture* texture, const TextureSignatureRequest& request, TextureSignature& outSignature, int depth);
	bool TryBuildAverageColorTextureSignatureImpl(FGameTexture* texture, TextureSignature& outSignature, int depth);
}

namespace nri_scene
{
bool IsTexturePersistentSignatureEligible(FTexture* texture)
{
	if (texture == nullptr)
	{
		return false;
	}

	// Canvas-backed textures are runtime-mutable and should never be treated as
	// persistently stable across execution, even if metadata becomes richer later.
	if (texture->isCanvas() || texture->isHardwareCanvas() || dynamic_cast<FCanvasTexture*>(texture) != nullptr)
	{
		return false;
	}

	return texture->GetImage() != nullptr;
}

bool IsTexturePersistentSignatureEligible(FGameTexture* texture)
{
	FTexture* baseTexture = TryResolveBaseTexture(texture);
	if (baseTexture == nullptr)
	{
		return false;
	}

	return IsTexturePersistentSignatureEligibleUnchecked(texture, baseTexture);
}

bool CanBuildTextureSignatureFromMetadata(const TextureSignatureRequest& request)
{
	if ((request.flags & TextureSignatureRequestFlag_Upscale) != 0)
	{
		return false;
	}

	if (request.scaler != 0 || request.scaleFactor != 0)
	{
		return false;
	}

	return true;
}

bool TryBuildImageTextureSignature(FTexture* texture, const TextureSignatureRequest& request, TextureSignature& outSignature)
{
	outSignature = {};
	if (!CanBuildTextureSignatureFromMetadata(request) || texture == nullptr || texture->GetImage() == nullptr)
	{
		return false;
	}

	const uint32_t expand = (request.flags & TextureSignatureRequestFlag_Expand) != 0 ? 1u : 0u;
	const int width = texture->GetWidth() + (int)(expand * 2u);
	const int height = texture->GetHeight() + (int)(expand * 2u);
	if (width <= 0 || height <= 0)
	{
		return false;
	}

	FContentIdBuilder builder = {};
	builder.imageID = texture->GetImage()->GetId();
	builder.translation = (unsigned)std::max(request.translation, 0);
	builder.expand = expand;
	builder.scaler = request.scaler & 0x0fu;
	builder.scalefactor = request.scaleFactor & 0x0fu;

	uint64_t key = BuildRequestSalt(request);
	key = HashCombine64(key, (uint64_t)(uint8_t)TextureSignatureSourceKind::ImageBacked);
	key = HashCombine64(key, builder.id);
	key = HashCombine64(key, ((uint64_t)(uint32_t)width << 32) | (uint32_t)height);

	outSignature.valid = true;
	outSignature.metadataDerived = true;
	outSignature.persistentEligible = IsTexturePersistentSignatureEligible(texture);
	outSignature.sourceKind = TextureSignatureSourceKind::ImageBacked;
	outSignature.request = request;
	outSignature.key = key;
	outSignature.width = (uint32_t)width;
	outSignature.height = (uint32_t)height;
	return true;
}

bool TryBuildSkyboxTextureSignature(FGameTexture* texture, const TextureSignatureRequest& request, TextureSignature& outSignature)
{
	outSignature = {};
	FTexture* baseTexture = TryResolveBaseTexture(texture);
	auto* skybox = dynamic_cast<FSkyBox*>(baseTexture);
	if (skybox == nullptr)
	{
		return false;
	}

	const bool success = TryBuildSkyboxTextureSignatureImpl(skybox, request, outSignature, 0, true);
	if (success)
	{
		outSignature.persistentEligible = outSignature.persistentEligible && IsTexturePersistentSignatureEligibleUnchecked(texture, baseTexture);
	}
	return success;
}

bool TryBuildTextureSignature(FGameTexture* texture, const TextureSignatureRequest& request, TextureSignature& outSignature)
{
	if (!IsUsableGameTexturePointer(texture))
	{
		outSignature = {};
		return false;
	}

	return TryBuildTextureSignatureImpl(texture, request, outSignature, 0);
}

bool TryBuildAverageColorTextureSignature(FGameTexture* texture, TextureSignature& outSignature)
{
	if (!IsUsableGameTexturePointer(texture))
	{
		outSignature = {};
		return false;
	}

	return TryBuildAverageColorTextureSignatureImpl(texture, outSignature, 0);
}
}

namespace
{
	bool TryBuildSkyboxTextureSignatureImpl(FSkyBox* skybox, const TextureSignatureRequest& request, TextureSignature& outSignature, int depth, bool includeFlipTopInKey)
	{
		outSignature = {};
		if (!CanBuildTextureSignatureFromMetadata(request) || skybox == nullptr || depth > kMaxTextureSignatureDepth)
		{
			return false;
		}

		uint64_t key = BuildRequestSalt(request);
		key = HashCombine64(key, (uint64_t)(uint8_t)TextureSignatureSourceKind::SkyboxStructural);

		uint32_t faceMask = 0;
		bool hasContributingSource = false;
		bool persistentEligible = true;
		bool dimensionsInitialized = false;
		uint32_t width = 0;
		uint32_t height = 0;

		for (int i = 0; i < 6; ++i)
		{
			FGameTexture* face = TryGetSkyFace(skybox, i);
			if (face == nullptr)
			{
				continue;
			}

			TextureSignature faceSignature = {};
			const bool faceOk = includeFlipTopInKey ?
				TryBuildTextureSignatureImpl(face, request, faceSignature, depth + 1) :
				TryBuildAverageColorTextureSignatureImpl(face, faceSignature, depth + 1);
			if (!faceOk)
			{
				return false;
			}

			faceMask |= 1u << i;
			hasContributingSource = true;
			persistentEligible = persistentEligible && faceSignature.persistentEligible;
			if (!dimensionsInitialized)
			{
				width = faceSignature.width;
				height = faceSignature.height;
				dimensionsInitialized = true;
			}

			key = HashCombine64(key, (uint64_t)(uint32_t)i);
			key = HashCombine64(key, faceSignature.key);
			key = HashCombine64(key, ((uint64_t)faceSignature.width << 32) | faceSignature.height);
		}

		if (!hasContributingSource)
		{
			FGameTexture* previous = TryGetSkyPrevious(skybox);
			TextureSignature previousSignature = {};
			const bool previousOk = previous != nullptr && (includeFlipTopInKey ?
				TryBuildTextureSignatureImpl(previous, request, previousSignature, depth + 1) :
				TryBuildAverageColorTextureSignatureImpl(previous, previousSignature, depth + 1));
			if (!previousOk)
			{
				return false;
			}

			hasContributingSource = true;
			persistentEligible = persistentEligible && previousSignature.persistentEligible;
			width = previousSignature.width;
			height = previousSignature.height;
			dimensionsInitialized = true;
			key = HashCombine64(key, 0x70726576696f7573ull);
			key = HashCombine64(key, previousSignature.key);
			key = HashCombine64(key, ((uint64_t)previousSignature.width << 32) | previousSignature.height);
		}

		if (!hasContributingSource || !dimensionsInitialized)
		{
			return false;
		}

		const bool isThreeFace = skybox->Is3Face();
		const bool flipTop = skybox->GetSkyFlip();
		key = HashCombine64(key, (uint64_t)faceMask);
		key = HashCombine64(key, isThreeFace ? 1ull : 0ull);
		if (includeFlipTopInKey)
		{
			key = HashCombine64(key, flipTop ? 1ull : 0ull);
		}

		outSignature.valid = true;
		outSignature.metadataDerived = true;
		outSignature.persistentEligible = persistentEligible;
		outSignature.sourceKind = TextureSignatureSourceKind::SkyboxStructural;
		outSignature.request = request;
		outSignature.key = key;
		outSignature.width = width;
		outSignature.height = height;
		outSignature.faceMask = faceMask;
		outSignature.isThreeFace = isThreeFace;
		outSignature.flipTop = flipTop;
		return true;
	}

	bool TryBuildTextureSignatureImpl(FGameTexture* texture, const TextureSignatureRequest& request, TextureSignature& outSignature, int depth)
	{
		outSignature = {};
		if (depth > kMaxTextureSignatureDepth)
		{
			return false;
		}

		FTexture* baseTexture = TryResolveBaseTextureUnchecked(texture);
		if (baseTexture == nullptr)
		{
			return false;
		}

		if (auto* skybox = dynamic_cast<FSkyBox*>(baseTexture))
		{
			const bool success = TryBuildSkyboxTextureSignatureImpl(skybox, request, outSignature, depth, true);
			if (success)
			{
				outSignature.persistentEligible = outSignature.persistentEligible && IsTexturePersistentSignatureEligibleUnchecked(texture, baseTexture);
			}
			return success;
		}

		const bool success = TryBuildImageTextureSignature(baseTexture, request, outSignature);
		if (success)
		{
			outSignature.persistentEligible = outSignature.persistentEligible && IsTexturePersistentSignatureEligibleUnchecked(texture, baseTexture);
		}
		return success;
	}

	bool TryBuildAverageColorTextureSignatureImpl(FGameTexture* texture, TextureSignature& outSignature, int depth)
	{
		outSignature = {};
		if (depth > kMaxTextureSignatureDepth)
		{
			return false;
		}

		FTexture* baseTexture = TryResolveBaseTextureUnchecked(texture);
		if (baseTexture == nullptr)
		{
			return false;
		}

		TextureSignatureRequest request = {};
		request.contentKind = TextureSignatureContentKind::ProcessedBGRA;
		request.translation = 0;
		request.flags = TextureSignatureRequestFlag_None;

		if (auto* skybox = dynamic_cast<FSkyBox*>(baseTexture))
		{
			const bool success = TryBuildSkyboxTextureSignatureImpl(skybox, request, outSignature, depth, false);
			if (success)
			{
				outSignature.persistentEligible = outSignature.persistentEligible && IsTexturePersistentSignatureEligibleUnchecked(texture, baseTexture);
			}
			return success;
		}

		const bool success = TryBuildImageTextureSignature(baseTexture, request, outSignature);
		if (success)
		{
			outSignature.persistentEligible = outSignature.persistentEligible && IsTexturePersistentSignatureEligibleUnchecked(texture, baseTexture);
		}
		return success;
	}
}
