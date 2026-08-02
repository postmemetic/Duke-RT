$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
$header = Get-Content (Join-Path $root 'source\core\lightoverlay.h') -Raw
$implementation = Get-Content (Join-Path $root 'source\core\lightoverlay.cpp') -Raw
$authored = Get-Content (Join-Path $root 'release-overlay\LIGHTOVR') -Raw
$authoringGuide = Get-Content (Join-Path $root 'LIGHTOVR-AUTHORING.md') -Raw

function Assert-Contains([string]$Text, [string]$Pattern, [string]$Message) {
    if ($Text -notmatch $Pattern) { throw $Message }
}

Assert-Contains $header 'struct ParsedLightOverlaySmokeStyle' 'Missing parsed smoke style contract.'
Assert-Contains $header 'struct ResolvedLightOverlaySmokeActorRule' 'Missing resolved smoke actor contract.'
Assert-Contains $header 'bool styleResolved = false' 'Smoke rule style resolution must be explicit.'
Assert-Contains $header 'LightOverlaySmokeTrigger[\s\S]*?Spawn,[\s\S]*?Interval' 'Smoke actor triggers must preserve spawn and add interval.'
Assert-Contains $header 'LightOverlaySmokeDirectionPolicy[\s\S]*?Aim,[\s\S]*?Normal,[\s\S]*?Incoming' 'Smoke event direction policy is incomplete.'
Assert-Contains $header 'LightOverlaySmokeRepresentation[\s\S]*?Grid,[\s\S]*?Analytic' 'Smoke representation policy is incomplete.'
Assert-Contains $header 'LightOverlaySmokeQueuePolicy[\s\S]*?Retry,[\s\S]*?Drop,[\s\S]*?Latest' 'Smoke queue policy is incomplete.'
Assert-Contains $header 'LightOverlaySmokeTrigger trigger = LightOverlaySmokeTrigger::Spawn' 'Existing actor rules must remain spawn-triggered by default.'
Assert-Contains $header 'LightOverlayActorActivationPolicy activationPolicy = LightOverlayActorActivationPolicy::Immediate' 'Existing smoke actor rules must remain immediately activated by default.'
Assert-Contains $header 'LightOverlaySmokeDirectionPolicy directionPolicy = LightOverlaySmokeDirectionPolicy::Aim' 'Existing event rules must retain aim direction by default.'
Assert-Contains $header 'LightOverlaySmokeRepresentation representation = LightOverlaySmokeRepresentation::Grid' 'Existing smoke rules must remain grid-routed by default.'
Assert-Contains $header 'LightOverlaySmokeQueuePolicy queuePolicy = LightOverlaySmokeQueuePolicy::Retry' 'Existing smoke rules must retain retry behavior by default.'
Assert-Contains $header 'bool hasMaxLatencySeconds = false' 'Existing smoke rules must remain without an implicit freshness deadline.'
Assert-Contains $header 'float densityScale = 1\.0f' 'Smoke emission density scale must default to identity.'
Assert-Contains $header 'float radiusScale = 1\.0f' 'Smoke emission radius scale must default to identity.'
Assert-Contains $header 'float velocityScale = 1\.0f' 'Smoke emission velocity scale must default to identity.'
Assert-Contains $header 'float offsetRandom\[3\] = \{ 0\.0f, 0\.0f, 0\.0f \}' 'Smoke event random offsets must default to no jitter.'
Assert-Contains $header 'float startDistance = 0\.0f' 'Existing smoke actor rules must remain immediately distance-eligible by default.'
Assert-Contains $header 'float startTime = 0\.0f' 'Existing smoke actor rules must remain immediately time-eligible by default.'
Assert-Contains $header 'float pulseAmount = 0\.0f' 'Existing smoke actor rules must remain unmodulated by default.'
Assert-Contains $header 'uint32_t pulsePeriodCadences = 1' 'The disabled smoke pulse period must remain an identity.'
Assert-Contains $header 'float pulsePhase = 0\.0f' 'Smoke pulse phase must default to the canonical cycle origin.'
Assert-Contains $header 'bool emitterForeground = false' 'Existing smoke actor rules must leave their emitter surfaces behind smoke by default.'
Assert-Contains $header 'struct ParsedLightOverlayMapSmokeEmitterRule' 'Missing parsed map smoke-emitter contract.'
Assert-Contains $header 'struct ResolvedLightOverlayMapSmokeEmitterRule' 'Missing resolved map smoke-emitter contract.'
Assert-Contains $header 'TArray<ParsedLightOverlayMapSmokeEmitterRule> mapSmokeEmitterRules' 'Parsed database does not own map smoke emitters.'
Assert-Contains $header 'TArray<ResolvedLightOverlayMapSmokeEmitterRule> mapSmokeEmitterRules' 'Resolved map view does not expose map smoke emitters.'
Assert-Contains $header 'float size\[2\] = \{ 1\.0f, 1\.0f \}' 'Map smoke-emitter size must have a bounded fallback.'
Assert-Contains $header 'float intervalSeconds = 0\.1f' 'Map smoke-emitter cadence default changed.'
Assert-Contains $header 'float velocityScale = 0\.0f' 'Map smoke emitters must default to no authored normal impulse.'

