$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
function Read-Source([string]$Path) { Get-Content (Join-Path $root $Path) -Raw }
function Require-Match([string]$Text, [string]$Pattern, [string]$Message) {
    if ($Text -notmatch $Pattern) { throw $Message }
}
function Require-Near([double]$Actual, [double]$Expected, [double]$Tolerance, [string]$Message) {
    if ([math]::Abs($Actual - $Expected) -gt $Tolerance) {
        throw "$Message (actual=$Actual expected=$Expected tolerance=$Tolerance)"
    }
}

$cvars = Read-Source 'source/common/rendering/nri/renderer/nri_cvars.cpp'
$settings = Read-Source 'source/common/rendering/nri/renderer/nri_renderer_settings.cpp'
$owner = Read-Source 'source/common/rendering/nri/renderer/nri_smoke_visuals.cpp'
$visuals = Read-Source 'source/common/rendering/nri/shaders/Include/SmokeVisuals.hlsli'
$evaluate = Read-Source 'source/common/rendering/nri/shaders/SmokeEvaluateGrid.cs.hlsl'
$compact = Read-Source 'source/common/rendering/nri/shaders/SmokeEvaluateGridCompact.cs.hlsl'
$runtime = Read-Source 'source/common/rendering/nri/renderer/nri_smoke.cpp'

foreach ($default in @(
    'CVAR(Float, nri_ptsmokethicknessstrength, 0.0f, 0)',
    'CVAR(Float, nri_ptsmokeflowhighlight, 0.0f, 0)',
    'CVAR(Float, nri_ptsmokecurlhighlight, 0.0f, 0)',
    'CVAR(Float, nri_ptsmokecompressionsculpt, 0.0f, 0)',
    'CVAR(Float, nri_ptsmokebandstrength, 0.0f, 0)',
    'CVAR(Float, nri_ptsmokecontourstrength, 0.0f, 0)')) {
    if (-not $cvars.Contains($default)) { throw "Missing session-only identity default: $default" }
}
Require-Match $settings 'thicknessStrength[\s\S]*-1\.0f,\s*1\.0f[\s\S]*thicknessSteps[\s\S]*1,\s*4[\s\S]*flowHighlight[\s\S]*curlHighlight[\s\S]*compressionSculpt[\s\S]*bandStrength[\s\S]*bandCount[\s\S]*2,\s*9[\s\S]*bandSoftness[\s\S]*0\.02f,\s*0\.49f[\s\S]*contourStrength' 'Carrier visual settings are not bounded in the frame snapshot.'
Require-Match $owner 'PackHalf2\(settings\.thicknessStrength,\s*settings\.thicknessPivot\)[\s\S]*PackHalf2\(settings\.flowHighlight,\s*settings\.flowSpeedReference\)[\s\S]*PackHalf2\(settings\.curlHighlight,\s*settings\.curlReference\)[\s\S]*PackHalf2\(settings\.compressionSculpt,\s*settings\.divergenceReference\)[\s\S]*PackHalf2\(settings\.bandStrength,\s*settings\.bandCount\)[\s\S]*PackHalf2\(settings\.bandSoftness,\s*settings\.contourStrength\)' 'Carrier visual packing is incomplete or reordered.'
Require-Match $owner 'runtimeLightTileCountX\s*=\s*words\[9\][\s\S]*runtimeLightTileCountY\s*=\s*words\[10\][\s\S]*particleCapacity\s*=\s*words\[11\][\s\S]*commandCount\s*=\s*words\[12\][\s\S]*styleCount\s*=\s*words\[13\][\s\S]*runtimeLightCount\s*=\s*words\[14\][\s\S]*lightSamples\s*=\s*words\[15\]' 'EvaluateGrid pass-local aliases do not match packed word ownership.'
Require-Match $owner 'for\s*\(uint32_t\s+word\s*:\s*PackMaterializationWords\(settings\)\)\s*HashWord' 'Carrier visual words do not invalidate visual histories.'
Require-Match $runtime 'EvaluateGrid\s*\|\|\s*pass\s*==\s*NRISmokePass::EvaluateGridCompact[\s\S]*NRIPopulateSmokeVisualMaterializationConstants' 'Pass-local aliases must only overlay shared grid materializers.'
Require-Match $compact '#include\s+"SmokeEvaluateGrid\.cs\.hlsl"[\s\S]*SmokeEvaluateGridFroxel' 'Compact materialization must share carrier visual math.'

# Item 8 must short-circuit and walk only current reciprocal topology.
Require-Match $evaluate '!SmokeVisualThicknessEnabled\(\)[\s\S]*return\s+1\.0[\s\S]*SmokeGridLightDirectedFaceOpen\(cursor,\s*face\)[\s\S]*cursor\s*\+=\s*NRI_SMOKE_GRID_LIGHT_LOBE_AXES\[face\][\s\S]*SmokeGridLightCellAddress\(cursor' 'Thickness does not fail closed through the reciprocal topology helper.'
Require-Match $evaluate 'integratedSource\s*\+=\s*correlatedSource\s*\*\s*thicknessGain\s*\*\s*sampleFlowGain' 'Thickness and flow must grade source rather than transmittance.'

# Item 9 must add no default field bandwidth, preserve dormant velocity, and
# suppress derivatives where fine/dormant authority is mixed.
Require-Match $evaluate 'if\s*\(loadVelocity\)[\s\S]*gSmokeRenderGridVelocityB[\s\S]*if\s*\(loadVelocity\)\s*velocity\s*=\s*gSmokeDormantVelocity' 'Conditional fine/dormant velocity loads are missing.'
Require-Match $visuals 'mixedAuthority\s*=\s*dormantMask\s*!=\s*0u\s*&&\s*dormantMask\s*!=\s*0xffu[\s\S]*derivativeGate\s*=\s*mixedAuthority\s*\?\s*0\.0' 'Mixed fine/dormant derivatives must fail closed.'
Require-Match $visuals 'turbulenceScale\s*=\s*max\(velocity\.w\s*/\s*max\(scalar\.x' 'Flow response must consume the transported turbulence-scale moment after interpolation.'

