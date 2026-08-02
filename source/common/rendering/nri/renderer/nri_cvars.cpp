#include "nri_cvars.h"

#include "nri_settings_profiles.h"
#include "../scene/nri_map_builder.h"
#include "../system/nri_renderdevice.h"
#include "printf.h"
#include "v_video.h"

#include <algorithm>
#include <iterator>

namespace
{
	static constexpr int kPtDebugMenuModes[] = {
		0, 1, 2, 3, 4, 5,
		9, 10, 11, 12,
		16, 17, 18, 19,
		21, 22, 24, 25,
		26, 27, 28, 29,
		33, 34, 45
	};

	int ClampPtDebugMenuIndex(int index)
	{
		return std::clamp(index, 0, (int)std::size(kPtDebugMenuModes) - 1);
	}

	int FindPtDebugMenuIndex(int debugMode)
	{
		for (int i = 0; i < (int)std::size(kPtDebugMenuModes); ++i)
		{
			if (kPtDebugMenuModes[i] == debugMode)
			{
				return i;
			}
		}

		return -1;
	}

	int ResolvePtDebugModeFromMenuIndex(int index)
	{
		return kPtDebugMenuModes[ClampPtDebugMenuIndex(index)];
	}

	int ResolvePtDebugMenuIndexFromMode(int debugMode)
	{
		const int index = FindPtDebugMenuIndex(debugMode);
		return index >= 0 ? index : 0;
	}

	bool gSyncingPtDebugMenu = false;

	NRIRenderDevice* GetActiveNriRenderDeviceForCVar()
	{
		return screen != nullptr && screen->Backend() == 4 ? static_cast<NRIRenderDevice*>(screen) : nullptr;
	}

	void RequestActiveFrameGenerationSwapChainRefresh()
	{
		if (NRIRenderDevice* frameBuffer = GetActiveNriRenderDeviceForCVar())
		{
			frameBuffer->RequestSwapChainRefresh("framegen-settings-change", true);
		}
	}

	void NotifyActiveGlowControlChange()
	{
		if (NRIRenderDevice* frameBuffer = GetActiveNriRenderDeviceForCVar())
		{
			frameBuffer->NotifyPathTracingGlowControlChange();
		}
	}

	void NotifyActiveMaterialLightingCalibrationChange()
	{
		if (NRIRenderDevice* frameBuffer = GetActiveNriRenderDeviceForCVar())
		{
			frameBuffer->NotifyPathTracingMaterialLightingCalibrationChange();
		}
	}

	void NotifyActiveVoxelNormalBlendChange()
	{
		if (NRIRenderDevice* frameBuffer = GetActiveNriRenderDeviceForCVar())
		{
			frameBuffer->NotifyPathTracingCameraCut("voxel-normal-blend-change");
		}
	}

	void NotifyActiveAnalyticLightSettingsChange()
	{
		if (NRIRenderDevice* frameBuffer = GetActiveNriRenderDeviceForCVar())
		{
			frameBuffer->NotifyPathTracingAnalyticLightSettingsChange();
		}
	}

	void NotifyActiveDebugSphereTessellationChange()
	{
		if (NRIRenderDevice* frameBuffer = GetActiveNriRenderDeviceForCVar())
		{
			frameBuffer->NotifyPathTracingDebugSphereTessellationChange();
		}
	}

	enum NRISettingsProfile
	{
		NRI_SETTINGS_PROFILE_SAFE = 0,
		NRI_SETTINGS_PROFILE_DLRR_FAST,
		NRI_SETTINGS_PROFILE_DLRR_MEDIUM,
		NRI_SETTINGS_PROFILE_DLRR_BEAUTIFUL,
		NRI_SETTINGS_PROFILE_DLSS_SR_FAST,
		NRI_SETTINGS_PROFILE_DLSS_SR_MEDIUM,
		NRI_SETTINGS_PROFILE_DLSS_SR_BEAUTIFUL,
	};

	struct NRISettingsProfilePreset
	{
		const char* name;
		int upscaler;
		int upscalerMode;
		int outputMode;
		bool denoise;
		const char* warning;
	};

	constexpr int kNRIUpscalerOff = 0;
	constexpr int kNRIUpscalerDlssSr = 2;
	constexpr int kNRIUpscalerDlrr = 3;

	constexpr int kNRIUpscalerModeNative = 0;
	constexpr int kNRIUpscalerModeQuality = 2;
	constexpr int kNRIUpscalerModeBalanced = 3;

	constexpr int kNRIOutputSdr = 0;
	constexpr int kNRIOutputHdr = 1;

	constexpr int kNRDDenoiserRelax = 1;

	constexpr const char* kMirrorsWithoutRayReconstructionWarning =
		"Warning: mirrors are currently bugged when ray reconstruction is not used.";
	constexpr const char* kMirrorsWithoutRayReconstructionMenuWarning =
		"Warning: Ray reconstruction is recommended due to visual bugs with other upscalers/denoisers";

	constexpr NRISettingsProfilePreset kNRISettingsProfilePresets[] = {
		{ "Safe Mode", kNRIUpscalerOff, kNRIUpscalerModeNative, kNRIOutputSdr, true, kMirrorsWithoutRayReconstructionWarning },
		{ "Fast Preset - DLRR", kNRIUpscalerDlrr, kNRIUpscalerModeBalanced, kNRIOutputHdr, false, nullptr },
		{ "Medium Preset - DLRR", kNRIUpscalerDlrr, kNRIUpscalerModeQuality, kNRIOutputHdr, false, nullptr },
		{ "Beautiful Preset - DLRR", kNRIUpscalerDlrr, kNRIUpscalerModeNative, kNRIOutputHdr, false, nullptr },
		{ "Fast Preset - DLSS-SR", kNRIUpscalerDlssSr, kNRIUpscalerModeBalanced, kNRIOutputHdr, true, kMirrorsWithoutRayReconstructionWarning },
		{ "Medium Preset - DLSS-SR", kNRIUpscalerDlssSr, kNRIUpscalerModeQuality, kNRIOutputHdr, true, kMirrorsWithoutRayReconstructionWarning },
		{ "Beautiful Preset - DLSS-SR", kNRIUpscalerDlssSr, kNRIUpscalerModeNative, kNRIOutputHdr, true, kMirrorsWithoutRayReconstructionWarning },
	};

