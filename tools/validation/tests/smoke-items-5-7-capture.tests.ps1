Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
function Read-RepoFile([string]$Path) { Get-Content -LiteralPath (Join-Path $root $Path) -Raw }
function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

$runner = Read-RepoFile 'tools\validation\run-smoke-visual-baseline.ps1'
$sheets = Read-RepoFile 'tools\validation\new-smoke-visual-contact-sheets.ps1'
$scenario = Read-RepoFile 'tools\validation\scenarios\smoke-visual-items-5-7.json' | ConvertFrom-Json

$expectedDefaults = [ordered]@{
    nri_ptsmokeduallobeweight = '0'
    nri_ptsmokeduallobeg = '0.75'
    nri_ptsmokerimstrength = '0'
    nri_ptsmokerimgain = '0'
    nri_ptsmokeradianceedgechroma = '0'
    nri_ptsmokeradiancecavitycontrast = '0'
    nri_ptsmokeradiancedesaturation = '0'
    nri_ptsmokeradianceconfidence = '0.25'
    nri_ptsmokeradiancedirectionality = '0.15'
}
foreach ($entry in $expectedDefaults.GetEnumerator()) {
    $pattern = '(?m)^\s*{0}\s*=\s*''{1}''\s*$' -f [regex]::Escape($entry.Key), [regex]::Escape($entry.Value)
    Require ($runner -match $pattern) "Capture defaults do not pin $($entry.Key)=$($entry.Value)."
}
Require ($runner -match '\[int\]\$SmokeEvolutionTics\s*=\s*600') 'Capture runner must default to 600 tics (20 seconds).'
Require ($runner -match 'wait\s+\$SmokeEvolutionTics') 'Capture command must use the configured smoke evolution time.'
Require ($runner.Contains("value.StartsWith('-', [StringComparison]::Ordinal)")) 'Capture runner must detect negative CVar values.'
Require ($runner.Contains('$DeferredCommands.Add')) 'Capture runner must defer negative CVar values to the in-game console.'
Require ($runner -match '\$\{deferredSettings\}nri_ptautoexposurefreeze[\s\S]*nri_ptreset') 'Deferred settings must apply before smoke reset and evolution.'

$variants = @($scenario.variants)
Require ([int]$scenario.schema -eq 1) 'Items 5-7 capture scenario schema must be 1.'
Require ($variants.Count -eq 17) 'Items 5-7 capture scenario must contain 17 variants.'
Require (@($variants.id | Sort-Object -Unique).Count -eq $variants.Count) 'Items 5-7 variant IDs must be unique.'
$expectedIds = @(
    '41-radiance-phase-identity',
    '42-radiance-rim-subtle',
    '43-radiance-rim-strong',
    '44-radiance-rim-gain',
    '45-radiance-edge-chroma',
    '46-radiance-cavity',
    '47-radiance-desaturation',
    '48-phase-broad-forward',
    '49-phase-balanced-forward',
    '50-phase-tight-forward',
    '51-phase-powder-backscatter',
    '52-phase-strong-backscatter',
    '53-combined-radiance-phase',
    '54-combined-radiance-phase-compact',
    '55-radiance-rim-ungated-stress',
    '56-radiance-cavity-max',
    '57-radiance-chroma-desat-stress'
)
Require (@(Compare-Object $expectedIds @($variants.id)).Count -eq 0) 'Items 5-7 capture scenario has an unexpected variant matrix.'

foreach ($variant in $variants) {
    Require ([math]::Abs([double]$variant.extinction - 0.008) -lt 1e-9) "$($variant.id) changed the fixed extinction baseline."
    Require ([math]::Abs([double]$variant.anisotropy - 0.12) -lt 1e-9) "$($variant.id) changed the fixed phase baseline."
    Require (-not [bool]$variant.history) "$($variant.id) must disable temporal history."
    Require (-not [bool]$variant.multiple) "$($variant.id) must disable multiple scattering."
    Require (-not [bool]$variant.selfShadow) "$($variant.id) must disable self shadowing."
}

$byId = @{}
foreach ($variant in $variants) { $byId[$variant.id] = $variant }
Require (@($byId['41-radiance-phase-identity'].settings.psobject.Properties).Count -eq 0) 'Identity variant must not override any item 5-7 controls.'
Require ([double]$byId['44-radiance-rim-gain'].settings.nri_ptsmokerimgain -gt 0) 'Radiance matrix must include a rim-gain comparison.'
Require ([double]$byId['45-radiance-edge-chroma'].settings.nri_ptsmokeradianceedgechroma -gt 0) 'Radiance matrix must include edge chroma.'
Require ([double]$byId['46-radiance-cavity'].settings.nri_ptsmokeradiancecavitycontrast -gt 0) 'Radiance matrix must include cavity contrast.'
Require ([double]$byId['47-radiance-desaturation'].settings.nri_ptsmokeradiancedesaturation -gt 0) 'Radiance matrix must include desaturation.'
Require ([double]$byId['48-phase-broad-forward'].settings.nri_ptsmokeduallobeg -gt 0) 'Phase matrix must include forward scattering.'
Require ([double]$byId['51-phase-powder-backscatter'].settings.nri_ptsmokeduallobeg -lt 0) 'Phase matrix must include backscatter.'
Require ([int]$byId['54-combined-radiance-phase-compact'].settings.nri_ptsmokeviewroute -eq 2) 'Compact-route comparison must select view route 2.'
Require ([double]$byId['55-radiance-rim-ungated-stress'].settings.nri_ptsmokerimgain -eq 1) 'Stress matrix must expose maximum rim gain.'
Require ([double]$byId['56-radiance-cavity-max'].settings.nri_ptsmokeradiancecavitycontrast -eq 1) 'Stress matrix must expose maximum cavity contrast.'
Require ([double]$byId['57-radiance-chroma-desat-stress'].settings.nri_ptsmokeradianceedgechroma -eq 1) 'Stress matrix must expose maximum edge chroma.'

foreach ($name in @('contact-sheet-radiance.png', 'contact-sheet-phase.png', 'contact-sheet-combined.png')) {
    Require ($sheets.Contains($name)) "Contact-sheet generator is missing $name."
}
foreach ($legacyName in @('contact-sheet-optics.png', 'contact-sheet-field-diagnostics.png', 'contact-sheet-thermal.png', 'contact-sheet-gradient.png')) {
    Require ($sheets.Contains($legacyName)) "Contact-sheet generator no longer exposes legacy output $legacyName."
}
Require ($sheets -match "id\s+-eq\s+'41-radiance-phase-identity'") 'Contact-sheet generator must explicitly recognize the items 5-7 suite.'

Write-Host 'Smoke items 5-7 capture validation passed.'
