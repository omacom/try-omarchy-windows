"""File-only audit of private 4K Vulkan compute repeats and retained controls."""
import argparse
import json
from pathlib import Path
from verify_background_async_waits import NATIVE, QEMU, KERNEL, sha, load, cpio_hashes
from vulkan_compute_evidence import (EXPECTED_HASHES, validate_compute_receipts,
    validate_compute_controls, validate_compute_cleanup, validate_refusal_retention)
from verify_4k_capture import require


def audit(lab):
    gpu = Path(__file__).resolve().parent
    original = lab / 'background-vulkan-compute-v1'
    controlled = lab / 'background-vulkan-compute-v2'
    first, second = load(original / 'build-receipt.json'), load(controlled / 'build-receipt.json')
    require(second['builderSha256'] == sha(gpu / 'build_vulkan_compute_probe.py'), 'Public builder changed')
    for folder, receipt in ((original, first), (controlled, second)):
        for name, digest in receipt['sourceHashes'].items():
            require(sha(folder / 'source-frozen' / name) == digest, 'Frozen source changed')
    for name, digest in second['sourceHashes'].items(): require(sha(gpu / name) == digest, 'Public source differs from controlled build')
    for row in second['profiles']:
        folder = controlled / row['profile']
        for name, key in (('compute.spv', 'spirvSha256'), ('vulkan_compute_shader.inc', 'shaderIncludeSha256'),
                          ('vulkan-compute-probe', 'probeSha256')):
            require(sha(folder / name) == row[key], 'Controlled shader/probe changed')
        require(row['shaderValidationPassed'] is True, 'SPIR-V validation did not pass')
    require([row['profile'] for row in second['profiles']] == ['normal', 'corrupt-last'], 'Wrong shader controls')
    normal, corrupt = second['profiles']
    require(first['spirvSha256'] == sha(original / 'compute.spv') == normal['spirvSha256'] and
            first['probeSha256'] == sha(original / 'vulkan-compute-probe') == normal['probeSha256'] and
            normal['spirvSha256'] != corrupt['spirvSha256'] and normal['probeSha256'] != corrupt['probeSha256'],
            'Default shader changed or corruption control was ineffective')
    for name in ('vulkan_compute_probe.cpp', 'vulkan_compute_workload.h'):
        require(first['sourceHashes'][name] == second['sourceHashes'][name], 'Oracle/host probe changed between controls')
    reference = load(original / 'reference-public-v1.json')
    require(reference['oracleSha256'] == sha(gpu / 'vulkan_compute_reference.py') and reference['width'] == 3840 and
            reference['height'] == 2160 and reference['rounds'] == [dict(round=i, words=8294400, fnv1a=value)
                for i, value in enumerate(EXPECTED_HASHES)], 'Independent expected hashes differ')
    snapshot = load(lab / 'nvidia-runtime-62101-snapshot-v1/receipt.json')
    require(snapshot['signedInstalledDriver'] is snapshot['sourceDriverUnchanged'] is True and
            snapshot['driverVersion'] == '32.0.16.2101', 'Unverified NVIDIA snapshot')
    icd = lab / 'background-vulkan-v1/build/src/microsoft/vulkan/libvulkan_dzn.so'
    require(sha(icd) == first['icdSha256'] == '4dbfee33f92941341c175b03b337f5df4355f114178e9ea0d2adfce1edb07f17', 'Original Dozen ICD changed')
    payloads, packs = {}, {}
    for folder, probe_hash in ((original, normal['probeSha256']), (controlled, corrupt['probeSha256'])):
        packed = load(folder / 'pack-receipt.json')
        require(packed['privateOnly'] is True and packed['redistributable'] is False and
                packed['presentationRequested'] is packed['inputRequested'] is packed['sourceDiskAttached'] is False and
                packed['genericInitializationOracleCoversShader'] is False, 'Wrong private background pack')
        require(packed['buildReceiptSha256'] == sha(folder / 'build-receipt.json') and packed['probeSha256'] == probe_hash and
                packed['initramfsSha256'] == sha(folder / 'private-vulkan-compute.cpio.gz'), 'Pack/build changed')
        payload = cpio_hashes(folder / 'private-vulkan-compute.cpio.gz')
        require(payload['guest-probe'] == probe_hash, 'Embedded probe differs')
        for name, key in (('init', 'rootInitSha256'), ('wddm-aperture.ko', 'moduleSha256'),
            ('aperture-contract', 'contractSha256'), ('linux-ioctl-bridge.so', 'shimSha256'), ('guest-graphics-broker', 'brokerSha256')):
            require(payload[name] == packed[key], 'Embedded runtime differs: ' + name)
        for name, metadata in snapshot['files'].items():
            require([digest for path, digest in payload.items() if path.endswith('/' + name)] == [metadata['sha256']],
                    'Embedded NVIDIA runtime changed')
        require([digest for path, digest in payload.items() if path.endswith('/libvulkan_dzn.so')] == [sha(icd)], 'Embedded ICD differs')
        packs[folder.name], payloads[folder.name] = packed, payload
    a, b = payloads[original.name], payloads[controlled.name]
    require(set(a) == set(b) and [name for name in a if a[name] != b[name]] == ['guest-probe'],
            'Non-probe payload contents changed for the corruption control')
    rows, commands = [], []
    for number, profile, slots, folder, runner in (
        (1, 'slot-refusal', 128, original, 'run-background-vulkan-compute-v1.py'),
        (2, 'pass', 256, original, 'run-background-vulkan-compute-v2.py'),
        (3, 'pass', 256, original, 'run-background-vulkan-compute-v2.py'),
        (4, 'corrupt-last', 256, controlled, 'run-background-vulkan-compute-v3.py')):
        run = lab / f'background-vulkan-compute-run-{number}'
        source, report = load(run / 'source.json'), load(run / 'driver.json')
        log = (run / 'driver.log').read_text()
        records = [json.loads(line) for line in (run / 'driver.host.log').read_text().splitlines() if line.startswith('{')]
        require(source['harnessUnchanged'] is source['artifactsUnchanged'] is True and
                source['readOnlyInstalledDriverBefore'] == source['readOnlyInstalledDriverAfter'] and
                source['readOnlyInstalledDriverAfter']['installedRuntimeMatchesSnapshot'] is True,
                'Runtime/source changed during run')
        require(source['runnerSha256'] == sha(lab / runner) and source['packReceiptSha256'] == sha(folder / 'pack-receipt.json'), 'Runner/pack changed')
        require(source['harnessSha256'] == sha(gpu / 'broker_runtime_qemu.py'), 'Harness changed')
        for name, digest in source['harnessComponents'].items(): require(sha(gpu / name) == digest, 'Harness component changed')
        require(source['actualWorkload'] == ('vulkan-compute-corrupt-last' if number == 4 else 'vulkan-compute') and
                source['genericHarnessWorkload'] == 'init' and source['genericInitializationOracleCoversShader'] is False and
                report['diagnosticAccepted'] is False and source['generalVerifierExit'] == 1, 'Generic rejection/scope differs')
        require(report['driverBridgeSha256'] == source['nativeSha256'] == NATIVE and report['qemuSha256'] == QEMU and
                report['kernelSha256'] == KERNEL and report['initramfsSha256'] == source['initramfsSha256'] ==
                packs[folder.name]['initramfsSha256'], 'Native/QEMU/kernel/image changed')
        require(report['liveNvidiaLinuxUmdLoaded'] is True and report['realWslDxgForwarding'] is False and
                report['capturedPrivateFixturesUsed'] is False and report['privateRuntimeImageRedistributable'] is False and
                report['hypervisor'] == 'QEMU/WHPX', 'Missing genuine runtime provenance')
        validate_compute_controls(source, log, records, slots)
        proof = validate_compute_receipts(log, profile)
        cleanup = validate_refusal_retention(report, records) if number == 1 else validate_compute_cleanup(report, records, slots, profile)
        command = list(source['command'])
        for option in ('--report', '--driver-cpu-slots'):
            command[command.index(option) + 1] = '<' + option[2:] + '>'
        commands.append(command)
        rows.append(dict(run=number, proof=proof, cleanup=cleanup, originalGenericAcceptance=False,
            initramfsSha256=source['initramfsSha256'], privateReceiptHashes={name: sha(run / name) for name in
                ('source.json', 'driver.json', 'driver.log', 'driver.host.log', 'driver.errors.log')}))
    require(commands[0] == commands[1] == commands[2], 'Positive repeat changed more than CPU virtual-slot selection/report destination')
    return dict(schema=1, scope='4K offscreen Vulkan compute and full integer readback; no window/raster/desktop acceptance',
        runs=rows, width=3840, height=2160, wordsPerRound=8294400, positiveRoundsPerRun=2,
        identicalPositiveProbeIcdAndImages=True, defaultShaderAndProbeByteIdenticalAfterControlAdded=True,
        corruptionPayloadContentDifference=['guest-probe'], cpuByteGuardUnchanged=True, gpuByteGuardUnchanged=True,
        independentExpectedHashes=reference, nativeSha256=NATIVE, qemuSha256=QEMU, kernelSha256=KERNEL,
        icdSha256=sha(icd), normalSpirvSha256=normal['spirvSha256'], corruptSpirvSha256=corrupt['spirvSha256'],
        publicSourceHashes={name: sha(gpu / name) for name in ('vulkan_compute.comp', 'vulkan_compute_probe.cpp',
            'vulkan_compute_workload.h', 'build_vulkan_compute_probe.py', 'vulkan_compute_reference.py',
            'vulkan_compute_evidence.py', 'verify_vulkan_compute.py')},
        vulkanComputeVerified=True, vulkanGraphicsRasterizationVerified=False, vulkanWindowPresentationVerified=False,
        deviceLossRecoveryVerified=False, slotRefusalRecoveryVerified=False, performanceMeasured=False, dailyUseAccepted=False)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lab', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True, help='New sanitized result; never overwrite')
    args = parser.parse_args()
    value = audit(args.lab.resolve())
    with args.output.open('x', encoding='utf-8', newline='\n') as output:
        json.dump(value, output, indent=2); output.write('\n')
    print('PASS: repeated full 4K Vulkan compute; final-word corruption rejected; slot/refusal retention preserved. No window/desktop acceptance.')
