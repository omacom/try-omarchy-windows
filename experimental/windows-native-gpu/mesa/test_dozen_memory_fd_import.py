"""Compile the patched real import-FD helper and Mesa duplication code; CPU only."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
from test_dozen_memory_fd import function


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--vulkan-include', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path, help='New immutable result directory')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    root = args.source.resolve(strict=True)
    sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    manifest = json.loads((here / 'dozen-memory-fd-import-patch.json').read_bytes())
    for name, row in manifest['files'].items():
        if sha(root / name) != row['patchedSha256']: parser.error('Unexpected patched source/header')
    export = json.loads((here / 'dozen-memory-fd-patch.json').read_bytes())
    source = (root / 'src/util/os_file.c').read_text(encoding='utf-8')
    helper = function(source, 'os_dupfd_cloexec', 1)
    helper_hash = hashlib.sha256(helper.encode()).hexdigest()
    if helper_hash != export['contractFunctions']['duplicateHelperSha256']: parser.error('Unexpected real duplication helper')
    out = args.output.resolve(); out.mkdir(exist_ok=False)
    (out / 'util').mkdir()
    for source_path, destination in ((root / 'src/microsoft/vulkan/dzn_imported_memory_fd.h', out / 'dzn_imported_memory_fd.h'),
        (root / 'src/util/os_file.h', out / 'util/os_file.h'), (here / 'dozen_memory_fd_import_contract.cpp', out / 'dozen_memory_fd_import_contract.cpp')):
        shutil.copyfile(source_path, destination)
    notice = source[:source.index('*/') + 2]
    (out / 'duplicate_helper.inc').write_text(notice + '\nint\n' + helper, encoding='utf-8', newline='\n')
    binary = out / 'dozen-memory-fd-import-contract'
    command = ['c++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-I' + str(out),
        '-I' + str(args.vulkan_include.resolve()), str(out / 'dozen_memory_fd_import_contract.cpp'), '-o', str(binary)]
    with (out / 'build.log').open('x') as log: subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
    with (out / 'contract.log').open('x') as log:
        result = subprocess.run([str(binary)], stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
    receipt = dict(schema=1, scope='Real OS descriptor transaction only; no GPU/device', exit=result.returncode,
        patchedDeviceSourceSha256=sha(root / 'src/microsoft/vulkan/dzn_device.c'), duplicateHelperSha256=helper_hash,
        files={name: sha(out / name) for name in ('dzn_imported_memory_fd.h', 'util/os_file.h', 'duplicate_helper.inc', 'dozen_memory_fd_import_contract.cpp',
            'dozen-memory-fd-import-contract')}, vulkanHeaderHashes={name: sha(args.vulkan_include / 'vulkan' / name) for name in ('vulkan_core.h', 'vk_platform.h')},
        command=command, runnerSha256=sha(Path(__file__)), gpuExecutionVerified=False, windowIntegrationVerified=False)
    with (out / 'receipt.json').open('x') as output: json.dump(receipt, output, indent=2); output.write('\n')
    print((out / 'contract.log').read_text(), end='')
    return result.returncode


if __name__ == '__main__': sys.exit(main())