	bool gSkipInitialProfileApply = true;

	int ClampNRISettingsProfile(int profile)
	{
		return std::clamp(profile, 0, (int)std::size(kNRISettingsProfilePresets) - 1);
	}

	void SyncNRISettingsProfileWarning(const NRISettingsProfilePreset& preset)
	{
		nri_settingsprofilewarning = preset.warning != nullptr ? kMirrorsWithoutRayReconstructionMenuWarning : "";
	}

	void ApplyNRISettingsProfile(const NRISettingsProfilePreset& preset)
	{
		SyncNRISettingsProfileWarning(preset);

		nri_upscaler = preset.upscaler;
		nri_postsharpen = 0;
		nri_upscalermode = preset.upscalerMode;
		nri_ptoutputmode = preset.outputMode;
		nri_pttaa = false;

		nri_denoise = preset.denoise;
		nri_nrddenoiser = kNRDDenoiserRelax;

		nri_validation = false;
		nri_apivalidation = false;
		nri_dred = false;

		nri_framegen = false;
		nri_framegenprovider = 0;

		Printf(PRINT_NOTIFY, "Applied settings profile: %s\n", preset.name);
		if (preset.warning != nullptr && preset.warning[0] != '\0')
		{
			Printf(PRINT_NOTIFY, "%s\n", preset.warning);
		}
	}
}


// Moved from source/common/rendering/nri/system/nri_renderdevice.cpp

CVAR(Bool, nri_ptsanity, false, 0)

CVAR(Bool, nri_ptscenedataring, true, 0)

CVAR(Bool, nri_ptscenedataringtrace, false, 0)

CVAR(Bool, nri_ptemissivestabilitytrace, false, 0)
CVAR(Bool, nri_ptindirectradiancecache, false, 0)
CVAR(Bool, nri_ptindirectradiancecacheaccept, false, 0)

CVAR(Bool, nri_ptwaitpresent, true, 0)

CVAR(Bool, nri_ptslowdowntrace, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CUSTOM_CVAR(Int, nri_pttraceframes, 0, 0)
{
	if (self < 0)
	{
		self = 0;
	}
	else if (self > 600)
	{
		self = 600;
	}
}

CUSTOM_CVAR(Int, nri_ptslowdowntraceinterval, 300, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 1)
	{
		self = 1;
	}
	else if (self > 36000)
	{
		self = 36000;
	}
}

CUSTOM_CVAR(Int, nri_ptslowdowntop, 5, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0)
	{
		self = 0;
	}
	else if (self > 16)
	{
		self = 16;
	}
}

CUSTOM_CVAR(Int, nri_ptnudgetrace, 0, 0)
{
	if (self < 0)
	{
		self = 0;
	}
	else if (self > 1)
	{
		self = 1;
	}
}

CUSTOM_CVAR(Int, nri_ptswaptextures, 0, 0)
{
	if (self < 0)
	{
		self = 0;
	}
	else if (self == 1)
	{
		self = 2;
	}
	else if (self > 8)
	{
		self = 8;
	}

	if (auto* frameBuffer = GetActiveNriRenderDeviceForCVar())
	{
		frameBuffer->RequestSwapChainRefresh("debug-swapchain-policy-change", false);
	}
}

CUSTOM_CVAR(Int, nri_ptswapflags, -1, 0)
{
	if (self < -1)
	{
		self = -1;
	}
	else if (self > 3)
	{
		self = 3;
	}

	if (auto* frameBuffer = GetActiveNriRenderDeviceForCVar())
	{
		frameBuffer->RequestSwapChainRefresh("debug-swapchain-policy-change", false);
	}
}


// Moved from source/common/rendering/nri/scene/nri_portal_bridge.cpp

