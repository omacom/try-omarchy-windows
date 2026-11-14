#!/bin/sh
# Compile only. The probe runs explicitly inside an owned graphical guest.
set -eu
if [ "$#" -ne 3 ]; then
    echo 'Usage: build_gpu_va_pressure_probe.sh DirectX-Headers-source Mesa-source output-directory' >&2
    exit 2
fi
base=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
directx=$(CDPATH= cd -- "$1" && pwd)
mesa=$(CDPATH= cd -- "$2" && pwd)
mkdir -p "$3"
output=$(CDPATH= cd -- "$3" && pwd)
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -shared -fPIC \
    -isystem "$directx/include" -isystem "$directx/include/wsl/stubs" \
    -isystem "$mesa/include" -isystem "$mesa/src/gallium/drivers/d3d12" \
    "$base/d3d12_va_pressure_probe.cpp" "$directx/src/dxguids.cpp" \
    -ldl -pthread -o "$output/d3d12-va-pressure-probe.so"
