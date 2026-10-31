#include "nri_settings_availability.h"
#include "nri_settings_profile_policy.h"

#include <cstring>

std::string NRIEvaluateSettingAvailability(const NRISettingsCapabilities& caps, const char* setting, int value)
{
	if (setting == nullptr)
		return {};
	const bool upscaler = std::strcmp(setting, "nri_upscaler") == 0;
	const bool output = std::strcmp(setting, "nri_ptoutputmode") == 0;
	const bool framegen = std::strcmp(setting, "nri_framegen") == 0;
	const bool profile = std::strcmp(setting, "nri_settingsprofile") == 0;
	const bool smoke = std::strcmp(setting, "nri_ptsmoke") == 0;
	const bool smokeQuality = std::strcmp(setting, "nri_ptsmokeworkprofile") == 0;
	if (!(upscaler || output || framegen || profile || smoke || smokeQuality))
		return {};
	// Off, SDR and Safe Mode remain selectable even during device startup.
	if (value == 0 && !smokeQuality)
		return {};
	if (!caps.deviceKnown)
		return "Availability will be known after the path tracing device starts.";
	if (profile)
	{
		if (value < 0 || value >= (int)std::size(kNRISettingsProfilePresets))
			return "This settings profile is not recognized.";
		const auto& preset = kNRISettingsProfilePresets[value];
		const auto reason = NRIEvaluateSettingAvailability(caps, "nri_upscaler", preset.upscaler);
		return reason.empty() ? reason : std::string(preset.name) + " cannot be applied. " + reason;
	}
	if (upscaler)
	{
		switch (value)
		{
		case 1: return {}; // Migrated legacy NIS setting; not a main-upscaler menu choice.
		case 2: return caps.dlss ? "" : "DLSS Super Resolution is unavailable on this GPU or graphics backend. Choose FSR or Off.";
		case 3: return caps.dlrr ? "" : "DLSS Ray Reconstruction is unavailable on this GPU or graphics backend. Choose FSR or Off.";
		case 4: return caps.fsr ? "" : "FSR is unavailable on this graphics backend or its runtime is missing. Choose Off.";
		default: return "This upscaler is not supported.";
		}
	}
	if (output)
	{
		if (!caps.presentationKnown)
			return "HDR availability is still being detected for the current display.";
		return caps.hdr ? "" : "HDR is unavailable on the current display. Enable HDR for this display in the operating system, or choose SDR.";
	}
	if (framegen)
	{
		if (!caps.d3d12)
			return "Frame generation currently requires the Direct3D 12 backend.";
		if (!caps.framegenShaderModel)
			return "Frame generation requires shader model 6.2 or newer.";
		if (!caps.framegenNativeDevice || !caps.framegenNativeQueue)
			return "Frame generation requires a native Direct3D 12 device and graphics queue.";
		if (!caps.framegenRuntime)
			return "The FidelityFX frame generation runtime is unavailable. Restore amd_fidelityfx_dx12.dll from the release package.";
		if (!caps.framegenWindowMode)
			return "Frame generation requires windowed or borderless fullscreen presentation.";
		return {};
	}
	if (caps.amd)
		return "Smoke is disabled on AMD GPUs while rendering artifacts are being resolved.";
	if (smokeQuality && !caps.smokeEnabled)
		return "Enable smoke before changing its quality.";
	return {};
}

NRISettingChangeDecision NRIResolveSettingChange(const NRISettingsCapabilities& caps, const char* setting, int requested, int previous)
{
	if (setting == nullptr || !caps.deviceKnown)
		return { requested, {} };
	if (!caps.presentationKnown && (std::strcmp(setting, "nri_ptoutputmode") == 0 || std::strcmp(setting, "nri_framegen") == 0))
		return { requested, {} };
	const auto reason = NRIEvaluateSettingAvailability(caps, setting, requested);
	if (reason.empty())
		return { requested, {} };
	// Quality is retained while smoke is disabled. Other previous settings
	// might themselves have become invalid after a display/backend change.
	if (std::strcmp(setting, "nri_ptsmokeworkprofile") != 0 && !NRIEvaluateSettingAvailability(caps, setting, previous).empty())
		previous = 0;
	return { previous, reason };
}
