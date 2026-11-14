"""File-only audit of the controlled native buffer / Dozen FD matrix."""
import argparse
import json
from pathlib import Path
from verify_background_async_waits import NATIVE, QEMU, KERNEL, sha, load, cpio_hashes
from verify_4k_capture import require
from vulkan_compute_evidence import validate_compute_controls, validate_compute_receipts, validate_compute_cleanup
from vulkan_memory_fd_evidence import validate_fd_receipts, validate_fd_cleanup
from background_shader_evidence import validate_background_controls
from broker_runtime_evidence import broker_aware_initialization_complete
from verify_4k_capture import verify

ORIGINAL_ICD = '4dbfee33f92941341c175b03b337f5df4355f114178e9ea0d2adfce1edb07f17'
CANDIDATE_ICD = 'f6281a549c4ceb5199aacadd1d7bd967da668b630498c3d1e07d1fae5240a755'
BUFFER_NATIVE = 'bee0db047f574cfc5c3b0964b2b466c92bfce13802c3cb32b5e4f839df5a9903'


def audit_texture_regression(lab, gpu):
    folder = lab / 'background-gpu-shader-run-4'
    old = lab / 'background-gpu-shader-run-3'
    build = lab / 'background-gpu-shader-v1'
    build_receipt = load(build / 'build-receipt.json')
    for group in ('sources', 'binaries'):
        for name, digest in build_receipt[group].items(): require(sha(build / name) == digest, 'Original texture probe changed')
    source, report = load(folder / 'source.json'), load(folder / 'driver.json')
    require(source['profile'] == '4k-sampled' and source['harnessUnchanged'] is source['artifactsUnchanged'] is True and
        source['harnessSha256'] == sha(gpu / 'broker_runtime_qemu.py') and
        source['runnerSha256'] == sha(lab / 'run-background-gpu-shader-v3.py') and
        source['nativeBuildReceiptSha256'] == sha(lab / 'native-v112-receipt.json') and
        source['buildReceiptSha256'] == sha(build / 'build-receipt.json'), 'Shader regression provenance differs')
    for name, digest in source['harnessComponents'].items(): require(sha(gpu / name) == digest, 'Shader harness component differs')
    require(source['readOnlyInstalledDriverBefore'] == source['readOnlyInstalledDriverAfter'] and
        source['readOnlyInstalledDriverAfter']['installedRuntimeMatchesSnapshot'] is True, 'Shader NVIDIA guard failed')
    require(report['driverBridgeSha256'] == source['nativeSha256'] == BUFFER_NATIVE and
        report['qemuSha256'] == QEMU and report['kernelSha256'] == KERNEL and
        report['initramfsSha256'] == source['initramfsSha256'] == sha(build / 'private-4k-sampled.cpio.gz') ==
        build_receipt['images']['4k-sampled'], 'Shader runtime/image differs')
    require(report['diagnosticAccepted'] is False and source['generalVerifierExit'] == 1 and
        report['liveNvidiaLinuxUmdLoaded'] is True and report['realWslDxgForwarding'] is False and
        report['capturedPrivateFixturesUsed'] is report['privateRuntimeImageRedistributable'] is False, 'Shader scope differs')
    old_source = load(old / 'source.json')
    a, b = list(source['command']), list(old_source['command'])
    for command in (a, b):
        for option in ('--bridge', '--report'): command[command.index(option) + 1] = '<' + option[2:] + '>'
    require(a == b and source['initramfsSha256'] == old_source['initramfsSha256'], 'Shader regression changed more than native/report paths')
    guest = (folder / 'driver.log').read_text()
    records = [json.loads(line) for line in (folder / 'driver.errors.log').read_text().splitlines() if line.startswith('{')]
    clean, vm = report['disconnectCleanup'], report['ownedQemuControl']
    require(broker_aware_initialization_complete(guest, report['hostBridgeExit'], vm, clean), 'Shader initialization/lifetime incomplete')
    for section in (clean, vm):
        for key, value in section.items():
            if key.startswith(('failed', 'live', 'pending', 'forced')) or key.endswith(('Timeouts', 'ReleasedAfterVmExit')):
                require(type(value) is int and value == 0, 'Shader failure or late retirement: ' + key)
    require(clean['sharedBufferProbes'] == clean['importedSharedBuffers'] == clean['failedSharedBufferProbes'] == 0 and
        clean['cpuStoreTestRequested'] is False and clean['completedCpuStoreTests'] == 0, 'Unexpected shader buffer/CPU diagnostic')
    validate_background_controls(source, guest, records, 2)
    oracle = lab / 'native-v111/rgba-oracle.exe'
    require(sha(oracle) == '6fde0d421216597e4e9c3d54574608d10ee6d2f5148cf23c95c3a640a708aaf4', 'Original pixel oracle changed')
    result = verify(folder / 'driver.json', folder, oracle)
    require(result['full4kDiagnosticVerified'] is result['sharedHandoffVerified'] is True and
        result['dxgiPresentationVerified'] is False, 'Shader/texture regression failed')
    for index in (1, 2):
        require(sha(folder / f'native-copy-{index}.rgba') == sha(old / f'native-copy-{index}.rgba'), 'Native bridge changed shader frame bytes')
    return dict(run=4, originalShaderImageAndOracleUnchanged=True, originalNativeFrameBytesIdentical=True,
        frames=result['frames'], sharedHandoffVerified=True, windowPresentationVerified=False,
        allNativeMappingsRetiredBeforeVmExit=True, nativeSubmissions=clean['completedNativeSubmissions'],
        privateReceiptHashes={name: sha(folder / name) for name in ('source.json', 'driver.json', 'driver.log', 'driver.errors.log')})


