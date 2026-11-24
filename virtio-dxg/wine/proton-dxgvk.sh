#!/bin/bash
# proton-dxgvk.sh PREFIX PROTON GAME_DIR BUILD_DIR [undo]
#
# Runs a Proton game's Direct3D (vkd3d-proton, DXVK) on NVIDIA's own
# Windows Vulkan driver through dxgvk, in the virtio-dxg guest.
#
#   PREFIX     the Wine prefix (Proton's compatdata/pfx)
#   PROTON     the Proton directory (files/bin/wine, toolmanifest.vdf)
#   GAME_DIR   the directory of the game's executable
#   BUILD_DIR  build.sh's output: dxgvk.dll, dxgvk.json, vulkan-1.dll, dxgs.dll
#   DRS_DIR    (environment, optional) NVIDIA's profile database files
#
# Minecraft Bedrock with BedrockOnLinux (GDK-Proton):
#   B=~/.local/share/bedrock-on-linux
#   proton-dxgvk.sh $B/compatdata/pfx $B/proton/GDK-Proton-xuser \
#       "$(readlink -f $B/content)" <build-dir>
#
# What it changes, all undone by "undo":
# - The Khronos loader as vulkan-1.dll and winevulkan.dll in the game's
#   directory (vkd3d-proton and DXVK load winevulkan.dll). Proton copies its
#   own DLLs back into system32 at every launch; a native-preferred DLL in
#   the application's directory is found first.
# - dxgs.dll in system32: the loader's DXGI (patch_loader.py). DXVK's
#   dxgi.dll would re-enter the loader and deadlock.
# - dxgvk.dll and its Linux half in C:\dxgvk, registered as the loader's only
#   driver; Proton 10's Wine gives Unix libraries only to builtin modules, so
#   dxgvk.dll (marked builtin) and dxgkmt.so (as dxgvk.so) also go into
#   Proton's own library directories.
# - DLL overrides for vulkan-1/winevulkan (native first), wineopenxr off.
# - Proton runs on the host, not in Steam's runtime container, which hides
#   /usr/lib/wsl and the session's Mesa: toolmanifest.vdf without
#   require_tool_appid (the original is kept as toolmanifest.vdf.vdxg-orig).
# - NVIDIA's driver folders linked where the driver's registry paths point.
#
# The session must not export VK_DRIVER_FILES (Wine passes it to Windows
# programs, whose loader then ignores its registry); the virtio-dxg boot
# hook adds Dozen with VK_ADD_DRIVER_FILES. Re-run after a game or Proton
# update. DXGVK_HUD=1 in the game's environment shows its frame rate.
set -eu
[ $# -ge 4 ] || { sed -n '2,13p' "$0"; exit 2; }
PFX=$1 PROTON=$2 GAME=$3 BUILD=$4
here=$(cd "$(dirname "$0")" && pwd)
WINE=$PROTON/files/bin/wine
SYS=$PFX/drive_c/windows/system32
LIB=$PROTON/files/lib/wine
K='HKLM\SOFTWARE\Khronos\Vulkan\Drivers'
O='HKCU\Software\Wine\DllOverrides'
export WINEPREFIX=$PFX WINEDEBUG=-all

if [ "${5:-}" = undo ]; then
    rm -f "$LIB/x86_64-windows/dxgvk.dll" "$LIB/x86_64-unix/dxgvk.so" "$SYS/dxgs.dll"
    rm -f "$GAME/vulkan-1.dll" "$GAME/winevulkan.dll"
    rm -rf "$PFX/drive_c/dxgvk"
    "$WINE" reg delete "$K" /v 'C:\dxgvk\dxgvk.json' /f >/dev/null 2>&1 || true
    "$WINE" reg add "$K" /v 'C:\windows\system32\winevulkan.json' /t REG_DWORD /d 0 /f >/dev/null
    for v in vulkan-1 winevulkan wineopenxr; do
        "$WINE" reg delete "$O" /v $v /f >/dev/null 2>&1 || true
    done
    if [ -f "$PROTON/toolmanifest.vdf.vdxg-orig" ]; then
        cp "$PROTON/toolmanifest.vdf.vdxg-orig" "$PROTON/toolmanifest.vdf"
    fi
    "$PROTON/files/bin/wineserver" -w
    echo "dxgvk removed"
    exit 0
fi

[ -f "$PROTON/toolmanifest.vdf.vdxg-orig" ] || cp "$PROTON/toolmanifest.vdf" "$PROTON/toolmanifest.vdf.vdxg-orig"
sed -i '/require_tool_appid/d' "$PROTON/toolmanifest.vdf"

D=$PFX/drive_c/dxgvk
mkdir -p "$D"
cp -f "$BUILD/dxgvk.dll" "$BUILD/dxgvk.json" "$D/"
gcc -shared -fPIC -O2 -o "$D/dxgkmt.so" "$here/dxgkmt_unix.c" \
    -lgbm -lxcb -lxcb-dri3 -lxcb-present -lpthread
# Replaced by rename: a running game keeps the old file mapped.
cp -f "$D/dxgvk.dll" "$LIB/x86_64-windows/.dxgvk.dll.new"
mv -f "$LIB/x86_64-windows/.dxgvk.dll.new" "$LIB/x86_64-windows/dxgvk.dll"
cp -f "$D/dxgkmt.so" "$LIB/x86_64-unix/.dxgvk.so.new"
mv -f "$LIB/x86_64-unix/.dxgvk.so.new" "$LIB/x86_64-unix/dxgvk.so"

for f in vulkan-1.dll winevulkan.dll; do cp -f "$BUILD/vulkan-1.dll" "$GAME/$f"; done
cp -f "$BUILD/dxgs.dll" "$SYS/dxgs.dll"

FR=$SYS/DriverStore/FileRepository
mkdir -p "$FR"
for d in /usr/lib/wsl/drivers/nv*.inf_amd64_*; do
    [ -d "$d" ] && ln -sfn "$d" "$FR/$(basename "$d")"
done
if [ -n "${DRS_DIR:-}" ]; then
    DRS="$SYS/drivers/NVIDIA Corporation/Drs"
    PD="$PFX/drive_c/ProgramData/NVIDIA Corporation/Drs"
    mkdir -p "$DRS" "$PD"
    cp -f "$DRS_DIR/sys-nvdrsdb.bin" "$DRS/nvdrsdb.bin"
    for f in nvAppTimestamps nvdrsdb0.bin nvdrsdb1.bin nvdrssel.bin update.bin; do
        cp -f "$DRS_DIR/$f" "$PD/"
    done
    chmod -R u+w "$DRS" "$PD"
fi

"$WINE" reg delete "$K" /f >/dev/null 2>&1 || true
"$WINE" reg add "$K" /v 'C:\dxgvk\dxgvk.json' /t REG_DWORD /d 0 /f >/dev/null
"$WINE" reg add "$O" /v vulkan-1 /d native,builtin /f >/dev/null
"$WINE" reg add "$O" /v winevulkan /d native,builtin /f >/dev/null
"$WINE" reg add "$O" /v wineopenxr /d "" /f >/dev/null
"$PROTON/files/bin/wineserver" -w
echo "dxgvk set up for $GAME"
