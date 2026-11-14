#!/bin/sh
# SPDX-License-Identifier: MIT
# Compile against caller-supplied, matching guest headers. Never load on host.
set -eu
headers=${1:?Supply the matching guest kernel build directory}
output=${2:?Supply a fresh output directory}
source=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/guest-kernel
[ -f "$headers/Module.symvers" ] && [ -f "$headers/include/generated/utsrelease.h" ] || {
  echo 'Matching prepared kernel headers and symbols are required' >&2; exit 1;
}
[ ! -e "$output" ] || { echo 'Use a fresh module output directory' >&2; exit 1; }
mkdir -p -- "$output"
output=$(CDPATH= cd -- "$output" && pwd)
cp -- "$source/wddm_aperture.c" "$source/wddm_aperture.h" "$source/wddm_object_fd.h" "$source/wddm_sync_file.h" "$source/wddm_fence_eventfd.h" "$source/wddm_native_buffer.h" "$source/wddm_native_prime.h" "$source/Makefile" "$output/"
make -C "$headers" M="$output" CC="${CC:-cc}" modules
"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Werror -static -I"$source" \
  "$source/aperture_contract.c" -o "$output/aperture-contract"
echo 'Built guest-only aperture module and contract probe; nothing loaded on host'
