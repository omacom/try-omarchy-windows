"""Audit the Go supervisor boundary without opening a VM or redistributing proprietary bytes."""
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
from verify_launcher_lifetime import files, NATIVE

GO_BINARY = '519c246fa6d2b30ca22162b98eefa31ef9e94c80a7b15e92747815271e5d2c3a'


def check_main_delta(path, original_hash):
    text = path.read_text(encoding='utf-8')
    changes = [
        ('\t// Prepared native GPU ownership is internal until normal guest/runtime\n\t// integration passes. The released settings leave this nil.\n\tnativeGPU nativeGPUPrepare\n', ''),
        ('\tvar guest *supervisedGuest\n', ''),
        ('guest, err = startSupervisedGuest(setupContext(), proc, cfg.nativeGPU)', 'err = proc.Start()'),
        ('publishSupervisedGuest(guest)', 'qemuPid.Store(uint32(proc.Process.Pid))'),
        ('err := guest.wait()', 'err := proc.Wait()'),
        ('\t\t\t\tguest.kill()', '\t\t\t\tproc.Process.Kill()'),
        ('\t\t\tguest.kill()', '\t\t\tproc.Process.Kill()'),
        ('\t\t\t_ = stopSupervisedGuest(pid)', '\t\t\tif p, err := os.FindProcess(int(pid)); err == nil {\n\t\t\t\tp.Kill()\n\t\t\t}')]
    for current, previous in changes:
        require(text.count(current) == 1, 'Supervisor delta differs: ' + current[:55])
        text = text.replace(current, previous, 1)
    import hashlib
    require(original_hash in {hashlib.sha256(text.encode()).hexdigest(), hashlib.sha256(text.replace('\n', '\r\n').encode()).hexdigest()},
        'Changes outside the exact supervisor start/publish/wait/stop boundary')


