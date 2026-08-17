Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$sceneUpload = Get-Content -LiteralPath (Join-Path $repoRoot 'source/common/rendering/nri/renderer/nri_scene_upload.cpp') -Raw
$upscaler = Get-Content -LiteralPath (Join-Path $repoRoot 'source/common/rendering/nri/renderer/nri_upscaler.cpp') -Raw
$frameBuild = Get-Content -LiteralPath (Join-Path $repoRoot 'source/common/rendering/nri/renderer/nri_scene_frame_build.cpp') -Raw
$dispatch = Get-Content -LiteralPath (Join-Path $repoRoot 'source/common/rendering/nri/renderer/nri_pass_dispatch.cpp') -Raw
$descriptorSets = Get-Content -LiteralPath (Join-Path $repoRoot 'source/common/rendering/nri/renderer/nri_descriptor_sets.cpp') -Raw
$renderer = Get-Content -LiteralPath (Join-Path $repoRoot 'source/common/rendering/nri/renderer/nri_renderer.cpp') -Raw
$persistentVoxels = Get-Content -LiteralPath (Join-Path $repoRoot 'source/common/rendering/nri/renderer/nri_persistent_voxels.cpp') -Raw
$persistentVoxelsHeader = Get-Content -LiteralPath (Join-Path $repoRoot 'source/common/rendering/nri/renderer/nri_persistent_voxels.h') -Raw

$snapshotStart = $sceneUpload.IndexOf('const auto acquireSceneDataDescriptorSnapshot', [StringComparison]::Ordinal)
$snapshotEnd = $sceneUpload.IndexOf('if (sceneInstances.empty())', $snapshotStart, [StringComparison]::Ordinal)
if ($snapshotStart -lt 0 -or $snapshotEnd -lt 0) {
	throw 'could not isolate scene-data snapshot acquisition'
}
$snapshot = $sceneUpload.Substring($snapshotStart, $snapshotEnd - $snapshotStart)
foreach ($required in @(
	'GetRecordingCommandFenceValue()',
	'if (recordingFenceValue == 0)',
	'IsCommandFenceValueComplete(snapshot.retireFenceValue)',
	'WaitForCommandsTracked("scene_data_snapshot_reuse")',
	'snapshot.retireFenceValue = recordingFenceValue;'
)) {
	if (-not $snapshot.Contains($required)) {
		throw "scene-data snapshots must use command-fence lifetime (missing '$required')"
	}
}

$unloadStart = $renderer.IndexOf('void NRIRenderer::OnLevelUnloadBegin', [StringComparison]::Ordinal)
$unloadEnd = $renderer.IndexOf('void NRIRenderer::OnLevelUnloadComplete', $unloadStart, [StringComparison]::Ordinal)
$loadStart = $renderer.IndexOf('void NRIRenderer::OnLevelLoadBegin', $unloadEnd, [StringComparison]::Ordinal)
$loadEnd = $renderer.IndexOf('void NRIRenderer::OnLevelFirstFrameRelease', $loadStart, [StringComparison]::Ordinal)
if ($unloadStart -lt 0 -or $unloadEnd -lt 0 -or $loadStart -lt 0 -or $loadEnd -lt 0) {
	throw 'could not isolate renderer level-transition lifecycle'
}
$unload = $renderer.Substring($unloadStart, $unloadEnd - $unloadStart)
$load = $renderer.Substring($loadStart, $loadEnd - $loadStart)
foreach ($lifecycle in @($unload, $load)) {
	$actorReset = $lifecycle.IndexOf('ResetPersistentVoxelActorCache(', [StringComparison]::Ordinal)
	$schedulingReset = $lifecycle.IndexOf('ResetLevelSchedulingState(', [StringComparison]::Ordinal)
	$mapIdentityGuard = $lifecycle.IndexOf('if (info.oldLevel != info.newLevel)', [StringComparison]::Ordinal)
	if ($actorReset -lt 0 -or $schedulingReset -lt 0) {
		throw 'every real level lifetime must reset voxel actors and scheduling, including a same-name reload'
	}
	if ($mapIdentityGuard -ge 0 -and ($actorReset -gt $mapIdentityGuard -or $schedulingReset -gt $mapIdentityGuard)) {
		throw 'same-name level reloads must not retain old voxel actor or scheduling occurrences'
	}
}
if (-not $persistentVoxelsHeader.Contains('uint64_t materialBridgeBuildSerial = 0;') -or
	-not $persistentVoxels.Contains('existingIt->second.materialBridgeBuildSerial == candidate.materialBridgeBuildSerial') -or
	-not $persistentVoxels.Contains('materialResource.materialBridgeBuildSerial == residencyLastBuildSerial') -or
	-not $persistentVoxels.Contains('existingIt->second.materialBridgeBuildSerial == buildSerial')) {
	throw 'retained voxel material bridges must be refreshed when the map build lifetime changes'
}
if ($snapshot.Contains('renderer.mFrameIndex + 1u') -or $snapshot.Contains('IsFrameFenceValueComplete')) {
	throw 'scene-data snapshot lifetime must not mix renderer frame identity with GPU fence identity'
}

