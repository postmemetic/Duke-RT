$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
$source = Get-Content -Raw (Join-Path $root 'source\common\rendering\nri\renderer\nri_smoke_emitters.cpp')
$continuous = Get-Content -Raw (Join-Path $root 'source\common\rendering\nri\renderer\nri_smoke_continuous_sources.h')
$dormant = Get-Content -Raw (Join-Path $root 'source\common\rendering\nri\renderer\nri_smoke_dormant_injection.cpp')

function Assert-Match([string]$Text, [string]$Pattern, [string]$Message) {
    if ($Text -notmatch $Pattern) { throw $Message }
}

Assert-Match $source 'TimedEmission[\s\S]*cadenceOrdinal[\s\S]*cadenceStepsBeforeInterval[\s\S]*skipped[\s\S]*emissionIndex' 'Pulse phase must follow logical cadence crossings, including a retained hitch suffix.'
Assert-Match $source 'NRIEvaluateSmokeSourceEnvelope\([\s\S]*emission\.cadenceOrdinal[\s\S]*command\.densityScale = rule\.densityScale \* pulseWeight' 'Actor command mass must be sampled from its logical cadence ordinal.'
Assert-Match $source 'state\.continuousCadenceOrdinal \+= continuousCadenceSteps[\s\S]*observation\.cadenceOrdinal = state\.continuousCadenceOrdinal' 'Persistent observations must publish the complete logical cadence range.'
Assert-Match $source 'observation\.pulseEnvelope = sourceEnvelope' 'Persistent observations must retain their authored pulse function.'
Assert-Match $source 'cadence_ordinal=%llu pulse_weight=%.4f density_scale=%.4f' 'Bounded source traces must expose pulse selection.'
Assert-Match $continuous 'request\.firstCadenceOrdinal = state\.firstCadenceOrdinal[\s\S]*request\.lastCadenceOrdinal = state\.lastCadenceOrdinal[\s\S]*request\.pulseEnvelope = state\.latest\.pulseEnvelope' 'Coalesced work must retain the exact ordinal range and pulse function.'
Assert-Match $dormant 'NRISumSmokeSourceEnvelope\(request\.pulseEnvelope,[\s\S]*request\.firstCadenceOrdinal, request\.lastCadenceOrdinal\)[\s\S]*request\.particlesPerCadence \* \(float\)weightedCadences' 'Dormant source mass must sum pulse weights instead of multiplying by the newest phase.'

Write-Host 'Smoke source envelope runtime static validation passed.'
