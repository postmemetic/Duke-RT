$ErrorActionPreference = 'Stop'

# CPU reference for the independently implemented HLSL field. This validates
# determinism, smooth lattice continuity, choppy shaping, and the phase-gradient
# approximation without requiring a renderer launch. The structural contract
# test keeps this helper's operations tied to FluidSurfaceD3D.hlsli.

$u32Mask = [uint64]4294967295

function ConvertTo-U32([uint64]$Value) {
    return [uint32]($Value -band $u32Mask)
}

function Get-SignedBits([int]$Value) {
    return [BitConverter]::ToUInt32([BitConverter]::GetBytes($Value), 0)
}

function Get-HashU32([uint32]$Value) {
    [uint64]$v = $Value
    $v = ($v -bxor ($v -shr 16)) -band $u32Mask
    $v = ($v * [uint64]2146121005) -band $u32Mask
    $v = ($v -bxor ($v -shr 15)) -band $u32Mask
    $v = ($v * [uint64]2221713035) -band $u32Mask
    $v = ($v -bxor ($v -shr 16)) -band $u32Mask
    return [uint32]$v
}

function Get-Hash01([uint32]$Value) {
    $hash = Get-HashU32 $Value
    return [double]($hash -band 0x00ffffff) / 16777216.0
}

function Get-CellHash([int]$X, [int]$Y, [uint32]$Seed) {
    [uint64]$xTerm = ([uint64](Get-SignedBits $X) * [uint64]2376512323) -band $u32Mask
    [uint64]$yTerm = ([uint64](Get-SignedBits $Y) * [uint64]3625334849) -band $u32Mask
    $mixed = ConvertTo-U32 (([uint64]$Seed -bxor $xTerm -bxor $yTerm) -band $u32Mask)
    $hash = Get-HashU32 $mixed
    return [double]($hash -band 0x00ffffff) * (2.0 / 16777216.0) - 1.0
}

function Get-ValueNoiseDerivative([double]$X, [double]$Y, [uint32]$Seed) {
    $cellX = [Math]::Floor($X)
    $cellY = [Math]::Floor($Y)
    $fx = $X - $cellX
    $fy = $Y - $cellY
    $ux = $fx * $fx * (3.0 - 2.0 * $fx)
    $uy = $fy * $fy * (3.0 - 2.0 * $fy)
    $dux = 6.0 * $fx * (1.0 - $fx)
    $duy = 6.0 * $fy * (1.0 - $fy)
    $h00 = Get-CellHash $cellX $cellY $Seed
    $h10 = Get-CellHash ($cellX + 1) $cellY $Seed
    $h01 = Get-CellHash $cellX ($cellY + 1) $Seed
    $h11 = Get-CellHash ($cellX + 1) ($cellY + 1) $Seed
    $row0 = $h00 + ($h10 - $h00) * $ux
    $row1 = $h01 + ($h11 - $h01) * $ux
    return [pscustomobject]@{
        Value = $row0 + ($row1 - $row0) * $uy
        Dx = $dux * (($h10 - $h00) + (($h11 - $h01) - ($h10 - $h00)) * $uy)
        Dy = $duy * ($row1 - $row0)
    }
}

function Get-FluidField(
    [double]$X,
    [double]$Y,
    [double]$Time,
    [uint32]$Seed,
    [double]$Choppiness = 3.6,
    [double]$Footprint = 0.0,
    [double]$Warp = 1.05) {
    $frequency = 2.0
    $lacunarity = 1.9
    $gain = 0.28
    $anisotropy = 1.0
    $noiseScale = 0.65
    $choppyRelaxation = 0.2
    $rotation = 37.0 * [Math]::PI / 180.0
    $rotationCosine = [Math]::Cos($rotation)
    $rotationSine = [Math]::Sin($rotation)
    $initialHashInput = ConvertTo-U32 (([uint64]$Seed -bxor [uint64]1327217884) -band $u32Mask)
    $initialAngle = 2.0 * [Math]::PI * (Get-Hash01 $initialHashInput)
    $directionX = [Math]::Cos($initialAngle)
    $directionY = [Math]::Sin($initialAngle)
    $amplitudeWeight = 1.0
    $height = 0.0
    $gradientX = 0.0
    $gradientY = 0.0
    $weightSum = 0.0

    for ($layer = 0; $layer -lt 4; ++$layer) {
        $layerSeed = ConvertTo-U32 ([uint64]$Seed + [uint64]$layer * [uint64]2654435769)
        $crossX = -$directionY
        $crossY = $directionX
        $layerX = ($X * $directionX + $Y * $directionY) * $anisotropy
        $layerY = $X * $crossX + $Y * $crossY
        $layerTime = $Time * 0.5 * (1.0 + 0.17 * $layer)
        $noise = Get-ValueNoiseDerivative `
            ($layerX * $frequency * $noiseScale + $layerTime * 0.13) `
            ($layerY * $frequency * $noiseScale - $layerTime * 0.09) `
            $layerSeed
        $phaseHashInput = ConvertTo-U32 (([uint64]$layerSeed -bxor [uint64]2769414579) -band $u32Mask)
        $phase = $layerX * $frequency + $Warp * $noise.Value + $layerTime + `
            2.0 * [Math]::PI * (Get-Hash01 $phaseHashInput)
        $sine = [Math]::Sin($phase)
        $cosine = [Math]::Cos($phase)
        $choppyAmount = [Math]::Max(0.0, [Math]::Min(1.0, ($Choppiness - 1.0) / 3.0))
        $normalization = 1.0 + 0.5 * $choppyAmount
        $wave = ($sine - 0.5 * $choppyAmount * ($cosine * $cosine - $sine * $sine)) / $normalization
        $waveDerivative = ($cosine + $choppyAmount * (2.0 * $sine * $cosine)) / $normalization
        $phaseDomainX = $frequency * $directionX * $anisotropy
        $phaseDomainY = $frequency * $directionY * $anisotropy
        $gradientBound = $frequency * [Math]::Max($anisotropy, 1.0) * (1.0 + 3.0 * $Warp * $noiseScale)
        $filterWidth = $Footprint * (1.0 / 64.0) * $gradientBound
        $filteredWeight = $amplitudeWeight / (1.0 + $filterWidth * $filterWidth)
        $height += $filteredWeight * $wave
        $gradientX += $filteredWeight * $waveDerivative * $phaseDomainX
        $gradientY += $filteredWeight * $waveDerivative * $phaseDomainY
        $weightSum += $filteredWeight
        $frequency *= $lacunarity
        $amplitudeWeight *= $gain
        $Choppiness += (1.0 - $Choppiness) * $choppyRelaxation
        $nextDirectionX = $rotationCosine * $directionX - $rotationSine * $directionY
        $nextDirectionY = $rotationSine * $directionX + $rotationCosine * $directionY
        $directionX = $nextDirectionX
        $directionY = $nextDirectionY
    }

    return [pscustomobject]@{
        Height = $height / $weightSum
        GradientX = $gradientX / $weightSum
        GradientY = $gradientY / $weightSum
    }
}