$ensureStart = $upscaler.IndexOf('bool NRIUpscalerContext::EnsureUpscaler', [StringComparison]::Ordinal)
$ensureEnd = $upscaler.IndexOf('bool NRIUpscalerContext::EnsureMainUpscaler', $ensureStart, [StringComparison]::Ordinal)
if ($ensureStart -lt 0 -or $ensureEnd -lt 0) {
	throw 'could not isolate upscaler replacement'
}
$ensure = $upscaler.Substring($ensureStart, $ensureEnd - $ensureStart)
$waitIndex = $ensure.IndexOf('frameBuffer.SubmitWaitAndRestartCommandList("upscaler-recreate")', [StringComparison]::Ordinal)
$destroyIndex = $ensure.IndexOf('DestroyUpscaler(frameBuffer, slot.instance);', [StringComparison]::Ordinal)
if ($waitIndex -lt 0 -or $destroyIndex -le $waitIndex) {
	throw 'upscaler replacement must submit the open list, drain consumers, and restart recording before destroying the provider instance'
}

foreach ($required in @(
	'PT current queued-frame scene bindings became incomplete after scene construction; skipping TraceOpaque.',
	'if (!currentTraceBindingsReady())'
)) {
	if (-not $frameBuild.Contains($required)) {
		throw "queued-frame scene binding validation contract is incomplete (missing '$required')"
	}
}
$selectionStart = $frameBuild.IndexOf('const bool staticMapSceneReady', [StringComparison]::Ordinal)
$selectionEnd = $frameBuild.IndexOf('bool residentStaticWorldGeometryChanged', $selectionStart, [StringComparison]::Ordinal)
if ($selectionStart -lt 0 -or $selectionEnd -lt 0) {
	throw 'could not isolate static scene path selection'
}
$earlySelection = $frameBuild.Substring($selectionStart, $selectionEnd - $selectionStart)
if ($earlySelection.Contains('RestoreStaticTopLevelScene()') -or $earlySelection.Contains('UpdateSceneDataSet(')) {
	throw 'scene-data publication must remain at the normal post-assembly point where all dependent payloads are ready'
}

foreach ($required in @(
	'mActiveSceneDataSnapshot->descriptorsInitialized',
	'mActiveSceneDataSnapshot->publishedMapEpoch == currentMapEpoch',
	'mSceneDataDescriptorMapEpochs[queuedFrameIndex] == currentMapEpoch',
	'mSceneDataDescriptorBuildEpochs[queuedFrameIndex] == currentBuildEpoch'
)) {
	if (-not $descriptorSets.Contains($required)) {
		throw "scene-data publication must be owner- and epoch-aware (missing '$required')"
	}
}

foreach ($required in @(
	'DestroyWorldTlasFrameSlots();',
	'mActiveSceneDataSnapshot = nullptr;',
	'mSceneDataDescriptorMapEpochs.begin()',
	'mSceneDataDescriptorBuildEpochs.begin()'
)) {
	if (-not $renderer.Contains($required)) {
		throw "level unload must invalidate old scene/TLAS publication (missing '$required')"
	}
}

