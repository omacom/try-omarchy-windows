#!/usr/bin/env python3
"""Apply the pinned opt-in EGL native-image patch after both timeline patches."""
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
    image = json.loads((here/'native-image-patch.json').read_text())
    sync = json.loads((here/'native-sync-patch.json').read_text())
    base = json.loads((here/'fence-fd-patch.json').read_text())
    digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    patch = here/'0003-linux-d3d12-native-images.patch'
    if not root.is_dir() or (root/'VERSION').read_text().strip() != image['mesaVersion']:
        parser.error('Requires the pinned Mesa version')
    if (digest(patch) != image['patchSha256'] or
        image['requiresNativeSyncPatchSha256'] != sync['patchSha256'] or
        digest(here/'0002-linux-d3d12-native-sync-files.patch') != sync['patchSha256'] or
        sync['requiresBasePatchSha256'] != base['patchSha256'] or
        digest(here/'0001-linux-d3d12-timeline-fence-fds.patch') != base['patchSha256']):
        parser.error('Image/timeline patch digests do not match')
    predecessors = {**base['files'], **sync['files']}
    observed = {}
    for name in set(predecessors) | set(image['files']):
        path = (root/name).resolve()
        if not path.is_relative_to(root) or (path.exists() and not path.is_file()):
            parser.error('Source file escapes supplied Mesa directory or is not a file')
        observed[name] = digest(path) if path.is_file() else None
    if any(observed[n] != v['patchedSha256'] for n, v in predecessors.items() if n not in image['files']):
        parser.error('Apply both pinned timeline patches first; unrelated source changes are rejected')
    if all(observed[n] == v['patchedSha256'] for n, v in image['files'].items()):
        print('Pinned Mesa native-image patch already verified')
        return
    if not all(observed[n] == v['originalSha256'] for n, v in image['files'].items()):
        parser.error('Source differs from verified native-sync base; no files changed')
    command = ['patch', '--batch', '--fuzz=0', '-p1', '-i', str(patch)]
    subprocess.run(command+['--dry-run'], cwd=root, check=True)
    subprocess.run(command, cwd=root, check=True)
    if not all(digest(root/n) == v['patchedSha256'] for n, v in image['files'].items()):
        raise SystemExit('Patched-source digest verification failed')
    print('Pinned Mesa native-image patch applied and verified')


if __name__ == '__main__':
    main()
