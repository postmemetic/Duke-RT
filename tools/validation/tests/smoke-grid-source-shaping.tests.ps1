$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
$shaderRoot = Join-Path $root 'source\common\rendering\nri\shaders'

function Read-Shader([string]$Name) {
    return Get-Content (Join-Path $shaderRoot $Name) -Raw
}

function Assert-Contains([string]$Text, [string]$Pattern, [string]$Message) {
    if ($Text -notmatch $Pattern) { throw $Message }
}

function Assert-Near([double]$Actual, [double]$Expected, [double]$Tolerance, [string]$Message) {
    if ([math]::Abs($Actual - $Expected) -gt $Tolerance) {
        throw "$Message (actual=$Actual expected=$Expected tolerance=$Tolerance)"
    }
}

$source = Read-Shader 'Include\SmokeSourceShaping.hlsli'
$particleResources = Read-Shader 'Include\SmokeResources.hlsli'
$gridResources = Read-Shader 'Include\SmokeGridResources.hlsli'
$spawn = Read-Shader 'SmokeSpawn.cs.hlsl'
$deposit = Read-Shader 'SmokeGridDeposit.cs.hlsl'
$resolve = Read-Shader 'SmokeGridResolveDeposit.cs.hlsl'
$advect = Read-Shader 'SmokeGridAdvectVelocity.cs.hlsl'
$advectFields = Read-Shader 'SmokeGridAdvectFields.cs.hlsl'

Assert-Contains $particleResources '#include "SmokeSourceShaping\.hlsli"' 'Particle shaders must consume the shared source-shaping contract.'
Assert-Contains $gridResources '#include "SmokeSourceShaping\.hlsli"' 'Grid shaders must consume the shared source-shaping contract.'
Assert-Contains $spawn 'SmokeSourceRandomDirection\(randomState\)[\s\S]*SmokeInjectionVelocityAxis\(command, halfAxisU, halfAxisV\)[\s\S]*SmokeSourceVelocityDirection\(velocityAxis,[\s\S]*command\.VelocityCone' 'Particle spawn must retain its spherical/cone sampling through the shared helper and rectangle-normal fallback.'
Assert-Contains $deposit 'SmokeSourceCellSeed\(command\.Serial, cell\)' 'Grid source variation must be deterministic in command/cell space.'
Assert-Contains $deposit 'SmokeInjectionVelocityAxis\(command, halfAxisU, halfAxisV\)[\s\S]*SmokeSourceVelocityDirection\(velocityAxis,[\s\S]*command\.VelocityCone' 'Grid deposition must consume the authored velocity cone and rectangle-normal fallback.'
Assert-Contains $deposit 'velocityDirection \*[\s\S]*style\.VelocityRandom' 'Grid deposition must consume authored stochastic velocity.'
Assert-Contains $deposit 'expansionDirection \*[\s\S]*style\.ExpansionVelocity' 'Grid deposition must add radial authored expansion momentum.'
Assert-Contains $deposit 'SmokeSourceLimitVelocity\([\s\S]*gSmokeGridConstants\.MaxVelocity' 'Grid source momentum must be finite and bounded before quantization.'
Assert-Contains $deposit 'if \(!isfinite\(value\) \|\| !isfinite\(scale\) \|\| scale <= 0\.0\)[\s\S]*return 0' 'Non-finite or invalid quantization inputs must fail closed.'
Assert-Contains $deposit 'style\.Temperature[\s\S]*style\.Buoyancy' 'Grid deposition must retain thermal buoyancy while freeing a dynamics moment.'
Assert-Contains $deposit 'gSmokeGridDeposit3\[cellIndex\]\.y[\s\S]*style\.Turbulence' 'Grid deposition must persist a mass-weighted turbulence moment.'
Assert-Contains $deposit 'gSmokeGridDeposit3\[cellIndex\]\.w[\s\S]*style\.TurbulenceScale' 'Grid deposition must persist a mass-weighted world scale.'
Assert-Contains $resolve 'velocity\.w \+= \(float\)q3\.w \* inverseMassQ' 'Turbulence scale must resolve as an additive mass moment.'
Assert-Contains $advect 'styleTurbulence = max\(dynamics\.z \* inverseMass' 'Velocity advection must recover the deposited turbulence strength.'
Assert-Contains $advect 'styleTurbulenceScale = max\(abs\(SmokeSourceFinite\(velocity\.w \* inverseMass' 'Velocity advection must recover a finite mass-normalized world-space turbulence scale.'
Assert-Contains $advect 'SmokeSourceWorldCurl\(worldPosition, styleTurbulenceScale,[\s\S]*CurlTime,[\s\S]*CurlEvolution\)' 'Turbulence forcing must use simulation-time phase without render-frame state.'
Assert-Contains $advect 'SmokeSourceLimitVelocity\(advectedVelocity, gSmokeGridConstants\.MaxVelocity\)' 'Post-force grid velocity must retain its CFL safety bound.'
Assert-Contains $advect 'scaleMoment = mass > 1e-8[\s\S]*exp\(-densityRate \* deltaTime\)' 'The turbulence-scale moment must decay with the transported smoke mass.'
Assert-Contains $advect 'SmokeGridLoadCellVelocity[\s\S]*const float3 omega = float3[\s\S]*gSmokeGridVorticity\[cellIndex\] = vorticity' 'Velocity advection must derive and publish centered velocity curl.'
Assert-Contains $advectFields 'localMass > max\(gSmokeGridConstants\.ActiveThreshold, 1e-8\)' 'Confinement must remain gated to occupied smoke cells.'
Assert-Contains $advectFields 'SmokeGridLoadCellVorticityMagnitude[\s\S]*const float3 gradient' 'Confinement must derive the gradient of neighboring vorticity magnitude.'
Assert-Contains $advectFields 'cross\(normal, omega\)[\s\S]*VorticityConfinement[\s\S]*deltaTime' 'Confinement must apply the cell-size-aware N cross omega force.'
Assert-Contains $advectFields 'VorticityClamps[\s\S]*SmokeSourceLimitVelocity' 'Post-confinement velocity must retain an explicit clamp counter and safety bound.'
if ($source -match 'gSmoke\w*Constants\.(Camera|Frame)|CameraPosition|FrameIndex') {
    throw 'Shared source shaping must not depend on camera or render-frame state.'
}

