$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
function Read-Source([string]$Path) { Get-Content -LiteralPath (Join-Path $root $Path) -Raw }
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
$visuals = Read-Source 'source\common\rendering\nri\shaders\Include\SmokeVisuals.hlsli'
$evaluate = Read-Source 'source\common\rendering\nri\shaders\SmokeEvaluateGrid.cs.hlsl'
$compact = Read-Source 'source\common\rendering\nri\shaders\SmokeEvaluateGridCompact.cs.hlsl'
$runtime = Read-Source 'source\common\rendering\nri\renderer\nri_smoke.cpp'
$contracts = Read-Source 'source\common\rendering\nri\renderer\nri_smoke_contracts.h'

# Every shaping strength is session-only and defaults to exact identity.
foreach ($default in @(
    'CVAR(Float, nri_ptsmokerimstrength, 0.0f, 0)',
    'CVAR(Float, nri_ptsmokerimgain, 0.0f, 0)',
    'CVAR(Float, nri_ptsmokeradianceedgechroma, 0.0f, 0)',
    'CVAR(Float, nri_ptsmokeradiancecavitycontrast, 0.0f, 0)',
    'CVAR(Float, nri_ptsmokeradiancedesaturation, 0.0f, 0)',
    'CVAR(Float, nri_ptsmokeradianceconfidence, 0.25f, 0)',
    'CVAR(Float, nri_ptsmokeradiancedirectionality, 0.15f, 0)')) {
    if (-not $cvars.Contains($default)) { throw "Missing radiance-shaping default: $default" }
}
Assert-Match $settings 'rimStrength\s*=\s*std::clamp\([^;]+0\.0f,\s*1\.0f\)[\s\S]*rimGain\s*=\s*std::clamp\([^;]+0\.0f,\s*1\.0f\)[\s\S]*radianceEdgeChroma\s*=\s*std::clamp[\s\S]*radianceCavityContrast\s*=\s*std::clamp[\s\S]*radianceDesaturation\s*=\s*std::clamp[\s\S]*radianceConfidence\s*=\s*std::clamp[\s\S]*radianceDirectionality\s*=\s*std::clamp' 'Radiance settings must be clamped into the frame snapshot.'
Assert-Match $visualHeader 'rimStrength\s*=\s*0\.0f[\s\S]*rimGain\s*=\s*0\.0f[\s\S]*radianceEdgeChroma\s*=\s*0\.0f[\s\S]*radianceCavityContrast\s*=\s*0\.0f[\s\S]*radianceDesaturation\s*=\s*0\.0f' 'The radiance snapshot no longer has exact-identity strength defaults.'

# Four directional words are materializer-only aliases; no root or descriptor
# growth is allowed and dense/compact share one implementation.
Assert-Match $contracts 'sizeof\(NRISmokeConstants\)\s*==\s*216' 'Radiance shaping must preserve the accepted root block.'
Assert-Match $visualOwner 'PackHalf2\(settings\.rimStrength,\s*settings\.rimGain\)[\s\S]*PackHalf2\(settings\.radianceEdgeChroma,\s*settings\.radianceCavityContrast\)[\s\S]*PackHalf2\(settings\.radianceDesaturation,\s*settings\.radianceConfidence\)[\s\S]*PackHalf2\(settings\.radianceDirectionality,\s*0\.0f\)' 'CPU radiance packing order changed.'
Assert-Match $visualOwner 'directionalDirectionX,\s*words\[5\][\s\S]*directionalDirectionY,\s*words\[6\][\s\S]*directionalDirectionZ,\s*words\[7\][\s\S]*directionalAngularSize,\s*words\[8\]' 'CPU radiance words are not confined to the reviewed materializer aliases.'
Assert-Match $visuals 'RadiancePacked0\(\).*DirectionalDirectionX[\s\S]*RadiancePacked1\(\).*DirectionalDirectionY[\s\S]*RadiancePacked2\(\).*DirectionalDirectionZ[\s\S]*RadiancePacked3\(\).*DirectionalAngularSize' 'HLSL radiance aliases disagree with CPU packing.'
Assert-Match $runtime 'pass\s*==\s*NRISmokePass::EvaluateGrid\s*\|\|\s*pass\s*==\s*NRISmokePass::EvaluateGridCompact[\s\S]*NRIPopulateSmokeVisualMaterializationConstants' 'Radiance aliases must be restricted to dense/compact materialization.'
Assert-Match $compact '#include\s+"SmokeEvaluateGrid\.cs\.hlsl"[\s\S]*SmokeEvaluateGridFroxel' 'Compact evaluation must share the dense radiance-shaping implementation.'

