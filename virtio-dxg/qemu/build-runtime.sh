#!/usr/bin/env bash
# Builds the complete WINQ-EMU runtime (QEMU + virglrenderer, the recipe in
# runtime-build/) with the virtio-dxg device added, in MSYS2 UCRT64:
#
#   virtio-dxg/qemu/build-runtime.sh <output-dir>
#
# The official recipe runs unchanged except for one added step after the
# locked QEMU patches: virtio-dxg/qemu/prepare.py. runtime-build/ itself is
# not edited (a temporary copy of build.sh runs from that folder so its
# paths resolve), which keeps upstream merges conflict-free.
#
# Install by extracting <output-dir>/winq-emu-alpha10-portable.zip to
# C:\WINQ-EMU: the launcher prefers a user-managed runtime there (-winq).
set -euo pipefail
if [[ $# -ne 1 ]]; then
    echo "usage: $0 OUTPUT_DIRECTORY" >&2
    exit 2
fi
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root=$(cd "$here/../.." && pwd)
copy="$root/runtime-build/.build-with-virtio-dxg.sh"
trap 'rm -f "$copy"' EXIT

anchor='apply_locked_patches qemu "$qemu_source"'
grep -qxF "$anchor" "$root/runtime-build/build.sh" || {
    echo "runtime-build/build.sh changed: '$anchor' not found" >&2
    exit 1
}
awk -v anchor="$anchor" '
    { print }
    $0 == anchor {
        print "python \"$repo_root/virtio-dxg/qemu/prepare.py\" \"$qemu_source\""
    }' "$root/runtime-build/build.sh" >"$copy"
bash "$copy" "$1"