def audit(lab):
    gpu = Path(__file__).resolve().parent; repo = gpu.parent.parent
    prior_file = repo / 'docs/evidence/NATIVE-LAUNCHER-LIFETIME-2026-10-29.json'
    require(sha(prior_file) == '5e706cdd4f29c3c30b23d71fa03697cda67649ac815e0eab4a4a98a1ebeec076', 'Prior lifetime proof differs')
    prior = load(prior_file)
    require(closure(repo) == prior['compiledWindowsInputHashes'] and sha(lab / 'native-v123/driver-bridge.exe') == NATIVE,
        'Native runtime changed during Go integration')
    compiled = lab / 'go-native-owner-v5'; build = load(compiled / 'build-receipt.json')
    require(build['binarySha256'] == sha(compiled / 'launcher-owner.test.exe') == GO_BINARY and
        build['goSha256'] == sha(lab.parent / 'toolchains/go/bin/go.exe') and build['builderSha256'] == sha(lab / 'build-go-native-owner-v5.py'), 'Go artifact/toolchain differs')
    current = {str(p.relative_to(repo / 'app')): sha(p) for p in (repo / 'app').rglob('*') if p.is_file() and p.suffix in ('.go', '.mod', '.sum')}
    require(current == build['actualGoLauncherSourceHashes'] and len(current) == 432, 'Compiled/current Go inputs differ')
    for name, digest in current.items(): require(sha(compiled / 'source-frozen' / name) == digest, 'Frozen Go source differs')
    original = load(lab / 'launcher-real-arguments-v1/receipt.json')['actualGoLauncherSourceHashes']
    require(len(original) == 430 and set(current) - set(original) == {'native_gpu_owner_windows.go', 'native_gpu_owner_windows_test.go'} and
        set(original) - set(current) == set(), 'Unexpected launcher files added/removed')
    require({name for name, digest in original.items() if current[name] != digest} == {'main.go'}, 'Original input/other launcher files changed')
    check_main_delta(repo / 'app/main.go', original['main.go'])
    unit_log = (compiled / 'unit-tests.log').read_text()
    require((compiled / 'vet.log').is_file() and (compiled / 'vet.log').stat().st_size == 0, 'Successful Windows vet log missing')
    for name in ('TestNativeGPUIdentityProtocol', 'TestNativeGPUCancelledBeforeStart', 'TestSupervisedGuestDefaultProcess',
            'TestNativeGPURejectAutomaticOwnerKill', 'TestAcceptQEMUBridgeServesOnlyQEMUWithinTheLimit', 'TestLifecycleConnectionWaitsForPublishedQEMUPID'):
        require('--- PASS: ' + name + ' ' in unit_log, 'Go protocol/default/peer control missing')
    regressions = {}
    for label in ('pressure', 'compute', 'texture'):
        folder = lab / ('background-go-native-owner-' + label + '-run-5')
        previous = lab / ('background-native-launcher-lifetime-' + label + '-run-1')
        require({**files(previous), 'lifetime-control.json': sha(previous / 'lifetime-control.json')} ==
            prior['regressions'][label]['privateReceiptHashes'], 'Prior GPU control differs')
        s, r, old_s = load(folder / 'source.json'), load(folder / 'driver.json'), load(previous / 'source.json')
        go, adapter = load(folder / 'go-owner.json'), load(folder / 'go-adapter.json')
        log = (folder / 'driver.log').read_text()
        records = [json.loads(line) for line in (folder / 'driver.host.log').read_text().splitlines() if line.startswith('{')]
        require(s['nativeSha256'] == r['driverBridgeSha256'] == NATIVE and r['qemuSha256'] == QEMU and r['kernelSha256'] == KERNEL and
            r['hypervisor'] == 'QEMU/WHPX' and r['liveNvidiaLinuxUmdLoaded'] is True, 'GPU/native provenance differs')
        image = lab / ('background-vulkan-gpu-admission-' + label + '-v1/private-vulkan.cpio.gz')
        require(s['initramfsSha256'] == r['initramfsSha256'] == old_s['initramfsSha256'] == sha(image) and
            s['packReceiptSha256'] == old_s['packReceiptSha256'] == sha(image.parent / 'pack-receipt.json'), 'Original image changed')
        require(s['readOnlyInstalledDriverBefore'] == s['readOnlyInstalledDriverAfter'] == old_s['readOnlyInstalledDriverAfter'] and
            s['harnessUnchanged'] is s['artifactsUnchanged'] is True, 'Runtime/driver guard differs')
        require(s['harnessSha256'] == adapter['adapterSha256'] == sha(gpu / 'go_owner_runtime_qemu.py') and
            all(sha(gpu / n) == digest for n, digest in s['harnessComponents'].items()), 'Historical harness or Go adapter differs')
        require(s['goBuildReceiptSha256'] == sha(compiled / 'build-receipt.json') and
            s['runnerSha256'] == sha(lab / 'run-background-go-owner-v4.py') and adapter['goExecutableSha256'] == GO_BINARY, 'Wrong compiled Go/run control')
        command, before = list(s['command']), list(old_s['command'])
        for option in ('--go-owner-test', '--go-owner-receipt'):
            require(command.count(option) == 1, 'Missing Go integration option')
            i = command.index(option); del command[i:i + 2]
        i = before.index('--lifetime-control-report'); del before[i:i + 2]
        command[1] = before[1]
        command[command.index('--report') + 1] = before[before.index('--report') + 1]
        require(command == before, 'Existing GPU options changed')
        require(adapter['parentHarnessCommand'] == s['command'][1:-4] and
            adapter['actualGoCommand'] == [str(compiled / 'launcher-owner.test.exe'), '-test.run=^TestNativeGPUProcessHost$'], 'Go launch boundary differs')
        expected = list(load(previous / 'lifetime-control.json')['actualNativeCommand'])[:-2]
        expected[6] = str(folder / 'driver.log')
        require(adapter['originalNativeCommand'] == expected and go['actualNativeCommand'][:-2] == expected and
            go['actualNativeCommand'][-2] == '--driver-launcher-cancel-handle' and go['actualNativeCommand'][-1].isdigit(), 'Native GPU command changed')
        identity = go['identity']
        validate_ready(identity, go['bridgePid'], adapter['goCreatorPid'], int(identity['launcherCreationTime100ns']),
            identity['qemuPid'], int(identity['qemuCreationTime100ns']), go['bridgePid'])
        require(go['actualGoCreatorPid'] == adapter['goCreatorPid'] == identity['launcherPid'] and
            go['publishedQemuPid'] == identity['qemuPid'] != go['bridgePid'] and go['qemuReapedThroughHeldHandle'] is go['nativeEventClosed'] is
            go['parentOutputEntryClosedAfterStart'] is go['wrongNativeOwnerStopPidRejected'] is True and go['qemuExit'] == go['nativeExit'] == r['hostBridgeExit'] == 0 and
            go['cancellationRequested'] is go['normalLauncherGuestBootVerified'] is go['dailyUseAccepted'] is False, 'Wrong Go identity/exit/publication scope')
        if label == 'pressure':
            validate_compute_controls(s, log, records, 256); cleanup = validate_pressure_cleanup(r, records); proof = validate_pressure_receipts(log)
            validate_admission_queries(records, r['disconnectCleanup'], True); validate_pressure_metadata(records, r['disconnectCleanup'])
        elif label == 'compute':
            validate_compute_controls(s, log, records, 256); cleanup = validate_compute_cleanup(r, records, 256, 'pass'); proof = validate_compute_receipts(log, 'pass')
        else:
            validate_background_controls(s, log, records, 10000)
            require(broker_aware_initialization_complete(log, r['hostBridgeExit'], r['ownedQemuControl'], r['disconnectCleanup']), 'Shader lifetime/cleanup failed')
            oracle = lab / 'native-v111/rgba-oracle.exe'
            require(sha(oracle) == '6fde0d421216597e4e9c3d54574608d10ee6d2f5148cf23c95c3a640a708aaf4', 'Pixel oracle differs')
            proof = verify(folder / 'driver.json', folder, oracle)
            require(proof['full4kDiagnosticVerified'] is proof['sharedHandoffVerified'] is True and proof['dxgiPresentationVerified'] is False, 'Wrong shader scope')
            for i in (1, 2): require(sha(folder / f'native-copy-{i}.rgba') == sha(previous / f'native-copy-{i}.rgba'), 'Original shader pixels changed')
            cleanup = dict(allMappingsRetiredBeforeVmExit=True, originalShaderPixelsByteIdentical=True)
        regressions[label] = dict(gpu=proof, cleanup=cleanup, publishedActualQemuPid=True, heldQemuExitVerified=True,
            privateReceiptHashes={**files(folder), 'go-owner.json': sha(folder / 'go-owner.json'), 'go-adapter.json': sha(folder / 'go-adapter.json')})
    stops = {}
    for mode in ('cancel', 'parent-exit'):
        folder = lab / ('background-go-native-owner-' + mode + '-run-2'); r = load(folder / 'receipt.json')
        for name, digest in r['fileHashes'].items(): require(sha(folder / name) == digest, 'Stop evidence changed')
        require(r['goBinarySha256'] == GO_BINARY and r['goBuildReceiptSha256'] == sha(compiled / 'build-receipt.json') and
            r['nativeSha256'] == NATIVE and r['testSourceSha256'] == sha(lab / 'test-go-native-owner-stop-v2.py'), 'Stop binary/source differs')
        require(r['readOnlyInstalledDriverBefore'] == r['readOnlyInstalledDriverAfter'] and r['nativeVmExited'] is True and
            r['vmExit'] == 90 and r['nativeExit'] == 1 and r['actualGoExit'] == 1 and r['guestGraphicsContextCreated'] is False, 'Stop/early workload scope differs')
        require(r['stopAcknowledgement'] == dict(nativeLauncherStopAcknowledged=True, reason='launcher-cancelled' if mode == 'cancel' else 'launcher-exited',
            guestExited=True, driverCleanupComplete=False), 'Stop reason/cleanup scope differs')
        raw = [json.loads(line) for line in (folder / 'native.stderr.log').read_text().splitlines() if line.startswith('{')]
        summaries = [row for row in raw if 'driverCleanupVerified' in row]
        require(len(summaries) == 1 and summaries[0]['driverCleanupVerified'] is True and summaries[0]['cleanupFailures'] == 0 and
            summaries[0]['nativeSubmissionAttempts'] == summaries[0]['completedNativeSubmissions'] == 0, 'Early stop cleanup/work scope differs')
        require(all(type(value) is int and value == 0 for key, value in summaries[0].items() if key.startswith(('live', 'failed', 'pending'))),
            'Early stop left native work or resources')
        if mode == 'cancel':
            observed = r['identityObservation']; identity = observed['identity']
            require(observed == load(folder / 'go-owner.json') and observed['qemuReapedThroughHeldHandle'] is observed['nativeEventClosed'] is
                observed['parentOutputEntryClosedAfterStart'] is observed['wrongNativeOwnerStopPidRejected'] is observed['cancellationRequested'] is True and observed['qemuExit'] == 90 and
                observed['publishedQemuPid'] == identity['qemuPid'] != observed['bridgePid'], 'Go stop route bypassed verified native ownership')
        else:
            require(r['creatorInterruptedWhileVmLive'] is True and r['identityObservation']['heldProcessIdentityVerified'] is True and
                not (folder / 'go-owner.json').exists(), 'Parent-death control finished normally')
        stops[mode] = dict(verified=True, qemuExit=90, nativeExit=1, nativeStopReason=r['stopAcknowledgement']['reason'],
            separateNativeCleanupSummaryVerified=True, noGpuSubmissions=True,
            midGpuCrashRecoveryVerified=False, privateReceiptHashes={**r['fileHashes'], 'receipt.json': sha(folder / 'receipt.json')})
    failure = lab / 'background-go-native-owner-pressure-run-1'
    require('SyntaxError: invalid syntax' in (failure / 'console.log').read_text() and not (failure / 'driver.json').exists() and
        not (failure / 'go-adapter.json').exists(), 'Prelaunch adapter syntax failure lost')
    earlier = {}
    for version in (1, 2, 3, 4):
        folder = lab / ('go-native-owner-v' + str(version)); receipt = load(folder / 'build-receipt.json')
        require(sha(folder / 'launcher-owner.test.exe') == receipt['binarySha256'], 'Earlier Go build changed')
        earlier[str(version)] = dict(binarySha256=receipt['binarySha256'], buildReceiptSha256=sha(folder / 'build-receipt.json'), acceptedAsFinal=False)
    earlier_gpu = {}
    for version, labels in ((2, ('pressure',)), (3, ('pressure', 'compute', 'texture')), (4, ('pressure', 'compute', 'texture'))):
        for label in labels:
            folder = lab / ('background-go-native-owner-' + label + '-run-' + str(version))
            earlier_gpu[str(version) + '-' + label] = dict(privateReceiptHashes={**files(folder), 'go-owner.json': sha(folder / 'go-owner.json'),
                'go-adapter.json': sha(folder / 'go-adapter.json')}, acceptedAsFinal=False)
    return dict(schema=1, scope='Go supervisor start/publish/stop/wait boundary; no normal guest activation',
        goBinarySha256=GO_BINARY, goBuildReceiptSha256=sha(compiled / 'build-receipt.json'), goLauncherSourceHashes=current,
        originalLauncherFilesUnchanged=429, changedOriginalLauncherFiles=['main.go'], exactSupervisorDeltaVerified=True,
        originalArgumentEnvironmentAndInputHandlersPreserved=True, goUnitLogSha256=sha(compiled / 'unit-tests.log'), windowsVetLogSha256=sha(compiled / 'vet.log'),
        nativeCompiledInputsUnchanged=True, nativeSha256=NATIVE, regressions=regressions, earlyLifecycleControls=stops,
        prelaunchAdapterFailure=dict(logSha256=sha(failure / 'console.log'), qemuStarted=False, accepted=False),
        earlierGoBuildsRetained=earlier, earlierGpuControlsRetained=earlier_gpu,
        windowsOpened=0, hostInputInjected=False, sourceDiskAttached=False, supervisorBoundaryConnected=True,
        normalGuestBootVerified=False, nativeArgumentProfileImplemented=False, userActivationEnabled=False,
        liveSdlRegressionVerified=False, midGpuCrashRecoveryVerified=False, performanceMeasured=False, dailyUseAccepted=False,
        publicSourceHashes={n: sha(gpu / n) for n in ('go_owner_runtime_qemu.py', 'verify_go_native_owner.py')})


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__); parser.add_argument('--lab', type=Path, required=True); parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args(); result = audit(args.lab.resolve(strict=True))
    with args.output.open('x', encoding='utf-8', newline='\n') as stream: json.dump(result, stream, indent=2); stream.write('\n')
    print('PASS: Go supervisor PID/cancellation/log ownership, original GPU oracles and cleanup; normal activation unverified')