def audit(lab):
    gpu = Path(__file__).resolve().parent
    repo = gpu.parent.parent
    native = load(lab / 'native-v112-receipt.json')
    require(native['bridgeSha256'] == sha(lab / 'native-v112/driver-bridge.exe') == BUFFER_NATIVE and
        native['defaultBridgeSha256'] == sha(lab / 'native-default-v112/driver-bridge.exe') and
        native['textureValidationChanged'] is native['byteGuardsChanged'] is False, 'Native build differs')
    for name, digest in native['sources'].items():
        require(sha(lab / 'native-v112-source-frozen' / name) == digest, 'Frozen native source changed')
        if name.endswith(('.h', '.cpp', '/build.ps1')):
            require(sha(repo / name) == digest, 'Public native build input differs: ' + name)
    require(native['buildLogSha256'] == sha(lab / 'build-native-v112.log') and
        native['defaultBuildLogSha256'] == sha(lab / 'build-native-default-v112.log'), 'Native build logs differ')
    icd_root = lab / 'mesa-dozen-fd-v1'
    icd_build = load(icd_root / 'build-receipt.json')
    manifest = load(gpu / 'mesa/dozen-memory-fd-patch.json')
    require(icd_build['originalIcdSha256'] == sha(lab / 'background-vulkan-v1/build/src/microsoft/vulkan/libvulkan_dzn.so') == ORIGINAL_ICD and
        icd_build['icdSha256'] == sha(icd_root / 'build/src/microsoft/vulkan/libvulkan_dzn.so') == CANDIDATE_ICD and
        icd_build['sourceArchiveSha256'] == manifest['archiveSha256'] and
        icd_build['patchedSourceSha256'] == sha(icd_root / 'mesa-26.0.8/src/microsoft/vulkan/dzn_device.c') ==
        manifest['files']['src/microsoft/vulkan/dzn_device.c']['patchedSha256'], 'ICD source/build differs')
    for name, digest in icd_build['patchHashes'].items():
        require(sha(gpu / 'mesa' / name) == sha(icd_root / 'patch-source' / name) == digest, 'Dozen patch differs')
    require(icd_build['sdkTreeHashesSha256'] == sha(icd_root / 'sdk-source-hashes.json'), 'SDK hash manifest changed')
    sdk = load(icd_root / 'sdk-source-hashes.json')
    require(len(sdk) == icd_build['sdkFiles'], 'SDK source count differs')
    for name, digest in sdk.items():
        require(sha(icd_root / 'mesa-26.0.8/subprojects/DirectX-Headers-1.0' / name) == digest, 'SDK source changed')
    probe_root = lab / 'vulkan-memory-fd-probe-v1'
    probe = load(probe_root / 'build-receipt.json')
    require(sha(probe_root / 'vulkan-memory-fd-probe') == probe['probeSha256'], 'FD probe binary changed')
    for name, digest in probe['sourceHashes'].items():
        require(sha(gpu / name) == sha(probe_root / 'source-frozen' / name) == digest, 'FD probe source changed')
    original = lab / 'background-vulkan-compute-v1'
    base_image = original / 'private-vulkan-compute.cpio.gz'
    base = cpio_hashes(base_image)
    icd_name = probe['guestIcdPath'].lstrip('/')
    require(base[icd_name] == ORIGINAL_ICD, 'Original guest ICD lookup path differs')
    snapshot = load(lab / 'nvidia-runtime-62101-snapshot-v1/receipt.json')
    require(snapshot['signedInstalledDriver'] is snapshot['sourceDriverUnchanged'] is True and
        snapshot['driverVersion'] == '32.0.16.2101', 'Unverified NVIDIA snapshot')
    for name, metadata in snapshot['files'].items():
        require([digest for path, digest in base.items() if path.endswith('/' + name)] == [metadata['sha256']], 'NVIDIA image runtime changed')
    folders = {'memory-baseline': 'background-vulkan-memory-fd-v1', 'memory-candidate': 'background-vulkan-memory-fd-v2',
        'compute-candidate': 'background-vulkan-fd-regression-v1'}
    packs = {}
    for profile, name in folders.items():
        folder = lab / name
        packed = load(folder / 'pack-receipt.json')
        image = folder / 'private-vulkan.cpio.gz'
        require(packed['privateOnly'] is True and packed['redistributable'] is packed['sourceDiskAttached'] is
            packed['presentationRequested'] is packed['inputRequested'] is packed['genericInitializationOracleApplicable'] is False and
            packed['sourceImageUnchanged'] is True, 'Wrong pack scope')
        require(packed['baselineImageSha256'] == sha(base_image) and
            packed['baselinePackReceiptSha256'] == sha(original / 'pack-receipt.json') and
            packed['initramfsSha256'] == sha(image), 'Pack/image changed')
        components = packed['componentReceipts']
        if profile != 'compute-candidate':
            require(components['probeBuildReceiptSha256'] == sha(probe_root / 'build-receipt.json'), 'Pack probe receipt differs')
        if profile != 'memory-baseline':
            require(components['candidateIcdBuildReceiptSha256'] == sha(icd_root / 'build-receipt.json'), 'Pack ICD receipt differs')
        payload = cpio_hashes(image)
        changes = {}
        if profile != 'compute-candidate': changes['guest-probe'] = probe['probeSha256']
        if profile != 'memory-baseline': changes[icd_name] = CANDIDATE_ICD
        require(set(payload) == set(base) and {key: payload[key] for key in base if payload[key] != base[key]} == changes,
            'Unexpected controlled image payload changes')
        require(packed['changedPayloads'] == [dict(name=key, originalSha256=base[key], candidateSha256=value)
            for key, value in changes.items()], 'CPIO changes differ from receipt')
        packs[profile] = packed
    rows, commands = [], []
    matrix = ((1, 'memory-baseline', 'buffer-refusal', NATIVE), (2, 'memory-candidate', 'buffer-refusal', NATIVE),
        (3, 'compute-candidate', 'pass', NATIVE), (4, 'memory-candidate', 'pass', BUFFER_NATIVE),
        (5, 'compute-candidate', 'pass', BUFFER_NATIVE), (6, 'memory-candidate', 'pass', BUFFER_NATIVE),
        (7, 'memory-baseline', 'bad-export', BUFFER_NATIVE))
    for number, profile, outcome, native_hash in matrix:
        run = lab / f'background-vulkan-fd-run-{number}'
        source, report = load(run / 'source.json'), load(run / 'driver.json')
        log = (run / 'driver.log').read_text()
        records = [json.loads(line) for line in (run / 'driver.host.log').read_text().splitlines() if line.startswith('{')]
        require(source['harnessUnchanged'] is source['artifactsUnchanged'] is True and
            source['readOnlyInstalledDriverBefore'] == source['readOnlyInstalledDriverAfter'] and
            source['readOnlyInstalledDriverAfter']['installedRuntimeMatchesSnapshot'] is True and
            source['readOnlyInstalledDriverAfter']['driverVersion'] == snapshot['driverVersion'], 'Runtime guard changed/failed')
        runner = 'run-background-vulkan-fd-v1.py' if number < 4 else 'run-background-vulkan-fd-v2.py'
        require(source['runnerSha256'] == sha(lab / runner) and
            source['packReceiptSha256'] == sha(lab / folders[profile] / 'pack-receipt.json'), 'Runner/pack changed')
        if number >= 4: require(source['nativeBuildReceiptSha256'] == sha(lab / 'native-v112-receipt.json'), 'Native receipt changed')
        require(source['harnessSha256'] == sha(gpu / 'broker_runtime_qemu.py'), 'Harness changed')
        for name, digest in source['harnessComponents'].items(): require(sha(gpu / name) == digest, 'Frozen harness component changed')
        actual = 'vulkan-compute' if profile == 'compute-candidate' else 'vulkan-memory-fd'
        require(source['actualWorkload'] == actual and source['profile'] == profile and
            source['genericHarnessWorkload'] == 'init' and source['genericInitializationOracleCoversFdControls'] is False and
            report['diagnosticAccepted'] is False and source['generalVerifierExit'] == 1, 'Generic rejection/scope differs')
        require(report['driverBridgeSha256'] == source['nativeSha256'] == native_hash and report['qemuSha256'] == QEMU and
            report['kernelSha256'] == KERNEL and report['initramfsSha256'] == source['initramfsSha256'] ==
            packs[profile]['initramfsSha256'], 'Native/QEMU/kernel/image differs')
        require(report['liveNvidiaLinuxUmdLoaded'] is True and report['realWslDxgForwarding'] is False and
            report['capturedPrivateFixturesUsed'] is report['privateRuntimeImageRedistributable'] is False and
            report['hypervisor'] == 'QEMU/WHPX', 'Missing genuine GPU provenance')
        validate_compute_controls(source, log, records, 256)  # isolation/slots/broker only; no shader inference
        if profile == 'compute-candidate':
            proof = validate_compute_receipts(log, 'pass')
            cleanup = validate_compute_cleanup(report, records, 256, 'pass')
        else:
            proof = validate_fd_receipts(log, outcome)
            cleanup = validate_fd_cleanup(report, records, outcome)
        command = list(source['command'])
        for option in ('--report', '--initramfs', '--bridge'): command[command.index(option) + 1] = '<' + option[2:] + '>'
        commands.append(command)
        rows.append(dict(run=number, profile=profile, outcome=outcome, proof=proof, cleanup=cleanup,
            nativeSha256=native_hash, initramfsSha256=source['initramfsSha256'], originalGenericAcceptance=False,
            privateReceiptHashes={name: sha(run / name) for name in ('source.json', 'driver.json', 'driver.log', 'driver.host.log', 'driver.errors.log')}))
    require(all(command == commands[0] for command in commands), 'Controlled commands changed beyond native/image/report paths')
    return dict(schema=1, scope='64 KiB Vulkan FD export/lifetime and bounded Windows buffer admission; separate 4K compute regressions',
        runs=rows, textureRegression=audit_texture_regression(lab, gpu), candidateIcdSha256=CANDIDATE_ICD,
        originalIcdSha256=ORIGINAL_ICD, nativeSha256=BUFFER_NATIVE,
        qemuSha256=QEMU, kernelSha256=KERNEL, cpuByteGuardUnchanged=True, gpuByteGuardUnchanged=True,
        textureAcceptanceUnchanged=True, controlledGuestIcdLookupPathAlias=True, successfulFdRepeats=2,
        originalIcdExportFailureRetained=True, oldNativeBufferRefusalRetained=True,
        actualHardwareBufferBytes=65536, actualHardwareBufferFlags=0,
        gpuPayloadImportVerified=False, gpuPayloadContentsVerified=False, vulkanRasterizationVerified=False,
        windowPresentationVerified=False, deviceLossRecoveryVerified=False, performanceMeasured=False, dailyUseAccepted=False,
        publicSourceHashes={name: sha(gpu / name) for name in ('shared_buffer_profile.h', 'shared_buffer_profile_test.cpp',
            'shared_resource_probe.h', 'driver_bridge.cpp', 'vulkan_memory_fd_probe.cpp', 'vulkan_memory_fd_workload.h',
            'vulkan_memory_fd_evidence.py', 'verify_vulkan_memory_fds.py')})


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lab', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path, help='New sanitized file; never overwrite')
    args = parser.parse_args()
    result = audit(args.lab.resolve())
    with args.output.open('x', encoding='utf-8', newline='\n') as output:
        json.dump(result, output, indent=2); output.write('\n')
    print('PASS: two 32-FD lifetime repeats; retained original-ICD/native refusals; two 4K compute regressions. No desktop acceptance.')
