"""Require executed original/fixed FD contracts; a failed build is not a control."""
import argparse
import hashlib
import json
from pathlib import Path
from test_dozen_memory_fd import function

CASES = ('invalid-sentinel', 'repeated-fresh-cloexec', 'descriptor-zero',
    'export-survives-owner-close', 'descriptor-exhaustion-rollback',
    'unknown-type-rollback', 'descriptor-range', 'close-on-exec')


def require(ok, message):
    if not ok: raise ValueError(message)


def validate_log(log, profile):
    require(profile in ('original', 'patched'), 'Unknown contract profile')
    expected = ['DOZEN_FD_CONTRACT case=' + case + ' passed=' +
        ('true' if profile == 'patched' or case == 'unknown-type-rollback' else 'false') + ' gpu=false'
        for case in CASES]
    failed = 7 if profile == 'original' else 0
    expected.append(f'DOZEN_FD_CONTRACT_COMPLETE cases=8 failed={failed} gpu=false')
    require(log.splitlines() == expected, 'Missing, duplicate or incorrect executed FD cases')
    return dict(profile=profile, cases=8, failed=failed, exit=1 if failed else 0,
        gpuExecutionVerified=False, windowIntegrationVerified=False)


def audit(folder, profile):
    here = Path(__file__).resolve().parent
    manifest = json.loads((here / 'dozen-memory-fd-patch.json').read_bytes())
    receipt = json.loads((folder / 'receipt.json').read_bytes())  # absent on compile failure
    proof = validate_log((folder / 'contract.log').read_text(), profile)
    sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    require(receipt['schema'] == 1 and receipt['profile'] == profile and
        type(receipt['exit']) is int and receipt['exit'] == proof['exit'] and
        receipt['gpuExecutionVerified'] is receipt['windowIntegrationVerified'] is False,
        'Wrong contract receipt/scope')
    require(receipt['runnerSha256'] == sha(here / 'test_dozen_memory_fd.py') and
        receipt['contractSourceSha256'] == sha(here / 'dozen_memory_fd_contract.cpp') ==
        sha(folder / 'dozen_memory_fd_contract.cpp') and
        receipt['sourceSha256'] == manifest['files']['src/microsoft/vulkan/dzn_device.c'][profile + 'Sha256'],
        'Contract runner/source differs')
    include = folder / 'dozen_memory_fd_under_test.inc'
    require(receipt['generatedIncludeSha256'] == sha(include) and
        receipt['binarySha256'] == sha(folder / 'dozen-memory-fd-contract'), 'Contract include/binary changed')
    fragment = include.read_text()
    digest = lambda text: hashlib.sha256(text.encode()).hexdigest()
    require(receipt['functionSha256'] == digest(function(fragment, 'dzn_GetMemoryFdKHR')) ==
        manifest['contractFunctions'][profile + 'Sha256'] and
        receipt['duplicateHelperSha256'] == digest(function(fragment, 'os_dupfd_cloexec')) ==
        manifest['contractFunctions']['duplicateHelperSha256'], 'Extracted upstream function/helper differs')
    return proof


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', required=True, type=Path)
    parser.add_argument('--patched', required=True, type=Path)
    args = parser.parse_args()
    for folder, profile in ((args.baseline, 'original'), (args.patched, 'patched')):
        print(json.dumps(audit(folder, profile), sort_keys=True))
