"""File-only audit of the typed-buffer GPU matrix and original 4K regressions."""
import argparse
import json
from pathlib import Path
import re
from verify_background_async_waits import QEMU, KERNEL, cpio_hashes, load, sha
from verify_4k_capture import require, verify
from broker_runtime_evidence import broker_runtime_complete, broker_aware_initialization_complete
from vulkan_compute_evidence import validate_compute_controls, validate_compute_cleanup, validate_compute_receipts
from vulkan_memory_roundtrip_evidence import validate_receipts, validate_cleanup
from vulkan_typed_buffer_evidence import validate_fd_control, validate_typed_metadata, validate_partial_creator_refusal
from background_shader_evidence import validate_background_controls

NATIVE = 'e1d4f13817d909c073c8403bda307d81196273ce5e3bd353698932fd672c9db0'
SHIM = 'c796255605297652a63429ad9d17033002ee0c7293001654527af94642bfe344'
BROKER = '683a56fd7995dae634c39afda35b6925d9bf8451f8cb31ec0fb8894386ff9af9'
ICD = '54adb229085d9bdf6ad8a9f816ce998c0b304811166465f653a914d9757f9340'
LEGACY = 'e6c29c6cf1ee42d93c8c3e858c4d685237a902d05218dbe840533c2069f03286'


