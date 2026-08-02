$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
function Read-Source([string]$Path) {
    return Get-Content (Join-Path $root $Path) -Raw
}
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
$contracts = Read-Source 'source\common\rendering\nri\renderer\nri_smoke_contracts.h'
$constants = Read-Source 'source\common\rendering\nri\shaders\Include\SmokeConstants.hlsli'
$visuals = Read-Source 'source\common\rendering\nri\shaders\Include\SmokeVisuals.hlsli'
$evaluate = Read-Source 'source\common\rendering\nri\shaders\SmokeEvaluateGrid.cs.hlsl'
$compact = Read-Source 'source\common\rendering\nri\shaders\SmokeEvaluateGridCompact.cs.hlsl'
$composite = Read-Source 'source\common\rendering\nri\shaders\SmokeComposite.cs.hlsl'
$smoke = Read-Source 'source\common\rendering\nri\renderer\nri_smoke.cpp'
$renderer = Read-Source 'source\common\rendering\nri\renderer\nri_renderer.cpp'

# Experimental controls are session-only and identity by default, preventing
# an INI written by another worktree from changing the accepted smoke look.
foreach ($default in @(
    'CVAR(Float, nri_ptsmokeextinctionthreshold, 0.0f, 0)',
    'CVAR(Float, nri_ptsmokeextinctionknee, 0.0f, 0)',
    'CVAR(Float, nri_ptsmokeextinctiongamma, 1.0f, 0)',
    'CVAR(Float, nri_ptsmokeextinctionreference, 1.0f, 0)',
    'CVAR(Float, nri_ptsmokeextinctionshoulder, 0.0f, 0)',
    'CVAR(Float, nri_ptsmokethincolorr, 1.0f, 0)',
    'CVAR(Float, nri_ptsmokethincolorg, 1.0f, 0)',
    'CVAR(Float, nri_ptsmokethincolorb, 1.0f, 0)',
    'CVAR(Float, nri_ptsmokecorecolorr, 1.0f, 0)',
    'CVAR(Float, nri_ptsmokecorecolorg, 1.0f, 0)',
    'CVAR(Float, nri_ptsmokecorecolorb, 1.0f, 0)',
    'CVAR(Float, nri_ptsmokecolorpivot, 0.05f, 0)'
)) {
    if (-not $cvars.Contains($default)) { throw "Missing identity visual default: $default" }
}
Assert-Match $settings 'nri_ptsmokedebug,\s*0,\s*21' 'Smoke debug must expose field modes 12 through 21.'
Assert-Match $settings 'extinctionThreshold[\s\S]*extinctionKnee[\s\S]*extinctionGamma[\s\S]*extinctionReference[\s\S]*extinctionShoulder[\s\S]*thinColor[\s\S]*coreColor[\s\S]*colorPivot' 'Every visual control must enter the per-frame settings snapshot.'

# D3D12 rejects even the nominally 64-DWORD expanded signature. Volume-only
# visual words therefore reuse the simulation-only time/wind lanes and retain
# the proven 216-byte root layout without changing simulation dispatch values.
Assert-Match $contracts 'Simulation dispatch:\s*time scale\.\s*Volume dispatch:\s*packed smoke visual word 0[\s\S]*Simulation dispatch:\s*world wind\.\s*Volume dispatch:\s*packed visual words 1\.\.3[\s\S]*sizeof\(NRISmokeConstants\)\s*==\s*216' 'CPU visual packing must reuse the phase-exclusive time/wind lanes and preserve 216 bytes.'
Assert-Match $constants 'Simulation dispatch:\s*time scale\.\s*Volume dispatch:\s*packed visual word 0[\s\S]*Simulation dispatch:\s*world wind\.\s*Volume dispatch:\s*packed visual words 1\.\.3' 'HLSL must document the matching phase-exclusive visual lanes.'
Assert-Match (Read-Source 'source\common\rendering\nri\renderer\nri_smoke_visuals.cpp') 'StorePackedWord\(constants\.timeScale[\s\S]*StorePackedWord\(constants\.wind\[0\][\s\S]*StorePackedWord\(constants\.wind\[1\][\s\S]*StorePackedWord\(constants\.wind\[2\]' 'CPU visual packing must populate all four reused lanes.'
Assert-Match $visuals 'SmokeVisualPacked0\(\).*TimeScale[\s\S]*SmokeVisualPacked1\(\).*Wind\.x[\s\S]*SmokeVisualPacked2\(\).*Wind\.y[\s\S]*SmokeVisualPacked3\(\).*Wind\.z' 'Shader visual decoding must read the same four reused lanes.'
Assert-Match $renderer 'requiredRootConstantSize\s*=\s*std::max\(\{[^}]*sizeof\(NRISmokeConstants\)' 'Backend availability must include the enlarged smoke root block.'

