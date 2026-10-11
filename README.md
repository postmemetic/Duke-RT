# Duke-RT

![Duke-RT gameplay](images/20.png)

Duke-RT is a fork of Raze that adds a new ray-tracing render backend based on [NVIDIA NRI](https://github.com/NVIDIA-RTX/NRI). The existing Build-engine game support from Raze remains the foundation, while this fork focuses on path tracing, RT renderer bring-up, lighting authoring, custom material authoring, denoising/upscaling integration, and backend diagnostics. It also includes tooling and overlay workflows so users can create their own material and lighting rules for Duke content. Windows remains the primary tested platform. An initial [Linux Vulkan milestone](LINUX.md) includes NRD/TAA/NIS and DLSS SR/RR; native Linux gameplay works, but it has not been thoroughly tested yet. [Watch a somewhat recent gameplay video](https://www.youtube.com/watch?v=7z7txcZg2q0).

![Duke-RT gameplay](images/EmergeFromSmoke.gif)

The renderer supports both Direct3D 12 and Vulkan, although feature support is more complete for D3D12. It's recommended that you play in D3D12 and HDR if possible!

![Duke-RT gameplay](images/1.png)

**Check out the linked project docs below to make your own lighting rules and material overrides!**

## How To Install and Run (Windows)

1. Purchase and install Duke Nukem 3D: 20th Anniversary World Tour (or otherwise have it installed on your computer - you need a commercial version of Duke 3D to use with the Duke-RT engine fork)
2. Download the .zip file in the assets section from this release
3. Unzip Duke-RT to your computer
4. Run **launch-duke-rt.cmd** in that folder
5. Give it the path to your Duke 3D install (if it doesn't find it automatically)
6. Let it grab the normal maps from Duke World Tour (if that's your Duke version, will still work without them)
7. Let it open your browser to grab the voxels
8. Download the voxels from that site
9. Go back to the Duke-RT console window and hit enter
10. Let it unpack the voxels into its own directory for you
11. Proceed into the game
12. **Always run via launch-duke-rt.cmd in the future**

## How to Install and Run (Linux)

1. Purchase and install Duke Nukem 3D: 20th Anniversary World Tour (or otherwise have it installed on your computer - you need a commercial version of Duke 3D to use with the Duke-RT engine fork)
2. Download the Linux tarball in the assets section from this release
3. Untar Duke-RT to your computer
4. Run **launch-duke-rt.sh** via CLI from that folder
5. Give it the path to your Duke 3D install (if it doesn't find it automatically)
6. Let it open your browser to grab the voxels
7. Download the voxels from that site
8. Go back to the Duke-RT console window and give it the path to your voxel zip file
9. Let it unpack the voxels into its own directory for you
10. Let it grab the normal maps from Duke World Tour (if that's your Duke version, will still work without them)
11. Proceed into the game
12. **Always run via launch-duke-rt.sh in the future**

## Current Status

Duke-RT is work in progress. The core renderer is in, with full support for modern graphics APIs and libraries like D3D12, DLSS Super Resolution and Ray Reconstruction, etc. I've also gone through and **remastered Duke episodes 1, 2, and 3 with PBR materials (based on the originals) as well as updated lighting**. The entire original game should be playable, and run with decent performance, as well as some nice bonus features like smoke that I've added. There are some known issues that are on my radar but I have yet to tackle.

![Duke-RT gameplay](images/38.png)

Known high-priority issues:
- slow perf in some smokey areas
- remaining transport-driven non-euclidean edge cases in `E5L1`

Known lower-priority issues:
- some end level switches slide during the animation
- the last scene panning sequence no longer appears on surveillance camera screens
- framegen only works with D3D12

I also have a bunch of features I'd like to tackle in the future, including some renderer improvements, as well as a complete pass on the other main Duke 3D episodes.

Upcoming feature work:
- water surfaces
- AMD support

![cropped spheres](images/23.png)

## Tools and Guides

Check these out if you want runtime commands, authoring tools, and the current workflows to add your own custom materials and lights.

- [RT renderer debug commands](RT-DEBUG-COMMANDS.md): runtime backend selection, path-tracing debug views, frame generation controls, lighting diagnostics, repro workflows, and useful command-line/logging tools while validating custom content.
- [LIGHTOVR authoring guide](LIGHTOVR-AUTHORING.md): the light overlay format and authoring workflow for creating your own custom lighting rules, including load/reload behavior, writable loose-overlay setup, and actor light edit mode.
- [VOXELPRELOAD authoring guide](VOXELPRELOAD-AUTHORING.md): the mounted voxel preload format for warming important voxel actor variants, map-specific props, projectiles, and animated picnum ranges during map loading.
- [Material overlay authoring guide](MATERIAL-OVERLAY-AUTHORING.md): the material-overlay authoring workflow for creating your own PBR companion maps, including Duke tile naming, supported map types, and how to add custom metallic, roughness, specular, normal, and glow textures.

![cropped spheres](images/cropped-spheres.png)

## Original Raze Background

[![Upstream Raze Continuous Integration](https://github.com/ZDoom/Raze/actions/workflows/continuous_integration.yml/badge.svg)](https://github.com/ZDoom/Raze/actions/workflows/continuous_integration.yml)

Raze is a fork of Build engine games backed by GZDoom tech and combines Duke Nukem 3D, Blood, Redneck Rampage, Shadow Warrior and Exhumed/Powerslave in a single package. It is also capable of playing Nam and WW2 GI.

The game modules are based on the following sources:

  * Duke Nukem: JFDuke, EDuke 2.0, World Tour extensions from DukeGDX and some minor fixes from EDuke32.
  * Redneck Rampage: Nuke.YKT's reconstructed source available in the Rednukem Git repo.
  * Blood: NBlood.
  * Shadow Warrior: SWP and VoidSW.
  * Exhumed/Powerslave: PCExhumed, with some enhancements inspired by PowerslaveGDX.

ZDoom, GZDoom Copyright (c) 1998-2022 ZDoom + GZDoom teams, and contributors

Doom Source (c) 1997 id Software, Raven Software, and contributors

EDuke32 and VoidSW Source (c) 2005-2020 EDuke32 teams, and contributors

NBlood source (c) 2019-2020 Nuke.YKT

PCExhumed source (c) 2019-2020 sirlemonhead, Nuke.YKT

BuildGDX (c) 2020

Duke Nukem 3D Source (c) 1996-2003 3D Realms

Shadow Warrior Source (c) 1997-2005 3D Realms

"Build Engine & Tools" Copyright (c) 1993-1997 Ken Silverman
Ken Silverman's official web site: http://www.advsys.net/ken
See the included license file "BUILDLIC.TXT" for license info.

Please see license files for individual contributor licenses

Special thanks to Coraline of the 3DGE team for allowing us to use her README.md as a template for this one.

### Non-Build code is licensed under the GPL v2
##### https://www.gnu.org/licenses/old-licenses/gpl-2.0.html
---

## How to build Duke-RT

For Linux, use [Build-LinuxReleasePackage.sh](tools/dist/Build-LinuxReleasePackage.sh)
or the Windows/WSL wrapper [Build-LinuxReleasePackage.ps1](tools/dist/Build-LinuxReleasePackage.ps1).
Both compile and package a release; [LINUX.md](LINUX.md#build-and-package-a-linux-release)
lists prerequisites, commands and native hardware tests. The Windows instructions
below have not recently been revalidated for a newcomer to the repo.

### Windows Build Instructions

#### Prerequisites

- Git, and CMake (at least 3.16) available in `PATH`
- Win 10 probably preferred for decent Terminal, although the toolchain mostly just needs a 64-bit OS
- Visual Studio 2022 with the “Desktop development with C++” workload (MSVC, Ninja, and the Windows 10+ SDK)
- Internet access for submodules and [vcpkg](https://github.com/microsoft/vcpkg) bootstrap

#### Recommended Windows setup

This is the current Windows flow for this repo. Run all commands from the repository root.

Important repo-specific rules:

- Enter the Visual Studio developer environment before configure or build commands that use MSVC.
- Build ZMusic first, then build Duke-RT.
- ZMusic uses `x64-windows` dependencies, while the main Duke-RT project uses `x64-windows-static`.
- The verified terminal output path is `build\terminal-ninja\raze.exe`.

##### Exact environment variables

If you are using `cmd.exe`:

```bat
set "REPO_ROOT=%CD%"
set "VCPKG_ROOT=%REPO_ROOT%\build\vcpkg"
set "VCPKG_OVERLAY_PORTS=%REPO_ROOT%\vcpkg-overlays"
set "VCPKG_CMAKE_CONFIGURE_OPTIONS=-DCMAKE_POLICY_DEFAULT_CMP0026=OLD"
set "VCPKG_KEEP_ENV_VARS=VCPKG_CMAKE_CONFIGURE_OPTIONS"
```

If you are using PowerShell:

```powershell
$RepoRoot = (Resolve-Path .).Path
$env:VCPKG_ROOT = "$RepoRoot\build\vcpkg"
$env:VCPKG_OVERLAY_PORTS = "$RepoRoot\vcpkg-overlays"
$env:VCPKG_CMAKE_CONFIGURE_OPTIONS = "-DCMAKE_POLICY_DEFAULT_CMP0026=OLD"
$env:VCPKG_KEEP_ENV_VARS = "VCPKG_CMAKE_CONFIGURE_OPTIONS"
```

The configure and build examples below intentionally use `cmd /c`, so `%REPO_ROOT%`, `%VCPKG_ROOT%`, and `%VCPKG_OVERLAY_PORTS%` expand correctly even when launched from PowerShell.

What those variables are for:

- `VCPKG_ROOT`: local `build\vcpkg` checkout
- `VCPKG_OVERLAY_PORTS`: required overlay ports for this repo
- `VCPKG_CMAKE_CONFIGURE_OPTIONS`: keeps the ZMusic/vcpkg configure path working with the older yasm port behavior
- `VCPKG_KEEP_ENV_VARS`: tells `vcpkg` to preserve `VCPKG_CMAKE_CONFIGURE_OPTIONS` for child CMake invocations

##### One-time dependency bootstrap

Bootstrap `vcpkg`:

```powershell
git clone https://github.com/microsoft/vcpkg build\vcpkg
git -C build\vcpkg checkout 74e6536215718009aae747d86d84b78376bf9e09
cmd /c "build\vcpkg\bootstrap-vcpkg.bat -disableMetrics"
```

Install the `x64-windows` runtime-side dependency set used by ZMusic and the shared Windows DLL cache:

```powershell
cmd /c "build\vcpkg\vcpkg.exe install --triplet x64-windows"
```

Clone ZMusic if needed:

```powershell
git clone https://github.com/zdoom/zmusic build\zmusic
```

##### Configure and build ZMusic

These are Powershell commands. When running, be sure to replace the path for the call to wherever you installed your copy of VS 2022.

Configure:

```powershell
cmd /c "call C:\PROGRA~1\MICROS~1\2022\COMMUN~1\Common7\Tools\VsDevCmd.bat -arch=x64 -host_arch=x64 && cmake -G Ninja -S build\zmusic -B build\zmusic\build-ninja-ovl2 -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake -DVCPKG_LIBSNDFILE=1 -DVCPKG_INSTALLED_DIR=%REPO_ROOT%\vcpkg_installed -DVCPKG_OVERLAY_PORTS=%VCPKG_OVERLAY_PORTS%"
```

Build:

```powershell
cmd /c "call C:\PROGRA~1\MICROS~1\2022\COMMUN~1\Common7\Tools\VsDevCmd.bat -arch=x64 -host_arch=x64 && cmake --build build\zmusic\build-ninja-ovl2 --config Release --target zmusiclite"
```

Important outputs:

- `build\zmusic\include`
- `build\zmusic\build-ninja-ovl2\source\zmusiclite.lib`
- `build\zmusic\build-ninja-ovl2\source\zmusiclite.dll`

##### Configure and build Duke-RT

More Powershell commands. Also be sure to replace the C: in the path as necessary for your install of VS 2022 here as well.

Configure:

```powershell
cmd /c "call C:\PROGRA~1\MICROS~1\2022\COMMUN~1\Common7\Tools\VsDevCmd.bat -arch=x64 -host_arch=x64 && cmake -G Ninja -S . -B build\terminal-ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake -DVCPKG_INSTALLED_DIR=%REPO_ROOT%\vcpkg_installed -DVCPKG_OVERLAY_PORTS=%VCPKG_OVERLAY_PORTS% -DZMUSIC_INCLUDE_DIR=%REPO_ROOT%\build\zmusic\include -DZMUSIC_LIBRARIES=%REPO_ROOT%\build\zmusic\build-ninja-ovl2\source\zmusiclite.lib"
```

Build:

```powershell
cmd /c "call C:\PROGRA~1\MICROS~1\2022\COMMUN~1\Common7\Tools\VsDevCmd.bat -arch=x64 -host_arch=x64 && cmake --build build\terminal-ninja --config RelWithDebInfo --target raze"
```

The resulting output is:

- `build\terminal-ninja\raze.exe`
- `build\terminal-ninja\raze.pk3`

The Windows CMake also stages the required audio runtime beside `raze.exe`:

- `zmusiclite.dll` and the adjacent codec DLLs from the local ZMusic build
- `OpenAL32.dll` from `bin\windows\runtime-deps` or the known local vcpkg locations

##### Windows release package

To build a stripped `Release` tree and stage a redistributable folder plus zip in one command, run:

```powershell
.\tools\dist\Build-WindowsReleasePackage.ps1
```

Default outputs:

- `build\terminal-release\raze.exe`
- `out\release\Duke-RT\`
- `out\release\Duke-RT.zip`

`Build-WindowsReleasePackage.cmd` is an equivalent entry point and forwards the same
parameters. Both initialize the installed Visual Studio C++ environment, build
ZMusic and the engine with production shaders, then package the result. CMake,
Ninja and Git must be available. Existing dependency layouts still work without a
settings file: `build\zmusic`, `build\vcpkg`, `vcpkg_installed`,
`libraries\NRD\_Shaders`, and the sibling `..\NRD-Sample` runtime/SDK tree.
These dependencies must already be prepared; this command does not bootstrap them.

For dependencies stored elsewhere, create the ignored file
`build\windows-release.local.json`. Only specify the paths that differ on your
machine, for example:

```json
{
  "ZMusicSourceDir": "D:/dependencies/ZMusic",
  "VcpkgRoot": "D:/dependencies/vcpkg",
  "VcpkgInstalledDir": "D:/dependencies/vcpkg_installed",
  "NrdShaderHeaderDir": "D:/dependencies/NRD/_Shaders",
  "NriRuntimeDir": "D:/dependencies/NRD-Sample/_Bin/Release",
  "FfxSdkRoot": "D:/dependencies/NRD-Sample/_Build/_deps/ffx-src"
}
```

Matching command-line parameters override local settings. `-ConfigPath` selects
another settings file. Optional settings also include `VsDevCmd`,
`DxcExecutable`, `CMakeExecutable`, `Jobs`, `ZMusicBuildDir`, `RazeBuildDir`,
`PackageDir`, and `ZipPath`. Visual Studio is discovered with `vswhere` unless
`VsDevCmd` or `RAZE_VSDEVCMD` is supplied; DXC uses the Vulkan SDK or PATH unless
explicitly configured. Relative paths always start at this checkout, even when
invoked from another working directory. The launcher, normal-map preparer,
tracked authored `release-overlay`, and `vcpkg-overlays` always come from this
checkout. Machine settings are optional and must not be committed.

Build directories must be separate subdirectories of this checkout's `build`;
package/zip outputs must be below `out\release`, with the zip outside the package
folder. Existing caches must belong to their configured source and use Ninja
Release. The default ZMusic output remains `build\zmusic\build-ninja-ovl2`, even
when its source is external. Concurrent release commands in one checkout are
rejected while compilation or packaging is in progress.

To restage or re-zip an already-built `Release` tree without rebuilding, run:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\dist\Build-WindowsReleasePackage.ps1 -SkipBuild
```

The staged package includes:

- `raze.exe` and `raze.pk3` from the `Release` build
- staged runtime DLLs such as `zmusiclite.dll`, `OpenAL32.dll`, NRI/NRD/FFX runtimes, and codec DLLs
- `launch-duke-rt.cmd`
- `tools\dist\Prepare-CommercialNormals.ps1`
- the tracked authored `release-overlay` (no imported normals, voxel models or GRP)
- the staged FidelityFX runtime license

The staged package includes `package\windows\launch-duke-rt.cmd`, `tools\dist\Prepare-CommercialNormals.ps1`, and `release-overlay`, and removes `*.pdb` files from the staged package.

`launch-duke-rt.cmd` resolves a valid `DUKE3D.GRP` once, stores the verified install root under `generated-content\state\content-preferences.json`, and passes the resolved file to Raze with `-gamegrp`. Optional World Tour normal-map import uses the same install root's `textures` folder; failure to find or convert normals does not block launch when `DUKE3D.GRP` is valid. Optional Cheello voxel staging can be controlled with `-VoxelAsk`, `-VoxelYes`, `-VoxelNo`, `-VoxelZip "path\to\voxel_duke3d.zip"`, and `-ForceVoxels`.

##### Fast rebuild command

Once `build\terminal-ninja` is configured, this is the normal rebuild command (Powershell, path needs updating for your install):

```powershell
cmd /c "call C:\PROGRA~1\MICROS~1\2022\COMMUN~1\Common7\Tools\VsDevCmd.bat -arch=x64 -host_arch=x64 && cmake --build build\terminal-ninja --config RelWithDebInfo --target raze"
```

##### Launch

```powershell
build\terminal-ninja\raze.exe
```

Example local overlay launch:

```powershell
build\terminal-ninja\raze.exe -file .\release-overlay
```
