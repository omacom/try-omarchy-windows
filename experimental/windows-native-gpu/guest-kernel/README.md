# Private QEMU aperture driver

This guest-only PCI module owns restricted CPU views of native Windows memory
exposed through the private QEMU fence/allocation hubs. It is a memory-lifetime
component, not a DRM device, dma-buf exporter or `/dev/dxg` graphics driver.

Only a trusted `CAP_SYS_RAWIO` broker can create leases. The broker must use
validated native host mapping responses; arbitrary client offsets must not be
authorized. Each returned anonymous FD exposes one bounded slice. Fence FDs
are read-only. Writable allocation slices cannot overlap and are limited to
96 MiB total. The controller can revoke a cookie it owns; close revokes all its
leases. Recipients can transfer an authorized FD with SCM_RIGHTS. Revocation
zaps every mapping before range reuse and leaves subsequent accesses faulting.

`wddm_aperture.h` specifies the fixed versioned ABI. `aperture_contract.c` runs
only in an owned test guest, including a UID/GID 65534 recipient. It does not
read unbacked BAR memory. The initramfs runs real OpenGL afterwards through the
new kernel path and verifies zero leases before unloading the module.

Build with `../build_guest_aperture.sh <matching-kernel-build> <fresh-output>`.
The module needs prepared x64 Linux headers and symbols matching the guest.
The build never loads anything on the host. The current tested guest kernel
is 7.2.8-arch1-2; CI also compiles against its prepared generic headers.

The kernel implementation and Kbuild file are GPL-2.0-only; see [COPYING](COPYING).
The independently authored userspace ABI header and contract probe are MIT.
See [hardware evidence and limits](../../../docs/LIVE-GPU-KERNEL-APERTURE.md).
