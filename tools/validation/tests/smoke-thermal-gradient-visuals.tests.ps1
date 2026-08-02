$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
function Read-Source([string]$Path) { Get-Content (Join-Path $root $Path) -Raw }
function Assert-Match([string]$Text, [string]$Pattern, [string]$Message) {
    if ($Text -notmatch $Pattern) { throw $Message }
}
function Assert-Near([double]$Actual, [double]$Expected, [double]$Tolerance, [string]$Message) {
    if ([math]::Abs($Actual - $Expected) -gt $Tolerance) {
        throw "$Message (actual=$Actual expected=$Expected tolerance=$Tolerance)"
    }
}

$cvars = Read-Source 'source\common\rendering\nri\renderer\nri_cvars.cpp'
$settings = Read-Source 'source\common\rendering\nri\renderer\nri_renderer_settings.cpp'
$visualHeader = Read-Source 'source\common\rendering\nri\renderer\nri_smoke_visuals.h'
$visualOwner = Read-Source 'source\common\rendering\nri\renderer\nri_smoke_visuals.cpp'
$contracts = Read-Source 'source\common\rendering\nri\renderer\nri_smoke_contracts.h'
$constants = Read-Source 'source\common\rendering\nri\shaders\Include\SmokeConstants.hlsli'
$visuals = Read-Source 'source\common\rendering\nri\shaders\Include\SmokeVisuals.hlsli'
$evaluate = Read-Source 'source\common\rendering\nri\shaders\SmokeEvaluateGrid.cs.hlsl'
$compact = Read-Source 'source\common\rendering\nri\shaders\SmokeEvaluateGridCompact.cs.hlsl'
$smoke = Read-Source 'source\common\rendering\nri\renderer\nri_smoke.cpp'
$capture = Read-Source 'tools\validation\run-smoke-visual-baseline.ps1'

# Every experimental control is session-only and strength/color defaults give
# an exact identity independently of the response knots.
foreach ($default in @(
    'CVAR(Float, nri_ptsmokethermallow, 1.0f, 0)',
    'CVAR(Float, nri_ptsmokethermalhigh, 4.0f, 0)',
    'CVAR(Float, nri_ptsmokethermaltintr, 1.0f, 0)',
    'CVAR(Float, nri_ptsmokethermaltintg, 1.0f, 0)',
    'CVAR(Float, nri_ptsmokethermaltintb, 1.0f, 0)',
    'CVAR(Float, nri_ptsmokethermalglowr, 0.0f, 0)',
    'CVAR(Float, nri_ptsmokethermalglowg, 0.0f, 0)',
    'CVAR(Float, nri_ptsmokethermalglowb, 0.0f, 0)',
    'CVAR(Float, nri_ptsmokegradientpivot, 0.5f, 0)',
    'CVAR(Float, nri_ptsmokegradientwidth, 0.25f, 0)',
    'CVAR(Float, nri_ptsmokeedgesculpt, 0.0f, 0)',
    'CVAR(Float, nri_ptsmokeedgepowder, 0.0f, 0)',
    'CVAR(Float, nri_ptsmokeedgetintr, 1.0f, 0)',
    'CVAR(Float, nri_ptsmokeedgetintg, 1.0f, 0)',
    'CVAR(Float, nri_ptsmokeedgetintb, 1.0f, 0)',
    'CVAR(Float, nri_ptsmokeedgetintstrength, 0.0f, 0)'
)) {
    if (-not $cvars.Contains($default)) { throw "Missing item 3-4 default: $default" }
}
Assert-Match $settings 'thermalLow[\s\S]*thermalHigh[\s\S]*thermalTint\[0\][\s\S]*thermalGlow\[0\][\s\S]*gradientPivot[\s\S]*gradientWidth[\s\S]*edgeSculpt[\s\S]*edgePowder[\s\S]*edgeTint\[0\][\s\S]*edgeTintStrength' 'Every item 3-4 CVar must enter the clamped settings snapshot.'
Assert-Match $visualHeader 'thermalLow[\s\S]*thermalHigh[\s\S]*thermalTint\[3\][\s\S]*thermalGlow\[3\][\s\S]*gradientPivot[\s\S]*gradientWidth[\s\S]*edgeSculpt[\s\S]*edgePowder[\s\S]*edgeTint\[3\][\s\S]*edgeTintStrength' 'The focused visual snapshot is incomplete.'

