#include "nri_indexed_mip_chain.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <utility>

namespace
{
	constexpr uint64_t FnvOffsetBasis = 1469598103934665603ull;
	constexpr uint64_t FnvPrime = 1099511628211ull;
	constexpr uint64_t PaletteMipKeyDomain = 0x50414c4d49500001ull;

	void AppendHash(uint64_t& hash, const void* data, size_t size)
	{
		const uint8_t* bytes = static_cast<const uint8_t*>(data);
		for (size_t index = 0; index < size; ++index)
		{
			hash ^= bytes[index];
			hash *= FnvPrime;
		}
	}

	uint32_t MipDimension(uint32_t dimension)
	{
		return std::max(1u, dimension / 2u);
	}

	uint32_t FootprintBegin(uint32_t destinationIndex, uint32_t sourceSize, uint32_t destinationSize)
	{
		return (uint32_t)(((uint64_t)destinationIndex * sourceSize) / destinationSize);
	}

	struct PaletteColorAggregate
	{
		uint64_t sumB = 0;
		uint64_t sumG = 0;
		uint64_t sumR = 0;
		uint64_t sampleCount = 0;
	};

	uint8_t QuantizePaletteColor(
		const PaletteColorAggregate& aggregate,
		const uint8_t* paletteRowBgra,
		bool alphaClip)
	{
		if (aggregate.sampleCount == 0)
		{
			return 0;
		}

		const uint32_t firstCandidate = alphaClip ? 1u : 0u;
		uint8_t bestIndex = (uint8_t)firstCandidate;
		int64_t bestScore = std::numeric_limits<int64_t>::max();
		for (uint32_t candidateIndex = firstCandidate; candidateIndex < 256u; ++candidateIndex)
		{
			const uint8_t* candidate = paletteRowBgra + candidateIndex * 4u;
			const int64_t squaredColorLength =
				(int64_t)candidate[0] * candidate[0] +
				(int64_t)candidate[1] * candidate[1] +
				(int64_t)candidate[2] * candidate[2];
			const int64_t colorDotSum =
				(int64_t)candidate[0] * (int64_t)aggregate.sumB +
				(int64_t)candidate[1] * (int64_t)aggregate.sumG +
				(int64_t)candidate[2] * (int64_t)aggregate.sumR;
			// This is squared RGB distance with the candidate-independent terms
			// removed and the mean denominator retained. It compares exactly
			// without floating-point rounding or a sampleCount-squared term.
			const int64_t score =
				(int64_t)aggregate.sampleCount * squaredColorLength - 2 * colorDotSum;
			if (score < bestScore)
			{
				bestScore = score;
				bestIndex = (uint8_t)candidateIndex;
			}
		}
		return bestIndex;
	}
}

