#!/usr/bin/env bash
# Builds qemu-system-x86_64.exe with the virtio-dxg device for development
# (MSYS2 UCRT64): the runtime's locked WINQ-EMU QEMU source with the official
# patches from runtime-build/, plus this device. No virglrenderer, so the
# build takes minutes; the shipped runtime adds the device to
# runtime-build/build.sh's full recipe instead.
#
#   virtio-dxg/qemu/build-dev.sh <work-dir>        first build
#   virtio-dxg/qemu/build-dev.sh <work-dir>        again: refreshes the device
#                                                  sources and rebuilds
set -euo pipefail
if [[ $# -ne 1 ]]; then
    echo "usage: $0 WORK_DIRECTORY" >&2
    exit 2
fi
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root=$(cd "$here/../.." && pwd)
recipe="$root/runtime-build"
work=$(mkdir -p "$1" && cd "$1" && pwd)
source="$work/source"
build="$work/build"

lock_field() {
    python - "$recipe/sources.lock.json" "$@" <<'PY'
import json
import sys

value = json.load(open(sys.argv[1], encoding="utf-8"))
for key in sys.argv[2:]:
    value = value[key]
if isinstance(value, list):
    for entry in value:
        print(f"{entry['file']}\t{entry['sha256']}")
else:
    print(value)
PY
}

if [[ ! -d "$source/.git" ]]; then
    repository=$(lock_field qemu repository | tr -d '\r')
    commit=$(lock_field qemu commit | tr -d '\r')
    git init --quiet "$source"
    git -C "$source" config core.autocrlf false
    git -C "$source" config core.longpaths true
    git -C "$source" remote add origin "$repository"
    git -C "$source" fetch --quiet --depth=1 origin "$commit"
    git -C "$source" checkout --quiet --detach "$commit"
    while IFS=$'\t' read -r patch digest; do
        patch=${patch%$'\r'}
        digest=${digest%$'\r'}
        [[ -n "$patch" ]] || continue
        printf '%s  %s\n' "$digest" "$recipe/$patch" | sha256sum -c --quiet -
        git -C "$source" apply --index "$recipe/$patch"
        echo "applied $patch"
    done < <(lock_field qemu patches)
fi
python "$here/prepare.py" "$source"

if [[ ! -f "$build/build.ninja" ]]; then
    mkdir -p "$build"
    (
        cd "$build"
        "$source/configure" --target-list=x86_64-softmmu --enable-whpx \
            --enable-sdl --disable-opengl --disable-virglrenderer \
            --disable-gtk --disable-vnc --disable-libusb --enable-slirp \
            --disable-tools --disable-guest-agent --disable-docs \
            --disable-plugins
    )
fi
ninja -C "$build" qemu-system-x86_64.exe
echo "$build/qemu-system-x86_64.exe"