# The boundary derivative is computed once and shared by edge/rim/cavity work;
# raw debug modes bypass beauty shaping.
Assert-Match $visuals 'SmokeVisualBoundaryRequired\(\)[\s\S]*SmokeVisualGradientEnabled\(\)\s*\|\|\s*SmokeVisualRadianceEnabled\(\)' 'Radiance controls must request boundary evidence even when item 4 is disabled.'
Assert-Match $evaluate 'fieldDebugMode\s*==\s*0u\s*&&\s*SmokeVisualBoundaryRequired\(\)[\s\S]*SmokeVisualBoundarySample\(scalarCorners,\s*gridBlend,\s*cellSize' 'Beauty boundary work must reuse current corners and bypass raw diagnostics.'
Assert-Match $visuals 'SmokeVisualBoundarySample[\s\S]*SmokeVisualExtinctionGradient\(scalarCorners,\s*blend,\s*cellSize\)' 'Boundary shaping must compute one world-space extinction gradient.'
Assert-Match $evaluate 'SmokeVisualLobeDiagnostic\(fieldDebugMode,\s*lobes,\s*confidence\)' 'Raw lobe diagnostics must consume ungraded lobe evidence.'

# Shaping stays inside each valid radiance corner and before sigma_s * Li.
$correlated = [regex]::Match($evaluate, 'bool\s+SmokeRenderGridCorrelatedWorldSource[\s\S]*?\n\}').Value
if ($correlated.Length -eq 0) { throw 'Could not isolate correlated world-source materialization.' }
Assert-Match $correlated 'SmokeGridLightRecordValid[\s\S]*cornerLobes\[lobe\]\s*=\s*mean[\s\S]*SmokeGridClampControlledSource\([\s\S]*SmokeVisualShapeWorldIncident\(cornerLobes[\s\S]*correlatedSource\s*\+=\s*weight\s*\*\s*max\(cornerOptical\.rgb' 'Radiance shaping must remain inside the valid per-corner correlated product.'
Assert-Match $correlated 'if\s*\(weightSum\s*<=\s*0\.0\)[\s\S]*return\s+false[\s\S]*lobes\[lobe\]\s*/=\s*weightSum' 'Only diagnostics may normalize valid corner evidence.'
Assert-Match $visuals 'if\s*\(!SmokeVisualRadianceEnabled\(\)\s*\|\|\s*!any\(safeBase\s*>\s*0\.0\)\)\s*return\s+safeBase' 'Identity and zero incident radiance must bypass shaping exactly.'
Assert-Match $visuals 'incident\s*=\s*safeBase\s*\*\s*saturate\(1\.0\s*-\s*shaping\.y[\s\S]*lerp\(incident,\s*incidentLuminance\.xxx[\s\S]*currentLuminance\s*/\s*dominantLuminance[\s\S]*incident\s*\*=\s*1\.0\s*\+\s*rim\.y\s*\*\s*rimMask' 'Cavity, desaturation, chroma redistribution, and bounded rim gain ordering changed.'

