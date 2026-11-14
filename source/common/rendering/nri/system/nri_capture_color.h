#pragma once

#include "NRI.h"

#include <cstddef>
#include <cstdint>

enum class NRICaptureEncoding
{
	Sdr,
	LinearScRGB
};

struct NRICaptureColorInfo
{
	nri::Format format = nri::Format::UNKNOWN;
	NRICaptureEncoding encoding = NRICaptureEncoding::Sdr;
	float paperWhiteNits = 80.0f;
	float peakNits = 80.0f;
};

uint32_t NRIGetCaptureBytesPerPixel(nri::Format format);

// Produces encoded SDR RGB8, including channel-order conversion. The source
// encoding describes the pixels, independently of their texture format.
bool NRIConvertCaptureToSdr(const void* source, size_t sourceSize,
	uint32_t width, uint32_t height, size_t sourcePitch,
	const NRICaptureColorInfo& colorInfo,
	uint8_t* destination, size_t destinationSize, size_t destinationPitch);