namespace nri_scene
{
uint64_t MakePaletteAwareIndexedMipKey(
	uint64_t rawContentKey,
	uint64_t palettePayloadSignature,
	uint32_t width,
	uint32_t height,
	uint32_t paletteIndex,
	bool alphaClip)
{
	uint64_t key = FnvOffsetBasis;
	const uint32_t alphaPolicy = alphaClip ? 1u : 0u;
	AppendHash(key, &PaletteMipKeyDomain, sizeof(PaletteMipKeyDomain));
	AppendHash(key, &PaletteAwareIndexedMipAlgorithmVersion, sizeof(PaletteAwareIndexedMipAlgorithmVersion));
	AppendHash(key, &rawContentKey, sizeof(rawContentKey));
	AppendHash(key, &palettePayloadSignature, sizeof(palettePayloadSignature));
	AppendHash(key, &width, sizeof(width));
	AppendHash(key, &height, sizeof(height));
	AppendHash(key, &paletteIndex, sizeof(paletteIndex));
	AppendHash(key, &alphaPolicy, sizeof(alphaPolicy));
	return key != 0 ? key : PaletteMipKeyDomain;
}

bool BuildPaletteAwareIndexedMipChain(
	const uint8_t* mip0Pixels,
	uint32_t width,
	uint32_t height,
	const uint8_t* paletteRowBgra,
	uint32_t paletteColorCount,
	bool alphaClip,
	PaletteAwareIndexedMipChain& outChain)
{
	outChain = {};
	if (mip0Pixels == nullptr || width == 0 || height == 0 ||
		paletteRowBgra == nullptr || paletteColorCount < 256u)
	{
		return false;
	}

	const uint64_t mip0Size64 = (uint64_t)width * height;
	// Quantization's exact integer score is bounded by approximately
	// 400,000 * sampleCount for 8-bit RGB.
	if (mip0Size64 > std::numeric_limits<size_t>::max() ||
		mip0Size64 > (uint64_t)std::numeric_limits<int64_t>::max() / 400000ull)
	{
		return false;
	}

	size_t packedSize = (size_t)mip0Size64;
	uint32_t mipWidth = width;
	uint32_t mipHeight = height;
	while (mipWidth > 1u || mipHeight > 1u)
	{
		mipWidth = MipDimension(mipWidth);
		mipHeight = MipDimension(mipHeight);
		const uint64_t levelSize = (uint64_t)mipWidth * mipHeight;
		if (levelSize > std::numeric_limits<size_t>::max() - packedSize)
		{
			return false;
		}
		packedSize += (size_t)levelSize;
	}

	outChain.pixels.reserve(packedSize);
	outChain.pixels.insert(outChain.pixels.end(), mip0Pixels, mip0Pixels + (size_t)mip0Size64);

	std::vector<PaletteColorAggregate> sourceAggregates((size_t)mip0Size64);
	for (size_t pixelIndex = 0; pixelIndex < (size_t)mip0Size64; ++pixelIndex)
	{
		const uint8_t sourceIndex = mip0Pixels[pixelIndex];
		if (alphaClip && sourceIndex == 0)
		{
			continue;
		}
		const uint8_t* color = paletteRowBgra + (size_t)sourceIndex * 4u;
		sourceAggregates[pixelIndex] = { color[0], color[1], color[2], 1u };
	}

	mipWidth = width;
	mipHeight = height;
	while (mipWidth > 1u || mipHeight > 1u)
	{
		const uint32_t nextWidth = MipDimension(mipWidth);
		const uint32_t nextHeight = MipDimension(mipHeight);
		std::vector<uint8_t> nextPixels((size_t)nextWidth * nextHeight);
		std::vector<PaletteColorAggregate> nextAggregates((size_t)nextWidth * nextHeight);
		for (uint32_t y = 0; y < nextHeight; ++y)
		{
			const uint32_t sourceYBegin = FootprintBegin(y, mipHeight, nextHeight);
			const uint32_t sourceYEnd = FootprintBegin(y + 1u, mipHeight, nextHeight);
			for (uint32_t x = 0; x < nextWidth; ++x)
			{
				const uint32_t sourceXBegin = FootprintBegin(x, mipWidth, nextWidth);
				const uint32_t sourceXEnd = FootprintBegin(x + 1u, mipWidth, nextWidth);
				PaletteColorAggregate aggregate = {};
				for (uint32_t sourceY = sourceYBegin; sourceY < sourceYEnd; ++sourceY)
				{
					for (uint32_t sourceX = sourceXBegin; sourceX < sourceXEnd; ++sourceX)
					{
						const PaletteColorAggregate& source =
							sourceAggregates[(size_t)sourceY * mipWidth + sourceX];
						aggregate.sumB += source.sumB;
						aggregate.sumG += source.sumG;
						aggregate.sumR += source.sumR;
						aggregate.sampleCount += source.sampleCount;
					}
				}
				const size_t destinationIndex = (size_t)y * nextWidth + x;
				nextAggregates[destinationIndex] = aggregate;
				nextPixels[destinationIndex] = QuantizePaletteColor(
					aggregate, paletteRowBgra, alphaClip);
			}
		}

		outChain.pixels.insert(outChain.pixels.end(), nextPixels.begin(), nextPixels.end());
		sourceAggregates = std::move(nextAggregates);
		mipWidth = nextWidth;
		mipHeight = nextHeight;
		++outChain.mipCount;
	}

	return true;
}
}
