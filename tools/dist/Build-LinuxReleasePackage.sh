#!/usr/bin/env bash
# Native Linux build owner; the PowerShell entry point forwards here through WSL.
set -euo pipefail

info() { printf '[linux-release] %s\n' "$*"; }
die() { printf '[linux-release] ERROR: %s\n' "$*" >&2; exit 1; }
usage() {
    cat <<'EOF'
Usage: bash tools/dist/Build-LinuxReleasePackage.sh [options]
Build ZMusic, NRI and Duke-RT, then create the public Linux tarball and checksum.

  --build-root DIR          Separate Linux caches (default: <repo>/build/linux-release)
  --zmusic-source DIR       ZMusic 1.3.0 source (default: <repo>/build/zmusic)
  --nrd-shader-headers DIR  Compatible generated headers (default: <repo>/libraries/NRD/_Shaders)
  --cmake FILE             Linux CMake >= 3.30 (default: cmake in PATH)
  --jobs N                 Parallel compiler jobs (default: 2)
  --output FILE.tar.gz     Archive (default: <repo>/out/release/duke-rt-linux-x86_64.tar.gz)
  --skip-build             Package existing outputs from these matching caches
  --help                   Show this help

Run on Linux x86-64 with the development packages listed in LINUX.md.
NRI downloads its pinned SDKs/DXC on first configure. NRD's generated DXBC,
DXIL and SPIR-V headers must already match the vendored NRD source/configuration.
Relative paths resolve from the caller's working directory. No sudo is used.
EOF
}

product=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
build_root="$product/build/linux-release"
zmusic_source="$product/build/zmusic"
nrd_headers="$product/libraries/NRD/_Shaders"
cmake=cmake
jobs=2
output="$product/out/release/duke-rt-linux-x86_64.tar.gz"
skip_build=0
while (($#)); do
    case "$1" in
        --help|-h) usage; exit 0 ;;
        --skip-build) skip_build=1; shift ;;
        --build-root|--zmusic-source|--nrd-shader-headers|--cmake|--jobs|--output)
            (($# >= 2)) && [[ -n "$2" ]] || die "Missing value for $1"
            case "$1" in
                --build-root) build_root=$2 ;;
                --zmusic-source) zmusic_source=$2 ;;
                --nrd-shader-headers) nrd_headers=$2 ;;
                --cmake) cmake=$2 ;;
                --jobs) jobs=$2 ;;
                --output) output=$2 ;;
            esac
            shift 2 ;;
        *) die "Unknown argument: $1 (use --help)" ;;
    esac
done

[[ $(uname -s) == Linux && $(uname -m) == x86_64 ]] || die 'Linux x86-64 is required.'
[[ $jobs =~ ^[1-9][0-9]*$ ]] || die '--jobs must be a positive integer.'
[[ $output == *.tar.gz ]] || die '--output must end in .tar.gz.'
for tool in python3 ninja git realpath flock; do
    command -v "$tool" >/dev/null || die "Required tool not found: $tool (see LINUX.md)"
done
command -v "$cmake" >/dev/null || die "CMake not found: $cmake"
python3 -c 'import sys; sys.exit(0 if sys.version_info >= (3, 11) else "Packaging requires Python 3.11+")'
cmake_version=$("$cmake" --version)
[[ $cmake_version =~ cmake\ version\ ([0-9]+)\.([0-9]+) ]] || die 'Cannot determine CMake version.'
(( BASH_REMATCH[1] > 3 || (BASH_REMATCH[1] == 3 && BASH_REMATCH[2] >= 30) )) || die 'CMake 3.30+ is required; use --cmake to select it.'

