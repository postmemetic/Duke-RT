Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
function Read-RepoFile([string]$Path) { Get-Content -LiteralPath (Join-Path $root $Path) -Raw }
function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

$runner = Read-RepoFile 'tools\validation\run-smoke-visual-baseline.ps1'
$sheets = Read-RepoFile 'tools\validation\new-smoke-visual-contact-sheets.ps1'
$scenario = Read-RepoFile 'tools\validation\scenarios\smoke-visual-billows-pulse-vorticity.json' | ConvertFrom-Json
$variants = @($scenario.variants)

Require ($runner -match "nri_ptsmokegridcurlevolution\s*=\s*'0'") 'Capture defaults must explicitly disable temporal curl evolution.'
Require ($runner -match "nri_ptsmokegridvorticity\s*=\s*'0'") 'Capture defaults must explicitly disable vorticity confinement.'
foreach ($field in @('pulseAmount', 'pulsePeriodCadences', 'pulsePhase')) {
    Require ($runner -match ($field + '\s*=')) "Capture source defaults are missing $field."
}
Require ($runner -match "pulseAmount\s*=\s*@\{\s*type='number';\s*minimum=0\.0;\s*maximum=1\.0") 'Pulse amount must have a typed [0,1] capture contract.'
Require ($runner -match "pulsePeriodCadences\s*=\s*@\{\s*type='integer';\s*minimum=1;\s*maximum=256") 'Pulse period must have a typed [1,256] capture contract.'
Require ($runner -match "pulsePhase\s*=\s*@\{\s*type='phase'") 'Pulse phase must use the wrapping typed capture contract.'
Require ($runner -match 'return \$phase - \[math\]::Floor\(\$phase\)') 'Capture phase normalization must match LIGHTOVR wrapping.'
foreach ($field in @('pulseamount', 'pulseperiodcadences', 'pulsephase')) {
    Require ($runner -match ($field + '\s+')) "Generated LIGHTOVR is missing $field."
}

Require ([int]$scenario.schema -eq 2) 'Pulse/vorticity capture scenario schema must be 2.'
Require ([string]$scenario.suite -eq 'smoke-billow-pulse-vorticity') 'Pulse/vorticity capture suite name changed.'
Require ($variants.Count -eq 8) 'Pulse/vorticity capture matrix must remain the compact eight-row design.'
Require (@($variants.id | Sort-Object -Unique).Count -eq $variants.Count) 'Pulse/vorticity capture IDs must be unique.'
$expectedIds = @(
    '70-feature-identity',
    '71-pulse-only',
    '72-evolving-curl-only',
    '73-vorticity-only',
    '74-rotation-pair',
    '75-pulse-rotation-combined',
    '76-chunky-billow-candidate',
    '77-rolling-billow-candidate'
)
Require (@(Compare-Object $expectedIds @($variants.id)).Count -eq 0) 'Pulse/vorticity capture matrix has unexpected IDs.'

$byId = @{}
foreach ($variant in $variants) {
    $byId[$variant.id] = $variant
    Require ([math]::Abs([double]$variant.extinction - 0.008) -lt 1e-9) "$($variant.id) changed the fixed extinction baseline."
    Require ([math]::Abs([double]$variant.anisotropy - 0.12) -lt 1e-9) "$($variant.id) changed the fixed phase baseline."
    Require ([bool]$variant.history) "$($variant.id) changed the history-on visual standard."
    Require (-not [bool]$variant.multiple) "$($variant.id) changed multiple scattering."
    Require (-not [bool]$variant.selfShadow) "$($variant.id) changed self shadowing."
    Require ([math]::Abs([double]$variant.expectedNominalMassRate - 96.4285714286) -lt 1e-8) "$($variant.id) changed mean source mass rate."
}

Require ($null -eq $byId['70-feature-identity'].PSObject.Properties['sourceOverrides']) 'Identity row must not alter source authoring.'
Require ($null -eq $byId['70-feature-identity'].PSObject.Properties['styleOverrides']) 'Identity row must not alter style authoring.'
Require ([bool]$byId['70-feature-identity'].settings.nri_ptsmokereadback) 'Identity diagnostic must enable grid readback.'

Require ([double]$byId['71-pulse-only'].sourceOverrides.pulseAmount -eq 0.65) 'Pulse-only row lost its bounded modulation amount.'
Require ([int]$byId['71-pulse-only'].sourceOverrides.pulsePeriodCadences -eq 8) 'Pulse-only row lost its eight-cadence period.'
Require ([int]$byId['71-pulse-only'].settings.nri_ptsmoketrace -eq 2) 'Pulse-only row must expose bounded ordinal/weight telemetry.'
Require ($null -eq $byId['71-pulse-only'].PSObject.Properties['styleOverrides']) 'Pulse-only row must retain the default style.'

Require ([double]$byId['72-evolving-curl-only'].settings.nri_ptsmokegridcurlevolution -eq 0.75) 'Curl isolation row changed.'
Require ($null -eq $byId['72-evolving-curl-only'].PSObject.Properties['sourceOverrides']) 'Curl isolation row must retain the default source.'
Require ([double]$byId['73-vorticity-only'].settings.nri_ptsmokegridvorticity -eq 1.0) 'Vorticity isolation row changed.'
Require ($null -eq $byId['73-vorticity-only'].PSObject.Properties['sourceOverrides']) 'Vorticity isolation row must retain the default source.'
Require ([double]$byId['74-rotation-pair'].settings.nri_ptsmokegridcurlevolution -eq 0.75 -and
    [double]$byId['74-rotation-pair'].settings.nri_ptsmokegridvorticity -eq 1.0) 'Rotation-pair row must combine only the two new solver controls.'

Require ([double]$byId['75-pulse-rotation-combined'].sourceOverrides.pulseAmount -eq 0.65) 'Controlled combination must reuse the isolated pulse.'
Require ([bool]$byId['75-pulse-rotation-combined'].settings.nri_ptsmokereadback) 'Controlled combination must enable grid readback.'
Require ([double]$byId['76-chunky-billow-candidate'].sourceOverrides.radiusScale -eq 1.75) 'Chunky candidate must retain the narrower release bracket.'
Require ([double]$byId['77-rolling-billow-candidate'].settings.nri_ptsmokegridvorticity -eq 1.75) 'Rolling candidate must retain the stronger confinement bracket.'
Require ([bool]$byId['77-rolling-billow-candidate'].settings.nri_ptsmokereadback) 'Rolling candidate must enable grid readback.'

Require ($sheets -match "id\s+-eq\s+'70-feature-identity'") 'Contact-sheet generator must recognize the pulse/vorticity suite.'
foreach ($name in @('contact-sheet-pulse-vorticity-isolation.png', 'contact-sheet-pulse-vorticity-candidates.png')) {
    Require ($sheets.Contains($name)) "Contact-sheet generator is missing $name."
}

Write-Host 'Smoke pulse/vorticity capture validation passed.'
