$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$modelSourceDir = Join-Path $repoRoot 'source\common\models'
$testSource = Join-Path $PSScriptRoot 'nri_voxel_adjacency_normals.tests.cpp'
$outputDir = Join-Path $repoRoot 'build\voxel-adjacency-normal-tests'
$testExe = Join-Path $outputDir 'nri_voxel_adjacency_normals.tests.exe'
$testObject = Join-Path $outputDir 'nri_voxel_adjacency_normals.tests.obj'
$vsDevCmd = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat'

New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
$compile = 'call "{0}" -arch=x64 -host_arch=x64 >nul && cl /nologo /std:c++17 /EHsc /W4 /WX /I"{1}" /Fo"{4}" "{2}" /Fe:"{3}"' -f `
	$vsDevCmd, $modelSourceDir, $testSource, $testExe, $testObject
cmd /c $compile
if ($LASTEXITCODE -ne 0)
{
	throw "Voxel adjacency-normal test compilation failed with exit code $LASTEXITCODE."
}

& $testExe
if ($LASTEXITCODE -ne 0)
{
	throw "Voxel adjacency-normal tests failed with exit code $LASTEXITCODE."
}

$normalShader = Get-Content -Raw -LiteralPath (Join-Path $repoRoot 'source\common\rendering\nri\shaders\Include\VoxelNormals.hlsli')
$parallelEmitter = Get-Content -Raw -LiteralPath (Join-Path $repoRoot 'source\common\rendering\nri\shaders\VoxelComputeEmitParallel.cs.hlsl')
$classifyShader = Get-Content -Raw -LiteralPath (Join-Path $repoRoot 'source\common\rendering\nri\shaders\VoxelComputeClassify.cs.hlsl')
$hitShader = Get-Content -Raw -LiteralPath (Join-Path $repoRoot 'source\common\rendering\nri\shaders\Include\RaytracingShared.hlsli')
$sceneBridge = Get-Content -Raw -LiteralPath (Join-Path $repoRoot 'source\common\rendering\nri\scene\nri_scene_bridge.cpp')
$persistentVoxels = Get-Content -Raw -LiteralPath (Join-Path $repoRoot 'source\common\rendering\nri\renderer\nri_persistent_voxels.cpp')
$meshing = Get-Content -Raw -LiteralPath (Join-Path $repoRoot 'source\common\rendering\nri\renderer\nri_voxel_compute_meshing.cpp')
$modelVoxel = Get-Content -Raw -LiteralPath (Join-Path $repoRoot 'source\common\models\models_voxel.cpp')

if ($normalShader -notmatch 'BuildVoxelSurfaceNormal' -or
	$normalShader -notmatch 'BuildVoxelSurfaceNormalKey' -or
	$normalShader -notmatch 'BuildVoxelAdjacencyNormalSpans' -or
	$normalShader -notmatch 'run\.LateralExposureMask' -or
	$normalShader -notmatch 'slab\.CapExposureMask' -or
	$parallelEmitter -notmatch 'EmitVoxelSideSpansForRun' -or
	$parallelEmitter -notmatch 'BuildVoxelSurfaceNormal\(exposureMask, faceCullBit\)' -or
	$parallelEmitter -notmatch 'run\.LateralExposureMask, slab\.CapExposureMask, spans\[spanIndex\]\.ZOffset' -or
	$parallelEmitter -notmatch 'EmitVoxelSideSpan\([^;]+exposureMask, faceCullBit' -or
	$parallelEmitter -match 'localVoxel < run\.ZLength' -or
	$parallelEmitter -notmatch 'uint3\(shadingNormal, shadingNormal, shadingNormal\)' -or
	$classifyShader -notmatch 'CountVoxelAdjacencyNormalSpans' -or
	$classifyShader -notmatch 'AlgorithmVersion == 3u' -or
	$hitShader -match 'ResolveVoxelBevelMask' -or
	$hitShader -notmatch 'lerp\(resolvedNormal, smoothNormal, blend\)' -or
	$sceneBridge -notmatch 'VXGEOM04' -or
	$sceneBridge -notmatch 'VXRPRI05' -or
	$persistentVoxels -notmatch 'PVMESHR4' -or
	$meshing -notmatch 'run\.LateralExposureMask' -or
	$modelVoxel -notmatch 'VoxelSurfaceRunLength' -or
	$modelVoxel -notmatch 'runRecord\.lateralExposureMask = lateralExposureMask' -or
	$modelVoxel -notmatch 'record\.capExposureMask = capExposureMask' -or
	$meshing -notmatch 'parallel_voxel_adjacency_coalesced_v3')
{
	throw 'Voxel adjacency-normal source contract is incomplete.'
}

Write-Host 'Voxel adjacency-normal tests passed.'
