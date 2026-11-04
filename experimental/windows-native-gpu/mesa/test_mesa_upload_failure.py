"""Compile the complete pinned uploader with CPU callback stubs; no GPU runtime."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--msvc', action='store_true')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    manifest = json.loads((here / 'upload-failure-patch.json').read_text(encoding='utf-8'))
    relative = 'src/gallium/auxiliary/util/u_upload_mgr.c'
    original = (args.source / relative).read_bytes()
    digest = lambda data: hashlib.sha256(data).hexdigest()
    if ((args.source / 'VERSION').read_text(encoding='utf-8').strip() != manifest['mesaVersion'] or
            digest(original) != manifest['files'][relative]['originalSha256']):
        parser.error('Use a fresh pinned Mesa source tree with the original uploader')
    def check(condition, message):
        if not condition:
            raise RuntimeError(message)

    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    creationflags = getattr(subprocess, 'CREATE_NO_WINDOW', 0)

    def recipe(root, script=here / 'apply_mesa_upload_failure.py', expected=0):
        result = subprocess.run([sys.executable, str(script), str(root)], capture_output=True,
                                timeout=15, creationflags=creationflags)
        if (result.returncode == 0) != (expected == 0):
            raise RuntimeError('Patch guard returned the wrong result: ' + result.stderr.decode('utf-8', 'replace'))

    def tree(name, version='26.0.8\n', source=original):
        root = out / name
        target = root / relative
        target.parent.mkdir(parents=True)
        (root / 'VERSION').write_text(version, encoding='utf-8', newline='\n')
        target.write_bytes(source)
        return root

    patched = tree('patched-source')
    recipe(patched)
    fixed = (patched / relative).read_bytes()
    check(digest(fixed) == manifest['files'][relative]['patchedSha256'], 'Patched digest differs')
    before = (patched / relative).stat().st_mtime_ns
    recipe(patched)
    check((patched / relative).read_bytes() == fixed, 'Repeated patch changed source')
    check((patched / relative).stat().st_mtime_ns == before, 'Repeated patch rewrote source')
    for name, version, source in [('wrong-version', '0.0\n', original),
                                  ('modified-source', '26.0.8\n', original + b'/* modified */\n')]:
        root = tree(name, version, source)
        recipe(root, expected=1)
        check((root / relative).read_bytes() == source, 'Rejected input was modified')
    bad_recipe = out / 'bad-recipe'
    bad_recipe.mkdir()
    for name in ('apply_mesa_upload_failure.py', 'upload-failure-patch.json',
                 '0009-gallium-upload-failure-lifetime.patch'):
        shutil.copyfile(here / name, bad_recipe / name)
    with (bad_recipe / '0009-gallium-upload-failure-lifetime.patch').open('ab') as stream:
        stream.write(b'corrupted patch\n')
    root = tree('bad-patch')
    recipe(root, bad_recipe / 'apply_mesa_upload_failure.py', expected=1)
    check((root / relative).read_bytes() == original, 'Corrupt patch modified source')

    stubs = (here / 'upload_failure_stubs.h').read_text(encoding='utf-8')
    cases = (here / 'upload_failure_cases.h').read_text(encoding='utf-8')
    receipts = {}
    for label, data in [('baseline', original), ('patched', fixed)]:
        # Keep all uploader functions. Replace only includes with the callback
        # fixture. The upstream unsigned ~0 sentinel needs C4245 suppression in
        # MSVC; fixture code remains under the strict warning configuration.
        source = re.sub(r'^#include[^\n]*\n', '', data.decode('utf-8'), flags=re.M)
        source = ('#ifdef _MSC_VER\n#pragma warning(push)\n#pragma warning(disable:4245)\n#endif\n' +
                  source + '\n#ifdef _MSC_VER\n#pragma warning(pop)\n#endif\n')
        unit = out / (label + '.c')
        unit.write_text(stubs + '\n' + source + '\n' + cases, encoding='utf-8', newline='\n')
        executable = out / (label + ('.exe' if sys.platform == 'win32' else ''))
        if args.msvc:
            command = [args.cc, '/nologo', '/TC', '/std:c11', '/W4', '/WX', '/O2',
                       '/DNDEBUG', str(unit), '/Fe:' + str(executable)]
        else:
            command = [args.cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2',
                       '-DNDEBUG', str(unit), '-o', str(executable)]
        result = subprocess.run(command, cwd=out, capture_output=True, timeout=90,
                                creationflags=creationflags)
        (out / (label + '-compile.log')).write_bytes(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(label + ' CPU fixture compile failed; see its private log')
        result = subprocess.run([str(executable)], cwd=out, capture_output=True,
                                timeout=15, creationflags=creationflags)
        (out / (label + '-results.jsonl')).write_bytes(result.stdout)
        if result.returncode:
            raise RuntimeError(label + ' CPU model invariant failed')
        rows = [json.loads(line) for line in result.stdout.splitlines()]
        if len(rows) != 10 or any(row['modelErrors'] for row in rows):
            raise RuntimeError('Unexpected callback-model results')
        receipts[label] = rows
    baseline, fixed_rows = receipts['baseline'], receipts['patched']
    check(sum(row.get('danglingOwner', False) for row in baseline) == 4, 'Baseline dangling ownership was not reproduced')
    check(sum(row['doubleRelease'] for row in baseline) == 4, 'Baseline duplicate release was not reproduced')
    check(sum(row['liveAfterDestroy'] for row in baseline) == 4, 'Baseline lost reference was not reproduced')
    check(all(not row.get('danglingOwner', False) and not row['doubleRelease'] and
              not row['liveAfterDestroy'] for row in fixed_rows), 'Patched uploader retained a fault')
    check(all(row['oldReturned'] == (row['scenario'] >= 2)
              for row in fixed_rows if not row.get('positive')), 'Retired buffer ownership was not returned')
    proof = dict(schema=1, mesaVersion=manifest['mesaVersion'], actualWholeUploaderCompiled=True,
                 onlyIncludeDirectivesReplaced=True, cpuCallbackModel=True,
                 libraryAssertionsDisabled=True, explicitFixtureChecksActive=True,
                 sourceSha256=manifest['files'][relative],
                 baselineCases=10, patchedCases=10, positiveDataCopies=4,
                 baselineDanglingOwners=4, baselineDuplicateReleases=4, baselineLeakedResources=4,
                 patchedDanglingOwners=0, patchedDuplicateReleases=0, patchedLeakedResources=0,
                 patchIdempotent=True, rejectedInputs=3, inputTreeUnchanged=True,
                 gpuUsed=False, vmLaunched=False, wslLaunched=False,
                 intentionalNativeCrash=False, quickshellCrashFixed=False)
    check((args.source / relative).read_bytes() == original, 'Input source tree changed')
    (out / 'receipt.json').write_text(json.dumps(proof, indent=2) + '\n', encoding='utf-8')
    print('MESA_UPLOAD_FAILURE_CPU_PASS baselineFaultsReproduced=true patchedCases=10 guards=3 gpu=false')


if __name__ == '__main__':
    main()
