# Linux renderer milestone

This milestone targets native Linux x86-64 using SDL and NRI Vulkan. It includes
NRD denoising, TAA, NIS sharpening, and NVIDIA DLSS Super Resolution (SR) and Ray
Reconstruction (RR). Native Linux gameplay has been reported working; image
quality, performance and DLSS coverage still need broader native GPU testing.
WSL software Vulkan startup is a separate,
limited check. Frame generation, FSR, XeSS and HDR are outside this milestone.

## Run the compiled package

Use `duke-rt-linux-x86_64.tar.gz`, not the Windows `raze.exe`. Extract the whole
archive and keep its libraries, shaders and data together. The archive contains
no commercial game data or third-party voxel pack. Supply your own `DUKE3D.GRP`.
The authored material, lighting, smoke and voxel-policy overlay is included and
always mounted, including when both optional imports are disabled.

The initial build uses Ubuntu 24.04, GCC 13 and glibc 2.39. Use Ubuntu 24.04 or a
compatible newer x86-64 distribution and a CPU with SSSE3 (required by NRD).
Older glibc distributions are not covered.
Install the distribution's Vulkan loader, SDL2, GTK3, OpenAL, libvpx, bzip2 and GCC
OpenMP (`libgomp`) runtime packages, plus Python 3.9 or newer for first-run setup.
Optional World Tour normal-map conversion needs Pillow (`python3-pil` on Ubuntu).
The package includes its matching NRI, ZMusic and DLSS libraries.
The NVIDIA driver and Vulkan ICD come from your Linux installation.

Before testing, run `vulkaninfo --summary`. It must identify your physical GPU.
`llvmpipe`/`lavapipe` is software rendering and does not validate native GPU
gameplay. For DLSS use an NVIDIA RTX GPU with a working native NVIDIA Vulkan
driver. Seeing a GPU in `nvidia-smi` alone does not establish Vulkan support.

```sh
tar -xzf duke-rt-linux-x86_64.tar.gz
cd duke-rt-linux-x86_64
sha256sum -c SHA256SUMS
ldd ./raze                       # no "not found" entries
./launch-duke-rt.sh
```

Run the launcher in a terminal for first-time prompts. It searches Steam's
standard and Flatpak installations and additional library manifests, or accepts
a manual game folder/GRP path. Steam is optional: any accessible installation
or copied supported GRP can be selected. Choices are saved in
`userdata/content-preferences.json`. Noninteractive runs use explicit/saved or
detected game data and skip optional providers without a saved choice.

```sh
./launch-duke-rt.sh --game-root "/path/to/World Tour"
./launch-duke-rt.sh --game-root "/path/to/DUKE3D.GRP" --normals no --voxels no
./launch-duke-rt.sh --normal-source "/path/to/World Tour/textures" --normals yes
./launch-duke-rt.sh --voxel-zip "/path/to/voxel_duke3d.zip" --voxels yes
```

Normals are converted from the user's World Tour installation. For voxels, setup
can open Cheello's official download page in the desktop browser; download the
ZIP there and enter its path. A `voxel_duke3d.zip` beside the launcher is also
recognized. Credits and source links are in `CONTENT-CREDITS.txt`; the pack's
readme is preserved after import. Missing dependencies, declined imports or
invalid optional archives do not prevent launch with a valid GRP.

Optional data lives in `userdata/content/normals` and `userdata/content/voxels`.
The authored `release-overlay` is mounted last so its rules stay authoritative.
`--normals no` and `--voxels no` also disable previously imported layers;
`yes` re-enables them, `ask` prompts again, and `--refresh` reimports enabled
providers. `--setup-only` prepares content without starting the engine.

Settings, saves, screenshots and timestamped logs stay in `userdata/`.
First launch selects windowed 1280x720, Vulkan, SDR and NRD/TAA/NIS.
Move the complete folder to relocate the setup; if the external GRP moved,
select it again with `--game-root`. Relative CLI paths refer to the terminal's
working directory. Pass engine arguments after `--`, for example:

```sh
./launch-duke-rt.sh -- +set nri_upscaler 2 +set nri_upscalermode 2
```

After entering a level, `lightoverlay_dumpresolved E1L1`, `nri_ptstatus` and
`materialoverlayprobe #00244` can confirm the mounted light and material data.
Linux paths and filenames are case-sensitive.

