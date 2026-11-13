param(
    [string]$ConfigPath = "",
    [string]$VsDevCmd = "",
    [string]$VcVarsVersion = "",
    [string]$ToolchainFile = "",
    [string]$VcpkgOverlayPorts = "",
    [string]$VcpkgRoot = "",
    [string]$VcpkgInstalledDir = "",
    [string]$ZMusicSourceDir = "",
    [string]$ZMusicBuildDir = "",
    [string]$ZMusicIncludeDir = "",
    [string]$RuntimeDllDir = "",
    [string]$NrdShaderHeaderDir = "",
    [string]$NriDependencySourceDir = "",
    # Accepted for compatibility with older local settings; no longer consumed.
    [string]$NriRuntimeDir = "",
    [string]$FfxSdkRoot = "",
    [string]$DxcExecutable = "",
    [string]$CMakeExecutable = "cmake",
    [string]$RazeBuildDir = "",
    [string]$PackageDir = "",
    [string]$ZipPath = "",
    [ValidateRange(1, 128)][int]$Jobs = 8,
    [switch]$SkipBuild,
    [switch]$SkipZip
)

$ErrorActionPreference = "Stop"

function Write-Info {
    param([string]$Message)
    Write-Host "[release-package] $Message"
}

function Get-FullPathSafe {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Base,
        [Parameter(Mandatory = $true)]
        [string]$Child
    )

    $path = [System.IO.Path]::GetFullPath([System.IO.Path]::Combine($Base, $Child))
    if ($path.Length -gt [System.IO.Path]::GetPathRoot($path).Length) { $path = $path.TrimEnd('\') }
    return $path
}

function Ensure-WithinRoot {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RootPath,
        [Parameter(Mandatory = $true)]
        [string]$CandidatePath,
        [Parameter(Mandatory = $true)]
        [string]$Label
    )

    $root = [System.IO.Path]::GetFullPath($RootPath)
    $candidate = [System.IO.Path]::GetFullPath($CandidatePath)
    $rootWithSlash = $root.TrimEnd('\') + '\'
    if ($candidate -ne $root -and -not $candidate.StartsWith($rootWithSlash, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "$Label path '$candidate' is outside root '$root'."
    }
}

function Resolve-VsDevCmd {
    param([string]$RequestedPath)

    if ($RequestedPath) {
        $resolved = Get-FullPathSafe $repoRoot $RequestedPath
        if (-not (Test-Path -LiteralPath $resolved -PathType Leaf)) {
            throw "VsDevCmd was not found: $resolved"
        }
        return $resolved
    }

    if ($env:RAZE_VSDEVCMD) {
        return Resolve-VsDevCmd -RequestedPath $env:RAZE_VSDEVCMD
    }

    $vswhereCandidates = @()
    $vswhereCommand = Get-Command vswhere.exe -ErrorAction SilentlyContinue
    if ($vswhereCommand) {
        $vswhereCandidates += $vswhereCommand.Source
    }
    if (${env:ProgramFiles(x86)}) {
        $vswhereCandidates += (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe")
    }

    foreach ($vswhere in @($vswhereCandidates | Select-Object -Unique)) {
        if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
            continue
        }
        $installationPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($LASTEXITCODE -eq 0 -and $installationPath) {
            $candidate = Join-Path (@($installationPath)[0].Trim()) "Common7\Tools\VsDevCmd.bat"
            if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                return [System.IO.Path]::GetFullPath($candidate)
            }
        }
    }

    throw "Could not discover a Visual Studio installation with the x64 C++ tools. Pass -VsDevCmd or set RAZE_VSDEVCMD."
}

function Resolve-DxcExecutable {
    param(
        [string]$RequestedPath,
        [Parameter(Mandatory = $true)]
        [string]$RepositoryRoot
    )

    if ($RequestedPath) {
        $resolved = Get-FullPathSafe $RepositoryRoot $RequestedPath
        if (-not (Test-Path -LiteralPath $resolved -PathType Leaf)) {
            throw "DXC was not found: $resolved"
        }
        return $resolved
    }

    if ($env:RAZE_DXC_EXECUTABLE) {
        return Resolve-DxcExecutable -RequestedPath $env:RAZE_DXC_EXECUTABLE -RepositoryRoot $RepositoryRoot
    }

    if ($env:VULKAN_SDK) {
        foreach ($sdkSubdir in @("Bin", "Bin32")) {
            $sdkDxc = Join-Path $env:VULKAN_SDK "$sdkSubdir\dxc.exe"
            if (Test-Path -LiteralPath $sdkDxc -PathType Leaf) {
                return [System.IO.Path]::GetFullPath($sdkDxc)
            }
        }
    }

    $dxcCommand = Get-Command dxc.exe -ErrorAction SilentlyContinue
    if ($dxcCommand) {
        return [System.IO.Path]::GetFullPath($dxcCommand.Source)
    }

    $candidates = @()
    $portableRoots = Get-ChildItem -LiteralPath (Join-Path $RepositoryRoot "build") -Directory -Filter "dxc-*" -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending
    foreach ($portableRoot in $portableRoots) {
        $candidates += (Join-Path $portableRoot.FullName "bin\x64\dxc.exe")
    }
    $candidates += (Join-Path $RepositoryRoot "build\nrd-shaders\_deps\dxc-src\bin\x64\dxc.exe")

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return [System.IO.Path]::GetFullPath($candidate)
        }
    }

    throw "DXC was not found. Pass -DxcExecutable, set RAZE_DXC_EXECUTABLE, or install a portable DXC under build\dxc-<version>\bin\x64."
}

