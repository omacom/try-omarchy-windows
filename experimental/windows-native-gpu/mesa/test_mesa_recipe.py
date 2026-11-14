"""CPU-only recipe rejection and real-source fence controls; never load Mesa."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

from apply_mesa_recipe import apply_recipe, load_recipe


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def function(text, name):
    start = text.index(name + '(')
    start = text.rfind('\n', 0, text.rfind('\n', 0, start)) + 1
    opening = text.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end] + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path, help='Pristine Mesa source')
    parser.add_argument('--recipe', required=True, type=Path)
    parser.add_argument('--profile', required=True, choices=('graphics', 'dozen'))
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    manifest, stage, _ = load_recipe(args.recipe, args.profile)
    source = args.source.resolve(strict=True)
    out = args.output.resolve()
    check(not out.is_relative_to(source), 'Output must be outside source')
    out.mkdir(parents=True, exist_ok=False)
    cases = []

    def tree(label):
        root = out / label
        root.mkdir()
        (root / 'VERSION').write_text(manifest['mesaVersion'] + '\n', encoding='utf-8')
        for relative, record in stage['files'].items():
            if record['originalSha256'] is not None:
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source / relative, path)
        return root

    def snapshot(root):
        return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in root.rglob('*') if p.is_file()}

    def reject(root, recipe=args.recipe):
        before = snapshot(root)
        try:
            apply_recipe(root, recipe, args.profile)
        except ValueError:
            pass
        else:
            raise RuntimeError('Invalid source/recipe was accepted: ' + root.name)
        check(snapshot(root) == before, 'Rejected input changed source: ' + root.name)
        cases.append(root.name)

    fixed = tree('valid')
    check(apply_recipe(fixed, args.recipe, args.profile), 'First application skipped')
    times = {p: p.stat().st_mtime_ns for p in fixed.rglob('*') if p.is_file()}
    check(not apply_recipe(fixed, args.recipe, args.profile), 'Repeated application changed source')
    check(all(p.stat().st_mtime_ns == time for p, time in times.items()), 'Idempotence rewrote files')
    cases.extend(['valid', 'idempotent'])
    root = tree('wrong-version')
    (root / 'VERSION').write_text('0.0.0\n', encoding='utf-8')
    reject(root)
    existing = next(n for n, r in stage['files'].items() if r['originalSha256'] is not None)
    changed = next(n for n, r in stage['files'].items()
                   if r['originalSha256'] != r['patchedSha256'])
    root = tree('modified-source')
    with (root / existing).open('ab') as stream:
        stream.write(b'/* modified */\n')
    reject(root)
    root = tree('mixed-stages')
    path = root / changed
    path.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(fixed / changed, path)
    reject(root)
    for label in ('corrupted-patch', 'escaping-target'):
        recipe = out / (label + '-recipe')
        shutil.copytree(args.recipe, recipe)
        if label == 'corrupted-patch':
            with (recipe / stage['patchFile']).open('ab') as stream:
                stream.write(b'corruption\n')
        else:
            document = json.loads((recipe / 'recipe.json').read_text(encoding='utf-8'))
            document['profiles'][args.profile]['files']['../outside.c'] = dict(
                originalSha256=None, patchedSha256='0' * 64)
            (recipe / 'recipe.json').write_text(json.dumps(document), encoding='utf-8')
        reject(tree(label), recipe)

    fence_cases = 0
    if args.profile == 'graphics':
        relative = 'src/gallium/drivers/d3d12/d3d12_fence.cpp'
        before = (source / relative).read_text(encoding='utf-8')
        after = (fixed / relative).read_text(encoding='utf-8')
        template = (Path(__file__).parent / 'mesa_fence_port_cases.h').read_text(encoding='utf-8')
        for label, text in [('baseline', before), ('patched', after)]:
            actual = function(text, 'd3d12_fence_ensure_event_registered') + '\n' + function(text, 'd3d12_fence_finish')
            unit = out / (label + '.cpp')
            unit.write_text(template.replace('// ACTUAL_MESA_FENCE_FUNCTIONS', actual),
                            encoding='utf-8', newline='\n')
            binary = out / label
            result = subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                     '-O2', str(unit), '-o', str(binary)],
                                    capture_output=True, timeout=90)
            (out / (label + '-compile.log')).write_bytes(result.stdout + result.stderr)
            check(result.returncode == 0, 'Fence fixture compilation failed: ' + label)
            result = subprocess.run([str(binary)], capture_output=True, timeout=10)
            (out / (label + '-run.log')).write_bytes(result.stdout + result.stderr)
            if label == 'baseline':
                check(result.returncode == 1 and b'Initial device removal must fail' in result.stderr,
                      'Unpatched fence failed to reproduce the device-removal completion defect')
            else:
                check(result.returncode == 0 and b'MESA_FENCE_PORT_CPU_PASS cases=20' in result.stdout,
                      'Patched real-source fence controls failed')
                fence_cases = 20
        resource = (fixed / 'src/gallium/drivers/d3d12/d3d12_resource.cpp').read_text(encoding='utf-8')
        retry = resource.index('   do {\n   /* Reclaim can retry')
        preparation = resource.index('d3d12_native_resource_preparation native_preparation;', retry)
        create = resource.index('CreatePlacedResource2(', preparation)
        cancel = resource.index('native_preparation.cancel();', create)
        repeat = resource.index('} while (hres == E_OUTOFMEMORY && d3d12_screen_reclaim_one(screen));', cancel)
        check(retry < preparation < create < cancel < repeat,
              'Native declaration must be renewed and cancelled inside the reclaim loop')
    result = dict(schema=1, mesaVersion=manifest['mesaVersion'], profile=args.profile,
                  recipeCases=cases, recipeCasesPassed=len(cases),
                  actualFenceCpuCasesPassed=fence_cases,
                  upstreamFenceSentinelDefectReproduced=fence_cases > 0,
                  resourceRetrySourceScopeChecked=fence_cases > 0,
                  resourceRetryHardwareTested=False, runtimeLoaded=False,
                  gpuWorkloadRun=False, vmLaunched=False, dailyUseAccepted=False)
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
