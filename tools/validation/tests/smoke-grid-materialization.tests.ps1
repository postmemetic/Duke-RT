$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
$evaluate = Get-Content (Join-Path $root 'source\common\rendering\nri\shaders\SmokeEvaluateGrid.cs.hlsl') -Raw

function Assert-Match([string]$Text, [string]$Pattern, [string]$Message) {
    if ($Text -notmatch $Pattern) { throw $Message }
}

function Assert-NotMatch([string]$Text, [string]$Pattern, [string]$Message) {
    if ($Text -match $Pattern) { throw $Message }
}

function Assert-Near([double]$Actual, [double]$Expected, [double]$Tolerance, [string]$Message) {
    if ([math]::Abs($Actual - $Expected) -gt $Tolerance) {
        throw "$Message (actual=$Actual expected=$Expected tolerance=$Tolerance)"
    }
}

# Grid optical authority must integrate the nonlinear froxel support without
# acquiring any dependency on lighting, history, or per-frame sample jitter.
Assert-Match $evaluate 'NRI_SMOKE_GRID_MAX_FOOTPRINT_SAMPLES\s+2u' 'Grid materialization must bound each lateral axis to two strata.'
Assert-Match $evaluate 'NRI_SMOKE_GRID_MAX_DEPTH_SAMPLES\s+8u' 'Grid materialization must bound depth integration to eight strata.'
Assert-Match $evaluate 'SmokeRenderGridIntegrateFroxel[\s\S]*SmokeSliceNearDepth[\s\S]*SmokeSliceFarDepth' 'Grid materialization must integrate the exact nonlinear slice.'
Assert-Match $evaluate 'footprintWidth[\s\S]*TanHalfFovX[\s\S]*footprintHeight[\s\S]*TanHalfFovY[\s\S]*footprintSampleCount' 'Lateral sampling must expand only when a froxel footprint exceeds one grid cell.'
Assert-Match $evaluate 'SmokeWorldSegmentLength[\s\S]*ceil\(segmentWorldLength\s*/\s*cellSize\)[\s\S]*NRI_SMOKE_GRID_MAX_DEPTH_SAMPLES' 'Depth sampling must be derived from world length and grid-cell spacing with a fixed cap.'
Assert-Match $evaluate 'sampleDepthUnit\s*=\s*\(\(float\)depthSample\s*\+\s*0\.5\)\s*/\s*\(float\)depthSampleCount' 'Depth integration must use deterministic midpoint strata.'
Assert-Match $evaluate 'SmokeWorldPosition\(sampleUv,\s*sampleViewDepth\)[\s\S]*integratedScalar\s*\+=[\s\S]*integratedOptical\s*\+=' 'Every stratum must sample and accumulate both scalar and optical moments.'
Assert-Match $evaluate 'sampleWeight\s*=\s*rcp\(\(float\)\(footprintSampleCount\.x\s*\*\s*footprintSampleCount\.y\s*\*\s*depthSampleCount\)\)[\s\S]*scalar\s*=\s*integratedScalar\s*\*\s*sampleWeight[\s\S]*optical\s*=\s*integratedOptical\s*\*\s*sampleWeight' 'All occupied and empty strata must share the complete support denominator.'
Assert-Match $evaluate 'SmokeRenderGridIntegrateFroxel\(dispatchThreadId,\s*cellSize,\s*scalar,\s*optical,\s*source,[\s\S]*fieldDebugExtinction,\s*dormant\)[\s\S]*scalar\.z\s*\*\s*gSmokeConstants\.DensityScale' 'Density scaling and occupancy rejection must occur after support integration.'
Assert-Match $evaluate 'SmokeRenderGridSample\(samplePosition[\s\S]*scalarCorners[\s\S]*opticalCorners' 'Grid materialization must retain the eight matched density corners used by world-light reconstruction.'
Assert-Match $evaluate 'SmokeRenderGridCorrelatedWorldSource[\s\S]*SmokeGridLightLoadShadingRecord[\s\S]*cornerOptical[\s\S]*SmokeGridClampControlledSource[\s\S]*correlatedSource\s*\+=' 'World emissive source must multiply each light corner by its matching density corner before trilinear accumulation.'
Assert-NotMatch $evaluate 'integratedSource\s*\+=\s*max\(sampleOptical\.rgb\s*\*\s*gSmokeConstants\.DensityScale' 'World emissive source must not multiply independently interpolated density and light fields.'
Assert-Match $evaluate 'fieldDebugMode\s*!=\s*0u[\s\S]*source\s*=\s*integratedFieldDebug[\s\S]*worldDebugMode\s*==\s*0u\s*\?\s*integratedSource\s*:\s*integratedWorldDebug[\s\S]*source\s*\*=\s*sampleWeight' 'Physical source and all diagnostic views must retain the common quadrature denominator.'
Assert-Match $evaluate 'gSmokeFroxelSource\[froxelIndex\]\s*=\s*float4\(source,\s*SmokeFroxelMetadataValue\(carrierMetadata\)\)' 'Materialization must publish the quadrature-integrated world emissive source with carrier identity.'
Assert-Match $evaluate 'SmokeRenderGridLookup\(brickCoordinate,\s*brickIndex\)[\s\S]*SmokeRenderDormantLookup\(brickCoordinate,\s*archiveIndex\)' 'Fine authority must win before dormant archive lookup.'
Assert-Match $evaluate 'dormant\s*\?\s*NRI_SMOKE_FROXEL_CARRIER_DORMANT\s*:\s*NRI_SMOKE_FROXEL_CARRIER_GRID' 'Dormant materialization must publish distinct receiver-light ownership.'
Assert-NotMatch $evaluate 'SmokeFroxelCenter\(dispatchThreadId' 'The grid medium must not return to one center sample per froxel.'
Assert-NotMatch $evaluate 'TraceRayInline|RayQuery|gSmokeConstants\.CurrentJitter' 'Grid materialization may consume a completed world field and stamp work, but must not trace, resample candidates, or add jitter.'

