#pragma once

#include <cstdint>
#include <vector>

namespace nri_scene
{
constexpr uint32_t PaletteAwareIndexedMipAlgorithmVersion = 1;

struct PaletteAwareIndexedMipChain
{
	std::vector<uint8_t> pixels;
	uint32_t mipCount = 1;
};

uint64_t MakePaletteAwareIndexedMipKey(
	uint64_t rawContentKey,
	uint64_t palettePayloadSignature,
	uint32_t width,
	uint32_t height,
	uint32_t paletteIndex,
	bool alphaClip);

bool BuildPaletteAwareIndexedMipChain(
	const uint8_t* mip0Pixels,
	uint32_t width,
	uint32_t height,
	const uint8_t* paletteRowBgra,
	uint32_t paletteColorCount,
	bool alphaClip,
	PaletteAwareIndexedMipChain& outChain);
}
