# virtio-dxg

NVIDIA GPU virtualization for the Omarchy guest on a Windows host, at the
driver level: the guest runs NVIDIA's own Linux user-mode driver (the one
WSL2 uses) and Microsoft's `dxgkrnl`, carried over virtio instead of VMBus,
and the host executes its requests with the Windows NVIDIA driver.

Design, feature matrix, status and roadmap: [docs/VIRTIO-DXG.md](../docs/VIRTIO-DXG.md).

| Directory | Contents | License |
| --- | --- | --- |
| `protocol/` | Wire contract shared by both sides | GPL-2.0 OR MIT |
| `guest/` | `dxgkrnl` with the virtio transport, out-of-tree module | GPL-2.0 |
| `host/` | `dxghost`: executes guest messages with D3DKMT | GPL-2.0 |
| `qemu/` | `virtio-dxg-pci` device for the WINQ-EMU QEMU | GPL-2.0-or-later |

```sh
virtio-dxg/guest/check-build.sh 7.2.9.arch1-1     # guest module, no root
```

```powershell
virtio-dxg/host/build.ps1                          # host library + tests
```

```sh
python virtio-dxg/qemu/prepare.py <qemu-source>    # add the device to QEMU
```