$traceStart = $dispatch.IndexOf('bool NRIPassDispatcher::DispatchTraceOpaque', [StringComparison]::Ordinal)
$traceEnd = $dispatch.IndexOf('bool NRIPassDispatcher::DispatchDenoiser', $traceStart, [StringComparison]::Ordinal)
if ($traceStart -lt 0 -or $traceEnd -lt 0) {
	throw 'could not isolate TraceOpaque dispatch'
}
$trace = $dispatch.Substring($traceStart, $traceEnd - $traceStart)
if (-not $trace.Contains('if (!context.mSceneBinding.BindSceneRootDescriptors())')) {
	throw 'TraceOpaque must not dispatch without a valid queued-frame TLAS root binding'
}
$guardCount = ([regex]::Matches($dispatch, [regex]::Escape('if (!context.mSceneBinding.BindSceneRootDescriptors())'))).Count
if ($guardCount -lt 5) {
	throw "all scene-root consumers must fail closed (found $guardCount guarded dispatches)"
}

if (-not $frameBuild.Contains('mPersistentVoxels.IsIndirectOnlyActorTlasAppendEligible(')) {
	throw 'local-player reflection handoff must use exact resident-actor TLAS eligibility'
}
if (-not $frameBuild.Contains('mPersistentVoxels.HasOverlayPreparationEligibleActor(')) {
	throw 'persistent voxel render admission must allow resource preparation before final TLAS eligibility'
}
$renderAdmissionStart = $frameBuild.IndexOf('overlayEligibilityInputs.persistentVoxelRenderable =', [StringComparison]::Ordinal)
$renderAdmissionEnd = $frameBuild.IndexOf('overlayEligibilityInputs.activeDynamicGeometry =', $renderAdmissionStart, [StringComparison]::Ordinal)
if ($renderAdmissionStart -lt 0 -or $renderAdmissionEnd -lt 0) {
	throw 'could not isolate persistent voxel render admission'
}
$renderAdmission = $frameBuild.Substring($renderAdmissionStart, $renderAdmissionEnd - $renderAdmissionStart)
if ($renderAdmission.Contains('HasTlasAppendEligibleActor(')) {
	throw 'persistent voxel render admission must not require the BLAS and arena views that its branch produces'
}
$exactEligibilityStart = $persistentVoxels.IndexOf('bool NRIPersistentVoxelResidency::IsActorTlasAppendEligible(', [StringComparison]::Ordinal)
$preparationEligibilityStart = $persistentVoxels.IndexOf('bool NRIPersistentVoxelResidency::IsActorOverlayPreparationEligible(', $exactEligibilityStart, [StringComparison]::Ordinal)
$eligibilityEnd = $persistentVoxels.IndexOf('bool NRIPersistentVoxelResidency::HasPreloadPending()', $preparationEligibilityStart, [StringComparison]::Ordinal)
if ($exactEligibilityStart -lt 0 -or $preparationEligibilityStart -lt 0 -or $eligibilityEnd -lt 0) {
	throw 'could not isolate persistent voxel TLAS append eligibility'
}
$exactEligibility = $persistentVoxels.Substring($exactEligibilityStart, $preparationEligibilityStart - $exactEligibilityStart)
$preparationEligibility = $persistentVoxels.Substring($preparationEligibilityStart, $eligibilityEnd - $preparationEligibilityStart)
foreach ($required in @(
	'mesh.accelerationStructure.accelerationStructure == nullptr',
	'materialBuffer.shaderView == nullptr',
	'(mesh.tlasPublished || mesh.tlasReadyFrame <= frameIndex)',
	'services.GetAccelerationStructureHandle(mesh.accelerationStructure) != 0'
)) {
	if (-not $exactEligibility.Contains($required)) {
		throw "final persistent voxel TLAS eligibility is missing an append gate ('$required')"
	}
}
foreach ($required in @(
	'PersistentVoxelMaterialRangeMatches(actor, material)',
	'primitiveRangeValid',
	'publishedMaterialRangeValid',
	'meshRangeMatches'
)) {
	if (-not $preparationEligibility.Contains($required)) {
		throw "persistent voxel overlay preparation is missing a safe preflight gate ('$required')"
	}
}
foreach ($forbidden in @(
	'mesh.accelerationStructure.accelerationStructure',
	'materialBuffer.shaderView',
	'GetAccelerationStructureHandle'
)) {
	if ($preparationEligibility.Contains($forbidden)) {
		throw "persistent voxel overlay preparation must not depend on a resource produced inside the overlay branch ('$forbidden')"
	}
}