def audit(lab):
    gpu = Path(__file__).resolve().parent
    repo = gpu.parent.parent
    frozen = lab / 'native-v116-source-frozen'
    native = load(lab / 'native-v116-receipt.json')
    require(native['bridgeSha256'] == sha(lab / 'native-v116/driver-bridge.exe') == NATIVE, 'Native executable changed')
    for name, digest in native['sources'].items():
        require(sha(frozen / name) == digest, 'Frozen native source changed')
    # Bind actual Windows translation-unit inputs, rather than treating an
    # unused Linux .cpp or a later parser/test edit as a compiled Windows input.
    inputs = set()
    def includes(path):
        if path in inputs:
            return
        inputs.add(path)
        for name in re.findall(r'^\s*#include "([^"]+)"', path.read_text(encoding='utf-8'), re.M):
            target = path.parent / name
            if target.is_file():
                includes(target)
    includes(frozen / 'experimental/windows-native-gpu/driver_bridge.cpp')
    for path in inputs:
        require(sha(path) == sha(repo / path.relative_to(frozen)), 'Current compiled Windows input differs')
    require(sha(gpu / 'test_owned_runtime_qemu.py') == LEGACY, 'Legacy harness changed')
    shims = lab / 'typed-buffer-shims-v5'
    sr = load(shims / 'build-receipt.json')
    require(all(sr[key] is True for key in ('legacyTransportControlsPassed', 'originalWireControlsPassed',
        'eventfdHandleControlsPassed', 'eventfdPreloadControlsPassed')), 'Guest CPU/preload controls incomplete')
    for name, digest in sr['sourceHashes'].items():
        require(sha(shims / 'source-frozen' / name) == digest, 'Frozen guest source changed')
    require(sha(shims / 'linux-ioctl-bridge-4k-async.so') == SHIM, 'Guest shim changed')
    br = load(lab / 'typed-buffer-broker-4k-v1/receipt.json')
    require(br['highResolutionProfile'] is br['testsRequiredRoot'] is True and br['byteGuardsChanged'] is False and
        br['brokerSha256'] == sha(lab / 'typed-buffer-broker-4k-v1/guest-graphics-broker') == BROKER, 'Broker profile/build differs')
    mesa = lab / 'mesa-dozen-native-buffer-v2'
    mr = load(mesa / 'build-receipt.json')
    require(mr['icdSha256'] == sha(mesa / 'build/src/microsoft/vulkan/libvulkan_dzn.so') == ICD and
        mr['originalIcdUnchanged'] is True and mr['sdkFiles'] == 104, 'Mesa ICD/build scope differs')
    for name, digest in mr['patchHashes'].items():
        require(sha(mesa / 'patch-source' / name) == sha(gpu / 'mesa' / name) == digest, 'Pinned Mesa patch changed')
    manifest = load(gpu / 'mesa/dozen-native-gpu-buffer-patch.json')
    for name, row in manifest['files'].items():
        require(sha(mesa / 'mesa-26.0.8' / name) == row['patchedSha256'], 'Compiled Mesa input changed')
    require(sha(gpu / 'mesa/dzn_wddm_native_gpu_buffer.h') == mr['nativeBufferHeaderSha256'], 'Canonical ICD helper changed')
    snapshot = load(lab / 'nvidia-runtime-62101-snapshot-v1/receipt.json')
    require(snapshot['signedInstalledDriver'] is snapshot['sourceDriverUnchanged'] is True and
        snapshot['driverVersion'] == '32.0.16.2101', 'NVIDIA snapshot scope differs')
    cache = {}
    def payload(path):
        if path not in cache:
            cache[path] = cpio_hashes(path)
        return cache[path]
    def image(label):
        folder = lab / ('background-vulkan-typed-buffer-' + label + '-v2')
        pack = load(folder / 'pack-receipt.json')
        baseline = lab / ('background-vulkan-typed-buffer-' + label + '-v1/private-vulkan.cpio.gz')
        candidate = folder / 'private-vulkan.cpio.gz'
        require(pack['initramfsSha256'] == sha(candidate) and pack['baselineImageSha256'] == sha(baseline) and
            pack['baselinePackReceiptSha256'] == sha(baseline.parent / 'pack-receipt.json'), 'Image lineage differs')
        old, new = payload(baseline), payload(candidate)
        require(old.keys() == new.keys() and {name: new[name] for name in old if old[name] != new[name]} ==
            {'guest-graphics-broker': BROKER}, 'Correction changes more than the broker profile')
        require(new['linux-ioctl-bridge.so'] == SHIM and new['guest-graphics-broker'] == BROKER, 'Wrong image broker/shim')
        if label != 'texture':
            require([value for name, value in new.items() if name.endswith('/libvulkan_dzn.so')] == [ICD], 'Wrong image ICD')
        for name, metadata in snapshot['files'].items():
            require([value for entry, value in new.items() if entry.endswith('/' + name)] == [metadata['sha256']], 'Embedded NVIDIA runtime differs')
        for key in ('redistributable', 'sourceDiskAttached', 'presentationRequested', 'inputRequested'):
            require(pack[key] is False, 'Image scope differs: ' + key)
        require(pack['privateOnly'] is pack['sourceImageUnchanged'] is True and
            pack['typedMetadataOptIn'] is (label not in ('baseline', 'texture')), 'Wrong image opt-in')
        return candidate
    def files(folder):
        return {name: sha(folder / name) for name in ('source.json', 'driver.json', 'driver.log', 'driver.host.log', 'driver.errors.log')}
    def observed(folder):
        source, report = load(folder / 'source.json'), load(folder / 'driver.json')
        log = (folder / 'driver.log').read_text(encoding='utf-8')
        records = [json.loads(line) for line in (folder / 'driver.host.log').read_text(encoding='utf-8').splitlines() if line.startswith('{')]
        require(source['readOnlyInstalledDriverBefore'] == source['readOnlyInstalledDriverAfter'] and source['harnessUnchanged'] is source['artifactsUnchanged'] is True,
            'Run guard changed')
        require(report['capturedPrivateFixturesUsed'] is report['privateRuntimeImageRedistributable'] is report['realWslDxgForwarding'] is False and
            report['liveNvidiaLinuxUmdLoaded'] is True and report['hypervisor'] == 'QEMU/WHPX', 'Actual GPU provenance differs')
        return source, report, log, records
    def current(folder, candidate):
        source, report, log, records = observed(folder)
        require(source['nativeSha256'] == report['driverBridgeSha256'] == NATIVE and
            source['initramfsSha256'] == report['initramfsSha256'] == sha(candidate) and
            report['qemuSha256'] == QEMU and report['kernelSha256'] == KERNEL, 'Runtime binary/image differs')
        require(source['harnessSha256'] == sha(gpu / 'broker_runtime_qemu.py') and all(
            sha(gpu / name) == digest for name, digest in source['harnessComponents'].items()), 'Original harness component differs')
        return source, report, log, records
    rows = []
    for number, label in ((7, 'baseline'), (8, 'normal'), (9, 'import-zero'), (10, 'fd-exhaustion'), (11, 'corrupt-last'), (12, 'normal')):
        folder = lab / f'background-vulkan-typed-buffer-run-{number}'
        source, report, log, records = current(folder, image(label))
        require(report['diagnosticAccepted'] is False and source['generalVerifierExit'] == 1 and
            source['genericInitializationOracleCoversRoundtrip'] is False, 'Generic initialization was promoted')
        validate_compute_controls(source, log, records, 256)
        if label == 'baseline':
            proof = validate_partial_creator_refusal(log, records, report)
            require(not any('nativeGpuBufferMetadataGenerated' in row or 'nativeGpuBufferMetadataApplied' in row for row in records), 'Disabled baseline used native metadata')
            rows.append(dict(run=number, profile=label, gpu=proof, strictBrokerAccepted=True,
                originalGenericAccepted=False, wholeRunAccepted=False, privateReceiptHashes=files(folder)))
            continue
        cleanup = validate_compute_cleanup(report, records, 256, 'pass')
        profile = 'corrupt-last' if label == 'corrupt-last' else 'pass'
        proof = validate_receipts(log, profile)
        fd = validate_fd_control(log, {'import-zero': 'zero', 'fd-exhaustion': 'exhaustion'}.get(label, 'none'), 1 if profile == 'corrupt-last' else 2)
        typed = validate_typed_metadata(records, report['disconnectCleanup'], profile, label != 'baseline')
        rows.append(dict(run=number, profile=label, gpu=proof, fdControl=fd, nativeMetadata=typed,
            cleanup=cleanup, strictBrokerAccepted=True, originalGenericAccepted=False, privateReceiptHashes=files(folder)))
    regressions = {}
    for label in ('compute', 'texture'):
        folder = lab / ('background-vulkan-typed-buffer-' + label + '-run-2')
        source, report, log, records = current(folder, image(label))
        if label == 'compute':
            validate_compute_controls(source, log, records, 256)
            cleanup = validate_compute_cleanup(report, records, 256, 'pass')
            proof = validate_compute_receipts(log, 'pass')
        else:
            validate_background_controls(source, log, records, 10000)
            require(broker_aware_initialization_complete(log, report['hostBridgeExit'], report['ownedQemuControl'], report['disconnectCleanup']), 'Texture initialization/retirement incomplete')
            oracle = lab / 'native-v111/rgba-oracle.exe'
            require(sha(oracle) == '6fde0d421216597e4e9c3d54574608d10ee6d2f5148cf23c95c3a640a708aaf4', 'Independent pixel oracle differs')
            proof = verify(folder / 'driver.json', folder, oracle)
            require(proof['full4kDiagnosticVerified'] is proof['sharedHandoffVerified'] is True and proof['dxgiPresentationVerified'] is False, '4K shader scope differs')
            for index in (1, 2):
                require(sha(folder / f'native-copy-{index}.rgba') == sha(lab / f'background-gpu-shader-run-1/native-copy-{index}.rgba'), 'Original GPU shader pixels differ')
            cleanup = dict(allMappingsRetiredBeforeVmExit=True, originalShaderPixelsByteIdentical=True)
        regressions[label] = dict(proof=proof, cleanup=cleanup, privateReceiptHashes=files(folder))
    # Retain the actual failed profile correction controls. A natural VM exit
    # alone did not establish their live mapping/queue cleanup or GPU rendering.
    rejected_profiles = {}
    for label in ('compute', 'texture'):
        folder = lab / ('background-vulkan-typed-buffer-' + label + '-run-1')
        _, report, log, _ = observed(folder)
        require(not broker_runtime_complete(log) and 'reason=Native CPU lock receipt: Protocol error' in log and
            'BRIDGE_RUNTIME_EXIT=125' in log and report['disconnectCleanup']['hwQueuesReleasedAfterVmExit'] == 7,
            'Failed small-profile regression erased')
        rejected_profiles[label] = dict(accepted=False, strictBrokerAccepted=False, guestExit=125,
            rejection='small broker build rejected native CPU mapping extent', privateReceiptHashes=files(folder))
    retained = {}
    for number, profile in ((1, 'map-refusal'), (2, 'pass'), (3, 'pass'), (4, 'early-refusal'), (5, 'corrupt-last'), (6, 'pass')):
        folder = lab / f'background-vulkan-roundtrip-run-{number}'
        _, report, log, records = observed(folder)
        proof = validate_receipts(log, profile)
        cleanup = validate_cleanup(report, records, profile)
        strict = broker_runtime_complete(log)
        expected_debt = 2 if number in (2, 3, 6) else 1 if number == 5 else 0
        require(strict is (expected_debt == 0) and f'BROKER_OBJECT_DRAIN producerExited=true retained={expected_debt}' in log,
            'Original broker FD debt differs')
        retained[str(number)] = dict(gpu=proof, nativeCleanup=cleanup, strictBrokerAccepted=strict,
            brokerFdReferencesRetainedUntilProducerExit=expected_debt, fullRunAccepted=False, privateReceiptHashes=files(folder))
    folder = lab / 'background-vulkan-roundtrip-fixed-run-4'
    _, report, log, _ = observed(folder)
    require(not broker_runtime_complete(log) and report['ownedQemuControl']['qemuForcedStop'] is True and
        report['ownedQemuControl']['qemuExit'] == 90 and 'BRIDGE_RUNTIME_EXIT=' not in log and
        'cpuFenceWaitRetired=true direct=true target=4 observed=4 owner=independent' in log and
        report['disconnectCleanup']['gpuReservationsReleasedAfterVmExit'] == 7, 'FD0 hang/recovery failure erased')
    hang = dict(accepted=False, forcedStop=True, vmExit=90, reservationsReleasedAfterVmExit=7, privateReceiptHashes=files(folder))
    folder = lab / 'background-vulkan-eventfd-run-5'
    source, report, log, records = observed(folder)
    validate_compute_controls(source, log, records, 256)
    partial_fd_zero = validate_partial_creator_refusal(log, records, report, 'zero')
    partial_fd_zero['privateReceiptHashes'] = files(folder)
    return dict(schema=1, scope='typed Windows metadata for dedicated 64 KiB external GPU buffers; original 4K regressions',
        nativeSha256=NATIVE, shimSha256=SHIM, brokerSha256=BROKER, icdSha256=ICD, currentWindowsRuntimeInputs=len(inputs),
        runs=rows, regressions=regressions, failedSmallBrokerProfileRegressions=rejected_profiles,
        originalRetainedRuns=retained, retainedDescriptorZeroHang=hang, retainedPartialDescriptorZeroRun=partial_fd_zero,
        cpuByteGuardUnchanged=True, gpuByteGuardUnchanged=True, testedDedicatedBufferBytes=65536,
        prototypeMetadataGenerationPerformanceMeasured=False, broaderBufferCompatibilityVerified=False,
        vulkanRasterizationVerified=False, windowPresentationVerified=False, dailyUseAccepted=False,
        compiledWindowsInputHashes={str(path.relative_to(frozen)).replace('\\', '/'): sha(path) for path in sorted(inputs)},
        publicSourceHashes={name: sha(gpu / name) for name in (
            'driver_bridge.cpp', 'driver_wire.h', 'linux_ioctl_bridge.cpp', 'native_buffer_metadata.h',
            'native_buffer_preparation.h', 'guest_broker_views.h', 'guest_eventfd_handle.h',
            'vulkan_memory_roundtrip_probe.cpp', 'vulkan_memory_roundtrip_workload.h',
            'vulkan_memory_roundtrip_reference.py', 'vulkan_memory_roundtrip_evidence.py',
            'vulkan_typed_buffer_evidence.py', 'test_vulkan_memory_roundtrip_evidence.py',
            'test_vulkan_typed_buffer_evidence.py', 'verify_vulkan_typed_buffers.py')})


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lab', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = audit(args.lab.resolve(strict=True))
    with args.output.open('x', encoding='utf-8', newline='\n') as f:
        json.dump(result, f, indent=2); f.write('\n')
    print('PASS: typed buffer GPU/FD controls and original 4K regressions; old failures retained; daily use unaccepted')