# Diagnostics are grid-only current-field views: shared dense/compact math,
# coefficient-correlated source, no downstream carrier lighting, and no final
# volume history.
foreach ($mode in 1..10) {
    Assert-Match $visuals ("NRI_SMOKE_FIELD_DEBUG_[A-Z_]+\s+{0}u" -f $mode) "Missing field diagnostic mode $mode."
}
Assert-Match $evaluate 'SmokeVisualLocalDiagnostic[\s\S]*\*\s*sampleFieldDebugExtinction' 'Local diagnostics must form color times their support coefficient per quadrature sample.'
Assert-Match $evaluate 'SmokeVisualLobeDiagnostic[\s\S]*\*\s*sampleExtinction' 'Lobe diagnostics must form color times extinction per quadrature sample.'
Assert-Match $evaluate 'fieldDebugMode\s*!=\s*0u[\s\S]*source\s*=\s*integratedFieldDebug[\s\S]*source\s*\*=\s*sampleWeight' 'Field diagnostic source must retain the complete quadrature denominator.'
Assert-Match $evaluate 'massSupport\s*=\s*fieldDebugMode\s*==\s*NRI_SMOKE_FIELD_DEBUG_MASS[\s\S]*NRI_SMOKE_FIELD_DEBUG_THERMAL[\s\S]*SmokeVisualTone\(max\(sampleScalar\.x,\s*0\.0\)\)\s*/\s*cellSize[\s\S]*fieldDebugExtinction' 'Mass and thermal diagnostics must remain visible when mass exists without optical extinction.'
Assert-Match $compact '#define\s+NRI_SMOKE_EVALUATE_GRID_LIBRARY[\s\S]*#include\s+"SmokeEvaluateGrid.cs.hlsl"[\s\S]*SmokeEvaluateGridFroxel' 'Compact materialization must call the shared diagnostic and shaping implementation.'
Assert-Match $composite 'SmokeDebugMode\(gSmokeConstants\.DebugMode\)\s*>=\s*12u[\s\S]*color\s*=\s*max\(volume\.rgb,\s*0\.0\)' 'Field diagnostics must present volume color without the beauty scene.'
Assert-Match $smoke 'fieldDiagnostics\s*=\s*mSettings\.debugMode\s*>=\s*12u[\s\S]*volumeHistoryAllowed\s*=\s*mSettings\.volumeHistory\s*&&\s*!fieldDiagnostics' 'Field diagnostics must force final volume history off.'
Assert-Match $smoke 'if\s*\(!fieldDiagnostics\)[\s\S]*NRIGpuTimingScope::SmokeViewPoint[\s\S]*if\s*\(!fieldDiagnostics\)[\s\S]*NRIGpuTimingScope::SmokeViewDirectional' 'Field diagnostics must skip later point and directional light additions.'

# Optical shaping happens once at the shared publication seam. Source receives
# the final per-channel scattering ratio, including density tint and clamps.
Assert-Match $evaluate 'SmokeVisualSculptExtinction\(baseExtinction,\s*edgeMask\)[\s\S]*flowSculptedExtinction[\s\S]*SmokeVisualShapeMedium\(baseExtinction,\s*flowSculptedExtinction,\s*baseScattering,\s*source[\s\S]*gSmokeFroxelMedium\[froxelIndex\]' 'Grid shaping must occur after support integration and before froxel publication.'
Assert-Match $visuals 'SmokeVisualShapeScatteringChannel[\s\S]*baseExtinction\s*<=\s*0\.0[\s\S]*candidate\s*=\s*extinction\s*\*\s*\(baseScattering\s*/\s*baseExtinction\)\s*\*\s*tint[\s\S]*isfinite\(candidate\)[\s\S]*clamp\(candidate,\s*0\.0,\s*extinction\)' 'Scattering shaping must use a finite ratio-equivalent albedo form bounded by extinction.'
Assert-Match $visuals 'SmokeVisualIncidentChannel[\s\S]*source\s*/\s*scattering[\s\S]*isfinite\(incident\)[\s\S]*source\s*=\s*incident\s*\*\s*scattering' 'Existing correlated source must receive the ratio-equivalent final scattering grade.'
Assert-Match $smoke 'visualHistoryHash\s*=\s*NRIHashSmokeVisualSettings[\s\S]*mLastSmokeVisualHash\s*==\s*visualHistoryHash[\s\S]*volumeLightingHash\s*=\s*HashCombine64\(volumeLightingHash,\s*visualHistoryHash\)' 'Visual changes must invalidate light and final-volume histories.'

