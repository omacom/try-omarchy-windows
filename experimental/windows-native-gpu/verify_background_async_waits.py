"""Audit saved private runs without launching a VM or loading vendor libraries.

The published result contains hashes and scoped counters, never VM images,
vendor libraries, native command streams, local paths or raw runtime logs.
"""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
from async_cpu_wait_evidence import (validate_wait_receipts, validate_gpu_signals,
    validate_exit_cleanup, validate_background_wait_controls)
from verify_4k_capture import require, validate_cleanup
from test_owned_runtime_qemu import async_submissions_complete

NATIVE = 'fba526a5ec94f0774136f97cafd546a1ed2f4e099f54399cdc4b2a3c45e00916'
QEMU = '5404287396eb03a181941f66f09afaee2bc5859a9b87830d64ea1e9eaa91a87c'
KERNEL = 'fb3e75cf32214fad38a22939f8bdafffa78bd7e0e5b2aa2afa89f0ebe02849ba'


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''): digest.update(block)
    return digest.hexdigest()


def load(path):
    return json.loads(path.read_bytes())


def cpio_hashes(path):
    result = {}
    with gzip.open(path, 'rb') as stream:
        while True:
            header = stream.read(110)
            require(len(header) == 110 and header[:6] == b'070701', 'Invalid newc header')
            words = [int(header[6 + i * 8:14 + i * 8], 16) for i in range(13)]
            size, name_size = words[6], words[11]
            require(0 < name_size <= 4096, 'Invalid newc name length')
            name = stream.read(name_size)
            require(len(name) == name_size and name.endswith(b'\0'), 'Truncated newc name')
            name = name[:-1].decode()
            stream.read(-(110 + name_size) % 4)
            digest = hashlib.sha256()
            remaining = size
            while remaining:
                block = stream.read(min(remaining, 1024 * 1024))
                require(bool(block), 'Truncated newc data')
                digest.update(block); remaining -= len(block)
            stream.read(-size % 4)
            if name == 'TRAILER!!!': break
            require(name not in result, 'Duplicate newc payload')
            result[name] = digest.hexdigest()
    return result


