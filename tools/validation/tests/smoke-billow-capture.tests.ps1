Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
function Read-RepoFile([string]$Path) { Get-Content -LiteralPath (Join-Path $root $Path) -Raw }
function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

$scenario = Read-RepoFile 'tools\validation\scenarios\smoke-visual-billows.json' | ConvertFrom-Json
$sheets = Read-RepoFile 'tools\validation\new-smoke-visual-contact-sheets.ps1'
$variants = @($scenario.variants)

Require ([int]$scenario.schema -eq 2) 'Billow capture scenario schema must be 2.'
Require ([string]$scenario.suite -eq 'smoke-billow') 'Billow capture scenario must identify the smoke-billow suite.'
Require ($variants.Count -eq 12) 'Billow capture scenario must contain 12 first-pass variants.'
Require (@($variants.id | Sort-Object -Unique).Count -eq $variants.Count) 'Billow variant IDs must be unique.'

$expectedIds = @(
    '00-motion-default',
    '01-pulse-042',
    '02-pulse-056',
    '03-pulse-084',
    '04-random-000',
    '05-random-010',
    '06-random-040',
    '07-curl-000',
    '08-curl-021',
    '09-curl-084',
    '10-curl-scale-012',
    '11-curl-scale-048'
)
Require (@(Compare-Object $expectedIds @($variants.id)).Count -eq 0) 'Billow capture scenario has an unexpected variant matrix.'

foreach ($variant in $variants) {
    Require ([math]::Abs([double]$variant.extinction - 0.008) -lt 1e-9) "$($variant.id) changed the fixed extinction baseline."
    Require ([math]::Abs([double]$variant.anisotropy - 0.12) -lt 1e-9) "$($variant.id) changed the fixed phase baseline."
    Require ([bool]$variant.history) "$($variant.id) must use the current history-on beauty default."
    Require (-not [bool]$variant.multiple) "$($variant.id) changed the multiple-scatter default."
    Require (-not [bool]$variant.selfShadow) "$($variant.id) changed the self-shadow default."
    Require ([math]::Abs([double]$variant.expectedNominalMassRate - 96.4286) -lt 1e-4) "$($variant.id) changed the expected nominal mass rate."
}

$byId = @{}
foreach ($variant in $variants) { $byId[$variant.id] = $variant }
$defaultSource = [ordered]@{ intervalSeconds = 0.28; count = 9.0; densityScale = 3.0 }
foreach ($id in $expectedIds) {
    $variant = $byId[$id]
    $interval = if ($null -ne $variant.sourceOverrides.PSObject.Properties['intervalSeconds']) {
        [double]$variant.sourceOverrides.intervalSeconds
    } else { [double]$defaultSource.intervalSeconds }
    $count = if ($null -ne $variant.sourceOverrides.PSObject.Properties['count']) {
        [double]$variant.sourceOverrides.count
    } else { [double]$defaultSource.count }
    $densityScale = if ($null -ne $variant.sourceOverrides.PSObject.Properties['densityScale']) {
        [double]$variant.sourceOverrides.densityScale
    } else { [double]$defaultSource.densityScale }
    Require ([math]::Abs(($count * $densityScale / $interval) - 96.4285714) -lt 1e-4) "$id does not preserve nominal source mass rate."
}

Require ([double]$byId['04-random-000'].styleOverrides.velocityRandom -eq 0) 'Random bracket must include zero source randomness.'
Require ([double]$byId['06-random-040'].styleOverrides.velocityRandom -eq 40) 'Random bracket must include doubled source randomness.'
Require ([double]$byId['07-curl-000'].styleOverrides.turbulence -eq 0) 'Curl bracket must include zero turbulence.'
Require ([double]$byId['09-curl-084'].styleOverrides.turbulence -eq 84) 'Curl bracket must include doubled turbulence.'
Require ([double]$byId['10-curl-scale-012'].styleOverrides.turbulenceScale -eq 12) 'Curl-scale bracket must include scale 12.'
Require ([double]$byId['11-curl-scale-048'].styleOverrides.turbulenceScale -eq 48) 'Curl-scale bracket must include scale 48.'

foreach ($name in @(
    'contact-sheet-pulse-cadence.png',
    'contact-sheet-source-random.png',
    'contact-sheet-curl.png'
)) {
    Require ($sheets.Contains($name)) "Contact-sheet generator is missing $name."
}
Require ($sheets -match "id\s+-eq\s+'00-motion-default'") 'Contact-sheet generator must explicitly recognize the billow suite.'

Write-Host 'Smoke billow capture validation passed.'