function Get-ShapedExtinction(
    [double]$Base, [double]$Threshold, [double]$Knee,
    [double]$Gamma, [double]$Reference, [double]$Shoulder) {
    $baseValue = [math]::Max($Base, 0.0)
    if ($baseValue -le 0.0) { return 0.0 }
    $shaped = $baseValue
    if ($Threshold -gt 0.0 -or $Knee -gt 0.0) {
        $low = [math]::Max($Threshold - $Knee, 0.0)
        $high = [math]::Max($Threshold + $Knee, $low + 1e-6)
        $t = [math]::Max(0.0, [math]::Min(1.0, ($baseValue - $low) / ($high - $low)))
        $smooth = $t * $t * (3.0 - 2.0 * $t)
        $shaped *= $smooth
    }
    $referenceValue = [math]::Max($Reference, 1e-6)
    $shaped = $referenceValue * [math]::Pow([math]::Max($shaped / $referenceValue, 0.0), [math]::Max($Gamma, 0.05))
    if ($Shoulder -gt 1e-6) {
        $shaped = $Shoulder * (1.0 - [math]::Exp(-$shaped / $Shoulder))
    }
    return [math]::Max($shaped, 0.0)
}

# Identity, exact zero, finiteness and monotonicity over representative control
# extremes are mathematical gates rather than screenshot judgments.
foreach ($baseValue in @(0.0, 1e-8, 0.0001, 0.01, 0.1, 1.0, 8.0)) {
    Assert-Near (Get-ShapedExtinction $baseValue 0 0 1 1 0) $baseValue 1e-12 'Identity transfer changed extinction.'
}
$controlCases = @(
    @(0.02, 0.01, 0.6, 0.05, 0.0),
    @(0.00, 0.03, 1.8, 0.10, 0.0),
    @(0.04, 0.04, 1.2, 0.02, 0.12),
    @(0.20, 0.00, 0.4, 1.00, 0.50)
)
foreach ($controls in $controlCases) {
    Assert-Near (Get-ShapedExtinction 0 $controls[0] $controls[1] $controls[2] $controls[3] $controls[4]) 0 0 'Zero extinction did not remain exact zero.'
    $previous = -1.0
    foreach ($step in 0..4096) {
        $baseValue = 4.0 * $step / 4096.0
        $value = Get-ShapedExtinction $baseValue $controls[0] $controls[1] $controls[2] $controls[3] $controls[4]
        if ([double]::IsNaN($value) -or [double]::IsInfinity($value) -or $value -lt 0.0) {
            throw "Transfer produced an invalid value for base=$baseValue controls=$controls"
        }
        if ($value + 1e-12 -lt $previous) {
            throw "Transfer is not monotonic for base=$baseValue controls=$controls previous=$previous value=$value"
        }
        $previous = $value
    }
    if ($controls[4] -gt 0.0 -and $previous -gt $controls[4] + 1e-12) {
        throw 'High-density shoulder exceeded its bound.'
    }
}

function Get-ShapedMedium(
    [double]$BaseExtinction, [double[]]$BaseScattering, [double[]]$BaseSource,
    [double]$ShapedExtinction, [double[]]$Tint) {
    $scattering = [double[]]@(0,0,0)
    $source = [double[]]@(0,0,0)
    foreach ($channel in 0..2) {
        $safeScattering = [math]::Max($BaseScattering[$channel], 0.0)
        $albedo = $(if ($BaseExtinction -gt 0.0) { $safeScattering / $BaseExtinction } else { 0.0 })
        $candidate = $(if ($ShapedExtinction -gt 0.0) { $ShapedExtinction * $albedo * [math]::Max($Tint[$channel], 0.0) } else { 0.0 })
        $scattering[$channel] = [math]::Min([math]::Max($candidate, 0.0), $ShapedExtinction)
        $incident = $(if ($safeScattering -gt 0.0) { [math]::Max($BaseSource[$channel], 0.0) / $safeScattering } else { 0.0 })
        $source[$channel] = $incident * $scattering[$channel]
    }
    return [pscustomobject]@{ extinction=$ShapedExtinction; scattering=$scattering; source=$source }
}