function Convert-ToUInt32([long]$Value) {
    return [uint32]($Value -band 0xffffffffL)
}

function Get-SmokeHash([uint32]$Value) {
    $value64 = [long]$Value
    $value64 = Convert-ToUInt32 ($value64 -bxor ($value64 -shr 16))
    $value64 = Convert-ToUInt32 ([long]$value64 * 0x7feb352dL)
    $value64 = Convert-ToUInt32 ($value64 -bxor ($value64 -shr 15))
    $value64 = Convert-ToUInt32 ([long]$value64 * 0x846ca68bL)
    return Convert-ToUInt32 ($value64 -bxor ($value64 -shr 16))
}

function Get-Random01([ref]$State) {
    $null = ($State.Value = Get-SmokeHash ([uint32]$State.Value))
    return ([double](([uint32]$State.Value) -band 0x00ffffff)) / 16777216.0
}

function Get-Dot([double[]]$A, [double[]]$B) {
    return $A[0] * $B[0] + $A[1] * $B[1] + $A[2] * $B[2]
}

function Get-Length([double[]]$Value) {
    return [math]::Sqrt((Get-Dot $Value $Value))
}

function Get-Normalized([double[]]$Value) {
    $length = Get-Length $Value
    if ($length -le 1e-12) { return @(0.0, -1.0, 0.0) }
    return @(
        ($Value[0] / $length),
        ($Value[1] / $length),
        ($Value[2] / $length)
    )
}

function Get-Cross([double[]]$A, [double[]]$B) {
    return @(
        ($A[1] * $B[2] - $A[2] * $B[1]),
        ($A[2] * $B[0] - $A[0] * $B[2]),
        ($A[0] * $B[1] - $A[1] * $B[0])
    )
}

