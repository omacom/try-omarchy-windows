#!/bin/sh
# Builds the Windows half with MinGW (MSYS2 UCRT64): build.sh <output-dir>
#   nvvkprobe.exe  driver test (no window)
#   dxgvk.dll      the Vulkan driver for Wine/Proton, with dxgvk.json
#   vkpresent.exe  presentation test
#   vulkan-1.dll   the Khronos loader, which applications load natively
# dxgkmt.so is built in the guest: see dxgkmt_unix.c.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-$here/build}
mkdir -p "$out"
cflags="-O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -I$here/../host/src/compat"
common="$here/nvload.c $here/kmt_bridge.c $here/kmt_display.c"
gcc $cflags "$here/nvvkprobe.c" $common -lsetupapi -static -o "$out/nvvkprobe.exe"
gcc $cflags -shared "$here/dxgvk.c" $common -lsetupapi -static \
	-Wl,--exclude-all-symbols -o "$out/dxgvk.dll"
gcc $cflags "$here/vkpresent.c" -lvulkan-1 -lm -o "$out/vkpresent.exe"
cp "$(dirname "$(command -v gcc)")/vulkan-1.dll" "$out/"
cat > "$out/dxgvk.json" <<'EOF'
{
    "file_format_version": "1.0.1",
    "ICD": {
        "library_path": ".\\dxgvk.dll",
        "api_version": "1.4.357"
    }
}
EOF
ls "$out"