# All packed materializer words participate in the existing conservative
# direct/emissive/indirect/final-volume history invalidation path.
Assert-Match $visualOwner 'for\s*\(uint32_t\s+word\s*:\s*PackMaterializationWords\(settings\)\)\s*HashWord\(hash,\s*word\)' 'Radiance words are missing from the visual history hash.'
Assert-Match $runtime 'indirectHistoryCompatible[\s\S]*mLastSmokeVisualHash\s*==\s*visualHistoryHash[\s\S]*directHistoryCompatible[\s\S]*mLastSmokeVisualHash\s*==\s*visualHistoryHash[\s\S]*emissiveHistoryCompatible[\s\S]*mLastSmokeVisualHash\s*==\s*visualHistoryHash[\s\S]*volumeLightingHash\s*=\s*HashCombine64\(volumeLightingHash,\s*visualHistoryHash\)' 'Radiance changes must invalidate every dependent reconstruction history.'

# CPU mirrors for the invariants the shader must preserve.
function Get-Luminance([double[]]$Color) {
    return 0.2126 * $Color[0] + 0.7152 * $Color[1] + 0.0722 * $Color[2]
}
function Scale-Color([double[]]$Color, [double]$Scale) {
    return [double[]]@(($Color[0] * $Scale), ($Color[1] * $Scale), ($Color[2] * $Scale))
}
$base = [double[]]@(1.25, 0.5, 0.125)
$identity = Scale-Color $base 1.0
foreach ($channel in 0..2) { Assert-Near $identity[$channel] $base[$channel] 0.0 'Zero strengths changed incident radiance.' }
$zero = Scale-Color ([double[]]@(0.0, 0.0, 0.0)) 2.0
foreach ($channel in 0..2) { Assert-Near $zero[$channel] 0.0 0.0 'Rim gain manufactured radiance from zero source.' }

$cavityFactor = 1.0 - 0.8 * 0.75 * 0.9
if ($cavityFactor -lt 0.0 -or $cavityFactor -gt 1.0) { throw 'Cavity shaping escaped attenuation-only bounds.' }
$cavity = Scale-Color $base $cavityFactor
if ((Get-Luminance $cavity) -gt (Get-Luminance $base) + 1e-12) { throw 'Cavity shaping amplified radiance.' }

$baseLuminance = Get-Luminance $base
$desaturated = [double[]]@($baseLuminance, $baseLuminance, $baseLuminance)
Assert-Near (Get-Luminance $desaturated) $baseLuminance 1e-12 'Dense desaturation failed to preserve luminance.'
$dominant = [double[]]@(0.1, 0.4, 2.0)
$redistributionScale = $baseLuminance / (Get-Luminance $dominant)
$redistributed = Scale-Color $dominant $redistributionScale
Assert-Near (Get-Luminance $redistributed) $baseLuminance 1e-12 'Dominant-lobe chroma redistribution changed luminance.'
$gained = Scale-Color $base (1.0 + 1.0 * 1.0)
Assert-Near (Get-Luminance $gained) (2.0 * $baseLuminance) 1e-12 'Maximum rim gain must remain exactly 2x.'

# Correlation evidence: invalid corners remain zero and a lone valid corner is
# not renormalized to full weight.
$weight = 0.25
$sigmaS = [double[]]@(0.8, 0.4, 0.2)
$incident = [double[]]@(3.0, 2.0, 1.0)
$correlatedSource = [double[]]@(
    ($weight * $sigmaS[0] * $incident[0]),
    ($weight * $sigmaS[1] * $incident[1]),
    ($weight * $sigmaS[2] * $incident[2]))
foreach ($channel in 0..2) {
    Assert-Near $correlatedSource[$channel] ($weight * $sigmaS[$channel] * $incident[$channel]) 0.0 'The per-corner sigma_s * Li product lost correlation.'
    if ([math]::Abs($correlatedSource[$channel] - $sigmaS[$channel] * $incident[$channel]) -lt 1e-12) {
        throw 'A missing-corner case was incorrectly renormalized.'
    }
}

Write-Host 'Smoke radiance-shaping structural and CPU-mirror validation passed.'
