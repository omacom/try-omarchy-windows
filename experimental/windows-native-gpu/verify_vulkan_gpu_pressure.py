"""File-only proof of bounded Vulkan pressure recovery and original 4K regressions."""
import argparse
import json
from pathlib import Path
import re
from verify_background_async_waits import QEMU, KERNEL, cpio_hashes, load, sha
from verify_4k_capture import require, verify
from broker_runtime_evidence import broker_aware_initialization_complete
from vulkan_compute_evidence import validate_compute_controls, validate_compute_cleanup, validate_compute_receipts
from vulkan_memory_roundtrip_evidence import validate_receipts
from vulkan_typed_buffer_evidence import validate_fd_control, validate_typed_metadata
from vulkan_gpu_pressure_evidence import validate_pressure_receipts, validate_admission_queries, validate_pressure_metadata, validate_pressure_cleanup
from background_shader_evidence import validate_background_controls

NATIVE = '1d46d7592776125d10760a9223274ebc85227565a6dddcd3de23404457c84c70'
SHIM = '51f379aa8bab464115005774a09dad7ba9af87bc82a086b6b949b6a134543030'
BROKER = '683a56fd7995dae634c39afda35b6925d9bf8451f8cb31ec0fb8894386ff9af9'
ICD = 'f0608e8dd5b71736fb9703042cb8a7d12952e04865e2baa0dd8e1ec38f41a22a'
LEGACY = 'e6c29c6cf1ee42d93c8c3e858c4d685237a902d05218dbe840533c2069f03286'


