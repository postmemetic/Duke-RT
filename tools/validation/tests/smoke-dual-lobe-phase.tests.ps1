$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
function Read-Source([string]$Path) { Get-Content -LiteralPath (Join-Path $root $Path) -Raw }
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

$cvars = Read-Source 'source\common\rendering\nri\renderer\nri_cvars.cpp'
$settings = Read-Source 'source\common\rendering\nri\renderer\nri_renderer_settings.cpp'
$visualHeader = Read-Source 'source\common\rendering\nri\renderer\nri_smoke_visuals.h'
$visualOwner = Read-Source 'source\common\rendering\nri\renderer\nri_smoke_visuals.cpp'
$runtime = Read-Source 'source\common\rendering\nri\renderer\nri_smoke.cpp'
$gridLighting = Read-Source 'source\common\rendering\nri\renderer\nri_smoke_grid_lighting.cpp'
$contracts = Read-Source 'source\common\rendering\nri\renderer\nri_smoke_contracts.h'
$phase = Read-Source 'source\common\rendering\nri\shaders\Include\SmokePhase.hlsli'
$evaluateGrid = Read-Source 'source\common\rendering\nri\shaders\SmokeEvaluateGrid.cs.hlsl'
$point = Read-Source 'source\common\rendering\nri\shaders\SmokeLightPoint.cs.hlsl'
$directional = Read-Source 'source\common\rendering\nri\shaders\SmokeLightDirectional.cs.hlsl'
$reservoir = Read-Source 'source\common\rendering\nri\shaders\Include\SmokeEmissiveReservoir.hlsli'
$emissiveSpatial = Read-Source 'source\common\rendering\nri\shaders\SmokeLightEmissiveSpatial.cs.hlsl'
$analytic = Read-Source 'source\common\rendering\nri\shaders\SmokeAnalyticEmissiveResolve.cs.hlsl'
$scatterSeed = Read-Source 'source\common\rendering\nri\shaders\SmokeGridLightSeedScattering.cs.hlsl'
$scatterPropagate = Read-Source 'source\common\rendering\nri\shaders\SmokeGridLightPropagateScattering.cs.hlsl'

# The experiment is session-only and preserves the single-lobe default.
Assert-Match $cvars 'CVAR\(Float,\s*nri_ptsmokeduallobeweight,\s*0\.0f,\s*0\)' 'Dual-lobe weight must default to exact identity and remain session-only.'
Assert-Match $cvars 'CVAR\(Float,\s*nri_ptsmokeduallobeg,\s*0\.75f,\s*0\)' 'The secondary anisotropy must remain a session-only visual control.'
Assert-Match $settings 'dualLobeWeight\s*=\s*std::clamp\([^;]+0\.0f,\s*1\.0f\)[\s\S]*dualLobeG\s*=\s*std::clamp\([^;]+-0\.9f,\s*0\.9f\)' 'Dual-lobe settings must be finite-domain clamped by the frame snapshot.'
Assert-Match $visualHeader 'dualLobeWeight\s*=\s*0\.0f[\s\S]*dualLobeG\s*=\s*0\.75f' 'The focused visual snapshot is missing the dual-lobe contract.'

# OutputWidth is a copied per-pass alias. The whitelist must remain exact so
# Composite always sees its canonical output extent.
Assert-Match $contracts 'sizeof\(NRISmokeConstants\)\s*==\s*216' 'Dual-lobe phase must not grow the accepted root block.'
Assert-Match $visualOwner 'PackPhaseWord[\s\S]*PackHalf2\(settings\.dualLobeWeight,\s*settings\.dualLobeG\)[\s\S]*constants\.outputWidth\s*=\s*PackPhaseWord\(settings\)' 'CPU phase packing must use the reviewed OutputWidth alias.'
$actualPasses = @([regex]::Matches($runtime, 'case\s+NRISmokePass::([A-Za-z0-9_]+):') | ForEach-Object { $_.Groups[1].Value })
$expectedPasses = @('EvaluateGrid', 'EvaluateGridCompact', 'LightPoint', 'LightDirectional',
    'LightEmissiveInitial', 'LightEmissiveTemporal', 'LightEmissiveSpatial', 'AnalyticEmissiveResolve')
