#!/usr/bin/env python3
"""Adds the virtio-dxg device to a WINQ-EMU QEMU source tree.

    python virtio-dxg/qemu/prepare.py <qemu-source>

Copies the device, the dxghost library and the shared protocol headers into
hw/display/, and builds them only for Windows hosts. Composes with
experimental/qemu/prepare_qemu.py and the official runtime patches: it
touches no existing QEMU source except by appending to
hw/display/meson.build.
"""
import pathlib
import shutil
import sys

here = pathlib.Path(__file__).resolve().parent
root = here.parent

MESON = """
# virtio-dxg: Windows host GPU paravirtualization (dxgkrnl over virtio)
if host_os == 'windows'
  virtio_dxg_inc = include_directories('virtio-dxg/protocol',
                                       'virtio-dxg/host/include')
  libdxghost = static_library('dxghost',
    files('virtio-dxg/host/src/dxghost.c', 'virtio-dxg/host/src/objects.c',
          'virtio-dxg/host/src/commands.c', 'virtio-dxg/host/src/kmt.c'),
    include_directories: include_directories('virtio-dxg/host/src/compat'),
    # Built against the guest's Linux headers with its own warning set.
    c_args: ['-Wno-error', '-Wno-missing-prototypes', '-Wno-shadow'],
    build_by_default: false)
  system_ss.add(when: 'CONFIG_VIRTIO_PCI', if_true: [
    files('virtio-dxg.c'),
    declare_dependency(link_with: libdxghost,
                       include_directories: virtio_dxg_inc)])
endif
"""
MARKER = "# virtio-dxg: Windows host GPU paravirtualization"


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    qemu = pathlib.Path(sys.argv[1])
    display = qemu / "hw/display"
    if not (display / "meson.build").exists():
        sys.exit(f"{qemu} is not a QEMU source tree")

    shutil.copyfile(here / "hw/display/virtio-dxg.c", display / "virtio-dxg.c")
    shutil.copyfile(here / "include/hw/display/virtio-dxg.h",
                    qemu / "include/hw/display/virtio-dxg.h")

    tree = display / "virtio-dxg"
    if tree.exists():
        shutil.rmtree(tree)
    shutil.copytree(root / "protocol", tree / "protocol")
    shutil.copytree(root / "host/include", tree / "host/include")
    shutil.copytree(root / "host/src", tree / "host/src")
    # dxghost compiles against the guest's own message definitions.
    (tree / "guest/dxgkrnl").mkdir(parents=True)
    shutil.copyfile(root / "guest/dxgkrnl/dxgvmbus.h",
                    tree / "guest/dxgkrnl/dxgvmbus.h")
    shutil.copytree(root / "guest/include", tree / "guest/include")

    meson = display / "meson.build"
    text = meson.read_text()
    if MARKER not in text:
        meson.write_text(text + MESON, newline="\n")

    # virtio core names devices by ID from a table and asserts the ID is in
    # it; virtio-dxg's ID is past the last standard one.
    virtio = qemu / "hw/virtio/virtio.c"
    text = virtio.read_text()
    entry = '    [62 /* VIRTIO_ID_DXG */] = "virtio-dxg",\n'
    if entry not in text:
        anchor = '    [VIRTIO_ID_SPI] = "virtio-spi"\n};'
        if anchor not in text:
            sys.exit("hw/virtio/virtio.c: device name table not found")
        text = text.replace(anchor, '    [VIRTIO_ID_SPI] = "virtio-spi",\n'
                            + entry + '};')
        virtio.write_text(text, newline="\n")
    print(f"virtio-dxg added to {qemu}")


if __name__ == "__main__":
    main()