build_root=$(realpath -m -- "$build_root")
zmusic_source=$(realpath -m -- "$zmusic_source")
nrd_headers=$(realpath -m -- "$nrd_headers")
output=$(realpath -m -- "$output")
nri_source="$product/libraries/NRIFramework/External/NRI"
[[ -f "$zmusic_source/CMakeLists.txt" && -f "$zmusic_source/include/zmusic.h" ]] || die "ZMusic source not found: $zmusic_source (see LINUX.md)"
for backend in dxbc dxil spirv; do
    [[ -f "$nrd_headers/REFERENCE_Copy.cs.$backend.h" ]] || die "Missing compatible NRD $backend headers in $nrd_headers; see LINUX.md."
done

cache_value() { sed -n "s/^$2:[^=]*=//p" "$1/CMakeCache.txt"; }
require_cache() {
    [[ -f "$1/CMakeCache.txt" ]] || die "Missing build cache: $1; run without --skip-build."
    [[ $(cache_value "$1" "$2") == "$3" ]] || die "Cache $1 has incompatible $2; expected '$3'. Select a fresh --build-root."
}
check_source() {
    if [[ -f "$1/CMakeCache.txt" ]]; then
        require_cache "$1" CMAKE_HOME_DIRECTORY "$2"
        require_cache "$1" CMAKE_GENERATOR Ninja
    fi
}
mkdir -p -- "$build_root"
# Keep the lock file in place: unlinking it could let callers lock different inodes.
# The descriptor stays open through packaging and closes on every exit path.
exec 9>"$build_root/.linux-release.lock"
flock -n 9 || die "Build root is busy: $build_root; retry after the other invocation finishes."
check_source "$build_root/nri-build" "$nri_source"
check_source "$build_root/zmusic-build" "$zmusic_source"
check_source "$build_root/raze-build" "$product"

if (( ! skip_build )); then
    info 'Configuring NRI (first run downloads its pinned dependencies)'
    "$cmake" -S "$nri_source" -B "$build_root/nri-build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DNRI_STATIC_LIBRARY=OFF \
        -DNRI_ENABLE_VK_SUPPORT=ON -DNRI_ENABLE_NIS_SDK=ON -DNRI_ENABLE_NGX_SDK=ON \
        -DNRI_ENABLE_FFX_SDK=OFF -DNRI_ENABLE_XESS_SDK=OFF \
        -DNRI_ENABLE_XLIB_SUPPORT=ON -DNRI_ENABLE_WAYLAND_SUPPORT=ON \
        "-DCMAKE_LIBRARY_OUTPUT_DIRECTORY=$build_root/nri-runtime" \
        "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$build_root/nri-runtime" \
        "-DNRI_SHADERS_PATH=$build_root/nri-shaders"
fi
for option in VK_SUPPORT NIS_SDK NGX_SDK XLIB_SUPPORT WAYLAND_SUPPORT; do
    require_cache "$build_root/nri-build" "NRI_ENABLE_$option" ON
done
require_cache "$build_root/nri-build" CMAKE_BUILD_TYPE Release
require_cache "$build_root/nri-build" CMAKE_LIBRARY_OUTPUT_DIRECTORY "$build_root/nri-runtime"

if (( ! skip_build )); then
    "$cmake" --build "$build_root/nri-build" --target NRI --parallel "$jobs"
    info 'Building ZMusic 1.3.0'
    "$cmake" -S "$zmusic_source" -B "$build_root/zmusic-build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON -DDYN_SNDFILE=ON -DDYN_MPG123=ON
    "$cmake" --build "$build_root/zmusic-build" --target zmusiclite --parallel "$jobs"
fi
require_cache "$build_root/zmusic-build" CMAKE_BUILD_TYPE Release
zmusic_lib="$build_root/zmusic-build/source/libzmusiclite.so"
[[ -f "$zmusic_lib.1.3.0" ]] || die 'Expected ZMusic 1.3.0 build output; use the source revision documented in LINUX.md.'
dxc=$(cache_value "$build_root/nri-build" SHADERMAKE_DXC_PATH)
[[ -x "$dxc" ]] || die "Linux DXC was not found in NRI's SHADERMAKE_DXC_PATH: $dxc"

