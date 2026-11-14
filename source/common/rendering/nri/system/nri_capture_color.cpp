#include "nri_capture_color.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace
{
	constexpr float ShoulderKnee = 0.5f;

	bool FitsImage(uint32_t width, uint32_t height, size_t bytesPerPixel, size_t pitch, size_t size)
	{
		if (width == 0 || height == 0 || width > std::numeric_limits<size_t>::max() / bytesPerPixel)
			return false;
		const size_t rowBytes = size_t(width) * bytesPerPixel;
		if (pitch < rowBytes || size < rowBytes)
			return false;
		return size_t(height - 1u) <= (size - rowBytes) / pitch;
	}

	float DecodeHalf(uint16_t bits)
	{
		const uint32_t exponent = (bits >> 10u) & 31u;
		const uint32_t mantissa = bits & 1023u;
		// Non-finite data is not useful image content; sanitize it before any
		// normalization so it cannot contaminate the other two channels.
		if (exponent == 31u)
			return 0.0f;
		const float value = exponent == 0u ? std::ldexp(float(mantissa), -24) :
			std::ldexp(1.0f + float(mantissa) / 1024.0f, int(exponent) - 15);
		return (bits & 0x8000u) != 0u ? -value : value;
	}

	uint8_t Quantize(float value)
	{
		return uint8_t(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
	}

	float LinearToSrgb(float value)
	{
		return value <= 0.0031308f ? value * 12.92f :
			1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
	}

	struct HdrConversion
	{
		float inverseWhiteScale;
		float peakInWhites;
		float shoulder;
	};

	HdrConversion MakeHdrConversion(const NRICaptureColorInfo& info)
	{
		const float white = std::isfinite(info.paperWhiteNits) && info.paperWhiteNits > 0.0f ?
			(std::max)(info.paperWhiteNits, 1.0f) : 80.0f;
		const float peak = std::isfinite(info.peakNits) && info.peakNits > 0.0f ?
			(std::max)(info.peakNits, 1.0f) : white;
		const float effectiveWhite = (std::min)(white, peak);
		const float peakInWhites = (std::max)(peak / effectiveWhite, 1.0f);
		return { 80.0f / effectiveWhite, peakInWhites,
			1.0f / (1.0f - ShoulderKnee) - 1.0f / (peakInWhites - ShoulderKnee) };
	}

	void ConvertHdr(float (&color)[3], const HdrConversion& conversion)
	{
		for (float& channel : color)
			channel = (std::max)(channel, 0.0f) * conversion.inverseWhiteScale;
		const float maximum = (std::max)(color[0], (std::max)(color[1], color[2]));
		if (maximum > ShoulderKnee)
		{
			// Leave shadows and midtones unchanged. The rational shoulder has
			// slope one at the knee and reaches SDR white at the display peak.
			// One scale for all channels preserves hue instead of clipping RGB.
			const float delta = (std::min)(maximum, conversion.peakInWhites) - ShoulderKnee;
			const float mapped = ShoulderKnee + delta / (1.0f + conversion.shoulder * delta);
			const float scale = mapped / maximum;
			for (float& channel : color)
				channel *= scale;
		}
		for (float& channel : color)
			channel = LinearToSrgb(channel);
	}
}

uint32_t NRIGetCaptureBytesPerPixel(nri::Format format)
{
	switch (format)
	{
	case nri::Format::RGBA8_UNORM:
	case nri::Format::RGBA8_SRGB:
	case nri::Format::BGRA8_UNORM:
	case nri::Format::BGRA8_SRGB:
	case nri::Format::R10_G10_B10_A2_UNORM:
		return 4u;
	case nri::Format::RGBA16_SFLOAT:
		return 8u;
	default:
		return 0u;
	}
}

bool NRIConvertCaptureToSdr(const void* source, size_t sourceSize,
	uint32_t width, uint32_t height, size_t sourcePitch,
	const NRICaptureColorInfo& colorInfo,
	uint8_t* destination, size_t destinationSize, size_t destinationPitch)
{
	const uint32_t sourceBytesPerPixel = NRIGetCaptureBytesPerPixel(colorInfo.format);
	if (source == nullptr || destination == nullptr || sourceBytesPerPixel == 0u ||
		!FitsImage(width, height, sourceBytesPerPixel, sourcePitch, sourceSize) ||
		!FitsImage(width, height, 3u, destinationPitch, destinationSize))
		return false;
	const bool hdr = colorInfo.encoding == NRICaptureEncoding::LinearScRGB;
	if ((!hdr && colorInfo.encoding != NRICaptureEncoding::Sdr) ||
		(hdr && colorInfo.format != nri::Format::RGBA16_SFLOAT))
		return false;

	const HdrConversion conversion = MakeHdrConversion(colorInfo);
	for (uint32_t y = 0; y < height; ++y)
	{
		const uint8_t* row = static_cast<const uint8_t*>(source) + size_t(y) * sourcePitch;
		uint8_t* output = destination + size_t(y) * destinationPitch;
		for (uint32_t x = 0; x < width; ++x)
		{
			const uint8_t* pixel = row + size_t(x) * sourceBytesPerPixel;
			uint8_t* result = output + size_t(x) * 3u;
			switch (colorInfo.format)
			{
			case nri::Format::RGBA8_UNORM:
			case nri::Format::RGBA8_SRGB:
				std::memcpy(result, pixel, 3u);
				break;
			case nri::Format::BGRA8_UNORM:
			case nri::Format::BGRA8_SRGB:
				result[0] = pixel[2];
				result[1] = pixel[1];
				result[2] = pixel[0];
				break;
			case nri::Format::R10_G10_B10_A2_UNORM:
			{
				uint32_t packed;
				std::memcpy(&packed, pixel, sizeof(packed));
				for (uint32_t channel = 0; channel < 3u; ++channel)
					result[channel] = uint8_t(((packed >> (channel * 10u)) & 1023u) * 255u / 1023u);
				break;
			}
			case nri::Format::RGBA16_SFLOAT:
			{
				uint16_t half[4];
				std::memcpy(half, pixel, sizeof(half));
				float color[3] = { DecodeHalf(half[0]), DecodeHalf(half[1]), DecodeHalf(half[2]) };
				if (hdr)
					ConvertHdr(color, conversion);
				for (uint32_t channel = 0; channel < 3u; ++channel)
					result[channel] = Quantize(color[channel]);
				break;
			}
			default:
				return false;
			}
		}
	}
	return true;
}
