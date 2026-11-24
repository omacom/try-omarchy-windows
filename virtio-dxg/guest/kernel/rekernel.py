"""rekernel.py ORIG_IMAGE ZSTD KDIR OLD_REL NEW_REL OUT_IMAGE: a Try Omarchy
guest initramfs moved to another kernel release, everything else as it was.
rekernel.py --list ORIG_IMAGE: the early archive's modules (kbuild.sh's
EARLY_LIST), relative to their release directory. KDIR is kbuild.sh's output for NEW_REL:
  early/NEW_REL/...   the early (uncompressed) archive's modules
  index/              depmod's indexes for them (in the main archive)
  kmods.tar           tryomarchy-kernel-modules.tar: the full module tree
The early archive's other entries and the main archive's other members
(udev, systemd, hooks, config, the compat overlay) are kept."""
import os
import pathlib
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'test'))
from mkdevinitramfs import split  # noqa: E402
from mkinitramfs import entry  # noqa: E402


def members(cpio):
    p = 0
    while p + 110 <= len(cpio) and cpio[p:p + 6] == b'070701':
        h = cpio[p:p + 110]
        f = [int(h[6 + 8 * i:14 + 8 * i], 16) for i in range(13)]
        mode, size, rmaj, rmin, ns = f[1], f[6], f[9], f[10], f[11]
        name = cpio[p + 110:p + 110 + ns - 1].decode()
        d = (p + 110 + ns + 3) & ~3
        p = (d + size + 3) & ~3
        if name == 'TRAILER!!!':
            while p < len(cpio) and cpio[p] == 0:
                p += 1
            continue
        yield name, mode, cpio[d:d + size], (rmaj, rmin)


if sys.argv[1] == '--list':
    early, _ = split(pathlib.Path(sys.argv[2]).read_bytes())
    for name, mode, data, rdev in members(early):
        if '.ko' in name and name.startswith('usr/lib/modules/'):
            print(name.split('/', 4)[4])
    sys.exit(0)
orig, zstd, kdir, old, new, out_path = sys.argv[1:7]
kdir = pathlib.Path(kdir)
image = pathlib.Path(orig).read_bytes()
prefix, rest = split(image)
old_dir, new_dir = f'usr/lib/modules/{old}', f'usr/lib/modules/{new}'
staging = pathlib.Path(out_path + '.partial')
with open(staging, 'wb') as out:
    # Early archive: its own entries, the module tree replaced.
    dir_mode = 0o040700
    for name, mode, data, rdev in members(prefix):
        if name.rstrip('/') == old_dir or name.startswith(old_dir + '/'):
            if (mode & 0o170000) == 0o040000:
                dir_mode = mode
            continue
        entry(out, name, mode, data, rdev)
    early_root = kdir / 'early' / new
    dirs, files = set(), []
    for root, _, names in os.walk(early_root):
        rel = pathlib.Path(root).relative_to(early_root).as_posix()
        dirs.add(new_dir + ('' if rel == '.' else '/' + rel))
        for n in names:
            files.append(((new_dir + '/' + ('' if rel == '.' else rel + '/') + n),
                          pathlib.Path(root, n)))
    for d in sorted(dirs):
        entry(out, d + '/', dir_mode)
    for name, src in sorted(files):
        entry(out, name, 0o100644, src.read_bytes())
    entry(out, 'TRAILER!!!', 0)
    out.write(b'\0' * ((512 - out.tell() % 512) % 512))
    early_len = out.tell()

    # Main archive: the module tar and the early modules' indexes replaced.
    with tempfile.TemporaryDirectory() as t:
        packed = pathlib.Path(t, 'main.cpio.zst')
        plain = pathlib.Path(t, 'main.cpio')
        newplain = pathlib.Path(t, 'new.cpio')
        packed.write_bytes(rest)
        subprocess.run([zstd, '-dqf', str(packed), '-o', str(plain)], check=True)
        with open(newplain, 'wb') as m:
            index_done = False
            for name, mode, data, rdev in members(plain.read_bytes()):
                if name == 'tryomarchy-kernel-modules.tar':
                    data = (kdir / 'kmods.tar').read_bytes()
                elif name.rstrip('/') == old_dir:
                    entry(m, new_dir + '/', mode)
                    for f in sorted((kdir / 'index').iterdir()):
                        entry(m, new_dir + '/' + f.name, 0o100644, f.read_bytes())
                    index_done = True
                    continue
                elif name.startswith(old_dir + '/'):
                    continue
                elif old in name:
                    sys.exit(f'unexpected member naming the old release: {name}')
                entry(m, name, mode, data, rdev)
            assert index_done, 'no module index directory in the main archive'
            entry(m, 'TRAILER!!!', 0)
            m.write(b'\0' * ((512 - m.tell() % 512) % 512))
        subprocess.run([zstd, '-qf', '-T0', '-12', str(newplain), '-o', str(packed)], check=True)
        out.write(packed.read_bytes())
os.replace(staging, out_path)
print(f'{out_path}: early {early_len // 1048576} MB, {len(files)} early files, '
      f'total {os.path.getsize(out_path) // 1048576} MB')