CVAR(Int, nri_ptportaldepth, 3, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CUSTOM_CVAR(Int, nri_ptscenedataringmaxbytes, 0, 0)
{
	if (self < 0)
	{
		self = 0;
	}
}


// Moved from source/common/rendering/nri/scene/nri_scene_bridge.cpp

CVAR(Bool, nri_voxelstats, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Bool, nri_ptvoxelactorstatetrace, false, 0)

CVAR(Bool, nri_ptvoxelactorlifecycle, true, 0)

CVAR(Int, nri_ptvoxelactorstatetracelimit, 12000, 0)

CVAR(Int, nri_ptvoxeltrianglebudget, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptvoxelmaxtriangles, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptvoxelcaptureactors, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptvoxelpersistentpromoteframes, 3, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptvoxelmeshbuilds, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Bool, nri_ptvoxelcompute, true, 0)

CVAR(Int, nri_ptvoxelcomputemode, 6, 0)

CVAR(Int, nri_ptvoxelcomputealgorithm, 3, 0)

CVAR(Int, nri_ptvoxelcomputemaxjobs, 4, 0)

CVAR(Int, nri_ptvoxelcomputeruntimemaxjobs, 2, 0)

CVAR(Int, nri_ptvoxelcomputedirectmaxprimitives, 0, 0)

CVAR(Int, nri_ptvoxelcomputetrace, 0, 0)

CVAR(Bool, nri_ptvoxelcomputedirectgpu, true, 0)

CVAR(Bool, nri_ptvoxelcomputerawarchive, true, 0)

CVAR(Bool, nri_ptvoxelcomputedirectpublish, true, 0)

CVAR(Bool, nri_ptvoxelcomputevalidatefullreadback, false, 0)

CVAR(Bool, nri_ptvoxelcomputeforcecpu, false, 0)

CVAR(Bool, nri_ptvoxelcomputedeferdynamic, true, 0)

CVAR(Bool, nri_ptvoxelcomputerawpreload, true, 0)

CVAR(Int, nri_ptvoxelcomputerawpreloadmaxsources, 0, 0)

CVAR(Int, nri_ptvoxelcomputerawpreloadmaxbytes, 0, 0)

CVAR(Bool, nri_ptvoxelcomputepreload, true, 0)

CVAR(Bool, nri_ptvoxelcomputepreloaddryrun, false, 0)

CVAR(Int, nri_ptvoxelcomputepreloadtrace, 0, 0)

CVAR(Bool, nri_ptvoxelcomputepreloadrequired, true, 0)

CVAR(Bool, nri_ptvoxelcomputepreloadoptional, true, 0)

CVAR(Bool, nri_ptvoxelcomputepreloadmaterials, true, 0)

CVAR(Bool, nri_ptvoxelcomputepreloadstrict, false, 0)

CVAR(String, nri_ptvoxelcomputepreloadterminalcommand, "", 0)

CVAR(String, nri_ptvoxelcomputepreloadreleasecommand, "", 0)

CVAR(Bool, nri_ptvoxelcomputepreloadbalancedoptional, false, 0)

CVAR(Int, nri_ptvoxelcomputepreloadbalancedmaxpriority, 1, 0)

CVAR(Int, nri_ptvoxelcomputepreloadbalancedminprimitives, 100000, 0)

CVAR(Int, nri_ptvoxelcomputepreloadruntimeprobemod, 0, 0)

CVAR(Int, nri_ptvoxelcomputepreloadruntimeproberem, 0, 0)

CVAR(Int, nri_ptvoxelcomputepreloadruntimewithholdmod, 1, 0)

CVAR(Int, nri_ptvoxelcomputepreloadruntimewithholdrem, 0, 0)

CVAR(Int, nri_ptvoxelcomputepreloadruntimecaptureframes, 0, 0)

CVAR(Int, nri_ptvoxelcomputepreloadmaxms, 0, 0)

CVAR(Int, nri_ptvoxelcomputepreloadmaxjobs, 0, 0)

CVAR(Int, nri_ptvoxelcomputepreloadmaxblas, 0, 0)

CVAR(Int, nri_ptvoxelcomputepreloadmaxbytes, 0, 0)

CVAR(Int, nri_ptvoxelcomputepreloadmaxmaterialrows, 0, 0)

CVAR(Int, nri_ptvoxelcomputepreloadwatchdogms, 180000, 0)

CVAR(Int, nri_ptvoxelcomputepreloadpeakpercent, 175, 0)

CVAR(Int, nri_ptvoxelcomputepreloadminreservemb, 1024, 0)

CVAR(Int, nri_ptvoxelblaspolicy, 3, 0)

CVAR(Bool, nri_ptvoxelarenapresize, false, 0)

CVAR(Bool, nri_ptvoxelblascompact, false, 0)

CVAR(Int, nri_ptloadingtrace, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptpreloadmaxsubmitspertick, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptpreloadmaterialtexturespersubmit, 64, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptpreloadmaterialbytespersubmit, 64 * 1024 * 1024, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptpreloadmaterialmaxms, 100, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptloadingsettingsversion, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Bool, nri_ptloadingmutationbaseline, false, 0)

CVAR(Bool, nri_ptloadingvoxelcpu, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Bool, nri_ptloadingvoxelgpu, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Bool, nri_ptloadingvoxelblockoptional, true, 0)

CVAR(Bool, nri_ptloadingvoxelgpuwhitelistonly, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptloadingvoxelgpuminprims, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptloadingvoxelgpumaxprims, 9000000, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptloadingvoxelgpumaxbytes, 1630491936, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptloadingvoxelgpumaxvariants, 256, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptloadingvoxelactors, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptloadingvoxelvariants, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptloadingvoxelvariantprims, 2000000, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptloadingvoxelpicrange, 64, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptloadingvoxelcpubudget, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptloadingvoxelcpumaxvariants, 256, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptloadingvoxelcpumaxprims, 9000000, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptloadingvoxelcpumaxms, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptloadingvoxelgpubudget, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Bool, nri_ptloadingvoxellist, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)


// Moved from source/common/rendering/nri/renderer/nri_exposure.cpp

CVAR(Bool, nri_ptautoexposure, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Bool, nri_pthdrautoexposure, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Bool, nri_ptautoexposurefreeze, false, 0)

CVAR(Bool, nri_ptautoexposurestats, false, 0)

CUSTOM_CVAR(Int, nri_ptautoexposuremetering, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0)
	{
		self = 0;
	}
	else if (self > 2)
	{
		self = 2;
	}
}

CUSTOM_CVAR(Int, nri_ptautoexposurebins, 256, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 16)
	{
		self = 16;
	}
	else if (self > 256)
	{
		self = 256;
	}
}

CUSTOM_CVAR(Int, nri_ptautoexposuresamplestep, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 1)
	{
		self = 1;
	}
	else if (self > 8)
	{
		self = 8;
	}
}

CUSTOM_CVAR(Float, nri_ptautoexposuretarget, 0.03225f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.02f)
	{
		self = 0.02f;
	}
	else if (self > 1.0f)
	{
		self = 1.0f;
	}
}

CUSTOM_CVAR(Float, nri_pthdrautoexposuretarget, 0.0445f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.02f)
	{
		self = 0.02f;
	}
	else if (self > 1.0f)
	{
		self = 1.0f;
	}
}

CUSTOM_CVAR(Float, nri_ptautoexposuremin, 2.74561f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.03125f)
	{
		self = 0.03125f;
	}
	else if (self > 8.0f)
	{
		self = 8.0f;
	}
}

