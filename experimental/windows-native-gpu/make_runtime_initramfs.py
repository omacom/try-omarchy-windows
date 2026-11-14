"""Pack installed Linux GPU libraries into a PRIVATE local QEMU diagnostic.

This image includes proprietary NVIDIA binaries. Never commit or upload it.
It does not install drivers, read a guest disk, or create a graphics desktop.
"""
import argparse
import gzip
import hashlib
import json
import os
import pathlib
import re
import stat
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('init', 'probe', 'shim', 'linux-umd', 'output'):
        parser.add_argument('--' + name, required=True, type=pathlib.Path)
    for name in ('d3d12', 'd3d12core', 'dxcore'):
        parser.add_argument('--' + name, type=pathlib.Path, default=pathlib.Path('/usr/lib/wsl/lib/lib' + name + '.so'))
    parser.add_argument('--strace', type=pathlib.Path, help='Optional locally supplied syscall tracer; failed calls only')
    parser.add_argument('--exit-lifetime-probe', type=pathlib.Path, help='Optional guest-only DSO finalizer diagnostic, exclusive to the OpenGL workload')
    parser.add_argument('--dependency-directory', type=pathlib.Path, action='append', default=[], help='Optional local ELF dependencies; mapped to the guest library directory')
    parser.add_argument('--extra-runtime', type=pathlib.Path, action='append', default=[], help='Installed library loaded dynamically; preserves its absolute guest path')
    parser.add_argument('--mesa-runtime-directory', type=pathlib.Path, help='Private non-GLVND Mesa install for the explicit OpenGL probe; relocated to /opt/wddm-mesa')
    parser.add_argument('--guest-aperture-module', type=pathlib.Path, help='Explicit guest-only wddm_aperture.ko built against the matching guest kernel')
    parser.add_argument('--guest-render-module', type=pathlib.Path, help='Opt-in matching guest-only wddm_render.ko; requires the native-buffer OpenGL broker route')
    parser.add_argument('--guest-aperture-contract', type=pathlib.Path, help='Static guest isolation probe, required with the aperture module')
    parser.add_argument('--guest-native-sync-files', action='store_true', help='Explicit guest-only native GPU sync-file export; requires aperture module')
    parser.add_argument('--guest-native-buffers', action='store_true', help='Explicit guest-only opaque native-resource DMA-BUF files; requires aperture module')
    parser.add_argument('--guest-broker-contract', type=pathlib.Path, help='Optional real-process broker peer contract executable')
    parser.add_argument('--guest-broker-finalizer', type=pathlib.Path, help='ELF finalizer DSO required with the broker peer contract')
    parser.add_argument('--guest-graphics-broker', type=pathlib.Path, help='Opt-in single-client privileged broker; render process runs as UID 65534')
    parser.add_argument('--runtime-workload', choices=('init', 'copy', 'clear', 'triangle', 'shared', 'consume', 'native', 'native-stream', 'opengl'), default='init', help='Explicit guest workload; OpenGL is a separate Mesa frontend probe')
    args = parser.parse_args()
    if sys.platform != 'linux':
        parser.error('Run in Linux/WSL with locally installed runtime libraries')
    if args.output.exists() or args.output.with_suffix('.manifest.json').exists():
        parser.error('Use a fresh output path; private runtime images are never overwritten')
    module = args.linux_umd.name
    if not re.fullmatch(r'[A-Za-z0-9._-]{1,128}', module):
        parser.error('Invalid Linux UMD module basename')
    files = {'init': args.init, 'guest-probe': args.probe, 'linux-ioctl-bridge.so': args.shim}
    if args.guest_native_sync_files and not args.guest_aperture_module:
        parser.error('Native sync-file export requires the guest aperture module')
    if args.guest_native_buffers and not args.guest_aperture_module:
        parser.error('Native buffers require the guest aperture module')
    if args.guest_render_module and (not args.guest_native_buffers or not args.guest_native_sync_files or
            not args.guest_graphics_broker or args.runtime_workload != 'opengl'):
        parser.error('The render node requires the native-buffer/native-sync OpenGL broker route')
    roots = [args.probe, args.shim, args.linux_umd, args.d3d12, args.d3d12core, args.dxcore]
    if args.guest_graphics_broker:
        if not args.guest_aperture_module or not args.guest_aperture_contract or args.runtime_workload not in ('init','copy','clear','triangle','native','opengl'):
            parser.error('The graphics broker requires the kernel aperture and a workload without native FD export')
        files['guest-graphics-broker'] = args.guest_graphics_broker
        roots.append(args.guest_graphics_broker)
    if bool(args.guest_broker_contract) != bool(args.guest_broker_finalizer):
        parser.error('The broker contract executable and finalizer DSO must be supplied together')
    if args.guest_broker_contract:
        files['broker-peer-contract'] = args.guest_broker_contract
        files['broker-finalizer-probe.so'] = args.guest_broker_finalizer
        roots.extend((args.guest_broker_contract, args.guest_broker_finalizer))
    if args.exit_lifetime_probe:
        if args.runtime_workload != 'opengl':
            parser.error('The exit-lifetime DSO requires the explicit OpenGL workload')
        files['runtime-exit-probe.so'] = args.exit_lifetime_probe
        roots.append(args.exit_lifetime_probe)
    if bool(args.guest_aperture_module) != bool(args.guest_aperture_contract):
        parser.error('The guest aperture module and contract probe must be supplied together')
    if args.guest_aperture_module:
        data = args.guest_aperture_module.read_bytes()
        if not 64 <= len(data) <= 16 * 1024 * 1024 or data[:6] != b'\x7fELF\x02\x01' or \
                data[16:20] != b'\x01\x00\x3e\x00' or b'name=wddm_aperture\0' not in data:
            parser.error('The aperture module must be a bounded x64 ELF relocatable named wddm_aperture')
        files['wddm-aperture.ko'] = args.guest_aperture_module
        files['aperture-contract'] = args.guest_aperture_contract
    if args.guest_render_module:
        data = args.guest_render_module.read_bytes()
        if not 64 <= len(data) <= 16 * 1024 * 1024 or data[:6] != b'\x7fELF\x02\x01' or \
                data[16:20] != b'\x01\x00\x3e\x00' or b'name=wddm_render\0' not in data:
            parser.error('The render module must be a bounded x64 ELF relocatable named wddm_render')
        release_pattern = rb'vermagic=([0-9][A-Za-z0-9._+-]{0,127}) '
        render_release = re.search(release_pattern, data)
        aperture_release = re.search(release_pattern, args.guest_aperture_module.read_bytes())
        if not render_release or not aperture_release or render_release.group(1) != aperture_release.group(1):
            parser.error('Render and aperture modules must match the same guest kernel release')
        files['wddm-render.ko'] = args.guest_render_module
    relocated = {}
    if (args.runtime_workload == 'opengl') != bool(args.mesa_runtime_directory):
        parser.error('The OpenGL workload requires --mesa-runtime-directory, exclusive to that workload')
    if args.mesa_runtime_directory:
        mesa = args.mesa_runtime_directory.resolve(strict=True)
        if not (mesa / 'lib/libEGL.so.1').is_file():
            parser.error('The private Mesa install must contain lib/libEGL.so.1')
        # Resolve local symlinks to bytes inside the private image, preserving
        # loader names. Never copy a host directory wholesale into the guest.
        mesa_files = (sorted((mesa / 'lib').glob('*.so*')) + sorted((mesa / 'lib/dri').glob('*.so')) +
                      sorted((mesa / 'lib/gbm').glob('*.so')))
        if not 1 <= len(mesa_files) <= 32:
            parser.error('Private Mesa library count is outside the diagnostic bound')
        for path in mesa_files:
            resolved = path.resolve(strict=True)
            if not resolved.is_relative_to(mesa) or not resolved.is_file():
                parser.error('Private Mesa libraries must resolve inside their install directory')
            name = 'opt/wddm-mesa/' + path.relative_to(mesa).as_posix()
            files[name] = path
            relocated[resolved] = name
        roots.extend(dict.fromkeys(p.resolve() for p in mesa_files))
    roots.extend(args.extra_runtime)
    if args.strace:
        files['runtime-tracer'] = args.strace
        roots.append(args.strace)
    for path in [args.init] + ([args.guest_aperture_contract] if args.guest_aperture_contract else []) + roots:
        data = path.read_bytes()
        if data[:6] != b'\x7fELF\x02\x01' or data[18:20] != b'\x3e\x00':
            parser.error(f'{path} must be a little-endian x64 Linux ELF file')
    # Preserve installed runtime paths because DXCore resolves DriverStore
    # filenames to /usr/lib/wsl/drivers; preserve the ELF interpreter path too.
    for path in [args.linux_umd, args.d3d12, args.d3d12core, args.dxcore] + args.extra_runtime:
        if not path.is_absolute():
            parser.error('Installed runtime paths must be absolute')
        files[path.as_posix().lstrip('/')] = path
    environment = dict(os.environ)
    dependency_directories = [p.resolve(strict=True) for p in args.dependency_directory]
    if args.mesa_runtime_directory:
        dependency_directories.append(mesa / 'lib')
    if any(not p.is_dir() for p in dependency_directories):
        parser.error('Dependency directories must exist')
    if dependency_directories:
        environment['LD_LIBRARY_PATH'] = ':'.join(str(p) for p in dependency_directories) + ':' + environment.get('LD_LIBRARY_PATH', '')
    for path in roots:
        result = subprocess.run(['ldd', str(path.resolve())], capture_output=True, text=True, timeout=10, check=True, env=environment)
        if 'not found' in result.stdout:
            parser.error(f'Unresolved dependency of {path}')
        for line in result.stdout.splitlines():
            match = re.search(r'(?:=>\s+)?(/[^\s]+)\s+\(0x[0-9a-f]+\)', line)
            if match:
                dependency = pathlib.Path(match[1])
                name = relocated.get(dependency.resolve(), dependency.as_posix().lstrip('/'))
                if dependency.resolve() not in relocated and any(dependency.resolve().is_relative_to(p) for p in dependency_directories):
                    name = 'usr/lib/x86_64-linux-gnu/' + dependency.name
                if name in files and files[name].resolve() != dependency.resolve():
                    parser.error(f'Conflicting guest dependency: {name}')
                files[name] = dependency
    archive = bytearray()
    inode = 0

    def entry(name, mode, data=b'', major=0, minor=0):
        nonlocal inode
        if name.startswith('/') or '..' in pathlib.PurePosixPath(name).parts or '\0' in name:
            raise ValueError('Unsafe archive path')
        inode += 1
        encoded = name.encode() + b'\0'
        fields = [inode, mode, 0, 0, 1, 0, len(data), 0, 0, major, minor, len(encoded), 0]
        archive.extend(b'070701' + ''.join(f'{f:08x}' for f in fields).encode())
        archive.extend(encoded); archive.extend(b'\0' * (-len(archive) % 4))
        archive.extend(data); archive.extend(b'\0' * (-len(archive) % 4))
        if len(archive) > 256 * 1024 * 1024:
            raise ValueError('Private runtime image exceeds 256 MiB bound')

    directories = {'dev', 'proc', 'sys', 'tmp'}
    for name in files:
        directories.update(str(p) for p in pathlib.PurePosixPath(name).parents if str(p) != '.')
    for directory in sorted(directories, key=lambda p: (p.count('/'), p)):
        entry(directory, stat.S_IFDIR | (0o1777 if directory == 'tmp' else 0o755))
    entry('dev/console', stat.S_IFCHR | 0o600, major=5, minor=1)
    manifest = {'schema': 1, 'privateImage': True, 'vendorBinariesIncluded': True,
                'redistributionApproved': False, 'runtimeWorkload': args.runtime_workload,
                'kernelApertureIncluded': bool(args.guest_aperture_module),
                'guestRenderNodeIncluded': bool(args.guest_render_module),
                'guestRenderKernelRelease': render_release.group(1).decode('ascii') if args.guest_render_module else None,
                'nativeSyncFilesEnabled': args.guest_native_sync_files,
                'nativeBuffersEnabled': args.guest_native_buffers,
                'guestBrokerContractIncluded': bool(args.guest_broker_contract),
                'guestGraphicsBrokerIncluded': bool(args.guest_graphics_broker),
                'mesaFrontendIncluded': bool(args.mesa_runtime_directory), 'files': []}
    for name, path in sorted(files.items()):
        data = path.read_bytes()
        entry(name, stat.S_IFREG | 0o755, data)
        manifest['files'].append({'path': '/' + name, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
    entry('linux-umd-name', stat.S_IFREG | 0o600, module.encode())
    entry('runtime-workload', stat.S_IFREG | 0o600, args.runtime_workload.encode())
    if args.guest_aperture_module:
        entry('kernel-aperture-required', stat.S_IFREG | 0o600, b'1')
    if args.guest_native_sync_files:
        entry('native-sync-files-required', stat.S_IFREG | 0o600, b'1')
    if args.guest_native_buffers:
        entry('native-buffers-required', stat.S_IFREG | 0o600, b'1')
    if args.guest_render_module:
        entry('native-render-required', stat.S_IFREG | 0o600, b'1')
    if args.guest_broker_contract:
        entry('broker-peer-required', stat.S_IFREG | 0o600, b'1')
    if args.guest_graphics_broker:
        entry('graphics-broker-required', stat.S_IFREG | 0o600, b'1')
    entry('TRAILER!!!', 0)
    packed = gzip.compress(archive, mtime=0)
    with args.output.open('xb') as destination:
        destination.write(packed)
    manifest['initramfsSha256'] = hashlib.sha256(packed).hexdigest()
    manifest['uncompressedBytes'] = len(archive)
    manifest['compressedBytes'] = len(packed)
    with args.output.with_suffix('.manifest.json').open('x', encoding='utf-8') as destination:
        json.dump(manifest, destination, indent=2); destination.write('\n')
    print(f'Created PRIVATE {args.output}: {len(packed)} bytes; never upload this image')


if __name__ == '__main__':
    main()
