"""Apply Linux import ownership only after the exact pinned Dozen export patch."""
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
    manifest = json.loads((here / 'dozen-memory-fd-import-patch.json').read_bytes())
    patch = here / '0006-linux-dozen-import-memory-fds.patch'
    digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    if not root.is_dir() or (root / 'VERSION').read_text().strip() != manifest['mesaVersion']:
        parser.error('Requires pinned Mesa version')
    if digest(patch) != manifest['patchSha256']:
        parser.error('Patch differs from manifest')
    observed = {}
    for name in manifest['files']:
        path = (root / name).resolve()
        if not path.is_relative_to(root) or (path.exists() and not path.is_file()):
            parser.error('Source path escapes root or is not a file')
        observed[name] = digest(path) if path.exists() else None
    if all(observed[name] == row['patchedSha256'] for name, row in manifest['files'].items()):
        print('Pinned Dozen import FD ownership patch already verified'); return
    if not all(observed[name] == row['originalSha256'] for name, row in manifest['files'].items()):
        parser.error('Requires exact export-patched predecessor; no files changed')
    command = ['patch', '--batch', '--fuzz=0', '-p1', '-i', str(patch)]
    subprocess.run(command + ['--dry-run'], cwd=root, check=True)
    subprocess.run(command, cwd=root, check=True)
    if not all(digest(root / name) == row['patchedSha256'] for name, row in manifest['files'].items()):
        raise SystemExit('Patched source digest differs')
    print('Pinned Dozen import FD ownership patch applied and verified')


if __name__ == '__main__': main()
