$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
function Read-Source([string]$Path) { Get-Content -Raw (Join-Path $root $Path) }
function Assert-Match([string]$Text, [string]$Pattern, [string]$Message) {
	if ($Text -notmatch $Pattern) { throw $Message }
}
function Assert-NotMatch([string]$Text, [string]$Pattern, [string]$Message) {
	if ($Text -match $Pattern) { throw $Message }
}

$contracts = Read-Source 'source/common/rendering/nri/renderer/nri_smoke_view_work_contracts.h'
$owner = Read-Source 'source/common/rendering/nri/renderer/nri_smoke_view_work.cpp'
$project = Read-Source 'source/common/rendering/nri/shaders/SmokeViewWorkProjectTiles.cs.hlsl'
$resolveDeposit = Read-Source 'source/common/rendering/nri/shaders/SmokeGridResolveDeposit.cs.hlsl'
$rebuild = Read-Source 'source/common/rendering/nri/shaders/SmokeGridRebuild.cs.hlsl'
$rehydrate = Read-Source 'source/common/rendering/nri/shaders/SmokeDormantGridRehydrate.cs.hlsl'
$expand = Read-Source 'source/common/rendering/nri/shaders/SmokeViewWorkExpandColumns.cs.hlsl'
$finalize = Read-Source 'source/common/rendering/nri/shaders/SmokeViewWorkFinalize.cs.hlsl'
$prefix = Read-Source 'source/common/rendering/nri/shaders/SmokeViewWorkPrefixColumns.cs.hlsl'
$smoke = Read-Source 'source/common/rendering/nri/renderer/nri_smoke.cpp'

Assert-Match $contracts 'NRI_SMOKE_VIEW_TILE_AXIS\s*=\s*8u' 'View work must retain the measured 8x8 column tile.'
Assert-Match $contracts 'NRI_SMOKE_VIEW_MASK_WORDS\s*=\s*2u' 'A column must retain all 48 default depth slices.'
Assert-Match $contracts 'brickTilePairBound[\s\S]*opticalCellTestBound[\s\S]*preparationUnitBound' 'Bounded input-work accounting is incomplete.'
Assert-Match $contracts 'attemptedMarks[\s\S]*duplicateMerges[\s\S]*uniqueFroxels[\s\S]*overflow' 'Exact output counters are incomplete.'
Assert-Match $contracts 'falseNegatives[\s\S]*falsePositives[\s\S]*tauErrorBits[\s\S]*opacityErrorBits[\s\S]*radianceErrorBits' 'Reference comparison contract has no reserved evidence.'
Assert-Match $owner 'brickTilePairBound\s*=\s*\(uint64_t\)result.tileCount \* brickCapacity' 'Tile/brick input work must have a closed form bound.'
Assert-Match $owner 'preparationUnitBound\s*=\s*result.brickTilePairBound \+ result.columnCount' 'Preparation must be bounded by tile-brick pairs plus columns.'
Assert-Match $project '\[numthreads\(64, 1, 1\)\][\s\S]*for \(uint brickIndex = 0u; brickIndex < gViewConstants.BrickCapacity; \+\+brickIndex\)' 'Each tile group must scan the complete fixed brick capacity.'
Assert-Match $project 'brick\.Flags\s*&\s*NRI_SMOKE_GRID_BRICK_OPTICAL_CONTENT' 'View discovery must consume the exact published optical summary.'
Assert-NotMatch $project 'gViewGridOptical[AB]\[' 'View discovery must not rescan per-cell optical buffers.'
Assert-Match $resolveDeposit 'any\(abs\(optical\)\s*>\s*0\.0\)[\s\S]*InterlockedOr\(gSmokeGridBricks\[brickIndex\]\.Flags,\s*NRI_SMOKE_GRID_BRICK_OPTICAL_CONTENT\)' 'Zero-substep deposition must publish optical support.'
Assert-Match $rebuild 'opticalOccupied\s*=\s*any\(abs\(optical\)\s*>\s*0\.0\)[\s\S]*opticalFlags[\s\S]*NRI_SMOKE_GRID_BRICK_CONTENT[\s\S]*opticalFlags[\s\S]*NRI_SMOKE_GRID_BRICK_HALO\s*\|\s*opticalFlags' 'Simulation rebuild must reduce all optical lanes and publish support for both lifecycle branches.'
Assert-Match $rebuild 'if \(\(gSmokeGridOccupied\[0\]\s*&\s*1u\)\s*!=\s*0u\)' 'Optical summary bits must not change brick lifecycle occupancy.'
Assert-Match $rehydrate 'InterlockedOr\(sOpticalContent,\s*1u\)[\s\S]*InterlockedOr\(gDormantFineBricks\[sFineIndex\]\.Flags,\s*NRI_SMOKE_GRID_BRICK_OPTICAL_CONTENT\)[\s\S]*State\s*=\s*NRI_SMOKE_GRID_RESIDENT' 'Dormant rehydration must publish exact optical support before residency.'
Assert-Match $project 'NRI_SMOKE_GRID_BRICK_AXIS \* 0.5 \+ 1.0' 'Projected brick support must include a trilinear-support expansion.'
Assert-Match $project 'cameraInside[\s\S]*crossesNear[\s\S]*minimumUv = 0.0[\s\S]*maximumUv = 1.0' 'Camera-inside and near-plane projection must remain conservative.'
Assert-Match $expand 'gViewColumnMasks\[columnIndex\]\.Words = mask' 'Merged tile masks must publish one mask per froxel column.'
Assert-Match ($prefix + $finalize) 'CompactCount[\s\S]*IndirectArgs[\s\S]*Overflow' 'Exact compaction must publish indirect work or fail closed on overflow.'
Assert-NotMatch $project 'AppendStructuredBuffer|InterlockedAdd\([^\r\n]*candidate|MAX_CANDIDATE|CandidateCapacity' 'View discovery must not hide truncation behind a candidate append cap.'
Assert-NotMatch $project 'opaque|Opaque|depth texture|DepthTexture' 'The conservative mask must not cull against opaque depth.'
Assert-Match $smoke 'SmokeMaterialize[\s\S]*EvaluateGrid' 'Dense evaluation must remain an explicit production stage.'
Assert-Match $smoke 'mSettings\.viewRoute\s*==\s*2u[\s\S]*dispatchIndirect\(NRISmokePass::EvaluateGridCompact[\s\S]*else[\s\S]*NRISmokePass::EvaluateGrid' 'Indirect materialization must remain an explicit route-2 alternative to dense authority.'

$tileX = [math]::Ceiling(120 / 8)
$tileY = [math]::Ceiling(68 / 8)
$tiles = $tileX * $tileY
$columns = 120 * 68
$bricks = 512
if ($tileX -ne 15 -or $tileY -ne 9 -or $tiles -ne 135) {
	throw 'Default tile dimensions are not 15x9 (135 tiles).'
}
if (($tiles * $bricks + $columns) -ne 77280) {
	throw 'Default preparation unit bound changed unexpectedly.'
}
if ((2 * 32) -lt 48) {
	throw 'Column masks do not cover the default depth lattice.'
}

Write-Host 'Smoke bounded view-work contracts passed.'
