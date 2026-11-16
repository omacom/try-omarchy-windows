"""Independent CPU expected hashes only; never observed GPU rendering evidence."""
import argparse
import hashlib
import json
from pathlib import Path


def reference():
    rows = []
    for generation in range(2):
        value = 0xcbf29ce484222325
        for v in range(2160):
            for u in range(3840):
                channels = ((u * 13 + v * 7 + generation * 31) & 0xff,
                    ((u ^ (v * 3)) + generation * 71) & 0xff,
                    ((u // 17) ^ (v // 11) ^ (generation * 113)) & 0xff, 255)
                for channel in channels:
                    value = ((value ^ channel) * 0x100000001b3) & 0xffffffffffffffff
        rows.append(dict(round=generation, words=8294400, fnv1a=f'{value:016x}'))
    return dict(schema=1, scope='CPU expected-value oracle only; not observed GPU output',
        width=3840, height=2160, rounds=rows,
        oracleSha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True, help='Fresh expected-hash result')
    args = parser.parse_args()
    with args.output.open('x', encoding='utf-8', newline='\n') as output:
        result = reference(); json.dump(result, output, indent=2); output.write('\n')
    print('PASS: independent CPU reference hashes generated; no GPU execution claimed')
