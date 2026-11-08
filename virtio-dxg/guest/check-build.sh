#!/bin/sh
# Build dxgkrnl_virtio.ko against the exact Arch kernel a Try Omarchy guest
# boots, without root. Works on any x86_64 Linux (including WSL) with gcc,
# make, curl and python3 >= 3.14 (for zstd).
#
#   virtio-dxg/guest/check-build.sh 7.2.9.arch1-1 [workdir]
#
# The module is left in <workdir>/src/guest/dxgkrnl_virtio.ko.
set -eu
version=${1:?usage: check-build.sh <arch-linux-version, e.g. 7.2.9.arch1-1> [workdir]}
work=${2:-$HOME/.cache/virtio-dxg}
here=$(cd "$(dirname "$0")/.." && pwd)
release=$(echo "$version" | sed 's/\.arch/-arch/')
pkg=linux-headers-$version-x86_64.pkg.tar.zst
hdr=$work/headers-$version
build=$hdr/usr/lib/modules/$release/build

mkdir -p "$work"
if [ ! -d "$build" ]; then
	curl -fsSLo "$work/$pkg" "https://archive.archlinux.org/packages/l/linux-headers/$pkg"
	python3 - "$work/$pkg" "$hdr" <<'EOF'
import compression.zstd as zstd, io, sys, tarfile
data = zstd.decompress(open(sys.argv[1], "rb").read())
tarfile.open(fileobj=io.BytesIO(data)).extractall(sys.argv[2], filter="tar")
EOF
fi

# Arch builds with a newer gcc than most hosts. counted_by on pointers is the
# one feature an older gcc rejects outright; turn it off for this check only.
if ! echo 'struct s { int n; int *p __attribute__((counted_by(n))); };' |
	gcc -x c -c -o /dev/null - 2>/dev/null; then
	sed -i '/#define CONFIG_CC_HAS_COUNTED_BY_PTR 1/d' \
		"$build/include/generated/autoconf.h"
fi

rm -rf "$work/src"
mkdir -p "$work/src"
cp -r "$here/guest" "$here/protocol" "$work/src/"
# BTF needs pahole matching the kernel's; it is not needed to load the module.
btf=
command -v pahole >/dev/null 2>&1 || btf=CONFIG_DEBUG_INFO_BTF_MODULES=
make -C "$work/src/guest" KDIR="$build" EXTRA="$btf W=1"
ls -l "$work/src/guest/dxgkrnl_virtio.ko"
