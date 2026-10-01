Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
function Read-Source([string]$Path) { Get-Content -LiteralPath (Join-Path $root $Path) -Raw }
function Require([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
$smoke = Read-Source 'source/common/rendering/nri/renderer/nri_smoke.cpp'
$transientRenderer = Read-Source 'source/common/rendering/nri/renderer/nri_smoke_transient_renderer.cpp'
$smokeHeader = Read-Source 'source/common/rendering/nri/renderer/nri_smoke.h'
$smokeGrid = Read-Source 'source/common/rendering/nri/renderer/nri_smoke_grid.cpp'
$descriptorBudget = Read-Source 'source/common/rendering/nri/renderer/nri_smoke_descriptor_budget.h'
$sharedPoolBudget = Read-Source 'source/common/rendering/nri/renderer/nri_static_tangent_capacity.h'
$renderer = Read-Source 'source/common/rendering/nri/renderer/nri_renderer.cpp'
$rendererHeader = Read-Source 'source/common/rendering/nri/renderer/nri_renderer.h'
$renderDevice = Read-Source 'source/common/rendering/nri/system/nri_renderdevice.cpp'
$descriptorSets = Read-Source 'source/common/rendering/nri/renderer/nri_descriptor_sets.cpp'
$dispatchContext = Read-Source 'source/common/rendering/nri/renderer/nri_pass_dispatch_context.cpp'
$pipelineState = Read-Source 'source/common/rendering/nri/renderer/nri_pipeline_state.cpp'
$shaderContracts = Read-Source 'source/common/rendering/nri/renderer/nri_shader_contracts.h'
$transientResourcesHeader = Read-Source 'source/common/rendering/nri/renderer/nri_smoke_transient_resources.h'
$transientResources = Read-Source 'source/common/rendering/nri/renderer/nri_smoke_transient_resources.cpp'
$contracts = Read-Source 'source/common/rendering/nri/renderer/nri_smoke_contracts.h'
$constants = Read-Source 'source/common/rendering/nri/shaders/Include/SmokeConstants.hlsli'
$resources = Read-Source 'source/common/rendering/nri/shaders/Include/SmokeResources.hlsli'
$transient = Read-Source 'source/common/rendering/nri/shaders/Include/SmokeTransientData.hlsli'
$cmake = Read-Source 'source/CMakeLists.txt'
Require ($smoke -match 'representationEffective == 0u && !mMayHaveParticleSmoke &&\s*mAnalyticCarriers\.GetGpuCarriers\(\)\.empty\(\) &&\s*mTransientClouds\.GetSnapshot\(\)\.visibleGroups == 0u') 'The particle-empty route must not skip independent visible analytic or transient clouds.'
Require ($dispatchContext -match 'SmokeService::GetVolumeSlot\(bool metadata\) const\s*\{\s*const uint32_t slot = getVolumeSlot != nullptr \? getVolumeSlot\(user, metadata\) : UINT32_MAX;[\s\S]*?return slot < \(uint32_t\)FrameTextureSlot::Count \? \(FrameTextureSlot\)slot : FrameTextureSlot::Count;') 'Smoke output must canonicalize UINT32_MAX and every out-of-range slot to Count before texture consumers index it.'
Require ($smoke -match 'kSmokeTransientStorageBase == 61u') 'Transient UAVs must not replace live dormant storage u54..60.'
Require ($smoke -match 'input.descriptorNum = nri_smoke_descriptors::InputCount;') 'Keep the input layout synchronized with the shared smoke descriptor budget.'
Require ($resources -match 'gSmokeAnalyticCarriers : register\(t2, space0\)') 'Legacy carrier ABI binding changed.'
foreach ($slot in 61..66) { Require ($transient.Contains("register(u$slot, space1)")) "Missing transient UAV u$slot." }
Require ($contracts -match 'sizeof\(NRISmokeConstants\) == 216') 'CPU root contract must retain the 54-DWORD ABI.'
Require ($smokeHeader -match 'PipelineDescriptorRangeCount\s*=\s*10u') 'Smoke root accounting must include all ten descriptor ranges.'
Require ((216 / 4 + 10) -le 64) 'Root constants plus ten descriptor ranges exceed the D3D12 root budget.'
Require ($smokeHeader -match 'PipelineDescriptorSetCount\s*=\s*6u') 'The smoke pipeline must publish its six-set device requirement.'
Require ($smokeHeader -match 'static_assert\(D3D12RootDwordCount\s*<=\s*64u') 'The compiled smoke ABI must guard the D3D12 root DWORD ceiling.'
Require ($smoke -match 'layout\.descriptorSetNum\s*=\s*PipelineDescriptorSetCount') 'Smoke layout creation must use the published descriptor-set count.'
Require ($smoke -match 'for \(auto& range : filteredSceneRanges\)\s*range.flags = NRIResourceFlags\(range.flags, nri::DescriptorRangeBits::PARTIALLY_BOUND\)') 'The independently readiness-gated scene/TLAS ranges must permit unused unbound descriptors on Vulkan.'
Require (($renderer | Select-String -Pattern 'descriptorSetMaxNum\s*<\s*NRISmokeSystem::PipelineDescriptorSetCount' -AllMatches).Matches.Count -eq 2) 'Both renderer availability paths must require all six smoke descriptor sets.'

# The shared pool fills before smoke initializes: three queued scene sets plus
# twelve scene snapshots consume 465 SRVs and four voxel-compute input sets
# consume another 16. Reserve every lazily allocated smoke SRV, including the
# grid input sets, instead of accounting for only the two newest t3/t4 inputs.
foreach ($contract in @(
    @('InputCount', '6u'), @('LightCount', '3u'),
    @('FilteredSceneCount', '8u'), @('ExtendedSceneCount', '10u'),
    @('GridInputCount', '2u'))) {
    Require ($descriptorBudget -match (('{0}\s*=\s*{1}' -f $contract[0], $contract[1]))) "Smoke descriptor budget must publish $($contract[0])=$($contract[1])."
}
Require ($descriptorBudget -match 'StructuredPerQueuedFrame\s*=\s*InputCount\s*\+\s*LightCount\s*\+[\s\S]{0,120}FilteredSceneCount\s*\+\s*ExtendedSceneCount\s*\+\s*GridInputCount') 'Smoke descriptor budget must include every structured-buffer range.'
Require ($descriptorBudget -match 'return\s+512u\s*\+\s*StructuredPerQueuedFrame\s*\*\s*queuedFrames') 'The established shared reserve must remain in addition to the complete smoke reservation.'
Require ($renderDevice -match 'structuredBufferMaxNum\s*=\s*NRIStaticTangentStructuredPoolCapacity\(NRIFrameShell::QueuedFrameCount\)') 'The device pool must consume the combined scene/smoke descriptor budget.'
Require ($sharedPoolBudget -match 'nri_smoke_descriptors::StructuredPerQueuedFrame\s*\*\s*queuedFrames') 'The complete scene pool census must use the current smoke layouts.'
Require ($sharedPoolBudget -match 'smokeReserve\s*=\s*nri_smoke_descriptors::SharedStructuredPoolCapacity\(queuedFrames\)' -and $sharedPoolBudget -match 'return known \+ 32u > smokeReserve \? known \+ 32u : smokeReserve') 'The combined pool must cover both expanded scene snapshots and the established smoke reserve.'
Require ($descriptorBudget -match 'TransientStorageCount\s*=\s*7u') 'The motion guide adds a seventh transient UAV.'
Require ($transientResourcesHeader -match 'StorageDescriptorCount\s*=\s*nri_smoke_descriptors::TransientStorageCount') 'Transient storage layout and device-pool reserve must share their count.'
Require ($renderDevice -match 'storageStructuredBufferMaxNum\s*=\s*nri_smoke_descriptors::SharedStoragePoolCapacity\(QueuedFrameCount\)') 'The device pool must reserve the motion UAV per queued frame.'
Require ($smoke -match 'input\.descriptorNum\s*=\s*nri_smoke_descriptors::InputCount') 'The main smoke input range must consume the shared input count.'
Require ($smoke -match 'lights\.descriptorNum\s*=\s*nri_smoke_descriptors::LightCount') 'The main smoke light range must consume the shared light count.'
Require ($smoke -match 'kSmokeFilteredSceneBufferCount\s*=\s*nri_smoke_descriptors::FilteredSceneCount') 'The filtered-scene layout must consume the shared filtered count.'
Require ($smoke -match 'kSmokeExtendedSceneBufferCount\s*=\s*nri_smoke_descriptors::ExtendedSceneCount') 'The extended-scene layout must consume the shared extended count.'
Require ($smokeGrid -match 'inputRange\.descriptorNum\s*=\s*nri_smoke_descriptors::GridInputCount') 'The grid input layout must consume the shared grid count.'
Require ($shaderContracts -match 'NRI_SCENE_DATA_DESCRIPTOR_NUM\s*=\s*(\d+)') 'Missing scene-data descriptor count.'
$sceneDataDescriptorCount = [int]$Matches[1]
Require ($sceneDataDescriptorCount -eq 31) 'The decal header and records extend the scene-data range to 31 SRVs.'
Require ($sharedPoolBudget -match 'known\s*=\s*NRI_SCENE_DATA_DESCRIPTOR_NUM\s*\*') 'The pool census must use the live scene-data descriptor count.'
Require ($descriptorSets -match 'sceneDataSnapshotCount\s*=\s*std::max\(8u,\s*queuedFrameCount\s*\*\s*4u\)') 'The pool audit must track the live scene-data snapshot formula.'
Require ($rendererHeader -match 'std::array<nri::DescriptorSet\*,\s*4>\s+mVoxelComputeInputSets') 'The pool audit must track all four voxel-compute input sets.'
Require ($shaderContracts -match 'NRI_VOXEL_COMPUTE_INPUT_DESCRIPTOR_NUM\s*=\s*2') 'Voxel-compute input SRV count changed without updating the pool audit.'
Require ($shaderContracts -match 'NRI_VOXEL_COMPUTE_FACE_DESCRIPTOR_NUM\s*=\s*2') 'Voxel-compute face SRV count changed without updating the pool audit.'
Require ($pipelineState -match 'inputRange\.descriptorNum\s*=\s*NRI_VOXEL_COMPUTE_INPUT_DESCRIPTOR_NUM') 'Voxel-compute input layout must consume its published descriptor count.'
Require ($pipelineState -match 'faceRange\.descriptorNum\s*=\s*NRI_VOXEL_COMPUTE_FACE_DESCRIPTOR_NUM') 'Voxel-compute face layout must consume its published descriptor count.'
$queuedFrames = 3
$sceneDataSets = $queuedFrames + [Math]::Max(8, $queuedFrames * 4)
$liveBeforeSmoke = $sceneDataSets * $sceneDataDescriptorCount + 4 * (2 + 2)
$historicalLiveWithSmoke = $sceneDataSets * 29 + 4 * (2 + 2) + $queuedFrames * (5 + 3 + 8 + 10 + 2)
$liveWithSmoke = $liveBeforeSmoke + $queuedFrames * (6 + 3 + 8 + 10 + 2)
$oldDeltaCapacity = 512 + 2 * $queuedFrames
$reservedCapacity = [Math]::Max(512 + 29 * $queuedFrames, $liveWithSmoke + 6 + 32)
Require ($liveBeforeSmoke -eq 481) 'Unexpected pre-smoke structured descriptor total for three queued frames.'
Require ($historicalLiveWithSmoke -eq 535 -and $liveWithSmoke -eq 568) 'The scene decal ranges and previous-lobe tracking must be included in the live total.'
Require ($oldDeltaCapacity -eq 518 -and $historicalLiveWithSmoke -gt $oldDeltaCapacity) 'The regression proof must preserve the observed 520-over-518 failure.'
Require ($reservedCapacity -eq 606 -and $reservedCapacity -ge $liveWithSmoke) 'The combined scene/smoke reservation must provide 606 structured descriptors for three queued frames.'
foreach ($name in @('TransientGroupCount', 'TransientLobeCount', 'TransientFullBuildBudget', 'TransientPointBudget')) {
    Require (-not $constants.Contains($name)) "Transient-only root field $name widened the shared ABI."
}
foreach ($mapping in @(
    @('GROUP_COUNT', 'CommandCount'), @('LOBE_COUNT', 'ParticleCapacity'),
    @('FULL_BUILD_BUDGET', 'MaxLightCandidates'), @('POINT_BUDGET', 'LightSamples'))) {
    Require ($transient -match (('NRI_SMOKE_TRANSIENT_{0}\s+gSmokeConstants\.{1}' -f $mapping[0], $mapping[1]))) "Missing transient pass-local alias $($mapping[0])."
}
Require ($smoke -match 'SmokePassUsesVisualPhase\(pass\)[\s\S]{0,180}NRIPopulateSmokeVisualPhaseConstants[\s\S]{0,260}pass\s*>=\s*NRISmokePass::TransientClear[\s\S]{0,500}passConstants\.commandCount\s*=\s*transientGroupCount') 'Transient ABI aliases must be populated after visual-phase packing so Materialize retains the packed phase word.'
foreach ($name in @('TransientClear', 'TransientBuildBins', 'TransientLightBuild', 'TransientMaterialize')) {
    Require ($contracts.Contains($name)) "Missing CPU pass $name."
    Require ($smoke.Contains('"Smoke' + $name + '"')) "Missing pipeline name $name."
}
Require ($cmake -match 'foreach\(_transient_pass Clear BuildBins LightBuild Materialize\)') 'Transient shader rules must enumerate exactly the four isolated entry points.'
Require ($cmake -match 'set\(_transient_source\s+"\$\{CMAKE_CURRENT_SOURCE_DIR\}/common/rendering/nri/shaders/SmokeTransient\$\{_transient_pass\}[.]cs[.]hlsl"\)') 'Transient shader compilation must use the enumerated source template.'
Require ($cmake -match 'set\(_transient_target cs_6_0\)[\s\S]{0,240}if\(_transient_pass STREQUAL "LightBuild"\)[\s\S]{0,160}set\(_transient_target cs_6_6\)[\s\S]{0,160}SPV_KHR_ray_query') 'Only transient LightBuild may opt into cs_6_6 and SPV_KHR_ray_query.'
Require ($cmake -match 'OUTPUT\s+"\$\{RAZE_NRI_SHADER_OUTPUT_DIR\}/SmokeTransient\$\{_transient_pass\}[.]cs[.]dxil"\s+"\$\{RAZE_NRI_SHADER_OUTPUT_DIR\}/SmokeTransient\$\{_transient_pass\}[.]cs[.]spirv"') 'Each isolated transient rule must publish paired DXIL/SPIR-V outputs.'
Require ($cmake -match '-T \$\{_transient_target\} -E main -Fo "\$\{RAZE_NRI_SHADER_OUTPUT_DIR\}/SmokeTransient\$\{_transient_pass\}[.]cs[.]dxil" "\$\{_transient_source\}"') 'The isolated transient DXIL compile command is missing.'
Require ($cmake -match '-spirv -fspv-target-env=vulkan1[.]2 -fvk-use-dx-layout \$\{_transient_spirv_args\}[\s\S]{0,180}SmokeTransient\$\{_transient_pass\}[.]cs[.]spirv" "\$\{_transient_source\}"') 'The isolated transient SPIR-V compile command is missing.'
Require ($cmake.Contains('list(FILTER NRI_SHARED_SHADER_FILES EXCLUDE REGEX "/SmokeTransient[^/]*[.]cs[.]hlsl$")')) 'Transient entry points must be excluded from shared production/diagnostic dependencies.'
Require (([regex]::Matches($cmake, '\$\{NRI_SHARED_SHADER_FILES\}')).Count -ge 3) 'Isolated, production, and diagnostic shader rules must consume the filtered shared dependency list.'
$packageStart = $cmake.IndexOf('add_custom_target(raze_nri_shaders ALL')
Require ($packageStart -ge 0) 'Missing production shader packaging target.'
$packageBlock = $cmake.Substring($packageStart)
foreach ($name in @('TransientClear', 'TransientBuildBins', 'TransientLightBuild', 'TransientMaterialize')) {
    foreach ($suffix in @('cs.dxil', 'cs.spirv')) {
        Require ($packageBlock.Contains('Smoke' + $name + '.' + $suffix)) "Missing packaged shader output $name.$suffix."
    }
    Require (-not $cmake.Contains('Smoke' + $name + '.cs.hlsl')) "Transient entry point $name leaked back into common/diagnostic literal dependencies."
}
$volume = $smoke.Substring($smoke.IndexOf('bool NRISmokeSystem::RecordVolume('))
$indirect = $volume.IndexOf('dispatch(NRISmokePass::LightIndirectReference')
$materialize = $volume.IndexOf('dispatch(NRISmokePass::TransientMaterialize')
$integrate = $volume.IndexOf('dispatch(NRISmokePass::Integrate')
Require ($indirect -ge 0 -and $indirect -lt $materialize -and $materialize -lt $integrate) 'Transient source must be added after grid/legacy lighting and before integration.'
$newBlock = $volume.Substring($volume.IndexOf('// Grid/legacy incident lighting'), $integrate - $volume.IndexOf('// Grid/legacy incident lighting'))
Require (-not $newBlock.Contains('worldEmissiveReady')) 'Transient lighting must not depend on grid world-emissive readiness.'
Require ($newBlock.Contains('mTransientResources.StorageBarrier')) 'Transient publication needs cross-dispatch UAV barriers.'
Require ($smoke -match 'mSettings.transientEmissiveLights && emissiveResourcesReady') 'Transient emissive family readiness must not inherit grid profile overrides.'
Require ($transientRenderer -match 'transientEmissiveLights \? 16u : 0u') 'Requested transient emissive family changes must invalidate the cache policy.'
Require ($smoke -match 'passConstants.lightMode = !fieldDiagnostics &&\s*\(pointLightsReady \|\| directionalLightReady \|\| transientEmissiveReady\)') 'Transient-only emissive lighting must work when the legacy grid profile disables it.'
Require ($smoke.Contains('completedSlot.transientSnapshot')) 'GPU observations need the CPU snapshot from their own completed slot.'
Require ($smoke -match 'mStatus\.gpuStatsValid\s*=\s*false;\s*mStatus\.transient\.valid\s*=\s*false;\s*mStatus\.analyticLight\.valid\s*=\s*false;') 'A failed or stale completed readback must invalidate every joined telemetry view.'
Require ($smoke -match 'if\s*\(!mSettings\.readback\)[\s\S]{0,200}mStatus\.transient\.valid\s*=\s*false;') 'Disabling readback must not leave stale transient telemetry valid.'
Require ($transientResourcesHeader.Contains('ConsumeCacheRecreated')) 'Transient resource ownership must expose cache recreation to the CPU owner.'
Require ($transientResources -match 'mCacheRecreated\s*\|=\s*mStorage\[i\]\.buffer\s*!=\s*nullptr') 'Destroying live light-cache banks must publish cache recreation.'
$prepare = $transientRenderer.Substring($transientRenderer.IndexOf('const auto services = BuildGridServices(renderer);'))
$consume = $prepare.IndexOf('mTransientResources.ConsumeCacheRecreated()')
$invalidate = $prepare.IndexOf('mTransientClouds.InvalidateLighting()')
$upload = $prepare.IndexOf('mTransientResources.Upload')
Require ($consume -ge 0 -and $consume -lt $invalidate -and $invalidate -lt $upload) 'Cache recreation must invalidate surviving CPU groups before their replacement GPU snapshot uploads.'
Require ($smoke -match 'if \(!PrepareTransientFrame\(renderer, now\)\)') 'Simulation must delegate transient residency and upload to the focused integration owner.'
Require ($transientRenderer -match 'mTransientResidency.BeginFrame[\s\S]*mTransientResidency.SubmitBatch[\s\S]*mTransientResidency.Resolve[\s\S]*mTransientResources.Upload') 'Retained requests must be classified, resolved, and uploaded in that order.'
Require (-not ($smoke -match 'mTransientClouds.AdmitBatch')) 'Orchestration must not bypass retained admission with immediate-or-drop requests.'
Require ($smoke -match 'mTransientClouds.Reset\(mStatus.simulationEpoch\);\s*mTransientResidency.Reset\(mStatus.simulationEpoch\);') 'Simulation reset must reset GPU pool ownership and retained event history together.'
Require ($transientRenderer -match 'view.planes\[5\]\[3\] \+= farDepth') 'Conservative transient interest must include the smoke far-depth plane.'
Require ($transientRenderer -match 'if \(!view.valid\) return view') 'Malformed camera data must conservatively bypass frustum demotion.'
Require (-not ($transientResources -match 'DestroyDescriptor\(\*buffer\.(shaderView|storageView)\)')) 'NRI descriptor destruction takes descriptor pointers, not dereferenced descriptors.'
Require (-not ($transientResources -match 'DestroyBuffer\(\*buffer\.buffer\)')) 'NRI buffer destruction takes buffer pointers, not dereferenced buffers.'
Write-Host 'Transient integration contracts passed.'
