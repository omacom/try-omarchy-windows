#!/bin/sh
# Optional local WSL reference. Dependencies are supplied by the caller.
set -eu
if [ "$#" -ne 3 ]; then
    echo 'Usage: build_umd_context.sh DirectX-Headers-directory libdxg-directory output-directory' >&2
    exit 2
fi
base=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
headers=$(CDPATH= cd -- "$1" && pwd)
dxg=$(CDPATH= cd -- "$2" && pwd)
mkdir -p "$3"
output=$(CDPATH= cd -- "$3" && pwd)
for captured in "$output"/wsl-*.bin "$output"/driver-*.bin; do
    if [ -e "$captured" ]; then
        echo 'Use a fresh output directory so stale driver captures cannot enter the fixture.' >&2
        exit 2
    fi
done
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror \
    -isystem "$headers/include" -isystem "$headers/include/wsl/stubs" \
    "$base/umd_context_probe.cpp" "$headers/src/dxguids.cpp" \
    -L/usr/lib/wsl/lib -Wl,-rpath,/usr/lib/wsl/lib -ld3d12 -ldxcore -o "$output/umd-context-probe"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror \
    -isystem "$headers/include" -isystem "$headers/include/wsl/stubs" -isystem "$dxg/include" \
    -shared -fPIC "$base/umd_context_trace.cpp" -ldl -pthread -o "$output/umd-context-trace.so"
cd "$output"
LD_PRELOAD=./umd-context-trace.so ./umd-context-probe > umd-context-probe.log 2> umd-context-profile.log
python3 "$base/make_context_fixture.py" --input . --output driver-contexts.bin
python3 "$base/make_query_fixture.py" --input . --output driver-queries.bin
