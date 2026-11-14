"""Data-only candidate controls using compiled Mesa and captured CI ELF bytes."""
import argparse
from collections import deque
import gzip
import io
import json
import os
from pathlib import Path
import stat
import tarfile

from guest_elf_abi import inspect, require
from prepare_guest_mesa_candidate import MESA, digest, file_digest, load_components, prepare


INPUT = 'normal-session-overlay/official-input-fixture'
PRIVATE = 'normal-root-overlay/wsl-drivers/private-fixture.so'


def base_fixture(folder, files, duplicate=None):
    folder.mkdir()
    raw = bytearray()
    sequence = 0

    def entry(name, mode, data):
        nonlocal sequence
        sequence += 1
        encoded = name.encode() + b'\0'
        fields = [sequence, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(encoded), 0]
        raw.extend(b'070701' + ''.join(f'{v:08x}' for v in fields).encode() + encoded)
        raw.extend(b'\0' * (-len(raw) % 4))
        raw.extend(data)
        raw.extend(b'\0' * (-len(raw) % 4))

    directories = {str(p) for name in files for p in Path(name).parents if str(p) != '.'}
    for name in sorted(directories, key=lambda n: (n.count('/'), n)):
        entry(name, stat.S_IFDIR | 0o755, b'')
    for name, (mode, data) in files.items():
        entry(name, mode, data)
    if duplicate:
        entry(duplicate, *files[duplicate])
    entry('TRAILER!!!', 0, b'')
    image = folder / 'base.cpio.gz'
    image.write_bytes(gzip.compress(bytes(raw), compresslevel=1, mtime=0))
    manifest = dict(schema=1, privateImage=True, redistributionApproved=False,
                    rootDiskWritesAllowed=False, runtimeBoundReadonly=True,
                    runtimeBytes=sum(len(v[1]) for v in files.values()),
                    initramfsSha256=file_digest(image), files={n: dict(mode=m, bytes=len(d), sha256=digest(d))
                        for n, (m, d) in files.items()})
    path = folder / 'manifest.json'
    path.write_text(json.dumps(manifest), encoding='utf-8', newline='\n')
    return image, path


