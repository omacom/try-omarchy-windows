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
archive="https://archive.archlinux.org/packages/l/linux-headers/$pkg"
curl --fail --location --proto '=https' --proto-redir '=https' --retry 2 --max-time 180 "$archive" -o "$pkg"
curl --fail --location --proto '=https' --proto-redir '=https' --retry 2 --max-time 60 "$archive.sig" -o "$pkg.sig"
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
