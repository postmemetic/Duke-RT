$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$cvars = Get-Content -LiteralPath (Join-Path $repo 'source\common\rendering\nri\renderer\nri_cvars.cpp') -Raw
$menu = Get-Content -LiteralPath (Join-Path $repo 'wadsrc\static\menudef.txt') -Raw

function Assert-Match
{
	param(
		[Parameter(Mandatory = $true)][string]$Text,
		[Parameter(Mandatory = $true)][string]$Pattern,
		[Parameter(Mandatory = $true)][string]$Message
	)

	if ($Text -notmatch $Pattern)
	{
		throw $Message
	}
}

# Profile values are archived. Keep the original 0-6 ABI in place and append
# the three FSR profiles as 7-9.
Assert-Match $cvars '(?s)enum NRISettingsProfile\s*\{\s*NRI_SETTINGS_PROFILE_SAFE\s*=\s*0,\s*NRI_SETTINGS_PROFILE_DLRR_FAST,\s*NRI_SETTINGS_PROFILE_DLRR_MEDIUM,\s*NRI_SETTINGS_PROFILE_DLRR_BEAUTIFUL,\s*NRI_SETTINGS_PROFILE_DLSS_SR_FAST,\s*NRI_SETTINGS_PROFILE_DLSS_SR_MEDIUM,\s*NRI_SETTINGS_PROFILE_DLSS_SR_BEAUTIFUL,\s*NRI_SETTINGS_PROFILE_FSR_FAST,\s*NRI_SETTINGS_PROFILE_FSR_MEDIUM,\s*NRI_SETTINGS_PROFILE_FSR_BEAUTIFUL,\s*\}' `
	'The settings-profile enum must preserve IDs 0-6 and append FSR profiles as IDs 7-9.'
Assert-Match $cvars 'constexpr int kNRIUpscalerFsr\s*=\s*4;' `
	'The FSR profile selector must use stable main-upscaler value 4.'
Assert-Match $cvars 'return std::clamp\(profile, 0, \(int\)std::size\(kNRISettingsProfilePresets\) - 1\);' `
	'Profile validation must continue to derive its upper bound from the preset table.'

$presetRows = @(
	'\{ "Safe Mode", kNRIUpscalerOff, kNRIUpscalerModeNative, kNRIOutputSdr, true, kMirrorsWithoutRayReconstructionWarning \}',
	'\{ "Fast Preset - DLRR", kNRIUpscalerDlrr, kNRIUpscalerModeBalanced, kNRIOutputHdr, false, nullptr \}',
	'\{ "Medium Preset - DLRR", kNRIUpscalerDlrr, kNRIUpscalerModeQuality, kNRIOutputHdr, false, nullptr \}',
	'\{ "Beautiful Preset - DLRR", kNRIUpscalerDlrr, kNRIUpscalerModeNative, kNRIOutputHdr, false, nullptr \}',
	'\{ "Fast Preset - DLSS-SR", kNRIUpscalerDlssSr, kNRIUpscalerModeBalanced, kNRIOutputHdr, true, kMirrorsWithoutRayReconstructionWarning \}',
	'\{ "Medium Preset - DLSS-SR", kNRIUpscalerDlssSr, kNRIUpscalerModeQuality, kNRIOutputHdr, true, kMirrorsWithoutRayReconstructionWarning \}',
	'\{ "Beautiful Preset - DLSS-SR", kNRIUpscalerDlssSr, kNRIUpscalerModeNative, kNRIOutputHdr, true, kMirrorsWithoutRayReconstructionWarning \}',
	'\{ "Fast Preset - AMD FSR 3", kNRIUpscalerFsr, kNRIUpscalerModeBalanced, kNRIOutputHdr, true, kMirrorsWithoutRayReconstructionWarning \}',
	'\{ "Medium Preset - AMD FSR 3", kNRIUpscalerFsr, kNRIUpscalerModeQuality, kNRIOutputHdr, true, kMirrorsWithoutRayReconstructionWarning \}',
	'\{ "Beautiful Preset - AMD FSR 3", kNRIUpscalerFsr, kNRIUpscalerModeNative, kNRIOutputHdr, true, kMirrorsWithoutRayReconstructionWarning \}'
)

$previousOffset = -1
foreach ($row in $presetRows)
{
	$match = [regex]::Match($cvars, $row)
	if (-not $match.Success -or $match.Index -le $previousOffset)
	{
		throw "Missing or out-of-order settings profile row: $row"
	}
	$previousOffset = $match.Index
}

# Every profile shares the same safety/ownership policy. FSR-specific rows only
# choose provider, quality mode, HDR, NRD, and the existing non-RR warning.
Assert-Match $cvars '(?s)void ApplyNRISettingsProfile\([^)]*\)\s*\{.*?nri_upscaler\s*=\s*preset\.upscaler;.*?nri_postsharpen\s*=\s*0;.*?nri_upscalermode\s*=\s*preset\.upscalerMode;.*?nri_ptoutputmode\s*=\s*preset\.outputMode;.*?nri_pttaa\s*=\s*false;.*?nri_denoise\s*=\s*preset\.denoise;.*?nri_nrddenoiser\s*=\s*kNRDDenoiserRelax;.*?nri_validation\s*=\s*false;.*?nri_apivalidation\s*=\s*false;.*?nri_dred\s*=\s*false;.*?nri_framegen\s*=\s*false;' `
	'Profile application must retain the shared TAA, sharpening, NRD, validation, and frame-generation policy.'

Assert-Match $cvars '(?s)CUSTOM_CVAR\(Int, nri_settingsprofile, NRI_SETTINGS_PROFILE_SAFE, CVAR_ARCHIVE \| CVAR_GLOBALCONFIG\).*?SyncNRISettingsProfileWarning\(preset\);\s*if \(gSkipInitialProfileApply\)\s*\{\s*gSkipInitialProfileApply = false;\s*return;\s*\}\s*ApplyNRISettingsProfile\(preset\);' `
	'The initial profile callback must continue to sync only the warning before live changes apply presets.'

Assert-Match $menu '(?s)OptionValue NRIMainUpscaler\s*\{\s*0, "\$OPTVAL_OFF"\s*2, "DLSS-SR"\s*3, "DLRR"\s*4, "AMD FSR 3 \(Upscaling\)"\s*\}' `
	'The main-upscaler menu must append AMD FSR 3 as value 4.'
Assert-Match $menu '(?s)OptionValue NRISettingsProfile\s*\{\s*0, "Safe Mode"\s*1, "Fast Preset - DLRR"\s*2, "Medium Preset - DLRR"\s*3, "Beautiful Preset - DLRR"\s*4, "Fast Preset - DLSS-SR"\s*5, "Medium Preset - DLSS-SR"\s*6, "Beautiful Preset - DLSS-SR"\s*7, "Fast Preset - AMD FSR 3"\s*8, "Medium Preset - AMD FSR 3"\s*9, "Beautiful Preset - AMD FSR 3"\s*\}' `
	'The profile menu must preserve values 0-6 and append the AMD FSR 3 profiles as values 7-9.'

Write-Host 'NRI settings profiles contract passed.'
