#pragma once

#include <cstdint>
#include "shaders/Include/HdrOutputMath.hlsli"

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
	return NriHdrSafeSdrWhite(displaySdrLuminance);
}

inline float GetNRIPTOutputSafeDisplayMaxLuminance(float displaySdrLuminance, float displayMaxLuminance)
{
	return NriHdrSafePeak(displaySdrLuminance, displayMaxLuminance);
}

inline float GetNRIPTOutputClampedPaperWhiteNits(float paperWhiteNits, float displaySdrLuminance, float displayMaxLuminance)
{
	return NriHdrScenePaperWhite(paperWhiteNits, displaySdrLuminance, displayMaxLuminance);
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
	return GetNRIPTOutputSafeDisplayMaxLuminance(policy.displaySdrLuminance, policy.displayMaxLuminance) / 80.0f;
}

inline float GetNRIPTHdrUiWhiteScale(const NRIPTOutputPolicy& policy)
{
	// Preserve the legacy UI brightness policy independently of scene white.
	// In particular, reducing native HDR paper white below Windows SDR white
	// must not dim the HUD, menus or frame-generation UI composition.
	const float safeSdr = NriHdrSafeSdrWhite(policy.displaySdrLuminance);
	const float nativePeak = NriHdrSafePeak(policy.displaySdrLuminance, policy.displayMaxLuminance);
	const float uiPeak = nativePeak > safeSdr ? nativePeak : safeSdr;
	const float requested = NriHdrIsFinite(policy.paperWhiteNits) ? policy.paperWhiteNits : safeSdr;
	const float uiWhite = requested > safeSdr ? requested : safeSdr;
	return (uiWhite < uiPeak ? uiWhite : uiPeak) / 80.0f;
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
