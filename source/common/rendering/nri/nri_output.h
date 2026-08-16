#pragma once

#include <cstdint>

enum class NRIPTOutputMode : uint32_t
{
	// `SDR` and `HDR` are the current user-facing request modes.
	// `HDRLinear16` and `HDR10PQ` remain internal resolved transport states.
	SDR = 0,
	HDR = 1,
	HDRLinear16 = 2,
	HDR10PQ = 3
};

enum class NRIPTTonemapMode : uint32_t
{
	Hable = 0,
	ACESFitted = 1,
	Reinhard = 2
};

struct NRIPTOutputPolicy
{
	NRIPTOutputMode requestedMode = NRIPTOutputMode::SDR;
	NRIPTOutputMode resolvedMode = NRIPTOutputMode::SDR;
	NRIPTTonemapMode tonemapMode = NRIPTTonemapMode::Hable;
	float exposure = 1.0f;
	float contrast = 1.0f;
	float saturation = 1.0f;
	float shoulder = 1.0f;
	float toe = 1.0f;
	float paperWhiteNits = 200.0f;
	float displayMinLuminance = 0.0f;
	float displayMaxLuminance = 80.0f;
	float displaySdrLuminance = 80.0f;
	bool displayInfoAvailable = false;
	bool displayHdrSupported = false;
	bool hdrSwapChainActive = false;
	bool offscreenHdrTarget = true;
};

inline float GetNRIPTOutputSafeDisplaySdrLuminance(float displaySdrLuminance)
{
	return displaySdrLuminance > 1.0f ? displaySdrLuminance : 1.0f;
}

inline float GetNRIPTOutputSafeDisplayMaxLuminance(float displaySdrLuminance, float displayMaxLuminance)
{
	const float safeDisplaySdr = GetNRIPTOutputSafeDisplaySdrLuminance(displaySdrLuminance);
	return displayMaxLuminance > safeDisplaySdr ? displayMaxLuminance : safeDisplaySdr;
}

inline float GetNRIPTOutputClampedPaperWhiteNits(float paperWhiteNits, float displaySdrLuminance, float displayMaxLuminance)
{
	const float safeDisplaySdr = GetNRIPTOutputSafeDisplaySdrLuminance(displaySdrLuminance);
	const float safeDisplayMax = GetNRIPTOutputSafeDisplayMaxLuminance(displaySdrLuminance, displayMaxLuminance);
	float safePaperWhite = paperWhiteNits > safeDisplaySdr ? paperWhiteNits : safeDisplaySdr;
	if (safePaperWhite > safeDisplayMax)
	{
		safePaperWhite = safeDisplayMax;
	}
	return safePaperWhite;
}

inline float GetNRIPTHdrPaperWhiteScale(const NRIPTOutputPolicy& policy)
{
	return GetNRIPTOutputClampedPaperWhiteNits(policy.paperWhiteNits, policy.displaySdrLuminance, policy.displayMaxLuminance) / 80.0f;
}

inline float GetNRIPTHdrHeadroomInPaperWhites(const NRIPTOutputPolicy& policy)
{
	const float safeDisplayMax = GetNRIPTOutputSafeDisplayMaxLuminance(policy.displaySdrLuminance, policy.displayMaxLuminance);
	const float safePaperWhite = GetNRIPTOutputClampedPaperWhiteNits(policy.paperWhiteNits, policy.displaySdrLuminance, policy.displayMaxLuminance);
	const float headroom = safeDisplayMax / safePaperWhite;
	return headroom > 1.0f ? headroom : 1.0f;
}

inline float GetNRIPTHdrMaxOutputScale(const NRIPTOutputPolicy& policy)
{
	return GetNRIPTHdrPaperWhiteScale(policy) * GetNRIPTHdrHeadroomInPaperWhites(policy);
}

inline bool IsNRIPTHdrOutputActive(const NRIPTOutputPolicy& policy)
{
	return policy.hdrSwapChainActive;
}

inline const char* GetNRIPTOutputControlBlockName(const NRIPTOutputPolicy& policy)
{
	return IsNRIPTHdrOutputActive(policy) ? "hdr" : "sdr";
}

inline const char* GetNRIPTOutputModeName(NRIPTOutputMode mode)
{
	switch (mode)
	{
	case NRIPTOutputMode::SDR: return "sdr";
	case NRIPTOutputMode::HDR: return "hdr";
	case NRIPTOutputMode::HDRLinear16: return "hdr-linear16";
	case NRIPTOutputMode::HDR10PQ: return "hdr10-pq";
	default: return "unknown";
	}
}

inline const char* GetNRIPTTonemapModeName(NRIPTTonemapMode mode)
{
	switch (mode)
	{
	case NRIPTTonemapMode::Hable: return "hable";
	case NRIPTTonemapMode::ACESFitted: return "aces-fitted";
	case NRIPTTonemapMode::Reinhard: return "reinhard";
	default: return "unknown";
	}
}