SDL supports X11 and Wayland native handles. To isolate display issues, retry
with `SDL_VIDEODRIVER=x11 ./launch-duke-rt.sh ...`, then try
`SDL_VIDEODRIVER=wayland` in a native Wayland session. Preserve each run's log.

## Native gameplay checks

Open the console with the backtick key. Run `nri_ptcaps` and `nri_ptstatus`; keep
the output. Confirm Vulkan and the intended GPU. Requested settings alone are
not proof that an upscaler initialized: inspect support, resolved mode and any
creation/fallback messages in the log.

Test each row separately, preferably restarting with its settings and a separate
log. `nri_upscalermode 2` means Quality; `0` means native resolution.

| Path | Console settings |
| --- | --- |
| NRD + TAA + NIS | `nri_upscaler 0; nri_denoise true; nri_nrddenoiser 1; nri_pttaa true; nri_postsharpen 1` |
| DLSS SR + NRD | `nri_upscaler 2; nri_upscalermode 2; nri_denoise true; nri_nrddenoiser 1; nri_pttaa false; nri_postsharpen 0` |
| DLSS RR | `nri_upscaler 3; nri_upscalermode 2; nri_denoise false; nri_pttaa false; nri_postsharpen 0` |

1. Load `map e3l6`, `map e1l5`, and `map e2l4`. Capture opening views with
   `screenshot`, then walk and turn through each scene. Check sky, textures,
   lighting, motion, denoising convergence and temporal trails.
2. On E1L1, fire explosive weapons or run `spawn DukeExplosion2`. Check the
   explosion light, sprite and cleanup after the effect ends.
3. Exercise menus, console, resize, fullscreen/windowed switching, alt-tab,
   save/load, level changes and exit/relaunch. Repeat with optional content on/off.
4. Play for at least 15 minutes per selected path. Watch for flicker, corruption,
   crashes, growing memory use or progressive slowdown. A longer session is
   needed before claiming prolonged stability.

Send the session logs, screenshots, GPU/driver information, distribution, display
session (X11/Wayland), resolution and exact settings. If a path fails, include
the shortest steps that reproduce it. Keep NRD/TAA/NIS results separate from
DLSS SR and RR results.

Linux sky and persistent texture caches validate manager ownership before
dereferencing wrappers. Unmanaged layer textures still upload through the
existing fallback, but their optional persistent signatures are unavailable;
native testing should include layered decals and watch their performance.

## Build and package a Linux release

Use `tools/dist/Build-LinuxReleasePackage.sh` on Linux, or
`tools/dist/Build-LinuxReleasePackage.ps1` on Windows with WSL2. Both run the same
Linux build: ZMusic, NRI, the engine and production shaders, followed by runtime
staging and the public tarball packager. No existing engine binary is required.
The PowerShell wrapper does not require a Visual Studio developer prompt.

Build prerequisites are Linux x86-64, GCC/G++, Ninja, Git, CMake 3.30 or newer
(tested with 3.31.10), Python 3.11 or newer, and development packages. On Ubuntu
24.04, install the system dependencies once:

```sh
sudo apt install build-essential ninja-build git pkg-config python3 python3-venv util-linux \
  libsdl2-dev libgtk-3-dev libbz2-dev libvpx-dev libasound2-dev \
  libx11-dev libwayland-dev libopenal-dev libsndfile1-dev libmpg123-dev
python3 -m venv "$HOME/.local/share/duke-rt-build-tools"
"$HOME/.local/share/duke-rt-build-tools/bin/pip" install 'cmake==3.31.10'
```

The scripts do not run sudo or install system packages. NRI's CMake configuration
downloads its pinned SDKs and Linux DXC on the first build, so that step requires
internet access. Build and runtime dependencies are separate from GPU drivers;
compilation does not need an exposed GPU.

Supply ZMusic 1.3.0 source and compatible **precompiled NRD shader headers**:

```sh
git clone https://github.com/zdoom/zmusic build/zmusic
git -C build/zmusic checkout cb15f4ba826ced1f558c202de80c377a913764b6
```

NRD headers must match the vendored `libraries/NRD` source and
`RAZE_NRD_CONFIGURATION` in `source/CMakeLists.txt`, including the DXBC, DXIL and
SPIR-V variants. They can be copied from a matching existing Windows dependency
build; this is the same prerequisite as the current Windows build. The script
does not generate these headers or establish their source/configuration identity
merely from their filenames. Supply them with `--nrd-shader-headers` or
`-NrdShaderHeaderDir`. Linux compilation and packaging then run without Windows
tools. A fresh machine still needs this dependency prepared first.

