#!/usr/bin/env python3
"""Apply the additive native-sync patch to the verified Mesa timeline base."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source',type=Path)
    args=parser.parse_args(); root=args.source.resolve(strict=True)
    here=Path(__file__).resolve().parent
    manifest=json.loads((here/'native-sync-patch.json').read_text())
    base=json.loads((here/'fence-fd-patch.json').read_text())
    patch=here/'0002-linux-d3d12-native-sync-files.patch'
    digest=lambda path:hashlib.sha256(path.read_bytes()).hexdigest()
    if not root.is_dir() or (root/'VERSION').read_text().strip()!=manifest['mesaVersion']:
        parser.error('Requires the pinned Mesa version')
    if digest(patch)!=manifest['patchSha256'] or manifest['requiresBasePatchSha256']!=base['patchSha256'] or \
            digest(here/'0001-linux-d3d12-timeline-fence-fds.patch')!=base['patchSha256']:
        parser.error('Native-sync/base patch digests do not match')
    observed={}
    for name in set(manifest['files'])|set(base['files']):
        path=(root/name).resolve(strict=True)
        if not path.is_relative_to(root) or not path.is_file():
            parser.error('Source file escapes supplied Mesa directory')
        observed[name]=digest(path)
    if any(observed[n]!=v['patchedSha256'] for n,v in base['files'].items() if n not in manifest['files']):
        parser.error('Apply the pinned base patch first; unrelated/changed source is rejected')
    if all(observed[n]==v['patchedSha256'] for n,v in manifest['files'].items()):
        print('Pinned Mesa native-sync patch already verified'); return
    if not all(observed[n]==v['originalSha256'] for n,v in manifest['files'].items()):
        parser.error('Source differs from verified timeline base; no files changed')
    command=['patch','--batch','--fuzz=0','-p1','-i',str(patch)]
    subprocess.run(command+['--dry-run'],cwd=root,check=True)
    subprocess.run(command,cwd=root,check=True)
    if not all(digest(root/n)==v['patchedSha256'] for n,v in manifest['files'].items()):
        raise SystemExit('Patched-source digest verification failed')
    print('Pinned Mesa native-sync patch applied and verified')


if __name__=='__main__': main()
