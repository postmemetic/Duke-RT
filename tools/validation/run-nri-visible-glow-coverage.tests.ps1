param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$testPath = Join-Path $PSScriptRoot 'tests\nri_visible_glow_coverage.tests.ps1'
& $testPath
