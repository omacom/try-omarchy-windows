#!/usr/bin/env python3
"""Packs the virtio-dxg bring-up initramfs (newc cpio, uncompressed).

    python mkinitramfs.py <init> <dxgtest> <dxgkrnl_virtio.ko> <output>

Needs no root: device nodes are written straight into the archive.
"""
import sys
import time

INO = [1]


def entry(out, name, mode, data=b'', rdev=(0, 0)):
    name_bytes = name.encode() + b'\0'
    header = '070701' + ''.join('%08X' % v for v in (
        INO[0], mode, 0, 0, 1, int(time.time()), len(data), 0, 0,
        rdev[0], rdev[1], len(name_bytes), 0))
    INO[0] += 1
    out.write(header.encode() + name_bytes)
    out.write(b'\0' * ((4 - (110 + len(name_bytes)) % 4) % 4))
    out.write(data)
    out.write(b'\0' * ((4 - len(data) % 4) % 4))


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    init, test, module, output = sys.argv[1:]
    with open(output, 'wb') as out:
        for d in ('dev', 'proc', 'sys'):
            entry(out, d, 0o040755)
        entry(out, 'dev/console', 0o020600, rdev=(5, 1))
        entry(out, 'init', 0o100755, open(init, 'rb').read())
        entry(out, 'dxgtest', 0o100755, open(test, 'rb').read())
        entry(out, 'dxgkrnl_virtio.ko', 0o100644, open(module, 'rb').read())
        entry(out, 'TRAILER!!!', 0)
        out.write(b'\0' * ((512 - out.tell() % 512) % 512))


if __name__ == '__main__':
    main()
