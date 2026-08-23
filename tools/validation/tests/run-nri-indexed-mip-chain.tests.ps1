$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$sceneDir = Join-Path $repoRoot 'source\common\rendering\nri\scene'
$source = Join-Path $sceneDir 'nri_indexed_mip_chain.cpp'
$header = Join-Path $sceneDir 'nri_indexed_mip_chain.h'
$test = Join-Path $PSScriptRoot 'nri_indexed_mip_chain.tests.cpp'
$outputDir = Join-Path $repoRoot 'build\validation-tests\nri-indexed-mip-chain'
$testExe = Join-Path $outputDir 'nri_indexed_mip_chain.tests.exe'
$vsDevCmd = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat'

foreach ($requiredPath in @($source, $header, $test)) {
	if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
		throw "Required indexed mip-chain test input is missing: $requiredPath"
	}
}

New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
$command = 'call "{0}" -arch=x64 -host_arch=x64 >nul && cl /nologo /std:c++17 /EHsc /W4 /WX /permissive- /I"{1}" "{2}" "{3}" /Fe:"{4}" && "{4}"' -f `
	$vsDevCmd, $sceneDir, $source, $test, $testExe
cmd /c $command
if ($LASTEXITCODE -ne 0) {
	throw "NRI indexed mip-chain tests failed with exit code $LASTEXITCODE."
}

Write-Host 'NRI indexed mip-chain tests passed.'
