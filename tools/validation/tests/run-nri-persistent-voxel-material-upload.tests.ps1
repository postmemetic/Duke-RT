$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$sourceDir = Join-Path $repoRoot 'source\common\rendering\nri\renderer'
$testSource = Join-Path $PSScriptRoot 'nri_persistent_voxel_material_upload.tests.cpp'
$uploadSource = Join-Path $sourceDir 'nri_persistent_voxel_material_upload.cpp'
$outputDir = Join-Path $repoRoot 'build\persistent-voxel-material-upload-tests'
$testExe = Join-Path $outputDir 'nri_persistent_voxel_material_upload.tests.exe'
$vsDevCmd = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat'

New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
$compile = 'call "{0}" -arch=x64 -host_arch=x64 >nul && cl /nologo /std:c++17 /EHsc /permissive- /W4 /WX /I"{1}" /Fo"{5}/" "{2}" "{3}" /Fe:"{4}"' -f `
	$vsDevCmd, $sourceDir, $uploadSource, $testSource, $testExe, $outputDir
cmd /c $compile
if ($LASTEXITCODE -ne 0)
{
	throw "Persistent voxel material upload test compilation failed with exit code $LASTEXITCODE."
}

& $testExe
if ($LASTEXITCODE -ne 0)
{
	throw "Persistent voxel material upload tests failed with exit code $LASTEXITCODE."
}
