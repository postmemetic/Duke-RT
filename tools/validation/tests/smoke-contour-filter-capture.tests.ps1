$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
$scenario = Get-Content (Join-Path $root 'tools/validation/scenarios/smoke-illustrative-contour-filter.json') -Raw | ConvertFrom-Json
$sheets = Get-Content (Join-Path $root 'tools/validation/new-smoke-visual-contact-sheets.ps1') -Raw
function Require([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }

Require ($scenario.variants.Count -eq 5) 'Contour-fix gallery must contain the broad-band guard and four affected rows.'
Require ($scenario.variants[0].id -eq '200-guard-broad-bands') 'Broad bands must be the first regression guard.'
Require ([double]$scenario.variants[0].settings.nri_ptsmokebandstrength -eq 0.90) 'Broad-band guard changed preset 108.'
Require ($null -eq $scenario.variants[0].settings.nri_ptsmokecontourstrength) 'Broad-band guard must keep contouring disabled.'
foreach ($variant in $scenario.variants) {
    Require (-not $variant.history) "Variant $($variant.id) must keep output history off."
    Require (-not $variant.multiple) "Variant $($variant.id) changed multiple scattering."
    Require (-not $variant.selfShadow) "Variant $($variant.id) changed self-shadow."
    Require ([double]$variant.settings.nri_ptsmokegridcurlevolution -eq 0.75) "Variant $($variant.id) changed selected curl evolution."
    Require ([double]$variant.settings.nri_ptsmokegridvorticity -eq 1.0) "Variant $($variant.id) changed selected vorticity confinement."
}
Require ([int]$scenario.variants[-1].settings.nri_ptsmokeviewroute -eq 2) 'Last row must retain compact-route parity coverage.'
Require ($sheets.Contains("'contact-sheet-contour-filter.png'")) 'Contact-sheet generator is missing the contour-filter sheet.'

Write-Host 'Smoke contour-filter capture validation passed.'
