"""Pack locally captured NVIDIA context inputs for the disk-free QEMU fixture.

The opaque driver data belongs to the locally installed driver version. Keep
captures and the resulting initramfs local; no vendor bytes belong in Git.
"""
import argparse
import pathlib
import struct


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=pathlib.Path, required=True)
    parser.add_argument('--output', type=pathlib.Path, required=True)
    args = parser.parse_args()
    files = sorted(args.input.glob('wsl-context-input-*.bin'))
    if not 1 <= len(files) <= 16:
        parser.error('Need 1..16 local context input files in a fresh capture directory')
    records = []
    for path in files:
        raw = path.read_bytes()
        if len(raw) < 24:
            parser.error(f'Truncated context header: {path.name}')
        magic, node, engine, flags, hint, size = struct.unpack('<6I', raw[:24])
        if (magic != 0x31585443 or node >= 64 or engine != 1 or flags not in (0, 16)
                or hint != 12 or not 0 < size <= 4000 or len(raw) != 24 + size):
            parser.error(f'Unsupported or malformed context input: {path.name}')
        records.append(struct.pack('<6I', node, engine, flags, hint, size, 0) + raw[24:])
    args.output.write_bytes(struct.pack('<I', len(records)) + b''.join(records))
    print(f'Packed {len(records)} locally captured contexts; guest rendering remains unverified')


if __name__ == '__main__':
    main()
