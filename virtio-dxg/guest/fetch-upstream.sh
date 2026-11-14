#!/bin/sh
# Re-fetch Microsoft's dxgkrnl sources at a pinned commit into dxgkrnl/.
set -eu
commit=${1:-a07f9ea8a99139913acbcc1c160b132cb2a49c81}
here=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
git -C "$tmp" init -q
git -C "$tmp" remote add origin https://github.com/microsoft/WSL2-Linux-Kernel.git
git -C "$tmp" fetch -q --depth 1 --filter=blob:none origin "$commit"
for f in $(git -C "$tmp" ls-tree --name-only FETCH_HEAD drivers/hv/dxgkrnl/); do
	git -C "$tmp" show "FETCH_HEAD:$f" > "$here/dxgkrnl/$(basename "$f")"
done
git -C "$tmp" show FETCH_HEAD:include/uapi/misc/d3dkmthk.h > "$here/include/uapi/misc/d3dkmthk.h"
echo "fetched $commit"
