"""Build a tiny newc initramfs from two locally compiled static ELF files."""
import argparse
import pathlib
import stat


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--init', type=pathlib.Path, required=True)
    parser.add_argument('--probe', type=pathlib.Path, required=True)
    parser.add_argument('--output', type=pathlib.Path, required=True)
    parser.add_argument('--context-fixture', type=pathlib.Path, help='Optional local UMD context data; never redistribute captured vendor data')
    parser.add_argument('--query-fixture', type=pathlib.Path, help='Optional local adapter query data; never redistribute captured vendor data')
    args = parser.parse_args()
    for source in (args.init, args.probe):
        if source.read_bytes()[:4] != b'\x7fELF':
            parser.error(f'{source} must be a Linux ELF executable')
    archive = bytearray()

    def entry(name, mode, data=b'', major=0, minor=0):
        encoded = name.encode() + b'\0'
        fields = [1, mode, 0, 0, 1, 0, len(data), 0, 0, major, minor, len(encoded), 0]
        archive.extend(b'070701' + ''.join(f'{f:08x}' for f in fields).encode())
        archive.extend(encoded)
        archive.extend(b'\0' * (-len(archive) % 4))
        archive.extend(data)
        archive.extend(b'\0' * (-len(archive) % 4))

    for directory in ('dev', 'proc', 'sys'):
        entry(directory, stat.S_IFDIR | 0o755)
    entry('dev/console', stat.S_IFCHR | 0o600, major=5, minor=1)
    entry('init', stat.S_IFREG | 0o755, args.init.read_bytes())
    entry('guest-probe', stat.S_IFREG | 0o755, args.probe.read_bytes())
    if args.context_fixture:
        data = args.context_fixture.read_bytes()
        if len(data) > 16 * 4024 + 4:
            parser.error('Context fixture exceeds bounded test size')
        entry('driver-contexts.bin', stat.S_IFREG | 0o600, data)
    if args.query_fixture:
        data = args.query_fixture.read_bytes()
        if len(data) > 32 * (65536 + 16) + 4:
            parser.error('Query fixture exceeds bounded test size')
        entry('driver-queries.bin', stat.S_IFREG | 0o600, data)
    entry('TRAILER!!!', 0)
    args.output.write_bytes(archive)
    print(f'Created {args.output}: {len(archive)} bytes; no disk image')


if __name__ == '__main__':
    main()
