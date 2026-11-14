"""Compile the exact patched ICD buffer helper with SDK types and mock exports."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--directx-headers', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    source = args.source.resolve(strict=True)
    headers = args.directx_headers.resolve(strict=True) / 'include'
    manifest = json.loads((here / 'dozen-native-gpu-buffer-patch.json').read_bytes())
    digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    for name, row in manifest['files'].items():
        if digest(source / name) != row['patchedSha256']:
            parser.error('Requires exact patched source and helper')
    helper = source / 'src/microsoft/vulkan/dzn_wddm_native_gpu_buffer.h'
    if digest(helper) != digest(here / helper.name):
        parser.error('Canonical helper and applied patch differ')
    out = args.output.resolve(); out.mkdir(exist_ok=False)
    contract = out / 'dozen_native_gpu_buffer_contract.cpp'
    shutil.copyfile(here / contract.name, contract)
    command = ['c++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
        '-isystem', str(headers), '-isystem', str(headers / 'wsl/stubs'),
        '-I' + str(source / 'include'), '-I' + str(helper.parent),
        str(contract), '-ldl', '-o', str(out / 'contract')]
    with (out / 'contract.log').open('x') as log:
        subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
        subprocess.run([str(out / 'contract')], check=True, stdout=log, stderr=subprocess.STDOUT)
    with (out / 'receipt.json').open('x', encoding='utf-8') as f:
        json.dump(dict(command=command, helperSha256=digest(helper),
            testSha256=digest(contract), passed=True, hardwareTested=False), f, indent=2)
        f.write('\n')
    print('PASS: exact ICD buffer helper, explicit opt-in, public bounds, rollback and one-shot cancellation; no GPU')


if __name__ == '__main__':
    main()
