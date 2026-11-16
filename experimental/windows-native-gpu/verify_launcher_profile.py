"""Audit original-argument wiring and CPU binding tests; new-profile VM use stays unverified."""
import argparse
import json
from pathlib import Path
from verify_background_async_waits import load, sha, QEMU, KERNEL
from verify_4k_capture import require, verify
from verify_graphics_launch_arguments import closure
from verify_launcher_lifetime import files
from vulkan_compute_evidence import validate_compute_controls, validate_compute_cleanup, validate_compute_receipts
from vulkan_gpu_pressure_evidence import validate_pressure_receipts, validate_pressure_cleanup, validate_admission_queries, validate_pressure_metadata
from background_shader_evidence import validate_background_controls
from broker_runtime_evidence import broker_aware_initialization_complete

NATIVE = '6676589f019a57b0265287d511473e73b2d9cfc5051378c13d7dcdd29528cc96'
DEFAULT = '3fb82452dc0fe4a9865807800415157478d30edbbfc08456282b9f6b9d0539d9'
GO = '678419eca67f08f9604c3ca087a23f4313fba30849ab8d57ce251e3de96d5278'


def audit(lab):
    gpu = Path(__file__).resolve().parent; repo = gpu.parent.parent
    prior_file = repo / 'docs/evidence/GO-NATIVE-GPU-SUPERVISION-2026-10-29.json'
    require(sha(prior_file) == 'c44ef3a590d364b5fb5db9837514de092e577c84fa925ccdd6bc88e18c59c82c', 'Prior Go proof differs')
    prior = load(prior_file)
    native_prior_file = repo / 'docs/evidence/NATIVE-LAUNCHER-LIFETIME-2026-10-29.json'
    require(sha(native_prior_file) == '5e706cdd4f29c3c30b23d71fa03697cda67649ac815e0eab4a4a98a1ebeec076', 'Prior native proof differs')
    native_prior = load(native_prior_file)
    native = load(lab / 'native-v125-receipt.json'); frozen = lab / 'native-v125-source-frozen'
    require(native['bridgeSha256'] == sha(lab / 'native-v125/driver-bridge.exe') == NATIVE and
        native['defaultBridgeSha256'] == sha(lab / 'native-default-v125/driver-bridge.exe') == DEFAULT, 'Wrong native binaries')
    for name, digest in native['sources'].items(): require(sha(frozen / name) == digest, 'Frozen native source differs')
    current = closure(repo)
    require(current == closure(frozen), 'Current native compiled input differs')
    old = native_prior['compiledWindowsInputHashes']
    delta = {name for name in current.keys() | old.keys() if current.get(name) != old.get(name)}
    require(delta == {'experimental/windows-native-gpu/driver_bridge.cpp', 'experimental/windows-native-gpu/qemu_runtime_host.h',
        'experimental/windows-native-gpu/binary_capture.h', 'experimental/windows-native-gpu/launcher_lifetime.h',
        'experimental/windows-native-gpu/launcher_profile.h'}, 'Unexpected native compiled change')
    for label, key in (('native-v125', 'buildLogSha256'), ('native-default-v125', 'defaultBuildLogSha256')):
        path = lab / ('build-' + label + '.log'); text = path.read_text()
        require(sha(path) == native[key] and 'LAUNCHER_PROFILE_PASS checks=30 ' in text and
            'LAUNCHER_LIFETIME_PASS checks=37 ' in text and 'OWNED_QEMU_PROCESS_PASS checks=48 ' in text, 'Strict native controls missing')
    for name in ('launcher_profile_test.cpp', 'launcher_binding_fixture.cpp'):
        require(sha(gpu / name) == native['sources']['experimental/windows-native-gpu/' + name], 'Tested native fixture differs')
    compiled = lab / 'go-native-owner-v8'; go = load(compiled / 'build-receipt.json')
    require(go['binarySha256'] == sha(compiled / 'launcher-owner.test.exe') == GO and
        go['goSha256'] == sha(lab.parent / 'toolchains/go/bin/go.exe'), 'Wrong Go binary/toolchain')
    actual = {str(p.relative_to(repo / 'app')): sha(p) for p in (repo / 'app').rglob('*') if p.is_file() and p.suffix in ('.go', '.mod', '.sum')}
    require(actual == go['actualGoLauncherSourceHashes'] and len(actual) == 434, 'Current/build Go sources differ')
    for name, digest in actual.items(): require(sha(compiled / 'source-frozen' / name) == digest, 'Frozen Go source differs')
    old_go = prior['goLauncherSourceHashes']
    require(set(actual) - set(old_go) == {'native_gpu_profile_windows.go', 'native_gpu_profile_windows_test.go'} and
        set(old_go) - set(actual) == set() and {n for n in old_go if actual[n] != old_go[n]} ==
        {'native_gpu_owner_windows.go', 'native_gpu_owner_windows_test.go'}, 'Unexpected Go file change')
    require(actual['main.go'] == old_go['main.go'] and (compiled / 'vet.log').is_file() and (compiled / 'vet.log').stat().st_size == 0,
        'Main supervision/input boundary changed or Windows vet failed')
    log = (compiled / 'unit-tests.log').read_text()
    for name in ('TestNativeEnvironmentSnapshot', 'TestNativeLauncherProfileEncoding', 'TestNativeGPUIdentityProtocol',
        'TestNativeGPUCancelledBeforeStart', 'TestSupervisedGuestDefaultProcess', 'TestNativeGPURejectAutomaticOwnerKill',
        'TestAcceptQEMUBridgeServesOnlyQEMUWithinTheLimit', 'TestLifecycleConnectionWaitsForPublishedQEMUPID'):
        require('--- PASS: ' + name + ' ' in log, 'Go CPU control missing')
    cpu_dir = lab / 'launcher-original-profile-cpu-v2'; cpu = load(cpu_dir / 'receipt.json')
    require(cpu['proof'] == dict(actualGoProfiles=6, accepted=3, rejected=3, unicodeRoundtrip=True,
        originalBootInputPreserved=True, qemuStarted=False, gpu=False, normalGuestBootVerified=False) and
        cpu['earlyBindingCpuModes'] == 5 and cpu['exactGoFlagsReachedLifetimeValidation'] is True and
        cpu['nativeOriginalArgumentVmTested'] is cpu['normalGuestBootVerified'] is cpu['dailyUseAccepted'] is False, 'CPU proof scope changed')
    expected_binaries = dict(go_host=GO, native_probe=sha(lab / 'native-v125/launcher-profile-test.exe'),
        binding_fixture=sha(lab / 'native-v125/launcher-binding-fixture.exe'), bridge=NATIVE, default_bridge=DEFAULT)
    require(cpu['binaryHashes'] == expected_binaries and cpu['verifierSha256'] == sha(gpu / 'test_launcher_profile.py') and
        cpu['exportedProfilesSha256'] == sha(cpu_dir / 'profiles.json') and cpu['goExportLogSha256'] == sha(cpu_dir / 'go-export.log') and
        cpu['bindingFixtureLogSha256'] == sha(cpu_dir / 'binding-fixture.log'), 'CPU cross-language source/artifact differs')
    fixture_log = (cpu_dir / 'binding-fixture.log').read_text()
    for mode in ('ready', 'exit-before-ready', 'cancel-before-ready', 'bad-ready', 'duplicate-bound'):
        require('--- PASS: TestNativeLauncherBindingFixture/' + mode + ' ' in fixture_log, 'Binding transition/lifecycle control missing')
    require(len(cpu['cliChecks']) == 8 and all(r['rejected'] is True and r['qemuStarted'] is False for r in cpu['cliChecks']) and
        cpu['fullProfileInvalidCapabilityLogSha256'] == sha(cpu_dir / 'full-profile-invalid-capability.log'), 'CLI control scope differs')
    for row in cpu['cliChecks']:
        require(sha(cpu_dir / (row['profile'] + '-' + str(row['index']) + '.log')) == row['logSha256'], 'CLI log differs')
    regressions = {}
    for label in ('pressure', 'compute', 'texture'):
        folder = lab / ('background-native-launcher-profile-' + label + '-run-2')
        previous = lab / ('background-go-native-owner-' + label + '-run-5')
        require({**files(previous), 'go-owner.json': sha(previous / 'go-owner.json'), 'go-adapter.json': sha(previous / 'go-adapter.json')} ==
            prior['regressions'][label]['privateReceiptHashes'], 'Prior hardware regression differs')
        s, r, old_s = load(folder / 'source.json'), load(folder / 'driver.json'), load(previous / 'source.json')
        observed, adapter = load(folder / 'go-owner.json'), load(folder / 'go-adapter.json')
        log = (folder / 'driver.log').read_text()
        records = [json.loads(line) for line in (folder / 'driver.host.log').read_text().splitlines() if line.startswith('{')]
        image = lab / ('background-vulkan-gpu-admission-' + label + '-v1/private-vulkan.cpio.gz')
        require(s['nativeSha256'] == r['driverBridgeSha256'] == NATIVE and s['nativeBuildReceiptSha256'] == sha(lab / 'native-v125-receipt.json') and
            s['goBuildReceiptSha256'] == sha(compiled / 'build-receipt.json') and adapter['goExecutableSha256'] == GO, 'Wrong hardware-test artifacts')
        require(s['initramfsSha256'] == r['initramfsSha256'] == old_s['initramfsSha256'] == sha(image) and
            s['packReceiptSha256'] == old_s['packReceiptSha256'] == sha(image.parent / 'pack-receipt.json'), 'Original guest image changed')
        require(s['readOnlyInstalledDriverBefore'] == s['readOnlyInstalledDriverAfter'] == old_s['readOnlyInstalledDriverAfter'] and
            s['harnessUnchanged'] is s['artifactsUnchanged'] is True and r['qemuSha256'] == QEMU and r['kernelSha256'] == KERNEL and
            r['hypervisor'] == 'QEMU/WHPX' and r['liveNvidiaLinuxUmdLoaded'] is True, 'Driver/kernel/VM provenance differs')
        require(s['harnessSha256'] == adapter['adapterSha256'] == sha(gpu / 'go_owner_runtime_qemu.py') and
            all(sha(gpu / n) == digest for n, digest in s['harnessComponents'].items()), 'Historical harness/adapter changed')
        require(s['nativeOriginalArgumentsEnabled'] is s['newProfileVmAcceptance'] is False, 'Legacy run promoted to new-profile hardware proof')
        require(s['runnerSha256'] == sha(lab / 'run-background-launcher-profile-regression-v2.py'), 'Regression runner differs')
        command, before = list(s['command']), list(old_s['command'])
        for option in ('--bridge', '--report', '--go-owner-test', '--go-owner-receipt'):
            require(command.count(option) == before.count(option) == 1, 'Missing regression option')
            command[command.index(option)+1] = before[before.index(option)+1]
        require(command == before, 'Original GPU regression options changed')
        expected = list(load(previous / 'go-adapter.json')['originalNativeCommand']); expected[0] = str(lab / 'native-v125/driver-bridge.exe'); expected[6] = str(folder / 'driver.log')
        require(adapter['originalNativeCommand'] == expected and observed['actualNativeCommand'][:-2] == expected and
            observed['actualNativeCommand'][-2] == '--driver-launcher-cancel-handle' and '--driver-launcher-profile' not in expected, 'Native launch boundary changed')
        require(observed['publishedQemuPid'] == observed['identity']['qemuPid'] != observed['bridgePid'] and
            observed['actualGoCreatorPid'] == adapter['goCreatorPid'] == observed['identity']['launcherPid'] and
            observed['qemuReapedThroughHeldHandle'] is observed['nativeEventClosed'] is observed['wrongNativeOwnerStopPidRejected'] is True and
            observed['nativeExit'] == observed['qemuExit'] == r['hostBridgeExit'] == 0, 'Go identity/held exit regression failed')
        if label == 'pressure':
            validate_compute_controls(s, log, records, 256); cleanup = validate_pressure_cleanup(r, records); proof = validate_pressure_receipts(log)
            validate_admission_queries(records, r['disconnectCleanup'], True); validate_pressure_metadata(records, r['disconnectCleanup'])
        elif label == 'compute':
            validate_compute_controls(s, log, records, 256); cleanup = validate_compute_cleanup(r, records, 256, 'pass'); proof = validate_compute_receipts(log, 'pass')
        else:
            validate_background_controls(s, log, records, 10000)
            require(broker_aware_initialization_complete(log, r['hostBridgeExit'], r['ownedQemuControl'], r['disconnectCleanup']), 'Shader cleanup failed')
            oracle = lab / 'native-v111/rgba-oracle.exe'; require(sha(oracle) == '6fde0d421216597e4e9c3d54574608d10ee6d2f5148cf23c95c3a640a708aaf4', 'Pixel oracle differs')
            proof = verify(folder / 'driver.json', folder, oracle)
            require(proof['full4kDiagnosticVerified'] is proof['sharedHandoffVerified'] is True and proof['dxgiPresentationVerified'] is False, 'Wrong shader scope')
            for i in (1, 2): require(sha(folder / f'native-copy-{i}.rgba') == sha(previous / f'native-copy-{i}.rgba'), 'Original shader pixels changed')
            cleanup = dict(allMappingsRetiredBeforeVmExit=True, originalShaderPixelsByteIdentical=True)
        regressions[label] = dict(gpu=proof, cleanup=cleanup, newLauncherProfileSelected=False,
            privateReceiptHashes={**files(folder), 'go-owner.json': sha(folder / 'go-owner.json'), 'go-adapter.json': sha(folder / 'go-adapter.json')})
    earlier = {}
    for label in ('pressure', 'compute', 'texture'):
        folder = lab / ('background-native-launcher-profile-' + label + '-run-1')
        earlier[label] = dict(privateReceiptHashes={**files(folder), 'go-owner.json': sha(folder / 'go-owner.json'), 'go-adapter.json': sha(folder / 'go-adapter.json')}, acceptedAsFinal=False)
    pre_binding = load(lab / 'native-v124-receipt.json')
    require(pre_binding['bridgeSha256'] == sha(lab / 'native-v124/driver-bridge.exe') and
        pre_binding['defaultBridgeSha256'] == sha(lab / 'native-default-v124/driver-bridge.exe'), 'Earlier native build changed')
    earlier_go = {}
    for version in (6, 7):
        path = lab / ('go-native-owner-v' + str(version)); receipt = load(path / 'build-receipt.json')
        require(receipt['binarySha256'] == sha(path / 'launcher-owner.test.exe'), 'Earlier Go build changed')
        earlier_go[str(version)] = dict(binarySha256=receipt['binarySha256'], receiptSha256=sha(path / 'build-receipt.json'), acceptedAsFinal=False)
    return dict(schema=1, scope='original argument runtime wiring, CPU process binding and unchanged legacy GPU workloads',
        nativeSha256=NATIVE, defaultNativeSha256=DEFAULT, goBinarySha256=GO, compiledWindowsInputHashes=current,
        changedWindowsInputs=sorted(delta), goLauncherSourceHashes=actual, existingMainAndInputHandlersUnchanged=True,
        nativeBuildReceiptSha256=sha(lab / 'native-v125-receipt.json'), goBuildReceiptSha256=sha(compiled / 'build-receipt.json'),
        cpuProfileReceiptSha256=sha(cpu_dir / 'receipt.json'), actualGoProfileCases=6, compatibleProfiles=3, unsupportedProfilesRejected=3,
        earlyBindingHelperModes=5, helperQmpReadinessSynthetic=True, earlyExitMemoryClassifierChecked=True,
        originalArgumentModeImplemented=True, originalQmpRolesAndCursorPreservedInCpuChecks=True, unicodeArgumentRoundtripVerified=True,
        regressions=regressions, earlierLegacyGpuRegressionsRetained=earlier,
        preBindingBuildRetained=dict(nativeSha256=pre_binding['bridgeSha256'], nativeReceiptSha256=sha(lab / 'native-v124-receipt.json'),
            earlierGoBuilds=earlier_go, cpuReceiptSha256=sha(lab / 'launcher-original-profile-cpu-v1/receipt.json'), acceptedAsFinal=False),
        nativeOriginalArgumentVmTested=False, normalGuestBootVerified=False, userActivationEnabled=False, windowsOpened=0,
        hostInputInjected=False, sourceDiskAttached=False, performanceMeasured=False, dailyUseAccepted=False,
        publicSourceHashes={n: sha(gpu / n) for n in ('launcher_profile.h', 'launcher_profile_test.cpp', 'launcher_binding_fixture.cpp', 'test_launcher_profile.py', 'verify_launcher_profile.py')})


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__); parser.add_argument('--lab', type=Path, required=True); parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args(); result = audit(args.lab.resolve(strict=True))
    with args.output.open('x', encoding='utf-8', newline='\n') as stream: json.dump(result, stream, indent=2); stream.write('\n')
    print('PASS: original-argument CPU wiring/binding and full legacy GPU regressions; new-profile VM/normal boot unverified')