# The proven root block stays fixed. A pass-local copy overlays five fields
# only for the two shared grid materializers; canonical constants are untouched.
Assert-Match $contracts 'sizeof\(NRISmokeConstants\)\s*==\s*216' 'Items 3-4 must preserve the 216-byte root layout.'
Assert-Match $constants 'uint\s+DirectionalColorPacked[\s\S]*uint\s+OutputWidth[\s\S]*uint\s+OutputHeight[\s\S]*float2\s+CurrentJitter' 'The reflected alias lanes changed order.'
Assert-Match $visualOwner 'PackMaterializationWords[\s\S]*thermalLow[\s\S]*thermalHigh[\s\S]*thermalTint[\s\S]*thermalGlow[\s\S]*gradientPivot[\s\S]*gradientWidth[\s\S]*edgeSculpt[\s\S]*edgePowder[\s\S]*edgeTintStrength[\s\S]*edgeTint' 'CPU packing must include every item 3-4 value.'
Assert-Match $visualOwner 'currentJitter\[0\][\s\S]*currentJitter\[1\][\s\S]*outputWidth\s*=\s*words\[2\][\s\S]*outputHeight\s*=\s*words\[3\][\s\S]*directionalColorPacked\s*=\s*words\[4\]' 'CPU materialization aliases have the wrong word order.'
Assert-Match $visuals 'ThermalPacked0\(\).*CurrentJitter\.x[\s\S]*ThermalPacked1\(\).*CurrentJitter\.y[\s\S]*GradientPacked0\(\).*OutputWidth[\s\S]*GradientPacked1\(\).*OutputHeight[\s\S]*GradientPacked2\(\).*DirectionalColorPacked' 'HLSL aliases must match CPU word order.'
Assert-Match $smoke 'NRISmokeConstants\s+passConstants\s*=\s*constants[\s\S]*EvaluateGrid[\s\S]*EvaluateGridCompact[\s\S]*NRIPopulateSmokeVisualMaterializationConstants[\s\S]*&passConstants' 'Grid dispatches must bind a temporary overlaid constants copy.'
Assert-Match $compact '#include\s+"SmokeEvaluateGrid.cs.hlsl"[\s\S]*SmokeEvaluateGridFroxel' 'Compact evaluation must share item 3-4 implementation.'
Assert-Match $evaluate 'SmokeFroxelRay\(froxel\.xy\)' 'Grid materialization must retain its unjittered froxel ray.'
Assert-Match $evaluate 'sampleUv\s*=\s*\(float2\(froxel\.xy\)\s*\+\s*footprintUnit\)\s*/' 'Grid materialization must derive UVs from froxel dimensions.'
Assert-Match $evaluate 'SmokeWorldPosition\(sampleUv,\s*sampleViewDepth\)' 'Grid materialization must retain explicit sample reconstruction.'
Assert-Match $evaluate 'SmokeRenderGridIntegrateFroxel[\s\S]*SmokeEvaluateGridFroxel' 'The shared materializer implementation is missing.'
if ($evaluate -match 'SmokePrimarySampleUv|SmokeDirectionalColor|gSmokeConstants\.(?:CurrentJitter|OutputWidth|OutputHeight|DirectionalColorPacked)') {
    throw 'EvaluateGrid acquired a canonical use of a pass-local visual alias.'
}
if ($compact -match 'SmokePrimarySampleUv|SmokeDirectionalColor|gSmokeConstants\.(?:CurrentJitter|OutputWidth|OutputHeight|DirectionalColorPacked)') {
    throw 'EvaluateGridCompact acquired a canonical use of a pass-local visual alias.'
}

# Gradient evaluation reuses the eight loaded corners, is coefficient weighted,
# precedes item-2 transfer, and never changes raw field diagnostics.
Assert-Match $evaluate 'SmokeRenderGridSample[\s\S]*SmokeVisualGradientSample\(scalarCorners,\s*gridBlend,\s*cellSize[\s\S]*integratedEdge\s*\+=\s*sampleEdge\s*\*\s*sampleExtinction[\s\S]*integratedEdgeWeight\s*\+=\s*sampleExtinction' 'Beauty gradient must reuse and coefficient-weight the current quadrature corners.'
Assert-Match $evaluate 'SmokeVisualSculptExtinction\(baseExtinction,\s*edgeMask\)[\s\S]*SmokeVisualShapeMedium\(baseExtinction,\s*sculptedBaseExtinction' 'Gradient sculpting must precede the item-2 transfer while retaining base albedo authority.'
Assert-Match $evaluate 'if\s*\(fieldDebugMode\s*==\s*0u\s*&&\s*SmokeVisualGradientEnabled' 'Raw field diagnostics must bypass beauty gradient evaluation.'
Assert-Match $visuals 'factor\s*=\s*exp2\(clamp\(stops,\s*-2\.0,\s*2\.0\)\)' 'Gradient sculpt factor must remain in the bounded four-stop range.'

