$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$sourcePath = Join-Path $repoRoot 'source\common\rendering\nri\renderer\nri_scene_textures.cpp'
$headerPath = Join-Path $repoRoot 'source\common\rendering\nri\renderer\nri_scene_textures.h'
$source = Get-Content -LiteralPath $sourcePath -Raw
$header = Get-Content -LiteralPath $headerPath -Raw

function Require-Contains {
	param(
		[Parameter(Mandatory = $true)][string]$Text,
		[Parameter(Mandatory = $true)][string]$Needle,
		[Parameter(Mandatory = $true)][string]$Failure
	)
	if (-not $Text.Contains($Needle)) {
		throw $Failure
	}
}

$functionStart = $source.IndexOf(
	'bool NRISceneTextureResidency::EnsurePaletteTexture(',
	[StringComparison]::Ordinal)
$functionEnd = $source.IndexOf(
	'bool NRISceneTextureResidency::EnsureCacheEntry(',
	$functionStart,
	[StringComparison]::Ordinal)
if ($functionStart -lt 0 -or $functionEnd -le $functionStart) {
	throw 'Could not isolate NRISceneTextureResidency::EnsurePaletteTexture.'
}
$ensurePalette = $source.Substring($functionStart, $functionEnd - $functionStart)

Require-Contains $header `
	'uint64_t mPalettePayloadSignature = 0;' `
	'Palette residency does not retain the accepted payload signature.'
Require-Contains $ensurePalette `
	'uint64_t payloadSignature = materials.paletteLookup.signature();' `
	'Palette residency does not derive identity from the immutable palette payload.'
Require-Contains $ensurePalette `
	'payloadSignature = nri_scene::Fnv1a64(materials.paletteLookup.data(), materials.paletteLookup.size());' `
	'Palette residency has no deterministic fallback when the stored payload signature is zero.'

$sameWidth = $ensurePalette.IndexOf(
	'mPaletteTexture.width == materials.paletteWidth',
	[StringComparison]::Ordinal)
$sameHeight = $ensurePalette.IndexOf(
	'mPaletteTexture.height == materials.paletteHeight',
	[StringComparison]::Ordinal)
$samePayload = $ensurePalette.IndexOf(
	'mPalettePayloadSignature == payloadSignature',
	[StringComparison]::Ordinal)
$fastReturn = $ensurePalette.IndexOf('return true;', $samePayload, [StringComparison]::Ordinal)
if ($sameWidth -lt 0 -or $sameHeight -lt 0 -or $samePayload -lt 0 -or
	$sameWidth -gt $samePayload -or $sameHeight -gt $samePayload -or $fastReturn -lt $samePayload) {
	throw 'Palette residency may reuse a same-sized texture without matching its payload signature.'
}

$destroy = $ensurePalette.IndexOf(
	'device.DestroyTextureResource(mPaletteTexture);',
	$fastReturn,
	[StringComparison]::Ordinal)
$clearAcceptedSignature = $ensurePalette.IndexOf(
	'mPalettePayloadSignature = 0;',
	$destroy,
	[StringComparison]::Ordinal)
$create = $ensurePalette.IndexOf(
	'device.CreateOwnedTexture(',
	$destroy,
	[StringComparison]::Ordinal)
if ($destroy -lt 0 -or $clearAcceptedSignature -lt $destroy -or $create -lt $clearAcceptedSignature) {
	throw 'Palette recreation does not invalidate the accepted payload signature before resource creation.'
}

$upload = $ensurePalette.IndexOf(
	'if (!device.UploadTextureData(',
	$create,
	[StringComparison]::Ordinal)
$uploadFailure = $ensurePalette.IndexOf(
	'return false;',
	$upload,
	[StringComparison]::Ordinal)
$commitAcceptedSignature = $ensurePalette.IndexOf(
	'mPalettePayloadSignature = payloadSignature;',
	$upload,
	[StringComparison]::Ordinal)
if ($upload -lt 0 -or $uploadFailure -lt $upload -or
	$commitAcceptedSignature -lt $uploadFailure) {
	throw 'Palette payload identity is committed before texture upload succeeds.'
}

$signatureAssignments = [regex]::Matches(
	$ensurePalette,
	'mPalettePayloadSignature\s*=\s*payloadSignature\s*;')
if ($signatureAssignments.Count -ne 1) {
	throw "Palette residency must have one success-only signature commit; found $($signatureAssignments.Count)."
}

Write-Host 'NRI palette residency payload contract tests passed.'