foreach ($block in @('smokestyle', 'smokeactorrule', 'smokeeventrule')) {
    Assert-Contains $implementation ('sc\.Compare\("' + $block + '"\)') "Parser does not recognize $block."
    Assert-Contains $implementation ('"' + $block + ' %s"') "Normalized serializer does not preserve $block."
}

foreach ($field in @(
    'density', 'extinction', 'albedo', 'anisotropy', 'radius', 'expansionvelocity',
    'lifetime', 'densityhalflife', 'risevelocity', 'velocityrandom', 'velocityinherit',
    'buoyancy', 'drag', 'turbulence', 'turbulencescale', 'temperature',
    'momentumscale', 'coolinghalflife')) {
    Assert-Contains $implementation ('sc\.Compare\("' + $field + '"\)') "Smoke style parser is missing $field."
}

foreach ($field in @(
    'ownerclass', 'excludeownerclass', 'activation', 'representation', 'queuepolicy', 'maxlatencyseconds', 'emitterforeground', 'offset', 'densityscale', 'radiusscale',
    'velocitycone', 'velocityscale', 'intervalseconds', 'pulseamount', 'pulseperiodcadences', 'pulsephase', 'starttime', 'startdistance', 'spacing', 'maxsegmentsperframe')) {
    Assert-Contains $implementation ('sc\.Compare\("' + $field + '"\)') "Smoke actor parser is missing $field."
    Assert-Contains $implementation ('"' + $field + ' ') "Smoke actor serializer is missing $field."
}
Assert-Contains $implementation 'LightOverlaySmokeTrigger::Interval' 'Interval actor trigger is not parsed or serialized.'
Assert-Contains $implementation 'expected spawn or interval' 'Invalid actor trigger diagnostics are missing.'
Assert-Contains $implementation 'expected surface or immediate' 'Invalid smoke actor activation diagnostics are missing.'
Assert-Contains $implementation 'ParseOnOffToken\(sc\.String, rule\.emitterForeground\)' 'Smoke emitter foreground must use the shared on/off parser.'
Assert-Contains $implementation 'expected on or off' 'Invalid smoke emitter foreground diagnostics are missing.'
Assert-Contains $implementation 'activation=%s' 'Smoke actor activation is absent from dumps.'
Assert-Contains $implementation 'emitterforeground=%s' 'Smoke emitter foreground is absent from dumps.'

foreach ($field in @('representation', 'queuepolicy', 'maxlatencyseconds', 'velocityscale', 'normaloffset', 'direction')) {
    Assert-Contains $implementation ('sc\.Compare\("' + $field + '"\)') "Smoke event parser is missing $field."
    Assert-Contains $implementation ('"' + $field + ' ') "Smoke event serializer is missing $field."
}
Assert-Contains $implementation 'sc\.Compare\("offsetrandom"\)' 'Smoke event parser is missing offsetrandom.'
Assert-Contains $implementation 'AppendVector3Field\(text, 2, "offsetrandom", rule\.offsetRandom\)' 'Smoke event serializer is missing offsetrandom.'
foreach ($policy in @('aim', 'normal', 'incoming')) {
    Assert-Contains $implementation ('stricmp\(sc\.String, "' + $policy + '"\)') "Smoke event parser is missing direction policy $policy."
}
Assert-Contains $implementation 'expected aim, normal, or incoming' 'Invalid event direction diagnostics are missing.'
Assert-Contains $implementation 'expected grid or analytic' 'Invalid smoke representation diagnostics are missing.'
Assert-Contains $implementation 'expected retry, drop, or latest' 'Invalid smoke queue-policy diagnostics are missing.'
Assert-Contains $implementation 'maxLatencySeconds = std::max\(0\.0f' 'Negative smoke freshness bounds must clamp to zero.'
Assert-Contains $implementation 'offsetRandom[\s\S]*?std::isfinite\(value\) \? std::max\(value, 0\.0f\) : 0\.0f' 'Smoke event random-offset extents must normalize nonfinite and negative values to zero.'
Assert-Contains $implementation 'SmokeRepresentationName\(rule\.representation\)' 'Resolved smoke representation is absent from dumps.'
Assert-Contains $implementation 'SmokeQueuePolicyName\(rule\.queuePolicy\)' 'Resolved smoke queue policy is absent from dumps.'
Assert-Contains $implementation 'LIGHTOVR smokeeventrule[\s\S]*?offsetrandom=\(' 'Parsed smoke-event diagnostics must report random-offset extents.'
Assert-Contains $implementation 'LIGHTOVR resolved smokeeventrule[\s\S]*?offsetrandom=\(' 'Resolved smoke-event diagnostics must report random-offset extents.'
Assert-Contains $authoringGuide '`offsetrandom <right> <forward> <up>`[\s\S]*?finite and nonnegative[\s\S]*?deterministically samples' 'Smoke event random-offset authoring semantics are undocumented.'
Assert-Contains $authored 'smokeeventrule "duke\.chaingun\.primary"[\s\S]*?offset 2\.5 32\.0 5\.0[\s\S]*?offsetrandom 6\.0 0\.0 0\.0' 'Chaingun primary must retain the validated horizontal-only local-right offset and randomness.'