function Get-DepthSampleCount([double]$SegmentLength, [double]$CellSize) {
    return [math]::Max(1, [math]::Min(8, [math]::Ceiling($SegmentLength / $CellSize)))
}

$sampleCases = @(
    @(0.0, 1),
    @(7.999, 1),
    @(8.0, 1),
    @(8.001, 2),
    @(63.9, 8),
    @(64.0, 8),
    @(10000.0, 8)
)
foreach ($sampleCase in $sampleCases) {
    $actual = Get-DepthSampleCount $sampleCase[0] 8.0
    if ($actual -ne $sampleCase[1]) {
        throw "Unexpected depth sample count for segment $($sampleCase[0]): $actual"
    }
}
if ((2 * 2 * (Get-DepthSampleCount 10000.0 8.0)) -gt 32) {
    throw 'Grid materialization exceeded its 32-sample per-froxel work bound.'
}

function Get-Average([double[]]$Samples) {
    return ($Samples | Measure-Object -Average).Average
}

# Missing/empty cells remain in the denominator. Normalizing only hits, taking
# a maximum, or storing an unnormalized sum would all turn partial coverage into
# an expanding opaque carrier.
Assert-Near (Get-Average @(0.0, 0.0, 2.0, 0.0)) 0.5 1e-12 'Sparse extinction support was not averaged.'
Assert-Near (Get-Average @(0.0, 4.0, 0.0, 0.0)) 1.0 1e-12 'Sparse scattering support was not averaged.'
foreach ($count in @(1, 4, 8)) {
    Assert-Near (Get-Average ([double[]](@(3.25) * $count))) 3.25 1e-12 'A constant field changed with integration sample count.'
}

# A thin occupied interval missed by the old center lookup contributes its
# proportional optical depth under midpoint quadrature.
$segmentLength = 32.0
$depthCount = Get-DepthSampleCount $segmentLength 8.0
$sum = 0.0
foreach ($sample in 0..($depthCount - 1)) {
    $worldDistance = ($sample + 0.5) * $segmentLength / $depthCount
    $sum += $(if ($worldDistance -ge 8.0 -and $worldDistance -lt 16.0) { 2.0 } else { 0.0 })
}
$averageExtinction = $sum / $depthCount
Assert-Near $averageExtinction 0.5 1e-12 'Thin occupied support disappeared during midpoint integration.'
Assert-Near ($averageExtinction * $segmentLength) 16.0 1e-12 'Integrated optical depth changed during coefficient normalization.'

