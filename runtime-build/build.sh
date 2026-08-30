#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 OUTPUT_DIRECTORY" >&2
    exit 2
fi

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
recipe="$repo_root/runtime-build"
lock="$recipe/sources.lock.json"
output=$(mkdir -p "$1" && cd "$1" && pwd)
work=$(mktemp -d "${RUNNER_TEMP:-/tmp}/try-omarchy-runtime.XXXXXX")
trap 'chmod -R u+w "$work" 2>/dev/null || true; rm -rf -- "$work"' EXIT

python "$recipe/validate-lock.py" "$lock"

lock_value() {
    python - "$lock" "$1" "$2" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    value = json.load(source)
print(value[sys.argv[2]][sys.argv[3]])
PY
}

source_date_epoch=$(python - "$lock" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    print(json.load(source)["sourceDateEpoch"])
PY
)
export SOURCE_DATE_EPOCH="$source_date_epoch"
export TZ=UTC
export LC_ALL=C

clone_locked() {
    local name=$1
    local destination=$2
    local repository commit base
    repository=$(lock_value "$name" repository)
    commit=$(lock_value "$name" commit)
    base=$(lock_value "$name" baseCommit)

    git init --quiet "$destination"
    git -C "$destination" remote add origin "$repository"
    git -C "$destination" fetch --quiet --depth=256 origin "$commit" "$base"
    git -C "$destination" checkout --quiet --detach "$commit"
    [[ $(git -C "$destination" rev-parse HEAD) == "$commit" ]]
    git -C "$destination" merge-base --is-ancestor "$base" "$commit" || {
        echo "$name commit is not based on its locked upstream commit" >&2
        exit 1
    }
}

qemu_source="$work/qemu"
virgl_source="$work/virglrenderer"
clone_locked qemu "$qemu_source"
clone_locked virglrenderer "$virgl_source"
git -C "$qemu_source" submodule update --init --recursive --depth=1

virgl_build="$work/virgl-build"
meson setup "$virgl_build" "$virgl_source" \
    --buildtype=release \
    --prefix=/ucrt64 \
    -Dvenus=true \
    -Dvideo=true \
    -Dtests=false
meson compile -C "$virgl_build"
meson install -C "$virgl_build"

qemu_build="$work/qemu-build"
mkdir -p "$qemu_build"
(
    cd "$qemu_build"
    "$qemu_source/configure" \
        --target-list=x86_64-softmmu \
        --prefix=/ucrt64 \
        --enable-whpx \
        --enable-opengl \
        --enable-virglrenderer \
        --enable-slirp \
        --disable-docs \
        --disable-plugins
)
meson compile -C "$qemu_build"

stage="$work/install"
DESTDIR="$stage" meson install -C "$qemu_build"
installed_windowless=$(find "$stage" -type f -name qemu-system-x86_64w.exe -print -quit)
[[ -n "$installed_windowless" ]] || {
    echo "QEMU install did not produce qemu-system-x86_64w.exe" >&2
    exit 1
}
installed_bin=$(dirname "$installed_windowless")
installed_firmware=$(find "$stage" -type f -name bios-256k.bin -print -quit)
[[ -n "$installed_firmware" ]] || {
    echo "QEMU install did not produce bios-256k.bin" >&2
    exit 1
}
installed_share=$(dirname "$installed_firmware")
runtime="$work/runtime"
mkdir -p "$runtime/bin/share" "$runtime/LICENSES/qemu" \
    "$runtime/LICENSES/virglrenderer" "$runtime/LICENSES/msys2" \
    "$runtime/provenance"

for binary in qemu-system-x86_64.exe qemu-system-x86_64w.exe qemu-img.exe; do
    [[ -f "$installed_bin/$binary" ]] || {
        echo "QEMU install did not produce $binary" >&2
        exit 1
    }
    cp "$installed_bin/$binary" "$runtime/bin/"
done
[[ -d "$installed_share" ]] || {
    echo "QEMU install did not produce its firmware directory" >&2
    exit 1
}
cp -R "$installed_share/." "$runtime/bin/share/"
cp /ucrt64/bin/libvirglrenderer-1.dll "$runtime/bin/"

