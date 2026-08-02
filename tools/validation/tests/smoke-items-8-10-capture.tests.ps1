$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
$runner = Get-Content (Join-Path $root 'tools/validation/run-smoke-visual-baseline.ps1') -Raw
$sheets = Get-Content (Join-Path $root 'tools/validation/new-smoke-visual-contact-sheets.ps1') -Raw
$scenario = Get-Content (Join-Path $root 'tools/validation/scenarios/smoke-visual-items-8-10.json') -Raw | ConvertFrom-Json
function Require([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }

Require ($runner -match '\[int\]\$SmokeEvolutionTics\s*=\s*600') 'Carrier gallery must retain the 20-second default.'
foreach ($setting in @(
    'nri_ptsmokethicknessstrength', 'nri_ptsmokethicknesspivot', 'nri_ptsmokethicknesssteps',
    'nri_ptsmokeflowhighlight', 'nri_ptsmokeflowspeedreference',
    'nri_ptsmokecurlhighlight', 'nri_ptsmokecurlreference',
    'nri_ptsmokecompressionsculpt', 'nri_ptsmokedivergencereference',
    'nri_ptsmokebandstrength', 'nri_ptsmokebandcount',
    'nri_ptsmokebandsoftness', 'nri_ptsmokecontourstrength')) {
    Require ($runner -match ($setting + '\s*=')) "Capture harness does not pin $setting."
}
Require ($runner -match 'pulseAmount\s*=\s*0\.90[\s\S]*pulsePeriodCadences\s*=\s*12[\s\S]*pulsePhase\s*=\s*0\.7916667') 'Generated fire override does not use selected variant 97.'
Require ($scenario.variants.Count -eq 13) 'Carrier gallery must contain 13 focused variants.'
Require ($scenario.variants[0].id -eq '100-carrier-identity') 'Carrier identity must be the first comparison row.'

$newVisualSettings = @(
    'nri_ptsmokethicknessstrength', 'nri_ptsmokethicknesspivot', 'nri_ptsmokethicknesssteps',
    'nri_ptsmokeflowhighlight', 'nri_ptsmokeflowspeedreference',
    'nri_ptsmokecurlhighlight', 'nri_ptsmokecurlreference',
    'nri_ptsmokecompressionsculpt', 'nri_ptsmokedivergencereference',
    'nri_ptsmokebandstrength', 'nri_ptsmokebandcount',
    'nri_ptsmokebandsoftness', 'nri_ptsmokecontourstrength')
foreach ($variant in $scenario.variants) {
    Require (-not $variant.history) "Variant $($variant.id) must expose intrinsic history-off behavior."
    Require (-not $variant.multiple) "Variant $($variant.id) changed multiple scattering."
    Require (-not $variant.selfShadow) "Variant $($variant.id) changed self-shadow."
    Require ([double]$variant.settings.nri_ptsmokegridcurlevolution -eq 0.75) "Variant $($variant.id) does not pin selected curl evolution."
    Require ([double]$variant.settings.nri_ptsmokegridvorticity -eq 1.0) "Variant $($variant.id) does not pin selected vorticity confinement."
}
foreach ($setting in $newVisualSettings) {
    Require ($null -eq $scenario.variants[0].settings.$setting) "Identity unexpectedly enables $setting."
}
Require ([int]$scenario.variants[-1].settings.nri_ptsmokeviewroute -eq 2) 'Final row must validate the compact route.'
foreach ($name in @('contact-sheet-thickness.png', 'contact-sheet-flow.png', 'contact-sheet-illustrative.png', 'contact-sheet-combined.png')) {
    Require ($sheets.Contains($name)) "Contact-sheet routing is missing $name."
}

Write-Host 'Smoke items 8-10 capture validation passed.'
