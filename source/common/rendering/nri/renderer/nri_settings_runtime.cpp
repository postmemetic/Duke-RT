#include "nri_settings_availability.h"
#include "nri_settings_profiles.h"
#include "nri_cvars.h"
#include "../system/nri_renderdevice.h"
#include "printf.h"

namespace
{
	NRISettingsCapabilities gCapabilities;

	void ReportFallback(const char* setting, int value, const std::string& reason)
	{
		Printf(PRINT_NOTIFY, "NRI settings: %s=%d. %s\n", setting, value, reason.c_str());
	}
}

NRISettingsCapabilities NRIGetSettingsCapabilities()
{
	auto caps = gCapabilities;
	caps.smokeEnabled = !!nri_ptsmoke;
	return caps;
}

std::string NRIGetSettingUnavailableReason(const char* setting, int value)
{
	return NRIEvaluateSettingAvailability(NRIGetSettingsCapabilities(), setting, value);
}

bool NRIIsSmokeSupported()
{
	return !gCapabilities.deviceKnown || !gCapabilities.amd;
}

void NRIResetSettingsAvailability()
{
	gCapabilities = {};
}

int NRIValidateSettingChange(const char* setting, int requested, int& previous)
{
	const auto decision = NRIResolveSettingChange(NRIGetSettingsCapabilities(), setting, requested, previous);
	previous = decision.value;
	if (requested != decision.value)
		Printf(PRINT_NOTIFY, "%s\n", decision.unavailableReason.c_str());
	return decision.value;
}

void NRIRefreshSettingsAvailability(const NRIRenderDevice& device)
{
	const auto previous = gCapabilities;
	gCapabilities = device.BuildSettingsCapabilities();
	if (!gCapabilities.deviceKnown)
		return;
	if (!previous.deviceKnown || previous.presentationKnown != gCapabilities.presentationKnown || previous.hdr != gCapabilities.hdr)
	{
		Printf("NRI settings availability: amd=%s dlss=%s dlrr=%s fsr=%s hdr=%s display_known=%s smoke=%s framegen_runtime=%s\n",
			gCapabilities.amd ? "yes" : "no", gCapabilities.dlss ? "yes" : "no",
			gCapabilities.dlrr ? "yes" : "no", gCapabilities.fsr ? "yes" : "no",
			gCapabilities.hdr ? "yes" : "no", gCapabilities.presentationKnown ? "yes" : "no",
			NRIIsSmokeSupported() ? "supported" : "disabled-on-amd", gCapabilities.framegenRuntime ? "yes" : "no");
	}

	if (nri_ptsmoke && !NRIIsSmokeSupported())
	{
		const auto reason = NRIGetSettingUnavailableReason("nri_ptsmoke", 1);
		nri_ptsmoke = false;
		ReportFallback("nri_ptsmoke", 0, reason);
	}
	const auto upscalerReason = NRIGetSettingUnavailableReason("nri_upscaler", nri_upscaler);
	if (!upscalerReason.empty())
	{
		const int fallback = gCapabilities.fsr ? 4 : 0;
		nri_upscaler = fallback;
		// A saved DLRR profile turns NRD off. Its replacement needs denoising.
		nri_denoise = true;
		ReportFallback("nri_upscaler", fallback, upscalerReason);
	}
	if (gCapabilities.presentationKnown)
	{
		const auto outputReason = NRIGetSettingUnavailableReason("nri_ptoutputmode", nri_ptoutputmode);
		if (!outputReason.empty())
		{
			nri_ptoutputmode = 0;
			ReportFallback("nri_ptoutputmode", 0, outputReason);
		}
		const auto framegenReason = NRIGetSettingUnavailableReason("nri_framegen", nri_framegen);
		if (!framegenReason.empty())
		{
			nri_framegen = false;
			ReportFallback("nri_framegen", 0, framegenReason);
		}
	}
	NRIReconcileSettingsProfileAvailability(gCapabilities);
}