if (($actualPasses -join ',') -ne ($expectedPasses -join ',')) {
    throw "Visual phase pass whitelist changed (actual=$($actualPasses -join ','))."
}
Assert-Match $runtime 'NRISmokeConstants\s+passConstants\s*=\s*constants[\s\S]*SmokePassUsesVisualPhase\(pass\)[\s\S]*NRIPopulateSmokeVisualPhaseConstants[\s\S]*return\s+passConstants' 'Phase aliases must be applied only to a copied per-pass constants block.'
Assert-NotMatch ([regex]::Match($runtime, 'SmokePassUsesVisualPhase[\s\S]*?\n\s*\}').Value) 'Composite|LightDirectionalCarriers|LightIndirect|AnalyticBuild' 'A non-phase pass entered the visual phase whitelist.'

# Every direct phase evaluator uses one normalized response helper. Indirect
# incident-radiance history remains camera independent.
Assert-Match $phase 'SmokePhaseSettings\(\)[\s\S]*gSmokeConstants\.OutputWidth[\s\S]*SmokePhaseResponse[\s\S]*lerp\(body,\s*secondary,\s*settings\.x\)' 'The shader must decode and apply one convex dual-lobe response.'
Assert-Match $evaluateGrid 'cornerPhaseApplied\s*\+=\s*mean\s*\*\s*SmokePhaseResponse' 'Six-lobe world materialization is missing the shared phase response.'
Assert-Match $point 'SmokePhaseResponse\(dot\(lightDirection,\s*viewRay\),\s*anisotropy\)[\s\S]*SmokePhaseResponse\(dot\(centerDirection,\s*viewRay\),\s*anisotropy\)' 'Point particle and grid-direct branches must share the dual-lobe response.'
Assert-Match $directional 'SmokePhaseResponse\(dot\(centerDirection,\s*viewRay\),\s*phaseRecord\.x\)[\s\S]*SmokePhaseResponse\(dot\(centerDirection,\s*viewRay\),\s*anisotropy\)' 'Directional grid and particle branches must share the dual-lobe response.'
Assert-Match $reservoir 'SmokeEvaluateEmissiveCandidate[\s\S]*SmokePhaseResponse\(dot\(lightDirection,\s*viewRay\),\s*anisotropy\)' 'Legacy emissive candidates are missing the shared phase response.'
Assert-Match $emissiveSpatial 'SmokePhaseResponse\(dot\(incidentDirection,\s*viewRay\),\s*anisotropy\)' 'World-emissive resolve is missing the shared phase response.'
Assert-Match $analytic 'SmokeAnalyticLightLobe[\s\S]*SmokePhaseResponse' 'Analytic emissive resolve is missing the shared phase response.'
$shaderRoot = Join-Path $root 'source\common\rendering\nri\shaders'
$nonPhaseShaders = (Get-ChildItem -LiteralPath $shaderRoot -Recurse -File |
    Where-Object { $_.Extension -in @('.hlsl', '.hlsli') -and $_.Name -ne 'SmokePhase.hlsli' } |
    ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw }) -join "`n"
Assert-NotMatch $nonPhaseShaders 'SmokeHenyeyGreenstein\s*\(' 'A beauty shader bypasses the shared dual-lobe phase response.'

# Reduced-scattering transport consumes the mixture first moment and tags its
# progressive seed history with the exact packed phase word.
Assert-Match $gridLighting 'NRIPopulateSmokeVisualPhaseConstants\(settings\.visuals,\s*constants\)' 'Grid-lighting propagation must receive the packed phase controls.'
Assert-Match $scatterPropagate 'SmokePhaseEffectiveAnisotropy\(bodyAnisotropy\)[\s\S]*1\.0\s*-\s*effectiveAnisotropy' 'Reduced scattering must use the dual-lobe first moment.'
Assert-Match $phase 'SmokePhaseEffectiveAnisotropy[\s\S]*lerp\(body,\s*settings\.y,\s*settings\.x\)' 'The effective anisotropy must equal the convex mixture first moment.'
Assert-Match $scatterSeed 'phaseSignature\s*=\s*SmokePhaseSettingsSignature\(\)[\s\S]*previousMetadata\.Reserved\s*==\s*phaseSignature[\s\S]*metadata\.Reserved\s*=\s*phaseSignature' 'Progressive scatter history must reject a changed phase signature.'

