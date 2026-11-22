#include "nri_settings_profiles.h"
#include "../system/nri_profile_transition.h"
#include "nri_settings_profile_policy.h"
#include "nri_cvars.h"
#include "printf.h"

namespace
{
	constexpr const char* kMirrorWarning =
		"Warning: mirrors are currently bugged when ray reconstruction is not used.";
	constexpr const char* kMirrorMenuWarning =
		"Warning: Ray reconstruction is recommended due to visual bugs with other upscalers/denoisers";
	bool gSkipInitialProfileApply = true;
	bool gSetProfileIndexOnly = false;
	int gPreviousProfile = kNRIHardwareRecommended;

	void SyncProfileWarning(const NRISettingsProfilePreset& preset)
	{
		nri_settingsprofilewarning = preset.mirrorWarning ? kMirrorMenuWarning : "";
	}

	void ApplyProfile(const NRISettingsProfilePlan& plan)
	{
		const auto& preset = plan.preset;
		if (NRIGetSettingsCapabilities().presentationKnown)
			nri_profile::Begin(preset.name);
		SyncProfileWarning(preset);
		nri_upscaler = preset.upscaler;
		nri_postsharpen = plan.nativeTemporalAA ? 1 : 0;
		nri_upscalermode = preset.upscalerMode;
		nri_ptoutputmode = preset.outputMode;
		nri_pttaa = plan.nativeTemporalAA;
		nri_denoise = preset.denoise;
		nri_nrddenoiser = 1; // RELAX
		nri_validation = false;
		nri_apivalidation = false;
		nri_dred = false;
		nri_framegen = false;
		Printf(PRINT_NOTIFY, "Applied settings profile: %s\n", preset.name);
		if (plan.sdrFallback)
			Printf(PRINT_NOTIFY, "HDR is unavailable on the current display; this profile is using SDR.\n");
		if (preset.mirrorWarning)
			Printf(PRINT_NOTIFY, "%s\n", kMirrorWarning);
	}
}

CVAR(String, nri_settingsprofilewarning, "Warning: Ray reconstruction is recommended due to visual bugs with other upscalers/denoisers", CVAR_GLOBALCONFIG)

CUSTOM_CVAR(Int, nri_settingsprofile, kNRIHardwareRecommended, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (gSetProfileIndexOnly)
		return;
	const int index = NRIClampSettingsProfile(self);
	const auto plan = NRIBuildSettingsProfilePlan(NRIGetSettingsCapabilities(), index);
	if (!plan.unavailableReason.empty())
	{
		Printf(PRINT_NOTIFY, "%s\n", plan.unavailableReason.c_str());
		gSetProfileIndexOnly = true;
		self = gPreviousProfile;
		gSetProfileIndexOnly = false;
		return;
	}
	if ((int)self != index)
	{
		gSetProfileIndexOnly = true;
		self = index;
		gSetProfileIndexOnly = false;
	}
	gPreviousProfile = index;
	SyncProfileWarning(plan.preset);
	if (gSkipInitialProfileApply)
	{
		gSkipInitialProfileApply = false;
		// Command-line and autoexec settings are read after device creation.
		// Resolve any defaults that depend on their final provider as well.
		const auto caps = NRIGetSettingsCapabilities();
		if (caps.deviceKnown) NRIInitializeSettingsDefaults(caps);
		return;
	}
	// The complete plan is checked before touching any constituent setting.
	ApplyProfile(plan);
}

void NRIInitializeSettingsDefaults(const NRISettingsCapabilities& capabilities)
{
	// Config, recovery and console writes clear CVAR_ISDEFAULT. The CVar default
	// setter fills missing values while retaining those explicit choices.
	auto plan = NRIBuildSettingsProfilePlan(capabilities, nri_settingsprofile);
	bool recommended = nri_settingsprofile == kNRIHardwareRecommended;
	if (!plan.unavailableReason.empty())
	{
		plan = NRIBuildSettingsProfilePlan(capabilities, kNRIHardwareRecommended);
		recommended = true;
	}
	const auto& preset = plan.preset;
	nri_upscaler->SetGenericRepDefault(preset.upscaler, CVAR_Int);
	// A saved provider from another GPU is reconciled by the settings runtime.
	int provider = nri_upscaler;
	if (!NRIEvaluateSettingAvailability(capabilities, "nri_upscaler", provider).empty())
		provider = capabilities.fsr ? 4 : 0;
	const bool native = recommended && provider <= 1;
	nri_upscalermode->SetGenericRepDefault(recommended ? NRIRecommendedUpscalerMode(capabilities, provider) : preset.upscalerMode, CVAR_Int);
	nri_ptoutputmode->SetGenericRepDefault(preset.outputMode, CVAR_Int);
	nri_denoise->SetGenericRepDefault(provider != 3, CVAR_Bool);
	nri_pttaa->SetGenericRepDefault(native, CVAR_Bool);
	nri_postsharpen->SetGenericRepDefault(native ? 1 : 0, CVAR_Int);
	nri_settingsprofilewarning = provider == 3 ? "" : kMirrorMenuWarning;
}

void NRIReconcileSettingsProfileAvailability(const NRISettingsCapabilities& capabilities)
{
	const auto plan = NRIBuildSettingsProfilePlan(capabilities, nri_settingsprofile);
	if (plan.unavailableReason.empty())
		return;
	Printf(PRINT_NOTIFY, "%s The saved profile selection has been cleared; supported individual settings are retained.\n",
		plan.unavailableReason.c_str());
	gSetProfileIndexOnly = true;
	nri_settingsprofile = 0;
	gSetProfileIndexOnly = false;
	gPreviousProfile = 0;
	SyncProfileWarning(kNRISettingsProfilePresets[0]);
}

void NRIApplySafeSettingsProfileForRecovery()
{
	gSetProfileIndexOnly = true;
	nri_settingsprofile = 0;
	gSetProfileIndexOnly = false;
	gPreviousProfile = 0;
	ApplyProfile(NRIBuildSettingsProfilePlan(NRIGetSettingsCapabilities(), 0));
}
