#include "nri_indexed_mip_chain.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

namespace
{
	void Check(bool condition, const char* message)
	{
		if (!condition)
		{
			std::cerr << "FAILED: " << message << '\n';
			std::exit(1);
		}
	}

	std::array<uint8_t, 256u * 4u> MakeGrayscalePalette()
	{
		std::array<uint8_t, 256u * 4u> palette = {};
		for (uint32_t index = 0; index < 256u; ++index)
		{
			palette[index * 4u + 0u] = (uint8_t)index;
			palette[index * 4u + 1u] = (uint8_t)index;
			palette[index * 4u + 2u] = (uint8_t)index;
			palette[index * 4u + 3u] = 255u;
		}
		return palette;
	}
}

int main()
{
	const auto palette = MakeGrayscalePalette();
	{
		const std::vector<uint8_t> mip0 = {
			10, 20, 50, 60,
			30, 40, 70, 80,
			90, 100, 130, 140,
			110, 120, 150, 160,
		};
		nri_scene::PaletteAwareIndexedMipChain chain = {};
		Check(nri_scene::BuildPaletteAwareIndexedMipChain(
			mip0.data(), 4, 4, palette.data(), 256, false, chain), "4x4 chain build");
		Check(chain.mipCount == 3, "4x4 mip count");
		Check(chain.pixels.size() == 21, "4x4 packed byte count");
		Check(std::memcmp(chain.pixels.data(), mip0.data(), mip0.size()) == 0, "mip 0 byte identity");
		Check(chain.pixels[16] == 25, "mip 1 top-left quantization");
		Check(chain.pixels[17] == 65, "mip 1 top-right quantization");
		Check(chain.pixels[18] == 105, "mip 1 bottom-left quantization");
		Check(chain.pixels[19] == 145, "mip 1 bottom-right quantization");
		Check(chain.pixels[20] == 85, "mip 2 aggregate quantization");
	}
	{
		const std::vector<uint8_t> mip0 = { 0, 100, 0, 100 };
		nri_scene::PaletteAwareIndexedMipChain clipped = {};
		nri_scene::PaletteAwareIndexedMipChain opaque = {};
		Check(nri_scene::BuildPaletteAwareIndexedMipChain(
			mip0.data(), 2, 2, palette.data(), 256, true, clipped), "alpha-clipped chain build");
		Check(nri_scene::BuildPaletteAwareIndexedMipChain(
			mip0.data(), 2, 2, palette.data(), 256, false, opaque), "opaque chain build");
		Check(clipped.pixels.back() == 100, "transparent index excluded from non-empty footprint");
		Check(opaque.pixels.back() == 50, "index 0 included without alpha clipping");

		const std::vector<uint8_t> emptyMip0 = { 0, 0, 0, 0 };
		Check(nri_scene::BuildPaletteAwareIndexedMipChain(
			emptyMip0.data(), 2, 2, palette.data(), 256, true, clipped), "empty alpha footprint build");
		Check(clipped.pixels.back() == 0, "empty alpha footprint remains index 0");
	}
	{
		const std::vector<uint8_t> mip0 = {
			0, 0, 60,
			0, 0, 60,
			0, 0, 90,
			0, 0, 90,
			0, 0, 90,
		};
		nri_scene::PaletteAwareIndexedMipChain chain = {};
		Check(nri_scene::BuildPaletteAwareIndexedMipChain(
			mip0.data(), 3, 5, palette.data(), 256, false, chain), "3x5 NPOT chain build");
		Check(chain.mipCount == 3, "3x5 NPOT mip count");
		Check(chain.pixels.size() == 18, "3x5 NPOT packed byte count");
		Check(chain.pixels[15] == 20, "3x5 NPOT first proportional footprint");
		Check(chain.pixels[16] == 30, "3x5 NPOT second proportional footprint");
		Check(chain.pixels[17] == 26, "3x5 NPOT exact weighted aggregate");
	}
	{
		// First-level means quantize to 0,0,0,1. Averaging those quantized
		// indices would produce 0; carrying the exact source aggregates yields 1.
		const std::vector<uint8_t> mip0 = {
			0, 0, 0, 0,
			1, 1, 1, 1,
			0, 0, 1, 1,
			1, 1, 2, 2,
		};
		nri_scene::PaletteAwareIndexedMipChain chain = {};
		Check(nri_scene::BuildPaletteAwareIndexedMipChain(
			mip0.data(), 4, 4, palette.data(), 256, false, chain), "aggregate drift chain build");
		Check(chain.pixels[16] == 0 && chain.pixels[17] == 0 &&
			chain.pixels[18] == 0 && chain.pixels[19] == 1, "aggregate drift mip 1 precondition");
		Check(chain.pixels[20] == 1, "deeper mip uses exact aggregates");
	}
	{
		const uint64_t base = nri_scene::MakePaletteAwareIndexedMipKey(10, 20, 64, 32, 3, false);
		Check(base == nri_scene::MakePaletteAwareIndexedMipKey(10, 20, 64, 32, 3, false), "key determinism");
		Check(base != nri_scene::MakePaletteAwareIndexedMipKey(11, 20, 64, 32, 3, false), "raw-content key input");
		Check(base != nri_scene::MakePaletteAwareIndexedMipKey(10, 21, 64, 32, 3, false), "palette-payload key input");
		Check(base != nri_scene::MakePaletteAwareIndexedMipKey(10, 20, 64, 32, 4, false), "palette-row key input");
		Check(base != nri_scene::MakePaletteAwareIndexedMipKey(10, 20, 64, 32, 3, true), "alpha-policy key input");
	}

	std::cout << "Palette-aware indexed mip-chain tests passed.\n";
	return 0;
}
