param(
	[string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$rendererDir = Join-Path $repoRoot 'source\common\rendering\nri\renderer'
$testSource = Join-Path $PSScriptRoot 'nri_trace_stats_readback_policy.tests.cpp'
$outputDir = if ($OutputDirectory) { $OutputDirectory } else {
	Join-Path $repoRoot 'build\planner-tests\trace-stats-readback'
}
$testObject = Join-Path $outputDir 'nri_trace_stats_readback_policy.tests.obj'
$testExe = Join-Path $outputDir 'nri_trace_stats_readback_policy.tests.exe'
$vsDevCmd = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat'

New-Item -ItemType Directory -Force -Path $outputDir | Out-Null

$compile = 'call "{0}" -arch=x64 -host_arch=x64 >nul && cl /nologo /c /std:c++17 /EHsc /W4 /WX /permissive- /MT /I"{1}" /Fo"{3}" "{2}"' -f `
	$vsDevCmd, $rendererDir, $testSource, $testObject
cmd /c $compile
if ($LASTEXITCODE -ne 0) {
	throw "Trace-stat readback policy compilation failed with exit code $LASTEXITCODE."
}

$link = 'call "{0}" -arch=x64 -host_arch=x64 >nul && link /nologo /out:"{1}" "{2}"' -f `
	$vsDevCmd, $testExe, $testObject
cmd /c $link
if ($LASTEXITCODE -ne 0) {
	throw "Trace-stat readback policy link failed with exit code $LASTEXITCODE."
}

& $testExe
if ($LASTEXITCODE -ne 0) {
	throw "Trace-stat readback policy tests failed with exit code $LASTEXITCODE."
}

$traceStats = Get-Content -LiteralPath (Join-Path $rendererDir 'nri_trace_stats.cpp') -Raw
$traceStatsHeader = Get-Content -LiteralPath (Join-Path $rendererDir 'nri_trace_stats.h') -Raw
$passDispatch = Get-Content -LiteralPath (Join-Path $rendererDir 'nri_pass_dispatch.cpp') -Raw
$renderDevice = Get-Content -LiteralPath (Join-Path $repoRoot 'source\common\rendering\nri\system\nri_renderdevice.cpp') -Raw
$raytracing = Get-Content -LiteralPath (Join-Path $repoRoot 'source\common\rendering\nri\shaders\Include\RaytracingShared.hlsli') -Raw

if ($traceStats -match '\bWaitForCommands\s*\(') {
	throw 'Trace shader stats must not introduce a host wait.'
}
if ($traceStatsHeader -notmatch 'NRI_TRACE_SHADER_READBACK_SLOT_COUNT' -or
	$traceStats -notmatch 'IsCommandFenceValueComplete' -or
	$traceStats -notmatch 'IsCommandFenceValueAbandoned') {
	throw 'Trace shader stats must use fence-retired readback slots with explicit abandoned-work handling.'
}
if ($traceStats -notmatch 'SelectNRITraceShaderReadback[\s\S]*MapBuffer' -or
	$traceStats -notmatch 'readbacksSuperseded' -or
	$traceStats -notmatch 'copiesDroppedBusy') {
	throw 'Trace shader stats must publish the newest ready slot and expose observer backpressure.'
}
if ($traceStats -notmatch 'NRITraceShaderInstanceAttribution' -or
	$traceStats -notmatch 'attributionBytesCopied') {
	throw 'Trace shader stats must preserve frame-correct compact attribution and expose its CPU copy volume.'
}
if ($passDispatch -notmatch 'traceOpaqueReadbackMs[\s\S]*?\.Readback\(' -or
	$passDispatch -notmatch 'traceOpaqueStatsCopyMs[\s\S]*?\.CopyForReadback\(') {
	throw 'Trace-stat observer work must remain separately timed at its readback and copy call sites.'
}
if ($passDispatch -notmatch 'GetRecordingCommandFenceValue' -or
	$passDispatch -notmatch 'IsCommandFenceValueComplete' -or
	$passDispatch -notmatch 'IsCommandFenceValueAbandoned') {
	throw 'The pass boundary must provide explicit command-fence ownership services.'
}
if ($traceStats -notmatch 'consumerFence\s*!=\s*mReadbackConsumerFence[\s\S]*?outStats\.valid\s*=\s*false' -or
	$renderDevice -notmatch 'PERF pt shader stats observer NRI:[^\n]*copies=%llu[^\n]*recorded=%llu[^\n]*busy=%llu[^\n]*no_fence=%llu[^\n]*published=%llu[^\n]*superseded=%llu[^\n]*abandoned=%llu[^\n]*map_fail=%llu[^\n]*pending=%u[^\n]*attribution_rows=%llu[^\n]*attribution_bytes=%llu') {
	throw 'Published snapshots must be one-consumer-frame events and observer cost/drop counters must be logged.'
}
if ($traceStatsHeader -notmatch 'NRI_TRACE_SHADER_SURFACE_PROBE_BASE\s*=\s*\r?\n\s*NRI_TRACE_SHADER_FILTER_QUERY_BASE\s*\+\s*NRI_TRACE_SHADER_FILTER_QUERY_COUNT' -or
	$traceStatsHeader -notmatch 'NRI_TRACE_SHADER_SURFACE_PROBE_COUNT\s*=\s*9' -or
	$traceStatsHeader -notmatch 'NRI_TRACE_SHADER_INSTANCE_COMMITTED_BASE\s*=\s*\r?\n\s*NRI_TRACE_SHADER_SURFACE_PROBE_BASE\s*\+\s*NRI_TRACE_SHADER_SURFACE_PROBE_COUNT' -or
	$raytracing -notmatch 'TRACE_STAT_SURFACE_PROBE_BASE\s*=\s*TRACE_STAT_FILTER_QUERY_BASE\s*\+\s*TRACE_STAT_FILTER_QUERY_COUNT' -or
	$raytracing -notmatch 'TRACE_STAT_SURFACE_PROBE_COUNT\s*=\s*9u' -or
	$raytracing -notmatch 'TRACE_STAT_INSTANCE_COMMITTED_BASE\s*=\s*\r?\n\s*TRACE_STAT_SURFACE_PROBE_BASE\s*\+\s*TRACE_STAT_SURFACE_PROBE_COUNT') {
	throw 'The CPU and shader trace-stat layouts must reserve the same nine-word surface-probe block before instance buckets.'
}
$surfaceProbeFields = [ordered]@{
	'VALID' = 0
	'SOURCE' = 1
	'INSTANCE' = 2
	'PRIMITIVE' = 3
	'MATERIAL' = 4
	'TEXTURE' = 5
	'PALETTE' = 6
	'FLAGS' = 7
	'LIGHTING_FLAGS' = 8
}
foreach ($field in $surfaceProbeFields.GetEnumerator()) {
	$cpuPattern = "NRI_TRACE_SHADER_SURFACE_PROBE_$($field.Key)\s*=\s*NRI_TRACE_SHADER_SURFACE_PROBE_BASE\s*\+\s*$($field.Value)"
	$shaderPattern = "TRACE_STAT_SURFACE_PROBE_$($field.Key)\s*=\s*TRACE_STAT_SURFACE_PROBE_BASE\s*\+\s*$($field.Value)u"
	if ($traceStatsHeader -notmatch $cpuPattern -or $raytracing -notmatch $shaderPattern) {
		throw "The CPU and shader surface-probe offset for $($field.Key) must remain mirrored at $($field.Value)."
	}
}
if ($raytracing -notmatch 'void\s+RecordSurfaceProbePrimaryPixel\([^\)]*\)[\s\S]*?TRACE_STAT_SURFACE_PROBE_SOURCE\]\s*=\s*hit\.dataSource;[\s\S]*?TRACE_STAT_SURFACE_PROBE_INSTANCE\]\s*=\s*hit\.instanceId;[\s\S]*?TRACE_STAT_SURFACE_PROBE_PRIMITIVE\]\s*=\s*hit\.primitiveIndex;[\s\S]*?TRACE_STAT_SURFACE_PROBE_MATERIAL\]\s*=\s*hit\.materialIndex;[\s\S]*?TRACE_STAT_SURFACE_PROBE_TEXTURE\]\s*=\s*material\.textureIndex;[\s\S]*?TRACE_STAT_SURFACE_PROBE_PALETTE\]\s*=\s*material\.paletteIndex;[\s\S]*?TRACE_STAT_SURFACE_PROBE_FLAGS\]\s*=\s*material\.flags;[\s\S]*?TRACE_STAT_SURFACE_PROBE_LIGHTING_FLAGS\]\s*=\s*material\.lightingFlags;[\s\S]*?TRACE_STAT_SURFACE_PROBE_VALID\]\s*=\s*1u;\s*\r?\n}') {
	throw 'The shader must publish all surface-probe payload words before setting the valid word.'
}

Write-Output 'NRI trace-stat nonblocking readback contract tests passed.'