if (( ! skip_build )); then
    info 'Building Duke-RT and production shaders'
    "$cmake" -S "$product" -B "$build_root/raze-build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DHAVE_NRI=ON -DHAVE_VULKAN=OFF -DHAVE_GLES2=OFF \
        -DPK3_QUIET_ZIPDIR=ON -DRAZE_NRI_SHADER_PROFILE=PRODUCTION \
        "-DZMUSIC_INCLUDE_DIR=$zmusic_source/include" "-DZMUSIC_LIBRARIES=$zmusic_lib" \
        "-DRAZE_DXC_EXECUTABLE=$dxc" "-DRAZE_NRD_SHADER_HEADER_DIR=$nrd_headers" \
        "-DRAZE_NRI_RUNTIME_DIR=$build_root/nri-runtime"
    "$cmake" --build "$build_root/raze-build" --target raze --parallel "$jobs"
fi
require_cache "$build_root/raze-build" CMAKE_BUILD_TYPE Release
require_cache "$build_root/raze-build" HAVE_NRI ON
require_cache "$build_root/raze-build" RAZE_NRI_SHADER_PROFILE PRODUCTION
require_cache "$build_root/raze-build" RAZE_NRD_SHADER_HEADER_DIR "$nrd_headers"
require_cache "$build_root/raze-build" ZMUSIC_LIBRARIES "$zmusic_lib"
require_cache "$build_root/raze-build" RAZE_NRI_RUNTIME_DIR "$build_root/nri-runtime"

info 'Staging compiled runtime and license notices'
stage=$(mktemp -d -- "$build_root/package-runtime.XXXXXXXX")
trap 'rm -rf -- "$stage"' EXIT
copy() { mkdir -p -- "$(dirname -- "$2")"; cp -L -- "$1" "$2"; }
for file in raze raze.pk3; do
    copy "$build_root/raze-build/$file" "$stage/$file"
done
copy "$build_root/nri-runtime/libNRI.so" "$stage/libNRI.so"
for file in "$build_root/nri-runtime"/libnvidia-ngx-dlss.so.* "$build_root/nri-runtime"/libnvidia-ngx-dlssd.so.*; do
    copy "$file" "$stage/${file##*/}"
done
for suffix in '' .1 .1.3.0; do
    copy "$zmusic_lib$suffix" "$stage/libzmusiclite.so$suffix"
done
cp -R -- "$build_root/raze-build/soundfonts" "$stage/soundfonts"
mkdir -p -- "$stage/shaders" "$stage/licenses"
cp -R -- "$build_root/raze-build/shaders/nri" "$stage/shaders/nri"
cp -R -- "$product/package/common" "$stage/licenses/product"
copy "$product/AUTHORS.md" "$stage/licenses/product/AUTHORS.md"
copy "$product/package/common/gamecontrollerdb.txt" "$stage/gamecontrollerdb.txt"
# Collect the tracked source notices without copying build caches or local assets.
git -C "$product" ls-files -z | while IFS= read -r -d '' relative; do
    name=${relative##*/}
    case "${name,,}" in
        *license*|*copying*) copy "$product/$relative" "$stage/licenses/source/$relative" ;;
    esac
done
for dependency in ngx vma vulkan_headers nvtx; do
    source="$build_root/nri-build/_deps/$dependency-src"
    [[ -d "$source" ]] || die "Missing dependency license source: $source"
    found=0
    for file in "$source"/*; do
        [[ -f "$file" ]] || continue
        name=${file##*/}
        case "${name,,}" in
            *license*|*copying*) copy "$file" "$stage/licenses/$dependency/$name"; found=1 ;;
        esac
    done
    ((found)) || die "Missing license notice for $dependency"
done
cp -R -- "$zmusic_source/licenses" "$stage/licenses/zmusic"
python3 "$product/tools/dist/build_linux_package.py" --runtime-dir "$stage" --output "$output"
info "Package ready: $output"
info "Checksum: $output.sha256"