$mediumCases = @(
    @(.12, @(.04,.03,.02), @(.8,.6,.4), .12, @(1,1,1)),
    @(.12, @(.04,.03,.02), @(.8,.6,.4), .06, @(1.2,.8,.4)),
    @(.12, @(.11,.08,.05), @(2,1,.5), .20, @(2,2,2)),
    @(.12, @(.04,.03,.02), @(.8,.6,.4), .08, @(0,0,0)),
    @(1e-38, @(5e-39,2e-39,1e-40), @(1e-38,4e-39,2e-40), .1, @(1,1.5,.5))
)
foreach ($case in $mediumCases) {
    $medium = Get-ShapedMedium $case[0] $case[1] $case[2] $case[3] $case[4]
    if ([double]::IsNaN($medium.extinction) -or [double]::IsInfinity($medium.extinction) -or $medium.extinction -lt 0) {
        throw 'Shaped extinction became invalid.'
    }
    foreach ($channel in 0..2) {
        if ([double]::IsNaN($medium.scattering[$channel]) -or [double]::IsInfinity($medium.scattering[$channel]) -or
            $medium.scattering[$channel] -lt 0 -or $medium.scattering[$channel] -gt $medium.extinction + 1e-12) {
            throw "Shaped scattering escaped [0, extinction] in channel $channel."
        }
        if ([double]::IsNaN($medium.source[$channel]) -or [double]::IsInfinity($medium.source[$channel]) -or $medium.source[$channel] -lt 0) {
            throw "Shaped source became invalid in channel $channel."
        }
    }
}

function Convert-GradeCode([double]$Value) {
    $value = [math]::Max(0.0, [math]::Min(2.0, $Value))
    if ($value -le 1.0) { return [math]::Max(0, [math]::Min(16, [math]::Round($value * 16.0))) }
    return 16 + [math]::Max(0, [math]::Min(15, [math]::Round(($value - 1.0) * 15.0)))
}
function Convert-GradeValue([int]$Code) {
    if ($Code -le 16) { return $Code / 16.0 }
    return 1.0 + ($Code - 16) / 15.0
}
Assert-Near (Convert-GradeValue (Convert-GradeCode 0.0)) 0.0 0 'Packed grade must preserve zero.'
Assert-Near (Convert-GradeValue (Convert-GradeCode 1.0)) 1.0 0 'Packed grade must preserve the identity exactly.'
Assert-Near (Convert-GradeValue (Convert-GradeCode 2.0)) 2.0 0 'Packed grade must preserve its upper bound.'

# The analytic trilinear gradient must reproduce a linear world field exactly
# and remain zero for constants. This mirrors the eight-corner implementation.
function Get-TrilinearGradient([double[]]$V, [double[]]$B, [double]$CellSize) {
    $dx0 = (1-$B[1])*($V[1]-$V[0]) + $B[1]*($V[3]-$V[2])
    $dx1 = (1-$B[1])*($V[5]-$V[4]) + $B[1]*($V[7]-$V[6])
    $dy0 = (1-$B[0])*($V[2]-$V[0]) + $B[0]*($V[3]-$V[1])
    $dy1 = (1-$B[0])*($V[6]-$V[4]) + $B[0]*($V[7]-$V[5])
    $dz0 = (1-$B[0])*($V[4]-$V[0]) + $B[0]*($V[5]-$V[1])
    $dz1 = (1-$B[0])*($V[6]-$V[2]) + $B[0]*($V[7]-$V[3])
    $gx = ((1-$B[2])*$dx0+$B[2]*$dx1)/$CellSize
    $gy = ((1-$B[2])*$dy0+$B[2]*$dy1)/$CellSize
    $gz = ((1-$B[1])*$dz0+$B[1]*$dz1)/$CellSize
    return [double[]]@($gx, $gy, $gz)
}
$constantGradient = Get-TrilinearGradient ([double[]](@(3.0) * 8)) @(0.2,0.7,0.4) 8.0
foreach ($component in $constantGradient) { Assert-Near $component 0 1e-12 'Constant field produced a gradient.' }
$linearCorners = [double[]]@(0,16,24,40,32,48,56,72) # 2x + 3y + 4z over an 8-unit cell
foreach ($blend in @(@(0.0,0.0,0.0), @(0.25,0.5,0.75), @(1.0,1.0,1.0))) {
    $gradient = Get-TrilinearGradient $linearCorners $blend 8.0
    Assert-Near $gradient[0] 2.0 1e-12 'Linear X gradient changed across the cell.'
    Assert-Near $gradient[1] 3.0 1e-12 'Linear Y gradient changed across the cell.'
    Assert-Near $gradient[2] 4.0 1e-12 'Linear Z gradient changed across the cell.'
}

Write-Host 'Smoke field diagnostics and optical shaping validation passed.'
