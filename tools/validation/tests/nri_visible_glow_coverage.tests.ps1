Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$shaderPath = Join-Path $repoRoot 'source\common\rendering\nri\shaders\TraceOpaque.cs.hlsl'
$shader = Get-Content -LiteralPath $shaderPath -Raw

function Assert-True([bool]$Condition, [string]$Message)
{
	if (-not $Condition) { throw $Message }
}

function Assert-VectorNear([double[]]$Actual, [double[]]$Expected, [string]$Message)
{
	for ($channel = 0; $channel -lt 3; ++$channel)
	{
		if ([Math]::Abs($Actual[$channel] - $Expected[$channel]) -gt 1e-9)
		{
			throw "$Message Channel $channel was $($Actual[$channel]); expected $($Expected[$channel])."
		}
	}
}

function Invoke-OverlayBlend([double[]]$Target, [double[]]$Blend)
{
	$result = [double[]]::new(3)
	for ($channel = 0; $channel -lt 3; ++$channel)
	{
		$targetValue = [Math]::Min([Math]::Max($Target[$channel], 0.0), 1.0)
		$blendValue = [Math]::Min([Math]::Max($Blend[$channel], 0.0), 1.0)
		$result[$channel] = if ($targetValue -ge 0.5)
		{
			1.0 - 2.0 * (1.0 - $targetValue) * (1.0 - $blendValue)
		}
		else
		{
			(2.0 * $targetValue) * $blendValue
		}
	}
	return $result
}

function Get-GlowCoverage([double[]]$Glow)
{
	return [double]([Math]::Min(
		[Math]::Max([Math]::Max($Glow[0], [Math]::Max($Glow[1], $Glow[2])), 0.0),
		1.0))
}

function Invoke-VisibleGlowEmission(
	[double[]]$Albedo,
	[double[]]$Glow,
	[double]$EmissiveIntensity = 1.0,
	[double]$VisibleBlend = 1.0,
	[double]$MaterialResponseScale = 1.0)
{
	[double[]]$overlay = @(Invoke-OverlayBlend $Albedo $Glow)
	[double]$scale = [double](Get-GlowCoverage $Glow) * $EmissiveIntensity * $VisibleBlend * $MaterialResponseScale
	[double]$red = $overlay[0] * $scale
	[double]$green = $overlay[1] * $scale
	[double]$blue = $overlay[2] * $scale
	return [double[]]@($red, $green, $blue)
}

$functionStart = $shader.IndexOf(
	'float3 EvaluateVisibleMaterialEmission(uint materialIndex, uint dataSource, uint primitiveIndex')
$functionEnd = $shader.IndexOf('uint GetEmissivePrimitiveCount()', $functionStart)
Assert-True ($functionStart -ge 0 -and $functionEnd -gt $functionStart) `
	'Could not isolate EvaluateVisibleMaterialEmission from TraceOpaque.'
$visibleEmission = $shader.Substring($functionStart, $functionEnd - $functionStart)

$coverageDeclaration = 'const\s+float\s+glowCoverage\s*=\s*saturate\(max\(glow\.r,\s*max\(glow\.g,\s*glow\.b\)\)\);'
$coveredReturn = 'return\s+OverlayBlend\(albedo,\s*glow\)\s*\*\s*glowCoverage\s*\*\s*material\.emissiveIntensity\s*\*\s*visibleBlend\s*\*\s*materialResponseScale\s*;'
Assert-True ($visibleEmission -match $coverageDeclaration) `
	'Visible mode-3 glow emission must derive coverage from the saturated maximum raw glow channel.'
Assert-True ($visibleEmission -match $coveredReturn) `
	'Visible mode-3 glow emission must apply raw glow coverage to OverlayBlend before all material scales.'
Assert-True ($visibleEmission.IndexOf('glowCoverage') -gt $visibleEmission.IndexOf('const float3 glow =')) `
	'Glow coverage must be computed from the sampled glow payload.'

$black = [double[]]@(0.0, 0.0, 0.0)
$brightAlbedo = [double[]]@(0.9, 0.7, 0.8)
$legacyLeak = Invoke-OverlayBlend $brightAlbedo $black
Assert-True (($legacyLeak[0] + $legacyLeak[1] + $legacyLeak[2]) -gt 0.0) `
	'The regression fixture must exercise the old high-albedo OverlayBlend leak.'
Assert-VectorNear (Invoke-VisibleGlowEmission $brightAlbedo $black) $black `
	'An exactly black glow texel must be absorbing even for high albedo.'
Assert-VectorNear (Invoke-VisibleGlowEmission ([double[]]@(0.2, 0.3, 0.4)) $black) $black `
	'An exactly black glow texel must remain absorbing for low albedo.'

$coloredGlow = [double[]]@(0.25, 0.5, 0.1)
$coloredAlbedo = [double[]]@(0.8, 0.3, 0.6)
[double[]]$overlay = @(Invoke-OverlayBlend $coloredAlbedo $coloredGlow)
$expectedRed = $overlay[0] * 0.5
$expectedGreen = $overlay[1] * 0.5
$expectedBlue = $overlay[2] * 0.5
$expected = [double[]]@($expectedRed, $expectedGreen, $expectedBlue)
Assert-VectorNear (Invoke-VisibleGlowEmission $coloredAlbedo $coloredGlow) $expected `
	'Nonblack glow must retain OverlayBlend while scaling by maximum-channel coverage.'
Assert-True ((Get-GlowCoverage ([double[]]@(0.2, 0.7, 0.3))) -eq 0.7) `
	'Glow coverage must follow the maximum RGB channel.'
Assert-True ((Get-GlowCoverage ([double[]]@(2.0, 0.1, 0.0))) -eq 1.0) `
	'Glow coverage must saturate values above one.'
Assert-VectorNear (Invoke-VisibleGlowEmission $coloredAlbedo $coloredGlow 4.0 0.0 2.0) $black `
	'A zero visible-blend scale must still suppress visible glow emission.'

Write-Output 'NRI visible glow coverage source and math contracts passed.'