$materialBridgeStart = $persistentVoxels.IndexOf('void NRIPersistentVoxelResidency::RebuildBatchMaterialBridge', [StringComparison]::Ordinal)
$materialBridgeEnd = $persistentVoxels.IndexOf('void NRIPersistentVoxelResidency::RecomputeBatchState', $materialBridgeStart, [StringComparison]::Ordinal)
$materialUploadStart = $persistentVoxels.IndexOf('bool NRIPersistentVoxelResidency::UploadArenaMaterialBuffers', [StringComparison]::Ordinal)
$materialUploadEnd = $persistentVoxels.IndexOf('bool NRIPersistentVoxelResidency::AppendTlasInstances', $materialUploadStart, [StringComparison]::Ordinal)
if ($materialBridgeStart -lt 0 -or $materialBridgeEnd -lt 0 -or $materialUploadStart -lt 0 -or $materialUploadEnd -lt 0) {
	throw 'could not isolate persistent voxel material publication policy'
}
$materialBridgePolicy = $persistentVoxels.Substring($materialBridgeStart, $materialBridgeEnd - $materialBridgeStart)
$materialUploadPolicy = $persistentVoxels.Substring($materialUploadStart, $materialUploadEnd - $materialUploadStart)
if (-not $persistentVoxels.Contains('CollectActivePersistentVoxelMaterialKeys(const PersistentVoxelBatch& batch)') -or
	-not $materialBridgePolicy.Contains('CollectActivePersistentVoxelMaterialKeys(targetBatch)') -or
	$materialBridgePolicy.Contains('materialResources.reserve(materialVariantResources.size())')) {
	throw 'the frame material bridge must include active actor materials rather than all session-resident materials'
}
if (-not $materialUploadPolicy.Contains('activeMaterialKeys.find(*dirtyIt) == activeMaterialKeys.end()') -or
	-not $materialUploadPolicy.Contains('dirtyMaterialResourceKeys.insert(activeMaterialKeys.begin(), activeMaterialKeys.end())')) {
	throw 'material upload dirtiness must be scoped to currently active actor materials'
}
if (-not $persistentVoxels.Contains('previousActiveMaterialKeys != currentActiveMaterialKeys') -or
	-not $persistentVoxels.Contains('batchMaterialPublicationGeneration++')) {
	throw 'active persistent voxel material-set changes must rebuild and version the published bridge'
}
if (-not $frameBuild.Contains('mPersistentVoxels.MaterialPublicationGeneration()')) {
	throw 'the scene material frame cache must be keyed by persistent material publication rather than resource residency'
}
if (-not $persistentVoxels.Contains('uploadedMaterialPublicationGeneration != batchMaterialPublicationGeneration') -or
	-not $persistentVoxels.Contains('uploadedMaterialPublicationGeneration = batchMaterialPublicationGeneration')) {
	throw 'reactivated persistent voxel materials must be validated and uploaded for the current publication'
}
if (-not $frameBuild.Contains('mSceneMaterialFrameCache.PersistentMaterialCount() != persistentVoxelMaterialCount') -or
	-not $frameBuild.Contains('PT persistent voxel material publication count did not match the frame cache.')) {
	throw 'persistent material slicing must fail closed when the frame cache does not match the active publication'
}
$validatedUploadCall = [regex]::Escape('UploadPersistentVoxelArenaMaterialBuffers(persistentVoxelGpuMaterials, true)')
if ([regex]::Matches($frameBuild, $validatedUploadCall).Count -lt 2) {
	throw 'primary and post-light persistent material uploads must validate active rows after texture-slot or proxy changes'
}

Write-Host 'NRI transition scene-binding lifetime tests passed.'