def change_tar(source, output, kind):
    with tarfile.open(source, 'r:gz') as old, tarfile.open(output, 'w:gz') as new:
        for member in old:
            data = old.extractfile(member).read() if member.isfile() else None
            name = member.name.removeprefix('./')
            if kind == 'alias' and name == 'root/usr/lib/libEGL.so':
                member.linkname = 'libgbm.so.1'
            if kind == 'bytes' and name == 'root/usr/lib/libEGL.so.1.0.0':
                changed = bytearray(data)
                changed[15] ^= 1  # ELF identification padding; parsing stays valid.
                data = bytes(changed)
            new.addfile(member, io.BytesIO(data) if data is not None else None)
        if kind == 'escape':
            member = tarfile.TarInfo('../../escape')
            member.size = 1
            new.addfile(member, io.BytesIO(b'x'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--components', type=Path, required=True)
    parser.add_argument('--component-head', required=True)
    parser.add_argument('--provider-directory', type=Path, action='append', default=[])
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    require(os.environ.get('GITHUB_ACTIONS') == 'true' and os.environ.get('RUNNER_OS') == 'Linux',
            'This fixture requires the Linux GitHub CPU runner')
    require(not args.output.exists(), 'Use a fresh control directory')
    args.output.mkdir(parents=True)
    sha = file_digest(args.components)
    replacements, roots, records, _ = load_components(args.components, sha, args.component_head)
    files = {n: (stat.S_IFREG | 0o755, data) for n, data in replacements.items()}
    files.update({INPUT: (stat.S_IFREG | 0o644, b'official-input-fixture-unchanged\n'),
                  PRIVATE: (stat.S_IFREG | 0o644, b'private-payload-placeholder-never-executed\n')})
    by_name = {Path(n).name: n for n in replacements}
    directories = args.provider_directory + [Path('/lib/x86_64-linux-gnu'), Path('/usr/lib/x86_64-linux-gnu')]
    queue = deque(n for r in records.values() for n in r['needed'])
    while queue:
        name = queue.popleft()
        if name in by_name:
            continue
        provider = next((p / name for p in directories if (p / name).is_file()), None)
        require(provider is not None, 'CI fixture dependency missing: ' + name)
        data = provider.read_bytes()
        require(len(data) <= 16 * 1024 * 1024, 'CI provider exceeds candidate inspection bound')
        path = 'normal-root-overlay/lib/' + name
        files[path] = (stat.S_IFREG | 0o755, data)
        by_name[name] = path
        queue.extend(inspect(data, name)['needed'])
    cases = []
    image, manifest = base_fixture(args.output / 'valid-base', files)
    result = prepare(image, manifest, args.components, sha, args.component_head, args.output / 'valid-candidate')
    require(result['replacementCount'] == 11 and result['staticDependencyPreflight']['strongImportsChecked'] > 0,
            'Expected the complete Mesa group and real import checks')
    updated_manifest = json.loads((args.output / 'valid-candidate/manifest.json').read_text(encoding='utf-8'))
    require(all(updated_manifest['files'][name]['sha256'] == digest(files[name][1]) for name in (INPUT, PRIVATE)),
            'Candidate changed unrelated input/private payloads')
    cases.append(dict(case='real-elf-candidate', accepted=True))

    def reject(name, changed=None, component=None, head=None, duplicate=None, expected=None, bad_hash=False):
        base = files if changed is None else changed
        source, metadata = base_fixture(args.output / (name + '-base'), base, duplicate)
        if bad_hash:
            document = json.loads(metadata.read_text(encoding='utf-8'))
            document['initramfsSha256'] = '0' * 64
            metadata.write_text(json.dumps(document), encoding='utf-8')
        component = args.components if component is None else component
        try:
            prepare(source, metadata, component, file_digest(component),
                    args.component_head if head is None else head, args.output / (name + '-candidate'))
        except ValueError as error:
            require(expected is None or expected in str(error), 'Unexpected rejection reason for ' + name + ': ' + str(error))
            cases.append(dict(case=name, accepted=False, reason=str(error)))
        else:
            raise RuntimeError('Unsafe candidate accepted: ' + name)

    reject('base-hash', bad_hash=True, expected='Base image hash differs')
    reject('component-head', head='0' * 40, expected='Wrong component receipt/head')
    changed = dict(files)
    del changed[MESA + '/libEGL.so']
    reject('missing-alias', changed, expected='Mesa replacement target missing')
    changed = dict(files)
    changed[MESA + '/libEGL.so'] = (stat.S_IFLNK | 0o777, b'libEGL.so.1')
    reject('symlink-target', changed, expected='Mesa replacement target missing')
    changed = dict(files)
    changed[MESA + '/libEGL.so'] = changed[MESA + '/libgbm.so.1.0.0']
    reject('old-alias-wrong-library', changed, expected='Captured loader alias differs')
    changed = dict(files)
    name = MESA + '/libEGL.so.1.0.0'
    require(b'eglGetDisplay\0' in changed[name][1], 'ELF ABI control cannot be constructed')
    changed[name] = (changed[name][0], changed[name][1].replace(b'eglGetDisplay\0', b'eglGetDisplaz\0'))
    reject('old-abi', changed, expected='Captured Mesa API differs')
    changed = dict(files)
    del changed['normal-root-overlay/lib/libzstd.so.1']
    reject('missing-provider', changed, expected='No captured provider')
    for name, old, new, message in (
        ('provider-symbol', b'malloc\0', b'xalloc\0', 'Strong import unavailable'),
        ('provider-version', b'GLIBC_2.2.5\0', b'GLIBC_9.2.5\0', 'Required provider version absent')):
        changed = dict(files)
        path = 'normal-root-overlay/lib/libc.so.6'
        require(old in changed[path][1] and len(old) == len(new), 'Provider control cannot be constructed')
        changed[path] = (changed[path][0], changed[path][1].replace(old, new))
        reject(name, changed, expected=message)
    reject('duplicate-cpio', duplicate=INPUT, expected='duplicate guest entry')
    for name, kind, message in (('component-alias', 'alias', 'Wrong component loader alias'),
                                ('component-bytes', 'bytes', 'Component library differs'),
                                ('component-escape', 'escape', 'Noncanonical archive name')):
        path = args.output / (name + '.tar.gz')
        change_tar(args.components, path, kind)
        reject(name, component=path, expected=message)
    require(file_digest(image) == json.loads(manifest.read_text(encoding='utf-8'))['initramfsSha256'], 'Controls changed the valid base')
    report = dict(schema=1, componentHead=args.component_head, cases=cases, passed=True,
                  validCandidate=result, privatePayloadsUsed=False, guestElfExecuted=False,
                  vmLaunched=False, gpuWorkloadRun=False, normalDesktopVerified=False, dailyUseAccepted=False)
    (args.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8', newline='\n')
    print(f'GUEST_MESA_CANDIDATE_CONTROLS_PASS cases={len(cases)} guestElfExecuted=false gpu=false')


if __name__ == '__main__':
    main()
