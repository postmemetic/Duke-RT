param(
	[string]$VsDevCmd,
	[string]$VCToolsVersion
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$sceneDir = Join-Path $repoRoot 'source\common\rendering\nri\scene'
$testSource = Join-Path $PSScriptRoot 'nri_wall_decal_projection.tests.cpp'
$outputDir = Join-Path $repoRoot 'build\wall-decal-projection-tests'
$testExe = Join-Path $outputDir 'nri_wall_decal_projection.tests.exe'
$testObject = Join-Path $outputDir 'nri_wall_decal_projection.tests.obj'

if (-not $VsDevCmd)
{
	$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
	if (-not (Test-Path -LiteralPath $vswhere))
	{
		throw 'Visual Studio discovery is unavailable. Pass -VsDevCmd with the developer environment script path.'
	}
	$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
	if (-not $installation) { throw 'Visual Studio with the C++ toolchain was not found.' }
	$VsDevCmd = Join-Path $installation 'Common7\Tools\VsDevCmd.bat'
}
if (-not (Test-Path -LiteralPath $VsDevCmd)) { throw "Developer environment script not found: $VsDevCmd" }
if ($VCToolsVersion -and $VCToolsVersion -notmatch '^\d+(\.\d+)*$')
{
	throw 'VCToolsVersion must contain only numeric version components.'
}
$versionArgument = if ($VCToolsVersion) { ' -vcvars_ver=' + $VCToolsVersion } else { '' }
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
$compile = 'call "{0}" -arch=x64 -host_arch=x64{5} >nul && cl /nologo /std:c++17 /EHsc /W4 /WX /I"{1}" /Fo"{4}" "{2}" /Fe:"{3}"' -f `
	$VsDevCmd, $sceneDir, $testSource, $testExe, $testObject, $versionArgument
cmd /c $compile
if ($LASTEXITCODE -ne 0) { throw "Wall decal projection test compilation failed with exit code $LASTEXITCODE." }
& $testExe
if ($LASTEXITCODE -ne 0) { throw "Wall decal projection tests failed with exit code $LASTEXITCODE." }
Write-Host 'Wall decal projection tests passed.'
