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
from contextlib import contextmanager

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


def boot_config(cpio, module_release):
    """Require the module tree of the initramfs selected for this boot."""
    pos = 0
    config = None
    releases = set()
    while pos + 110 <= len(cpio) and cpio[pos:pos + 6] == b'070701':
        h = cpio[pos:pos + 110]
        size = int(h[54:62], 16)
        namesize = int(h[94:102], 16)
        name = cpio[pos + 110:pos + 110 + namesize - 1].decode()
        data = (pos + 110 + namesize + 3) & ~3
        if data + size > len(cpio):
            raise ValueError('truncated initramfs entry')
        if name == 'TRAILER!!!':
            break
        name = name.removeprefix('./')
        if name == 'config':
            config = cpio[data:data + size]
        tree = re.fullmatch(r'(?:usr/)?lib/modules/([^/]+)/modules\.dep(?:\.bin)?', name)
        if tree:
            releases.add(tree[1])
        pos = (data + size + 3) & ~3
    else:
        raise ValueError('missing main initramfs trailer')
    if releases != {module_release}:
        raise ValueError(f'initramfs kernel releases {sorted(releases)}; DXG requires {module_release}')
    if config is None:
        raise ValueError('no mkinitcpio config in the main archive')
    return config.decode()


def kernel_release(kernel):
    with kernel.open('rb') as f:
        header = f.read(0x210)
        if header[0x202:0x206] != b'HdrS' or len(header) != 0x210:
            raise ValueError(f'{kernel} lacks an x86 Linux boot header')
        f.seek(0x200 + int.from_bytes(header[0x20e:0x210], 'little'))
        version = f.read(256).split(b'\0', 1)[0].split()
        if not version:
            raise ValueError(f'{kernel} lacks a release string')
        return version[0].decode('ascii')


@contextmanager
def staged_image(output):
    target = pathlib.Path(output)
    out = tempfile.NamedTemporaryFile(dir=target.parent, delete=False)
    staging = pathlib.Path(out.name)
    try:
        with out:
            yield out
        staging.replace(target)
    finally:
        staging.unlink(missing_ok=True)


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    original, module, zstd, output = sys.argv[1:]
    module_data = pathlib.Path(module).read_bytes()
    releases = set(re.findall(rb'\x00vermagic=([^\x00 ]+) ', module_data))
    if not module_data.startswith(b'\x7fELF') or len(releases) != 1:
        sys.exit('expected an uncompressed guest module with a single vermagic release')
    release = releases.pop().decode('ascii')
    kernel = pathlib.Path(original).with_name('vmlinuz-linux')
    if kernel.is_file() and kernel_release(kernel) != release:
        sys.exit(f'{kernel} and the DXG module for {release} have different kernel releases')
    if pathlib.Path(output).resolve() in {pathlib.Path(original).resolve(),
                                         pathlib.Path(module).resolve(), kernel.resolve()}:
        sys.exit('choose a development output path distinct from the boot inputs')
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
    config = boot_config(main_cpio, release)
    if 'virtio_dxg' not in config:
        config, n = re.subn(r'^LATEHOOKS="([^"]*)"',
                            lambda m: 'LATEHOOKS="%s virtio_dxg"' %
                            m.group(1).strip(), config, flags=re.M)
        if not n:
            config += 'LATEHOOKS="virtio_dxg"\n'
    hook = pathlib.Path(__file__).resolve().parent / 'initcpio/virtio_dxg'
    with staged_image(output) as out:
        out.write(prefix)
        out.write(main_cpio)
        out.write(b'\0' * ((4 - out.tell() % 4) % 4))
        entry(out, 'config', 0o100644, config.encode())
        entry(out, 'hooks/virtio_dxg', 0o100755, hook.read_bytes())
        entry(out, 'virtio-dxg', 0o040755)
        entry(out, 'virtio-dxg/dxgkrnl_virtio.ko', 0o100644, module_data)
        for name in ('desktop_payload.py', 'chromium-wrapper', 'desktop-priority',
                     'app-prefetch', 'browser-boost'):
            entry(out, 'virtio-dxg/' + name, 0o100644,
                  (hook.parent / name).read_bytes())
        entry(out, 'virtio-dxg/mesa-recipe.json', 0o100644,
              (hook.parents[2] / 'mesa/versions' /
               (hook.parents[2] / 'mesa/CURRENT').read_text().strip() /
               'recipe.json').read_bytes())
        entry(out, 'TRAILER!!!', 0)
        out.write(b'\0' * ((512 - out.tell() % 512) % 512))
    print('%s: LATEHOOKS now %s' % (output, re.search(
        r'^LATEHOOKS=.*$', config, re.M).group(0)))


if __name__ == '__main__':
    main()