CUSTOM_CVAR(Float, nri_pthdrautoexposuremin, 0.604004f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.03125f)
	{
		self = 0.03125f;
	}
	else if (self > 8.0f)
	{
		self = 8.0f;
	}
}

CUSTOM_CVAR(Float, nri_ptautoexposuremax, 4.00977f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.125f)
	{
		self = 0.125f;
	}
	else if (self > 32.0f)
	{
		self = 32.0f;
	}
}

CUSTOM_CVAR(Float, nri_pthdrautoexposuremax, 7.09766f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.125f)
	{
		self = 0.125f;
	}
	else if (self > 32.0f)
	{
		self = 32.0f;
	}
}

CUSTOM_CVAR(Float, nri_ptautoexposurebias, 0.469531f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.125f)
	{
		self = 0.125f;
	}
	else if (self > 8.0f)
	{
		self = 8.0f;
	}
}

CUSTOM_CVAR(Float, nri_pthdrautoexposurebias, 0.371094f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.125f)
	{
		self = 0.125f;
	}
	else if (self > 8.0f)
	{
		self = 8.0f;
	}
}

CUSTOM_CVAR(Float, nri_ptautoexposurelowpercentile, 1.01563f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
	else if (self > 99.0f)
	{
		self = 99.0f;
	}
}

CUSTOM_CVAR(Float, nri_ptautoexposurehighpercentile, 98.9844f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 1.0f)
	{
		self = 1.0f;
	}
	else if (self > 100.0f)
	{
		self = 100.0f;
	}
}

CUSTOM_CVAR(Float, nri_ptautoexposureadaptup, 3.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
	else if (self > 16.0f)
	{
		self = 16.0f;
	}
}

CUSTOM_CVAR(Float, nri_ptautoexposureadaptdown, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
	else if (self > 16.0f)
	{
		self = 16.0f;
	}
}

CVAR(Bool, nri_ptbloom, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CUSTOM_CVAR(Float, nri_ptbloomintensity, 0.053125f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
	else if (self > 1.0f)
	{
		self = 1.0f;
	}
}

CUSTOM_CVAR(Float, nri_ptbloomsigma, 0.01f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
	else if (self > 1.0f)
	{
		self = 1.0f;
	}
}

CUSTOM_CVAR(Float, nri_ptbloomcutoff, 0.65f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
	else if (self > 16.0f)
	{
		self = 16.0f;
	}
}

CUSTOM_CVAR(Float, nri_ptbloomfuzziness, 0.15f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
	else if (self > 1.0f)
	{
		self = 1.0f;
	}
}

CUSTOM_CVAR(Int, nri_ptbloomlevels, 6, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 1)
	{
		self = 1;
	}
	else if (self > 8)
	{
		self = 8;
	}
}

