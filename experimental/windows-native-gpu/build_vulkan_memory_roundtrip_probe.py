"""Compile normal/corrupt GPU FD probes from frozen sources; never run them."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def build(args):
    root = Path(__file__).resolve().parent
    out = args.output.resolve(); out.mkdir(exist_ok=False)
    frozen = out / 'source-frozen'; frozen.mkdir()
    names = ('vulkan_memory_roundtrip_probe.cpp', 'vulkan_memory_roundtrip_workload.h')
    for name in names: shutil.copyfile(root / name, frozen / name)
    compiler = Path(shutil.which(args.cxx) or args.cxx).resolve(strict=True)
    rows, commands = [], []
    with (out / 'build.log').open('x') as log:
        for profile in ('normal', 'corrupt-last', 'import-zero', 'fd-exhaustion', 'gpu-pressure'):
            folder = out / profile; folder.mkdir()
            binary = folder / 'vulkan-memory-roundtrip-probe'
            command = [str(compiler), '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                '-I' + str(args.vulkan_include.resolve()), '-DWDDM_BRIDGE_VULKAN_ICD_PATH="' + str(args.icd) + '"']
            if profile == 'corrupt-last': command.append('-DWDDM_BRIDGE_FD_ROUNDTRIP_CORRUPT=1')
            if profile == 'import-zero': command.append('-DWDDM_BRIDGE_FD_ROUNDTRIP_IMPORT_ZERO=1')
            if profile == 'fd-exhaustion': command.append('-DWDDM_BRIDGE_FD_ROUNDTRIP_EXHAUSTION=1')
            if profile == 'gpu-pressure': command.append('-DWDDM_BRIDGE_FD_ROUNDTRIP_GPU_PRESSURE=1')
            command += [str(frozen / names[0]), '-ldl', '-o', str(binary)]
            commands.append(command)
            subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
            rows.append(dict(profile=profile, probeSha256=sha(binary)))
    if len({row['probeSha256'] for row in rows}) != 5: raise ValueError('Control ineffective')
    receipt = dict(schema=1, sourceHashes={name: sha(frozen / name) for name in names}, profiles=rows,
        commands=commands, compilerSha256=sha(compiler), builderSha256=sha(Path(__file__)),
        guestIcdPath=str(args.icd), bufferBytes=65536, rounds=2, wordsPerRound=16384,
        hardwareTested=False, windowsOpened=0)
    with (out / 'build-receipt.json').open('x', encoding='utf-8', newline='\n') as output:
        json.dump(receipt, output, indent=2); output.write('\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--vulkan-include', required=True, type=Path)
    parser.add_argument('--icd', required=True, type=Path, help='Absolute guest ICD path; no library is loaded')
    parser.add_argument('--output', required=True, type=Path, help='New build directory')
    parser.add_argument('--cxx', default='c++')
    args = parser.parse_args()
    if not args.icd.is_absolute() or any(c in str(args.icd) for c in ('"', '\n', '\r')): parser.error('Invalid absolute ICD path')
    build(args)
    print('PASS: normal/corrupt/zero/FD-exhaustion/GPU-pressure probes compiled; no GPU workload launched')
