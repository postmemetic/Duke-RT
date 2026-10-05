# Linux renderer milestone

This milestone targets native Linux x86-64 using SDL and NRI Vulkan. It includes
NRD denoising, TAA, NIS sharpening, and NVIDIA DLSS Super Resolution (SR) and Ray
Reconstruction (RR). Linux path-traced gameplay, image quality, and DLSS dispatch
still require testing on a native GPU. WSL software Vulkan startup is a separate,
limited check. Frame generation, FSR, XeSS and HDR are outside this milestone.

## Run the compiled package

Use `duke-rt-linux-x86_64.tar.gz`, not the Windows `raze.exe`. Extract the whole
archive and keep its libraries, shaders and data together. The archive contains
no commercial game data. Supply your own `DUKE3D.GRP`.

The initial build uses Ubuntu 24.04, GCC 13 and glibc 2.39. Use Ubuntu 24.04 or a
compatible newer x86-64 distribution and a CPU with SSSE3 (required by NRD).
Older glibc distributions are not covered.
Install the distribution's Vulkan loader, SDL2, GTK3, OpenAL, libvpx, bzip2 and GCC
OpenMP (`libgomp`) runtime packages.
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
mkdir -p "$HOME/duke-rt-linux-test"
./launch-duke-rt.sh \
  -gamegrp "/absolute/path/to/DUKE3D.GRP" \
  -config "$HOME/duke-rt-linux-test/raze.ini" \
  +logfile "$HOME/duke-rt-linux-test/session.log" \
  +set nri_api vulkan +set nri_framegen false +set nri_ptoutputmode 0 \
  +set nri_upscaler 0 +set nri_denoise true +set nri_nrddenoiser 1 \
  +set nri_pttaa true +set nri_postsharpen 1
```

Use a fresh Linux configuration first. To test with your voxel/material overlay,
add `-file "/absolute/path/to/full-voxel-overlay"`. That overlay is external to
this package. Linux paths and filenames are case-sensitive. Do not copy Windows
DLLs into the Linux package.

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
   save/load, level changes and exit/relaunch. Repeat with your overlay.
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

## Source build inputs

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

The launcher changes to the package directory so shader and optional runtime
discovery use the matching package. Keep the supplied license notices.
