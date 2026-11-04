"""Check the pinned presentation recipe on genuine source copies; no graphics."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path,
                        help='Pinned Mesa after graphics patches 1-4, before presentation')
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    source = args.source.resolve(strict=True)
    out = args.output.resolve()
    check(not out.is_relative_to(source), 'Test output must be outside the supplied source')
    digest = lambda raw: hashlib.sha256(raw).hexdigest()
    names = ['fence-fd-patch.json', 'native-sync-patch.json',
             'native-image-patch.json', 'native-render-patch.json']
    expected = {}
    recipe_files = ['apply_mesa_native_presentation.py', 'native-presentation-patch.json',
                    '0011-linux-d3d12-owned-presentation.patch']
    patch_names = ['0001-linux-d3d12-timeline-fence-fds.patch',
                   '0002-linux-d3d12-native-sync-files.patch',
                   '0003-linux-d3d12-native-images.patch',
                   '0004-linux-d3d12-native-render-node.patch']
    for name, patch in zip(names, patch_names):
        manifest = json.loads((here / name).read_text(encoding='utf-8'))
        expected.update({n: r['patchedSha256'] for n, r in manifest['files'].items()})
        recipe_files.extend([name, patch])
    manifest = json.loads((here / 'native-presentation-patch.json').read_text(encoding='utf-8'))
    check((source / 'VERSION').read_text(encoding='utf-8').strip() == manifest['mesaVersion'],
          'Requires the pinned Mesa version')
    expected.update({n: r['originalSha256'] for n, r in manifest['files'].items()})
    original = {n: (source / n).read_bytes() for n in expected}
    check(all(digest(original[n]) == v for n, v in expected.items()),
          'Source differs from the pinned native-render base')
    out.mkdir(parents=True, exist_ok=False)
    frozen = out / 'recipe'
    frozen.mkdir()
    for name in recipe_files:
        shutil.copyfile(here / name, frozen / name)
    cases = []

    def tree(name):
        root = out / name
        for relative, raw in original.items():
            p = root / relative
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(raw)
        (root / 'VERSION').write_text(manifest['mesaVersion'] + '\n', encoding='utf-8', newline='\n')
        return root

    def snapshot(root):
        return {p.relative_to(root).as_posix(): digest(p.read_bytes())
                for p in root.rglob('*') if p.is_file()}

    def apply(root, succeeds=True, recipe=frozen):
        before = snapshot(root)
        p = subprocess.run([sys.executable, str(recipe / 'apply_mesa_native_presentation.py'), str(root)],
                           capture_output=True, timeout=20,
                           creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        check((p.returncode == 0) == succeeds,
              'Unexpected applicator result: ' + p.stderr.decode('utf-8', 'replace'))
        if not succeeds:
            check(snapshot(root) == before, 'Rejected recipe modified source files')
        cases.append(dict(name=root.name,expectedSuccess=succeeds,returnCode=p.returncode))

    fixed = tree('apply')
    apply(fixed)
    for n, record in manifest['files'].items():
        check(digest((fixed / n).read_bytes()) == record['patchedSha256'], 'Patched source differs')
    times = {n: (fixed / n).stat().st_mtime_ns for n in manifest['files']}
    apply(fixed)
    check(all((fixed / n).stat().st_mtime_ns == t for n, t in times.items()),
          'Repeated application rewrote patched files')
    root = tree('wrong-version')
    (root / 'VERSION').write_text('0.0\n', encoding='utf-8', newline='\n')
    apply(root, False)
    for label, name in [('changed-predecessor', 'src/gallium/auxiliary/target-helpers/drm_helper_public.h'),
                        ('changed-target', 'src/gallium/drivers/d3d12/d3d12_native_buffer.h')]:
        root = tree(label)
        with (root / name).open('ab') as f:
            f.write(b'/* changed */\n')
        apply(root, False)
    root = tree('mixed-targets')
    name = next(iter(manifest['files']))
    (root / name).write_bytes((fixed / name).read_bytes())
    apply(root, False)
    for label, name in [('corrupted-patch', '0011-linux-d3d12-owned-presentation.patch'),
                        ('broken-chain', 'native-presentation-patch.json')]:
        recipe = out / (label + '-recipe')
        shutil.copytree(frozen, recipe)
        if label == 'corrupted-patch':
            with (recipe / name).open('ab') as f:
                f.write(b'corrupted patch\n')
        else:
            changed = json.loads((recipe / name).read_text(encoding='utf-8'))
            changed['requiresNativeRenderPatchSha256'] = '0' * 64
            (recipe / name).write_text(json.dumps(changed), encoding='utf-8', newline='\n')
        apply(tree(label), False, recipe)
    result = dict(schema=1,sourceFilesVerified=len(original),changedSourceFiles=len(manifest['files']),
                  patchSha256=manifest['patchSha256'],cases=cases,idempotencePreservesMtime=True,
                  rejectedSourcesUnchanged=True,gpuWorkloadRun=False,runtimeLoaded=False,
                  vmLaunched=False,hardwareAcceptance=False,dailyUseAccepted=False)
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8', newline='\n')
    print('PRESENTATION_SOURCE_RECIPE_PASS cases=' + str(len(cases)) + ' gpu=false')


if __name__ == '__main__':
    main()
