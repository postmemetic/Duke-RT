$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
$cvars = Get-Content (Join-Path $root 'source/common/rendering/nri/renderer/nri_cvars.cpp') -Raw
$settings = Get-Content (Join-Path $root 'source/common/rendering/nri/renderer/nri_renderer_settings.cpp') -Raw
$settingsHeader = Get-Content (Join-Path $root 'source/common/rendering/nri/renderer/nri_renderer_settings.h') -Raw
$menudef = Get-Content (Join-Path $root 'wadsrc/static/menudef.txt') -Raw

foreach ($default in @(
    'CUSTOM_CVAR(Bool, nri_ptsmoke, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)',
    'CVAR(Int, nri_ptsmokequality, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)',
    'CVAR(Int, nri_ptsmokeparticles, 8192, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)',
    'CVAR(Int, nri_ptsmokefroxelpixels, 16, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)',
    'CVAR(Int, nri_ptsmokefroxelz, 48, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)',
    'CVAR(Int, nri_ptsmokecolumncapacity, 64, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)',
    'CVAR(Int, nri_ptsmokesimrate, 60, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)',
    'CVAR(Int, nri_ptsmokemaxsubsteps, 4, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)',
    'CVAR(Float, nri_ptsmokewindx, 5.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)',
    'CVAR(Float, nri_ptsmokewindy, 20.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)',
    'CVAR(Float, nri_ptsmokewindz, 5.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)',
    'CVAR(Float, nri_ptsmokedensityscale, 5.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)',
    'CVAR(Bool, nri_ptsmokepointlights, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)',
    'CVAR(Int, nri_ptsmokeemissivereuse, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)',
    'CVAR(Int, nri_ptsmokeemissivepoints, 4, 0)',
    'CVAR(Int, nri_ptsmokeemissivebackend, 2, 0)',
    'CVAR(Bool, nri_ptsmokeemissiveworldfilter, false, 0)',
	'CVAR(Bool, nri_ptsmokeemissivelocal, false, 0)',
	'CVAR(Int, nri_ptsmokespawnhalorequests, 512, 0)',
	'CVAR(Int, nri_ptsmokespawnhaloallocations, 16, 0)',
    'CVAR(Int, nri_ptsmokedirectreuse, 2, 0)',
    'CVAR(Int, nri_ptsmokedirectreference, 0, 0)',
    'CVAR(Int, nri_ptsmokelightmode, 3, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)',
    'CVAR(Int, nri_ptsmokelightsamples, 4, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)',
    'CVAR(Int, nri_ptsmokemaxlightcandidates, 8, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)',
    'CVAR(Bool, nri_ptsmokefilteredvisibility, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)',
    'CVAR(Int, nri_ptsmokerepresentation, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)',
    'CVAR(Int, nri_ptsmokegridbricks, 512, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)',
    'CVAR(Float, nri_ptsmokegridcellsize, 8.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)',
    'CVAR(Float, nri_ptsmokegridcurlevolution, 0.75f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)',
    'CVAR(Float, nri_ptsmokegridvorticity, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)'
)) {
    if (-not $cvars.Contains($default)) { throw "missing smoke default: $default" }
}

foreach ($default in @(
    'bool indirect = false;',
    'float wind[3] = { 5.0f, 20.0f, 5.0f };',
    'float densityScale = 5.0f;',
    'float gridCurlEvolution = 0.0f;',
    'float gridVorticity = 0.0f;'
)) {
    if (-not $settingsHeader.Contains($default)) { throw "missing smoke settings fallback default: $default" }
}

foreach ($clamp in @(
    'std::clamp((int)nri_ptsmokeparticles, 256, 65536)',
    'std::clamp((int)nri_ptsmokefroxelpixels, 4, 64)',
    'std::clamp((int)nri_ptsmokefroxelz, 8, 128)',
    'std::clamp((int)nri_ptsmokecolumncapacity, 8, 256)',
    'std::clamp((int)nri_ptsmokesimrate, 15, 240)',
    'std::clamp((int)nri_ptsmokemaxsubsteps, 1, 8)',
    'std::clamp((int)nri_ptsmokeemissivepoints, 1, 8)',
    'std::clamp((int)nri_ptsmokelightmode, 0, 3)',
    'std::clamp((int)nri_ptsmokelightsamples, 1, 4)',
	'std::clamp((int)nri_ptsmokemaxlightcandidates, 1, 32)',
	'std::clamp((int)nri_ptsmokespawnhalorequests, 26, 65536)',
	'std::clamp((int)nri_ptsmokespawnhaloallocations, 1, 512)',
    'std::clamp((float)nri_ptsmokefroxelmaxdistance, 64.0f, 32768.0f)',
    'std::clamp((float)nri_ptsmoketimescale, 0.0f, 4.0f)',
    'std::clamp((int)nri_ptsmokerepresentation, 0, 2)',
    'std::clamp((int)nri_ptsmokegridbricks, 64, 4096)',
    'std::clamp((float)nri_ptsmokegridcellsize, 1.0f, 64.0f)',
    'std::clamp((float)nri_ptsmokegridcurlevolution, 0.0f, 16.0f)',
    'std::clamp((float)nri_ptsmokegridvorticity, 0.0f, 16.0f)'
)) {
    if (-not $settings.Contains($clamp)) { throw "missing smoke clamp: $clamp" }
}

$videoOptionsStart = $menudef.IndexOf('OptionMenu "VideoOptions" protected', [System.StringComparison]::Ordinal)
$nriOptionsStart = $menudef.IndexOf('OptionMenu "NRIOptions" protected', [System.StringComparison]::Ordinal)
if ($videoOptionsStart -lt 0 -or $nriOptionsStart -le $videoOptionsStart) { throw 'could not isolate the main Display Options menu' }
$videoOptions = $menudef.Substring($videoOptionsStart, $nriOptionsStart - $videoOptionsStart)
if ($videoOptions -notmatch 'Option\s+"Bloom",\s*"nri_ptbloom",\s*"OnOff"\s*\r?\n\s*Option\s+"Smoke",\s*"nri_ptsmoke",\s*"OnOff"') {
    throw 'the main Display Options menu must expose Smoke directly beneath Bloom'
}
if ($videoOptions -notmatch 'Option\s+"Smoke",\s*"nri_ptsmoke",\s*"OnOff"\s*\r?\n\s*CVarStaticText\s+"nri_settingsprofilewarning"\s*\r?\n\s*CVarStaticText\s+"nri_ptsmokemenuwarning"') {
    throw 'the smoke warning must occupy its own line beneath the ray-reconstruction profile warning'
}
if ($cvars -notmatch 'CVAR\(String, nri_ptsmokemenuwarning, "", 0\)[\s\S]*CUSTOM_CVAR\(Bool, nri_ptsmoke, false, CVAR_ARCHIVE \| CVAR_GLOBALCONFIG\)[\s\S]*nri_ptsmokemenuwarning\s*=\s*self[\s\S]*Smoke is experimental, and is still very taxing on your GPU[\s\S]*:\s*""') {
    throw 'the smoke warning must be populated only while smoke is enabled'
}
if ([regex]::Matches($menudef, '"nri_ptsmoke"').Count -ne 1) { throw 'Smoke must have exactly one MENUDEF binding' }

Write-Host 'smoke settings tests passed.'
