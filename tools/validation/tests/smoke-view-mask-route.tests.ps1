$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
function Read-Repo([string]$Path) { Get-Content -LiteralPath (Join-Path $root $Path) -Raw }
function Assert-Match([string]$Text, [string]$Pattern, [string]$Message) {
    if ($Text -notmatch $Pattern) { throw $Message }
}
function Assert-NotMatch([string]$Text, [string]$Pattern, [string]$Message) {
    if ($Text -match $Pattern) { throw $Message }
}

$cvars = Read-Repo 'source/common/rendering/nri/renderer/nri_cvars.cpp'
$editor = Read-Repo 'source/core/lightoverlay_smoke_editor.cpp'
$settings = Read-Repo 'source/common/rendering/nri/renderer/nri_renderer_settings.cpp'
$runtime = Read-Repo 'source/common/rendering/nri/renderer/nri_smoke.cpp'
$evaluate = Read-Repo 'source/common/rendering/nri/shaders/SmokeEvaluateGrid.cs.hlsl'
$resources = Read-Repo 'source/common/rendering/nri/shaders/Include/SmokeResources.hlsli'
$finalize = Read-Repo 'source/common/rendering/nri/shaders/SmokeViewWorkFinalize.cs.hlsl'
$owner = Read-Repo 'source/common/rendering/nri/renderer/nri_smoke_view_work.cpp'

Assert-Match $cvars 'CVAR\(Int,\s*nri_ptsmokeviewroute,\s*2,\s*0\)' 'The preset-213 view route must default compact and remain session-only.'
foreach ($diagnosticDefault in @(
    'CVAR\(Bool,\s*nri_ptsmokeemissivereference,\s*false,\s*0\)',
    'CVAR\(Int,\s*nri_ptsmokeemissivecandidate,\s*-1,\s*0\)',
    'CVAR\(Bool,\s*nri_ptsmokeemissiveworldfilter,\s*false,\s*0\)',
    'CVAR\(Int,\s*nri_ptsmokeemissiveworlddebug,\s*0,\s*0\)',
    'CVAR\(Bool,\s*nri_ptsmokeemissivelegacygatherdisable,\s*false,\s*0\)',
    'CVAR\(Bool,\s*nri_ptsmokeemissivequarterkey,\s*false,\s*0\)',
    'CVAR\(Int,\s*nri_ptsmokedirectreference,\s*0,\s*0\)',
    'CVAR\(Int,\s*nri_ptsmokemultiplescatterdebug,\s*0,\s*0\)',
    'CVAR\(Int,\s*nri_ptsmokeselfshadowdebug,\s*0,\s*0\)',
    'CVAR\(Int,\s*nri_ptsmokedebug,\s*0,\s*0\)',
    'CVAR\(Int,\s*nri_ptsmoketrace,\s*0,\s*0\)',
    'CVAR\(Bool,\s*nri_ptsmokereadback,\s*false,\s*0\)',
    'CVAR\(Bool,\s*nri_ptsmokeviewcompare,\s*false,\s*0\)')) {
    Assert-Match $cvars $diagnosticDefault 'Every smoke diagnostic selector must have a session-only off default.'
}
Assert-Match $cvars 'CVAR\(Int,\s*nri_ptsmokeemissivebackend,\s*2,\s*0\)' 'Smoke emissive lighting must default production world rather than comparison.'
Assert-Match $cvars 'CVAR\(Int,\s*nri_ptsmokerepresentation,\s*1,\s*CVAR_ARCHIVE\s*\|\s*CVAR_GLOBALCONFIG\)' 'Smoke representation must default production grid rather than split comparison.'
Assert-Match $editor 'CVAR\(Bool,\s*nri_ptmapsmokeeditmode,\s*false,\s*0\)' 'Map smoke editing instrumentation must default off and remain session-only.'
Assert-Match $settings 'viewRoute\s*=\s*\(uint32_t\)std::clamp\(\(int\)nri_ptsmokeviewroute,\s*0,\s*2\)' 'Dense, masked-full, and exact compact static routes may resolve.'
Assert-Match $runtime 'viewCompare\s*\|\|\s*mSettings\.viewRoute\s*!=\s*0u' 'Either diagnostic comparison or the static masked route must prepare the bounded mask.'
Assert-Match $runtime 'GetOutputs\(viewOutputs\)[\s\S]*columnMasks[\s\S]*constants\.flags\s*\|=\s*kSmokeFlagViewMask' 'Masked execution may activate only after a valid prepared mask is bound.'
Assert-Match $runtime 'opticalThreshold\s*=\s*0\.0f' 'The view mask must classify every represented optical coefficient, independent of lifecycle thresholds.'
Assert-Match $resources 'gSmokeViewColumnMasks\s*:\s*register\(u45,\s*space1\)' 'The main smoke layout must expose the prepared column mask at the appended descriptor.'
Assert-Match $evaluate 'NRI_SMOKE_FLAG_VIEW_MASK[\s\S]*gSmokeViewColumnMasks\[columnIndex\][\s\S]*return;' 'The masked route must be an early-out inside the existing full dispatch.'
Assert-Match $evaluate 'SmokeRenderGridIntegrateFroxel\(dispatchThreadId,\s*cellSize,\s*scalar,\s*optical,\s*source,' 'Selected froxels must retain the established evaluation function and math.'
Assert-NotMatch $evaluate 'DispatchIndirect|AppendStructuredBuffer|opaque|Opaque|CandidateCapacity|MAX_CANDIDATE' 'Slice 3B may not add indirect work, append caps, truncation, or opaque-depth culling.'
Assert-NotMatch $runtime 'available.*headroom|gpu.*budget' 'Static smoke routes may not add adaptive work policy.'
Assert-Match $runtime 'else\s+if\s*\(viewComparatorPrepared\s*&&\s*\(mSettings\.readback\s*\|\|\s*PerfCompactCaptureTimingActive\(\)\)\)[\s\S]*mViewWork\.Finish' 'The normal compact route must record CPU readback only for explicit smoke diagnostics or compact capture.'
Assert-NotMatch $runtime 'viewComparatorPrepared[^\r\n]*PerfCompactCaptureReadbackDrainActive' 'Readback drainage must not schedule new off-sample view-work copies.'
Assert-Match $owner 'CompareDense\([\s\S]{0,3000}RecordReadback\(services\)' 'The explicit dense comparator must retain its diagnostic readback.'
Assert-Match $owner 'void\s+NRISmokeViewWork::Finish\([^\)]*\)[\s\S]{0,300}RecordReadback\(services\)' 'Finish must remain a diagnostic-only readback seam.'

Assert-Match $finalize 'EvaluationDispatched[\s\S]*EvaluationSelected[\s\S]*EvaluationSkipped' 'Mask preparation must publish exact dispatched/selected/skipped work.'
foreach ($field in @('route_requested', 'route_effective', 'dispatched', 'selected', 'skipped', 'dense_contributing', 'output_hash')) {
    Assert-Match $owner ([regex]::Escape($field + '=')) "View status must report $field."
}
Assert-Match $owner 'PERF pt smoke view work NRI:[\s\S]*renderer_frame=%llu[\s\S]*selected=%u[\s\S]*output_hash=%08x%08x' 'Opt-in captures must expose frame-joinable view work and output fingerprints.'
Assert-Match $owner 'authority=smoke-evaluate-grid\s+comparator_output_mutation=no' 'SmokeEvaluateGrid must remain output authority.'

Write-Host 'Smoke static masked-dense view route contract passed.'
