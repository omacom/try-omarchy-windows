#!/usr/bin/env python3
"""Makes a development copy of the launcher's guest initramfs that loads
virtio-dxg as root before the real root starts.

    python mkdevinitramfs.py <initramfs-linux.img> <dxgkrnl_virtio.ko> \
        <zstd-executable> <output>

The original is only read. The output is the original's leading
uncompressed archives, its compressed main archive decompressed, and one
more archive with the module, the virtio_dxg late hook and a config that
adds that hook. Every part is a plain cpio, which the kernel unpacks in
order; later files replace earlier ones.
"""
import pathlib
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from mkinitramfs import entry  # noqa: E402

ZSTD_MAGIC = b'\x28\xb5\x2f\xfd'


def split(image):
    """Returns (uncompressed prefix, compressed remainder)."""
    pos = 0
    while pos + 110 <= len(image) and image[pos:pos + 6] == b'070701':
        while True:  # walk one archive to its trailer
            h = image[pos:pos + 110]
            size = int(h[54:62], 16)
            namesize = int(h[94:102], 16)
            name = image[pos + 110:pos + 110 + namesize - 1]
            data = (pos + 110 + namesize + 3) & ~3
            pos = (data + size + 3) & ~3
            if name == b'TRAILER!!!':
                break
        while pos < len(image) and image[pos] == 0:
            pos += 1
    return image[:pos], image[pos:]


def read_member(cpio, wanted):
    pos = 0
    while pos + 110 <= len(cpio) and cpio[pos:pos + 6] == b'070701':
        h = cpio[pos:pos + 110]
        size = int(h[54:62], 16)
        namesize = int(h[94:102], 16)
        name = cpio[pos + 110:pos + 110 + namesize - 1].decode()
        data = (pos + 110 + namesize + 3) & ~3
        if name == wanted:
            return cpio[data:data + size]
        pos = (data + size + 3) & ~3
    return None


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    original, module, zstd, output = sys.argv[1:]
    image = pathlib.Path(original).read_bytes()
    prefix, rest = split(image)
    if not rest.startswith(ZSTD_MAGIC):
        sys.exit('expected a zstd-compressed main archive after the '
                 'uncompressed ones')
    with tempfile.TemporaryDirectory() as tmp:
        packed = pathlib.Path(tmp, 'main.cpio.zst')
        plain = pathlib.Path(tmp, 'main.cpio')
        packed.write_bytes(rest)
        subprocess.run([zstd, '-dqf', str(packed), '-o', str(plain)],
                       check=True)
        main_cpio = plain.read_bytes()
    config = read_member(main_cpio, 'config')
    if config is None:
        sys.exit('no mkinitcpio config in the main archive')
    config = config.decode()
    if 'virtio_dxg' not in config:
        config, n = re.subn(r'^LATEHOOKS="([^"]*)"',
                            lambda m: 'LATEHOOKS="%s virtio_dxg"' %
                            m.group(1).strip(), config, flags=re.M)
        if not n:
            config += 'LATEHOOKS="virtio_dxg"\n'
    hook = pathlib.Path(__file__).resolve().parent / 'initcpio/virtio_dxg'
    with open(output, 'wb') as out:
        out.write(prefix)
        out.write(main_cpio)
        out.write(b'\0' * ((4 - out.tell() % 4) % 4))
        entry(out, 'config', 0o100644, config.encode())
        entry(out, 'hooks/virtio_dxg', 0o100755, hook.read_bytes())
        entry(out, 'virtio-dxg', 0o040755)
        entry(out, 'virtio-dxg/dxgkrnl_virtio.ko', 0o100644,
              pathlib.Path(module).read_bytes())
        entry(out, 'TRAILER!!!', 0)
        out.write(b'\0' * ((512 - out.tell() % 512) % 512))
    print('%s: LATEHOOKS now %s' % (output, re.search(
        r'^LATEHOOKS=.*$', config, re.M).group(0)))


if __name__ == '__main__':
    main()
