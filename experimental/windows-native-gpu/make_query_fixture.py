"""Pack successful local NVIDIA adapter query inputs; never redistribute vendor data."""
import argparse
import pathlib
import struct


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=pathlib.Path, required=True)
    parser.add_argument('--output', type=pathlib.Path, required=True)
    args = parser.parse_args()
    records = []
    nvidia = set()
    for path in args.input.glob('wsl-query-input-*.bin'):
        raw = path.read_bytes()
        if len(raw) < 24:
            parser.error(f'Truncated header: {path.name}')
        magic, sequence, kind, size, adapter, reserved = struct.unpack('<6I', raw[:24])
        if magic != 0x31595141 or reserved or not 0 < size <= 65536 or len(raw) != 24 + size:
            parser.error(f'Malformed query: {path.name}')
        output = args.input / f'wsl-query-output-{sequence}.bin'
        if output.exists():
            reference = output.read_bytes()
            if len(reference) != size:
                parser.error(f'Malformed reply: {output.name}')
            if kind == 31 and size == 28 and struct.unpack_from('<I', reference, 4)[0] == 0x10de:
                nvidia.add(adapter)
            records.append((sequence, kind, size, adapter, raw[24:]))
    selected = []
    for sequence, kind, size, adapter, data in sorted(records):
        if adapter not in nvidia:
            continue
        if kind not in (0, 1, 3, 13, 15, 17, 18, 24, 27, 30, 31, 34, 55, 56, 60, 61, 62, 66):
            parser.error(f'Successful NVIDIA query type {kind} requires protocol support')
        selected.append(struct.pack('<4I', kind, size, 0, 0) + data)
    if not 1 <= len(selected) <= 32:
        parser.error('Need 1..32 successful NVIDIA queries in a fresh local capture')
    args.output.write_bytes(struct.pack('<I', len(selected)) + b''.join(selected))
    print(f'Packed {len(selected)} local query inputs; live guest graphics remains unverified')


if __name__ == '__main__':
    main()