def audit(lab):
    gpu = Path(__file__).resolve().parent
    repo = gpu.parent.parent
    require(sha(gpu / 'test_owned_runtime_qemu.py') == LEGACY, 'Frozen legacy harness changed')
    frozen = lab / 'native-v118-source-frozen'
    native = load(lab / 'native-v118-receipt.json')
    require(native['bridgeSha256'] == sha(lab / 'native-v118/driver-bridge.exe') == NATIVE and
        native['defaultBridgeSha256'] == sha(lab / 'native-default-v118/driver-bridge.exe'), 'Native binary differs')
    for name, digest in native['sources'].items():
        require(sha(frozen / name) == digest, 'Frozen Windows source differs')
    inputs = set()
    def includes(path):
        if path in inputs: return
        inputs.add(path)
        for name in re.findall(r'^\s*#include "([^"]+)"', path.read_text(encoding='utf-8'), re.M):
            target = path.parent / name
            if target.is_file(): includes(target)
    includes(frozen / 'experimental/windows-native-gpu/driver_bridge.cpp')
    for path in inputs:
        require(sha(path) == sha(repo / path.relative_to(frozen)), 'Current compiled Windows input differs')
    shims = lab / 'gpu-buffer-admission-shims-v7'
    sr = load(shims / 'build-receipt.json')
    for key in ('legacyTransportControlsPassed', 'originalWireControlsPassed', 'eventfdHandleControlsPassed',
        'admissionReplyControlsPassed', 'eventfdPreloadControlsPassed', 'importedGpuVaProfileControlsPassed'):
        require(sr[key] is True, 'Shim CPU/preload control missing: ' + key)
    for name, digest in sr['sourceHashes'].items():
        require(sha(shims / 'source-frozen' / name) == digest, 'Frozen guest source differs')
    for name in ('linux_ioctl_bridge.cpp', 'driver_wire.h', 'native_buffer_admission_reply.h', 'native_buffer_preparation.h'):
        require(sha(gpu / name) == sr['sourceHashes']['experimental/windows-native-gpu/' + name], 'Current guest input differs')
    require(sha(shims / 'linux-ioctl-bridge-4k-async.so') == SHIM ==
        next(r['binarySha256'] for r in sr['builds'] if r['profile'] == '4k-async'), 'Wrong shim binary')
    broker = lab / 'gpu-buffer-admission-broker-4k-v1'
    br = load(broker / 'receipt.json')
    require(br['highResolutionProfile'] is br['testsRequiredRoot'] is True and br['byteGuardsChanged'] is False and
        br['brokerSha256'] == sha(broker / 'guest-graphics-broker') == BROKER, 'Wrong 4K broker build')
    mesa = lab / 'mesa-dozen-buffer-admission-v1'
    mr = load(mesa / 'build-receipt.json')
    require(mr['icdSha256'] == sha(mesa / 'build/src/microsoft/vulkan/libvulkan_dzn.so') == ICD and
        mr['originalIcdUnchanged'] is True and mr['sdkFiles'] == 104, 'Wrong ICD build')
    for name, digest in mr['patchHashes'].items():
        require(sha(mesa / 'patch-source' / name) == sha(gpu / 'mesa' / name) == digest, 'Mesa patch differs')
    for manifest_name in ('dozen-memory-fd-import-patch.json', 'dozen-native-gpu-buffer-patch.json', 'dozen-buffer-admission-patch.json'):
        for name, row in load(gpu / 'mesa' / manifest_name)['files'].items():
            if name.endswith('/dzn_device.c') and manifest_name != 'dozen-buffer-admission-patch.json': continue
            require(sha(mesa / 'mesa-26.0.8' / name) == row['patchedSha256'], 'Applied ICD source differs')
    require(sha(gpu / 'mesa/dzn_wddm_native_gpu_buffer.h') == mr['nativeBufferHeaderSha256'] and
        sha(gpu / 'mesa/dzn_wddm_native_gpu_buffer_admission.h') == mr['admissionHeaderSha256'], 'Canonical SDK helper differs')
    probes = lab / 'vulkan-memory-roundtrip-probe-v7'
    pr = load(probes / 'build-receipt.json')
    for name, digest in pr['sourceHashes'].items():
        require(sha(probes / 'source-frozen' / name) == sha(gpu / name) == digest, 'Current GPU workload differs')
    require(sha(gpu / 'build_vulkan_memory_roundtrip_probe.py') == pr['builderSha256'], 'GPU workload builder differs')
    for r in pr['profiles']:
        require(sha(probes / r['profile'] / 'vulkan-memory-roundtrip-probe') == r['probeSha256'], 'GPU probe binary differs')
    old_pr = load(lab / 'vulkan-memory-roundtrip-probe-v5/build-receipt.json')
    require(pr['profiles'][:4] == old_pr['profiles'], 'Original four workload binaries changed')
    snapshot = load(lab / 'nvidia-runtime-62101-snapshot-v1/receipt.json')
    require(snapshot['signedInstalledDriver'] is snapshot['sourceDriverUnchanged'] is True and
        snapshot['driverVersion'] == '32.0.16.2101', 'Signed NVIDIA snapshot differs')
    cache = {}
    def payload(path):
        if path not in cache: cache[path] = cpio_hashes(path)
        return cache[path]
    def image(label):
        folder = lab / ('background-vulkan-gpu-admission-' + label + '-v1')
        pack = load(folder / 'pack-receipt.json')
        base = lab / ('background-vulkan-gpu-memory-pressure-v2' if label == 'pressure' else 'background-vulkan-typed-buffer-' + label + '-v2')
        old, new = base / 'private-vulkan.cpio.gz', folder / 'private-vulkan.cpio.gz'
        require(pack['initramfsSha256'] == sha(new) and pack['baselineImageSha256'] == sha(old) and
            pack['baselinePackReceiptSha256'] == sha(base / 'pack-receipt.json'), 'Private image lineage differs')
        a, b = payload(old), payload(new)
        wanted = {'linux-ioctl-bridge.so': SHIM}
        if label != 'texture':
            paths = [p for p in b if p.endswith('/libvulkan_dzn.so')]
            require(len(paths) == 1, 'Wrong embedded ICD count')
            wanted[paths[0]] = ICD
        require(a.keys() == b.keys() and {n: b[n] for n in a if a[n] != b[n]} == wanted and
            b['guest-graphics-broker'] == BROKER, 'Image changed more than shim/ICD or wrong broker')
        for name, row in snapshot['files'].items():
            require([v for n, v in b.items() if n.endswith('/' + name)] == [row['sha256']], 'Embedded NVIDIA runtime differs')
        for key in ('redistributable', 'sourceDiskAttached', 'presentationRequested', 'inputRequested', 'byteGuardsChanged'):
            require(pack[key] is False, 'Private image scope differs: ' + key)
        require(pack['privateOnly'] is pack['sourceImageUnchanged'] is True and pack['componentReceipts'] ==
            dict(shim=sha(shims / 'build-receipt.json'), broker=sha(broker / 'receipt.json'), icd=sha(mesa / 'build-receipt.json')),
            'Image component receipt differs')
        return new
    def files(folder):
        return {name: sha(folder / name) for name in ('source.json', 'driver.json', 'driver.log', 'driver.host.log', 'driver.errors.log')}
    def observed(folder, version=118, candidate_image=None):
        source, report = load(folder / 'source.json'), load(folder / 'driver.json')
        log = (folder / 'driver.log').read_text(encoding='utf-8')
        records = [json.loads(s) for s in (folder / 'driver.host.log').read_text(encoding='utf-8').splitlines() if s.startswith('{')]
        binary = lab / f'native-v{version}/driver-bridge.exe'
        receipt = lab / f'native-v{version}-receipt.json'
        require(source['nativeSha256'] == report['driverBridgeSha256'] == sha(binary) == load(receipt)['bridgeSha256'] and
            source['nativeBuildReceiptSha256'] == sha(receipt), 'Actual native executable/receipt differs')
        require(source['initramfsSha256'] == report['initramfsSha256'] and
            (candidate_image is None or source['initramfsSha256'] == sha(candidate_image)), 'Actual VM image differs')
        require(report['qemuSha256'] == QEMU and report['kernelSha256'] == KERNEL and
            report['hypervisor'] == 'QEMU/WHPX' and report['liveNvidiaLinuxUmdLoaded'] is True and
            report['capturedPrivateFixturesUsed'] is report['privateRuntimeImageRedistributable'] is report['realWslDxgForwarding'] is False,
            'GPU provenance differs')
        require(source['readOnlyInstalledDriverBefore'] == source['readOnlyInstalledDriverAfter'] and
            source['readOnlyInstalledDriverAfter']['installedRuntimeMatchesSnapshot'] is True and
            source['harnessUnchanged'] is source['artifactsUnchanged'] is True, 'Run guard changed')
        require(source['harnessSha256'] == sha(gpu / 'broker_runtime_qemu.py') and all(
            sha(gpu / name) == digest for name, digest in source['harnessComponents'].items()), 'Harness component differs')
        return source, report, log, records
    rows = []
    for number, label in ((6, 'pressure'), (7, 'normal'), (8, 'pressure')):
        folder = lab / f'background-vulkan-metadata-memory-run-{number}'
        source, report, log, records = observed(folder, candidate_image=image(label))
        validate_compute_controls(source, log, records, 256)
        cleanup = validate_pressure_cleanup(report, records) if label == 'pressure' else validate_compute_cleanup(report, records, 256, 'pass')
        require(report['diagnosticAccepted'] is False and source['generalVerifierExit'] == 1 and
            source['genericInitializationOracleCoversRoundtrip'] is False, 'Generic initialization was promoted')
        queries = validate_admission_queries(records, report['disconnectCleanup'], label == 'pressure')
        proof = validate_pressure_receipts(log) if label == 'pressure' else validate_receipts(log, 'pass')
        metadata = validate_pressure_metadata(records, report['disconnectCleanup']) if label == 'pressure' else \
            validate_typed_metadata(records, report['disconnectCleanup'], 'pass', True)
        rows.append(dict(run=number, profile=label, gpu=proof, admission=queries, metadata=metadata,
            cleanup=cleanup, originalGenericAccepted=False, privateReceiptHashes=files(folder)))
    for number, label in ((1, 'import-zero'), (2, 'fd-exhaustion'), (3, 'corrupt-last')):
        folder = lab / f'background-vulkan-gpu-admission-run-{number}'
        source, report, log, records = observed(folder, candidate_image=image(label))
        validate_compute_controls(source, log, records, 256)
        cleanup = validate_compute_cleanup(report, records, 256, 'pass')
        profile = 'corrupt-last' if label == 'corrupt-last' else 'pass'
        require(report['diagnosticAccepted'] is False and source['generalVerifierExit'] == 1, 'Generic initialization was promoted')
        rows.append(dict(run=number, profile=label, gpu=validate_receipts(log, profile), cleanup=cleanup,
            admission=validate_admission_queries(records, report['disconnectCleanup'], rounds=1 if profile == 'corrupt-last' else 2),
            fdControl=validate_fd_control(log, {'import-zero': 'zero', 'fd-exhaustion': 'exhaustion'}.get(label, 'none')),
            metadata=validate_typed_metadata(records, report['disconnectCleanup'], profile, True), privateReceiptHashes=files(folder)))
    regressions = {}
    for label in ('compute', 'texture'):
        folder = lab / ('background-vulkan-gpu-admission-' + label + '-run-1')
        source, report, log, records = observed(folder, candidate_image=image(label))
        if label == 'compute':
            validate_compute_controls(source, log, records, 256)
            cleanup = validate_compute_cleanup(report, records, 256, 'pass')
            proof = validate_compute_receipts(log, 'pass')
        else:
            validate_background_controls(source, log, records, 10000)
            require(broker_aware_initialization_complete(log, report['hostBridgeExit'], report['ownedQemuControl'],
                report['disconnectCleanup']), 'Original shader initialization/retirement failed')
            oracle = lab / 'native-v111/rgba-oracle.exe'
            require(sha(oracle) == '6fde0d421216597e4e9c3d54574608d10ee6d2f5148cf23c95c3a640a708aaf4', 'Original pixel oracle differs')
            proof = verify(folder / 'driver.json', folder, oracle)
            require(proof['full4kDiagnosticVerified'] is proof['sharedHandoffVerified'] is True and
                proof['dxgiPresentationVerified'] is False, 'Shader regression scope differs')
            for i in (1, 2):
                require(sha(folder / f'native-copy-{i}.rgba') == sha(lab / f'background-gpu-shader-run-1/native-copy-{i}.rgba'), 'Original shader pixels changed')
            cleanup = dict(allMappingsRetiredBeforeVmExit=True, originalShaderPixelsByteIdentical=True)
        require(report['disconnectCleanup']['nativeGpuBufferAdmissionQueries'] == 0, 'Regression silently entered typed buffer admission')
        regressions[label] = dict(proof=proof, cleanup=cleanup, privateReceiptHashes=files(folder))
    retained = {}
    for number, version, reason in ((2, 116, 'reservation count refusal'), (3, 116, 'host fatal metadata quota refusal'),
        (4, 117, 'late refusal loses guest device'), (5, 117, 'new shim legacy fallback still loses guest device')):
        folder = lab / f'background-vulkan-metadata-memory-run-{number}'
        source, report, log, records = observed(folder, version, image('pressure') if number == 5 else None)
        require('VULKAN_GPU_MEMORY_PRESSURE_RECOVERED' not in log and 'VULKAN_MEMORY_ROUNDTRIP_COMPLETE' not in log,
            'Retained pressure failure was promoted')
        if number == 3:
            errors = (folder / 'driver.errors.log').read_text(encoding='utf-8')
            require(report['hostBridgeExit'] == 1 and report['ownedQemuControl']['qemuForcedStop'] is True and
                report['ownedQemuControl']['qemuExit'] == 90 and 'BRIDGE_RUNTIME_EXIT=' not in log and
                'Native buffer template exceeds existing allocation budget' in errors, 'Original host crash erased')
        else:
            require(report['hostBridgeExit'] == report['ownedQemuControl']['qemuExit'] == 0 and
                'VULKAN_BRIDGE_RESULT stage=pressure-device-idle result=-2' in log and 'BRIDGE_RUNTIME_EXIT=1' in log and
                report['disconnectCleanup']['gpuReservationsReleasedAfterVmExit'] == 4, 'Guest device loss/deferred reservations erased')
        if number == 2:
            require('LINUX_BRIDGE ioctlFailed nr=8 errno=24' in log and 'D3D12: Removing Device.' in log, 'Reservation-count control differs')
        else:
            rejected = [r for r in records if 'nativeReportedGpuUsageRejected' in r]
            require(len(rejected) == 1 and rejected[0]['usageBytes'] == 263806976 and
                rejected[0]['additionalBytes'] == 67108864 and rejected[0]['limitBytes'] == 268435456, 'Metadata budget refusal erased')
        if version == 117:
            require(report['disconnectCleanup']['nativeGpuBufferMetadataMemoryRefusals'] == 1 and
                any(r.get('nativeGpuBufferMetadataMemoryRefused') is True and r.get('temporaryOwnershipUnwound') is True and
                    r.get('allocationPublished') is False for r in records), 'Host-only catch scope erased')
        if number == 5:
            require('nativeGpuBufferAdmissionUnsupported=true legacyProfileRetained=true' in log, 'Legacy fallback was not exercised')
        retained[str(number)] = dict(accepted=False, reason=reason, privateReceiptHashes=files(folder))
    return dict(schema=1, scope='single-process dedicated external Vulkan GPU-buffer preflight recovery; original 4K regressions',
        nativeSha256=NATIVE, shimSha256=SHIM, brokerSha256=BROKER, icdSha256=ICD, runs=rows, regressions=regressions,
        retainedPressureFailures=retained, cpuMappedByteLimit=100663296, reportedGpuUsageByteLimit=268435456,
        gpuMappedByteLimit=268435456, byteGuardsChanged=False, advisoryOnly=True, hardQuotaImplemented=False,
        concurrentAdmissionVerified=False, complete64MiBContentsVerified=False, vulkanRasterizationVerified=False,
        windowPresentationVerified=False, performanceMeasured=False, dailyUseAccepted=False,
        compiledWindowsInputHashes={str(p.relative_to(frozen)).replace('\\', '/'): sha(p) for p in sorted(inputs)},
        publicSourceHashes={name: sha(gpu / name) for name in ('driver_bridge.cpp', 'driver_wire.h', 'linux_ioctl_bridge.cpp',
            'native_metadata_attempt.h', 'native_buffer_metadata.h', 'native_buffer_admission_reply.h',
            'vulkan_memory_roundtrip_probe.cpp', 'vulkan_memory_roundtrip_workload.h', 'vulkan_gpu_pressure_evidence.py',
            'test_vulkan_gpu_pressure_evidence.py', 'verify_vulkan_gpu_pressure.py', 'mesa/0008-linux-dozen-buffer-admission-errors.patch')})


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lab', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = audit(args.lab.resolve(strict=True))
    with args.output.open('x', encoding='utf-8', newline='\n') as output:
        json.dump(result, output, indent=2); output.write('\n')
    print('PASS: actual pressure refusal/retry, complete GPU rounds, descriptor controls and original 4K regressions; daily use unaccepted')
