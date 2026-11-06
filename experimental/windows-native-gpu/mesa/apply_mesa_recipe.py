"""Apply a versioned, hash-pinned source recipe. Never build or load Mesa."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import subprocess


def check(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inside(root, relative):
    check(isinstance(relative, str) and relative, 'Empty recipe path')
    parts = PurePosixPath(relative).parts
    check(not relative.startswith('/') and '\\' not in relative and ':' not in relative and
          '/'.join(parts) == relative and
          not any(p in ('.', '..', '.git') for p in parts), 'Noncanonical recipe path')
    path = root / relative
    check(path.resolve().is_relative_to(root), 'Recipe path escapes root')
    # Source aliases are not legitimate patch targets, including aliases of
    # an existing directory inside the tree.
    check(not any(p.is_symlink() for p in [path, *path.parents] if p != root.parent),
          'Symlink in recipe path')
    check(not path.exists() or path.is_file(), 'Recipe target is not a regular file')
    return path


def load_recipe(recipe, profile):
    recipe = recipe.resolve(strict=True)
    manifest = json.loads((recipe / 'recipe.json').read_text(encoding='utf-8'))
    check(manifest.get('schema') == 1 and profile in ('graphics', 'dozen'),
          'Unsupported recipe or profile')
    check(re.fullmatch(r'\d+\.\d+\.\d+', manifest.get('mesaVersion', '')),
          'Invalid pinned version')
    stage = manifest['profiles'][profile]
    patch = inside(recipe, stage['patchFile'])
    check(digest(patch) == stage['patchSha256'], 'Recipe patch hash differs')
    check(stage['files'], 'Empty patch file contract')
    for record in stage['files'].values():
        for field in ('originalSha256', 'patchedSha256'):
            value = record[field]
            check(value is None or re.fullmatch(r'[0-9a-f]{64}', value),
                  'Invalid source digest')
    return manifest, stage, patch


def verify_patched(source, recipe, profile):
    source = source.resolve(strict=True)
    manifest, stage, _ = load_recipe(recipe, profile)
    check((source / 'VERSION').read_text(encoding='utf-8').strip() == manifest['mesaVersion'],
          'Wrong Mesa source version')
    observed = {}
    for name, record in stage['files'].items():
        path = inside(source, name)
        actual = digest(path) if path.is_file() else None
        check(actual == record['patchedSha256'], 'Patched source differs: ' + name)
        observed[name] = actual
    return manifest, observed


def apply_recipe(source, recipe, profile):
    source = source.resolve(strict=True)
    manifest, stage, patch = load_recipe(recipe, profile)
    check((source / 'VERSION').read_text(encoding='utf-8').strip() == manifest['mesaVersion'],
          'Wrong Mesa source version')
    observed = {}
    for name in stage['files']:
        path = inside(source, name)
        observed[name] = digest(path) if path.is_file() else None
    if all(observed[n] == r['patchedSha256'] for n, r in stage['files'].items()):
        return False
    check(all(observed[n] == r['originalSha256'] for n, r in stage['files'].items()),
          'Source differs from the pristine pinned profile; no files changed')
    command = ['patch', '--batch', '--forward', '--fuzz=0', '-p1', '-i', str(patch)]
    subprocess.run(command + ['--dry-run'], cwd=source, check=True, timeout=30)
    subprocess.run(command, cwd=source, check=True, timeout=30)
    verify_patched(source, recipe, profile)
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('--recipe', required=True, type=Path)
    parser.add_argument('--profile', required=True, choices=('graphics', 'dozen'))
    args = parser.parse_args()
    applied = apply_recipe(args.source, args.recipe, args.profile)
    print('Pinned Mesa profile ' + ('applied and verified' if applied else 'already verified'))


if __name__ == '__main__':
    main()