function Resolve-RuntimeDllDirectory {
    param(
        [string]$RequestedPath,
        [Parameter(Mandatory = $true)]
        [string]$RepositoryRoot,
        [Parameter(Mandatory = $true)]
        [string]$InstalledDirectory
    )

    if ($RequestedPath) {
        $resolved = Get-FullPathSafe $RepositoryRoot $RequestedPath
        if (-not (Test-Path -LiteralPath (Join-Path $resolved "OpenAL32.dll") -PathType Leaf)) {
            throw "Runtime DLL directory does not contain OpenAL32.dll: $resolved"
        }
        return $resolved
    }

    $directoryCandidates = @(
        (Join-Path $RepositoryRoot "bin\windows\runtime-deps"),
        (Join-Path $InstalledDirectory "x64-windows\bin"),
        (Join-Path $RepositoryRoot "build\vcpkg\packages\openal-soft_x64-windows\bin")
    )
    foreach ($candidate in $directoryCandidates) {
        if (Test-Path -LiteralPath (Join-Path $candidate "OpenAL32.dll") -PathType Leaf) {
            return [System.IO.Path]::GetFullPath($candidate)
        }
    }

    $buildTreeDll = Get-ChildItem -Path (Join-Path $RepositoryRoot "build\vcpkg\buildtrees\openal-soft\*-rel\OpenAL32.dll") -File -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if ($buildTreeDll) {
        return [System.IO.Path]::GetFullPath($buildTreeDll.DirectoryName)
    }

    throw "OpenAL32.dll was not found. Pass -RuntimeDllDir or install the dynamic x64-windows OpenAL package."
}

function Invoke-CMake {
    param([string[]]$Arguments)
    & $CMakeExecutable @Arguments
    if ($LASTEXITCODE -ne 0) { throw "CMake failed with exit code $LASTEXITCODE." }
}

