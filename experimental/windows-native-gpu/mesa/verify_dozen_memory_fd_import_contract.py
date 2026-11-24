"""Require all executed CPU import ownership cases and their actual binary."""
import argparse
import hashlib
import json
from pathlib import Path
from test_dozen_memory_fd import function

CASES = ('invalid-input', 'commit-closes-both', 'rollback-retains-source', 'descriptor-zero',
    'exhaustion-rollback', 'close-on-exec', 'idempotent-cleanup-reuse', 'busy-owner-rollback')


def require(ok, message):
    if not ok: raise ValueError(message)


def validate_log(log):
    expected = [f'DOZEN_IMPORT_FD_CONTRACT case={name} passed=true gpu=false' for name in CASES]
    expected.append('DOZEN_IMPORT_FD_CONTRACT_COMPLETE cases=8 failed=0 gpu=false')
    require(log.splitlines() == expected, 'Missing, duplicate or failed import ownership cases')


def audit(folder, source, vulkan_include):
    here = Path(__file__).resolve().parent
    sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    receipt = json.loads((folder / 'receipt.json').read_bytes())
    validate_log((folder / 'contract.log').read_text())
    manifest = json.loads((here / 'dozen-memory-fd-import-patch.json').read_bytes())
    for name, row in manifest['files'].items(): require(sha(source / name) == row['patchedSha256'], 'Patched CPU source differs')
    require(receipt['schema'] == 1 and type(receipt['exit']) is int and receipt['exit'] == 0 and
        receipt['gpuExecutionVerified'] is receipt['windowIntegrationVerified'] is False, 'CPU receipt scope/exit differs')
    require(receipt['runnerSha256'] == sha(here / 'test_dozen_memory_fd_import.py') and
        receipt['patchedDeviceSourceSha256'] == manifest['files']['src/microsoft/vulkan/dzn_device.c']['patchedSha256'], 'Runner/device source changed')
    for name, digest in receipt['files'].items(): require(sha(folder / name) == digest, 'Compiled contract file changed')
    require(set(receipt['files']) == {'dzn_imported_memory_fd.h', 'util/os_file.h', 'duplicate_helper.inc',
        'dozen_memory_fd_import_contract.cpp', 'dozen-memory-fd-import-contract'}, 'Missing contract files')
    require(receipt['files']['dozen_memory_fd_import_contract.cpp'] == sha(here / 'dozen_memory_fd_import_contract.cpp') and
        receipt['files']['dzn_imported_memory_fd.h'] == manifest['files']['src/microsoft/vulkan/dzn_imported_memory_fd.h']['patchedSha256'] and
        receipt['files']['util/os_file.h'] == sha(source / 'src/util/os_file.h'), 'Contract/header inputs differ')
    exported = json.loads((here / 'dozen-memory-fd-patch.json').read_bytes())
    duplicate = function((folder / 'duplicate_helper.inc').read_text(encoding='utf-8'), 'os_dupfd_cloexec')
    require(receipt['duplicateHelperSha256'] == hashlib.sha256(duplicate.encode()).hexdigest() ==
        exported['contractFunctions']['duplicateHelperSha256'], 'Real Mesa duplicate helper differs')
    require(set(receipt['vulkanHeaderHashes']) == {'vulkan_core.h', 'vk_platform.h'}, 'Missing Vulkan header hashes')
    for name, digest in receipt['vulkanHeaderHashes'].items(): require(sha(vulkan_include / 'vulkan' / name) == digest, 'Vulkan header differs')
    return dict(cases=8, failed=0, descriptorZeroVerified=True, rollbackAndExhaustionVerified=True,
        gpuExecutionVerified=False, windowIntegrationVerified=False, receiptSha256=sha(folder / 'receipt.json'))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--contract', required=True, type=Path)
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--vulkan-include', required=True, type=Path)
    args = parser.parse_args()
    print(json.dumps(audit(args.contract, args.source, args.vulkan_include), sort_keys=True))
