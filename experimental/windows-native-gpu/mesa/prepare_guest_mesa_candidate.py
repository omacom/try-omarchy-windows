"""Build a private normal-root Mesa candidate as archive data; never boot it."""
import argparse
import copy
import gzip
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import posixpath
import re
import stat
import tarfile

from guest_elf_abi import dependency_preflight, inspect, require


MESA = 'normal-root-overlay/mesa/lib'
SEARCH = (MESA, 'normal-root-overlay/wsl-libs', 'normal-root-overlay/lib')
MAX_RAW = 384 * 1024 * 1024
MAX_COMPONENT = 128 * 1024 * 1024
MAX_CAPTURE = 128 * 1024 * 1024
MANIFESTS = ('fence-fd-patch.json', 'native-sync-patch.json', 'native-image-patch.json',
             'native-render-patch.json', 'native-presentation-patch.json',
             'upload-failure-patch.json', 'constbuf-failure-patch.json')


def digest(data):
    return hashlib.sha256(data).hexdigest()


def file_digest(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def name_check(name):
    require(isinstance(name, str) and name and '\\' not in name and ':' not in name and '\0' not in name,
            'Invalid archive name')
    require(not name.startswith('/') and all(p not in ('', '.', '..') for p in name.split('/')) and
            PurePosixPath(name).as_posix() == name, 'Noncanonical archive name: ' + name)
    return name


def load_components(path, expected_hash, expected_head):
    require(path.stat().st_size <= 64 * 1024 * 1024 and file_digest(path) == expected_hash,
            'Component archive hash/size differs')
    with gzip.open(path, 'rb') as stream:
        raw = stream.read(MAX_COMPONENT + 1)
    require(len(raw) <= MAX_COMPONENT, 'Expanded component archive exceeds bound')
    members, payloads, links = {}, {}, {}
    with tarfile.open(fileobj=io.BytesIO(raw), mode='r:') as archive:
        for index, member in enumerate(archive):
            require(index < 4096, 'Too many component members')
            name = member.name.removeprefix('./')
            if member.isdir():
                name = name.rstrip('/')
            if name in ('', '.') and member.isdir():
                continue
            name_check(name)
            require(name not in members, 'Duplicate component member: ' + name)
            members[name] = member
            if member.isfile():
                require(member.size <= 64 * 1024 * 1024, 'Component member exceeds bound')
                payloads[name] = archive.extractfile(member).read()
                require(len(payloads[name]) == member.size, 'Truncated component file')
            elif member.issym():
                require(member.linkname and not member.linkname.startswith('/') and
                        '\\' not in member.linkname and ':' not in member.linkname, 'Invalid component symlink')
                links[name] = member.linkname
            else:
                require(member.isdir(), 'Unsupported component member type')

    def resolve(name):
        seen = set()
        while name in links:
            require(name not in seen, 'Cyclic component symlink')
            seen.add(name)
            name = posixpath.normpath(posixpath.join(posixpath.dirname(name), links[name]))
            name_check(name)
            require(name.startswith('root/'), 'Component library symlink escapes root')
        require(name in payloads, 'Missing component library target: ' + name)
        return name

    receipt = json.loads(payloads['BUILD-RECEIPT.json'])
    here = Path(__file__).resolve().parent
    contract = json.loads((here / 'stage-contract.json').read_text(encoding='utf-8'))
    require(receipt['schema'] == 2 and receipt['headCommit'] == expected_head and
            receipt['mesaVersion'] == contract['mesaVersion'], 'Wrong component receipt/head')
    require(all(receipt.get(n) is False for n in ('runtimeLoaded', 'gpuWorkloadRun', 'vmLaunched')) and
            receipt.get('libraryLayoutVerified') is True and receipt.get('binaryInterfaceComparisonPassed') is True,
            'Wrong component verification scope')
    expected_sources = {}
    for name in MANIFESTS:
        record = json.loads((here / name).read_text(encoding='utf-8'))
        expected_sources.update({n: r['patchedSha256'] for n, r in record['files'].items()})
    require(receipt['patchedSourceSha256'] == expected_sources, 'Component does not contain the current guarded recipe')
    require(set(receipt['stagedLibraries']) == set(contract['libraries']), 'Wrong component library set')
    replacements, roots, records = {}, [], {}
    for relative, expected in contract['libraries'].items():
        name = 'root/' + relative
        require(name in payloads and name not in links, 'Canonical component library is not regular')
        data = payloads[name]
        actual = inspect(data, relative)
        require(actual['sha256'] == receipt['stagedLibraries'][relative]['sha256'] and
                actual['soname'] == expected['soname'] and not actual['runpaths'] and
                actual['apiExportCount'] == expected['apiExportCount'] and
                actual['apiExportAbiSha256'] == expected['apiExportAbiSha256'], 'Component library differs: ' + relative)
        target = MESA + '/' + relative.removeprefix('usr/lib/')
        replacements[target] = data
        roots.append(target)
        records[target] = actual
    for relative, target in contract['libraryAliases'].items():
        name = 'root/' + relative
        require(name in links and links[name] == receipt['stagedLibraryAliases'][relative] and
                resolve(name) == 'root/' + target, 'Wrong component loader alias: ' + relative)
        replacements[MESA + '/' + relative.removeprefix('usr/lib/')] = payloads[resolve(name)]
    return replacements, roots, records, receipt


def scan_image(path, manifest, capture=False, replacements=None, sink=None, expected_headers=None):
    """Stream one bounded newc/gzip image; never extract paths onto the host."""
    replacements = replacements or {}
    metadata, blobs, headers, order = {}, {}, {}, []
    expanded, captured = 0, 0
    with gzip.open(path, 'rb') as source:
        def read(count):
            nonlocal expanded
            require(0 <= count <= MAX_RAW - expanded, 'Guest image exceeds expanded bound')
            data = source.read(count)
            require(len(data) == count, 'Truncated guest image')
            expanded += count
            return data

        while True:
            require(len(headers) < 8192, 'Too many guest image members')
            header = read(110)
            require(header[:6] == b'070701' and re.fullmatch(rb'[0-9a-fA-F]{104}', header[6:]), 'Invalid newc header')
            fields = [int(header[6 + i * 8:14 + i * 8], 16) for i in range(13)]
            require(0 < fields[11] <= 4096 and fields[2:4] == [0, 0] and fields[12] == 0, 'Unsupported guest header')
            encoded = read(fields[11])
            require(encoded.endswith(b'\0') and b'\0' not in encoded[:-1], 'Invalid guest entry name')
            name = encoded[:-1].decode('utf-8')
            padding = read(-(110 + fields[11]) % 4)
            require(not any(padding) and name not in headers, 'Invalid padding or duplicate guest entry')
            if name != 'TRAILER!!!':
                name_check(name)
            require(fields[6] <= MAX_RAW and (not stat.S_ISDIR(fields[1]) or fields[6] == 0), 'Invalid guest payload size')
            if expected_headers is not None:
                require(name in expected_headers and fields == expected_headers[name], 'Guest header changed: ' + name)
            headers[name] = fields
            order.append(name)
            replacement = replacements.get(name)
            updated = list(fields)
            if replacement is not None:
                require(stat.S_ISREG(fields[1]) and fields[4] == 1, 'Mesa target is not an independent regular file')
                updated[6] = len(replacement)
            if sink is not None:
                sink.write(b'070701' + ''.join(f'{v:08x}' for v in updated).encode() + encoded + padding)
            wanted = capture and (name.startswith(MESA + '/') or
                (any(name.startswith(p + '/') for p in SEARCH[1:]) and fields[6] <= 16 * 1024 * 1024)) and stat.S_ISREG(fields[1])
            if wanted:
                require(fields[6] <= MAX_CAPTURE - captured, 'Captured dependency bytes exceed bound')
                captured += fields[6]
            chunks, hashed, remaining = [], hashlib.sha256(), fields[6]
            while remaining:
                block = read(min(remaining, 1024 * 1024))
                hashed.update(block)
                if wanted:
                    chunks.append(block)
                if sink is not None and replacement is None:
                    sink.write(block)
                remaining -= len(block)
            trailer_padding = read(-fields[6] % 4)
            require(not any(trailer_padding), 'Invalid guest payload padding')
            if wanted:
                blobs[name] = b''.join(chunks)
            if sink is not None:
                if replacement is not None:
                    sink.write(replacement)
                sink.write(b'\0' * (-updated[6] % 4))
            if name == 'TRAILER!!!':
                require(fields[6] == 0 and not source.read(1), 'Data follows guest trailer')
                break
            if stat.S_ISDIR(fields[1]):
                require(name not in manifest['files'], 'Directory in payload manifest')
                continue
            require(name in manifest['files'], 'Unmanifested guest payload: ' + name)
            expected = manifest['files'][name]
            require(fields[6] == expected['bytes'] and fields[1] == expected['mode'] and
                    hashed.hexdigest() == expected['sha256'], 'Guest payload differs from manifest: ' + name)
            metadata[name] = dict(mode=fields[1], bytes=fields[6], sha256=hashed.hexdigest())
    require(set(metadata) == set(manifest['files']), 'Manifest payload missing from guest image')
    return dict(metadata=metadata, blobs=blobs, headers=headers, order=order, expandedBytes=expanded)


def prepare(base_image, base_manifest, components, component_sha256, component_head, output):
    require(re.fullmatch(r'[0-9a-f]{64}', component_sha256) and re.fullmatch(r'[0-9a-f]{40}', component_head), 'Invalid component pin')
    require(base_image.stat().st_size <= 256 * 1024 * 1024 and base_manifest.stat().st_size <= 2 * 1024 * 1024, 'Base input exceeds bound')
    original = json.loads(base_manifest.read_text(encoding='utf-8'))
    require(original.get('privateImage') is True and original.get('redistributionApproved') is False and
            original.get('rootDiskWritesAllowed') is False and original.get('runtimeBoundReadonly') is True,
            'Requires the private read-only normal-root profile')
    base_hash = file_digest(base_image)
    require(base_hash == original['initramfsSha256'], 'Base image hash differs')
    replacements, roots, records, component_receipt = load_components(components, component_sha256, component_head)
    before = scan_image(base_image, original, capture=True)
    require(set(replacements).issubset(before['blobs']), 'Mesa replacement target missing or nonregular')
    for name in roots:
        old = inspect(before['blobs'][name], name)
        require(old['apiExportAbiSha256'] == records[name]['apiExportAbiSha256'] and
                old['apiExportCount'] == records[name]['apiExportCount'] and old['soname'] == records[name]['soname'],
                'Captured Mesa API differs: ' + name)
    contract = json.loads((Path(__file__).parent / 'stage-contract.json').read_text(encoding='utf-8'))
    for alias, target in contract['libraryAliases'].items():
        old_alias, old_target = (MESA + '/' + p.removeprefix('usr/lib/') for p in (alias, target))
        require(digest(before['blobs'][old_alias]) == digest(before['blobs'][old_target]), 'Captured loader alias differs: ' + old_alias)
    providers = dict(before['blobs'])
    providers.update(replacements)
    preflight = dependency_preflight(roots, providers, SEARCH)
    delta = sum(len(data) - original['files'][name]['bytes'] for name, data in replacements.items())
    require(isinstance(original['runtimeBytes'], int) and not isinstance(original['runtimeBytes'], bool) and
            0 < original['runtimeBytes'] + delta <= MAX_RAW, 'Candidate runtime exceeds bound')
    require(not output.exists(), 'Use a fresh candidate directory')
    output.mkdir(parents=True)
    partial = output / 'private-normal-session.cpio.gz.partial'
    image = output / 'private-normal-session.cpio.gz'
    with partial.open('xb') as file, gzip.GzipFile(filename='', fileobj=file, mode='wb', compresslevel=1, mtime=0) as stream:
        copied = scan_image(base_image, original, replacements=replacements, sink=stream)
    require(copied['headers'] == before['headers'] and copied['order'] == before['order'] and file_digest(base_image) == base_hash,
            'Base image changed during preparation')
    manifest, headers = copy.deepcopy(original), copy.deepcopy(before['headers'])
    changes = {}
    for name, data in replacements.items():
        prior = manifest['files'][name]
        changes[name] = dict(before=prior['sha256'], after=digest(data))
        prior.update(bytes=len(data), sha256=digest(data), origin=dict(componentHead=component_head, componentArchiveSha256=component_sha256))
        headers[name][6] = len(data)
    checked = scan_image(partial, manifest, expected_headers=headers)
    require(checked['order'] == before['order'], 'Candidate member order changed')
    partial.rename(image)
    manifest.update(initramfsSha256=file_digest(image), initramfsBytes=image.stat().st_size,
                    runtimeBytes=original['runtimeBytes'] + delta, parentImageSha256=base_hash,
                    guestBooted=False, normalSessionVerified=False, gpuFrameVerified=False,
                    appEnvironmentIntegrationVerified=False, dailyUseAccepted=False,
                    purpose='Private normal-root candidate with guarded Mesa; never booted',
                    mesaComponentHead=component_head, mesaComponentSourceCommit=component_receipt['sourceCommit'],
                    mesaComponentPatchedSourceSha256=component_receipt['patchedSourceSha256'],
                    builderSha256=file_digest(Path(__file__)),
                    mesaCandidateBuilderSourceSha256={n: file_digest(Path(__file__).parent / n) for n in
                        ('prepare_guest_mesa_candidate.py', 'guest_elf_abi.py', 'stage-contract.json')})
    receipt = dict(schema=1, privateImage=True, redistributionApproved=False, baseImageSha256=base_hash,
                   candidateImageSha256=manifest['initramfsSha256'], componentArchiveSha256=component_sha256,
                   componentHead=component_head, changedPayloads=changes, replacementCount=len(changes),
                   allOtherPayloadsAndHeadersExact=True, inputConfigurationPayloadsModified=False,
                   staticDependencyPreflight=preflight, vmLaunched=False, librariesLoaded=False,
                   hardwareIntegrationTested=False, normalDesktopVerified=False, dailyUseAccepted=False)
    for name, record in (('manifest.json', manifest), ('candidate-receipt.json', receipt)):
        with (output / name).open('x', encoding='utf-8', newline='\n') as stream:
            json.dump(record, stream, indent=2)
            stream.write('\n')
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('base-image', 'base-manifest', 'components', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--component-sha256', required=True)
    parser.add_argument('--component-head', required=True)
    args = parser.parse_args()
    result = prepare(args.base_image, args.base_manifest, args.components,
                     args.component_sha256, args.component_head, args.output)
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
