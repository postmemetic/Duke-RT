$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
$header = Get-Content (Join-Path $root 'source\core\fluidoverlay.h') -Raw
$implementation = Get-Content (Join-Path $root 'source\core\fluidoverlay.cpp') -Raw
$cmake = Get-Content (Join-Path $root 'source\CMakeLists.txt') -Raw
$gameControl = Get-Content (Join-Path $root 'source\core\gamecontrol.cpp') -Raw
$releaseOverlayPath = Join-Path $root 'release-overlay\FLUIDOVR'
$developmentOverlayDirectory = Join-Path $root 'full-voxel-overlay'
$developmentOverlayPath = Join-Path $developmentOverlayDirectory 'FLUIDOVR'
$packagedOverlayPath = Join-Path $root 'wadsrc\static\FLUIDOVR'

if (Test-Path $packagedOverlayPath) {
    throw 'FLUIDOVR must be distributed from release-overlay, not embedded in raze.pk3.'
}
if (-not (Test-Path $releaseOverlayPath)) {
    throw 'Missing distributable release-overlay/FLUIDOVR.'
}
$releaseOverlay = Get-Content $releaseOverlayPath -Raw
if (Test-Path $developmentOverlayDirectory) {
    if (-not (Test-Path $developmentOverlayPath)) {
        throw 'The local full-voxel-overlay exists but is missing its FLUIDOVR mirror.'
    }
    $developmentOverlay = Get-Content $developmentOverlayPath -Raw
    if ($developmentOverlay -cne $releaseOverlay) {
        throw 'full-voxel-overlay/FLUIDOVR must exactly mirror release-overlay/FLUIDOVR.'
    }
}

function Assert-Contains([string]$Text, [string]$Pattern, [string]$Message) {
    if ($Text -notmatch $Pattern) { throw $Message }
}

function Get-ProfileBlock([string]$Text, [string]$ProfileId) {
    $escapedId = [regex]::Escape($ProfileId)
    $match = [regex]::Match(
        $Text,
        '(?ms)^\s*profile\s+"' + $escapedId + '"\s*\{\s*(?<body>.*?)^\s*\}')
    if (-not $match.Success) {
        throw "Missing complete overlay profile: $ProfileId"
    }
    return $match.Groups['body'].Value
}

function Get-ProfileScalar([string]$Body, [string]$Field, [string]$ProfileId) {
    $number = '[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?'
    $matches = [regex]::Matches(
        $Body,
        '(?m)^\s*' + [regex]::Escape($Field) + '\s+(?<value>' + $number + ')\s*$')
    if ($matches.Count -ne 1) {
        throw "Profile '$ProfileId' must declare exactly one numeric '$Field' value."
    }
    return [double]::Parse(
        $matches[0].Groups['value'].Value,
        [Globalization.CultureInfo]::InvariantCulture)
}

foreach ($type in @(
    'ParsedFluidOverlayDatabase', 'ResolvedFluidOverlaySet', 'ResolvedFluidOverlayBinding',
    'FluidOverlayMaterialIdentity', 'FluidOverlayMaterialMatch')) {
    Assert-Contains $header ('struct\s+' + $type) "Missing public FLUIDOVR type: $type"
}

foreach ($api in @(
    'GetParsedFluidOverlayDatabase', 'GetResolvedFluidOverlaySet', 'ResolveFluidOverlaysForMap',
    'ResolveFluidOverlayMaterial', 'ParseFluidOverlays', 'SerializeFluidOverlayDatabase')) {
    Assert-Contains $header ([regex]::Escape($api) + '\s*\(') "Missing public FLUIDOVR API: $api"
}

Assert-Contains $implementation 'sc\.Compare\("profile"\)\) ParseProfile' 'FLUIDOVR root does not parse profile definitions.'
Assert-Contains $implementation 'sc\.Compare\("material"\)\) ParseMaterial\(""\)' 'FLUIDOVR root does not parse global material rules.'
Assert-Contains $implementation 'sc\.Compare\("map"\)\) ParseMap' 'FLUIDOVR root does not parse map scopes.'
Assert-Contains $implementation 'ParseMaterial\(mapName\)' 'FLUIDOVR map scopes do not parse local material rules.'

