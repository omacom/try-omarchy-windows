#!/bin/sh
set -eu
base=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mkdir -p "$base/build"
"${CXX:-c++}" -static -std=c++17 -Wall -Wextra -Werror -O2 "$base/guest_probe.cpp" -o "$base/build/guest-probe"
"${CC:-cc}" -static -std=c11 -Wall -Wextra -Werror -O2 "$base/guest_init.c" -o "$base/build/guest-init"
"${CC:-cc}" -static -std=c11 -Wall -Wextra -Werror -O2 "$base/fence_guest.c" -o "$base/build/fence-init"
python3 "$base/make_test_initramfs.py" --init "$base/build/guest-init" --probe "$base/build/guest-probe" --output "$base/build/bridge-test.cpio"
python3 "$base/make_test_initramfs.py" --init "$base/build/fence-init" --probe "$base/build/fence-init" --output "$base/build/fence-test.cpio"
