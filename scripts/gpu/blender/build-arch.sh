#!/usr/bin/env bash
# Build an optional, locally patched Blender alongside the distribution package.
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
work=${1:?Usage: build-arch.sh BUILD_DIRECTORY [INSTALL_DIRECTORY]}
mkdir -p -- "$work"
work=$(cd -- "$work" && pwd)
prefix=${2:-"$HOME/.local/opt/blender-5.2.2-omarchy"}
mkdir -p -- "$prefix"
prefix=$(cd -- "$prefix" && pwd)
jobs=${JOBS:-4}
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo 'JOBS must be a positive integer' >&2; exit 1; }

blender_commit=d13f752e3b9c4f8c261cda552b1021f8bcc0382c
arch_commit=45cd72aaf75ac3c013a71217b3e5719a517b87e5
ffmpeg_sha512=ee67438e0c868eda0616ae444631ccbbce4484c37097875df037fb5db5341be13f1a9e886ba5a1cb514ef2764080542b90cf8d30942a4bf138a9b15e42b378a1

if [[ ! -d "$work/source" ]]; then
    GIT_LFS_SKIP_SMUDGE=1 git clone --depth 1 --branch v5.2.2 \
        https://projects.blender.org/blender/blender.git "$work/source"
fi
[[ $(git -C "$work/source" rev-parse HEAD) == "$blender_commit" ]] || {
    echo 'Unexpected Blender source commit; use a fresh build directory.' >&2; exit 1;
}
git -C "$work/source" lfs install --local
git -C "$work/source" lfs pull

if [[ ! -d "$work/arch-package" ]]; then
    git init "$work/arch-package"
    git -C "$work/arch-package" remote add origin \
        https://gitlab.archlinux.org/archlinux/packaging/packages/blender.git
    git -C "$work/arch-package" fetch --depth 1 origin "$arch_commit"
    git -C "$work/arch-package" checkout --detach FETCH_HEAD
fi
[[ $(git -C "$work/arch-package" rev-parse HEAD) == "$arch_commit" ]] || {
    echo 'Unexpected Arch packaging commit; use a fresh build directory.' >&2; exit 1;
}
printf '%s  %s\n' "$ffmpeg_sha512" "$work/arch-package/ffmpeg-9.patch" | sha512sum --check

apply_once() {
    if git -C "$work/source" apply --check "$1"; then
        git -C "$work/source" apply "$1"
    else
        git -C "$work/source" apply --reverse --check "$1"
    fi
}
apply_once "$script_dir/buffer-storage-fallback.patch"
apply_once "$work/arch-package/ffmpeg-9.patch"

# Arch supplies libraries and Python. This is not a self-contained Linux binary.
# GPU compute APIs and Vulkan are not available in the tested QEMU graphics path.
cmake -S "$work/source" -B "$work/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DWITH_LIBS_PRECOMPILED=OFF -DWITH_LINKER_MOLD=ON \
    -DWITH_INSTALL_PORTABLE=ON -DWITH_PYTHON_INSTALL=OFF \
    -DPYTHON_VERSION="$(python3 -c 'import sys; print("%d.%d" % sys.version_info[:2])')" \
    -DWITH_SYSTEM_GFLAGS=ON -DWITH_SYSTEM_GLOG=ON \
    -DWITH_CYCLES_DEVICE_CUDA=OFF -DWITH_CYCLES_DEVICE_OPTIX=OFF \
    -DWITH_CYCLES_DEVICE_HIP=OFF -DWITH_CYCLES_DEVICE_HIPRT=OFF \
    -DWITH_CYCLES_DEVICE_ONEAPI=OFF -DWITH_VULKAN_BACKEND=OFF \
    -DWITH_CYCLES_OSL=ON -DWITH_CYCLES_PATH_GUIDING=ON -DWITH_CYCLES_EMBREE=ON \
    -DWITH_CODEC_FFMPEG=ON -DWITH_USD=ON -DWITH_MATERIALX=ON \
    -DWITH_GTESTS=OFF
cmake --build "$work/build" --parallel "$jobs"
cmake --install "$work/build"
git -C "$work/source" diff > "$prefix/omarchy-source.patch"
pacman -Q > "$prefix/omarchy-build-packages.txt"
printf 'Blender %s\nArch packaging %s\n' "$blender_commit" "$arch_commit" > "$prefix/omarchy-source-revisions.txt"
echo "Built $prefix/blender; install its optional launcher with app_compat.py."