foreach ($selector in @('tile', 'tilerange', 'tilefamily', 'texture', 'palette', 'surfaceclass')) {
    Assert-Contains $implementation ('sc\.Compare\("' + $selector + '"\)') "FLUIDOVR parser is missing selector: $selector"
}

foreach ($parameter in @(
    'bodycolor', 'undersidecolor', 'crestcolor', 'emissioncolor', 'emission', 'roughness',
    'undersideroughness', 'opacity', 'scale', 'amplitude', 'frequency', 'timescale', 'speed',
    'lacunarity', 'gain', 'flow', 'anisotropy', 'warp', 'choppiness', 'octaverotation',
    'choppyrelaxation', 'noisescale', 'maxslope', 'crestgain', 'normalstrength',
    'virtualheight', 'seed', 'octaves')) {
    Assert-Contains $implementation ('"' + $parameter + '"') "FLUIDOVR parser/serializer is missing parameter: $parameter"
}

$newFieldContracts = [ordered]@{
    'Lacunarity' = 'lacunarity'
    'Gain' = 'gain'
    'OctaveRotation' = 'octaveRotation'
    'ChoppyRelaxation' = 'choppyRelaxation'
    'NoiseScale' = 'noiseScale'
    'MaxSlope' = 'maxSlope'
    'CrestGain' = 'crestGain'
}
foreach ($entry in $newFieldContracts.GetEnumerator()) {
    $bit = 'FluidOverlayParameter_' + $entry.Key
    $member = $entry.Value
    Assert-Contains $header ([regex]::Escape($bit) + '\s*=\s*1ull\s*<<') `
        "Missing present-mask bit for $member."
    Assert-Contains $implementation ('if \(mask & ' + [regex]::Escape($bit) + `
        '\) destination\.' + [regex]::Escape($member) + '\s*=\s*source\.' + [regex]::Escape($member)) `
        "Resolved overrides do not apply $member."
    Assert-Contains $implementation ('value\.' + [regex]::Escape($member) + '\s*=\s*std::') `
        "Sanitization does not bound $member."
    Assert-Contains $implementation ('parameters\.' + [regex]::Escape($member)) `
        "Parser/serializer does not retain $member."
}
Assert-Contains $implementation 'std::clamp\(sc\.Number,\s*1,\s*4\)' `
    'FLUIDOVR octave authoring range must match the four-layer GPU contract.'

foreach ($style in @('still', 'waves', 'flow', 'fall', 'viscous', 'authored', 'lava')) {
    Assert-Contains $implementation ('"' + $style + '"') "FLUIDOVR is missing motion style: $style"
}

foreach ($profile in @(
    'water.calm', 'water.waves', 'water.flow', 'water.fall',
    'sludge.viscous', 'lava', 'authored.micro')) {
    Assert-Contains $implementation ([regex]::Escape('MakeBuiltInProfile("' + $profile + '"')) "Missing built-in fluid profile: $profile"
}

Assert-Contains $implementation 'MakeMaterialRuleKey\(rule\.mapName, rule\.id\)' 'Duplicate material identity is not scope-aware.'
Assert-Contains $implementation 'existing\s*=\s*rule' 'Duplicate material definitions do not replace the whole prior definition.'
Assert-Contains $implementation 'appendScope\(false\);\s*appendScope\(true\);' 'Resolved rule precedence must place all global rules before map-local rules.'
Assert-Contains $implementation 'rule\.resolvedDisable\s*=\s*source->hasDisable\s*&&\s*source->disable' 'Disable tombstones are not resolved explicitly.'
Assert-Contains $implementation 'references unresolved profile.*rule skipped' 'Unresolved profile diagnostics are missing.'
Assert-Contains $implementation 'for \(uint32_t ruleIndex.*resolved\.materialRules[\s\S]*?outMatch\.ruleIndex\s*=\s*ruleIndex' 'Material matching does not retain the last matching rule.'

