Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$residencyPath = Join-Path $repoRoot 'source/common/rendering/nri/renderer/nri_persistent_voxels.cpp'
$residency = Get-Content -LiteralPath $residencyPath -Raw

$start = $residency.IndexOf('bool NRIPersistentVoxelResidency::UploadArenaMaterialBuffers', [StringComparison]::Ordinal)
$end = $residency.IndexOf('bool NRIPersistentVoxelResidency::AppendTlasInstances', $start, [StringComparison]::Ordinal)
if ($start -lt 0 -or $end -lt 0) {
	throw 'could not isolate UploadArenaMaterialBuffers'
}
$upload = $residency.Substring($start, $end - $start)

$prepare = $upload.IndexOf('materialUploadMirror.Prepare', [StringComparison]::Ordinal)
$stage = $upload.IndexOf('services.StageMaterialRanges', [StringComparison]::Ordinal)
$rollback = $upload.IndexOf('materialUploadMirror.Rollback', [StringComparison]::Ordinal)
$commit = $upload.IndexOf('materialUploadMirror.Commit', [StringComparison]::Ordinal)
$hashCommit = $upload.IndexOf('upload.resource->materialUploadHash = upload.materialHash', [StringComparison]::Ordinal)
if ($prepare -lt 0 -or $stage -le $prepare -or $rollback -le $stage -or
	$commit -le $rollback -or $hashCommit -le $commit) {
	throw 'material mirror prepare/stage/rollback/commit/hash publication order is invalid'
}
if ($upload -notmatch 'StageMaterialRanges\s*\(\s*materialBuffer,\s*coalescedRanges,\s*materialUploadMirror\.Data\(\),\s*materialUploadMirror\.Size\(\)') {
	throw 'persistent voxel material ranges must stage from the authoritative upload mirror'
}

foreach ($functionName in @(
	'void NRIPersistentVoxelResidency::DestroyArenaBuffers',
	'void NRIPersistentVoxelResidency::Reset',
	'bool NRIPersistentVoxelResidency::CompactMaterialRangesForQuiescentLevelTransition')) {
	$functionStart = $residency.IndexOf($functionName, [StringComparison]::Ordinal)
	if ($functionStart -lt 0) { throw "could not find $functionName" }
	$nextFunction = $residency.IndexOf("`nbool NRIPersistentVoxelResidency::", $functionStart + $functionName.Length, [StringComparison]::Ordinal)
	$nextVoidFunction = $residency.IndexOf("`nvoid NRIPersistentVoxelResidency::", $functionStart + $functionName.Length, [StringComparison]::Ordinal)
	$functionEnd = @($nextFunction, $nextVoidFunction) | Where-Object { $_ -gt $functionStart } | Measure-Object -Minimum | Select-Object -ExpandProperty Minimum
	if (-not $functionEnd) { $functionEnd = $residency.Length }
	$functionBody = $residency.Substring($functionStart, $functionEnd - $functionStart)
	if (-not $functionBody.Contains('materialUploadMirror.Reset()')) {
		throw "$functionName must invalidate mirrored material arena authority"
	}
}

Write-Host 'Persistent voxel material upload integration tests passed.'