function Get-RandomDirection([ref]$State) {
    [double]$z = [double](Get-Random01 $State) * 2.0 - 1.0
    [double]$phi = [double](Get-Random01 $State) * 2.0 * [math]::PI
    [double]$radius = [math]::Sqrt([math]::Max(0.0, 1.0 - $z * $z))
    [double]$x = $radius * [math]::Cos($phi)
    [double]$y = $radius * [math]::Sin($phi)
    return @($x, $y, $z)
}

function Get-ConeDirection([double[]]$Velocity, [double]$ConeDegrees,
    [double[]]$Fallback, [ref]$State) {
    $lengthSquared = Get-Dot $Velocity $Velocity
    if ($lengthSquared -le 1e-8) { return $Fallback }
    $axis = Get-Normalized $Velocity
    $clampedDegrees = [math]::Max(0.0, [math]::Min(180.0, $ConeDegrees))
    $coneCosine = [math]::Cos($clampedDegrees * [math]::PI / 180.0)
    $cosTheta = 1.0 + ($coneCosine - 1.0) * (Get-Random01 $State)
    $sinTheta = [math]::Sqrt([math]::Max(0.0, 1.0 - $cosTheta * $cosTheta))
    $phi = (Get-Random01 $State) * 2.0 * [math]::PI
    $reference = $(if ([math]::Abs($axis[2]) -lt 0.999) { @(0.0, 0.0, 1.0) } else { @(0.0, 1.0, 0.0) })
    $tangent = Get-Normalized (Get-Cross $reference $axis)
    $bitangent = Get-Cross $axis $tangent
    return @(
        ($axis[0] * $cosTheta + ($tangent[0] * [math]::Cos($phi) + $bitangent[0] * [math]::Sin($phi)) * $sinTheta),
        ($axis[1] * $cosTheta + ($tangent[1] * [math]::Cos($phi) + $bitangent[1] * [math]::Sin($phi)) * $sinTheta),
        ($axis[2] * $cosTheta + ($tangent[2] * [math]::Cos($phi) + $bitangent[2] * [math]::Sin($phi)) * $sinTheta)
    )
}

function Get-CellSeed([uint32]$Serial, [int[]]$Cell) {
    $seed = (Get-SmokeHash ($Serial -bxor 0x9e3779b9))
    $seed = $seed -bxor (Get-SmokeHash (Convert-ToUInt32 ([long]$Cell[0] + 0x85ebca6bL)))
    $seed = $seed -bxor (Get-SmokeHash (Convert-ToUInt32 ([long]$Cell[1] + 0xc2b2ae35L)))
    $seed = $seed -bxor (Get-SmokeHash (Convert-ToUInt32 ([long]$Cell[2] + 0x27d4eb2fL)))
    return Get-SmokeHash ([uint32]$seed)
}

$seedA = Get-CellSeed 77 @(12, -4, 9)
$seedB = Get-CellSeed 77 @(12, -4, 9)
$seedNeighbor = Get-CellSeed 77 @(13, -4, 9)
if ($seedA -ne $seedB) { throw 'Identical command/cell source keys were not deterministic.' }
if ($seedA -eq $seedNeighbor) { throw 'Neighboring grid cells collapsed to one source-shaping key.' }

$state = [uint32]$seedA
$fallback = Get-RandomDirection ([ref]$state)
$cone = Get-ConeDirection @(12.0, -3.0, 4.0) 30.0 $fallback ([ref]$state)
$axis = Get-Normalized @(12.0, -3.0, 4.0)
Assert-Near (Get-Length $cone) 1.0 1e-12 'Cone shaping must return a unit direction.'
if ((Get-Dot $cone $axis) -lt [math]::Cos(30.0 * [math]::PI / 180.0) - 1e-12) {
    throw 'Cone shaping escaped the authored solid angle.'
}

$zeroState = [uint32]$seedA
$zeroFallback = Get-RandomDirection ([ref]$zeroState)
$stateBeforeZeroCone = $zeroState
$zeroDirection = Get-ConeDirection @(0.0, 0.0, 0.0) 90.0 $zeroFallback ([ref]$zeroState)
if ($zeroState -ne $stateBeforeZeroCone) { throw 'Axisless cone fallback consumed unexpected random samples.' }
foreach ($axisIndex in 0..2) {
    Assert-Near $zeroDirection[$axisIndex] $zeroFallback[$axisIndex] 0.0 'Axisless cone fallback changed the spherical direction.'
}