Assert-Contains $implementation 'std::sort\(uniqueBindings[\s\S]*?a\.contentKey.*b\.contentKey[\s\S]*?ParametersLess' 'Stable binding ordering is not content-based.'
Assert-Contains $implementation 'bindingIndex\s*=\s*index' 'Resolved fluid bindings do not expose stable indices.'
Assert-Contains $implementation 'ComputeParameterContentKey' 'Resolved profiles and bindings do not expose content keys.'
Assert-Contains $implementation 'const auto orderedRules = SortByOrder\(database\.materialRules\)' 'Normalized serialization does not preserve material source order.'
Assert-Contains $implementation 'for \(const auto\* rule : orderedRules\)[\s\S]*?rule->mapName\.IsNotEmpty\(\)' 'Normalized serialization does not retain global material ordering.'
Assert-Contains $implementation 'for \(const auto& mapName : mapNames\)[\s\S]*?for \(const auto\* rule : orderedRules\)' 'Normalized serialization does not retain map-local material ordering.'

foreach ($command in @(
    'fluidoverlay_reload', 'fluidoverlay_dump', 'fluidoverlay_dumpresolved',
    'fluidoverlay_dumpnormalized')) {
    Assert-Contains $implementation ('CCMD\(' + $command + '\)') "Missing FLUIDOVR console command: $command"
}

Assert-Contains $cmake 'core/fluidoverlay\.cpp' 'FLUIDOVR owner is not registered in source/CMakeLists.txt.'
Assert-Contains $gameControl '#include\s+"fluidoverlay\.h"' 'FLUIDOVR startup integration is missing its header.'
Assert-Contains $gameControl 'ParseLightOverlays\(\);\s*ParseFluidOverlays\(\);' 'FLUIDOVR is not parsed beside LIGHTOVR at startup.'

Assert-Contains $releaseOverlay '(?m)^FLUIDOVR\s*$' 'Release overlay is missing its FLUIDOVR root block.'
$shippedProfiles = [ordered]@{
    'water.waves' = 'waves'
    'sludge.viscous' = 'viscous'
    'lava' = 'lava'
    'water.fall' = 'fall'
}
$profileDefinitions = [regex]::Matches(
    $releaseOverlay,
    '(?m)^\s*profile\s+"[^"\r\n]+"\s*\r?\n\s*\{').Count
if ($profileDefinitions -ne $shippedProfiles.Count) {
    throw "Release FLUIDOVR must define exactly $($shippedProfiles.Count) complete shipped profiles; found $profileDefinitions."
}
$completeProfileFields = @(
    'bodycolor', 'undersidecolor', 'crestcolor', 'emissioncolor', 'emission',
    'roughness', 'undersideroughness', 'opacity', 'scale', 'amplitude',
    'frequency', 'lacunarity', 'gain', 'timescale', 'speed', 'flow',
    'anisotropy', 'warp', 'choppiness', 'octaverotation', 'choppyrelaxation',
    'noisescale', 'maxslope', 'crestgain', 'normalstrength', 'virtualheight',
    'seed', 'octaves')
