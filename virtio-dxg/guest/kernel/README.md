# Moving a guest to another kernel release

Try Omarchy's guest boots the kernel and initramfs of its release assets
(`vmlinuz-linux`, `initramfs-linux.img`). The initramfs holds the kernel's
release in three places: the early archive's modules (uncompressed archive,
compressed `.ko.zst` files), their `depmod` indexes in the main archive, and
`tryomarchy-kernel-modules.tar`, the full module tree that the
`tryomarchy_compat` hook installs into the disk's `/usr/lib/modules` on the
first boot of a release. Everything else - udev, systemd, the hooks, the
compat overlay - does not depend on the kernel.

These tools move a released initramfs to another Arch `linux` package, for
instance 7.2.15 (v0.15.0's) to 7.3.5:

```sh
# On the host: the early archive's module list.
python3 rekernel.py --list guest-v0.15.0/initramfs-linux.img > early-modules.txt
# As root in an Arch guest: the signed package (checked against the guest's
# keyring), its modules with depmod's indexes, and the early subset.
EARLY_LIST=early-modules.txt OUT=/srv/out bash kbuild.sh 7.3.5.arch1-1
# On the host: the initramfs for the new release, next to its kernel.
python3 rekernel.py guest-v0.15.0/initramfs-linux.img zstd /srv/out/k-7.3.5-arch1-1 \
    7.2.15-arch1-1 7.3.5-arch1-1 guest-7.3.5/initramfs-linux.img
cp /srv/out/k-7.3.5-arch1-1/vmlinuz-linux guest-7.3.5/
```

Then build `dxgkrnl_virtio.ko` for the release
(`../check-build.sh 7.3.5.arch1-1`) and the development initramfs on top
(`../test/mkdevinitramfs.py`, which checks that kernel, modules and DXG
module are the same release). Modules the new release no longer has (7.3.5:
`aead`, now built in, and `sata_inic162x`, removed) are reported and left
out.
