# Building Linux releases

Build the Linux x86-64 release from a Git checkout using either script:

| Build host | Script |
| --- | --- |
| Linux | `tools/dist/Build-LinuxReleasePackage.sh` |
| Windows with WSL2 | `tools/dist/Build-LinuxReleasePackage.ps1` |

Both scripts compile the required components and create a `.tar.gz` package.
Run the commands below from the checkout root unless stated otherwise.

## Prerequisites

The Linux environment needs GCC/G++, Ninja, Git, CMake **3.30 or newer**,
Python **3.11 or newer**, and development libraries. On Windows, install these
inside the WSL distribution that will perform the build. The PowerShell wrapper
requires PowerShell 5.1 or newer and `wsl.exe` on Windows `PATH`.

For Ubuntu 24.04, run these commands in a Linux or WSL terminal:

```sh
sudo apt update
sudo apt install build-essential ninja-build git pkg-config python3 python3-venv \
  coreutils util-linux libsdl2-dev libgtk-3-dev libbz2-dev libvpx-dev \
  libasound2-dev libx11-dev libwayland-dev libopenal-dev libsndfile1-dev libmpg123-dev
```

If the distribution's CMake is older than 3.30, install a suitable version in a
Python virtual environment:

```sh
python3 -m venv "$HOME/.local/share/duke-rt-build-tools"
"$HOME/.local/share/duke-rt-build-tools/bin/pip" install 'cmake==3.31.10'
```

The examples below select that CMake explicitly; activating the virtual
environment is unnecessary. If a suitable Linux CMake is already on `PATH`,
omit `--cmake` or `-CMake`.

The first build requires internet access to download additional SDKs and build
tools. The scripts do not install system packages. GPU access and a Visual Studio
developer prompt are unnecessary for compilation.

## Prepare dependency inputs

Obtain the required ZMusic 1.3.0 source revision:

```sh
git clone https://github.com/zdoom/zmusic build/zmusic
git -C build/zmusic checkout cb15f4ba826ced1f558c202de80c377a913764b6
```

Also provide generated NRD shader headers containing the DXBC, DXIL and SPIR-V
variants. They must match the vendored `libraries/NRD` source and
`RAZE_NRD_CONFIGURATION` in `source/CMakeLists.txt`. Headers from a matching
Windows dependency build can be reused. The release scripts do not generate
these headers; supply their directory with `--nrd-shader-headers` or
`-NrdShaderHeaderDir`.

## Build on Linux

```sh
bash tools/dist/Build-LinuxReleasePackage.sh \
  --build-root "$HOME/build/duke-rt-linux-release" \
  --zmusic-source "$PWD/build/zmusic" \
  --nrd-shader-headers "/path/to/NRD/_Shaders" \
  --cmake "$HOME/.local/share/duke-rt-build-tools/bin/cmake" \
  --jobs 2
```

Replace the NRD path with the prepared header directory. Explicit relative paths
resolve from the terminal's current directory.

## Build on Windows with WSL2

Prepare the Linux prerequisites in the selected WSL distribution, then run this
from PowerShell in the Windows checkout:

```powershell
.\tools\dist\Build-LinuxReleasePackage.ps1 `
  -Distribution Ubuntu `
  -BuildRoot /home/yourname/build/duke-rt-linux-release `
  -ZMusicSourceDir .\build\zmusic `
  -NrdShaderHeaderDir C:\dependencies\NRD\_Shaders `
  -CMake /home/yourname/.local/share/duke-rt-build-tools/bin/cmake `
  -Jobs 2
```

Replace `Ubuntu`, `yourname` and the NRD path as appropriate. Omit `-Distribution`
to use the default WSL distribution. Directory arguments accept Windows paths
or absolute Linux paths accessible in that distribution. `-CMake` must select a
**Linux executable**; the Windows CMake installation is not used.

Keep the build directory on WSL's Linux filesystem for faster compilation.
The source checkout and dependency inputs may remain on Windows.

## Environment and build options

The Linux `PATH` must contain the compiler, Ninja, Git, Python, `pkg-config`,
`realpath` and `flock`. CMake is selected from that same `PATH` unless supplied
explicitly. No project-specific environment variables are required. Leave
`VULKAN_SDK` unset in the Linux build environment so an inherited Windows SDK
cannot override the downloaded Linux shader compiler.

| Setting | Linux option | PowerShell option | Default |
| --- | --- | --- | --- |
| Build directory | `--build-root` | `-BuildRoot` | `build/linux-release` |
| ZMusic source | `--zmusic-source` | `-ZMusicSourceDir` | `build/zmusic` |
| NRD headers | `--nrd-shader-headers` | `-NrdShaderHeaderDir` | `libraries/NRD/_Shaders` |
| CMake executable | `--cmake` | `-CMake` | `cmake` on Linux `PATH` |
| Parallel jobs | `--jobs` | `-Jobs` | `2` |
| Output archive | `--output` | `-OutputPath` | `out/release/duke-rt-linux-x86_64.tar.gz` |
| Package existing build | `--skip-build` | `-SkipBuild` | Disabled |
| Show help | `--help` | `-Help` | |

Default directory and output paths are relative to the checkout root. Choose
separate build directories for different checkouts and for Windows versus Linux;
CMake caches cannot be shared between them. Rerunning the same command builds
incrementally. Use `--skip-build` or `-SkipBuild` only to package an existing,
up-to-date build, keeping the same build and dependency paths.

The output archive must end in `.tar.gz`. The build writes a `.sha256` checksum
file beside it. Increase the job count only if sufficient memory is available.
