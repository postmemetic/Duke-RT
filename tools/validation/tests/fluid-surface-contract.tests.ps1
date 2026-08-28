$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
$cpuContract = Get-Content (Join-Path $root 'source\common\rendering\nri\renderer\nri_fluid_surface_gpu.h') -Raw
$shaderContract = Get-Content (Join-Path $root 'source\common\rendering\nri\shaders\Include\FluidSurfaceContracts.hlsli') -Raw
$evaluator = Get-Content (Join-Path $root 'source\common\rendering\nri\shaders\Include\FluidSurface.hlsli') -Raw
$trace = Get-Content (Join-Path $root 'source\common\rendering\nri\shaders\TraceOpaque.cs.hlsl') -Raw
$shaderSlots = Get-Content (Join-Path $root 'source\common\rendering\nri\renderer\nri_shader_contracts.h') -Raw

function Assert-Contains([string]$Text, [string]$Pattern, [string]$Message) {
    if ($Text -notmatch $Pattern) { throw $Message }
}

Assert-Contains $cpuContract 'NRI_FLUID_MAX_LAYERS\s*=\s*4u' 'CPU fluid layer cap must remain four.'
Assert-Contains $shaderContract 'NRI_FLUID_MAX_LAYERS\s+4u' 'HLSL fluid layer cap must remain four.'
Assert-Contains $cpuContract 'sizeof\(NRIFluidBindingGpuData\)\s*==\s*224u' 'CPU binding ABI size check is missing.'
Assert-Contains $cpuContract 'sizeof\(NRIFluidFrameGpuData\)\s*==\s*48u' 'CPU frame ABI size check is missing.'
Assert-Contains $shaderSlots 'NRI_SCENE_DATA_DESCRIPTOR_NUM\s*=\s*30' 'Scene descriptor count does not include fluid resources.'
Assert-Contains $shaderSlots 'NRI_SCENE_DATA_FLUID_BINDING_SLOT\s*=\s*28' 'Fluid binding descriptor slot changed unexpectedly.'
Assert-Contains $shaderSlots 'NRI_SCENE_DATA_FLUID_FRAME_SLOT\s*=\s*29' 'Fluid frame descriptor slot changed unexpectedly.'

Assert-Contains $evaluator 'FluidHash01\(' 'Procedural layers need a stable seed hash.'
Assert-Contains $evaluator '\[unroll\][\s\S]*?layer\s*<\s*NRI_FLUID_MAX_LAYERS' 'Procedural work is not visibly bounded by the fixed layer cap.'
Assert-Contains $evaluator 'frame\.currentGameplaySeconds\s*\*\s*timeScale' 'Gameplay time is not the procedural motion authority.'
Assert-Contains $evaluator 'filterWidth[\s\S]*?filteredAmplitude' 'Continuous footprint filtering is missing.'
Assert-Contains $evaluator 'NRI_FLUID_STYLE_FLOW' 'Horizontal flow composition is missing.'
Assert-Contains $evaluator 'NRI_FLUID_STYLE_FALL' 'Falling-sheet composition is missing.'
Assert-Contains $evaluator 'NRI_FLUID_STYLE_VISCOUS' 'Viscous composition is missing.'
Assert-Contains $evaluator 'NRI_FLUID_STYLE_CRUST_EMISSION' 'Lava crust/emission composition is missing.'

Assert-Contains $evaluator 'allowVirtualHeight\s*&&[\s\S]*?NRI_FLUID_FRAME_FLAG_VIRTUAL_HEIGHT' 'Virtual height is not gated by both the caller and runtime frame flag.'
Assert-Contains $evaluator 'virtualStepCount,\s*2u,\s*12u' 'Virtual-height quality input is not bounded.'
Assert-Contains $evaluator 'maximumTravel\s*=\s*min' 'Virtual-height grazing travel is not clamped.'
Assert-Contains $evaluator 'evaluation\.samplePosition\s*=\s*samplePosition' 'Virtual height does not publish its shading-only sample position.'
Assert-Contains $evaluator 'evaluation\.carrierNormal\s*=\s*carrierNormal' 'The authoritative carrier normal is not kept separate.'

Assert-Contains $trace 'TryEvaluateFluidSurface\(' 'TraceOpaque does not invoke the fluid evaluator.'
Assert-Contains $trace 'guideNormal\s*=\s*fluidSurface\.shadingNormal' 'The procedural normal is not supplied to the temporal/NRD guide.'
Assert-Contains $trace 'FluidFaceCarrierNormalToDirection\(fluidSurface\.carrierNormal' 'Fluid shadow origins are not biased from the carrier normal.'
Assert-Contains $trace 'directEmission\s*\+=\s*fluidSurface\.emission' 'Surface-only fluid emission is not composed at the accepted hit.'
Assert-Contains $trace 'max\(existingReactive,\s*fluidSurface\.reactive\)' 'Procedural motion does not contribute to temporal reactivity.'

Write-Host 'Fluid surface CPU/HLSL contract validation passed.'
