$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

& (Join-Path $PSScriptRoot 'nri_palette_residency_contract.tests.ps1')
if (-not $?) {
	exit 1
}
