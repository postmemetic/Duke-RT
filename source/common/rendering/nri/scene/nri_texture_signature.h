#pragma once

#include <cstdint>

class FGameTexture;
class FTexture;

namespace nri_scene
{
enum class TextureSignatureContentKind : uint8_t
{
	ProcessedBGRA = 0,
	Indexed,
};

enum TextureSignatureRequestFlags : uint32_t
{
	TextureSignatureRequestFlag_None = 0,
	TextureSignatureRequestFlag_Expand = 1u << 0,
	TextureSignatureRequestFlag_Upscale = 1u << 1,
};

struct TextureSignatureRequest
{
	TextureSignatureContentKind contentKind = TextureSignatureContentKind::ProcessedBGRA;
	int translation = 0;
	uint32_t flags = TextureSignatureRequestFlag_None;
	uint8_t scaler = 0;
	uint8_t scaleFactor = 0;
};

enum class TextureSignatureSourceKind : uint8_t
{
	None = 0,
	ImageBacked,
	SkyboxStructural,
};

struct TextureSignature
{
	bool valid = false;
	bool metadataDerived = false;
	bool persistentEligible = false;
	TextureSignatureSourceKind sourceKind = TextureSignatureSourceKind::None;
	TextureSignatureRequest request = {};
	uint64_t key = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t faceMask = 0;
	bool isThreeFace = false;
	bool flipTop = false;
};

// Returns whether the requested output form can be identified cheaply from metadata.
// Returning false does not mean the request is impossible, only that a future caller
// should fall back to realized texture creation / hashing.
bool CanBuildTextureSignatureFromMetadata(const TextureSignatureRequest& request);

// Attempts a metadata-only signature for a resolved base texture.
bool TryBuildImageTextureSignature(FTexture* texture, const TextureSignatureRequest& request, TextureSignature& outSignature);

// Attempts a structural metadata-only signature for a skybox-backed game texture.
// The returned signature describes the skybox source structure, not a fully consumer-
// specific derived resource. Later consumers may derive narrower keys from it.
bool TryBuildSkyboxTextureSignature(FGameTexture* texture, const TextureSignatureRequest& request, TextureSignature& outSignature);

// Attempts to build a metadata-only signature for a game texture by resolving it to
// either an image-backed texture or a structural skybox description.
bool TryBuildTextureSignature(FGameTexture* texture, const TextureSignatureRequest& request, TextureSignature& outSignature);

// Attempts to build a metadata-only signature for average-color caching.
// This is intentionally narrower than a generic texture-content signature:
// it describes the source structure needed to derive an average color, so
// skybox-only orientation details like flip-top are excluded from the key.
bool TryBuildAverageColorTextureSignature(FGameTexture* texture, TextureSignature& outSignature);

// Returns whether a texture is safe to treat as persistently stable across
// execution for cache reuse. Callers may still build valid metadata-derived
// signatures for non-persistent cases, but should keep them on short-lived
// fallback paths.
bool IsTexturePersistentSignatureEligible(FTexture* texture);
bool IsTexturePersistentSignatureEligible(FGameTexture* texture);
}
