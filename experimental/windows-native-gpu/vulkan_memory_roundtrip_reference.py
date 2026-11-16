"""Independent integer hashes only; this never creates GPU or image evidence."""
import json


def expected_round(round_number):
    if type(round_number) is not int or round_number not in (0, 1): raise ValueError('Unknown round')
    value = 14695981039346656037
    for stripe in range(16):
        word = 0x8f24b17d ^ ((0x01030507 * (stripe + 1)) & 0xffffffff) ^ ((0x13579bdf * (round_number + 1)) & 0xffffffff)
        for _ in range(1024):
            for byte in word.to_bytes(4, 'little'):
                value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return dict(round=round_number, words=16384, fnv1a=f'{value:016x}')


if __name__ == '__main__':
    print(json.dumps(dict(scope='Independent expected values only', rounds=[expected_round(i) for i in range(2)],
        gpuExecutionVerified=False, windowIntegrationVerified=False), indent=2))