$radial = Get-Normalized @(3.0, 4.0, 0.0)
Assert-Near ($radial[0] * 8.0) 4.8 1e-12 'Expansion momentum must point radially from the source center.'
Assert-Near ($radial[1] * 8.0) 6.4 1e-12 'Expansion momentum must preserve authored magnitude.'

function Get-WorldCurl([double[]]$Position, [double]$Scale, [double]$SimulationTime, [double]$EvolutionRate) {
    $scale = [math]::Max([math]::Abs($Scale), 0.0001)
    $q = @(
        ($Position[0] / $scale),
        ($Position[1] / $scale),
        ($Position[2] / $scale)
    )
    $phase = $SimulationTime * [math]::Max($EvolutionRate, 0.0)
    return @(
        (([math]::Cos($q[1] + 1.17 + $phase * 0.73) - [math]::Sin($q[2] + 2.03 - $phase * 1.11)) * 0.28867513459),
        (([math]::Cos($q[2] + 2.71 + $phase * 0.91) - [math]::Sin($q[0] + 0.43 + $phase * 0.67)) * 0.28867513459),
        (([math]::Cos($q[0] + 4.11 - $phase * 0.79) - [math]::Sin($q[1] + 5.37 + $phase * 1.03)) * 0.28867513459)
    )
}

$position = @(31.25, -8.5, 101.75)
$curlA = Get-WorldCurl $position 32.0 0.0 0.0
$curlB = Get-WorldCurl $position 32.0 0.0 0.0
foreach ($axisIndex in 0..2) { Assert-Near $curlA[$axisIndex] $curlB[$axisIndex] 0.0 'World curl was not deterministic.' }
if ((Get-Length $curlA) -gt 1.0 + 1e-12) { throw 'World curl escaped its unit acceleration bound.' }
$curlEvolved = Get-WorldCurl $position 32.0 1.0 0.7
if ((Get-Length @(
    ($curlEvolved[0] - $curlA[0]),
    ($curlEvolved[1] - $curlA[1]),
    ($curlEvolved[2] - $curlA[2]))) -le 1e-6) { throw 'Nonzero evolution failed to change the curl phase.' }
if ((Get-Length $curlEvolved) -gt 1.0 + 1e-12) { throw 'Evolved world curl escaped its unit acceleration bound.' }

$epsilon = 0.001
$divergence = 0.0
foreach ($axisIndex in 0..2) {
    $positive = @($position[0], $position[1], $position[2])
    $negative = @($position[0], $position[1], $position[2])
    $positive[$axisIndex] += $epsilon
    $negative[$axisIndex] -= $epsilon
    $divergence += ((Get-WorldCurl $positive 32.0 2.0 0.7)[$axisIndex] -
        (Get-WorldCurl $negative 32.0 2.0 0.7)[$axisIndex]) / (2.0 * $epsilon)
}
Assert-Near $divergence 0.0 1e-10 'Analytic world turbulence must remain divergence-free.'

function Convert-ToGridQuantized([double]$Value, [double]$Scale) {
    if ([double]::IsNaN($Value) -or [double]::IsInfinity($Value) -or
        [double]::IsNaN($Scale) -or [double]::IsInfinity($Scale) -or $Scale -le 0.0) { return 0 }
    $scaled = [math]::Max(-1073741824.0, [math]::Min(1073741824.0, $Value * $Scale))
    return [int64][math]::Round($scaled)
}

if ((Convert-ToGridQuantized ([double]::NaN) 1024.0) -ne 0) { throw 'NaN deposition did not fail closed.' }
if ((Convert-ToGridQuantized 1.0 0.0) -ne 0) { throw 'Invalid quantization scale did not fail closed.' }
if ([math]::Abs((Convert-ToGridQuantized 1e100 1024.0)) -gt 1073741824) { throw 'Deposition quantization escaped its signed safety bound.' }

Write-Host 'Smoke grid source-shaping structural and CPU-mirror validation passed.'
