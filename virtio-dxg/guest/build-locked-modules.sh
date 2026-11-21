#!/usr/bin/env bash
# Build-only entry point for the guest builder's pinned Arch container.
# /repo and /guest are read-only. Only /out is published.
set -euo pipefail
cd /tmp
# Require a trusted Arch package signature even for a local package file.
pacman-key --init
pacman-key --populate archlinux
sed -E '/^[[:space:]]*LocalFileSigLevel[[:space:]]*=/d; /^\[options\]/a LocalFileSigLevel = Required TrustedOnly' \
    /etc/pacman.conf > /tmp/dxg-pacman.conf
pacman=(pacman --config /tmp/dxg-pacman.conf --disable-sandbox --noconfirm)
"${pacman[@]}" -Syu --needed base-devel python zstd kmod curl libelf bc
version=$(python3 -c 'import json; p=json.load(open("/guest/packages.lock.json"))["packages"]; assert p["linux"] == p["linux-headers"]; print(p["linux"])')
[[ $version =~ ^[0-9][0-9.]+\.arch[0-9]+-[0-9]+$ ]]
release=${version/.arch/-arch}
pkg="linux-headers-$version-x86_64.pkg.tar.zst"
fetch=(curl --fail --location --proto '=https' --proto-redir '=https' --connect-timeout 12)
# The desktop's stable mirror can retain a release after Arch's rolling
# mirror moves on. Every source must supply the same exact name and version.
downloaded=false
for base in https://stable-mirror.omarchy.org/core/os/x86_64 \
            https://geo.mirror.pkgbuild.com/core/os/x86_64 \
            https://archive.archlinux.org/packages/l/linux-headers; do
    if "${fetch[@]}" --max-time 30 "$base/$pkg.sig" -o "$pkg.sig" &&
       "${fetch[@]}" --max-time 180 "$base/$pkg" -o "$pkg"; then
        downloaded=true
        break
    fi
done
if [[ $downloaded != true ]]; then
    echo "No configured mirror provided the locked $pkg and its signature" >&2
    exit 1
fi
pacman-key --verify "$pkg.sig" "$pkg"
test "$(pacman -Qp "./$pkg")" = "linux-headers $version"
"${pacman[@]}" -U "./$pkg"
test "$(pacman -Q linux-headers)" = "linux-headers $version"
headers="/usr/lib/modules/$release/build"
test "$(cat "$headers/include/config/kernel.release")" = "$release"

mkdir dxg-src
cp -r /repo/virtio-dxg/{guest,protocol} dxg-src/
export MAKEFLAGS=-j2
make -C "$headers" M="$PWD/dxg-src/guest" W=1 modules
module="$PWD/dxg-src/guest/dxgkrnl_virtio.ko"
lab=/repo/experimental/windows-native-gpu
sh "$lab/build_guest_aperture.sh" "$headers" "$PWD/aperture"
sh "$lab/build_guest_render.sh" "$headers" "$PWD/render"
# The earlier bridge targets its own devices. Build it against the same ABI
# and ship it separately; the normal DXG session loads dxgkrnl_virtio.
mkdir /out/legacy
cp aperture/wddm_aperture.ko render/wddm_render.ko /out/legacy/
for ko in "$module" /out/legacy/*.ko; do
    magic=$(modinfo -F vermagic "$ko")
    test "${magic%% *}" = "$release"
    printf '%s %s\n' "$(basename "$ko")" "$magic" >> /out/vermagic.txt
done
cp "$module" /out/dxgkrnl_virtio.ko
cd /out
sha256sum dxgkrnl_virtio.ko legacy/*.ko > SHA256SUMS