function Assert-Near([double]$Actual, [double]$Expected, [double]$Tolerance, [string]$Message) {
    if ([double]::IsNaN($Actual) -or [double]::IsInfinity($Actual) -or
        [Math]::Abs($Actual - $Expected) -gt $Tolerance) {
        throw "$Message actual=$Actual expected=$Expected tolerance=$Tolerance"
    }
}

$sample = Get-FluidField 1.234 -2.345 7.25 13
$repeat = Get-FluidField 1.234 -2.345 7.25 13
Assert-Near $sample.Height $repeat.Height 0.0 'Fixed field inputs are not deterministic.'
Assert-Near $sample.GradientX $repeat.GradientX 0.0 'Fixed X gradient is not deterministic.'
Assert-Near $sample.GradientY $repeat.GradientY 0.0 'Fixed Y gradient is not deterministic.'

$differentSeed = Get-FluidField 1.234 -2.345 7.25 14
if ([Math]::Abs($sample.Height - $differentSeed.Height) -lt 1.0e-4) {
    throw 'Changing the stable profile seed did not change the field.'
}
$differentTime = Get-FluidField 1.234 -2.345 7.75 13
if ([Math]::Abs($sample.Height - $differentTime.Height) -lt 1.0e-4) {
    throw 'Advancing gameplay time did not animate the field.'
}
$sineShape = Get-FluidField 1.234 -2.345 7.25 13 1.0
if ([Math]::Abs($sample.Height - $sineShape.Height) -lt 1.0e-4) {
    throw 'Choppiness did not change the evaluated height field.'
}

$step = 1.0e-5
$noWarp = Get-FluidField 1.234 -2.345 7.25 13 -Warp 0.0
$plusX = Get-FluidField (1.234 + $step) -2.345 7.25 13 -Warp 0.0
$minusX = Get-FluidField (1.234 - $step) -2.345 7.25 13 -Warp 0.0
$plusY = Get-FluidField 1.234 (-2.345 + $step) 7.25 13 -Warp 0.0
$minusY = Get-FluidField 1.234 (-2.345 - $step) 7.25 13 -Warp 0.0
$finiteDx = ($plusX.Height - $minusX.Height) / (2.0 * $step)
$finiteDy = ($plusY.Height - $minusY.Height) / (2.0 * $step)
Assert-Near $noWarp.GradientX $finiteDx 2.0e-4 'Phase X gradient disagrees with centered finite differences when warp is disabled.'
Assert-Near $noWarp.GradientY $finiteDy 2.0e-4 'Phase Y gradient disagrees with centered finite differences when warp is disabled.'

$noiseLeft = Get-ValueNoiseDerivative (1.0 - 1.0e-6) 0.37 13
$noiseRight = Get-ValueNoiseDerivative (1.0 + 1.0e-6) 0.37 13
Assert-Near $noiseLeft.Value $noiseRight.Value 1.0e-5 'Value noise is discontinuous at a lattice boundary.'
Assert-Near $noiseLeft.Dx $noiseRight.Dx 1.0e-4 'Value-noise X derivative is discontinuous at a lattice boundary.'
Assert-Near $noiseLeft.Dy $noiseRight.Dy 1.0e-4 'Value-noise Y derivative is discontinuous at a lattice boundary.'

foreach ($field in @($sample.Height, $sample.GradientX, $sample.GradientY)) {
    if ([double]::IsNaN($field) -or [double]::IsInfinity($field)) {
        throw 'Reference field produced a non-finite result.'
    }
}
if ([Math]::Abs($sample.Height) -gt 1.000001) {
    throw "Normalized field height escaped its authored bound: $($sample.Height)"
}

Write-Host 'Seascape-style fluid field numeric reference validation passed.'
