"""Exercise the complete pinned renderer factory, plus guarded source application.

GPU/framework constructors are mocked; timer descriptors are real. Also compiles
the original complete factory to prove legacy construction and the actual native
selection regression. No GPU, guest, desktop, account or input is used.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess

GPU = Path(__file__).resolve().parent
FILES = ('apply_aquamarine_session_selection.py', 'test_aquamarine_session_selection.py',
         'aquamarine_selection_fixture.cpp.in')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def hashes(root):
    return {path.relative_to(root).as_posix(): sha(path) for path in root.rglob('*')
            if path.is_file() and '.git' not in path.relative_to(root).parts}


def factory(text):
    start = text.index('Hyprutils::Memory::CSharedPointer<CBackend> Aquamarine::CBackend::create(')
    end = text.index('\nAquamarine::CBackend::~CBackend()', start)
    selected = text[start:end].strip()
    if not selected.endswith('}'):
        raise ValueError('Factory extraction boundary changed')
    return selected + '\n'


def run(source, output, cxx='c++'):
    source, output = Path(source).resolve(), Path(output).resolve()
    output.mkdir(parents=True, exist_ok=False)
    frozen = output/'control-source'
    frozen.mkdir()
    for name in FILES:
        (frozen/name).write_bytes((GPU/name).read_bytes())
    spec = importlib.util.spec_from_file_location('session_selection_frozen', frozen/FILES[0])
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    target = output/'source'
    shutil.copytree(source, target, ignore=shutil.ignore_patterns('.git'))
    before = hashes(target)
    path = target/'src/backend/Backend.cpp'
    original = path.read_bytes()
    module.apply(target)
    selected = path.read_bytes()
    after = hashes(target)
    assert before.keys() == after.keys()
    assert [name for name in before if before[name] != after[name]] == ['src/backend/Backend.cpp']
    assert selected.decode().replace(module.AFTER, module.BEFORE, 1).encode() == original
    module.apply(target)
    assert hashes(target) == after
    assert sha(path) == module.SELECTED
    boundaries = 0
    for damaged in (original.decode().replace(module.BEFORE, '', 1),
                    selected.decode().replace(module.AFTER, module.AFTER+module.AFTER, 1)):
        try:
            module.patch_text(damaged)
        except ValueError:
            boundaries += 1
        else:
            raise AssertionError('Damaged source boundary accepted')
    negatives = []
    for name, relative, content in (
        ('altered-original', 'src/backend/Backend.cpp', original+b'// unexpected source\n'),
        ('altered-selected', 'src/backend/Backend.cpp', selected+b'// unexpected source\n'),
        ('partial-selection', 'src/backend/Backend.cpp', selected.replace(b'            continue;\n', b'            ;\n', 1)),
        ('wrong-version', 'VERSION', b'0.15.2\n'),
        ('missing-session-adapter', 'src/backend/Headless.cpp', b'// no persistent session\n'),
    ):
        case = output/('negative-'+name)
        shutil.copytree(target, case)
        (case/relative).write_bytes(content)
        previous = hashes(case)
        try:
            module.apply(case)
        except ValueError:
            pass
        else:
            raise AssertionError(name+' accepted')
        assert hashes(case) == previous
        negatives.append(name)
    case = output/'negative-symlink'
    shutil.copytree(target, case)
    outside = output/'outside-backend.cpp'
    outside.write_bytes(selected)
    link = case/'src/backend/Backend.cpp'
    link.unlink()
    link.symlink_to(outside)
    try:
        module.apply(case)
    except ValueError:
        pass
    else:
        raise AssertionError('Symlinked backend accepted')
    assert link.is_symlink() and outside.read_bytes() == selected
    negatives.append('symlink')

    template = (frozen/'aquamarine_selection_fixture.cpp.in').read_text()
    marker = '/* @PRODUCTION_FACTORY@ */'
    assert template.count(marker) == 1
    builds = []
    for name, data, flags in (('selected', selected, []), ('original', original, ['-DFACTORY_BASELINE=1'])):
        cpp = output/(name+'-factory.cpp')
        cpp.write_text(template.replace(marker, factory(data.decode())), encoding='utf-8', newline='\n')
        command = [cxx, '-std=c++23', '-O2', '-Wall', '-Wextra', '-Werror', *flags,
                   str(cpp), '-o', str(output/(name+'-factory'))]
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        (output/(name+'-compile.log')).write_text(result.stdout)
        if result.returncode:
            raise RuntimeError(name+' fixture compile failed; see '+str(output))
        builds.append(dict(name=name, command=command, extractedFactorySha256=hashlib.sha256(factory(data.decode()).encode()).hexdigest()))
    results = []
    for name, arguments, expected in (('selected', [], 0), ('original', ['--default-only'], 0),
                                      ('original', ['--native-only'], 1)):
        result = subprocess.run([str(output/(name+'-factory')), *arguments], stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True)
        (output/(name+('-native' if expected else '')+'-run.log')).write_text(result.stdout)
        if result.returncode != expected or (expected and 'native constructor isolation' not in result.stdout):
            raise RuntimeError('Factory control failed: '+result.stdout)
        results.append(dict(name=name, arguments=arguments, exit=result.returncode, expectedExit=expected, output=result.stdout.strip()))
    assert hashes(target) == after
    receipt = dict(schema=1, sourceVersion='0.15.1', sourceFiles=len(before), changedFiles=['src/backend/Backend.cpp'],
                   otherSourceFilesUnchanged=len(before)-1, fullInverseExact=True, reapplicationExact=True,
                   rejectedSourceControls=negatives, rejectedBoundaryControls=boundaries,
                   backendOriginalSha256=module.ORIGINAL, backendSelectedSha256=module.SELECTED,
                   sourceSha256={name:sha(frozen/name) for name in FILES}, authoredSourceSha256=after,
                   builds=builds, factoryControls=results, realTimers=True, deviceConstructorsMocked=True,
                   gpuUsed=False, desktopLaunched=False, normalGuestIntegrationVerified=False)
    (output/'receipt.json').write_text(json.dumps(receipt, indent=2)+'\n')
    print('AQUAMARINE_SELECTION_PASS selectedControls=17 originalDefaults=6 originalNativeRejected=true sourceGuards=8 gpu=false')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source')
    parser.add_argument('output')
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    args = parser.parse_args()
    run(args.source, args.output, args.cxx)
