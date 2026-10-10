param(
    [string]$ConfigPath = "",
    [string]$VsDevCmd = "",
    [string]$VcpkgRoot = "",
    [string]$VcpkgInstalledDir = "",
    [string]$ZMusicSourceDir = "",
    [string]$ZMusicBuildDir = "",
    [string]$NrdShaderHeaderDir = "",
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

function Assert-CacheSource {
    param([string]$BuildDir, [string]$SourceDir)
    $cachePath = Join-Path $BuildDir "CMakeCache.txt"
    if (Test-Path -LiteralPath $cachePath) {
        $cache = @{}
        foreach ($line in Get-Content -LiteralPath $cachePath) {
            if ($line -match '^([^#/:][^:]*):[^=]+=(.*)$') { $cache[$Matches[1]] = $Matches[2] }
        }
        if (-not $cache.CMAKE_HOME_DIRECTORY -or
            (Get-FullPathSafe $SourceDir $cache.CMAKE_HOME_DIRECTORY) -ne $SourceDir) {
            throw "CMake cache belongs to another source tree: $cachePath. Choose a fresh build directory."
        }
        if ($cache.CMAKE_GENERATOR -ne "Ninja" -or $cache.CMAKE_BUILD_TYPE -ne "Release") {
            throw "Expected a Ninja Release cache: $cachePath. Choose a fresh build directory."
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
    NrdShaderHeaderDir = "libraries\NRD\_Shaders"
    NriRuntimeDir = "..\NRD-Sample\_Bin\Release"
    FfxSdkRoot = "..\NRD-Sample\_Build\_deps\ffx-src"
    RazeBuildDir = "build\terminal-release"
    PackageDir = "out\release\Duke-RT"
    ZipPath = "out\release\Duke-RT.zip"
}
$explicitConfig = [bool]$ConfigPath
if (-not $ConfigPath) { $ConfigPath = "build\windows-release.local.json" }
$ConfigPath = Get-FullPathSafe $repoRoot $ConfigPath
$settings = @{}
if (Test-Path -LiteralPath $ConfigPath) {
    $config = Get-Content -LiteralPath $ConfigPath -Raw | ConvertFrom-Json
    if ($null -eq $config -or $config -isnot [pscustomobject]) { throw "Expected a JSON settings object: $ConfigPath" }
    foreach ($property in $config.PSObject.Properties) {
        if ($property.Name -notin @($defaults.Keys) + @("VsDevCmd", "DxcExecutable", "CMakeExecutable", "Jobs")) {
            throw "Unknown release setting '$($property.Name)' in $ConfigPath"
        }
        $settings[$property.Name] = $property.Value
    }
    Write-Info "Local settings: $ConfigPath"
} elseif ($explicitConfig) {
    throw "Settings file not found: $ConfigPath"
}
foreach ($name in @($defaults.Keys) + @("VsDevCmd", "DxcExecutable", "CMakeExecutable", "Jobs")) {
    if (-not $PSBoundParameters.ContainsKey($name) -and $settings.ContainsKey($name)) {
        Set-Variable -Name $name -Value $settings[$name]
    }
    if ($defaults.ContainsKey($name)) {
        if (-not (Get-Variable -Name $name -ValueOnly)) { Set-Variable -Name $name -Value $defaults[$name] }
        Set-Variable -Name $name -Value (Get-FullPathSafe $repoRoot (Get-Variable -Name $name -ValueOnly))
    }
}
if ($Jobs -lt 1 -or $Jobs -gt 128) { throw "Jobs must be between 1 and 128." }
foreach ($name in @("VsDevCmd", "DxcExecutable", "CMakeExecutable")) {
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
foreach ($inputPath in @($ZMusicSourceDir, $VcpkgRoot, $VcpkgInstalledDir, $NrdShaderHeaderDir, $NriRuntimeDir, $FfxSdkRoot, $ConfigPath)) {
    foreach ($outputPath in @($PackageDir, $ZipPath, $RazeBuildDir, $ZMusicBuildDir)) {
        if ($inputPath -eq $outputPath -or $inputPath.StartsWith($outputPath.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw "Output would overwrite an input: $outputPath"
        }
    }
    foreach ($outputPath in @($PackageDir, $ZipPath)) {
        if ($outputPath.StartsWith($inputPath.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw "Package output must not be inside an input: $inputPath"
        }
    }
}
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
    $toolchain = Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"
    $prerequisites = @($toolchain, (Join-Path $ZMusicSourceDir "CMakeLists.txt"),
        (Join-Path $ZMusicSourceDir "include\zmusic.h"), (Join-Path $FfxSdkRoot "sdk\LICENSE.txt"))
    foreach ($backend in @("dxbc", "dxil", "spirv")) {
        $prerequisites += Join-Path $NrdShaderHeaderDir "REFERENCE_Copy.cs.$backend.h"
    }
    foreach ($name in @("NRI.dll", "NRD.dll", "amd_fidelityfx_dx12.dll", "amd_fidelityfx_vk.dll", "AgilitySDK\D3D12Core.dll")) {
        $prerequisites += Join-Path $NriRuntimeDir $name
    }
    foreach ($path in $prerequisites) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Missing build prerequisite: $path. Set its path in $ConfigPath or pass the corresponding parameter."
        }
    }
    if (-not $VsDevCmd) { $VsDevCmd = $env:RAZE_VSDEVCMD }
    if (-not $VsDevCmd) {
        $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
        if (Test-Path -LiteralPath $vswhere) {
            $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
            if ($installation) { $VsDevCmd = Join-Path $installation "Common7\Tools\VsDevCmd.bat" }
        }
    }
    if (-not $VsDevCmd) { throw "Visual Studio C++ tools not found. Set VsDevCmd or RAZE_VSDEVCMD." }
    $VsDevCmd = Get-FullPathSafe $repoRoot $VsDevCmd
    $devShell = Join-Path (Split-Path -Parent $VsDevCmd) "Launch-VsDevShell.ps1"
    if (-not (Test-Path -LiteralPath $devShell)) { throw "Visual Studio developer PowerShell not found: $devShell" }
    & $devShell -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) { throw "Visual Studio did not initialize the C++ compiler." }
    $CMakeExecutable = (Get-Command $CMakeExecutable -ErrorAction Stop).Source
    $env:VCPKG_OVERLAY_PORTS = Join-Path $repoRoot "vcpkg-overlays"
    $env:VCPKG_CMAKE_CONFIGURE_OPTIONS = "-DCMAKE_POLICY_DEFAULT_CMP0026=OLD"
    $common = @("-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_TOOLCHAIN_FILE=$toolchain",
        "-DVCPKG_INSTALLED_DIR=$VcpkgInstalledDir", "-DVCPKG_OVERLAY_PORTS=$env:VCPKG_OVERLAY_PORTS")
    Write-Info "Building ZMusic in $ZMusicBuildDir"
    Invoke-CMake (@("-S", $ZMusicSourceDir, "-B", $ZMusicBuildDir, "-DVCPKG_LIBSNDFILE=1") + $common)
    Invoke-CMake @("--build", $ZMusicBuildDir, "--target", "zmusiclite", "--parallel", "$Jobs")
    Write-Info "Building Windows Release in $RazeBuildDir"
    $engine = @("-S", $repoRoot, "-B", $RazeBuildDir, "-DHAVE_NRI=ON", "-DRAZE_NRI_SHADER_PROFILE=PRODUCTION",
        "-DZMUSIC_INCLUDE_DIR=$ZMusicSourceDir/include", "-DZMUSIC_LIBRARIES=$ZMusicBuildDir/source/zmusiclite.lib",
        "-DRAZE_NRD_SHADER_HEADER_DIR=$NrdShaderHeaderDir", "-DRAZE_NRI_RUNTIME_DIR=$NriRuntimeDir", "-DRAZE_FFX_SDK_ROOT=$FfxSdkRoot")
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
$packageLauncher = Join-Path $repoRoot "package\windows\launch-duke-rt.cmd"
$prepareNormals = Join-Path $repoRoot "tools\dist\Prepare-CommercialNormals.ps1"
$releaseOverlay = Join-Path $repoRoot "release-overlay"

$requiredBuildFiles = @(
    (Join-Path $RazeBuildDir "raze.exe"),
    (Join-Path $RazeBuildDir "raze.pk3"),
    (Join-Path $RazeBuildDir "OpenAL32.dll"),
    (Join-Path $RazeBuildDir "zmusiclite.dll"),
    (Join-Path $RazeBuildDir "NRI.dll"),
    (Join-Path $RazeBuildDir "NRD.dll"),
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

if (-not (Test-Path -LiteralPath $releaseOverlay)) {
    throw "release-overlay was not found: $releaseOverlay"
}

$overlayFiles = @(& git -C $repoRoot -c core.quotepath=false ls-files -- release-overlay)
if ($LASTEXITCODE -ne 0 -or $overlayFiles.Count -eq 0) { throw "Could not enumerate the checkout's tracked release-overlay." }
foreach ($relative in $overlayFiles) {
    if ($relative -match '(?i)(\.grp$|\.kvx$|/normalmaps/)' -or
        -not (Test-Path -LiteralPath (Join-Path $repoRoot $relative) -PathType Leaf)) {
        throw "Missing or third-party file in authored release-overlay: $relative"
    }
}
$nriManifest = Get-Content -LiteralPath $nriShaderManifestPath -Raw | ConvertFrom-Json
if ([int]$nriManifest.schema -ne 1 -or [string]$nriManifest.resolvedProfile -ne "PRODUCTION") {
    throw "Release packaging requires an NRI shader manifest with schema 1 and resolvedProfile PRODUCTION: $nriShaderManifestPath"
}
$productionEntries = @($nriManifest.entries | Where-Object { [string]$_.variant -eq "production" })
if ($productionEntries.Count -eq 0 -or $productionEntries.Count -ne [int]$nriManifest.canonicalBlobCount) {
    throw "Release packaging requires the declared canonical NRI shader set; manifest declares $($nriManifest.canonicalBlobCount) blobs and has $($productionEntries.Count) production entries"
}
$expectedStagedNri = @("nri-shaders.json")
foreach ($entry in $productionEntries) {
    $relative = [string]$entry.path
    if ([string]::IsNullOrWhiteSpace($relative) -or [System.IO.Path]::IsPathRooted($relative) -or
        $relative.Contains("..") -or $relative.Contains("/") -or $relative.Contains("\")) {
        throw "Invalid canonical NRI shader manifest path: $relative"
    }
    if ([string]$entry.backend -notin @("dxil", "spirv")) {
        throw "Invalid NRI shader backend for $relative"
    }
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
foreach ($name in @("raze.exe", "raze.pk3", "FidelityFX-SDK-LICENSE.txt")) {
    Copy-RequiredFile (Join-Path $RazeBuildDir $name) (Join-Path $PackageDir $name)
}
$runtimeDlls = Get-ChildItem -LiteralPath $RazeBuildDir -File -Filter *.dll | Sort-Object Name
foreach ($runtimeDll in $runtimeDlls) {
    Copy-RequiredFile $runtimeDll.FullName (Join-Path $PackageDir $runtimeDll.Name)
}
Copy-OptionalDirectory (Join-Path $RazeBuildDir "AgilitySDK") (Join-Path $PackageDir "AgilitySDK")
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
    (Join-Path $PackageDir "zmusiclite.dll")
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
