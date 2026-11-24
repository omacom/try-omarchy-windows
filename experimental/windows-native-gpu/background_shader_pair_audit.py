"""Audit the saved background shader pair and isolated-adapter repeat."""
from pathlib import Path
import argparse
import hashlib
import json
import shutil
import subprocess
import tempfile
from background_shader_evidence import validate_background_controls
from broker_runtime_evidence import broker_aware_initialization_complete as initialization_complete
from verify_4k_capture import require, verify


def audit(lab, repo):
    sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    build = lab / 'background-gpu-shader-v1'
    receipt = json.loads((build / 'build-receipt.json').read_bytes())
    require(receipt['geometry'] == [3840, 2160] and receipt['workload'] == 'native', 'Wrong shader build')
    require(all(sha(build / name) == digest for name, digest in receipt['sources'].items()), 'Frozen sources changed')
    require(all(sha(build / name) == digest for name, digest in receipt['binaries'].items()), 'Frozen binaries changed')
    native = json.loads((lab / 'native-v111-receipt.json').read_bytes())
    require(all(sha(lab / 'native-v111-source-frozen' / name) == digest for name, digest in native['sources'].items()),
            'Native compile sources changed')
    oracle = lab / 'native-v111/rgba-oracle.exe'
    require(sha(oracle) == '6fde0d421216597e4e9c3d54574608d10ee6d2f5148cf23c95c3a640a708aaf4', 'Wrong compiled integer oracle')
    git = shutil.which('git') or 'C:/Program Files/Git/cmd/git.exe'
    old_harness = subprocess.check_output([git, 'show',
        '6975c5c33e3bd69a811c4f694999f289170a9665:experimental/windows-native-gpu/test_owned_runtime_qemu.py'], cwd=repo)
    rows, manifests, payloads = [], [], []
    adapter = repo / 'experimental/windows-native-gpu/broker_runtime_qemu.py'
    components = {name: sha(adapter.with_name(name)) for name in
                  ('test_owned_runtime_qemu.py', 'broker_runtime_evidence.py')}
    require(components['test_owned_runtime_qemu.py'] ==
            'e6c29c6cf1ee42d93c8c3e858c4d685237a902d05218dbe840533c2069f03286',
            'Historical harness changed')
    for number, label, reads in ((1, '4k', 10000), (2, '4k-sampled', 2), (3, '4k-sampled', 2)):
        folder = lab / f'background-gpu-shader-run-{number}'
        report = json.loads((folder / 'driver.json').read_bytes())
        source = json.loads((folder / 'source.json').read_bytes())
        guest = (folder / 'driver.log').read_text(encoding='utf-8')
        records = [json.loads(line) for line in (folder / 'driver.errors.log').read_text().splitlines() if line.startswith('{')]
        require(report['driverBridgeSha256'] == source['nativeSha256'] == native['bridgeSha256'] ==
                'fba526a5ec94f0774136f97cafd546a1ed2f4e099f54399cdc4b2a3c45e00916', 'Wrong native executable')
        require(report['qemuSha256'] == '5404287396eb03a181941f66f09afaee2bc5859a9b87830d64ea1e9eaa91a87c', 'Wrong QEMU')
        require(sha(build / f'private-{label}.cpio.gz') == report['initramfsSha256'] ==
                source['initramfsSha256'] == receipt['images'][label], 'Wrong private image')
        if number == 3:
            require(source['harnessSha256'] == sha(adapter) and source['harnessComponents'] == components,
                    'Wrong isolated adapter or component sources')
            # The historical whole-run verdict also gates runtimeInitializationComplete.
            # Preserve that rejection; require the independent initialization result.
            require(report['runtimeInitializationResultsSucceeded'] is True,
                    'Adapter did not verify broker initialization')
        else:
            require(source['harnessSha256'] == hashlib.sha256(old_harness).hexdigest(), 'Wrong original runtime harness')
        require(source['harnessUnchanged'] is source['artifactsUnchanged'] is True, 'Artifact guard failed')
        require(source['readOnlyInstalledDriverBefore'] == source['readOnlyInstalledDriverAfter'] and
                source['readOnlyInstalledDriverAfter']['installedRuntimeMatchesSnapshot'] is True, 'Installed runtime changed')
        require(report['diagnosticAccepted'] is False and source['generalVerifierExit'] == 1, 'Original verdict changed')
        clean, vm = report['disconnectCleanup'], report['ownedQemuControl']
        require(initialization_complete(guest, report['hostBridgeExit'], vm, clean), 'Broker initialization incomplete')
        require(all(value == 0 for key, value in clean.items() if key.startswith(('failed', 'live', 'pending', 'forced')) or
                    key.endswith(('Timeouts', 'ReleasedAfterVmExit'))), 'Native failure or delayed retirement')
        require(all(value == 0 for key, value in vm.items() if key.startswith('live') or key.endswith('ReleasedAfterVmExit')),
                'Aperture mappings outlived VM')
        require(clean['cpuStoreTestRequested'] is False and clean['completedCpuStoreTests'] == 0,
                'CPU store diagnostic enabled')
        selection = validate_background_controls(source, guest, records, reads)
        result = verify(folder / 'driver.json', folder, oracle)
        require(result['full4kDiagnosticVerified'] is result['sharedHandoffVerified'] is True and
                result['dxgiPresentationVerified'] is False, '4K shader/handoff incomplete')
        frames = [(folder / f'native-copy-{index}.rgba').read_bytes() for index in (1, 2)]
        payloads.append(frames)
        if number < 3:
            manifests.append(json.loads((build / f'private-{label}.cpio.manifest.json').read_bytes()))
        require(clean['reportedGpuUsageLimitBytes'] == 268435456 and
                clean['peakReportedGpuUsageBytes'] <= clean['reportedGpuUsageLimitBytes'], 'GPU usage guard exceeded')
        rows.append(dict(run=number, profile=label, full4kShaderAndNativeCopyVerified=True,
            originalWholeRunAccepted=False, isolatedAdapterRepeat=number == 3, frames=result['frames'], **selection,
            nativeSubmissions=clean['completedNativeSubmissions'], allNativeWorkRetired=True,
            allTrackedNativeObjectsAndMappingsRetiredBeforeVmExit=True, hostExit=0, naturalVmExit=True,
            peakReportedGpuUsageBytes=clean['peakReportedGpuUsageBytes'], gpuUsageLimitBytes=clean['reportedGpuUsageLimitBytes'],
            deviceHealthQueries=clean['completedDeviceExecutionQueries'],
            privateFiles={name: sha(folder / name) for name in ('driver.json', 'driver.log', 'driver.host.log',
                'driver.errors.log', 'source.json', 'native-copy-1.rgba', 'native-copy-2.rgba')}))
    require(all(frames == payloads[0] for frames in payloads[1:]),
            'Sampling or the adapter changed the actual GPU image bytes')
    first, second = ({item['path']: item for item in manifest['files']} for manifest in manifests)
    require(set(first) == set(second) and [name for name in first if first[name] != second[name]] == ['/linux-ioctl-bridge.so'],
            'Paired image payloads differ beyond the shim')
    # Deliberately change a background alpha byte in a new temporary file.
    # The real captures stay immutable. A queued receipt/hash alone cannot pass.
    with tempfile.TemporaryDirectory(prefix='shader-oracle-negative-', dir=lab) as temporary:
        corrupt = Path(temporary) / 'alpha-corrupted.rgba'
        changed = bytearray(payloads[1][0]); changed[3] ^= 1; corrupt.write_bytes(changed)
        rejected = subprocess.run([str(oracle), str(corrupt), str(lab / 'background-gpu-shader-run-2/native-copy-2.rgba')],
                                  capture_output=True, text=True, timeout=60)
        require(rejected.returncode == 1, 'Integer oracle accepted corrupt background alpha')
    return dict(schema=1, scope='Paired 4K GPU shader/native copy and adapter repeat without display presentation', runs=rows,
        byteIdenticalAcrossReadSelections=True, onlyImagePayloadDifference='/linux-ioctl-bridge.so',
        geometry=[3840, 2160], capturedRoundsPerRun=2, shaderDrawingVerified=True,
        corruptPixelRejected=True, oracleSha256=sha(oracle), buildReceiptSha256=sha(build / 'build-receipt.json'),
        existing4kVerifierSha256=sha(repo / 'experimental/windows-native-gpu/verify_4k_capture.py'),
        backgroundControlsSha256=sha(repo / 'experimental/windows-native-gpu/background_shader_evidence.py'),
        brokerEvidenceSha256=components['broker_runtime_evidence.py'], brokerAdapterSha256=sha(adapter),
        legacyHarnessSha256=components['test_owned_runtime_qemu.py'],
        originalPairHarnessSha256=hashlib.sha256(old_harness).hexdigest(),
        auditorSha256=sha(Path(__file__)), desktopFrameVerified=False, performanceMeasured=False,
        physicalScanoutVerified=False, dailyUseAccepted=False)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('lab', type=Path)
    parser.add_argument('repository', type=Path)
    parser.add_argument('new_output', type=Path)
    args = parser.parse_args()
    value = audit(args.lab.resolve(), args.repository.resolve())
    with args.new_output.open('x', encoding='utf-8', newline='\n') as output:
        json.dump(value, output, indent=2); output.write('\n')
    print('PASS: six actual 4K shader captures across the read pair and isolated adapter repeat; native GPU handoff, corruption rejection, no presentation/input, live cleanup; no desktop/performance acceptance')
