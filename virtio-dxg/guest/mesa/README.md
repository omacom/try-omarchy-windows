# virtio-dxg Mesa

The Omarchy desktop renders on the host GPU through Mesa's d3d12 Gallium
driver: Hyprland, its shell, Xwayland and GL apps all run on NVIDIA's own
D3D12 driver (`libnvwgf2umx.so`) over `/dev/dxg`. VA-API (d3d12) and Vulkan
(Dozen) come from the same build.

Stock Mesa cannot drive a compositor that way on a VM display. The KMS
device (virtio-gpu in 2D mode) only understands dumb buffers, d3d12 cannot
export or import them, and GBM falls back to allocations Hyprland cannot
use. `0001-d3d12-kms-mirror.patch` closes that gap:

- On the `kms_swrast` winsys, d3d12 reports dma-buf import and export.
- Scanout, cursor and shared images become **mirrors**. A normal GPU
  texture does all the rendering, and a dumb buffer (or the imported
  dma-buf) is what handles, dma-bufs and KMS see.
- Imported dma-bufs are mapped through their own descriptor, because
  compositors render on the render node, where dumb buffers cannot be
  mapped.
- Every context flush that wrote a mirror copies it into its buffer before
  returning. `glFlush` therefore publishes the frame, and the compositor's
  page flip shows it.

`gbmtest` (in `../test`) checks the whole path. It allocates a GBM scanout
buffer, imports it into EGL as a dma-buf, renders with the GPU and reads the
dumb buffer back. On an RTX 5090 Laptop GPU, a 1280×800 frame reaches the
scanout buffer about 3 ms after `glFlush`.

## Building

`build-in-guest.sh` builds inside the guest it is for. Run it as an ordinary
user; meson, ninja and mako go into a private venv, and nothing is
installed. It uses the system Mesa's version string, so the result is a
self-consistent replacement set: libgallium, EGL, GBM, GLX, the DRI loader,
`d3d12_drv_video.so` and `libvulkan_dzn.so`.

```sh
bash build-in-guest.sh mesa-26.2.4.tar.xz mesa-virtio-dxg.tar \
    0001-d3d12-kms-mirror.patch
```

The development boot hook (`../test/initcpio/virtio_dxg`, kernel argument
`virtio_dxg.desktop=1`) unpacks the tarball to `/opt/virtio-dxg-mesa`. It
then points the session's `LD_LIBRARY_PATH`, `LIBGL_DRIVERS_PATH`,
`GBM_BACKENDS_PATH`, `LIBVA_DRIVERS_PATH` and `VK_DRIVER_FILES` at it.

## Next

The mirror copy is the remaining cost of presenting a frame (GPU → guest
memory, then virtio-gpu copies it again to the host). The next step links a
mirror's virtio-gpu resource to its D3D12 resource on the host. QEMU then
copies, or later shows, the GPU texture directly, and the guest copy goes
away.
