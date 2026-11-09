"""Prepare boot-local graphics data. Never import or execute a graphics library."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import tarfile
import tempfile

LIMIT = 512 * 1024 ** 2


def canonical(name):
    path = PurePosixPath(name)
    if (not name or not path.parts or path.is_absolute() or '\\' in name or ':' in name or
            '..' in path.parts or path.as_posix() != name.rstrip('/') or
            path.parts[0] not in ('lib', 'share')):
        raise ValueError('Unsupported payload path: ' + name)
    return path.as_posix()


def extract(archive, destination, recipe):
    """Reject a partial/mixed cohort before it can select session libraries."""
    expected = json.loads(recipe.read_text(encoding='utf-8'))
    if destination.exists() or archive.stat().st_size > LIMIT:
        raise ValueError('Destination exists or archive exceeds limit')
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive, 'r:') as tar:
        entries = tar.getmembers()
        if len(entries) > 10000 or sum(m.size for m in entries) > LIMIT:
            raise ValueError('Expanded payload exceeds limit')
        members = {}
        for member in entries:
            name = canonical(member.name)
            if name in members or not (member.isdir() or member.isfile() or member.issym()):
                raise ValueError('Duplicate or unsupported member: ' + name)
            if member.size < 0 or member.size > 256 * 1024 ** 2:
                raise ValueError('Member exceeds limit: ' + name)
            members[name] = member
        receipt_member = members.get('lib/virtio-dxg-build.json')
        if not receipt_member or not receipt_member.isfile() or receipt_member.size > 4 * 1024 ** 2:
            raise ValueError('Missing bounded build receipt')
        receipt = json.load(tar.extractfile(receipt_member))
        if (receipt.get('schema') != 1 or receipt.get('profile') != 'virtio-dxg' or
                receipt.get('mesaVersion') != expected['mesaVersion'] or
                receipt.get('recipeSha256') != hashlib.sha256(recipe.read_bytes()).hexdigest() or
                receipt.get('sourceArchiveSha256') != expected['sourceArchiveSha256'] or
                receipt.get('directXHeadersCommit') != expected['directXHeadersCommit']):
            raise ValueError('Payload and initramfs recipes differ; rebuild the matched pair')
        inventory = receipt['files']
        names = {n for n, m in members.items() if not m.isdir()}
        if names != set(inventory) | {'lib/virtio-dxg-build.json', 'lib/virtio-dxg-mesa-version'}:
            raise ValueError('Payload inventory differs')
        for name, record in inventory.items():
            canonical(name)
            member = members[name]
            if 'symlink' in record:
                if not member.issym() or member.linkname != record['symlink']:
                    raise ValueError('Changed library alias: ' + name)
            elif (not member.isfile() or member.size != record['bytes'] or
                  hashlib.sha256(tar.extractfile(member).read()).hexdigest() != record['sha256']):
                raise ValueError('Changed component: ' + name)
        version_member = members['lib/virtio-dxg-mesa-version']
        if not version_member.isfile() or version_member.size > 64:
            raise ValueError('Invalid payload version label')
        version = tar.extractfile(version_member).read().decode().strip()
        if version != expected['mesaVersion']:
            raise ValueError('Payload version label differs')
        with tempfile.TemporaryDirectory(prefix='.virtio-dxg-mesa-', dir=destination.parent) as temporary:
            stage = Path(temporary)
            tar.extractall(stage, members=entries, filter='data')
            for name in inventory:
                path = stage / name
                if not path.resolve(strict=True).is_relative_to(stage):
                    raise ValueError('Alias leaves the payload: ' + name)
            for name in (f'libgallium-{version}.so', 'libEGL_mesa.so.0', 'libGLX_mesa.so.0',
                         'libgbm.so.1', 'libvulkan_dzn.so'):
                if not (stage / 'lib' / name).is_file():
                    raise ValueError('Incomplete graphics cohort: ' + name)
            icd = stage / 'share/vulkan/icd.d/dzn_icd.x86_64.json'
            data = json.loads(icd.read_text(encoding='utf-8'))
            data['ICD']['library_path'] = str(destination / 'lib/libvulkan_dzn.so')
            icd.write_text(json.dumps(data, indent=2) + '\n', encoding='utf-8')
            os.rename(stage, destination)
            # TemporaryDirectory tolerates its renamed source disappearing.
    return version


def desktop_entry(text):
    """Preserve URLs, field codes, actions and localization in the installed entry."""
    pattern = r'(?m)^Exec=(?:/usr/bin/chromium|chromium|"/usr/bin/chromium")(?=\s|$)'
    result, count = re.subn(pattern, 'Exec=/run/virtio-dxg/bin/chromium', text)
    if not count:
        raise ValueError('Chromium desktop entry has no recognized launcher')
    return re.sub(r'(?m)^TryExec=(?:/usr/bin/chromium|chromium)\s*$',
                  'TryExec=/run/virtio-dxg/bin/chromium', result)


def prepare_browser(root, source):
    """Create data only; systemd mounts the read-only boot-local overlays later."""
    desktop = root / 'usr/share/applications/chromium.desktop'
    if not (root / 'usr/bin/chromium').is_file() or not desktop.is_file():
        return False
    changed = desktop_entry(desktop.read_text(encoding='utf-8'))
    boot = root / 'run/virtio-dxg'
    (boot / 'bin').mkdir(parents=True, exist_ok=True)
    (boot / 'applications').mkdir(exist_ok=True)
    shutil.copyfile(source, boot / 'bin/chromium')
    (boot / 'bin/chromium').chmod(0o755)
    (boot / 'applications/chromium.desktop').write_text(changed, encoding='utf-8')
    unit = root / 'run/systemd/system/virtio-dxg-bin.service'
    unit.parent.mkdir(parents=True, exist_ok=True)
    unit.write_text('''[Unit]
Description=virtio-dxg Chromium launch paths for this boot
DefaultDependencies=no
After=local-fs.target
Before=sysinit.target systemd-user-sessions.service

[Service]
Type=oneshot
ExecStart=/usr/bin/mount -t overlay overlay -o ro,lowerdir=/run/virtio-dxg/bin:/usr/local/bin /usr/local/bin
ExecStart=/usr/bin/mount -t overlay overlay -o ro,lowerdir=/run/virtio-dxg/applications:/usr/share/applications /usr/share/applications
ExecStop=/usr/bin/umount /usr/share/applications
ExecStop=/usr/bin/umount /usr/local/bin
RemainAfterExit=yes
''', encoding='utf-8')
    wants = unit.parent / 'sysinit.target.wants'
    wants.mkdir(exist_ok=True)
    (wants / unit.name).symlink_to('../' + unit.name)
    return True


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('destination', type=Path)
    parser.add_argument('--recipe', required=True, type=Path)
    parser.add_argument('--browser-wrapper', type=Path)
    args = parser.parse_args()
    version = extract(args.archive, args.destination, args.recipe)
    if args.browser_wrapper:
        try:
            prepare_browser(Path('/'), args.browser_wrapper)
        except (ValueError, OSError) as error:
            print('virtio-dxg: Chromium wrapper unavailable: ' + str(error))
    print('virtio-dxg: staged matched Mesa ' + version + '; hardware acceptance pending')
