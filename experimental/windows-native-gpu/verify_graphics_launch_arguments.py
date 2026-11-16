"""Audit launcher-generated argument preservation and the refactored native GPU runs."""
import argparse
import hashlib
import json
from pathlib import Path
import re
from verify_background_async_waits import load, sha, QEMU, KERNEL
from verify_4k_capture import require, verify
from vulkan_compute_evidence import validate_compute_controls, validate_compute_cleanup, validate_compute_receipts
from vulkan_gpu_pressure_evidence import validate_pressure_receipts, validate_admission_queries, validate_pressure_metadata, validate_pressure_cleanup
from background_shader_evidence import validate_background_controls
from broker_runtime_evidence import broker_aware_initialization_complete


def closure(root):
    result = {}
    def visit(path):
        name = str(path.relative_to(root)).replace('\\', '/')
        if name in result: return
        result[name] = sha(path)
        for include in re.findall(r'^\s*#include "([^"]+)"', path.read_text(encoding='utf-8'), re.M):
            target = path.parent / include
            if target.is_file(): visit(target)
    visit(root / 'experimental/windows-native-gpu/driver_bridge.cpp')
    return result


def audit(lab):
    gpu = Path(__file__).resolve().parent
    repo = gpu.parent.parent
    previous_path = repo / 'docs/evidence/VULKAN-GPU-PRESSURE-2026-10-29.json'
    require(sha(previous_path) == '176e067d34ca462dc8d0549c417fa3063e4342c73c6f15edcab671d176c3c0c0', 'Prior pressure evidence changed')
    previous_proof = load(previous_path)
    frozen = lab / 'native-v119-source-frozen'
    native = load(lab / 'native-v119-receipt.json')
    for name, digest in native['sources'].items(): require(sha(frozen / name) == digest, 'Frozen source changed')
    inputs = closure(frozen)
    require(all(sha(repo / name) == digest for name, digest in inputs.items()), 'Current compiled Windows source differs')
    before = closure(lab / 'native-v118-source-frozen')
    delta = {name for name in inputs.keys() | before.keys() if inputs.get(name) != before.get(name)}
    require(delta == {'experimental/windows-native-gpu/qemu_runtime_host.h', 'experimental/windows-native-gpu/qemu_graphics_arguments.h'},
        'Refactor changes Windows inputs beyond argument composition')
    require(native['bridgeSha256'] == sha(lab / 'native-v119/driver-bridge.exe') == 'c2dd18a003353af21f33583169236730dffa470b6517107131e342c7adc202da' and
        native['defaultBridgeSha256'] == sha(lab / 'native-default-v119/driver-bridge.exe') == 'f20242f19e7c73bb8eb90428c343895fdce03c7424b9120190042b1809ea67d0', 'Wrong native binaries')
    require(sha(lab / 'qemu-sdl-user-network-v1/bin/qemu-system-x86_64.exe') == QEMU and
        sha(lab.parent / 'compositor-buffer-lab/kernel-7.2.8/vmlinuz-linux') == KERNEL, 'QEMU/kernel artifact differs')
    marker = 'QEMU_GRAPHICS_ARGUMENTS_PASS checks=35 originalInputPreserved=true processHandoffVerified=false gpu=false'
    for label in ('native-v119', 'native-default-v119'):
        require(marker in (lab / ('build-' + label + '.log')).read_text(), 'Strict native CPU controls missing')
    for name in ('qemu_graphics_arguments.h', 'qemu_graphics_arguments_test.cpp'):
        require(sha(gpu / name) == native['sources']['experimental/windows-native-gpu/' + name], 'Tested composer source differs')
    launcher = lab / 'launcher-real-arguments-v1'
    lr = load(launcher / 'receipt.json')
    proof = dict(launcherProfilesVerified=5, compatibleProfiles=2, unsupportedProfilesRejected=3,
        officialArgumentsPreserved=True, qemuLaunched=False, inputInjected=False, processHandoffVerified=False, dailyUseAccepted=False)
    require(lr['proof'] == proof and lr['sourceUnchanged'] is True and lr['hardwareTested'] is False and lr['windowsOpened'] == 0,
        'Wrong launcher test scope')
    for name, digest in lr['actualGoLauncherSourceHashes'].items(): require(sha(repo / 'app' / name) == digest, 'Launcher source changed')
    require(len(lr['actualGoLauncherSourceHashes']) == 430 and sha(launcher / 'launcher-arguments.json') == lr['launcherArgumentsSha256'], 'Wrong launcher export')
    require(lr['composerHeaderSha256'] == sha(gpu / 'qemu_graphics_arguments.h') and
        lr['composerTestSourceSha256'] == sha(gpu / 'qemu_launcher_arguments_test.cpp') and
        lr['exportTestSha256'] == sha(gpu / 'launcher-args-probe/export_test.go') and
        lr['verifierSha256'] == sha(gpu / 'test_launcher_graphics_arguments.py'), 'Current launcher test differs')
    compiler = lab / 'launcher-argument-probe-v1'
    cr = load(compiler / 'recovered-build-receipt-v1.json')
    require(cr['strictCompilationSucceeded'] is cr['receiptCommandFailed'] is cr['recoveredWithPython'] is True,
        'Compiler/receipt failure scope erased')
    require(cr['binarySha256'] == lr['composerTestSha256'] == sha(compiler / 'qemu-launcher-arguments-test.exe') ==
        '6418a984e842116bfe1ead37491f73502f405d20093832176091016b6793881e', 'Wrong composer binary')
    for name, digest in cr['sourceHashes'].items(): require(sha(gpu / name) == digest, 'Composer compiler input changed')
    require(cr['buildLogSha256'] == sha(lab / 'build-launcher-argument-probe-v1.log') and cr['buildBatchSha256'] == sha(compiler / 'build.cmd') and
        "Get-FileHash : The term 'Get-FileHash' is not recognized" in (lab / 'build-launcher-argument-probe-v1.log').read_text(),
        'Receipt-tool failure was not retained')
    def files(folder):
        return {name: sha(folder / name) for name in ('source.json', 'driver.json', 'driver.log', 'driver.host.log', 'driver.errors.log')}
    def observed(folder, old, label):
        s, r = load(folder / 'source.json'), load(folder / 'driver.json')
        old_s = load(old / 'source.json')
        log = (folder / 'driver.log').read_text()
        records = [json.loads(line) for line in (folder / 'driver.host.log').read_text().splitlines() if line.startswith('{')]
        image = lab / ('background-vulkan-gpu-admission-' + label + '-v1/private-vulkan.cpio.gz')
        require(s['initramfsSha256'] == r['initramfsSha256'] == old_s['initramfsSha256'] == sha(image), 'Regression image differs')
        pack = load(image.parent / 'pack-receipt.json')
        require(s['packReceiptSha256'] == old_s['packReceiptSha256'] == sha(image.parent / 'pack-receipt.json') and
            pack['sourceDiskAttached'] is pack['presentationRequested'] is pack['inputRequested'] is False, 'Image/pack scope differs')
        require(s['nativeSha256'] == r['driverBridgeSha256'] == native['bridgeSha256'] and
            s['nativeBuildReceiptSha256'] == sha(lab / 'native-v119-receipt.json') and r['qemuSha256'] == QEMU and r['kernelSha256'] == KERNEL,
            'Runtime executable/kernel differs')
        require(s['readOnlyInstalledDriverBefore'] == s['readOnlyInstalledDriverAfter'] == old_s['readOnlyInstalledDriverAfter'] and
            s['readOnlyInstalledDriverAfter']['installedRuntimeMatchesSnapshot'] is True and
            s['harnessUnchanged'] is s['artifactsUnchanged'] is True, 'Runtime/driver guards differ')
        require(r['hypervisor'] == 'QEMU/WHPX' and r['liveNvidiaLinuxUmdLoaded'] is True and
            r['capturedPrivateFixturesUsed'] is r['privateRuntimeImageRedistributable'] is r['realWslDxgForwarding'] is False, 'GPU provenance differs')
        require(s['harnessSha256'] == old_s['harnessSha256'] == sha(gpu / 'broker_runtime_qemu.py') and all(
            sha(gpu / name) == digest for name, digest in s['harnessComponents'].items()), 'Harness changed')
        # This refactor changes the host executable; the input VM, options,
        # byte budgets and scoped GPU oracles stay the same as the prior run.
        command, previous = list(s['command']), list(old_s['command'])
        for option in ('--bridge', '--report'):
            require(command.count(option) == previous.count(option) == 1, 'Missing command boundary')
            command[command.index(option) + 1] = previous[previous.index(option) + 1]
        require(command == previous, 'Regression options changed')
        return s, r, log, records
    rows = {}
    for label, name, old in (
        ('pressure', 'background-native-graphics-arguments-run-1', 'background-vulkan-metadata-memory-run-6'),
        ('compute', 'background-native-graphics-arguments-compute-run-1', 'background-vulkan-gpu-admission-compute-run-1'),
        ('texture', 'background-native-graphics-arguments-texture-run-1', 'background-vulkan-gpu-admission-texture-run-1')):
        folder = lab / name
        historical = previous_proof['runs'][0]['privateReceiptHashes'] if label == 'pressure' else previous_proof['regressions'][label]['privateReceiptHashes']
        require(files(lab / old) == historical, 'Prior accepted control receipts changed')
        source, report, log, records = observed(folder, lab / old, label)
        if label == 'pressure':
            validate_compute_controls(source, log, records, 256)
            cleanup = validate_pressure_cleanup(report, records)
            gpu_proof = validate_pressure_receipts(log)
            validate_admission_queries(records, report['disconnectCleanup'], True)
            validate_pressure_metadata(records, report['disconnectCleanup'])
        elif label == 'compute':
            validate_compute_controls(source, log, records, 256)
            cleanup = validate_compute_cleanup(report, records, 256, 'pass')
            gpu_proof = validate_compute_receipts(log, 'pass')
        else:
            validate_background_controls(source, log, records, 10000)
            require(broker_aware_initialization_complete(log, report['hostBridgeExit'], report['ownedQemuControl'], report['disconnectCleanup']), 'Shader initialization failed')
            oracle = lab / 'native-v111/rgba-oracle.exe'
            require(sha(oracle) == '6fde0d421216597e4e9c3d54574608d10ee6d2f5148cf23c95c3a640a708aaf4', 'Pixel oracle changed')
            gpu_proof = verify(folder / 'driver.json', folder, oracle)
            require(gpu_proof['full4kDiagnosticVerified'] is gpu_proof['sharedHandoffVerified'] is True and gpu_proof['dxgiPresentationVerified'] is False, 'Wrong shader scope')
            for i in (1, 2): require(sha(folder / f'native-copy-{i}.rgba') == sha(lab / old / f'native-copy-{i}.rgba'), 'Original shader pixels changed')
            cleanup = dict(allMappingsRetiredBeforeVmExit=True, originalShaderPixelsByteIdentical=True)
        rows[label] = dict(gpu=gpu_proof, cleanup=cleanup, privateReceiptHashes=files(folder))
    source_inventory = json.dumps(lr['actualGoLauncherSourceHashes'], sort_keys=True, separators=(',', ':')).encode()
    return dict(schema=1, scope='graphics argument composition; actual launcher argument preservation; original hidden GPU regressions',
        nativeSha256=native['bridgeSha256'], defaultNativeSha256=native['defaultBridgeSha256'],
        compiledWindowsInputHashes=inputs, changedWindowsInputs=sorted(delta), launcherProof=proof,
        launcherSourcesUnchanged=430, launcherSourceInventorySha256=hashlib.sha256(source_inventory).hexdigest(),
        launcherPrivateReceiptSha256=sha(launcher / 'receipt.json'), launcherCompilerReceiptSha256=sha(compiler / 'recovered-build-receipt-v1.json'),
        strictPortableControls=35, regressions=rows, byteGuardsChanged=False,
        launcherActivationImplemented=False, launcherProcessHandoffVerified=False, normalGuestBootVerified=False,
        officialInputImplementationChanged=False, liveSdlRefactorRegressionVerified=False,
        performanceMeasured=False, dailyUseAccepted=False, retainedReceiptCommandFailure=True,
        publicSourceHashes={name: sha(gpu / name) for name in ('qemu_graphics_arguments.h', 'qemu_graphics_arguments_test.cpp',
            'qemu_launcher_arguments_test.cpp', 'qemu_runtime_host.h', 'launcher-args-probe/export_test.go',
            'test_launcher_graphics_arguments.py', 'verify_graphics_launch_arguments.py')})


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--lab', type=Path, required=True); p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    result = audit(args.lab.resolve(strict=True))
    with args.output.open('x', encoding='utf-8', newline='\n') as f: json.dump(result, f, indent=2); f.write('\n')
    print('PASS: actual launcher argument preservation and unchanged-image native GPU regressions; process handoff unverified')