function Assert-NoLinks {
    param([string]$Path)
    # Check existing ancestors as well as the target before creating/deleting outputs.
    for ($current = $Path; $current; $current = Split-Path -Parent $current) {
        if (Test-Path -LiteralPath $current) {
            if ((Get-Item -LiteralPath $current -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Output path must not traverse a link: $current"
            }
        }
    }
}

function Get-CMakeCache {
    param([string]$BuildDir)
    $cache = @{}
    $cachePath = Join-Path $BuildDir "CMakeCache.txt"
    if (Test-Path -LiteralPath $cachePath) {
        foreach ($line in Get-Content -LiteralPath $cachePath) {
            if ($line -match '^([^#/:][^:]*):[^=]+=(.*)$') { $cache[$Matches[1]] = $Matches[2] }
        }
    }
    return $cache
}

function Assert-CacheSource {
    param([string]$BuildDir, [string]$SourceDir)
    $cachePath = Join-Path $BuildDir "CMakeCache.txt"
    if (Test-Path -LiteralPath $cachePath) {
        $cache = Get-CMakeCache $BuildDir
        if (-not $cache.CMAKE_HOME_DIRECTORY -or
            (Get-FullPathSafe $SourceDir $cache.CMAKE_HOME_DIRECTORY) -ne $SourceDir) {
            throw "CMake cache belongs to another source tree: $cachePath. Choose a fresh build directory."
        }
        if ($cache.CMAKE_GENERATOR -ne "Ninja" -or $cache.CMAKE_BUILD_TYPE -ne "Release") {
            throw "Expected a Ninja Release cache: $cachePath. Choose a fresh build directory."
        }
    }
}

function Assert-ProtectedInput {
    param([string]$InputPath)
    if (-not $InputPath) { return }
    foreach ($outputPath in @($PackageDir, $ZipPath, $RazeBuildDir, $ZMusicBuildDir)) {
        if ($InputPath -eq $outputPath -or $InputPath.StartsWith($outputPath.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw "Output would overwrite an input: $InputPath (output: $outputPath)"
        }
    }
    foreach ($outputPath in @($PackageDir, $ZipPath)) {
        if ($outputPath.StartsWith($InputPath.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw "Package output must not be inside an input: $InputPath"
        }
    }
}

function Copy-RequiredFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$SourcePath,
        [Parameter(Mandatory = $true)]
        [string]$DestinationPath
    )

    if (-not (Test-Path -LiteralPath $SourcePath)) {
        throw "Required file not found: $SourcePath"
    }

    $destinationDir = Split-Path -Parent $DestinationPath
    if (-not (Test-Path -LiteralPath $destinationDir)) {
        New-Item -ItemType Directory -Path $destinationDir -Force | Out-Null
    }

    Copy-Item -LiteralPath $SourcePath -Destination $DestinationPath -Force
}

function Copy-OptionalDirectory {
    param(
        [Parameter(Mandatory = $true)]
        [string]$SourcePath,
        [Parameter(Mandatory = $true)]
        [string]$DestinationPath
    )

    if (Test-Path -LiteralPath $SourcePath) {
        Copy-Item -LiteralPath $SourcePath -Destination $DestinationPath -Recurse -Force
    }
}

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $scriptRoot "..\.."))
$packageRootDir = Join-Path $repoRoot "out\release"
$defaults = @{
    ZMusicSourceDir = "build\zmusic"
    ZMusicBuildDir = "build\zmusic\build-ninja-ovl2"
    VcpkgRoot = "build\vcpkg"
    VcpkgInstalledDir = "vcpkg_installed"
    VcpkgOverlayPorts = "vcpkg-overlays"
    NrdShaderHeaderDir = "libraries\NRD\_Shaders"
    RazeBuildDir = "build\terminal-release"
    PackageDir = "out\release\Duke-RT"
    ZipPath = "out\release\Duke-RT.zip"
}
$explicitConfig = [bool]$ConfigPath
if (-not $ConfigPath) { $ConfigPath = "build\windows-release.local.json" }
$ConfigPath = Get-FullPathSafe $repoRoot $ConfigPath
$settings = @{}
$settingNames = @($defaults.Keys) + @("VsDevCmd", "VcVarsVersion", "ToolchainFile", "ZMusicIncludeDir", "RuntimeDllDir", "NriDependencySourceDir", "DxcExecutable", "CMakeExecutable", "Jobs", "NriRuntimeDir", "FfxSdkRoot")
if (Test-Path -LiteralPath $ConfigPath) {
    $config = Get-Content -LiteralPath $ConfigPath -Raw | ConvertFrom-Json
    if ($null -eq $config -or $config -isnot [pscustomobject]) { throw "Expected a JSON settings object: $ConfigPath" }
    foreach ($property in $config.PSObject.Properties) {
        if ($property.Name -notin $settingNames) {
            throw "Unknown release setting '$($property.Name)' in $ConfigPath"
        }
        $settings[$property.Name] = $property.Value
    }
    Write-Info "Local settings: $ConfigPath"
} elseif ($explicitConfig) {
    throw "Settings file not found: $ConfigPath"
}
foreach ($name in $settingNames) {
    if (-not $PSBoundParameters.ContainsKey($name) -and $settings.ContainsKey($name)) {
        Set-Variable -Name $name -Value $settings[$name]
    }
    if ($defaults.ContainsKey($name)) {
        if (-not (Get-Variable -Name $name -ValueOnly)) { Set-Variable -Name $name -Value $defaults[$name] }
        Set-Variable -Name $name -Value (Get-FullPathSafe $repoRoot (Get-Variable -Name $name -ValueOnly))
    }
}
if ($NriRuntimeDir -or $FfxSdkRoot) {
    Write-Info "Ignoring legacy NriRuntimeDir/FfxSdkRoot settings: NRI builds its SDK runtimes, and NRD is compiled into the engine."
}
if (-not $ToolchainFile) { $ToolchainFile = Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake" }
if (-not $ZMusicIncludeDir) { $ZMusicIncludeDir = Join-Path $ZMusicSourceDir "include" }
if ($VcVarsVersion -and $VcVarsVersion -notmatch '^[0-9.]+$') { throw "VcVarsVersion contains invalid characters: $VcVarsVersion" }
if ($Jobs -lt 1 -or $Jobs -gt 128) { throw "Jobs must be between 1 and 128." }
if (-not $VsDevCmd) { $VsDevCmd = $env:RAZE_VSDEVCMD }
foreach ($name in @("VsDevCmd", "ToolchainFile", "ZMusicIncludeDir", "RuntimeDllDir", "NriDependencySourceDir", "DxcExecutable", "CMakeExecutable")) {
    $value = Get-Variable -Name $name -ValueOnly
    if ($value -and ($name -ne "CMakeExecutable" -or $value -match '[/\\]')) {
        Set-Variable -Name $name -Value (Get-FullPathSafe $repoRoot $value)
    }
}

# Only output trees owned by this checkout may be mutated. External inputs stay external.
foreach ($path in @($ZMusicBuildDir, $RazeBuildDir)) {
    Ensure-WithinRoot (Join-Path $repoRoot "build") $path "Build"
    if ($path -eq (Join-Path $repoRoot "build")) { throw "Choose a build subdirectory." }
    Assert-NoLinks $path
}
foreach ($path in @($PackageDir, $ZipPath)) {
    Ensure-WithinRoot $packageRootDir $path "Package output"
    if ($path -eq $packageRootDir) { throw "Choose a package output below $packageRootDir." }
    Assert-NoLinks $path
}
if ([IO.Path]::GetExtension($ZipPath) -ne ".zip" -or $ZipPath -eq $PackageDir -or
    $ZipPath.StartsWith($PackageDir.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw "ZipPath must name a .zip file outside PackageDir."
}
if (Test-Path -LiteralPath $ZipPath -PathType Container) { throw "ZipPath names a directory: $ZipPath" }
foreach ($inputPath in @($ZMusicSourceDir, $ZMusicIncludeDir, $ToolchainFile, $VcpkgOverlayPorts, $RuntimeDllDir, $VcpkgRoot, $VcpkgInstalledDir, $NrdShaderHeaderDir, $NriDependencySourceDir, $ConfigPath, $VsDevCmd, $DxcExecutable)) {
    Assert-ProtectedInput $inputPath
}
if ([IO.Path]::IsPathRooted($CMakeExecutable)) { Assert-ProtectedInput $CMakeExecutable }
if ($RazeBuildDir -eq $ZMusicBuildDir -or
    $RazeBuildDir.StartsWith($ZMusicBuildDir + '\', [StringComparison]::OrdinalIgnoreCase) -or
    $ZMusicBuildDir.StartsWith($RazeBuildDir + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw "Engine and ZMusic need separate build directories."
}
New-Item -ItemType Directory -Path (Join-Path $repoRoot "build") -Force | Out-Null
try {
    $releaseLock = [IO.File]::Open((Join-Path $repoRoot "build\windows-release.lock"), 'OpenOrCreate', 'ReadWrite', 'None')
} catch { throw "Another Windows release invocation holds this checkout's lock: $($_.Exception.Message)" }
try {
Assert-CacheSource $RazeBuildDir $repoRoot
if (-not $SkipBuild) {
    Assert-CacheSource $ZMusicBuildDir $ZMusicSourceDir
    $toolchain = $ToolchainFile
    $prerequisites = @($toolchain, (Join-Path $ZMusicSourceDir "CMakeLists.txt"),
        (Join-Path $ZMusicIncludeDir "zmusic.h"))
    foreach ($backend in @("dxbc", "dxil", "spirv")) {
        $prerequisites += Join-Path $NrdShaderHeaderDir "REFERENCE_Copy.cs.$backend.h"
    }
    # NRI and its SDK runtimes are built/staged from this checkout by CMake.
    # NRD implementation is compiled into the engine; only its generated headers are external.
    foreach ($path in $prerequisites) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Missing build prerequisite: $path. Set its path in $ConfigPath or pass the corresponding parameter."
        }
    }
    $VsDevCmd = Resolve-VsDevCmd -RequestedPath $VsDevCmd
    $DxcExecutable = Resolve-DxcExecutable -RequestedPath $DxcExecutable -RepositoryRoot $repoRoot
    $RuntimeDllDir = Resolve-RuntimeDllDirectory -RequestedPath $RuntimeDllDir -RepositoryRoot $repoRoot -InstalledDirectory $VcpkgInstalledDir
    foreach ($inputPath in @($VsDevCmd, $DxcExecutable, $RuntimeDllDir)) { Assert-ProtectedInput $inputPath }
    if (-not (Test-Path -LiteralPath $VcpkgOverlayPorts -PathType Container)) { throw "Overlay ports not found: $VcpkgOverlayPorts" }
    $vsToolsDir = Split-Path -Parent $VsDevCmd
    $devShellModule = Join-Path $vsToolsDir "Microsoft.VisualStudio.DevShell.dll"
    if (-not (Test-Path -LiteralPath $devShellModule)) {
        $devShellModule = Join-Path $vsToolsDir "vsdevshell\Microsoft.VisualStudio.DevShell.dll"
    }
    if (-not (Test-Path -LiteralPath $devShellModule)) { throw "Visual Studio developer PowerShell module not found: $devShellModule" }
    Import-Module $devShellModule -ErrorAction Stop
    $developerArguments = "-arch=x64 -host_arch=x64"
    if ($VcVarsVersion) { $developerArguments += " -vcvars_ver=$VcVarsVersion" }
    Enter-VsDevShell -VsInstallPath (Get-FullPathSafe $vsToolsDir "..\..") -SkipAutomaticLocation -DevCmdArguments $developerArguments
    Write-Info "Visual Studio environment: $VsDevCmd (toolset: $VcVarsVersion)"
    Write-Info "DXC: $DxcExecutable"
    Write-Info "OpenAL runtime: $RuntimeDllDir"
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) { throw "Visual Studio did not initialize the C++ compiler." }
    $CMakeExecutable = (Get-Command $CMakeExecutable -ErrorAction Stop).Source
    Assert-ProtectedInput $CMakeExecutable
    $env:VCPKG_OVERLAY_PORTS = $VcpkgOverlayPorts
    $env:VCPKG_CMAKE_CONFIGURE_OPTIONS = "-DCMAKE_POLICY_DEFAULT_CMP0026=OLD"
    $common = @("-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_TOOLCHAIN_FILE=$toolchain",
        "-DVCPKG_INSTALLED_DIR=$VcpkgInstalledDir", "-DVCPKG_OVERLAY_PORTS=$env:VCPKG_OVERLAY_PORTS")
    Write-Info "Building ZMusic in $ZMusicBuildDir"
    Invoke-CMake (@("-S", $ZMusicSourceDir, "-B", $ZMusicBuildDir, "-DVCPKG_LIBSNDFILE=1") + $common)
    Invoke-CMake @("--build", $ZMusicBuildDir, "--target", "zmusiclite", "--parallel", "$Jobs")
    Write-Info "Building Windows Release in $RazeBuildDir"
    $engine = @("-S", $repoRoot, "-B", $RazeBuildDir, "-DHAVE_NRI=ON", "-DRAZE_NRI_SHADER_PROFILE=PRODUCTION",
        "-DRAZE_NRI_PERF_AUDIT_SHADERS=OFF", "-DRAZE_NRI_DEPENDENCY_SOURCE_DIR=$NriDependencySourceDir",
        "-DZMUSIC_INCLUDE_DIR=$ZMusicIncludeDir", "-DRAZE_RUNTIME_DLL_DIR=$RuntimeDllDir", "-DZMUSIC_LIBRARIES=$ZMusicBuildDir/source/zmusiclite.lib",
        "-DRAZE_NRD_SHADER_HEADER_DIR=$NrdShaderHeaderDir")
    $savedVulkanSdk = $env:VULKAN_SDK
    try {
        if ($DxcExecutable) {
            if (-not (Test-Path -LiteralPath $DxcExecutable -PathType Leaf)) { throw "DXC not found: $DxcExecutable" }
            # CMake prefers VULKAN_SDK over the cache; an explicit setting must win.
            $env:VULKAN_SDK = $null
            $engine += "-DRAZE_DXC_EXECUTABLE=$DxcExecutable"
        }
        Invoke-CMake ($engine + $common)
        Invoke-CMake @("--build", $RazeBuildDir, "--target", "revision_check")
        Invoke-CMake @("--build", $RazeBuildDir, "--target", "raze", "--parallel", "$Jobs")
    } finally { $env:VULKAN_SDK = $savedVulkanSdk }
}
# Inspect cached tool paths without requiring compilers to be installed in SkipBuild mode.
foreach ($buildDir in @($RazeBuildDir, $ZMusicBuildDir)) {
    $cache = Get-CMakeCache $buildDir
    foreach ($name in @("RAZE_DXC_EXECUTABLE", "CMAKE_COMMAND", "CMAKE_MAKE_PROGRAM", "CMAKE_C_COMPILER", "CMAKE_CXX_COMPILER", "CMAKE_LINKER", "CMAKE_RC_COMPILER")) {
        if ($cache[$name] -and [IO.Path]::IsPathRooted($cache[$name])) {
            Assert-ProtectedInput (Get-FullPathSafe $repoRoot $cache[$name])
        }
    }
}
$releaseCache = Get-CMakeCache $RazeBuildDir
if ($releaseCache.CMAKE_BUILD_TYPE -ne "Release" -or $releaseCache.RAZE_NRI_SHADER_PROFILE -notin @("AUTO", "PRODUCTION")) {
    throw "Release packaging requires a Release cache with AUTO or PRODUCTION NRI shaders: $RazeBuildDir"
}
$auditShaders = [string]$releaseCache.RAZE_NRI_PERF_AUDIT_SHADERS
if ($auditShaders -and $auditShaders -notmatch '^(0|OFF|NO|FALSE|N|IGNORE|NOTFOUND)$' -and $auditShaders -notmatch '-NOTFOUND$') {
    throw "Release packaging requires RAZE_NRI_PERF_AUDIT_SHADERS=OFF: $RazeBuildDir"
}
$packageLauncher = Join-Path $repoRoot "package\windows\launch-duke-rt.cmd"
$prepareNormals = Join-Path $repoRoot "tools\dist\Prepare-CommercialNormals.ps1"
$releaseOverlay = Join-Path $repoRoot "release-overlay"

$requiredBuildFiles = @(
    (Join-Path $RazeBuildDir "raze.exe"),
    (Join-Path $RazeBuildDir "raze.pk3"),
    (Join-Path $RazeBuildDir "OpenAL32.dll"),
    (Join-Path $RazeBuildDir "zmusiclite.dll"),
    (Join-Path $RazeBuildDir "NRI.dll"),
    (Join-Path $RazeBuildDir "amd_fidelityfx_dx12.dll"),
    (Join-Path $RazeBuildDir "amd_fidelityfx_vk.dll"),
    (Join-Path $RazeBuildDir "AgilitySDK\D3D12Core.dll"),
    (Join-Path $RazeBuildDir "FidelityFX-SDK-LICENSE.txt"),
    $packageLauncher,
    $prepareNormals
)

$nriShaderDir = Join-Path $RazeBuildDir "shaders\nri"
$nriShaderManifestPath = Join-Path $nriShaderDir "nri-shaders.json"
$requiredBuildFiles += $nriShaderManifestPath

foreach ($requiredFile in $requiredBuildFiles) {
    if (-not (Test-Path -LiteralPath $requiredFile)) {
        throw "Required Release build output was not found: $requiredFile"
    }
}

# Verify before touching package outputs, including -SkipBuild. A matching API
# version alone cannot distinguish the required vendored NRI backend fixes.
$nriRuntimeVerifier = Join-Path $repoRoot "cmake\VerifyNriRuntime.cmake"
Invoke-CMake @("-DSOURCE_ROOT=$repoRoot", "-DRUNTIME_DIR=$RazeBuildDir",
    "-DEXPECTED_CONFIGURATION=Release", "-P", $nriRuntimeVerifier)

if (-not (Test-Path -LiteralPath $releaseOverlay)) {
    throw "release-overlay was not found: $releaseOverlay"
}

$gitExecutable = (Get-Command git -CommandType Application -ErrorAction Stop).Source
Assert-ProtectedInput $gitExecutable
$overlayFiles = @(& $gitExecutable -C $repoRoot -c core.quotepath=false ls-files -- release-overlay)
if ($LASTEXITCODE -ne 0 -or $overlayFiles.Count -eq 0) { throw "Could not enumerate the checkout's tracked release-overlay." }
foreach ($relative in $overlayFiles) {
    if ($relative -match '(?i)(\.grp$|\.kvx$|/normalmaps/)' -or
        -not (Test-Path -LiteralPath (Join-Path $repoRoot $relative) -PathType Leaf)) {
        throw "Missing or third-party file in authored release-overlay: $relative"
    }
}
$nriManifest = Get-Content -LiteralPath $nriShaderManifestPath -Raw | ConvertFrom-Json
if ([int]$nriManifest.schema -ne 1 -or [string]$nriManifest.resolvedProfile -ne "PRODUCTION" -or
    [string]$nriManifest.defaultVariant -ne "production") {
    throw "Release packaging requires an NRI shader manifest with schema 1, resolvedProfile PRODUCTION and defaultVariant production: $nriShaderManifestPath"
}
$productionEntries = @($nriManifest.entries | Where-Object { [string]$_.variant -eq "production" })
if ($productionEntries.Count -eq 0 -or $productionEntries.Count -ne [int]$nriManifest.canonicalBlobCount -or
    $productionEntries.Count -ne @($nriManifest.entries).Count) {
    throw "Release packaging requires only the declared canonical production shader set; manifest declares $($nriManifest.canonicalBlobCount) blobs and has $($productionEntries.Count) production entries"
}
$expectedStagedNri = @("nri-shaders.json")
$seenNriShaders = @{}
foreach ($entry in $productionEntries) {
    $relative = [string]$entry.path
    if ([string]::IsNullOrWhiteSpace($relative) -or [System.IO.Path]::IsPathRooted($relative) -or
        $relative.Contains("..") -or $relative.Contains("/") -or $relative.Contains("\") -or
        $relative -notmatch '^[A-Za-z0-9_-]+(?:\.[A-Za-z0-9_-]+)*\.(dxil|spirv)$' -or
        $relative -match '^audit-task-' -or $seenNriShaders.ContainsKey($relative)) {
        throw "Invalid canonical NRI shader manifest path: $relative"
    }
    if ([string]$entry.backend -notin @("dxil", "spirv") -or
        [IO.Path]::GetExtension($relative) -cne ("." + [string]$entry.backend)) {
        throw "Invalid NRI shader backend for $relative"
    }
    $seenNriShaders[$relative] = $true
    $source = Join-Path $nriShaderDir $relative
    if (-not (Test-Path -LiteralPath $source)) {
        throw "Manifest-listed NRI shader was not found: $source"
    }
    $actualHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne ([string]$entry.sha256).ToLowerInvariant()) {
        throw "NRI shader hash mismatch: $relative"
    }
    $expectedStagedNri += $relative
}

if (Test-Path -LiteralPath $PackageDir) {
    if (Get-ChildItem -LiteralPath $PackageDir -Recurse -Force | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }) {
        throw "Existing package contains links; refusing recursive removal: $PackageDir"
    }
    Write-Info "Removing existing package dir $PackageDir"
    Remove-Item -LiteralPath $PackageDir -Recurse -Force
}
New-Item -ItemType Directory -Path $PackageDir -Force | Out-Null
Write-Info "Copying release binaries and staged runtime files"
foreach ($name in @("raze.exe", "raze.pk3", "FidelityFX-SDK-LICENSE.txt", "NRI-LICENSE.txt", "nri-runtime.json")) {
    Copy-RequiredFile (Join-Path $RazeBuildDir $name) (Join-Path $PackageDir $name)
}
# Older build trees may contain a copied NRD.dll; its implementation is now in the engine.
$runtimeDlls = Get-ChildItem -LiteralPath $RazeBuildDir -File -Filter *.dll |
    Where-Object { $_.Name -ine "NRD.dll" } | Sort-Object Name
foreach ($runtimeDll in $runtimeDlls) {
    Copy-RequiredFile $runtimeDll.FullName (Join-Path $PackageDir $runtimeDll.Name)
}
Copy-OptionalDirectory (Join-Path $RazeBuildDir "AgilitySDK") (Join-Path $PackageDir "AgilitySDK")
Invoke-CMake @("-DSOURCE_ROOT=$repoRoot", "-DRUNTIME_DIR=$PackageDir",
    "-DEXPECTED_CONFIGURATION=Release", "-P", $nriRuntimeVerifier)
$stagedNriDir = Join-Path $PackageDir "shaders\nri"
New-Item -ItemType Directory -Path $stagedNriDir -Force | Out-Null
foreach ($entry in $productionEntries) {
    Copy-RequiredFile (Join-Path $nriShaderDir $entry.path) (Join-Path $stagedNriDir $entry.path)
}
Copy-RequiredFile -SourcePath $nriShaderManifestPath -DestinationPath (Join-Path $stagedNriDir "nri-shaders.json")
$actualStagedNri = @(Get-ChildItem -LiteralPath $stagedNriDir -Recurse -File |
    ForEach-Object { $_.FullName.Substring($stagedNriDir.Length + 1) } | Sort-Object)
$expectedStagedNri = @($expectedStagedNri | Sort-Object)
if (($actualStagedNri -join "`n") -ne ($expectedStagedNri -join "`n")) {
    throw "Staged NRI shader files do not exactly match the production manifest"
}
Copy-OptionalDirectory -SourcePath (Join-Path $RazeBuildDir "soundfonts") -DestinationPath (Join-Path $PackageDir "soundfonts")

Write-Info "Copying release launcher assets and overlay"
Copy-RequiredFile -SourcePath $packageLauncher -DestinationPath (Join-Path $PackageDir "launch-duke-rt.cmd")
Copy-RequiredFile -SourcePath $prepareNormals -DestinationPath (Join-Path $PackageDir "tools\dist\Prepare-CommercialNormals.ps1")
foreach ($relative in $overlayFiles) {
    Copy-RequiredFile (Join-Path $repoRoot $relative) (Join-Path $PackageDir $relative)
}

Get-ChildItem -LiteralPath $PackageDir -Recurse -File -Filter *.pdb | Remove-Item -Force

if (-not $SkipZip) {
    if (Test-Path -LiteralPath $ZipPath) { Remove-Item -LiteralPath $ZipPath -Force }
    New-Item -ItemType Directory -Path (Split-Path -Parent $ZipPath) -Force | Out-Null
    Write-Info "Creating zip archive $ZipPath"
    Compress-Archive -Path $PackageDir -DestinationPath $ZipPath -CompressionLevel Optimal
}

$packagedRequiredFiles = @(
    (Join-Path $PackageDir "raze.exe"),
    (Join-Path $PackageDir "raze.pk3"),
    (Join-Path $PackageDir "launch-duke-rt.cmd"),
    (Join-Path $PackageDir "tools\dist\Prepare-CommercialNormals.ps1"),
    (Join-Path $PackageDir "release-overlay"),
    (Join-Path $PackageDir "OpenAL32.dll"),
    (Join-Path $PackageDir "zmusiclite.dll"),
    (Join-Path $PackageDir "NRI.dll"),
    (Join-Path $PackageDir "amd_fidelityfx_dx12.dll"),
    (Join-Path $PackageDir "amd_fidelityfx_vk.dll"),
    (Join-Path $PackageDir "AgilitySDK\D3D12Core.dll")
)

foreach ($packagedFile in $packagedRequiredFiles) {
    if (-not (Test-Path -LiteralPath $packagedFile)) {
        throw "Packaged output is missing required entry: $packagedFile"
    }
}

$remainingPdbs = Get-ChildItem -LiteralPath $PackageDir -Recurse -File -Filter *.pdb
if ($remainingPdbs) {
    throw "Packaged output still contains PDB files."
}

Write-Info "Package ready:"
Write-Info "  folder: $PackageDir"
if (-not $SkipZip) {
    Write-Info "  zip:    $ZipPath"
}
} finally { $releaseLock.Dispose() }
