#!/usr/bin/env bash
set -euo pipefail
# A small engineering artifact, not a publishable complete runtime.
output=$(mkdir -p "$1" && cd "$1" && pwd)
recipe=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
python "$recipe/validate-lock.py"
python "$recipe/prepare-renderer.py" "$output/source/virglrenderer"
source="$output/source/virglrenderer"
python "$recipe/test-win32-handles.py" "$source"
python "$recipe/test-host-memory.py" "$source"
python "$recipe/test-image-capabilities.py" "$source"
meson setup "$output/build" "$source" --buildtype=release --prefix=/ucrt64 -Dvenus=true -Dvideo=true -Dtests=false
meson compile -C "$output/build"
meson install -C "$output/build"
mkdir -p "$output/bin" "$output/source/build-recipe"
cp /ucrt64/bin/libvirglrenderer-1.dll "$output/bin/"
"$recipe/collect-dlls.sh" "$output/bin" "$output/dll-sources.txt"
python "$recipe/test-win32-handles-native.py" "$output/bin/libvirglrenderer-1.dll"
cp "$source/COPYING" "$output/COPYING"
cp "$recipe"/*.py "$recipe"/*.sh "$recipe"/*.json "$output/source/build-recipe/"
cp -R "$recipe/patches" "$output/source/build-recipe/"
python "$recipe/archive.py" "$output/bin" "$output/renderer-bin.zip" --epoch 1777269494
python "$recipe/archive.py" "$output/source" "$output/renderer-source.zip" --epoch 1777269494
(cd "$output" && sha256sum renderer-bin.zip renderer-source.zip >SHA256SUMS)
