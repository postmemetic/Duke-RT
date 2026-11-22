#pragma once

#include "nri_settings_availability.h"

#include <algorithm>
#include <iterator>

struct NRISettingsProfilePreset
{
	const char* name;
	int upscaler;
	int upscalerMode;
	int outputMode;
	bool denoise;
	bool mirrorWarning;
};

inline constexpr NRISettingsProfilePreset kNRISettingsProfilePresets[] = {
	{ "Safe Mode", 0, 0, 0, true, true },
	{ "Fast Preset - DLRR", 3, 3, 1, false, false },
	{ "Medium Preset - DLRR", 3, 2, 1, false, false },
	{ "Beautiful Preset - DLRR", 3, 0, 1, false, false },
	{ "Fast Preset - DLSS-SR", 2, 3, 1, true, true },
	{ "Medium Preset - DLSS-SR", 2, 2, 1, true, true },
	{ "Beautiful Preset - DLSS-SR", 2, 0, 1, true, true },
	{ "Fast Preset - AMD FSR 3", 4, 3, 1, true, true },
	{ "Medium Preset - AMD FSR 3", 4, 2, 1, true, true },
	{ "Beautiful Preset - AMD FSR 3", 4, 0, 1, true, true },
	{ "Hardware Recommended", 0, 0, 0, true, true },
};

inline constexpr int kNRIHardwareRecommended = (int)std::size(kNRISettingsProfilePresets) - 1;

inline int NRIRecommendedUpscalerMode(const NRISettingsCapabilities& capabilities, int upscaler)
{
	if (upscaler < 2) return 0; // Native
	return capabilities.smallDiscreteGpu ? 5 : 3; // Ultra Performance or Balanced
}

inline int NRIClampSettingsProfile(int profile)
{
	return std::clamp(profile, 0, (int)std::size(kNRISettingsProfilePresets) - 1);
}

struct NRISettingsProfilePlan
{
	NRISettingsProfilePreset preset;
	std::string unavailableReason;
	bool sdrFallback = false;
	bool nativeTemporalAA = false;
};

// Resolve every prerequisite before the caller changes any CVARs. HDR is a
// preference; the named upscaler is a defining requirement of each profile.
inline NRISettingsProfilePlan NRIBuildSettingsProfilePlan(const NRISettingsCapabilities& capabilities, int profile)
{
	NRISettingsProfilePlan plan{ kNRISettingsProfilePresets[NRIClampSettingsProfile(profile)] };
	if (profile == kNRIHardwareRecommended)
	{
		auto& preset = plan.preset;
		if (capabilities.dlrr) preset.upscaler = 3;
		else if (capabilities.dlss) preset.upscaler = 2;
		else if (capabilities.fsr) preset.upscaler = 4;
		preset.upscalerMode = NRIRecommendedUpscalerMode(capabilities, preset.upscaler);
		preset.denoise = preset.mirrorWarning = preset.upscaler != 3;
		plan.nativeTemporalAA = preset.upscaler == 0;
		return plan;
	}
	if (capabilities.deviceKnown)
		plan.unavailableReason = NRIEvaluateSettingAvailability(capabilities, "nri_settingsprofile", profile);
	if (plan.preset.outputMode != 0 && capabilities.presentationKnown && !capabilities.hdr)
	{
		plan.preset.outputMode = 0;
		plan.sdrFallback = true;
	}
	return plan;
}
