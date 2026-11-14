#!/bin/bash
# Build the pinned virtio-dxg cohort without changing installed guest libraries.
# Run from a full checkout: build-in-guest.sh <mesa-26.2.6.tar.xz> <out.tar>
set -euo pipefail
[ "$#" -eq 2 ] || { echo "usage: $0 <mesa-26.2.6.tar.xz> <out.tar>" >&2; exit 2; }
here=$(cd -- "$(dirname -- "$0")" && pwd)
tarball=$(realpath -- "$1")
out=$(realpath -m -- "$2")
[ "$tarball" != "$out" ] || { echo 'output must differ from source archive' >&2; exit 2; }
jobs=${JOBS:-2}
case $jobs in 1|2|3|4|5|6|7|8) ;; *) echo 'JOBS must be 1..8' >&2; exit 2 ;; esac
recipe=$here/versions/26.2.6
work=${WORK:-$HOME/.cache/virtio-dxg-mesa}
mkdir -p -- "$work" "$(dirname -- "$out")"
build=$(mktemp -d "$work/build-26.2.6.XXXXXXXX")
pending=$(mktemp "${out}.pending.XXXXXXXX")
trap 'rm -f -- "$pending"' EXIT
python3 "$here/stage_source.py" "$tarball" "$build/source" --recipe "$recipe"
src=$build/source/mesa-26.2.6
python3 -m venv "$build/venv"
"$build/venv/bin/pip" install --disable-pip-version-check --only-binary=:all: \
    --require-hashes --no-deps -r "$here/build-tools.txt"
export PATH=$build/venv/bin:$PATH
meson setup "$build/compile" "$src" \
    --prefix=/usr --libdir=lib --buildtype=release --wrap-mode=default \
    --force-fallback-for=DirectX-Headers \
    -Db_ndebug=true -Dplatforms=x11,wayland \
    -Dgallium-drivers=d3d12,softpipe -Dvulkan-drivers=microsoft-experimental \
    -Dvulkan-manifest-per-architecture=true \
    -Dgallium-va=enabled -Dvideo-codecs=all -Dgallium-rusticl=false \
    -Dglx=dri -Degl=enabled -Dgbm=enabled -Dglvnd=enabled -Dgles1=disabled \
    -Dllvm=disabled -Dmicrosoft-clc=disabled \
    -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled \
    -Dbuild-tests=false -Dtools=[]
meson compile -C "$build/compile" -j "$jobs"
DESTDIR=$build/dest meson install -C "$build/compile" --no-rebuild
licenses=$build/dest/usr/share/doc/virtio-dxg/licenses
mkdir -p "$licenses"
cp -a "$src/licenses" "$licenses/Mesa"
cp "$src/docs/license.rst" "$licenses/Mesa-license.rst"
cp "$src/subprojects/DirectX-Headers-1.0/LICENSE" "$licenses/DirectX-Headers-LICENSE"
python3 "$here/record_build.py" "$src" "$build/dest/usr" "$recipe"
tar --hard-dereference --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner \
    -C "$build/dest/usr" -cf "$pending" lib share
mv -f -- "$pending" "$out"
printf 'Packed actual Mesa 26.2.6 into %s; build retained in %s\n' "$out" "$build"