CVAR(Bool, nri_ptbloomenergyconstrained, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CUSTOM_CVAR(Int, nri_ptbloomdebug, 0, 0)
{
	if (self < 0)
	{
		self = 0;
	}
	else if (self > 3)
	{
		self = 3;
	}
}


// Moved from source/common/rendering/nri/renderer/nri_renderer.cpp

CUSTOM_CVAR(Int, nri_ptdebug, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	const int resolvedMode = ResolvePtDebugModeFromMenuIndex(ResolvePtDebugMenuIndexFromMode(self));
	if (self != resolvedMode)
	{
		self = resolvedMode;
		return;
	}

	if (gSyncingPtDebugMenu)
	{
		return;
	}

	gSyncingPtDebugMenu = true;
	nri_ptdebugmenu = ResolvePtDebugMenuIndexFromMode(self);
	gSyncingPtDebugMenu = false;
}

CUSTOM_CVAR(Int, nri_ptdebugmenu, 0, CVAR_GLOBALCONFIG)
{
	const int clampedIndex = ClampPtDebugMenuIndex(self);
	if (self != clampedIndex)
	{
		self = clampedIndex;
		return;
	}

	if (gSyncingPtDebugMenu)
	{
		return;
	}

	gSyncingPtDebugMenu = true;
	nri_ptdebug = ResolvePtDebugModeFromMenuIndex(self);
	gSyncingPtDebugMenu = false;
}

CVAR(Bool, nri_denoise, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_nrddenoiser, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_upscaler, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_postsharpen, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_upscalermode, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Bool, nri_pttaa, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Float, nri_renderscale, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Float, nri_sharpness, 0.1375f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Bool, nri_ptselftest, false, 0)

CVAR(String, nri_ptsmokemenuwarning, "", 0)
CUSTOM_CVAR(Bool, nri_ptsmoke, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	nri_ptsmokemenuwarning = self
		? "Smoke is very taxing on your GPU, especially at Ultra settings"
		: "";
}
CVAR(Int, nri_ptsmokequality, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptsmokeworkprofile, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptsmokeparticles, 8192, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptsmokefroxelpixels, 16, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptsmokefroxelz, 48, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_ptsmokefroxelmaxdistance, 4096.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptsmokecolumncapacity, 64, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptsmokesimrate, 60, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptsmokemaxsubsteps, 4, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_ptsmoketimescale, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_ptsmokewindx, 5.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_ptsmokewindy, 20.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_ptsmokewindz, 5.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_ptsmokedensityscale, 5.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_ptsmokeradiancescale, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_ptsmokeextinctionthreshold, 0.0f, 0)
CVAR(Float, nri_ptsmokeextinctionknee, 0.0f, 0)
CVAR(Float, nri_ptsmokeextinctiongamma, 1.0f, 0)
CVAR(Float, nri_ptsmokeextinctionreference, 1.0f, 0)
CVAR(Float, nri_ptsmokeextinctionshoulder, 0.0f, 0)
CVAR(Float, nri_ptsmokethincolorr, 1.0f, 0)
CVAR(Float, nri_ptsmokethincolorg, 1.0f, 0)
CVAR(Float, nri_ptsmokethincolorb, 1.0f, 0)
CVAR(Float, nri_ptsmokecorecolorr, 1.0f, 0)
CVAR(Float, nri_ptsmokecorecolorg, 1.0f, 0)
CVAR(Float, nri_ptsmokecorecolorb, 1.0f, 0)
CVAR(Float, nri_ptsmokecolorpivot, 0.05f, 0)
CVAR(Float, nri_ptsmokethermallow, 1.0f, 0)
CVAR(Float, nri_ptsmokethermalhigh, 4.0f, 0)
CVAR(Float, nri_ptsmokethermaltintr, 1.0f, 0)
CVAR(Float, nri_ptsmokethermaltintg, 1.0f, 0)
CVAR(Float, nri_ptsmokethermaltintb, 1.0f, 0)
CVAR(Float, nri_ptsmokethermalglowr, 0.0f, 0)
CVAR(Float, nri_ptsmokethermalglowg, 0.0f, 0)
CVAR(Float, nri_ptsmokethermalglowb, 0.0f, 0)
CVAR(Float, nri_ptsmokegradientpivot, 0.5f, 0)
CVAR(Float, nri_ptsmokegradientwidth, 0.25f, 0)
CVAR(Float, nri_ptsmokeedgesculpt, 0.0f, 0)
CVAR(Float, nri_ptsmokeedgepowder, 0.0f, 0)
CVAR(Float, nri_ptsmokeedgetintr, 1.0f, 0)
CVAR(Float, nri_ptsmokeedgetintg, 1.0f, 0)
CVAR(Float, nri_ptsmokeedgetintb, 1.0f, 0)
CVAR(Float, nri_ptsmokeedgetintstrength, 0.0f, 0)
CVAR(Float, nri_ptsmokeduallobeweight, 0.0f, 0)
CVAR(Float, nri_ptsmokeduallobeg, 0.75f, 0)
CVAR(Float, nri_ptsmokerimstrength, 0.0f, 0)
CVAR(Float, nri_ptsmokerimgain, 0.0f, 0)
CVAR(Float, nri_ptsmokeradianceedgechroma, 0.0f, 0)
CVAR(Float, nri_ptsmokeradiancecavitycontrast, 0.0f, 0)
CVAR(Float, nri_ptsmokeradiancedesaturation, 0.0f, 0)
CVAR(Float, nri_ptsmokeradianceconfidence, 0.25f, 0)
CVAR(Float, nri_ptsmokeradiancedirectionality, 0.15f, 0)
CVAR(Bool, nri_ptsmokepointlights, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_ptsmokedirectionallight, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_ptsmokeemissivelights, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptsmokeemissivereuse, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_ptsmokeemissivereference, false, 0)
CVAR(Int, nri_ptsmokeemissivepoints, 4, 0)
CVAR(Int, nri_ptsmokeemissivecandidate, -1, 0)
CVAR(Int, nri_ptsmokeemissivebackend, 2, 0)
CVAR(Bool, nri_ptsmokeemissiveworldfilter, false, 0)
CVAR(Bool, nri_ptsmokeemissivelocal, true, 0)
CVAR(Int, nri_ptsmokeemissiveworlddebug, 0, 0)
CVAR(Int, nri_ptsmokeworldpartitions, 1, 0)
CVAR(Int, nri_ptsmokeworldnewcells, 8192, 0)
CVAR(Int, nri_ptsmokeworldmaintenancecells, 32768, 0)
CVAR(Int, nri_ptsmokeworldradiancemaxage, 16, 0)
CVAR(Bool, nri_ptsmokeemissivelegacygatherdisable, false, 0)
CVAR(Bool, nri_ptsmokeemissivequarterkey, false, 0)
CVAR(Float, nri_ptsmokeemissiveclamp, 32.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptsmokedirectreuse, 2, 0)
CVAR(Int, nri_ptsmokedirectreference, 1, 0)
CVAR(Bool, nri_ptsmokevolumehistory, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptsmokedlrrmode, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_ptsmokeindirect, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptsmokeindirectcache, 3, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_ptsmokemultiplescatter, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_ptsmokemultiplescatterscale, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptsmokemultiplescatteriterations, 0, 0)
CVAR(Int, nri_ptsmokemultiplescatterdebug, 0, 0)
CVAR(Bool, nri_ptsmokeselfshadow, false, 0)
CVAR(Int, nri_ptsmokeselfshadowdebug, 0, 0)
CVAR(Int, nri_ptsmokelightmode, 3, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptsmokelightsamples, 4, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptsmokemaxlightcandidates, 8, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_ptsmokefilteredvisibility, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptsmokedebug, 0, 0)
CVAR(Int, nri_ptsmoketrace, 0, 0)
CVAR(Bool, nri_ptsmokereadback, false, 0)
CVAR(Bool, nri_ptsmokeviewcompare, false, 0)
CVAR(Int, nri_ptsmokeviewroute, 0, 0)
CVAR(Float, nri_ptsmokeindirectscale, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptsmokerepresentation, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptsmokegridbricks, 512, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_ptsmokedormantgrid, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_ptsmokegridcellsize, 8.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_ptsmokegridbuoyancy, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_ptsmokegridcurlevolution, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_ptsmokegridvorticity, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_ptsmokegridvelocitydamping, 0.15f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_ptsmokegridwindcoupling, 0.5f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_ptsmokegriddensityhalflifescale, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_ptsmokegridcoolingscale, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_ptsmokegridmaxvelocity, 128.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_ptsmokegridmaxbacktrace, 32.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nri_ptsmokegridactivethreshold, 0.0001f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nri_ptsmokegridreclaimgrace, 120, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CUSTOM_CVAR(Float, nri_ptemissivelighteditnotifyrange, 2048.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
}

CUSTOM_CVAR(Int, nri_ptmutationworklistvalidate, 0, 0)
{
	if (self < 0)
	{
		self = 0;
	}
	else if (self > 2)
	{
		self = 2;
	}
}

CVAR(Bool, nri_ptzerotickreuse, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nri_ptzerotickreusevalidate, false, 0)

CUSTOM_CVAR(Int, nri_ptscenebufferdirtyrangegap, 256, 0)
{
	if (self < 0)
	{
		self = 0;
	}
}

CUSTOM_CVAR(Int, nri_ptscenebufferrangeuploadmaxranges, 256, 0)
{
	if (self < 1)
	{
		self = 1;
	}
}

CUSTOM_CVAR(Int, nri_ptscenebufferrangeuploadmaxpercent, 75, 0)
{
	if (self < 1)
	{
		self = 1;
	}
	else if (self > 100)
	{
		self = 100;
	}
}

CUSTOM_CVAR(Int, nri_ptsceneuploadvalidateinterval, 0, 0)
{
	if (self < 0)
	{
		self = 0;
	}
}

CUSTOM_CVAR(Int, nri_ptactorspritetrace, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0)
	{
		self = 0;
	}
	else if (self > 2)
	{
		self = 2;
	}
}

CUSTOM_CVAR(Int, nri_ptoutputmode, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0)
	{
		self = 0;
	}
	else if (self > 1)
	{
		self = 1;
	}
}