def audit(lab):
    repo = Path(__file__).resolve().parents[2]
    gpu = repo / 'experimental/windows-native-gpu'
    pack_dir = lab / 'background-async-waits-v1'
    pack = load(pack_dir / 'pack-receipt.json')
    snapshot = load(lab / 'nvidia-runtime-62101-snapshot-v1/receipt.json')
    require(snapshot['signedInstalledDriver'] is snapshot['sourceDriverUnchanged'] is True and
            snapshot['driverVersion'] == '32.0.16.2101', 'Unverified NVIDIA snapshot')
    require(pack['privateOnly'] is True and pack['redistributable'] is False and
            pack['presentationRequested'] is pack['inputRequested'] is pack['sourceDiskAttached'] is False,
            'Wrong private/background pack profile')
    builds = {}
    for folder, receipt_key in (('kernel-eventfd-v3', 'kernelReceiptSha256'),
                               ('async-cpu-shims-v1', 'shimReceiptSha256'),
                               ('async-wait-probes-v1', 'probeReceiptSha256')):
        path = lab / folder / 'build-receipt.json'
        require(sha(path) == pack[receipt_key], 'Build receipt changed')
        builds[folder] = load(path)
        for name, digest in builds[folder]['sourceHashes'].items():
            frozen = lab / folder / 'source-frozen' / name
            require(sha(frozen) == digest, 'Frozen build source changed: ' + name)
            # This probe was edited after the shim build. It is not compiled
            # into the shared library; the later probe receipt binds its bytes.
            if folder == 'async-cpu-shims-v1' and name == 'experimental/windows-native-gpu/umd_context_probe.cpp':
                continue
            current = (gpu if folder == 'kernel-eventfd-v3' else repo) / name
            # Git normalizes text checkouts; the immutable build digest above
            # still binds the exact original bytes used by the compiler.
            require(current.read_bytes().replace(b'\r\n', b'\n') == frozen.read_bytes().replace(b'\r\n', b'\n'),
                    'Public source differs from build: ' + name)
    for path, key in ((lab / 'kernel-eventfd-v3/kernel/wddm_aperture.ko', 'moduleSha256'),
                      (lab / 'kernel-eventfd-v3/kernel/aperture-contract', 'contractSha256'),
                      (lab / 'async-cpu-shims-v1/linux-ioctl-bridge-4k-async.so', 'shimSha256'),
                      (lab / 'async-wait-probes-v1/runtime-init', 'rootInitSha256')):
        require(sha(path) == pack[key], 'Packed binary changed: ' + key)
    require(builds['kernel-eventfd-v3']['moduleSha256'] == pack['moduleSha256'] and
            builds['kernel-eventfd-v3']['contractSha256'] == pack['contractSha256'] and
            builds['async-wait-probes-v1']['rootInitSha256'] == pack['rootInitSha256'], 'Build/pack mismatch')
    shim_rows = [row for row in builds['async-cpu-shims-v1']['builds'] if row['profile'] == '4k-async']
    require(len(shim_rows) == 1 and shim_rows[0]['binarySha256'] == pack['shimSha256'] and
            '-DWDDM_BRIDGE_ASYNC_CPU_WAITS=1' in shim_rows[0]['command'], 'Candidate shim not selected')
    rows = []
    for number, profile in ((1, 'normal'), (2, 'process-exit')):
        folder = lab / f'background-async-wait-run-{number}'
        source, report = load(folder / 'source.json'), load(folder / 'driver.json')
        guest = (folder / 'driver.log').read_text()
        records = [json.loads(line) for line in (folder / 'driver.host.log').read_text().splitlines() if line.startswith('{')]
        require(source['profile'] == profile and source['actualWorkload'] ==
                ('async-cpu-waits' if profile == 'normal' else 'async-cpu-waits-process-exit') and
                source['genericHarnessWorkload'] == 'init' and source['genericInitializationOracleCoversWaitControls'] is False,
                'Wrong workload scope')
        require(source['harnessUnchanged'] is source['artifactsUnchanged'] is True and
                source['readOnlyInstalledDriverBefore'] == source['readOnlyInstalledDriverAfter'] and
                source['readOnlyInstalledDriverAfter']['installedRuntimeMatchesSnapshot'] is True,
                'Source/runtime changed during run')
        require(source['packReceiptSha256'] == sha(pack_dir / 'pack-receipt.json') and
                source['runnerSha256'] == sha(lab / 'run-background-async-waits-v1.py'), 'Runner/pack changed')
        require(source['harnessSha256'] == sha(gpu / 'broker_runtime_qemu.py'), 'Harness changed')
        for name, digest in source['harnessComponents'].items(): require(sha(gpu / name) == digest, 'Harness component changed')
        require(report['diagnosticAccepted'] is False and source['generalVerifierExit'] == 1,
                'Original generic rejection must be retained')
        require(report['liveNvidiaLinuxUmdLoaded'] is True and report['realWslDxgForwarding'] is False and
                report['capturedPrivateFixturesUsed'] is False and report['privateRuntimeImageRedistributable'] is False and
                report['hypervisor'] == 'QEMU/WHPX', 'Missing native GPU provenance')
        require(report['driverBridgeSha256'] == source['nativeSha256'] == NATIVE and
                report['qemuSha256'] == QEMU and report['kernelSha256'] == KERNEL, 'Runtime binary changed')
        image = pack_dir / (profile + '.cpio.gz')
        require(report['initramfsSha256'] == source['initramfsSha256'] == pack['images'][profile]['sha256'] == sha(image),
                'Image changed')
        probe_rows = [row for row in builds['async-wait-probes-v1']['builds'] if row['profile'] == profile]
        require(len(probe_rows) == 1 and probe_rows[0]['binarySha256'] == pack['images'][profile]['probeSha256'] ==
                sha(lab / 'async-wait-probes-v1' / ('async-wait-probe-' + profile)), 'Probe changed')
        payload = cpio_hashes(image)
        for name, key in (('init', 'rootInitSha256'), ('wddm-aperture.ko', 'moduleSha256'),
                          ('aperture-contract', 'contractSha256'), ('linux-ioctl-bridge.so', 'shimSha256')):
            require(payload[name] == pack[key], 'Embedded component differs: ' + name)
        require(payload['native-sync-files-required'] == hashlib.sha256(b'1').hexdigest() and
                payload['guest-probe'] == pack['images'][profile]['probeSha256'], 'Wrong embedded wait profile')
        for name, metadata in snapshot['files'].items():
            matches = [digest for path, digest in payload.items() if path.endswith('/' + name)]
            require(matches == [metadata['sha256']], 'NVIDIA payload differs from signed snapshot')
        broker = validate_background_wait_controls(source, guest, records, profile)
        waits = validate_wait_receipts(guest, profile)
        signals = validate_gpu_signals(report, records, profile)
        if profile == 'normal':
            validate_cleanup(report, records)
            cleanup = dict(eventualNativeAndVmCleanupVerified=True, releasedAfterVmExit={},
                allNativeMappingsRetiredBeforeVmExit=True, liveNativeCrashRecoveryBeforeVmExitVerified=False)
            require(async_submissions_complete(report['guestCommandEnqueues'], report['nativeAsyncSubmissionAcceptances'],
                report['nativeAsyncSubmissionRetirements'], report['disconnectCleanup']), 'Unretired initialization command')
            require(report['disconnectCleanup']['completedNativeSubmissions'] == 2, 'Wrong initialization submission count')
        else:
            cleanup = validate_exit_cleanup(report, records)
            require(cleanup['liveNativeCrashRecoveryBeforeVmExitVerified'] is False and bool(cleanup['releasedAfterVmExit']),
                    'Observed process-exit recovery failure was erased')
            try: validate_cleanup(report, records)
            except ValueError as error:
                require(str(error) == 'Missing or mismatched mapping retirement', 'Unexpected original cleanup rejection')
            else: raise ValueError('Original before-VM cleanup oracle must still reject the exit run')
        counters = report['disconnectCleanup']
        require(all(type(v) is int and v == 0 for k, v in counters.items()
                    if k.startswith(('failed', 'live', 'pending', 'forced')) or k.endswith('Timeouts')), 'Native work failed')
        require(not any(v for k, v in counters.items() if k.endswith('ReleasedAfterVmExit')) if profile == 'normal' else True,
                'Normal run retained native objects until VM exit')
        for key, marker in (('nativeAsyncSubmissionAcceptances', 'nativeAsyncSubmissionAccepted'),
                            ('nativeAsyncSubmissionRetirements', 'nativeAsyncSubmissionRetired')):
            require(json.dumps(report[key], sort_keys=True) == json.dumps([r for r in records if marker in r], sort_keys=True),
                    'Report/command receipt mismatch')
        rows.append(dict(run=number, profile=profile, waits=waits, gpuSignals=signals, broker=broker, cleanup=cleanup,
            nativeInitializationSubmissions=counters['completedNativeSubmissions'], deviceHealthQueries=counters['completedDeviceExecutionQueries'],
            peakReportedGpuUsageBytes=counters['peakReportedGpuUsageBytes'], gpuUsageLimitBytes=counters['reportedGpuUsageLimitBytes'],
            genericWholeRunAccepted=False, privateReceiptHashes={name: sha(folder / name) for name in
                ('source.json', 'driver.json', 'driver.log', 'driver.host.log', 'driver.errors.log')}))
    return dict(schema=1, scope='Opt-in GPU fence to Linux eventfd waits and process-exit cancellation only',
        runs=rows, sourceHashes={str(p.relative_to(repo)).replace('\\', '/'): sha(p) for p in (gpu / 'linux_ioctl_bridge.cpp',
            gpu / 'guest_async_cpu_wait.h', gpu / 'async_cpu_wait_workload.h', gpu / 'guest-kernel/wddm_fence_eventfd.h',
            gpu / 'guest-kernel/eventfd_wait_contract.h', gpu / 'runtime_guest_init.c')},
        nativeSha256=NATIVE, qemuSha256=QEMU, kernelSha256=KERNEL,
        componentHashes={key: pack[key] for key in ('moduleSha256', 'contractSha256', 'shimSha256', 'rootInitSha256')},
        publicBuildSourceComparison='LF normalized; frozen build receipts retain original raw-byte hashes',
        shaderExecutionVerified=False, vulkanWindowPresentationVerified=False, deviceLossRecoveryVerified=False,
        physicalInputVerified=False, performanceMeasured=False, dailyUseAccepted=False,
        verifierHashes={p.name: sha(p) for p in (Path(__file__), gpu / 'async_cpu_wait_evidence.py', gpu / 'verify_4k_capture.py')})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lab', type=Path, required=True, help='Private saved build/run directory')
    parser.add_argument('--output', type=Path, required=True, help='New sanitized result; never overwritten')
    args = parser.parse_args()
    result = audit(args.lab.resolve())
    with args.output.open('x', encoding='utf-8', newline='\n') as output:
        json.dump(result, output, indent=2); output.write('\n')
    print('PASS: six GPU wait cases; 69 notifications; exit cancellation; native VM-exit retention preserved. No desktop/performance acceptance.')


if __name__ == '__main__':
    main()
