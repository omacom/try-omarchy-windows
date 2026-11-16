"""File-only proof of verified process ownership and unchanged GPU workloads."""
import argparse
import json
from pathlib import Path
from verify_background_async_waits import load, sha, QEMU, KERNEL
from verify_4k_capture import require, verify
from verify_graphics_launch_arguments import closure
from vulkan_compute_evidence import validate_compute_controls, validate_compute_cleanup, validate_compute_receipts
from vulkan_gpu_pressure_evidence import validate_pressure_receipts, validate_pressure_cleanup, validate_admission_queries, validate_pressure_metadata
from background_shader_evidence import validate_background_controls
from broker_runtime_evidence import broker_aware_initialization_complete

NATIVE = '88e9b7994c8e8fd46f8b29b1c811d93a11ec816fe1a0c0ef574ed64490f6cc4e'
DEFAULT = '39d9c3467c3cffa30b08cc7cee4090c183fd1b4da49ee92b408e2d37b4fd0c82'


def audit(lab):
    gpu = Path(__file__).resolve().parent; repo = gpu.parent.parent
    prior_file = repo / 'docs/evidence/LAUNCHER-GRAPHICS-ARGUMENTS-2026-10-29.json'
    require(sha(prior_file) == 'da198642893142c4fba88c81b204be07a95b20ad27c78ff58e033dacf9764a7a', 'Prior proof differs')
    prior = load(prior_file)
    frozen = lab / 'native-v121-source-frozen'; build = load(lab / 'native-v121-receipt.json')
    require(build['bridgeSha256'] == sha(lab / 'native-v121/driver-bridge.exe') == NATIVE and
        build['defaultBridgeSha256'] == sha(lab / 'native-default-v121/driver-bridge.exe') == DEFAULT, 'Native executable differs')
    for name, digest in build['sources'].items(): require(sha(frozen / name) == digest, 'Frozen native source differs')
    current = closure(frozen)
    require(all(sha(repo / name) == digest for name, digest in current.items()), 'Current compiled input differs')
    old = prior['compiledWindowsInputHashes']
    delta = {name for name in current.keys() | old.keys() if current.get(name) != old.get(name)}
    require(delta == {'experimental/windows-native-gpu/qemu_runtime_host.h', 'experimental/windows-native-gpu/owned_qemu_process.h'}, 'Unexpected compiled input change')
    require(sha(gpu / 'owned_qemu_process_test.cpp') == build['sources']['experimental/windows-native-gpu/owned_qemu_process_test.cpp'], 'Ownership controls differ')
    marker = ('OWNED_QEMU_PROCESS_PASS checks=48 rejectedCandidatesUntouched=true heldIdentity=true naturalExit=true forcedExit=true '
        'duplicateAttachRejected=true cancelledAttachRejected=true qemu=false gpu=false input=false windows=0')
    for label, key in (('native-v121', 'buildLogSha256'), ('native-default-v121', 'defaultBuildLogSha256')):
        path = lab / ('build-' + label + '.log')
        require(sha(path) == build[key] and marker in path.read_text(), 'Strict ownership controls/build missing')
    launcher = load(lab / 'launcher-real-arguments-v1/receipt.json')
    require(sha(lab / 'launcher-real-arguments-v1/receipt.json') == prior['launcherPrivateReceiptSha256'], 'Previous launcher check differs')
    actual = {str(p.relative_to(repo / 'app')): sha(p) for p in (repo / 'app').rglob('*') if p.is_file() and p.suffix in ('.go', '.mod', '.sum')}
    require(actual == launcher['actualGoLauncherSourceHashes'] and len(actual) == 430, 'Launcher source changed')
    require(sha(gpu / 'qemu_graphics_arguments.h') == prior['publicSourceHashes']['qemu_graphics_arguments.h'], 'Graphics composer changed')
    require(sha(gpu / 'test_owned_runtime_qemu.py') == 'e6c29c6cf1ee42d93c8c3e858c4d685237a902d05218dbe840533c2069f03286', 'Legacy harness changed')
    require(sha(lab / 'qemu-sdl-user-network-v1/bin/qemu-system-x86_64.exe') == QEMU and
        sha(lab.parent / 'compositor-buffer-lab/kernel-7.2.8/vmlinuz-linux') == KERNEL, 'QEMU/kernel differs')
    def files(folder):
        return {n: sha(folder / n) for n in ('source.json', 'driver.json', 'driver.log', 'driver.host.log', 'driver.errors.log')}
    rows = {}
    for label, name, baseline in (
        ('pressure', 'background-native-owned-process-run-2', 'background-native-graphics-arguments-run-1'),
        ('compute', 'background-native-owned-process-compute-run-2', 'background-native-graphics-arguments-compute-run-1'),
        ('texture', 'background-native-owned-process-texture-run-2', 'background-native-graphics-arguments-texture-run-1')):
        folder, previous = lab / name, lab / baseline
        require(files(previous) == prior['regressions'][label]['privateReceiptHashes'], 'Prior accepted control changed')
        s, r, old_s = load(folder / 'source.json'), load(folder / 'driver.json'), load(previous / 'source.json')
        log = (folder / 'driver.log').read_text()
        records = [json.loads(line) for line in (folder / 'driver.host.log').read_text().splitlines() if line.startswith('{')]
        image = lab / ('background-vulkan-gpu-admission-' + label + '-v1/private-vulkan.cpio.gz')
        require(s['nativeSha256'] == r['driverBridgeSha256'] == NATIVE and s['nativeBuildReceiptSha256'] == sha(lab / 'native-v121-receipt.json'), 'Wrong host artifact')
        require(s['initramfsSha256'] == r['initramfsSha256'] == old_s['initramfsSha256'] == sha(image) and
            s['packReceiptSha256'] == old_s['packReceiptSha256'] == sha(image.parent / 'pack-receipt.json'), 'Guest image/pack differs')
        require(s['readOnlyInstalledDriverBefore'] == s['readOnlyInstalledDriverAfter'] == old_s['readOnlyInstalledDriverAfter'] and
            s['readOnlyInstalledDriverAfter']['installedRuntimeMatchesSnapshot'] is True and
            s['harnessUnchanged'] is s['artifactsUnchanged'] is True, 'Driver/runtime guard differs')
        require(r['qemuSha256'] == QEMU and r['kernelSha256'] == KERNEL and r['hypervisor'] == 'QEMU/WHPX' and r['liveNvidiaLinuxUmdLoaded'] is True and
            r['capturedPrivateFixturesUsed'] is r['privateRuntimeImageRedistributable'] is r['realWslDxgForwarding'] is False, 'GPU provenance differs')
        require(s['harnessSha256'] == old_s['harnessSha256'] == sha(gpu / 'broker_runtime_qemu.py') and
            s['harnessComponents'] == old_s['harnessComponents'] and all(sha(gpu / n) == digest for n, digest in s['harnessComponents'].items()), 'Harness differs')
        command, before = list(s['command']), list(old_s['command'])
        for option in ('--bridge', '--report'):
            require(command.count(option) == before.count(option) == 1, 'Missing launch option boundary')
            command[command.index(option)+1] = before[before.index(option)+1]
        require(command == before, 'GPU regression options differ')
        if label == 'pressure':
            validate_compute_controls(s, log, records, 256)
            cleanup = validate_pressure_cleanup(r, records)
            proof = validate_pressure_receipts(log)
            validate_admission_queries(records, r['disconnectCleanup'], True)
            validate_pressure_metadata(records, r['disconnectCleanup'])
        elif label == 'compute':
            validate_compute_controls(s, log, records, 256)
            cleanup = validate_compute_cleanup(r, records, 256, 'pass')
            proof = validate_compute_receipts(log, 'pass')
        else:
            validate_background_controls(s, log, records, 10000)
            require(broker_aware_initialization_complete(log, r['hostBridgeExit'], r['ownedQemuControl'], r['disconnectCleanup']), 'Shader handoff/cleanup failed')
            oracle = lab / 'native-v111/rgba-oracle.exe'
            require(sha(oracle) == '6fde0d421216597e4e9c3d54574608d10ee6d2f5148cf23c95c3a640a708aaf4', 'Original pixel oracle differs')
            proof = verify(folder / 'driver.json', folder, oracle)
            require(proof['full4kDiagnosticVerified'] is proof['sharedHandoffVerified'] is True and proof['dxgiPresentationVerified'] is False, 'Wrong shader scope')
            for i in (1, 2): require(sha(folder / f'native-copy-{i}.rgba') == sha(previous / f'native-copy-{i}.rgba'), 'Original shader image changed')
            cleanup = dict(allMappingsRetiredBeforeVmExit=True, originalShaderPixelsByteIdentical=True)
        rows[label] = dict(gpu=proof, cleanup=cleanup, privateReceiptHashes=files(folder))
    superseded_file = lab / 'owned-qemu-process-audit-v1.json'
    require(sha(superseded_file) == 'a23fc5710cdef69dfc6e2d5e75961c5d04b159c693ca69c2381fb29cd12fb389', 'Pre-correction control evidence differs')
    superseded = load(superseded_file)
    require(superseded['nativeSha256'] == sha(lab / 'native-v120/driver-bridge.exe') == load(lab / 'native-v120-receipt.json')['bridgeSha256'], 'Pre-correction native differs')
    old_inputs = closure(lab / 'native-v120-source-frozen')
    require({n for n in current.keys() | old_inputs.keys() if current.get(n) != old_inputs.get(n)} ==
        {'experimental/windows-native-gpu/owned_qemu_process.h'}, 'Correction changed unrelated native input')
    for label, name in (('pressure', 'background-native-owned-process-run-1'), ('compute', 'background-native-owned-process-compute-run-1'),
        ('texture', 'background-native-owned-process-texture-run-1')):
        require(files(lab / name) == superseded['regressions'][label]['privateReceiptHashes'], 'Pre-correction GPU controls changed')
    return dict(schema=1, scope='held Windows process identity and job ownership; unchanged hidden GPU regressions',
        nativeSha256=NATIVE, defaultNativeSha256=DEFAULT, compiledWindowsInputHashes=current, changedWindowsInputs=sorted(delta),
        ownershipChecks=48, unavailableFileIdsAndNulPathsRejected=True, nativeAndDefaultBuildControlsPassed=True, wrongClaimsLeaveCandidatesUntouched=True,
        duplicateAndCancelledAttachRejected=True, heldProcessIdentityVerified=True, executableFileIdentityChecked=True,
        liveCreatorCheckedAtAdoption=True, inheritedProcessHandle=False, naturalAndForcedHelperExitsReaped=True,
        actualOsPidReuseInduced=False, launcherControlProtocolImplemented=False, launcherParentDeathRecoveryVerified=False,
        nativeRuntimeUsesVerifiedAdoption=True, launcherSourcesUnchanged=430, regressions=rows,
        byteGuardsChanged=False, officialInputImplementationChanged=False, normalGuestBootVerified=False,
        liveSdlRegressionVerified=False, performanceMeasured=False, dailyUseAccepted=False,
        preCorrectionControl=dict(receiptSha256=sha(superseded_file), nativeSha256=superseded['nativeSha256'],
            gpuControlsRetained=True, unavailableIdentityRefusalImplemented=False, acceptedAsFinal=False),
        publicSourceHashes={n: sha(gpu / n) for n in ('owned_qemu_process.h', 'owned_qemu_process_test.cpp', 'qemu_runtime_host.h', 'verify_owned_qemu_process.py')})


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--lab', type=Path, required=True); p.add_argument('--output', type=Path, required=True)
    args = p.parse_args(); result = audit(args.lab.resolve(strict=True))
    with args.output.open('x', encoding='utf-8', newline='\n') as f: json.dump(result, f, indent=2); f.write('\n')
    print('PASS: held process ownership, real hidden child controls and unchanged GPU regressions; launcher handoff unverified')
