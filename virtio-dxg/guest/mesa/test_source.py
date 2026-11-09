"""Run CPU fixtures around extracted source; never load Mesa or create a device."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
LEGACY = HERE.parents[2] / 'experimental/windows-native-gpu/mesa'
sys.path.insert(0, str(LEGACY))
from apply_mesa_recipe import apply_recipe, load_recipe


def function(text, name):
    # Anchor the definition; calls/comments are not source definitions.
    match = re.search(r'(?m)^(?:static )?(?:inline )?(?:bool|void|struct pipe_resource \*)\s*\n' +
                      re.escape(name) + r'\(', text)
    if not match:
        raise ValueError('Function definition missing: ' + name)
    start = match.start()
    opening = text.index('{', match.end())
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end] + '\n'


def run_fixture(out, name, template, actual, expected, diagnostic):
    unit = out / (name + '.cpp')
    unit.write_text(template.replace('// ACTUAL_FUNCTIONS', actual), encoding='utf-8')
    binary = out / name
    result = subprocess.run(['c++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                             str(unit), '-o', str(binary)], capture_output=True, timeout=90)
    (out / (name + '-compile.log')).write_bytes(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError('CPU fixture did not compile: ' + name)
    result = subprocess.run([str(binary)], capture_output=True, timeout=10)
    (out / (name + '-run.log')).write_bytes(result.stdout + result.stderr)
    if result.returncode != expected or diagnostic.encode() not in result.stdout + result.stderr:
        raise RuntimeError('Unexpected CPU fixture result: ' + name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True, help='Pristine Mesa 26.2.6')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    recipe = HERE / 'versions/26.2.6'
    manifest, profile, _ = load_recipe(recipe, 'graphics')

    def copy_source(label):
        root = out / label
        root.mkdir()
        for name, record in profile['files'].items():
            if record['originalSha256'] is not None:
                target = root / name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(args.source / name, target)
        return root

    fixed = copy_source('valid')
    apply_recipe(fixed, recipe, 'graphics')
    if apply_recipe(fixed, recipe, 'graphics'):
        raise RuntimeError('Recipe was not idempotent')
    for label, relative, value in [('wrong-version', 'VERSION', b'0.0.0\n'),
                                    ('changed-source', 'src/gallium/drivers/d3d12/d3d12_resource.cpp', b'changed')]:
        root = copy_source(label)
        (root / relative).write_bytes(value)
        before = {p: p.read_bytes() for p in root.rglob('*') if p.is_file()}
        try:
            apply_recipe(root, recipe, 'graphics')
        except ValueError:
            pass
        else:
            raise RuntimeError('Invalid recipe input accepted: ' + label)
        if any(p.read_bytes() != data for p, data in before.items()):
            raise RuntimeError('Rejected input changed source')

    driver = fixed / 'src/gallium/drivers/d3d12'
    raw = (driver / 'd3d12_resource.cpp').read_text(encoding='utf-8')
    mirror = '\n'.join(function(raw, name) for name in (
        'd3d12_scanout_try_reference', 'd3d12_scanout_active',
        'd3d12_mirror_reference', 'd3d12_mirror_sync'))
    template = (HERE / 'mirror_cases.cpp.in').read_text().replace(
        '// ACTUAL_MIRROR_FUNCTIONS', '// ACTUAL_FUNCTIONS')
    run_fixture(out, 'mirror-patched', template, mirror, 0, 'MIRROR_CPU_PASS cases=14')
    # Use the actual old batch function to show the shared-bit defect, not a
    # second hand-written version of its algorithm.
    old = copy_source('claude-baseline')
    subprocess.run(['patch', '--batch', '--fuzz=0', '-p1', '-i',
                    str(HERE / '0001-d3d12-kms-mirror.patch')], cwd=old,
                   check=True, timeout=30)
    baseline = function((old / 'src/gallium/drivers/d3d12/d3d12_batch.cpp').read_text(),
                        'd3d12_batch_reference_resource')
    baseline += '''\nvoid d3d12_mirror_reference(struct d3d12_context *ctx, struct d3d12_resource *res) {
   d3d12_batch_reference_resource(&ctx->batch, res, true);
}\n'''
    original_mirror = mirror.replace(function(raw, 'd3d12_mirror_reference'), baseline)
    run_fixture(out, 'mirror-baseline', template, original_mirror, 1,
                'Second context write was suppressed')

    fence_template = (LEGACY / 'mesa_fence_port_cases.h').read_text()
    # This profile has the real dxg kernel, not the old private sync-file ABI.
    a = fence_template.index('      for (int mode = 0; mode < 5; ++mode)')
    b = fence_template.index('      std::printf(', a)
    fence_template = (fence_template[:a] + fence_template[b:]).replace(
        '// ACTUAL_MESA_FENCE_FUNCTIONS', '// ACTUAL_FUNCTIONS')
    for label, source, expected, message in (
        ('fence-patched', driver, 0, 'MESA_FENCE_PORT_CPU_PASS cases=15'),
        ('fence-baseline', args.source / 'src/gallium/drivers/d3d12', 1,
         'Initial device removal must fail')):
        text = (source / 'd3d12_fence.cpp').read_text(encoding='utf-8')
        actual = function(text, 'd3d12_fence_ensure_event_registered') + function(text, 'd3d12_fence_finish')
        run_fixture(out, label, fence_template, actual, expected, message)
    reclaim_template = (HERE / 'reclaim_cases.cpp.in').read_text()
    for label, source, expected, message in (
        ('reclaim-patched', driver, 0, 'RECLAIM_CPU_PASS cases=10'),
        ('reclaim-baseline', args.source / 'src/gallium/drivers/d3d12', 1,
         'Device removal retired pending resources')):
        text = (source / 'd3d12_bufmgr.cpp').read_text(encoding='utf-8')
        actual = function(text, 'd3d12_screen_reclaim_completed') + function(text, 'd3d12_screen_reclaim_one')
        run_fixture(out, label, reclaim_template, actual, expected, message)
    receipt = {'schema': 1, 'recipeSha256': hashlib.sha256((recipe / 'recipe.json').read_bytes()).hexdigest(),
               'mesaVersion': manifest['mesaVersion'], 'recipeControls': 4,
               'actualMirrorCpuCases': 14, 'actualFenceCpuCases': 15,
               'actualReclaimCpuCases': 10,
               'baselineDefectsReproduced': ['shared-dirty-bit', 'device-removal-completion', 'device-removal-reclaim'],
               'graphicsLibraryLoaded': False, 'hardwareIntegrationTested': False}
    (out / 'result.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps(receipt, indent=2))


if __name__ == '__main__':
    main()