Assert-Contains $implementation 'smokeStyleLookup\[MakeNormalizedKey\(source->id\)\]' 'Resolved styles are not indexed case-insensitively.'
Assert-Contains $implementation 'destination\.styleResolved = style != smokeStyleLookup\.end\(\)' 'Invalid smoke style references are not retained as explicitly unresolved.'
Assert-Contains $implementation 'destination\.actorClass = PClass::FindActor' 'Smoke actor classes are not resolved.'
Assert-Contains $implementation 'destination\.ownerClass = PClass::FindActor' 'Smoke owner-class filters are not resolved.'
Assert-Contains $implementation 'destination\.excludeOwnerClass = PClass::FindActor' 'Smoke excluded-owner filters are not resolved.'
Assert-Contains $implementation 'duplicate %s.*using the last definition' 'Smoke duplicate last-definition-wins behavior is missing.'
Assert-Contains $implementation 'densityhalflife.*minimum = 0\.001f' 'Invalid zero density half-life values are not clamped.'
Assert-Contains $implementation 'startdistance.*std::max\(0\.0f' 'Negative actor start distances must clamp to immediate eligibility.'
Assert-Contains $implementation 'starttime.*std::max\(0\.0f' 'Negative actor start times must clamp to immediate eligibility.'
Assert-Contains $implementation 'pulseAmount = std::clamp[\s\S]*?0\.0f, 1\.0f' 'Smoke pulse amount must clamp to a nonnegative mean-preserving range.'
Assert-Contains $implementation 'pulsePeriodCadences = \(uint32_t\)std::clamp\(sc\.Number, 1, 256\)' 'Smoke pulse periods must remain bounded.'
Assert-Contains $implementation 'pulsePhase = phase - std::floor\(phase\)' 'Smoke pulse phase must wrap to a canonical cycle.'
Assert-Contains $implementation 'smoke_styles=%d smoke_actor_rules=%d smoke_event_rules=%d' 'Smoke family counts are absent from dumps.'
Assert-Contains $implementation 'maxsegmentsperframe=%u' 'Actor interval/segment diagnostics are missing.'
Assert-Contains $implementation 'startdistance=%.3f' 'Actor distance-gate diagnostics are missing.'
Assert-Contains $implementation 'starttime=%.3f' 'Actor time-gate diagnostics are missing.'
Assert-Contains $implementation 'pulseamount=%.3f pulseperiodcadences=%u pulsephase=%.3f' 'Actor pulse diagnostics are missing.'
Assert-Contains $implementation 'normaloffset=%.3f direction=%s' 'Event direction diagnostics are missing.'

