#!/bin/sh
# Build source only. Runtime libraries remain separately installed local files.
set -eu
if [ "$#" -lt 3 ] || [ "$#" -gt 4 ]; then
    echo 'Usage: build_runtime_diagnostic.sh DirectX-Headers-directory libdxg-directory output-directory [4k|stream]' >&2
    exit 2
fi
base=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
headers=$(CDPATH= cd -- "$1" && pwd)
dxg=$(CDPATH= cd -- "$2" && pwd)
mkdir -p "$3"
output=$(CDPATH= cd -- "$3" && pwd)
profile_define=
timeline_define=
if [ "${WDDM_BRIDGE_TEST_GPU_TIMELINE:-0}" = 1 ]; then
    timeline_define=-DWDDM_BRIDGE_TEST_GPU_TIMELINE=1
fi
if [ "$#" -eq 4 ]; then
    case "$4" in
        4k) profile_define=-DWDDM_BRIDGE_4K_LAB=1 ;;
        stream) profile_define='-DWDDM_BRIDGE_4K_LAB=1 -DWDDM_BRIDGE_STREAM_LAB=1' ;;
        *) echo 'Only the explicit 4k and stream lab profiles are supported' >&2; exit 2 ;;
    esac
fi
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror $profile_define \
    -isystem "$headers/include" -isystem "$headers/include/wsl/stubs" -isystem "$dxg/include" \
    -shared -fPIC "$base/linux_ioctl_bridge.cpp" -ldl -pthread -o "$output/linux-ioctl-bridge.so"
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror $profile_define $timeline_define \
    -isystem "$headers/include" -isystem "$headers/include/wsl/stubs" \
    "$base/umd_context_probe.cpp" "$headers/src/dxguids.cpp" \
    -L/usr/lib/wsl/lib -Wl,-rpath,/usr/lib/wsl/lib -ld3d12 -ldxcore -ldl -o "$output/runtime-probe"
"${CC:-cc}" -static -std=c11 -Wall -Wextra -Werror -O2 "$base/runtime_guest_init.c" -o "$output/runtime-init"
if [ "${WDDM_BRIDGE_TEST_BROKER_ROUTE:-0}" = 1 ]; then
    "${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -pthread $profile_define \
        "$base/guest_graphics_broker.cpp" -o "$output/guest-graphics-broker"
fi
