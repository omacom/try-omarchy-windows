#!/bin/bash
# kbuild.sh VERSION (root, in an Arch guest): the kernel parts of a Try Omarchy
# guest initramfs for Arch's linux VERSION (e.g. 7.3.5.arch1-1), from the
# signed package (verified against the guest's Arch keyring):
#   out/k-REL/vmlinuz-linux          the kernel
#   out/k-REL/kmods.tar              the full module tree with depmod's
#                                    indexes (tryomarchy-kernel-modules.tar)
#   out/k-REL/early/REL/...          the early archive's modules: the same
#                                    paths as early-modules.txt lists
#   out/k-REL/index/                 depmod's indexes for that subset
set -euo pipefail
V=$1
R=${V/.arch/-arch}
W=${WORK:-/var/tmp}/k-$R
O=${OUT:?OUT: output directory}/k-$R
LIST=${EARLY_LIST:?EARLY_LIST: the early archive module list (rekernel.py --list)}
mkdir -p "$W" "$O"
cd "$W"
pkg=linux-$V-x86_64.pkg.tar.zst
for base in https://geo.mirror.pkgbuild.com/core/os/x86_64 https://archive.archlinux.org/packages/l/linux; do
    [ -s "$pkg" ] && [ -s "$pkg.sig" ] && break
    curl -fsSL --proto '=https' -o "$pkg.sig" "$base/$pkg.sig" &&
        curl -fsSL --proto '=https' -o "$pkg" "$base/$pkg" || rm -f "$pkg" "$pkg.sig"
done
pacman-key --verify "$pkg.sig" "$pkg"
rm -rf root early index
mkdir root
bsdtar -xf "$pkg" -C root
ln -s usr/lib root/lib   # depmod -b looks under BASE/lib/modules
M=root/usr/lib/modules/$R
[ -f "$M/vmlinuz" ]
depmod -b root "$R"
cp "$M/vmlinuz" "$O/vmlinuz-linux"
tar -C root/usr/lib/modules --exclude="$R/vmlinuz" --owner=0 --group=0 -cf "$O/kmods.tar" "$R"
# The early subset, and its indexes.
mkdir -p "early/usr/lib/modules/$R"
ln -s usr/lib early/lib
missing=0
while read -r rel; do
    [ -n "$rel" ] || continue
    if [ -f "$M/$rel" ]; then
        install -D -m 0644 "$M/$rel" "early/usr/lib/modules/$R/$rel"
    else
        echo "not in $R: $rel"; missing=$((missing + 1))
    fi
done < "$LIST"
for f in modules.order modules.builtin modules.builtin.modinfo; do
    [ -f "$M/$f" ] && cp "$M/$f" "early/usr/lib/modules/$R/"
done
depmod -b early "$R"
mkdir -p index
for f in modules.alias.bin modules.builtin.alias.bin modules.builtin.bin modules.dep.bin \
         modules.devname modules.softdep modules.symbols.bin; do
    cp "early/usr/lib/modules/$R/$f" index/
done
rm -rf "$O/early" "$O/index"
mkdir -p "$O/early"
cp -r "early/usr/lib/modules/$R" "$O/early/"
find "$O/early" -name 'modules.*' -delete
cp -r index "$O/index"
echo "kbuild $R: $(find "$O/early" -name '*.ko*' | wc -l) early modules, $missing missing, kmods $(du -m "$O/kmods.tar" | cut -f1) MB"
