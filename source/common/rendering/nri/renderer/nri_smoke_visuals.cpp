#include "nri_smoke_visuals.h"

#include "nri_smoke_contracts.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

namespace
{
	uint32_t PackColorChannel(float value)
	{
		value = std::clamp(value, 0.0f, 2.0f);
		return value <= 1.0f ?
			(uint32_t)std::clamp((int)std::lround((double)value * 16.0), 0, 16) :
			16u + (uint32_t)std::clamp((int)std::lround((double)(value - 1.0f) * 15.0), 0, 15);
	}

	uint32_t PackColor15(const float color[3])
	{
		return PackColorChannel(color[0]) |
			(PackColorChannel(color[1]) << 5u) |
			(PackColorChannel(color[2]) << 10u);
	}

	uint32_t FloatBits(float value)
	{
		uint32_t bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		return bits;
	}

	void HashWord(uint64_t& hash, uint32_t value)
	{
		hash ^= value;
		hash *= 1099511628211ull;
	}

	uint16_t FloatToHalf(float value)
	{
		const uint32_t bits = FloatBits(value);
		const uint32_t sign = (bits >> 16u) & 0x8000u;
		const uint32_t magnitude = bits & 0x7fffffffu;
		if (magnitude >= 0x7f800000u)
			return (uint16_t)(sign | (magnitude > 0x7f800000u ? 0x7e00u : 0x7c00u));
		const int32_t exponent = (int32_t)((magnitude >> 23u) & 0xffu) - 127 + 15;
		const uint32_t mantissa = magnitude & 0x7fffffu;
		if (exponent <= 0)
		{
			if (exponent < -10) return (uint16_t)sign;
			const uint32_t denormal = (mantissa | 0x800000u) >> (uint32_t)(1 - exponent);
			return (uint16_t)(sign | ((denormal + 0x1000u) >> 13u));
		}
		if (exponent >= 31) return (uint16_t)(sign | 0x7c00u);
		uint32_t halfExponent = (uint32_t)exponent;
		uint32_t halfMantissa = (mantissa + 0x1000u) >> 13u;
		if (halfMantissa == 0x400u)
		{
			halfMantissa = 0u;
			if (++halfExponent >= 31u) return (uint16_t)(sign | 0x7c00u);
		}
		return (uint16_t)(sign | (halfExponent << 10u) | halfMantissa);
	}

	uint32_t PackHalf2(float low, float high)
	{
		return (uint32_t)FloatToHalf(low) | ((uint32_t)FloatToHalf(high) << 16u);
	}

	void StorePackedWord(float& destination, uint32_t word)
	{
		std::memcpy(&destination, &word, sizeof(word));
	}
}

void NRIPopulateSmokeVisualConstants(const NRISmokeVisualSettings& settings,
	NRISmokeConstants& constants)
{
	StorePackedWord(constants.timeScale,
		PackHalf2(settings.extinctionThreshold, settings.extinctionKnee));
	StorePackedWord(constants.wind[0],
		PackHalf2(settings.extinctionGamma, settings.extinctionReference));
	StorePackedWord(constants.wind[1],
		PackHalf2(settings.extinctionShoulder, settings.colorPivot));
	StorePackedWord(constants.wind[2], PackColor15(settings.thinColor) |
		(PackColor15(settings.coreColor) << 15u));
}

uint64_t NRIHashSmokeVisualSettings(const NRISmokeVisualSettings& settings)
{
	uint64_t hash = 1469598103934665603ull;
	HashWord(hash, FloatBits(settings.extinctionThreshold));
	HashWord(hash, FloatBits(settings.extinctionKnee));
	HashWord(hash, FloatBits(settings.extinctionGamma));
	HashWord(hash, FloatBits(settings.extinctionReference));
	HashWord(hash, FloatBits(settings.extinctionShoulder));
	HashWord(hash, PackColor15(settings.thinColor));
	HashWord(hash, PackColor15(settings.coreColor));
	HashWord(hash, FloatBits(settings.colorPivot));
	return hash;
}

const char* NRIGetSmokeFieldDebugName(uint32_t mode)
{
	static const char* const names[] = {
		"off", "mass", "extinction", "albedo", "thermal-buoyancy",
		"extinction-gradient-magnitude", "extinction-gradient-orientation",
		"world-lobe-sum", "world-lobe-dominant-direction",
		"world-lobe-opposing-contrast", "world-lobe-confidence"
	};
	return mode < std::size(names) ? names[mode] : "unknown";
}
