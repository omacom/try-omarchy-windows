"""Build normal/corrupt-last 4K Vulkan probes without launching a GPU workload.

Requires an existing GLSL compiler, SPIR-V validator, C++ compiler and Vulkan
headers. Tool installation, proprietary libraries and VM packaging are separate.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def build(args):
    root = Path(__file__).resolve().parent
    out = args.output.resolve()
    out.mkdir(exist_ok=False)
    frozen = out / 'source-frozen'; frozen.mkdir()
    sources = ('vulkan_compute_probe.cpp', 'vulkan_compute_workload.h', 'vulkan_compute.comp')
    for name in sources: shutil.copyfile(root / name, frozen / name)
    cxx = Path(shutil.which(args.cxx) or args.cxx).resolve(strict=True)
    tools = dict(cxx=cxx, glslang=args.glslang.resolve(strict=True), spirvVal=args.spirv_val.resolve(strict=True))
    rows = []
    commands = []
    with (out / 'build.log').open('x', encoding='utf-8', newline='\n') as log:
        def run(command):
            commands.append(command)
            subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
        for profile in ('normal', 'corrupt-last'):
            folder = out / profile; folder.mkdir()
            spv = folder / 'compute.spv'
            command = [str(tools['glslang']), '-V', '--target-env', 'vulkan1.1', '-S', 'comp']
            if profile == 'corrupt-last': command.append('-DWDDM_VULKAN_TEST_CORRUPT_LAST=1')
            run(command + ['-o', str(spv), str(frozen / 'vulkan_compute.comp')])
            run([str(tools['spirvVal']), '--target-env', 'vulkan1.1', str(spv)])
            data = spv.read_bytes()
            if len(data) < 20 or len(data) % 4: raise ValueError('Invalid SPIR-V size')
            words = struct.unpack('<' + 'I' * (len(data) // 4), data)
            if words[0] != 0x07230203: raise ValueError('Invalid SPIR-V magic')
            include = folder / 'vulkan_compute_shader.inc'
            with include.open('x', encoding='utf-8', newline='\n') as output:
                output.write('// Generated from validated vulkan_compute.comp, SPIR-V SHA256 ' + sha(spv) + '\n')
                output.write('static constexpr std::uint32_t VulkanComputeShader[] = {\n')
                for start in range(0, len(words), 8):
                    output.write('    ' + ', '.join(f'0x{word:08x}u' for word in words[start:start + 8]) + ',\n')
                output.write('};\n')
            binary = folder / 'vulkan-compute-probe'
            run([str(cxx), '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                 '-I' + str(args.vulkan_include.resolve()), '-I' + str(folder),
                 '-DWDDM_BRIDGE_VULKAN_ICD_PATH="' + str(args.icd) + '"',
                 str(frozen / 'vulkan_compute_probe.cpp'), '-ldl', '-o', str(binary)])
            rows.append(dict(profile=profile, spirvSha256=sha(spv), shaderIncludeSha256=sha(include),
                probeSha256=sha(binary), shaderValidationPassed=True))
    if rows[0]['spirvSha256'] == rows[1]['spirvSha256']: raise ValueError('Corruption control did not change the shader')
    receipt = dict(schema=1, sourceHashes={name: sha(frozen / name) for name in sources},
        profiles=rows, commands=commands, toolHashes={name: sha(path) for name, path in tools.items()},
        icdPath=str(args.icd), builderSha256=sha(Path(__file__)), width=3840, height=2160,
        rounds=2, wordsPerRound=8294400, hardwareTested=False, windowsOpened=0)
    with (out / 'build-receipt.json').open('x', encoding='utf-8', newline='\n') as output:
        json.dump(receipt, output, indent=2); output.write('\n')
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--glslang', type=Path, required=True)
    parser.add_argument('--spirv-val', type=Path, required=True)
    parser.add_argument('--vulkan-include', type=Path, required=True)
    parser.add_argument('--icd', type=Path, required=True, help='Absolute Linux guest ICD path embedded in the probes')
    parser.add_argument('--cxx', default='c++')
    parser.add_argument('--output', type=Path, required=True, help='Fresh build directory')
    args = parser.parse_args()
    if not args.icd.is_absolute() or '"' in str(args.icd) or '\n' in str(args.icd): parser.error('ICD path must be absolute and contain no quote/newline')
    build(args)
    print('PASS: normal/corrupt-last GLSL validated and both probes compiled; no hardware or window launched')


if __name__ == '__main__':
    main()