CUSTOM_CVAR(Int, nri_pttonemap, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0)
	{
		self = 0;
	}
	else if (self > 2)
	{
		self = 2;
	}
}

CUSTOM_CVAR(Float, nri_ptexposure, 1.06016f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.125f)
	{
		self = 0.125f;
	}
	else if (self > 8.0f)
	{
		self = 8.0f;
	}
}

CUSTOM_CVAR(Float, nri_ptcontrast, 1.14688f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.50f)
	{
		self = 0.50f;
	}
	else if (self > 1.50f)
	{
		self = 1.50f;
	}
}

CUSTOM_CVAR(Float, nri_ptsaturation, 1.75f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.00f)
	{
		self = 0.00f;
	}
	else if (self > 2.00f)
	{
		self = 2.00f;
	}
}

CUSTOM_CVAR(Float, nri_ptshoulder, 1.5f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.50f)
	{
		self = 0.50f;
	}
	else if (self > 1.50f)
	{
		self = 1.50f;
	}
}

CUSTOM_CVAR(Float, nri_pttoe, 1.1f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.50f)
	{
		self = 0.50f;
	}
	else if (self > 1.50f)
	{
		self = 1.50f;
	}
}

CUSTOM_CVAR(Float, nri_ptpaperwhite, 300.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 80.0f)
	{
		self = 80.0f;
	}
	else if (self > 400.0f)
	{
		self = 400.0f;
	}
}

CUSTOM_CVAR(Int, nri_pthdrtonemap, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0)
	{
		self = 0;
	}
	else if (self > 2)
	{
		self = 2;
	}
}

CUSTOM_CVAR(Float, nri_pthdrexposure, 0.986328f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.125f)
	{
		self = 0.125f;
	}
	else if (self > 8.0f)
	{
		self = 8.0f;
	}
}

CUSTOM_CVAR(Float, nri_pthdrcontrast, 1.10312f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.50f)
	{
		self = 0.50f;
	}
	else if (self > 1.50f)
	{
		self = 1.50f;
	}
}

CUSTOM_CVAR(Float, nri_pthdrsaturation, 1.75f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.00f)
	{
		self = 0.00f;
	}
	else if (self > 2.00f)
	{
		self = 2.00f;
	}
}

CUSTOM_CVAR(Float, nri_pthdrshoulder, 0.84375f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.50f)
	{
		self = 0.50f;
	}
	else if (self > 1.50f)
	{
		self = 1.50f;
	}
}

CUSTOM_CVAR(Float, nri_pthdrtoe, 1.1f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.50f)
	{
		self = 0.50f;
	}
	else if (self > 1.50f)
	{
		self = 1.50f;
	}
}

CVAR(Bool, nri_ptnightvision, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CUSTOM_CVAR(Float, nri_ptnightvisionexposure, 1.39844f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.25f)
	{
		self = 0.25f;
	}
	else if (self > 4.0f)
	{
		self = 4.0f;
	}
}

CUSTOM_CVAR(Float, nri_ptnightvisioncontrast, 0.971875f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.25f)
	{
		self = 0.25f;
	}
	else if (self > 2.0f)
	{
		self = 2.0f;
	}
}

CUSTOM_CVAR(Float, nri_ptnightvisionsaturation, 1.10625f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
	else if (self > 2.0f)
	{
		self = 2.0f;
	}
}

CUSTOM_CVAR(Float, nri_ptnightvisionred, 0.46875f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
	else if (self > 2.0f)
	{
		self = 2.0f;
	}
}

CUSTOM_CVAR(Float, nri_ptnightvisiongreen, 1.0625f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
	else if (self > 2.0f)
	{
		self = 2.0f;
	}
}

CUSTOM_CVAR(Float, nri_ptnightvisionblue, 0.16875f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
	else if (self > 2.0f)
	{
		self = 2.0f;
	}
}

CVAR(Bool, nri_validation, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CUSTOM_CVAR(Bool, nri_framegen, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	RequestActiveFrameGenerationSwapChainRefresh();
}

CUSTOM_CVAR(Int, nri_ptspherelongs, 256, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 8)
	{
		self = 8;
		return;
	}
	else if (self > 256)
	{
		self = 256;
		return;
	}
	NotifyActiveDebugSphereTessellationChange();
}

CUSTOM_CVAR(Int, nri_ptspherelats, 128, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 4)
	{
		self = 4;
		return;
	}
	else if (self > 128)
	{
		self = 128;
		return;
	}
	NotifyActiveDebugSphereTessellationChange();
}

