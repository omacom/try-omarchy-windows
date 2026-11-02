#!/usr/bin/env bash
set -euo pipefail
if [[ $# -ne 1 ]]; then
    echo 'usage: build_qemu.sh OUTPUT_DIRECTORY' >&2
    exit 2
fi
recipe=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$recipe/../.." && pwd)
output=$(mkdir -p "$1" && cd "$1" && pwd)
sdl_candidate=${QEMU_SDL_OPENGL_CANDIDATE:-0}
user_network=${QEMU_USER_NETWORK_CANDIDATE:-0}
if [[ "$sdl_candidate" != 0 && "$sdl_candidate" != 1 ]]; then
    echo 'QEMU_SDL_OPENGL_CANDIDATE must be 0 or 1' >&2
    exit 2
fi
if [[ "$user_network" != 0 && "$user_network" != 1 ]]; then
    echo 'QEMU_USER_NETWORK_CANDIDATE must be 0 or 1' >&2
    exit 2
fi
if [[ -e "$output/source" || -e "$output/build" ]]; then
    echo 'Use a fresh output directory' >&2
    exit 1
fi
mapfile -t locked < <(python - "$root/runtime-build/sources.lock.json" <<'PY'
import json, sys
qemu = json.load(open(sys.argv[1]))['qemu']
print(qemu['repository'])
print(qemu['commit'])
PY
)
repository=${locked[0]%$'\r'}
commit=${locked[1]%$'\r'}
git init --quiet "$output/source"
git -C "$output/source" remote add origin "$repository"
git -C "$output/source" fetch --quiet --depth=1 origin "$commit"
git -C "$output/source" -c core.autocrlf=false checkout --quiet --detach "$commit"
python "$recipe/prepare_qemu.py" "$output/source"
gl_option=--disable-opengl
slirp_option=--disable-slirp
if [[ "$user_network" == 1 ]]; then
    slirp_option=--enable-slirp
fi
if [[ "$sdl_candidate" == 1 ]]; then
    python "$recipe/prepare_sdl_candidate.py" "$output/source"
    cp "$output/source/sdl-candidate-provenance.json" "$output/sdl-candidate-provenance.json"
    {
        python "$root/runtime-build/test-sdl-input.py" "$output/source/ui/sdl2.c"
        python "$root/runtime-build/test-sdl-cursor.py" "$output/source"
        python "$root/runtime-build/test-sdl-keyboard-hook.py" "$output/source"
        python "$root/runtime-build/test-sdl-scanout.py" "$output/source/ui/sdl2-gl.c" --native-relay
    } | tee "$output/sdl-input-controls.log"
    gl_option=--enable-opengl
fi
mkdir "$output/build"
cd "$output/build"
compiler_options=()
if [[ "${QEMU_COMPILER_CACHE:-0}" == 1 ]]; then
    export CCACHE_DIR="$root/.qemu-ccache/${QEMU_CACHE_VARIANT:-local}"
    export CCACHE_BASEDIR="$root"
    export CCACHE_COMPILERCHECK=content
    ccache --max-size=1G
    ccache --zero-stats
    compiler_options=(--cc="ccache gcc")
fi
../source/configure --target-list=x86_64-softmmu --enable-whpx --enable-sdl \
    "${compiler_options[@]}" \
    --disable-gtk --disable-vnc "$gl_option" --disable-virglrenderer \
    --disable-tools --disable-guest-agent --disable-docs "$slirp_option" --disable-libusb
ninja -j4 qemu-system-x86_64.exe
if [[ "${QEMU_COMPILER_CACHE:-0}" == 1 ]]; then
    ccache --show-stats | tee "$output/ccache-stats.txt"
fi
mkdir -p "$output/bin" "$output/share"
cp qemu-system-x86_64.exe "$output/bin/"
cp -r "$output/source/pc-bios" "$output/share/qemu"
bash "$root/runtime-build/collect-dlls.sh" "$output/bin" "$output/dll-sources.txt"
pacman -Q > "$output/build-packages.txt"
cp config-host.mak "$output/"
cp config-host.h "$output/"
tar --exclude=.git -czf "$output/qemu-corresponding-source.tar.gz" -C "$output" source
echo 'Built engineering QEMU: WHPX, SDL and shared Windows section memory; no guest GPU backend'
if [[ "$sdl_candidate" == 1 ]]; then
    python "$recipe/verify_sdl_candidate.py" "$output/source" "$output/config-host.h"
    echo 'SDL/OpenGL candidate compiled with the official runtime input patches; GPU scanout integration remains unverified'
fi
