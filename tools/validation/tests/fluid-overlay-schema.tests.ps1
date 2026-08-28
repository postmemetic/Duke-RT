$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
$header = Get-Content (Join-Path $root 'source\core\fluidoverlay.h') -Raw
$implementation = Get-Content (Join-Path $root 'source\core\fluidoverlay.cpp') -Raw
$cmake = Get-Content (Join-Path $root 'source\CMakeLists.txt') -Raw
$gameControl = Get-Content (Join-Path $root 'source\core\gamecontrol.cpp') -Raw

function Assert-Contains([string]$Text, [string]$Pattern, [string]$Message) {
    if ($Text -notmatch $Pattern) { throw $Message }
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
    'flow', 'anisotropy', 'warp', 'choppiness', 'normalstrength', 'virtualheight', 'seed', 'octaves')) {
    Assert-Contains $implementation ('"' + $parameter + '"') "FLUIDOVR parser/serializer is missing parameter: $parameter"
}

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

Write-Host 'FLUIDOVR schema static validation passed.'
