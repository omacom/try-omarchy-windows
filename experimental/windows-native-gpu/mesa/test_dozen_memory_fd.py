"""Compile the exact pinned Dozen export function and test real Linux FD rules.

This is a source-level CPU contract. It never creates a Vulkan device or loads
an ICD and must not be counted as GPU, sharing or window-integration acceptance.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys


def function(source, name, occurrence=0):
    matches = list(re.finditer(r'^' + re.escape(name) + r'\(', source, re.M))
    if occurrence >= len(matches): raise ValueError('Missing pinned function ' + name)
    start = matches[occurrence].start()
    return source[start:source.index('\n}\n', start) + 3]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True, help='Pinned original or patched Mesa source tree')
    parser.add_argument('--vulkan-include', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True, help='New immutable contract directory')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    manifest = json.loads((here / 'dozen-memory-fd-patch.json').read_bytes())
    root = args.source.resolve(strict=True)
    source_path = root / 'src/microsoft/vulkan/dzn_device.c'
    raw = source_path.read_bytes()
    sha = lambda data: hashlib.sha256(data).hexdigest()
    expected = manifest['files']['src/microsoft/vulkan/dzn_device.c']
    profile = next((name for name in ('original', 'patched') if sha(raw) == expected[name + 'Sha256']), None)
    if not profile or (root / 'VERSION').read_text().strip() != manifest['mesaVersion']:
        parser.error('Unrecognized source bytes/version; no output created')
    source = raw.decode('utf-8')
    under_test = function(source, 'dzn_GetMemoryFdKHR')
    helper = function((root / 'src/util/os_file.c').read_text(), 'os_dupfd_cloexec', 1)
    if sha(under_test.encode()) != manifest['contractFunctions'][profile + 'Sha256'] or \
       sha(helper.encode()) != manifest['contractFunctions']['duplicateHelperSha256']:
        parser.error('Pinned function/helper differs; no output created')
    out = args.output.resolve(); out.mkdir(exist_ok=False)
    # Preserve the upstream MIT notice in this generated derivative fragment.
    license_text = source[:source.index('*/') + 2]
    generated = license_text + '\nint\n' + helper + '\nVkResult\n' + under_test
    include = out / 'dozen_memory_fd_under_test.inc'
    include.write_text(generated, encoding='utf-8', newline='\n')
    contract_source = out / 'dozen_memory_fd_contract.cpp'
    contract_source.write_bytes((here / contract_source.name).read_bytes())
    program = out / 'dozen-memory-fd-contract'
    command = ['c++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
        '-I' + str(args.vulkan_include.resolve()), '-I' + str(out),
        str(contract_source), '-o', str(program)]
    with (out / 'build.log').open('x') as log:
        subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
    with (out / 'contract.log').open('x') as log:
        result = subprocess.run([str(program)], stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
    receipt = dict(schema=1, scope='Extracted-function CPU FD contract only', profile=profile,
        sourceSha256=sha(raw), functionSha256=sha(under_test.encode()), duplicateHelperSha256=sha(helper.encode()),
        generatedIncludeSha256=sha(include.read_bytes()), contractSourceSha256=sha(contract_source.read_bytes()),
        binarySha256=sha(program.read_bytes()), command=command, exit=result.returncode,
        runnerSha256=sha(Path(__file__).read_bytes()), gpuExecutionVerified=False, windowIntegrationVerified=False)
    with (out / 'receipt.json').open('x') as output:
        json.dump(receipt, output, indent=2); output.write('\n')
    print((out / 'contract.log').read_text(), end='')
    return result.returncode


if __name__ == '__main__':
    sys.exit(main())