foreach ($entry in $shippedProfiles.GetEnumerator()) {
    $profileId = $entry.Key
    $body = Get-ProfileBlock $releaseOverlay $profileId
    Assert-Contains $body ('(?m)^\s*style\s+' + [regex]::Escape($entry.Value) + '\s*$') `
        "Profile '$profileId' must explicitly declare style '$($entry.Value)'."
    foreach ($field in $completeProfileFields) {
        $fieldCount = [regex]::Matches(
            $body,
            '(?m)^\s*' + [regex]::Escape($field) + '\s+').Count
        if ($fieldCount -ne 1) {
            throw "Profile '$profileId' must explicitly declare '$field' exactly once."
        }
    }
    $scale = Get-ProfileScalar $body 'scale' $profileId
    $amplitude = Get-ProfileScalar $body 'amplitude' $profileId
    $frequency = Get-ProfileScalar $body 'frequency' $profileId
    $lacunarity = Get-ProfileScalar $body 'lacunarity' $profileId
    $gain = Get-ProfileScalar $body 'gain' $profileId
    $choppiness = Get-ProfileScalar $body 'choppiness' $profileId
    $choppyRelaxation = Get-ProfileScalar $body 'choppyrelaxation' $profileId
    $noiseScale = Get-ProfileScalar $body 'noisescale' $profileId
    $maxSlope = Get-ProfileScalar $body 'maxslope' $profileId
    $crestGain = Get-ProfileScalar $body 'crestgain' $profileId
    $normalStrength = Get-ProfileScalar $body 'normalstrength' $profileId
    $virtualHeight = Get-ProfileScalar $body 'virtualheight' $profileId
    $octaves = Get-ProfileScalar $body 'octaves' $profileId
    if ([double]::IsNaN($scale) -or [double]::IsInfinity($scale) -or $scale -le 0 -or
        [double]::IsNaN($amplitude) -or [double]::IsInfinity($amplitude) -or $amplitude -le 0 -or
        [double]::IsNaN($frequency) -or [double]::IsInfinity($frequency) -or $frequency -le 0 -or
        [double]::IsNaN($lacunarity) -or [double]::IsInfinity($lacunarity) -or $lacunarity -lt 1.01 -or $lacunarity -gt 4 -or
        [double]::IsNaN($gain) -or [double]::IsInfinity($gain) -or $gain -lt 0 -or $gain -gt 1 -or
        [double]::IsNaN($choppiness) -or [double]::IsInfinity($choppiness) -or $choppiness -lt 0.05 -or $choppiness -gt 4 -or
        [double]::IsNaN($choppyRelaxation) -or [double]::IsInfinity($choppyRelaxation) -or $choppyRelaxation -lt 0 -or $choppyRelaxation -gt 1 -or
        [double]::IsNaN($noiseScale) -or [double]::IsInfinity($noiseScale) -or $noiseScale -lt 0.05 -or $noiseScale -gt 4 -or
        [double]::IsNaN($maxSlope) -or [double]::IsInfinity($maxSlope) -or $maxSlope -le 0 -or $maxSlope -gt 4 -or
        [double]::IsNaN($crestGain) -or [double]::IsInfinity($crestGain) -or $crestGain -lt 0 -or $crestGain -gt 4 -or
        [double]::IsNaN($normalStrength) -or [double]::IsInfinity($normalStrength) -or $normalStrength -le 0 -or
        [double]::IsNaN($virtualHeight) -or [double]::IsInfinity($virtualHeight) -or $virtualHeight -le 0 -or
        $octaves -lt 1 -or $octaves -gt 4 -or $octaves -ne [Math]::Floor($octaves)) {
        throw "Profile '$profileId' must use finite, positive spatial controls."
    }
    if (($gain * $lacunarity) -ge 1) {
        throw "Profile '$profileId' must reduce derivative energy across octaves (gain*lacunarity < 1)."
    }
    $baseSlope = $amplitude * $frequency * $normalStrength / $scale
    if ($baseSlope -lt 0.05) {
        throw "Profile '$profileId' base slope $baseSlope is below the visible world-scale floor."
    }
}
foreach ($binding in @(
    'tilefamily\s+336[\s\S]*?profile\s+"water\.waves"',
    'tilefamily\s+200[\s\S]*?profile\s+"sludge\.viscous"',
    'tilefamily\s+1082[\s\S]*?profile\s+"lava"',
    'tilefamily\s+889[\s\S]*?profile\s+"water\.fall"')) {
    Assert-Contains $releaseOverlay $binding "Release overlay is missing expected fluid binding: $binding"
}

Write-Host 'FLUIDOVR schema static validation passed.'
