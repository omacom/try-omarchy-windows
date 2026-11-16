"""Stage pinned Mesa sources as data; never build or load graphics libraries."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import sys
import tarfile

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[2] / 'experimental/windows-native-gpu/mesa'))
from apply_mesa_recipe import apply_recipe


def stage(archive, destination, recipe):
    manifest = json.loads((recipe / 'recipe.json').read_text(encoding='utf-8'))
    if archive.stat().st_size > 128 * 1024 * 1024:
        raise ValueError('Source archive exceeds 128 MiB')
    if hashlib.sha256(archive.read_bytes()).hexdigest() != manifest['sourceArchiveSha256']:
        raise ValueError('Source archive hash differs')
    destination.mkdir(parents=True, exist_ok=False)
    prefix = 'mesa-' + manifest['mesaVersion']
    with tarfile.open(archive, 'r:xz') as source:
        members = source.getmembers()
        if len(members) > 100000 or sum(m.size for m in members) > 1024 ** 3:
            raise ValueError('Expanded source exceeds limits')
        for member in members:
            parts = PurePosixPath(member.name).parts
            if (not parts or parts[0] != prefix or '\\' in member.name or
                    ':' in member.name or '..' in parts or member.size > 64 * 1024 ** 2 or
                    not (member.isfile() or member.isdir() or member.issym() or member.islnk())):
                raise ValueError('Unsupported source member: ' + member.name)
        source.extractall(destination, members=members, filter='data')
    root = destination / prefix
    if (root / 'VERSION').read_text(encoding='utf-8').strip() != manifest['mesaVersion']:
        raise ValueError('Source VERSION differs')
    apply_recipe(root, recipe, 'graphics')
    return root


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('destination', type=Path)
    parser.add_argument('--recipe', type=Path, default=HERE / 'versions/26.2.6')
    args = parser.parse_args()
    print(stage(args.archive.resolve(strict=True), args.destination.resolve(),
                args.recipe.resolve(strict=True)))


if __name__ == '__main__':
    main()
