#!/usr/bin/env python3
"""Apply the experimental patch only to its exact, unmodified Mesa sources."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    args = parser.parse_args()
    root = args.source.resolve(strict=True)
    here = Path(__file__).resolve().parent
    manifest = json.loads((here / 'dozen-memory-fd-patch.json').read_text())
    patch = here / '0005-linux-dozen-memory-fds.patch'
    digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    if not root.is_dir() or (root / 'VERSION').read_text().strip() != manifest['mesaVersion']:
        parser.error('Requires the pinned Mesa version')
    if digest(patch) != manifest['patchSha256']:
        parser.error('Patch digest does not match its manifest')
    observed = {}
    for name, hashes in manifest['files'].items():
        path = (root / name).resolve(strict=True)
        if not path.is_relative_to(root) or not path.is_file():
            parser.error('Source file escapes the supplied Mesa directory')
        observed[name] = digest(path)
    if all(observed[name] == hashes['patchedSha256'] for name, hashes in manifest['files'].items()):
        print('Pinned Mesa Dozen memory-FD patch already verified')
        return
    if not all(observed[name] == hashes['originalSha256'] for name, hashes in manifest['files'].items()):
        parser.error('Source differs from the pinned originals; no files changed')
    command = ['patch', '--batch', '--fuzz=0', '-p1', '-i', str(patch)]
    subprocess.run(command + ['--dry-run'], cwd=root, check=True)
    subprocess.run(command, cwd=root, check=True)
    if not all(digest(root / name) == hashes['patchedSha256'] for name, hashes in manifest['files'].items()):
        raise SystemExit('Patched-source digest verification failed')
    print('Pinned Mesa Dozen memory-FD patch applied and verified')


if __name__ == '__main__':
    main()