# Item 10 is coefficient-space, skips exact identity before log/floor, remaps
# scattering/source safely, and precedes thermal emission.
Require-Match $visuals '!SmokeVisualIllustrationEnabled\(\)[\s\S]*return;[\s\S]*log2\(max\(extinction[\s\S]*smoothstep\(0\.5\s*-\s*softness,\s*0\.5\s*\+\s*softness[\s\S]*SmokeVisualShapeScatteringChannel[\s\S]*SmokeVisualApplyScatteringTint' 'Soft coefficient bands and contours do not preserve the bounded medium contract.'
Require-Match $visuals 'SmokeVisualContourMask[\s\S]*effectiveWidth\s*=\s*min\(max\(softness,\s*0\.5\s*\*\s*footprint\),\s*0\.5\)[\s\S]*areaScale\s*=\s*softness\s*/\s*effectiveWidth[\s\S]*SmokeVisualContourPhaseFootprint' 'Contour transfer is not area-preserving across an under-resolved sample footprint.'
Require-Match $evaluate 'contourEnabled[\s\S]*halfSubFootprintUv[\s\S]*SmokeVisualContourPhaseFootprint[\s\S]*SmokeVisualExtinctionGradient[\s\S]*integratedContour[\s\S]*integratedContourWeight[\s\S]*SmokeVisualApplyIllustration\(contourCoverage' 'Contour mask is not filtered in existing world-grid quadrature before froxel publication.'
Require-Match $evaluate 'SmokeVisualApplyGradientTint[\s\S]*SmokeVisualApplyIllustration[\s\S]*SmokeVisualApplyThermal' 'Illustration must run before additive thermal glow.'

function Get-CurlAndDivergence([double[]]$Dx, [double[]]$Dy, [double[]]$Dz) {
    return [pscustomobject]@{
        Curl = @(($Dy[2]-$Dz[1]), ($Dz[0]-$Dx[2]), ($Dx[1]-$Dy[0]))
        Divergence = $Dx[0]+$Dy[1]+$Dz[2]
    }
}
$constant = Get-CurlAndDivergence -Dx @(0,0,0) -Dy @(0,0,0) -Dz @(0,0,0)
foreach ($component in $constant.Curl) { Require-Near $component 0 1e-12 'Constant velocity produced curl.' }
Require-Near $constant.Divergence 0 1e-12 'Constant velocity produced divergence.'
$rotation = Get-CurlAndDivergence -Dx @(0,2,0) -Dy @(-2,0,0) -Dz @(0,0,0)
Require-Near $rotation.Curl[2] 4 1e-12 'Rigid rotation curl must equal twice angular velocity.'
$expansion = Get-CurlAndDivergence -Dx @(3,0,0) -Dy @(0,3,0) -Dz @(0,0,3)
Require-Near $expansion.Divergence 9 1e-12 'Uniform expansion divergence is incorrect.'

function Get-SoftBand([double]$Extinction, [int]$Count, [double]$Softness) {
    if ($Extinction -le 1e-6) { return 0.0 }
    $scaled = [math]::Log($Extinction, 2) * $Count
    $level = [math]::Floor($scaled)
    $fraction = $scaled - $level
    $low = 0.5 - $Softness; $high = 0.5 + $Softness
    $t = [math]::Max(0, [math]::Min(1, ($fraction-$low)/($high-$low)))
    $smooth = $t*$t*(3-2*$t)
    return [math]::Pow(2, ($level+$smooth)/$Count)
}
foreach ($count in @(2,4,9)) {
    $previous = 0.0
    foreach ($step in 0..4096) {
        $extinction = [math]::Pow(2, -12.0 + 20.0*$step/4096.0)
        $banded = Get-SoftBand $extinction $count 0.25
        if ([double]::IsNaN($banded) -or [double]::IsInfinity($banded) -or $banded -lt $previous) {
            throw "Soft band transfer is invalid/non-monotonic at count=$count extinction=$extinction."
        }
        $previous = $banded
    }
}

function Get-FilteredContour([double]$Phase, [double]$Softness, [double]$Footprint) {
    $width = [math]::Min([math]::Max($Softness, 0.5*$Footprint), 0.5)
    $distance = [math]::Abs(($Phase-[math]::Floor($Phase))-0.5)
    $t = [math]::Max(0.0, [math]::Min(1.0, $distance/$width))
    $smooth = $t*$t*(3-2*$t)
    return [math]::Max(0.0, [math]::Min(1.0, (1-$smooth)*$Softness/$width))
}
foreach ($softness in @(0.12, 0.30)) {
    # A resolved footprint retains the contour's integrated darkness instead
    # of selecting one whole screen froxel at full strength.
    Require-Near (Get-FilteredContour 0.5 $softness 1.0) (2.0*$softness) 1e-12 'Full-period contour filtering lost the broadened peak.'
    $sum = 0.0
    foreach ($step in 0..4095) { $sum += Get-FilteredContour (($step+0.5)/4096.0) $softness 0.6 }
    Require-Near ($sum/4096.0) $softness 2e-4 'Filtered contour does not preserve average darkness.'
    Require-Near (Get-FilteredContour 0.5 $softness 0.0) 1.0 1e-12 'Resolved contour changed the original peak.'
}

Write-Host 'Smoke carrier visual validation passed.'
