<#
.SYNOPSIS
Build and package Linux Duke-RT through WSL (Windows PowerShell 5.1+).
.DESCRIPTION
Forwards to Build-LinuxReleasePackage.sh, the shared native Linux build owner.
Path parameters accept Windows paths or absolute Linux paths in the selected
distribution. CMake must be a Linux executable or a command on its PATH.
See LINUX.md for compiler/system packages, ZMusic and generated NRD prerequisites.
.EXAMPLE
.\tools\dist\Build-LinuxReleasePackage.ps1 -Distribution Ubuntu -NrdShaderHeaderDir M:\Raze\libraries\NRD\_Shaders
#>
[CmdletBinding()]
param(
    [string]$Distribution = '',
    [string]$BuildRoot = '',
    [string]$ZMusicSourceDir = '',
    [string]$NrdShaderHeaderDir = '',
    [string]$CMake = 'cmake',
    [ValidateRange(1, 1024)][int]$Jobs = 2,
    [string]$OutputPath = '',
    [switch]$SkipBuild,
    [switch]$Help
)

$ErrorActionPreference = 'Stop'
$wsl = (Get-Command wsl.exe -ErrorAction Stop).Source
$wslOptions = @()
if ($Distribution) { $wslOptions += @('--distribution', $Distribution) }

function Convert-ToLinuxPath {
    param([string]$Path)
    if ($Path.StartsWith('/')) { return $Path }
    $absolute = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
    $converted = & $wsl @wslOptions --exec wslpath -a -u $absolute
    if ($LASTEXITCODE -ne 0) { throw "Could not convert path for WSL: $Path" }
    return ($converted -join "`n").TrimEnd("`r", "`n")
}

$script = Convert-ToLinuxPath (Join-Path $PSScriptRoot 'Build-LinuxReleasePackage.sh')
$arguments = @('--exec', 'bash', $script)
if ($Help) {
    $arguments += '--help'
} else {
    foreach ($option in @(
        @('--build-root', $BuildRoot),
        @('--zmusic-source', $ZMusicSourceDir),
        @('--nrd-shader-headers', $NrdShaderHeaderDir),
        @('--output', $OutputPath)
    )) {
        if ($option[1]) { $arguments += @($option[0], (Convert-ToLinuxPath $option[1])) }
    }
    $linuxCMake = $CMake
    if ($CMake.Contains('\') -or $CMake.Contains(':')) { $linuxCMake = Convert-ToLinuxPath $CMake }
    $arguments += @('--cmake', $linuxCMake, '--jobs', [string]$Jobs)
    if ($SkipBuild) { $arguments += '--skip-build' }
}

& $wsl @wslOptions @arguments
exit $LASTEXITCODE