Assert-Contains $implementation 'sc\.Compare\("smokeemitter"\)[\s\S]*?ParseMapSmokeEmitterRule\(mapName\)' 'Map parser does not recognize smokeemitter blocks.'
foreach ($field in @(
    'style', 'position', 'normal', 'size', 'rotation', 'offset', 'count', 'intervalseconds',
    'spawnradius', 'densityscale', 'radiusscale', 'velocityscale', 'velocitycone', 'maxsegmentsperframe')) {
    Assert-Contains $implementation ('ParseMapSmokeEmitterRule[\s\S]*?sc\.Compare\("' + $field + '"\)') "Map smoke-emitter parser is missing $field."
}
Assert-Contains $implementation 'rule\.size\[0\] = std::clamp\(\(float\)sc\.Float, 1\.0f, 256\.0f\)' 'Map smoke-emitter width is not clamped to the safe range.'
Assert-Contains $implementation 'rule\.size\[1\] = std::clamp\(\(float\)sc\.Float, 1\.0f, 256\.0f\)' 'Map smoke-emitter length is not clamped to the safe range.'
Assert-Contains $implementation 'rule\.intervalSeconds = std::max\(0\.001f' 'Map smoke-emitter cadence must clamp away from zero.'
Assert-Contains $implementation 'rule\.velocityCone = std::clamp\(\(float\)sc\.Float, 0\.0f, 180\.0f\)' 'Map smoke-emitter cone is not clamped.'
Assert-Contains $implementation 'smokeemitter.*requires style' 'Missing required map smoke-emitter style diagnostic.'
Assert-Contains $implementation 'smokeemitter.*requires position' 'Missing required map smoke-emitter position diagnostic.'
Assert-Contains $implementation 'smokeemitter.*requires normal' 'Missing required map smoke-emitter normal diagnostic.'
Assert-Contains $implementation 'smokeemitter.*requires size' 'Missing required map smoke-emitter size diagnostic.'
Assert-Contains $implementation 'MakeMapScopedKey\(rule\.mapName, "smokeemitter", rule\.id\)' 'Map smoke-emitter duplicate identity is not map-scoped and case-insensitive.'
Assert-Contains $implementation 'AppendMapSmokeEmitterRuleBlock' 'Normalized serializer omits map smoke emitters.'
Assert-Contains $implementation 'resolved\.mapSmokeEmitterRules\.Push' 'Active-map resolver omits map smoke emitters.'
Assert-Contains $implementation 'source\.mapName\.CompareNoCase\(mapName\)' 'Map smoke emitters are not filtered to the active map.'
Assert-Contains $implementation 'ResolvedLightOverlayMapSmokeEmitterRule[\s\S]*?styleResolved = style != smokeStyleLookup\.end\(\)' 'Map smoke-emitter styles are not resolved case-insensitively.'
Assert-Contains $implementation 'LIGHTOVR resolved smokeemitter' 'Resolved map smoke-emitter diagnostics are missing.'
Assert-Contains $implementation 'case LightOverlayRuleKind::MapSmokeEmitter' 'Map smoke-emitter removal parity is missing.'
Assert-Contains $implementation 'AddOrReplaceLightOverlayRule\(ParsedLightOverlayDatabase& database, const ParsedLightOverlayMapSmokeEmitterRule& rule' 'Map smoke-emitter add/replace parity is missing.'

Assert-Contains $authored 'smokestyle\s+"duke_muzzle_smoke"' 'Duke muzzle smoke style is not authored.'
foreach ($eventId in @('duke.pistol.primary', 'duke.shotgun.primary', 'duke.chaingun.primary')) {
    Assert-Contains $authored ('smokeeventrule\s+"' + [regex]::Escape($eventId) + '"') "Missing authored smoke event rule: $eventId"
    Assert-Contains $authored ('smokeeventrule\s+"' + [regex]::Escape($eventId) + '"[\s\S]*?representation\s+analytic[\s\S]*?queuepolicy\s+drop[\s\S]*?maxlatencyseconds\s+0\.075') "Muzzle smoke must use fresh immediate-or-drop analytic presentation: $eventId"
}
foreach ($eventId in @('duke.hitscan.impact.plane', 'duke.hitscan.impact.wall')) {
    Assert-Contains $authored ('smokeeventrule\s+"' + [regex]::Escape($eventId) + '"[\s\S]*?representation\s+analytic[\s\S]*?queuepolicy\s+drop[\s\S]*?maxlatencyseconds\s+0\.050') "Impact smoke must use fresh immediate-or-drop analytic presentation: $eventId"
}
Assert-Contains $authored 'smokeeventrule\s+"nri\.smoke\.test"' 'Missing smoke-only diagnostic event rule.'
Assert-Contains $authored 'smokeeventrule\s+"duke\.shotgun\.primary"[\s\S]*?velocitycone\s+22\.0' 'Shotgun smoke must retain authored directional spread.'
foreach ($eventId in @('duke.pistol.primary', 'duke.chaingun.primary')) {
    Assert-Contains $authored ('smokeeventrule\s+"' + [regex]::Escape($eventId) + '"[\s\S]*?spawnradius\s+3\.0[\s\S]*?densityscale\s+1(?:\.0)?(?:\s|$)') "Pistol/chaingun muzzle smoke must retain enlarged analytic support without doubled opacity: $eventId"
}
Assert-Contains $authored 'smokeeventrule\s+"duke\.hitscan\.impact\.plane"[\s\S]*?densityscale\s+0\.8' 'Plane-impact analytic smoke must retain reduced optical mass.'
Assert-Contains $authored 'smokeeventrule\s+"duke\.hitscan\.impact\.wall"[\s\S]*?densityscale\s+0\.9' 'Wall-impact analytic smoke must retain reduced optical mass.'
if ($authored -match 'smokeactorrule\s+"duke_fire_sustained"') {
    Assert-Contains $authored 'smokeactorrule\s+"duke_fire_sustained"[\s\S]*?actorclass\s+"DukeFire"[\s\S]*?emitterforeground\s+on' 'The local Duke dumpster-fire smoke rule must keep its emitter surface in the foreground.'
}
Write-Host 'Smoke LIGHTOVR schema static validation passed.'
