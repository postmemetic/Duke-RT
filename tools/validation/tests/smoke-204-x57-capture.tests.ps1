$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
$scenario = Get-Content (Join-Path $root 'tools/validation/scenarios/smoke-visual-204-x-57.json') -Raw | ConvertFrom-Json
$sheets = Get-Content (Join-Path $root 'tools/validation/new-smoke-visual-contact-sheets.ps1') -Raw
$cvars = Get-Content (Join-Path $root 'source/common/rendering/nri/renderer/nri_cvars.cpp') -Raw
function Require([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }

Require ($scenario.variants.Count -eq 12) '204 x 57 gallery must contain 12 focused variants.'
Require ($scenario.variants[0].id -eq '204-x57-base') 'Exact 204 control must be the first row.'
$base = $scenario.variants[0].settings
$carrierFields = @(
    'nri_ptsmokegridcurlevolution', 'nri_ptsmokegridvorticity', 'nri_ptsmokeviewroute',
    'nri_ptsmokethicknessstrength', 'nri_ptsmokethicknesspivot', 'nri_ptsmokethicknesssteps',
    'nri_ptsmokeflowhighlight', 'nri_ptsmokeflowspeedreference',
    'nri_ptsmokecurlhighlight', 'nri_ptsmokecurlreference',
    'nri_ptsmokecompressionsculpt', 'nri_ptsmokedivergencereference',
    'nri_ptsmokebandstrength', 'nri_ptsmokebandcount',
    'nri_ptsmokebandsoftness', 'nri_ptsmokecontourstrength')
$radianceFields = @('nri_ptsmokeradianceedgechroma', 'nri_ptsmokeradiancedesaturation',
    'nri_ptsmokeradianceconfidence', 'nri_ptsmokeradiancedirectionality')
foreach ($variant in $scenario.variants) {
    Require (-not $variant.history) "Variant $($variant.id) must keep history off."
    Require (-not $variant.multiple) "Variant $($variant.id) changed multiple scattering."
    Require (-not $variant.selfShadow) "Variant $($variant.id) changed self-shadow."
    foreach ($field in $carrierFields) {
        Require ([double]$variant.settings.$field -eq [double]$base.$field) "Variant $($variant.id) polluted 204 field $field."
    }
    foreach ($field in $radianceFields) {
        Require ($null -ne $variant.settings.$field) "Variant $($variant.id) does not explicitly pin $field."
    }
}
Require ([double]$base.nri_ptsmokeradianceedgechroma -eq 0 -and [double]$base.nri_ptsmokeradiancedesaturation -eq 0) '204 control unexpectedly enables radiance shaping.'
$stress = $scenario.variants[-1].settings
Require ([double]$stress.nri_ptsmokeradianceedgechroma -eq 1 -and [double]$stress.nri_ptsmokeradiancedesaturation -eq 1 -and [double]$stress.nri_ptsmokeradianceconfidence -eq 0 -and [double]$stress.nri_ptsmokeradiancedirectionality -eq 0) 'Final row is not exact preset-57 radiance stress.'

# The selected gallery result is also the production default for a fresh profile.
$selected = $scenario.variants | Where-Object id -eq '213-x57-moderately-open'
Require ($null -ne $selected) 'Selected preset 213 is missing from the gallery.'
Require (-not $selected.history -and -not $selected.multiple -and -not $selected.selfShadow) 'Preset 213 top-level switches changed.'
foreach ($default in @(
    'CVAR(Bool, nri_ptsmokevolumehistory, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)',
    'CVAR(Bool, nri_ptsmokemultiplescatter, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)',
    'CVAR(Bool, nri_ptsmokeselfshadow, false, 0)',
    'CVAR(Float, nri_ptsmokegridcurlevolution, 0.75f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)',
    'CVAR(Float, nri_ptsmokegridvorticity, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)',
    'CVAR(Int, nri_ptsmokeviewroute, 2, 0)',
    'CVAR(Float, nri_ptsmokethicknessstrength, 0.25f, 0)',
    'CVAR(Float, nri_ptsmokethicknesspivot, 0.50f, 0)',
    'CVAR(Int, nri_ptsmokethicknesssteps, 3, 0)',
    'CVAR(Float, nri_ptsmokeflowhighlight, 0.20f, 0)',
    'CVAR(Float, nri_ptsmokeflowspeedreference, 24.0f, 0)',
    'CVAR(Float, nri_ptsmokecurlhighlight, 0.40f, 0)',
    'CVAR(Float, nri_ptsmokecurlreference, 0.60f, 0)',
    'CVAR(Float, nri_ptsmokecompressionsculpt, 0.20f, 0)',
    'CVAR(Float, nri_ptsmokedivergencereference, 0.60f, 0)',
    'CVAR(Float, nri_ptsmokebandstrength, 0.45f, 0)',
    'CVAR(Int, nri_ptsmokebandcount, 3, 0)',
    'CVAR(Float, nri_ptsmokebandsoftness, 0.30f, 0)',
    'CVAR(Float, nri_ptsmokecontourstrength, 0.25f, 0)',
    'CVAR(Float, nri_ptsmokeradianceedgechroma, 0.70f, 0)',
    'CVAR(Float, nri_ptsmokeradiancedesaturation, 0.65f, 0)',
    'CVAR(Float, nri_ptsmokeradianceconfidence, 0.10f, 0)',
    'CVAR(Float, nri_ptsmokeradiancedirectionality, 0.05f, 0)')) {
    Require ($cvars.Contains($default)) "Production preset 213 default is missing: $default"
}
foreach ($name in @('contact-sheet-204-x57-axes.png', 'contact-sheet-204-x57-gates.png', 'contact-sheet-204-x57-candidates.png')) {
    Require ($sheets.Contains($name)) "Contact-sheet generator is missing $name."
}

Write-Host 'Smoke 204 x 57 capture validation passed.'