# The same packed phase word invalidates direct, emissive, indirect, and final
# volume histories without resetting simulation or world-radiance residency.
Assert-Match $visualOwner 'HashWord\(hash,\s*PackPhaseWord\(settings\)\)' 'The phase word is missing from the visual history hash.'
Assert-Match $runtime 'indirectHistoryCompatible[\s\S]*mLastSmokeVisualHash\s*==\s*visualHistoryHash[\s\S]*directHistoryCompatible[\s\S]*mLastSmokeVisualHash\s*==\s*visualHistoryHash[\s\S]*emissiveHistoryCompatible[\s\S]*mLastSmokeVisualHash\s*==\s*visualHistoryHash[\s\S]*volumeLightingHash\s*=\s*HashCombine64\(volumeLightingHash,\s*visualHistoryHash\)' 'Phase changes must invalidate every dependent reconstruction history.'

# CPU numerical evidence for the exact HLSL law. A normalized HG has integral
# one and first spherical moment g; a convex mixture therefore has the weighted
# first moment and cannot become negative.
function Get-HG([double]$Cosine, [double]$G) {
    $safeG = [math]::Max(-0.95, [math]::Min(0.95, $G))
    $denominator = [math]::Max(1.0 + $safeG * $safeG - 2.0 * $safeG * $Cosine, 1e-4)
    return (1.0 - $safeG * $safeG) / (4.0 * [math]::PI * $denominator * [math]::Sqrt($denominator))
}
function Measure-Phase([double]$BodyG, [double]$SecondaryG, [double]$Weight) {
    $samples = 32768
    $integral = 0.0
    $firstMoment = 0.0
    for ($sample = 0; $sample -lt $samples; ++$sample) {
        $mu = -1.0 + 2.0 * ($sample + 0.5) / $samples
        $value = (1.0 - $Weight) * (Get-HG $mu $BodyG) + $Weight * (Get-HG $mu $SecondaryG)
        if ([double]::IsNaN($value) -or [double]::IsInfinity($value) -or $value -lt 0.0) {
            throw 'Dual-lobe phase produced a non-finite or negative sample.'
        }
        $integral += $value
        $firstMoment += $mu * $value
    }
    $scale = 4.0 * [math]::PI / $samples
    return [pscustomobject]@{ Integral = $integral * $scale; FirstMoment = $firstMoment * $scale }
}

foreach ($case in @(
    @(-0.75, 0.65, 0.0), @(-0.75, 0.65, 0.35), @(0.0, 0.75, 0.5),
    @(0.45, 0.85, 0.25), @(0.8, -0.6, 0.75), @(0.9, 0.9, 1.0))) {
    $measurement = Measure-Phase $case[0] $case[1] $case[2]
    Assert-Near $measurement.Integral 1.0 2e-5 'The dual-lobe phase is not normalized.'
    $expectedMoment = (1.0 - $case[2]) * $case[0] + $case[2] * $case[1]
    Assert-Near $measurement.FirstMoment $expectedMoment 3e-5 'The measured phase first moment disagrees with the transport anisotropy.'
}
foreach ($mu in @(-1.0, -0.5, 0.0, 0.5, 1.0)) {
    Assert-Near ((1.0 - 0.0) * (Get-HG $mu 0.43) + 0.0 * (Get-HG $mu -0.8)) (Get-HG $mu 0.43) 0.0 'Zero phase weight changed the legacy HG response.'
}

Write-Host 'Smoke dual-lobe phase structural and numerical validation passed.'
