$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
$contracts = Get-Content (Join-Path $root 'source\common\rendering\nri\renderer\nri_smoke_grid_contracts.h') -Raw
$ownerHeaderPath = Join-Path $root 'source\common\rendering\nri\renderer\nri_smoke_grid.h'
$ownerImplementationPath = Join-Path $root 'source\common\rendering\nri\renderer\nri_smoke_grid.cpp'
$resources = Get-Content (Join-Path $root 'source\common\rendering\nri\shaders\Include\SmokeGridResources.hlsli') -Raw
$gridData = Get-Content (Join-Path $root 'source\common\rendering\nri\shaders\Include\SmokeGridData.hlsli') -Raw
$prepare = Get-Content (Join-Path $root 'source\common\rendering\nri\shaders\SmokeGridPrepareBricks.cs.hlsl') -Raw
$evaluate = Get-Content (Join-Path $root 'source\common\rendering\nri\shaders\SmokeEvaluateGrid.cs.hlsl') -Raw
$particleEvaluate = Get-Content (Join-Path $root 'source\common\rendering\nri\shaders\SmokeEvaluateMedium.cs.hlsl') -Raw

function Assert-Contains([string]$Text, [string]$Pattern, [string]$Message) {
    if ($Text -notmatch $Pattern) { throw $Message }
}

if (-not (Test-Path $ownerHeaderPath) -or -not (Test-Path $ownerImplementationPath)) {
    throw 'The focused sparse-grid owner is missing.'
}
$ownerHeader = Get-Content $ownerHeaderPath -Raw
$ownerImplementation = Get-Content $ownerImplementationPath -Raw

Assert-Contains $contracts 'NRI_SMOKE_GRID_BRICK_AXIS\s*=\s*8u' 'Sparse smoke bricks must remain 8^3.'
Assert-Contains $contracts 'AdvectVelocity[\s\S]*AdvectFields' 'Velocity and material advection must be separate barrierable passes.'
Assert-Contains $contracts 'static_assert\(sizeof\(NRISmokeGridConstants\) == 128\)' 'Sparse-grid root constants must retain the Vulkan-safe 128-byte contract.'
Assert-Contains $gridData 'NRI_SMOKE_GRID_TOMBSTONE' 'Open-addressed hash deletion must preserve probe chains with tombstones.'
Assert-Contains $gridData 'float CurlTime;[\s\S]*float CurlEvolution;[\s\S]*float VorticityConfinement;' 'Temporal curl and confinement must consume the existing root-constant tail.'
Assert-Contains $resources 'State\s*==\s*NRI_SMOKE_GRID_RESIDENT' 'Sampling must reject unpublished bricks.'
Assert-Contains $resources 'gSmokeGridVorticity\s*:\s*register\(u22, space1\)' 'Vorticity scratch must have one focused grid-only storage binding.'
Assert-Contains $ownerHeader 'StorageDescriptorCount\s*=\s*23u' 'The grid storage layout must bind the vorticity scratch resource.'
Assert-Contains $ownerHeader 'EvaluationDescriptorCount\s*=\s*11u' 'Vorticity scratch must not expand the render-evaluation surface.'
Assert-Contains $ownerHeader 'DormantTransactionDescriptorCount\s*=\s*18u' 'Vorticity scratch must not expand the dormant transaction surface.'
Assert-Contains $ownerImplementation 'CreateBuffer\(services, mVorticity, cells \* 16u, 16u' 'Vorticity scratch must allocate exactly one float4 per grid cell.'
Assert-Contains $prepare 'gSmokeGridVorticity\[cellIndex\]\s*=\s*0\.0' 'New bricks must publish cleared vorticity scratch cells.'
Assert-Contains $ownerImplementation 'AdvectVelocity\);[\s\S]*StorageBarrier\(services\);[\s\S]*AdvectFields\);' 'Confinement must consume vorticity only after the existing velocity barrier.'
Assert-Contains $ownerImplementation 'mSimulationSeconds \+= \(double\)constants\.deltaTime \* \(double\)constants\.timeScale' 'Curl time must advance only by completed simulation substeps.'
Assert-Contains $ownerImplementation 'vorticity_clamps=%u' 'Sparse-grid status must expose confinement velocity clamps.'
Assert-Contains $ownerImplementation 'MemoryLocation::DEVICE' 'Sparse field storage must remain device local.'
Assert-Contains $ownerImplementation 'CmdDispatchIndirect' 'Sparse active-brick work must use GPU-built indirect dispatch.'
Assert-Contains $ownerImplementation 'deposition_cells=%u deposition_rejected=%u' 'Sparse-grid status must publish deposition work and rejection counts.'
Assert-Contains $ownerHeader 'GetEvaluationStorageDescriptors' 'The grid owner must expose a narrow render-evaluation surface.'
Assert-Contains $evaluate 'SmokeRenderGridSample' 'Grid rendering must trilinearly sample the world field.'
Assert-Contains $evaluate 'NRI_SMOKE_FLAG_COMPARE_REPRESENTATION' 'Grid comparison mode must own only one side of the image.'
Assert-Contains $particleEvaluate 'NRI_SMOKE_FLAG_COMPARE_REPRESENTATION' 'Particle comparison mode must own only one side of the image.'
Assert-Contains $evaluate 'gSmokeOccupiedFroxelIndices' 'Grid evaluation must feed the existing occupied-froxel lighting worklist.'

Write-Host 'Smoke sparse-grid contract validation passed.'
