#!/bin/sh
# SPDX-License-Identifier: MIT
# Compile only against supplied guest headers; never load into the host kernel.
set -eu
headers=${1:?Supply matching guest kernel headers}
output=${2:?Supply fresh module output directory}
source=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/guest-kernel
[ -f "$headers/Module.symvers" ] && [ -f "$headers/include/generated/utsrelease.h" ]
[ ! -e "$output" ] || { echo 'Use a fresh render-module directory' >&2; exit 1; }
mkdir -p -- "$output"
output=$(CDPATH= cd -- "$output" && pwd)
cp -- "$source/wddm_render.c" "$source/wddm_render.h" "$source/wddm_native_prime.h" "$output/"
printf '%s\n' 'obj-m += wddm_render.o' 'ccflags-y += -Werror' >"$output/Makefile"
make -C "$headers" M="$output" CC="${CC:-cc}" modules
"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Werror -static -I"$source" \
  "$source/native_prime_contract.c" -o "$output/native-prime-contract"
echo 'Built guest-only render-node module; nothing loaded on host'
