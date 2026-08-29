$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
$cpuContract = Get-Content (Join-Path $root 'source\common\rendering\nri\renderer\nri_fluid_surface_gpu.h') -Raw
$shaderContract = Get-Content (Join-Path $root 'source\common\rendering\nri\shaders\Include\FluidSurfaceContracts.hlsli') -Raw
$gpuBuilder = Get-Content (Join-Path $root 'source\common\rendering\nri\renderer\nri_fluid_surfaces.cpp') -Raw
$dispatcher = Get-Content (Join-Path $root 'source\common\rendering\nri\shaders\Include\FluidSurface.hlsli') -Raw
$evaluator = Get-Content (Join-Path $root 'source\common\rendering\nri\shaders\Include\FluidSurfaceD3D.hlsli') -Raw
$spirvCompatibility = Get-Content (Join-Path $root 'source\common\rendering\nri\shaders\Include\FluidSurfaceSpirvCompatibility.hlsli') -Raw
$trace = Get-Content (Join-Path $root 'source\common\rendering\nri\shaders\TraceOpaque.cs.hlsl') -Raw
$shaderSlots = Get-Content (Join-Path $root 'source\common\rendering\nri\renderer\nri_shader_contracts.h') -Raw

function Assert-Contains([string]$Text, [string]$Pattern, [string]$Message) {
    if ($Text -notmatch $Pattern) { throw $Message }
}

function Assert-NotContains([string]$Text, [string]$Pattern, [string]$Message) {
    if ($Text -match $Pattern) { throw $Message }
}

Assert-Contains $cpuContract 'NRI_FLUID_MAX_LAYERS\s*=\s*4u' 'CPU fluid layer cap must remain four.'
Assert-Contains $shaderContract 'NRI_FLUID_MAX_LAYERS\s+4u' 'HLSL fluid layer cap must remain four.'
Assert-Contains $cpuContract 'sizeof\(NRIFluidBindingGpuData\)\s*==\s*224u' 'CPU binding ABI size check is missing.'
Assert-Contains $cpuContract 'sizeof\(NRIFluidFrameGpuData\)\s*==\s*48u' 'CPU frame ABI size check is missing.'
Assert-Contains $shaderSlots 'NRI_SCENE_DATA_DESCRIPTOR_NUM\s*=\s*30' 'Scene descriptor count does not include fluid resources.'
Assert-Contains $shaderSlots 'NRI_SCENE_DATA_FLUID_BINDING_SLOT\s*=\s*28' 'Fluid binding descriptor slot changed unexpectedly.'
Assert-Contains $shaderSlots 'NRI_SCENE_DATA_FLUID_FRAME_SLOT\s*=\s*29' 'Fluid frame descriptor slot changed unexpectedly.'
Assert-Contains $dispatcher '__spirv__[\s\S]*?FluidSurfaceSpirvCompatibility\.hlsli[\s\S]*?FluidSurfaceD3D\.hlsli' 'Fluid shader backend dispatch is missing its explicit DXC compatibility path.'
Assert-Contains $spirvCompatibility 'void FluidEvaluateOctaves\(' 'The previously validated Vulkan fluid evaluator is not preserved.'

Assert-Contains $evaluator 'FluidHash01\(' 'Procedural layers need a stable seed hash.'
Assert-Contains $evaluator 'FluidHashCellSigned\(' 'The field is missing deterministic integer lattice hashing.'
Assert-Contains $evaluator '\(uint\)cell\.x[\s\S]*?\(uint\)cell\.y' 'Signed world-domain cells are not hashed through stable integer bit patterns.'
Assert-Contains $evaluator 'FluidValueNoise\([\s\S]*?f \* f \* \(3\.0 - 2\.0 \* f\)[\s\S]*?lerp' 'Smooth cubic value noise is missing from the D3D fluid field.'
Assert-Contains $evaluator 'FluidChoppyWaveDerivative\([\s\S]*?choppiness[\s\S]*?sineDouble[\s\S]*?cosineDouble' 'Choppiness does not shape both wave value and analytic derivative.'
Assert-Contains $evaluator 'FluidRotate2\([\s\S]*?rotationCosine[\s\S]*?rotationSine' 'Octaves do not use the authored stable domain rotation.'
Assert-Contains $evaluator '\[loop\][\s\S]*?layer\s*<\s*NRI_FLUID_FIELD_LAYER_CAP' 'Procedural work is not visibly bounded by the fixed layer cap.'
Assert-Contains $evaluator 'frame\.currentGameplaySeconds\s*\*\s*timeScale' 'Gameplay time is not the procedural motion authority.'
Assert-Contains $evaluator 'filterWidth[\s\S]*?filteredWeight' 'Continuous footprint filtering is missing.'
Assert-Contains $evaluator 'heightRatio\s*/=\s*filteredWeightSum[\s\S]*?gradientRatio\s*/=\s*filteredWeightSum' 'Height and derivative are not normalized by the same octave weights.'
Assert-Contains $evaluator 'NRI_FLUID_STYLE_FLOW' 'Horizontal flow composition is missing.'
Assert-Contains $evaluator 'NRI_FLUID_STYLE_FALL' 'Falling-sheet composition is missing.'
Assert-Contains $evaluator 'NRI_FLUID_STYLE_CRUST_EMISSION' 'Lava crust/emission composition is missing.'
Assert-NotContains $evaluator 'ridgedWave|ridgeMix|FluidEvaluateVirtualHeightRatio' 'The legacy ridged/two-wave carrier remains in the fluid field.'

