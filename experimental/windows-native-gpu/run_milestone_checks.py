"""Run a deterministic partition of the complete archived milestone suite."""
import argparse
from pathlib import Path
import sys
import unittest


def suite(index, count):
    if count < 1 or not 0 <= index < count:
        raise ValueError('Invalid milestone shard')
    root = Path(__file__).resolve().parent
    files = sorted(root.glob('test_*milestone.py'))
    if count > len(files):
        raise ValueError('More shards than milestone modules')
    selected = files[index::count]
    sys.path.insert(0, str(root))
    loader = unittest.TestLoader()
    tests = unittest.TestSuite(loader.loadTestsFromName(path.stem) for path in selected)
    if loader.errors:
        raise RuntimeError('\n'.join(loader.errors))
    return tests, [path.name for path in selected]


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--shard-index', type=int, required=True)
    parser.add_argument('--shard-count', type=int, required=True)
    args = parser.parse_args()
    tests, names = suite(args.shard_index, args.shard_count)
    print(f'Milestone shard {args.shard_index + 1}/{args.shard_count}: '
          f'{len(names)} modules, {tests.countTestCases()} tests', flush=True)
    for name in names:
        print('  ' + name, flush=True)
    result = unittest.TextTestRunner(verbosity=2).run(tests)
    sys.exit(0 if result.wasSuccessful() else 1)
