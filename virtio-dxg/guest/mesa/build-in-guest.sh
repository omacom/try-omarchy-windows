#!/bin/bash
# Builds virtio-dxg's Mesa inside the Arch guest it is for, as an ordinary
# user (no packages are installed: meson, ninja and mako come from a private
# Python venv, DirectX-Headers from Mesa's own wrap).
#
#   build-in-guest.sh <mesa-X.Y.Z.tar.xz> <out.tar> [patch...]
#
# The library version string is the guest's own (from its libgallium), so
# the result is a complete, self-consistent replacement set: libgallium,
# EGL, GBM, the DRI loader, GLX, the d3d12 VA-API driver and Dozen
# (Vulkan on D3D12), packed as <out.tar> (usr/-relative lib/ and
# share/vulkan/) and selected per session with LD_LIBRARY_PATH,
# LIBGL_DRIVERS_PATH and GBM_BACKENDS_PATH.
set -euo pipefail
tarball=$1 out=$2; shift 2
work=${WORK:-$HOME/.cache/virtio-dxg-mesa}
mkdir -p "$work"

if [ ! -x "$work/venv/bin/meson" ]; then
    python3 -m venv "$work/venv"
    "$work/venv/bin/pip" install -q meson ninja mako packaging pyyaml
fi
export PATH=$work/venv/bin:$PATH

src=$work/$(basename "$tarball" .tar.xz)
rm -rf "$src"
tar -xJf "$tarball" -C "$work"
for p in "$@"; do
    patch -d "$src" -p1 --forward < "$p"
done

# Same version string as the system Mesa (libgallium-<version>.so).
system=$(ls /usr/lib/libgallium-*.so | head -1)
version=$(basename "$system" .so); version=${version#libgallium-}
echo "$version" > "$src/VERSION"

meson setup "$src/build" "$src" \
    --prefix=/usr --libdir=lib --buildtype=release --wrap-mode=default \
    -Db_ndebug=true \
    -Dplatforms=x11,wayland \
    -Dgallium-drivers=d3d12,softpipe \
    -Dvulkan-drivers=microsoft-experimental \
    -Dgallium-va=enabled -Dvideo-codecs=all \
    -Dgallium-rusticl=false \
    -Dglx=dri -Degl=enabled -Dgbm=enabled -Dglvnd=enabled -Dgles1=disabled \
    -Dllvm=disabled -Dmicrosoft-clc=disabled \
    -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled \
    -Dbuild-tests=false -Dtools=[]
ninja -C "$src/build"
rm -rf "$work/dest"
DESTDIR=$work/dest ninja -C "$src/build" install >/dev/null

# A tarball, not a tree: the library symlinks would not survive a 9p share
# backed by Windows.
echo "$version" > "$work/dest/usr/lib/virtio-dxg-mesa-version"
tar -C "$work/dest/usr" -cf "$out.tmp" lib $(cd "$work/dest/usr" && ls -d share/vulkan 2>/dev/null)
mv "$out.tmp" "$out"
echo "packed Mesa $version into $out"
