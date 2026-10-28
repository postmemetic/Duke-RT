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
};

inline int NRIClampSettingsProfile(int profile)
{
	return std::clamp(profile, 0, (int)std::size(kNRISettingsProfilePresets) - 1);
}

struct NRISettingsProfilePlan
{
	NRISettingsProfilePreset preset;
	std::string unavailableReason;
	bool sdrFallback = false;
};

// Resolve every prerequisite before the caller changes any CVARs. HDR is a
// preference; the named upscaler is a defining requirement of each profile.
inline NRISettingsProfilePlan NRIBuildSettingsProfilePlan(const NRISettingsCapabilities& capabilities, int profile)
{
	NRISettingsProfilePlan plan{ kNRISettingsProfilePresets[NRIClampSettingsProfile(profile)] };
	if (capabilities.deviceKnown)
		plan.unavailableReason = NRIEvaluateSettingAvailability(capabilities, "nri_settingsprofile", profile);
	if (plan.preset.outputMode != 0 && capabilities.presentationKnown && !capabilities.hdr)
	{
		plan.preset.outputMode = 0;
		plan.sdrFallback = true;
	}
	return plan;
}
