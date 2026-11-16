# Vendored dxgkrnl

Source: https://github.com/microsoft/WSL2-Linux-Kernel
Branch: `linux-msft-wsl-6.6.y`
Commit: `a07f9ea8a99139913acbcc1c160b132cb2a49c81` (2026-03-05)
Paths: `drivers/hv/dxgkrnl/*`, `include/uapi/misc/d3dkmthk.h`
License: GPL-2.0 (Microsoft Corporation)

The first commit that adds this directory contains the files byte-for-byte as
published. Every later change is ours and is visible with:

```sh
git log -p -- virtio-dxg/guest/dxgkrnl virtio-dxg/guest/include
```

To move to a newer upstream revision, re-run `virtio-dxg/guest/fetch-upstream.sh <commit>`
on a clean tree and re-apply our commits (`git cherry-pick`) on top.
