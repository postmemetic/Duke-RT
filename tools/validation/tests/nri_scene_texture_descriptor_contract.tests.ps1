$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$cpu = Get-Content -Raw (Join-Path $repoRoot 'source/common/rendering/nri/renderer/nri_shader_contracts.h')
$residency = Get-Content -Raw (Join-Path $repoRoot 'source/common/rendering/nri/renderer/nri_scene_textures.h')
$shared = Get-Content -Raw (Join-Path $repoRoot 'source/common/rendering/nri/shaders/Include/Shared.hlsli')
$smokeShader = Get-Content -Raw (Join-Path $repoRoot 'source/common/rendering/nri/shaders/Include/SmokeLighting.hlsli')
$smokeRuntime = Get-Content -Raw (Join-Path $repoRoot 'source/common/rendering/nri/renderer/nri_smoke.cpp')

function Require-Match([string]$Text, [string]$Pattern, [string]$Message) {
    if ($Text -notmatch $Pattern) { throw $Message }
}

Require-Match $cpu 'NRI_MAX_SCENE_TEXTURES\s*=\s*1024' 'CPU scene texture ceiling must be 1024.'
Require-Match $cpu 'NRI_SCENE_DESCRIPTOR_NUM\s*=\s*NRI_BLUE_NOISE_SOBOL_SLOT\s*\+\s*1' 'CPU scene descriptor count must include both blue-noise slots.'
Require-Match $residency 'mSlotTable\s*\{\s*NRI_MAX_SCENE_TEXTURES\s*\}' 'Scene residency slot capacity must derive from the shader contract.'
Require-Match $shared '#define\s+MAX_SCENE_TEXTURES\s+1024' 'HLSL scene texture ceiling must be 1024.'
Require-Match $shared 'gBlueNoiseScramblingRanking\s*:\s*register\(t1026,\s*space1\)' 'Scrambling/ranking texture must follow palette, sky, and 1024 scene textures.'
Require-Match $shared 'gBlueNoiseSobol\s*:\s*register\(t1027,\s*space1\)' 'Sobol texture must occupy the final scene-set descriptor.'
Require-Match $smokeShader '#define\s+NRI_SMOKE_SCENE_TEXTURE_COUNT\s+1024u' 'Smoke must expose all scene texture slots.'
Require-Match $smokeShader '#define\s+NRI_SMOKE_WORLD_TLAS_REGISTER\s+t1044' 'Smoke TLAS must follow its 18 buffers and 1026 textures.'
Require-Match $smokeRuntime 'smokeSceneTextureDescriptorCount\s*=\s*2u\s*\+\s*NRI_MAX_SCENE_TEXTURES' 'Smoke runtime count must exclude blue-noise descriptors.'
Require-Match $smokeRuntime 'smokeWorldTlasRegister\s*=\s*18u\s*\+\s*smokeSceneTextureDescriptorCount' 'Smoke TLAS register must derive from the filtered texture range.'

Write-Host 'NRI scene texture descriptor contract tests passed.'