# Density-weighted anisotropy is reconstructed from the averaged moments, not
# from independently averaged ratios.
$anisotropyNumerator = Get-Average @(0.0, 0.4, 0.0, -0.1)
$anisotropyWeight = Get-Average @(0.0, 0.5, 0.0, 0.5)
Assert-Near ($anisotropyNumerator / $anisotropyWeight) 0.3 1e-12 'Integrated anisotropy moments were not density weighted.'

function Get-CorrelatedSource([double[]]$Sigma, [double[]]$Incident, [double[]]$Weights) {
    $source = 0.0
    for ($corner = 0; $corner -lt 8; ++$corner) {
        $source += $Weights[$corner] * $Sigma[$corner] * $Incident[$corner]
    }
    return $source
}

function Get-IndependentSource([double[]]$Sigma, [double[]]$Incident, [double[]]$Weights) {
    $sigmaMean = 0.0
    $incidentMean = 0.0
    for ($corner = 0; $corner -lt 8; ++$corner) {
        $sigmaMean += $Weights[$corner] * $Sigma[$corner]
        $incidentMean += $Weights[$corner] * $Incident[$corner]
    }
    return $sigmaMean * $incidentMean
}

# Density and incident radiance are defined at the same sparse-grid cells. Their
# product must therefore be formed per corner before reconstruction. Multiplying
# two independently reconstructed fields both attenuates co-located sparse energy
# and invents energy between unrelated density/light corners.
$centerWeights = [double[]](@(0.125) * 8)
$oneDense = [double[]]@(1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0)
$oneLitSame = [double[]]@(1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0)
$oneLitOther = [double[]]@(0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0)
Assert-Near (Get-CorrelatedSource $oneDense $oneLitSame $centerWeights) 0.125 1e-12 'One co-located dense/lit corner was not reconstructed at its trilinear weight.'
Assert-Near (Get-IndependentSource $oneDense $oneLitSame $centerWeights) 0.015625 1e-12 'The regression fixture no longer demonstrates independent-field underweighting.'
Assert-Near (Get-CorrelatedSource $oneDense $oneLitOther $centerWeights) 0.0 1e-12 'Light leaked between different density and incident-radiance corners.'
Assert-Near (Get-IndependentSource $oneDense $oneLitOther $centerWeights) 0.015625 1e-12 'The regression fixture no longer demonstrates independent-field leakage.'

$fourDense = [double[]]@(1.0, 1.0, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0)
$fourLitSame = [double[]]@(1.0, 1.0, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0)
$fourLitOther = [double[]]@(0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0)
Assert-Near (Get-CorrelatedSource $fourDense $fourLitSame $centerWeights) 0.5 1e-12 'Half-cell co-located density/light support was underweighted.'
Assert-Near (Get-CorrelatedSource $fourDense $fourLitOther $centerWeights) 0.0 1e-12 'Complementary half-cell density/light support created false source.'

$constantLight = [double[]](@(1.0) * 8)
$varyingDensity = [double[]]@(0.0, 0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 1.75)
Assert-Near (Get-CorrelatedSource $varyingDensity $constantLight $centerWeights) (Get-Average $varyingDensity) 1e-12 'Constant incident light did not preserve reconstructed density.'
Assert-Near (Get-CorrelatedSource $constantLight $varyingDensity $centerWeights) (Get-Average $varyingDensity) 1e-12 'Constant density did not preserve reconstructed incident light.'
Assert-Near (Get-CorrelatedSource ([double[]](@(0.0) * 8)) $constantLight $centerWeights) 0.0 1e-12 'Zero density produced emissive source.'
Assert-Near (Get-CorrelatedSource $constantLight ([double[]](@(0.0) * 8)) $centerWeights) 0.0 1e-12 'Zero incident light produced emissive source.'

Write-Host 'Smoke grid materialization structural and CPU-mirror validation passed.'
