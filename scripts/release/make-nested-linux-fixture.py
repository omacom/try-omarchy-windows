#!/usr/bin/env python3
"""Build the diskless nested-Linux test initramfs with a static C compiler.

The tiny PID 1 prints kernel/readiness markers and powers off or reboots according
 to tryomarchy.test=poweroff|reboot. It contains no shell, disks or network tools.
"""
import argparse
import gzip
import os
from pathlib import Path
import shlex
import stat
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    source = Path(__file__).with_name('fixtures') / 'nested-linux-init.c'
    with tempfile.TemporaryDirectory() as directory:
        binary = Path(directory) / 'init'
        subprocess.run(shlex.split(os.environ.get('CC', 'cc')) +
                       ['-static', '-Os', '-Wall', '-Wextra', '-Werror',
                        str(source), '-o', str(binary)], check=True)
        archive = bytearray()

        def entry(name, mode, data=b'', major=0, minor=0):
            encoded = name.encode() + b'\0'
            fields = [1, mode, 0, 0, 1, 0, len(data), 0, 0, major, minor, len(encoded), 0]
            archive.extend(b'070701' + b''.join(f'{x:08x}'.encode() for x in fields) + encoded)
            archive.extend(bytes(-len(archive) % 4))
            archive.extend(data)
            archive.extend(bytes(-len(archive) % 4))

        entry('dev', stat.S_IFDIR | 0o755)
        entry('dev/console', stat.S_IFCHR | 0o600, major=5, minor=1)
        entry('init', stat.S_IFREG | 0o755, binary.read_bytes())
        entry('TRAILER!!!', 0)
        args.output.write_bytes(gzip.compress(archive, mtime=0))
    print(args.output)


if __name__ == '__main__':
    main()