# Thermal applies after shaped/gradient medium reconstruction. Glow is an
# extinction-correlated bounded source, and the mixed phase weight follows the
# final scattering rather than the pre-style optical moment.
Assert-Match $evaluate 'SmokeVisualShapeMedium[\s\S]*SmokeVisualApplyGradientTint[\s\S]*scalar\.y\s*/\s*scalar\.x[\s\S]*SmokeVisualApplyThermal[\s\S]*gSmokeFroxelMedium' 'Thermal styling must use the normalized integrated moment after optical shaping.'
Assert-Match $visuals 'glowSource\s*=\s*extinction\s*\*\s*heat\s*\*\s*min\(max\(glow,\s*0\.0\),\s*4\.0\)' 'Thermal glow must be bounded and require final optical coverage.'
Assert-Match $evaluate 'gSmokeFroxelPhase\[froxelIndex\][\s\S]*dot\(scattering,\s*float3\(0\.2126,\s*0\.7152,\s*0\.0722\)\)' 'Phase mixture weight must use final scattering luminance.'
Assert-Match $evaluate 'SmokeVisualApplyGradientTint[\s\S]*nonThermalSourcePresent\s*=\s*any\(source\s*>\s*0\.0\)[\s\S]*SmokeVisualApplyThermal' 'Provenance must inspect non-thermal source after all scattering shaping.'
Assert-Match $evaluate 'nonThermalSourcePresent\s*\?\s*NRI_SMOKE_FALLBACK_WORLD[\s\S]*thermalEmission\s*\?\s*NRI_SMOKE_FALLBACK_EMISSIVE' 'Glow-only source must not claim pooled-world provenance.'
Assert-Match $visualOwner 'for\s*\(uint32_t\s+word\s*:\s*PackMaterializationWords\(settings\)\)\s*HashWord' 'Effective overlay words must invalidate visual histories.'

foreach ($name in @(
    'nri_ptsmokethermallow', 'nri_ptsmokethermalhigh',
    'nri_ptsmokethermaltintr', 'nri_ptsmokethermaltintg', 'nri_ptsmokethermaltintb',
    'nri_ptsmokethermalglowr', 'nri_ptsmokethermalglowg', 'nri_ptsmokethermalglowb',
    'nri_ptsmokegradientpivot', 'nri_ptsmokegradientwidth',
    'nri_ptsmokeedgesculpt', 'nri_ptsmokeedgepowder',
    'nri_ptsmokeedgetintr', 'nri_ptsmokeedgetintg', 'nri_ptsmokeedgetintb',
    'nri_ptsmokeedgetintstrength'
)) {
    Assert-Match $capture ("{0}\s*=" -f $name) "Capture defaults do not pin $name."
}

# CPU mirrors: normalized thermal is mass weighted, response is monotone, glow
# cannot create coverage, and signed gradient factors stay finite and bounded.
function Get-Heat([double]$Mass, [double]$Moment, [double]$Low, [double]$High) {
    if ($Mass -le 1e-6) { return 0.0 }
    $thermal = [math]::Max($Moment / $Mass, 0.0)
    $lowValue = [math]::Max($Low, 0.0)
    $highValue = [math]::Max($High, $lowValue + 1e-4)
    $t = [math]::Max(0.0, [math]::Min(1.0, ($thermal - $lowValue) / ($highValue - $lowValue)))
    return $t * $t * (3.0 - 2.0 * $t)
}
Assert-Near (Get-Heat 0 100 1 4) 0 0 'Zero mass produced heat.'
Assert-Near (Get-Heat 1 0 1 4) 0 0 'Zero thermal moment produced heat.'
$previousHeat = -1.0
foreach ($moment in 0..640) {
    $heat = Get-Heat 10 $moment 1 4
    if ($heat + 1e-12 -lt $previousHeat) { throw 'Thermal response is not monotone.' }
    $previousHeat = $heat
}
foreach ($extinction in @(0.0, 1e-12, 0.01, 1.0, 16.0)) {
    foreach ($heat in @(0.0, 0.25, 1.0)) {
        foreach ($glow in @(0.0, 0.25, 4.0)) {
            $source = $extinction * $heat * $glow
            if ($extinction -eq 0 -and $source -ne 0) { throw 'Glow created optical coverage.' }
            if ($source -gt $extinction * 4.0 + 1e-12) { throw 'Glow exceeded its radiance bound.' }
        }
    }
}
foreach ($edge in @(0.0, 0.25, 1.0)) {
    foreach ($sculpt in @(-1.0, 0.0, 1.0)) {
        foreach ($powder in @(-1.0, 0.0, 1.0)) {
            $middle = 1.0
            $stops = [math]::Max(-2.0, [math]::Min(2.0, $edge * ($sculpt + $powder * $middle)))
            $factor = [math]::Pow(2.0, $stops)
            if ([double]::IsNaN($factor) -or [double]::IsInfinity($factor) -or $factor -lt 0.25 -or $factor -gt 4.0) {
                throw 'Gradient factor escaped [0.25, 4].'
            }
            if ($sculpt -eq 0 -and $powder -eq 0) { Assert-Near $factor 1 0 'Zero gradient controls changed extinction.' }
        }
    }
}

Write-Host 'Smoke thermal and gradient visual validation passed.'