CUSTOM_CVAR(Int, nri_framegenprovider, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0)
	{
		self = 0;
	}
	else if (self > 1)
	{
		self = 1;
	}

	RequestActiveFrameGenerationSwapChainRefresh();
}

CUSTOM_CVAR(Int, nri_framegenui, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0)
	{
		self = 0;
	}
	else if (self > 3)
	{
		self = 3;
	}
}

CVAR(Bool, nri_framegenasync, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CUSTOM_CVAR(Bool, nri_framegenlatency, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	RequestActiveFrameGenerationSwapChainRefresh();
}

CVAR(Int, nri_nrdmaxframes, 13, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_nrdfastframes, 20, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_nrdstabilizationframes, 48, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Bool, nri_nrdantifirefly, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_nrdhitdistrecon, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_nrdsplit, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Float, nri_nrdfasthistorysigma, 2.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Float, nri_nrdprepassdiffuse, 3.45f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Float, nri_nrdprepassspecular, 3.675f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Float, nri_nrdblurmin, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Float, nri_nrdblurmax, 2.025f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_nrdsigmastabilization, 5, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Float, nri_nrdsigmaplanedistance, 0.0418375f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Bool, nri_apivalidation, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Bool, nri_dred, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Bool, nri_ptbootstrap, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptbootstrapmode, 13, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Bool, nri_ptdirectscene, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Bool, nri_ptdirectionallight, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Float, nri_ptbaseambient, 0.021875f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Float, nri_ptmetalambient, 0.03125f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CUSTOM_CVAR(Float, nri_ptanalyticsoftshadowradius, 4.0f, 0)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
	else if (self > 1024.0f)
	{
		self = 1024.0f;
	}
	NotifyActiveAnalyticLightSettingsChange();
}

CVAR(Int, nri_ptlightbounces, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptmirrorbounces, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptsurfaceprobe, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Bool, nri_pttemporaltrace, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Bool, nri_ptscenestats, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Bool, nri_ptceilingnudge, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CUSTOM_CVAR(Float, nri_ptceilingnudgedistance, 0.01f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
}

CVAR(Int, nri_ptmutationtracechunk, 66, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptmutationtracesector, 198, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptmapmovertrace, 0, 0)

CVAR(Int, nri_ptmapmovershadow, 0, 0)

CVAR(Int, nri_ptmapmovermode, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Bool, nri_ptruntimelinktrace, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Float, nri_ptemissiveminpower, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Float, nri_ptemissiveminsurface, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CUSTOM_CVAR(Float, nri_ptglowscale, 3.025f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
	NotifyActiveGlowControlChange();
}

CUSTOM_CVAR(Float, nri_ptglowreach, 16.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
	NotifyActiveGlowControlChange();
}

CUSTOM_CVAR(Float, nri_ptglowfalloff, 0.847656f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.25f)
	{
		self = 0.25f;
	}
	else if (self > 4.0f)
	{
		self = 4.0f;
	}
	NotifyActiveGlowControlChange();
}

CUSTOM_CVAR(Float, nri_ptglowblend, 0.20625f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
	else if (self > 3.0f)
	{
		self = 3.0f;
	}
	NotifyActiveGlowControlChange();
}

CUSTOM_CVAR(Float, nri_voxelemissionboost, 3.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
	NotifyActiveMaterialLightingCalibrationChange();
}

CUSTOM_CVAR(Float, nri_ptvoxelnormalblend, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
	else if (self > 1.0f)
	{
		self = 1.0f;
	}
	NotifyActiveVoxelNormalBlendChange();
}

CUSTOM_CVAR(Float, nri_ptfullbrightboost, 1.50781f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.50f)
	{
		self = 0.50f;
	}
	else if (self > 8.00f)
	{
		self = 8.00f;
	}
	NotifyActiveMaterialLightingCalibrationChange();
}

CUSTOM_CVAR(Float, nri_ptskybrightness, 0.15f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
	else if (self > 4.0f)
	{
		self = 4.0f;
	}
}

CUSTOM_CVAR(Float, nri_ptsurfacelightminbrightness, 0.2f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
	else if (self > 4.0f)
	{
		self = 4.0f;
	}
}

CVAR(Bool, nri_ptemissivetlas, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Bool, nri_ptemissivefastshadow, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptemissivesamples, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// Session-only policy: 0 preserves the requested count; 1..4 cap only the primary direct loop.
CUSTOM_CVAR(Int, nri_ptemissiveprimarybudget, 2, 0)
{
	if (self < 0 || self > 4)
	{
		self = std::clamp((int)self, 0, 4);
	}
}

// Session-only policy: 0 preserves dual indirect paths; 1 requests probabilistic single-lobe sampling.
CUSTOM_CVAR(Int, nri_ptindirectsampling, 1, 0)
{
	if (self < 0 || self > 1)
	{
		self = std::clamp((int)self, 0, 1);
	}
}

CVAR(Bool, nri_ptsectorlighting, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CUSTOM_CVAR(Float, nri_ptsectorlightmultiplier, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
}

CVAR(Float, nri_ptsectorambientscale, 0.20f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Float, nri_ptsectorhemiscale, 0.12f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Float, nri_ptsectorfogscale, 0.20f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Float, nri_ptsectorclamp, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptsectorfilterpal, -1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptsectorfilterminshade, -128, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptsectorfiltermaxshade, 127, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptsectorfilterlotag, -1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptsectorpulseframes, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Float, nri_ptsectorpulseamount, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CUSTOM_CVAR(Float, nri_ptsectoremissionsignalstrength, 4.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
}

CUSTOM_CVAR(Float, nri_ptsectoremissionresponsemin, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
}

CUSTOM_CVAR(Float, nri_ptsectoremissionresponsemax, 2.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
}

CUSTOM_CVAR(Float, nri_ptsectoremissionlightmin, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
}

CUSTOM_CVAR(Float, nri_ptsectoremissionlightmax, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
}

CUSTOM_CVAR(Float, nri_ptsectoremissionreachmin, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
}

CUSTOM_CVAR(Float, nri_ptsectoremissionreachmax, 1.6f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
}

CUSTOM_CVAR(Float, nri_ptsectoremissionmaterialmin, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
}

CUSTOM_CVAR(Float, nri_ptsectoremissionmaterialmax, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
}

CVAR(Bool, nri_ptvisiblechunkgate, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Bool, nri_ptshaderstats, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Bool, nri_ptstaticsegmentblasbuild, false, 0)

CVAR(Bool, nri_ptstaticsegmentroute, false, 0)

CVAR(Bool, nri_ptdynamicoverlayblasbuild, false, 0)

CVAR(Bool, nri_ptdynamicoverlayblasroute, false, 0)

CVAR(Int, nri_ptdynamicoverlayblasbuilds, 1, 0)


// Moved from source/common/rendering/nri/renderer/nri_renderer_settings.cpp

CVAR(Int, nri_ptpersistentvoxelbuildactors, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptpersistentvoxelbuildprims, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptpersistentvoxelbuildbytes, 4 * 1024 * 1024, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptpersistentvoxeltextureprewarms, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptpersistentvoxeltexturebytes, 1024 * 1024, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptvoxelruntimebudget, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptvoxeladmissionloadvariants, 8, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptvoxeladmissionloadbytes, 64 * 1024 * 1024, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptvoxeladmissionruntimevariants, 4, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptvoxeladmissionruntimebytes, 32 * 1024 * 1024, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptvoxeladmissiongraceframes, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptvoxeladmissiongracevariants, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptvoxelpreloadreadygraceframes, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptvoxeladmitmaxbytesloading, 64 * 1024 * 1024, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptvoxeladmitmaxbytesruntime, 32 * 1024 * 1024, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptvoxeladmitmaxmsloading, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptvoxeladmitmaxmsruntime, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptvoxeladmitmaxblasloading, 8, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptvoxeladmitmaxblasruntime, 4, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptvoxeladmitmaxblasprims, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptvoxeladmitisolateblasprims, 65536, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptvoxelresidentmaxbytes, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptvoxelresidentminheadroombytes, 512 * 1024 * 1024, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptvoxelresidentmaxcoldmaps, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Int, nri_ptvoxelpressureauditframes, 0, 0)

CVAR(Bool, nri_ptvoxeltrimcoldloading, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Bool, nri_ptvoxeltransformkeyed, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Bool, nri_ptvoxelsharedblasbuild, false, 0)

CVAR(Int, nri_ptvoxelsharedblasbuilds, 128, 0)

CVAR(Bool, nri_ptvoxelsharedblasloading, false, 0)

CVAR(Bool, nri_ptvoxelsharedblasroute, false, 0)

CVAR(Bool, nri_ptvoxelshadowproxybuild, false, 0)

CVAR(Bool, nri_ptvoxelshadowproxyroute, false, 0)

CVAR(Int, nri_ptvoxelshadowproxybuilds, 1, 0)

CVAR(Int, nri_ptvoxelshadowproxytransitions, 8, 0)

CVAR(Int, nri_ptvoxelexcludeindex, -1, 0)

CVAR(Int, nri_ptvoxelexcludeindex2, -1, 0)

CVAR(Int, nri_ptvoxelexcludeindex3, -1, 0)

CVAR(Int, nri_ptvoxelexcludeminprims, 0, 0)

CVAR(Bool, nri_ptvoxelomitoccurrences, false, 0)

CVAR(Bool, nri_ptruntimeworklist, true, 0)

CVAR(Int, nri_ptruntimeworklistsweepbudget, 32, 0)

CVAR(Bool, nri_ptruntimedeferfarmaterial, true, 0)

CVAR(Bool, nri_ptruntimedefernearinvisiblematerial, true, 0)

CVAR(Int, nri_ptruntimenearinvisiblematerialbudget, 4, 0)

CVAR(Bool, nri_ptruntimedeferfarstructural, true, 0)

CVAR(Int, nri_ptruntimefarstructuralbudget, 2, 0)

CVAR(Bool, nri_ptruntimedefernearinvisiblestructural, true, 0)

CVAR(Int, nri_ptruntimenearinvisiblestructuralbudget, 2, 0)

CUSTOM_CVAR(Float, nri_ptruntimemutationneardistance, 1024.0f, 0)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
}


// Moved from source/common/rendering/nri/renderer/nri_settings_profiles.cpp

CVAR(String, nri_settingsprofilewarning, "Warning: Ray reconstruction is recommended due to visual bugs with other upscalers/denoisers", CVAR_GLOBALCONFIG)

CUSTOM_CVAR(Int, nri_settingsprofile, NRI_SETTINGS_PROFILE_SAFE, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	const int clampedProfile = ClampNRISettingsProfile(self);
	if (self != clampedProfile)
	{
		self = clampedProfile;
		return;
	}

	const auto& preset = kNRISettingsProfilePresets[self];
	SyncNRISettingsProfileWarning(preset);

	if (gSkipInitialProfileApply)
	{
		gSkipInitialProfileApply = false;
		return;
	}

	ApplyNRISettingsProfile(preset);
}


// Moved from source/common/rendering/nri/renderer/nri_scene_lights.cpp

CVAR(Int, nri_ptactoroverlaylighttrace, 0, 0)

void NRIApplySafeSettingsProfileForRecovery()
{
	nri_settingsprofile = NRI_SETTINGS_PROFILE_SAFE;
	ApplyNRISettingsProfile(kNRISettingsProfilePresets[NRI_SETTINGS_PROFILE_SAFE]);
}
