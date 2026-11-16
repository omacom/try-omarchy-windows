"""File-only audit of native lifetime control, actual identity and original GPU oracles."""
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
from launcher_lifetime_runtime_qemu import validate_ready

NATIVE = 'b103948323f952ebf540b726561c606619b6f6a660e649a8cf8a5aec57065d5b'
DEFAULT = '1805032dd038965fd1d17bd63cf8dd4ff9f3bbc568766ff304e4bbd26424ec69'


def files(folder, names=('source.json', 'driver.json', 'driver.log', 'driver.host.log', 'driver.errors.log')):
    return {n: sha(folder / n) for n in names}


def audit(lab):
    gpu = Path(__file__).resolve().parent; repo = gpu.parent.parent
    prior_file = repo / 'docs/evidence/QEMU-PROCESS-OWNERSHIP-2026-10-29.json'
    require(sha(prior_file) == 'aff15f05b317e249649510d140227adea5555241b3cdd98516c0208eefcefb47', 'Prior ownership proof differs')
    prior = load(prior_file)
    frozen = lab / 'native-v123-source-frozen'; build = load(lab / 'native-v123-receipt.json')
    require(build['bridgeSha256'] == sha(lab / 'native-v123/driver-bridge.exe') == NATIVE and
        build['defaultBridgeSha256'] == sha(lab / 'native-default-v123/driver-bridge.exe') == DEFAULT, 'Native executable differs')
    for name, digest in build['sources'].items(): require(sha(frozen / name) == digest, 'Frozen native source differs')
    current = closure(frozen)
    require(all(sha(repo / name) == digest for name, digest in current.items()), 'Current compiled input differs')
    old = prior['compiledWindowsInputHashes']
    delta = {name for name in current.keys() | old.keys() if current.get(name) != old.get(name)}
    require(delta == {'experimental/windows-native-gpu/driver_bridge.cpp', 'experimental/windows-native-gpu/qemu_runtime_host.h',
        'experimental/windows-native-gpu/owned_qemu_process.h', 'experimental/windows-native-gpu/launcher_lifetime.h'}, 'Unexpected compiled input change')
    for name in ('launcher_lifetime_test.cpp', 'test_launcher_lifetime_cli.py'):
        require(sha(gpu / name) == build['sources']['experimental/windows-native-gpu/' + name], 'Tested lifetime control differs')
    for label, key in (('native-v123', 'buildLogSha256'), ('native-default-v123', 'defaultBuildLogSha256')):
        path = lab / ('build-' + label + '.log'); text = path.read_text()
        require(sha(path) == build[key] and 'LAUNCHER_LIFETIME_PASS checks=37 ' in text and 'OWNED_QEMU_PROCESS_PASS checks=48 ' in text,
            'Strict ownership/lifetime build controls missing')
        require((lab / ('launcher-lifetime-cli-' + label + '.log')).read_text().strip() ==
            'PASS 13 launcher lifetime CLI controls; no VM/GPU/window/input', 'Lifetime CLI controls missing')
    launcher_file = lab / 'launcher-real-arguments-v1/receipt.json'; launcher = load(launcher_file)
    argument_proof_file = repo / 'docs/evidence/LAUNCHER-GRAPHICS-ARGUMENTS-2026-10-29.json'
    require(sha(argument_proof_file) == 'da198642893142c4fba88c81b204be07a95b20ad27c78ff58e033dacf9764a7a', 'Argument proof changed')
    require(sha(launcher_file) == load(argument_proof_file)['launcherPrivateReceiptSha256'], 'Previous launcher check differs')
    actual = {str(p.relative_to(repo / 'app')): sha(p) for p in (repo / 'app').rglob('*') if p.is_file() and p.suffix in ('.go', '.mod', '.sum')}
    require(actual == launcher['actualGoLauncherSourceHashes'] and len(actual) == 430, 'Launcher source changed')
    require(sha(gpu / 'test_owned_runtime_qemu.py') == 'e6c29c6cf1ee42d93c8c3e858c4d685237a902d05218dbe840533c2069f03286', 'Legacy harness changed')
    require(sha(lab / 'qemu-sdl-user-network-v1/bin/qemu-system-x86_64.exe') == QEMU and
        sha(lab.parent / 'compositor-buffer-lab/kernel-7.2.8/vmlinuz-linux') == KERNEL, 'QEMU/kernel differs')
    regressions = {}
    for label, previous_name in (('pressure', 'background-native-owned-process-run-2'),
            ('compute', 'background-native-owned-process-compute-run-2'), ('texture', 'background-native-owned-process-texture-run-2')):
        folder = lab / ('background-native-launcher-lifetime-' + label + '-run-1'); previous = lab / previous_name
        require(files(previous) == prior['regressions'][label]['privateReceiptHashes'], 'Prior GPU evidence changed')
        s, r, old_s = load(folder / 'source.json'), load(folder / 'driver.json'), load(previous / 'source.json')
        life = load(folder / 'lifetime-control.json')
        log = (folder / 'driver.log').read_text()
        records = [json.loads(line) for line in (folder / 'driver.host.log').read_text().splitlines() if line.startswith('{')]
        require(s['nativeSha256'] == r['driverBridgeSha256'] == NATIVE and s['nativeBuildReceiptSha256'] == sha(lab / 'native-v123-receipt.json'), 'Wrong native artifact')
        image = lab / ('background-vulkan-gpu-admission-' + label + '-v1/private-vulkan.cpio.gz')
        require(s['initramfsSha256'] == r['initramfsSha256'] == old_s['initramfsSha256'] == sha(image) and
            s['packReceiptSha256'] == old_s['packReceiptSha256'] == sha(image.parent / 'pack-receipt.json'), 'Guest image differs')
        require(s['readOnlyInstalledDriverBefore'] == s['readOnlyInstalledDriverAfter'] == old_s['readOnlyInstalledDriverAfter'] and
            s['readOnlyInstalledDriverAfter']['installedRuntimeMatchesSnapshot'] is True and
            s['harnessUnchanged'] is s['artifactsUnchanged'] is True, 'Driver/runtime guard differs')
        require(r['qemuSha256'] == QEMU and r['kernelSha256'] == KERNEL and r['hypervisor'] == 'QEMU/WHPX' and
            r['liveNvidiaLinuxUmdLoaded'] is True and r['realWslDxgForwarding'] is r['capturedPrivateFixturesUsed'] is False, 'GPU provenance differs')
        require(s['harnessSha256'] == life['adapterSha256'] == sha(gpu / 'launcher_lifetime_runtime_qemu.py') and
            all(sha(gpu / name) == digest for name, digest in s['harnessComponents'].items()), 'Adapter/harness differs')
        command, before = list(s['command']), list(old_s['command'])
        require(command.count('--lifetime-control-report') == 1, 'Missing explicit lifetime adapter option')
        index = command.index('--lifetime-control-report'); del command[index:index + 2]
        command[1] = before[1]
        for option in ('--bridge', '--report'):
            require(command.count(option) == before.count(option) == 1, 'Missing launch boundary')
            command[command.index(option) + 1] = before[before.index(option) + 1]
        require(command == before, 'Existing GPU options changed')
        validate_ready(life['ready'], life['bridgePid'], life['launcherPid'], int(life['launcherCreationTime100ns']),
            life['ready']['qemuPid'], int(life['ready']['qemuCreationTime100ns']), life['bridgePid'])
        require(life['identityVerified'] is life['qemuImageVerified'] is life['processIdentityHeldUntilExit'] is True and
            life['nativeExit'] == life['qemuExit'] == 0 and life['cancellationRequested'] is False and
            life['explicitInheritedHandleCount'] == 1 and life['launcherFrontendIntegrated'] is life['dailyUseAccepted'] is False, 'Wrong lifetime identity result')
        actual_command = life['actualNativeCommand']
        require(actual_command[-2] == '--driver-launcher-cancel-handle' and actual_command[-1].isdigit() and int(actual_command[-1]) > 0,
            'Actual native command omitted private cancellation capability')
        def value(option): return s['command'][s['command'].index(option) + 1]
        expected_native = [value('--bridge'), '--run-qemu', value('--qemu'), value('--firmware'), value('--kernel'), value('--initramfs'),
            str(Path(value('--report')).with_suffix('.log')), '--driver-contexts', '--driver-queries']
        for option in ('--driver-allocations', '--driver-gpuva', '--driver-residency', '--driver-cpu', '--driver-cpu-slots',
                '--driver-fence-slots', '--driver-translation', '--driver-hwqueues', '--driver-syncs', '--driver-reservation',
                '--driver-gpu-state', '--driver-submit', '--driver-retirement', '--driver-async-submit', '--driver-shared-resources',
                '--driver-consume-shared', '--driver-native-metadata'):
            if option in s['command']:
                expected_native.append(option)
                if option.endswith('-slots'): expected_native.append(value(option))
        require(actual_command == expected_native + actual_command[-2:], 'Actual native GPU options changed')
        require(life['parentHarnessCommand'] == s['command'][1:-2] and
            s['runnerSha256'] == sha(lab / 'run-background-launcher-lifetime-v1.py'), 'Recorded harness/runner boundary differs')
        require(not any(row.get('nativeLauncherStopAcknowledged') is True for row in records), 'Natural control unexpectedly cancelled')
        if label == 'pressure':
            validate_compute_controls(s, log, records, 256)
            cleanup = validate_pressure_cleanup(r, records); proof = validate_pressure_receipts(log)
            validate_admission_queries(records, r['disconnectCleanup'], True); validate_pressure_metadata(records, r['disconnectCleanup'])
        elif label == 'compute':
            validate_compute_controls(s, log, records, 256)
            cleanup = validate_compute_cleanup(r, records, 256, 'pass'); proof = validate_compute_receipts(log, 'pass')
        else:
            validate_background_controls(s, log, records, 10000)
            require(broker_aware_initialization_complete(log, r['hostBridgeExit'], r['ownedQemuControl'], r['disconnectCleanup']), 'Shader cleanup/handoff failed')
            oracle = lab / 'native-v111/rgba-oracle.exe'
            require(sha(oracle) == '6fde0d421216597e4e9c3d54574608d10ee6d2f5148cf23c95c3a640a708aaf4', 'Original pixel oracle differs')
            proof = verify(folder / 'driver.json', folder, oracle)
            require(proof['full4kDiagnosticVerified'] is proof['sharedHandoffVerified'] is True and proof['dxgiPresentationVerified'] is False, 'Wrong shader scope')
            for i in (1, 2): require(sha(folder / f'native-copy-{i}.rgba') == sha(previous / f'native-copy-{i}.rgba'), 'Original shader pixels changed')
            cleanup = dict(allMappingsRetiredBeforeVmExit=True, originalShaderPixelsByteIdentical=True)
        regressions[label] = dict(gpu=proof, cleanup=cleanup, naturalVmIdentityVerified=True,
            privateReceiptHashes={**files(folder), 'lifetime-control.json': sha(folder / 'lifetime-control.json')})
    stops = {}
    for mode in ('cancel', 'parent-exit'):
        folder = lab / ('background-native-launcher-' + mode + '-run-2'); r = load(folder / 'receipt.json')
        for name, digest in r['fileHashes'].items(): require(sha(folder / name) == digest, 'Stop control raw evidence changed')
        require(r['testSourceSha256'] == sha(lab / 'test-native-launcher-lifetime-stop-v2.py') and
            r['adapterSha256'] == sha(gpu / 'launcher_lifetime_runtime_qemu.py') and r['nativeSha256'] == NATIVE, 'Stop control source/artifact differs')
        require(r['readOnlyInstalledDriverBefore'] == r['readOnlyInstalledDriverAfter'] and
            r['readOnlyInstalledDriverAfter']['installedRuntimeMatchesSnapshot'] is True, 'Stop control driver guard differs')
        require(r['actualCreatorIdentityVerified'] is r['heldNativeAndVmIdentityVerified'] is r['vmReaped'] is r['nativeExited'] is True and
            r['qemuExit'] == 90 and r['nativeExit'] == 1 and 0 < r['stopSeconds'] < 15, 'Owned stop/reap failed')
        ready = load(folder / 'ready.json'); helper = load(folder / 'helper-result.json'); message = ready['ready']
        validate_ready(message, message['bridgePid'], ready['launcherPid'], int(ready['launcherCreationTime100ns']),
            message['qemuPid'], int(message['qemuCreationTime100ns']), message['bridgePid'])
        ack = r['stopAcknowledgement']; control = r['finalVmControl']
        require(ack == dict(nativeLauncherStopAcknowledged=True, reason='launcher-cancelled' if mode == 'cancel' else 'launcher-exited',
            guestExited=True, driverCleanupComplete=False), 'Stop acknowledgement promoted driver cleanup')
        require(control['ownedQemuExited'] is control['qemuForcedStop'] is True and control['qemuExit'] == 90 and
            all(control[key] == 0 for key in ('liveFenceMappings', 'liveAllocationMappings', 'liveAllocationMappedBytes', 'liveAllocationSlotMappings',
                'fenceMappingsCreated', 'allocationMappingsCreated')), 'Early stop scope changed')
        require(helper['cancelSignalledByActualCreator'] is (mode == 'cancel'), 'Wrong stop trigger')
        if mode == 'parent-exit': require(helper['nativeLiveBeforeCreatorExit'] is True, 'Creator death did not leave live native owner')
        stops[mode] = dict(stopSeconds=r['stopSeconds'], heldProcessExitVerified=True, qemuExit=90, nativeExit=1,
            nativeStopReason=ack['reason'], noGpuMappingsCreated=True, midGpuCrashRecoveryVerified=False,
            privateReceiptHashes={**r['fileHashes'], 'receipt.json': sha(folder / 'receipt.json')})
    # Retain validation-order and test-environment failures; neither is a GPU acceptance.
    old_cli = lab / 'launcher-lifetime-cli-native-v122.log'
    require('Wrong lifetime rejection' in old_cli.read_text() and '4K lab build requires' in old_cli.read_text(), 'Validation-order failure lost')
    failed = lab / 'background-native-launcher-cancel-run-1'
    require((failed / 'native.stdout.log').read_text() == '' and 'requires WDDM_BRIDGE_CAPTURE_DIRECTORY' in (failed / 'native.stderr.log').read_text(),
        'Missing-environment control failure lost')
    return dict(schema=1, scope='native launcher lifetime and hidden QEMU identity; not normal launcher activation',
        nativeSha256=NATIVE, defaultNativeSha256=DEFAULT, compiledWindowsInputHashes=current, changedWindowsInputs=sorted(delta),
        nativeBuildReceiptSha256=sha(lab / 'native-v123-receipt.json'), lifetimeChecksPerProfile=37, priorOwnershipChecksPerProfile=48,
        malformedCliChecksPerProfile=13, launcherSourcesUnchanged=430, launcherPrivateReceiptSha256=sha(launcher_file),
        inheritedWaitOnlyCancellation=True, creatorProcessHeld=True, readyTimestampPrecisionPreserved=True,
        actualHiddenProcessChainVerified=True, regressions=regressions, earlyLifecycleControls=stops,
        validationOrderFailure=dict(logSha256=sha(old_cli), accepted=False),
        missingEnvironmentFailure=dict(fileHashes=files(failed, ('helper.log', 'native.stdout.log', 'native.stderr.log')), accepted=False, qemuStarted=False),
        sourceDiskAttached=False, windowsOpened=0, hostInputInjected=False, officialInputModified=False,
        launcherFrontendIntegrated=False, normalGuestBootVerified=False, midGpuCrashRecoveryVerified=False,
        liveSdlRegressionVerified=False, performanceMeasured=False, dailyUseAccepted=False,
        publicSourceHashes={n: sha(gpu / n) for n in ('launcher_lifetime.h', 'launcher_lifetime_test.cpp', 'test_launcher_lifetime_cli.py',
            'launcher_lifetime_runtime_qemu.py', 'test_launcher_lifetime_protocol.py', 'verify_launcher_lifetime.py')})


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lab', type=Path, required=True); parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args(); result = audit(args.lab.resolve(strict=True))
    with args.output.open('x', encoding='utf-8', newline='\n') as stream: json.dump(result, stream, indent=2); stream.write('\n')
    print('PASS: native identity, early cancellation/creator exit, original GPU data/pixels and cleanup; frontend activation unverified')