From a Linux checkout:

```sh
bash tools/dist/Build-LinuxReleasePackage.sh \
  --build-root "$HOME/build/duke-rt-linux-release" \
  --zmusic-source "$PWD/build/zmusic" \
  --nrd-shader-headers "/path/to/matching/NRD/_Shaders" \
  --cmake "$HOME/.local/share/duke-rt-build-tools/bin/cmake" --jobs 4
```

From the Windows checkout, using Linux dependencies in the selected WSL distro:

```powershell
.\tools\dist\Build-LinuxReleasePackage.ps1 -Distribution Ubuntu `
  -BuildRoot /home/yourname/build/duke-rt-linux-release `
  -ZMusicSourceDir C:\src\zmusic `
  -NrdShaderHeaderDir C:\src\NRD\_Shaders `
  -CMake /home/yourname/.local/share/duke-rt-build-tools/bin/cmake -Jobs 4
```

PowerShell path arguments accept Windows paths or absolute Linux paths. The
`-CMake` executable always runs inside Linux. The native script resolves relative
paths from the calling terminal. Prefer the WSL Linux filesystem for build caches
to reduce compilation overhead; the source checkout may remain on Windows.

By default, build caches go in `build/linux-release`, and the archive is
`out/release/duke-rt-linux-x86_64.tar.gz`, with a `.sha256` sidecar. Override the
archive with `--output` / `-OutputPath`. The default compiler concurrency is two
jobs; raise it only if memory allows. Rerunning performs an incremental build,
refreshing source-revision metadata before compilation.
`--skip-build` / `-SkipBuild` packages existing outputs from matching caches;
use the same dependency/build arguments, and omit this switch after source edits.
Incompatible source-root caches are rejected without deleting them. A lock covers
cache validation, compilation and packaging, including package-only mode. A
competing invocation using the same build root fails immediately with a busy
message; retry after the first finishes. The lock releases on success or failure.
Separate build roots can run independently; give them different output paths.

Use `--help` / `-Help` for options. The produced package includes player setup,
licenses and all authored materials/policies, while excluding GRP, imported
normals, voxel models and local userdata. Extract it and follow the launch and
native gameplay checks above. This build retains the host distribution's glibc
requirement; it is not an older-distribution compatibility build.

## Manual source build inputs

Windows can continue to orchestrate builds through WSL. Use a separate Linux
CMake build directory; never reuse a Windows CMake cache. Enable `HAVE_NRI=ON`.
The vendored NRI CMake project requires CMake 3.30 or later. Build it with
`NRI_ENABLE_VK_SUPPORT=ON`, `NRI_ENABLE_NIS_SDK=ON`, and
`NRI_ENABLE_NGX_SDK=ON`; leave FFX and XeSS disabled. Its dependency declarations
pin the official SDK downloads, including DLSS 310.5.3.
Install X11 and Wayland development headers before configuring NRI so it enables
both display backends, alongside the application's SDL2 and other build dependencies.

Build ZMusic for Linux, then configure the application with its installation in
`CMAKE_PREFIX_PATH`, the Linux DXC executable in `RAZE_DXC_EXECUTABLE`, and the
NRI runtime output directory in `RAZE_NRI_RUNTIME_DIR`. Use
`RAZE_NRI_SHADER_PROFILE=PRODUCTION`. NRD is compiled into the application and
currently consumes generated headers from `RAZE_NRD_SHADER_HEADER_DIR`; these
must match the vendored NRD source and `RAZE_NRD_CONFIGURATION` in
`source/CMakeLists.txt`. The initial setup reuses compatible generated NRD
headers from the Windows development dependencies.

The launcher starts the engine in the package directory so shader and runtime
discovery use the matching package. Keep the supplied license notices.

## Package an existing Linux runtime

With Python 3.11 or newer, stage a dependency-complete Linux runtime (executable,
PK3, NRI/DLSS/ZMusic libraries, soundfonts, production shaders and license notices):

```sh
python3 tools/dist/build_linux_package.py --runtime-dir /path/to/linux-runtime \
  --output /path/to/delivery/duke-rt-linux-x86_64.tar.gz
```

The packager adds the current launcher, setup helper, defaults, credits and
authored overlay. It excludes GRP, normal maps, voxel models and local userdata.
Use an extracted clean prior Linux package as the runtime input when only
updating setup/content. The archive and its checksum file are written together.
