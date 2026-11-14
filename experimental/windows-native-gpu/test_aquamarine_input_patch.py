"""Exercise exact patch layering and rejection without building or using a GPU."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import subprocess

from apply_aquamarine_native_input_patch import apply_input


def controls(source, output):
    source = source.resolve(strict=True)
    output = output.resolve()
    output.mkdir(exist_ok=False)
    base = Path(__file__).resolve().parent
    names = {'VERSION'}
    for manifest in ('aquamarine-native-render.lock.json', 'aquamarine-logging-lifetime.lock.json',
                     'aquamarine-native-capture.lock.json', 'aquamarine-native-input.lock.json'):
        names.update(json.loads((base / manifest).read_text())['files'])
    for name in names:
        path = PurePosixPath(name)
        if path.is_absolute() or '..' in path.parts or '\\' in name or not (source / name).resolve().is_relative_to(source):
            raise ValueError('Control source path escaped checkout')
    original = {name: (source / name).read_bytes() for name in names}
    snapshot = lambda root: {name: (root / name).read_bytes() for name in names}

    def clone(name):
        target = output / name
        target.mkdir()
        for relative, data in original.items():
            destination = target / relative
            if not destination.resolve().is_relative_to(output):
                raise ValueError('Control destination escaped output directory')
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(data)
        return target

    target = clone('idempotent')
    environment = dict(os.environ)
    apply_input(target)
    assert snapshot(target) == original and dict(os.environ) == environment

    def reverse_layers(target):
        for patch in ('aquamarine-0.15.1-native-input.patch', 'aquamarine-0.15.1-native-capture.patch',
                      'aquamarine-0.15.1-logging-lifetime.patch'):
            subprocess.run(['git', '-c', 'core.autocrlf=false', 'apply', '-R', str(base / patch)], cwd=target, check=True)

    target = clone('fresh-input')
    reverse_layers(target)
    apply_input(target)
    assert snapshot(target) == original and dict(os.environ) == environment

    target = clone('foreign-logging-before-capture')
    reverse_layers(target)
    path = target / 'src/backend/Session.cpp'
    path.write_bytes(path.read_bytes() + b'\n/* foreign before applying any layer */\n')
    before = snapshot(target)
    try:
        apply_input(target)
    except ValueError:
        pass
    else:
        raise AssertionError('Foreign logging source accepted before capture')
    assert snapshot(target) == before and dict(os.environ) == environment

    for case, name in (('foreign-render', 'src/NativeRender.cpp'),
                       ('partial-input', 'src/backend/Backend.cpp'),
                       ('foreign-logging', 'src/backend/Session.cpp'),
                       ('wrong-version', 'VERSION')):
        target = clone(case)
        (target / name).write_bytes(original[name] + b'\n/* unrelated mutation */\n')
        before = snapshot(target)
        try:
            apply_input(target)
        except ValueError:
            pass
        else:
            raise AssertionError('Unexpected acceptance: ' + case)
        assert snapshot(target) == before and dict(os.environ) == environment

    previous = os.environ.get('GIT_CONFIG_COUNT')
    try:
        target = clone('leading-zero-config')
        os.environ['GIT_CONFIG_COUNT'] = '00'
        apply_input(target)
        assert snapshot(target) == original and os.environ['GIT_CONFIG_COUNT'] == '00'
    finally:
        if previous is None:
            os.environ.pop('GIT_CONFIG_COUNT', None)
        else:
            os.environ['GIT_CONFIG_COUNT'] = previous
    assert dict(os.environ) == environment
    print('AQUAMARINE_INPUT_PATCH_CONTROLS checks=8 exactSources=true idempotent=true '
          'foreignAndPartialRejected=true gitEnvironmentRestored=true native=none gpu=false')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path, help='Fresh control directory; retained for inspection')
    args = parser.parse_args()
    controls(args.source, args.output)
