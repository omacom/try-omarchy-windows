"""Record the staged build without loading any installed ELF library."""
import argparse
import hashlib
import json
from pathlib import Path
import os


def record(source, installed, recipe):
    manifest = json.loads((recipe / 'recipe.json').read_text(encoding='utf-8'))
    version = (source / 'VERSION').read_text(encoding='utf-8').strip()
    if version != manifest['mesaVersion']:
        raise ValueError('Cannot relabel a different Mesa release')
    for name in manifest['runtimeRequiredPaths']:
        if not (installed / name).is_file():
            raise ValueError('Incomplete graphics cohort: ' + name)
    for name, target in manifest['runtimeAliases'].items():
        path = installed / name
        if not path.is_symlink() or os.readlink(path) != target:
            raise ValueError('Incorrect runtime alias: ' + name)
    inventory = {}
    for path in sorted(installed.rglob('*')):
        relative = path.relative_to(installed).as_posix()
        # The runtime archive ships lib/ and share/, not SDK headers/tools.
        if relative.split('/', 1)[0] not in ('lib', 'share'):
            continue
        if relative in ('lib/virtio-dxg-build.json', 'lib/virtio-dxg-mesa-version'):
            continue
        if path.is_symlink():
            if not path.resolve(strict=True).is_relative_to(installed):
                raise ValueError('Installed link leaves the cohort: ' + relative)
            inventory[relative] = {'symlink': os.readlink(path)}
        elif path.is_file():
            inventory[relative] = {'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                                   'bytes': path.stat().st_size}
    receipt = {'schema': 1, 'mesaVersion': version,
               'recipeSha256': hashlib.sha256((recipe / 'recipe.json').read_bytes()).hexdigest(),
               'sourceArchiveSha256': manifest['sourceArchiveSha256'],
               'directXHeadersCommit': manifest['directXHeadersCommit'],
               'profile': 'virtio-dxg', 'files': inventory,
               'graphicsLibraryLoaded': False, 'hardwareIntegrationTested': False,
               'dailyUseAccepted': False}
    (installed / 'lib/virtio-dxg-build.json').write_text(
        json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    (installed / 'lib/virtio-dxg-mesa-version').write_text(version + '\n', encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('installed', type=Path)
    parser.add_argument('recipe', type=Path)
    args = parser.parse_args()
    record(args.source.resolve(strict=True), args.installed.resolve(strict=True),
           args.recipe.resolve(strict=True))