echo "Collecting runtime DLLs"
"$recipe/collect-dlls.sh" "$runtime/bin" "$runtime/provenance/dll-sources.txt"

for license in COPYING COPYING.LIB LICENSE; do
    [[ -f "$qemu_source/$license" ]] || {
        echo "QEMU source is missing $license" >&2
        exit 1
    }
    cp "$qemu_source/$license" "$runtime/LICENSES/qemu/$license"
done
[[ -f "$virgl_source/COPYING" ]] || {
    echo "virglrenderer source is missing COPYING" >&2
    exit 1
}
cp "$virgl_source/COPYING" "$runtime/LICENSES/virglrenderer/COPYING"
echo "Collected project licenses"

used_packages="$runtime/provenance/msys2-packages.txt"
: >"$used_packages"
while IFS= read -r source_path; do
    [[ -n "$source_path" ]] || continue
    owner=$(pacman -Qoq "$source_path" 2>/dev/null || true)
    if [[ -z "$owner" || "$owner" == *$'\n'* ]]; then
        echo "Could not resolve one package owner for $source_path" >&2
        exit 1
    fi
    package_line=$(pacman -Q "$owner" 2>/dev/null || true)
    if [[ -z "$package_line" ]]; then
        echo "Could not read the package version for $owner" >&2
        exit 1
    fi
    printf '%s\n' "$package_line" >>"$used_packages"
done <"$runtime/provenance/dll-sources.txt"
sort -u -o "$used_packages" "$used_packages"

echo "Collecting dependency licenses"
while read -r package _version; do
    license_root="$runtime/LICENSES/msys2/$package"
    while IFS= read -r license_path; do
        [[ -f "$license_path" ]] || continue
        relative=${license_path#*/share/licenses/}
        mkdir -p "$license_root/$(dirname "$relative")"
        cp "$license_path" "$license_root/$relative"
    done < <(pacman -Qql "$package" | grep '/share/licenses/' || true)
done <"$used_packages"
echo "Collected licenses for $(wc -l <"$used_packages") runtime packages"

cp "$lock" "$runtime/provenance/sources.lock.json"
pacman -Q | sort >"$runtime/provenance/build-environment-packages.txt"
cat >"$runtime/README.txt" <<'EOF'
WINQ-EMU runtime for Try Omarchy

This portable runtime contains the patched QEMU and virglrenderer components
used by Try Omarchy. Exact source commits, binary hashes, dependency versions,
and licenses are included under provenance and LICENSES.

The corresponding source archive is published beside this runtime artifact.
EOF

source_bundle="$work/source"
echo "Preparing corresponding source"
mkdir -p "$source_bundle/qemu" "$source_bundle/virglrenderer" \
    "$source_bundle/build-recipe"
tar -C "$qemu_source" --exclude=.git --exclude='*/.git' -cf - . | \
    tar -C "$source_bundle/qemu" -xf -
tar -C "$virgl_source" --exclude=.git --exclude='*/.git' -cf - . | \
    tar -C "$source_bundle/virglrenderer" -xf -
cp "$recipe"/*.py "$recipe"/*.sh "$recipe"/*.json "$recipe"/*.txt \
    "$source_bundle/build-recipe/"

manifest="$runtime/provenance/runtime-manifest.json"
echo "Creating runtime archives"
python "$recipe/manifest.py" "$runtime" "$lock" "$used_packages" "$manifest"
runtime_zip="$output/winq-emu-alpha10-portable.zip"
source_zip="$output/winq-emu-alpha10-source.zip"
python "$recipe/archive.py" "$runtime" "$runtime_zip" --epoch "$source_date_epoch"
python "$recipe/archive.py" "$source_bundle" "$source_zip" --epoch "$source_date_epoch"
(
    cd "$output"
    sha256sum "$(basename "$runtime_zip")" "$(basename "$source_zip")" >SHA256SUMS
)

mkdir -p "$output/smoke"
cp "$runtime/bin/qemu-system-x86_64.exe" "$output/smoke/"
cp "$runtime/bin"/*.dll "$output/smoke/"
python "$recipe/verify.py" "$output"
echo "Built $(du -h "$runtime_zip" | cut -f1) runtime and $(du -h "$source_zip" | cut -f1) source archive"
