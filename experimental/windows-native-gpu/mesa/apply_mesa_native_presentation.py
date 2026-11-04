#!/usr/bin/env python3
"""Apply the pinned owned opaque presentation patch after four Mesa predecessors."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source',type=Path)
    args=parser.parse_args()
    root=args.source.resolve(strict=True)
    here=Path(__file__).resolve().parent
    digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
    stages=[('fence-fd-patch.json','0001-linux-d3d12-timeline-fence-fds.patch',None),
            ('native-sync-patch.json','0002-linux-d3d12-native-sync-files.patch','requiresBasePatchSha256'),
            ('native-image-patch.json','0003-linux-d3d12-native-images.patch','requiresNativeSyncPatchSha256'),
            ('native-render-patch.json','0004-linux-d3d12-native-render-node.patch','requiresNativeImagePatchSha256'),
            ('native-presentation-patch.json','0011-linux-d3d12-owned-presentation.patch','requiresNativeRenderPatchSha256')]
    predecessors={}; previous=None
    for manifest,patchname,requires in stages:
        stage=json.loads((here/manifest).read_text(encoding='utf-8'))
        if (root/'VERSION').read_text(encoding='utf-8').strip()!=stage['mesaVersion']:
            parser.error('Requires the pinned Mesa version')
        if digest(here/patchname)!=stage['patchSha256'] or (requires and stage[requires]!=previous):
            parser.error('Mesa patch chain digests do not match')
        if manifest!='native-presentation-patch.json': predecessors.update(stage['files'])
        previous=stage['patchSha256']
    observed={}
    for name in set(predecessors)|set(stage['files']):
        path=(root/name).resolve()
        if not path.is_relative_to(root) or (path.exists() and not path.is_file()):
            parser.error('Source escapes supplied Mesa directory or is not a file')
        observed[name]=digest(path) if path.is_file() else None
    if any(observed[n]!=v['patchedSha256'] for n,v in predecessors.items() if n not in stage['files']):
        parser.error('Apply the four pinned graphics predecessors first; unrelated source changes are rejected')
    if all(observed[n]==v['patchedSha256'] for n,v in stage['files'].items()):
        print('Pinned Mesa owned-presentation patch already verified')
        return
    if not all(observed[n]==v['originalSha256'] for n,v in stage['files'].items()):
        parser.error('Source differs from verified native-render base; no files changed')
    command=['patch','--batch','--fuzz=0','-p1','-i',str(here/patchname)]
    subprocess.run(command+['--dry-run'],cwd=root,check=True,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
    subprocess.run(command,cwd=root,check=True,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
    if not all(digest(root/n)==v['patchedSha256'] for n,v in stage['files'].items()):
        raise SystemExit('Patched-source digest verification failed')
    print('Pinned Mesa owned-presentation patch applied and verified')


if __name__=='__main__': main()
