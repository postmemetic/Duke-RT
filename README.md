# Duke-RT

![Duke-RT gameplay](images/20.png)

Duke-RT is a fork of Raze that adds a new ray-tracing render backend based on [NVIDIA NRI](https://github.com/NVIDIA-RTX/NRI). The existing Build-engine game support from Raze remains the foundation, while this fork focuses on path tracing, RT renderer bring-up, lighting authoring, custom material authoring, denoising/upscaling integration, and backend diagnostics. It also includes tooling and overlay workflows so users can create their own material and lighting rules for Duke content. It only works on Windows due to reliance on libraries for DLSS, frame generation, denoising, etc. [Watch a somewhat recent gameplay video](https://www.youtube.com/watch?v=7z7txcZg2q0).

![Duke-RT gameplay](images/EmergeFromSmoke.gif)

The renderer supports both Direct3D 12 and Vulkan, although feature support is more complete for D3D12. It's recommended that you play in D3D12 and HDR if possible!

![Duke-RT gameplay](images/1.png)

**Check out the linked project docs below to make your own lighting rules and material overrides!**

## How To Run

- Launch `launch-duke-rt.cmd` from the packaged release folder instead of starting `raze.exe` directly.
- You should own and have installed a Duke Nukem 3D release with a valid `DUKE3D.GRP`; `Duke Nukem 3D: 20th Anniversary World Tour` remains the intended visual baseline.
- On first run, the launcher will either confirm the detected Duke Nukem 3D install or ask for the folder containing `DUKE3D.GRP`; the verified path is reused on later runs.
- If auto-detection picks the wrong install, launch with `launch-duke-rt.cmd -GameRoot "D:\path\to\Duke Nukem 3D"`.
- When the launcher detects World Tour normals, let it copy them into the packaged `release-overlay`. That is the intended setup for the current visual baseline.
- Duke-RT can still run without those copied normals. The launcher warns and continues when the selected Duke Nukem 3D install does not include World Tour normal-map data.
- The launcher can also guide you through installing Cheello's Voxel Duke 3D pack into the local `release-overlay`. If `voxel_duke3d.zip` is beside `launch-duke-rt.cmd`, it is used automatically; otherwise the launcher can open the ModDB page and then unpack the downloaded archive.
- Other Build-engine games may possibly launch because the underlying Raze game support is still present, but that path is not tested here and is not supported yet.

## Current Status

Duke-RT is work in progress. The core renderer is in, with full support for modern graphics APIs and libraries like D3D12, DLSS Super Resolution and Ray Reconstruction, etc. I've also gone through and **remastered Duke episodes 1 and 2 with PBR materials (based on the originals) as well as updated lighting**. Episode 3 is not yet done. There are some known issues that are on my radar but I have yet to tackle.

![Duke-RT gameplay](images/38.png)

Known high-priority issues:
- slow perf on the start of level 1 due to large voxel objects
- remaining transport-driven non-euclidean edge cases in `E5L1`
- slow CPU-side perf on maps with lots of geometry movement like `E1L4` and `E2L7`
- sometimes you get stuck on the level end screen rather than transitioning to the next level
- there's an occasional crash on multiple level transitions in a session

Known lower-priority issues:
- flickering material state for the vent at the start of `E1L4`
- the last scene panning sequence no longer appears on surveillance camera screens
- framegen only works with D3D12

I also have a bunch of features I'd like to tackle in the future, including some renderer improvements, as well as a complete pass on the other main Duke 3D episodes.

Upcoming feature work:
- thorough material and lighting pass on Episode 3
- volumetrics such as rocket smoke
- water surfaces
- proper mirror replacements
- better glass

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

These are a bit of a mess and I haven't revalidated them for a newcomer to the repo recently. Also please note that only Windows is supported for Duke-RT!

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
.\tools\dist\Build-WindowsReleasePackage.cmd
```

Default outputs:

- `build\terminal-release\raze.exe`
- `out\release\Duke-RT\`
- `out\release\Duke-RT.zip`

To restage or re-zip an already-built `Release` tree without rebuilding, run:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\dist\Build-WindowsReleasePackage.ps1 -SkipBuild
```

The staged package includes:

- `raze.exe` and `raze.pk3` from the `Release` build
- staged runtime DLLs such as `zmusiclite.dll`, `OpenAL32.dll`, NRI/NRD/FFX runtimes, and codec DLLs
- `launch-duke-rt.cmd`
- `tools\dist\Prepare-CommercialNormals.ps1`
- `release-overlay`

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
build\terminal-ninja\raze.exe -file M:\Raze\default-overlay
```