foreach ($mapping in @(
    'surfaceResponse\[3\]\s*=\s*std::clamp\(source\.maxSlope',
    'spectrum\[2\]\s*=\s*std::clamp\(source\.lacunarity',
    'spectrum\[3\]\s*=\s*std::clamp\(source\.gain',
    'shaping\[0\]\s*=\s*std::clamp\(source\.choppiness',
    'shaping\[1\]\s*=\s*std::clamp\(source\.choppyRelaxation',
    'shaping\[2\]\s*=\s*std::clamp\(source\.noiseScale',
    'shaping\[3\]\s*=\s*source\.octaveRotation',
    'virtualParams\[2\]\s*=\s*std::clamp\(source\.crestGain')) {
    Assert-Contains $gpuBuilder $mapping "GPU fluid binding is missing authored field mapping: $mapping"
}
Assert-NotContains $gpuBuilder 'spectrum\[2\]\s*=\s*1\.92f|0\.30f\s*\+\s*source\.choppiness' `
    'GPU fluid binding still derives authored field controls from legacy literals.'

Assert-Contains $evaluator 'allowVirtualHeight\s*&&[\s\S]*?NRI_FLUID_FRAME_FLAG_VIRTUAL_HEIGHT' 'Virtual height is not gated by both the caller and runtime frame flag.'
Assert-Contains $gpuBuilder 'frame\.virtualStepCount\s*=\s*1u' 'The compatibility frame contract does not publish the single-sample virtual-height budget.'
Assert-NotContains $evaluator 'for \(uint virtualStep' 'D3D virtual height unexpectedly contains an iterative predictor loop.'
Assert-Contains $evaluator 'maximumTravel\s*=\s*min' 'Virtual-height grazing travel is not clamped.'
Assert-Contains $gpuBuilder 'virtualHalfHeight\s*=\s*std::max\(source\.virtualHeight,\s*0\.0f\)' 'Virtual height must remain an authored non-negative world-unit half-height.'
Assert-Contains $gpuBuilder 'virtualParams\[0\]\s*=\s*virtualHalfHeight' 'The authored world-unit virtual height is not uploaded directly.'
Assert-Contains $gpuBuilder 'virtualParams\[1\]\s*=\s*virtualHalfHeight\s*/\s*minimumVirtualViewCosine' 'The virtual-height ray-travel bound is not derived in world units.'
Assert-Contains $evaluator 'binding\.virtualParams\.x\s*>\s*0\.0' 'A zero authored virtual height must disable refinement.'
Assert-Contains $evaluator 'FluidEvaluateVirtualField\([\s\S]*?binding\.seed[\s\S]*?FluidChoppyWaveDerivative' 'Virtual height is not bounded to the shared low-frequency field predictor.'
Assert-Contains $evaluator 'FluidEvaluateVirtualField\([\s\S]*?FluidValueNoise\([\s\S]*?FluidChoppyWaveDerivative\(' 'Virtual height does not use the shared noise-warped wave primitives.'
Assert-Contains $evaluator 'if \(useVirtualHeight\)[\s\S]*?FluidEvaluateVirtualField\(' 'Virtual-height prediction does not evaluate the shared field prefix.'
Assert-Contains $evaluator 'targetOffset\s*=\s*clamp\([\s\S]*?targetHeight\s*/\s*signedViewCosine' 'Virtual-height prediction is missing the shared-field offset update.'
Assert-Contains $evaluator 'FluidEvaluateField\([\s\S]*?sampleDomain[\s\S]*?layerCap' 'Final shading does not evaluate the shared field helper.'
Assert-Contains $evaluator 'evaluation\.samplePosition\s*=\s*samplePosition' 'Virtual height does not publish its shading-only sample position.'
Assert-Contains $evaluator 'evaluation\.carrierNormal\s*=\s*carrierNormal' 'The authoritative carrier normal is not kept separate.'

Assert-Contains $trace 'TryEvaluateFluidSurface\(' 'TraceOpaque does not invoke the fluid evaluator.'
Assert-Contains $trace 'RecordSurfaceProbePrimaryPixel\(pixelPos,\s*hit,\s*material,\s*fluidSurface\.active\)' 'The surface probe does not publish actual shader-side fluid activation.'
Assert-Contains $trace 'guideNormal\s*=\s*fluidSurface\.shadingNormal' 'The procedural normal is not supplied to the temporal/NRD guide.'
Assert-Contains $trace 'FluidFaceCarrierNormalToDirection\(fluidSurface\.carrierNormal' 'Fluid shadow origins are not biased from the carrier normal.'
Assert-Contains $trace 'directEmission\s*\+=\s*fluidSurface\.emission' 'Surface-only fluid emission is not composed at the accepted hit.'
Assert-Contains $trace 'max\(existingReactive,\s*fluidSurface\.reactive\)' 'Procedural motion does not contribute to temporal reactivity.'

Write-Host 'Fluid surface CPU/HLSL contract validation passed.'
