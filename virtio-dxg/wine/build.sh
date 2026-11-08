#!/bin/sh
# Builds nvvkprobe.exe with MinGW (MSYS2 UCRT64): build.sh <output-dir>
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-$here/build}
mkdir -p "$out"
gcc -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
	-I"$here/../host/src/compat" \
	"$here/nvvkprobe.c" "$here/kmt_bridge.c" "$here/kmt_display.c" \
	-lsetupapi -static -o "$out/nvvkprobe.exe"
echo "$out/nvvkprobe.exe"
