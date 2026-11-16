"""Apply the opt-in native seat/input layer to the exact pinned Aquamarine prototype."""
import argparse
import hashlib
import json
import os
from contextlib import contextmanager
from pathlib import Path

from apply_aquamarine_native_render_patch import apply


@contextmanager
def exact_git_newlines():
    count = os.environ.get('GIT_CONFIG_COUNT', '0')
    if not count.isdecimal() or int(count) >= 64:
        raise ValueError('Invalid inherited Git configuration count')
    index = str(int(count))
    added = {'GIT_CONFIG_COUNT': str(int(count) + 1),
             'GIT_CONFIG_KEY_' + index: 'core.autocrlf',
             'GIT_CONFIG_VALUE_' + index: 'false'}
    previous = {key: os.environ.get(key) for key in added}
    os.environ.update(added)
    try:
        yield
    finally:
        for key, value in previous.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value


def apply_input(source):
    source = source.resolve(strict=True)
    base = Path(__file__).resolve().parent
    prior = json.loads((base / 'aquamarine-native-render.lock.json').read_text())
    current = json.loads((base / 'aquamarine-native-input.lock.json').read_text())
    capture = json.loads((base / 'aquamarine-native-capture.lock.json').read_text())
    logging = json.loads((base / 'aquamarine-logging-lifetime.lock.json').read_text())
    digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    if any(lock['schema'] != 1 for lock in (prior, current, capture, logging)) or (source / 'VERSION').read_text().strip() != '0.15.1':
        raise ValueError('Pinned Aquamarine input source/version required')
    if digest(base / current['patch']) != current['patchSha256']:
        raise ValueError('Native input patch changed')
    if current['requiresPatchSha256'] != prior['patchSha256'] or digest(base / prior['patch']) != prior['patchSha256']:
        raise ValueError('Native input requires the exact pinned native-render patch')
    if capture['requiresPatchSha256'] != prior['patchSha256'] or digest(base / capture['patch']) != capture['patchSha256']:
        raise ValueError('Native input capture layer changed')
    if digest(base / logging['patch']) != logging['patchSha256']:
        raise ValueError('Native input logging layer changed')
    # Validate every layer before applying any of them. A foreign logging file
    # must not leave a previously unpatched capture layer half-installed.
    for name, pins in logging['files'].items():
        target = (source / name).resolve(strict=True)
        if not target.is_relative_to(source) or digest(target) not in {pins['beforeSha256'], pins['afterSha256']}:
            raise ValueError('Logging source changed before input layering: ' + name)
    for name, pins in prior['files'].items():
        allowed = {pins['afterSha256']}
        if name in current['files']:
            if current['files'][name]['beforeSha256'] != pins['afterSha256']:
                raise ValueError('Input patch does not extend the pinned render source')
            allowed.add(current['files'][name]['afterSha256'])
        if name in capture['files']:
            if capture['files'][name]['beforeSha256'] != pins['afterSha256']:
                raise ValueError('Capture patch does not extend the pinned render source')
            allowed.add(capture['files'][name]['afterSha256'])
        target = (source / name).resolve(strict=True)
        if not target.is_relative_to(source) or digest(target) not in allowed:
            raise ValueError('Native-render source changed before input layering: ' + name)
    with exact_git_newlines():
        apply(source, 'aquamarine-native-capture.lock.json')
        apply(source, 'aquamarine-logging-lifetime.lock.json')
        apply(source, 'aquamarine-native-input.lock.json')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    args = parser.parse_args()
    apply_input(args.source)
