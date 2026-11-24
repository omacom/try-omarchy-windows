"""Audit rejected, real two-application attempts without granting acceptance."""
from aperture_admission_audit import admission_snapshots
from verify_coordinated_gpu_clients import ROOT
from verify_hyprland_lifecycle_capture import require
from virtio_trace import merge_session

BRIDGES = {
    82: '260def3126c56c8dbfcf8a4be400788c2300fb163a5115af47fbfc95c5544e1d',
    83: '91684f518eb7a005662e3a01c6edd40a781249870ead298dbeef4fc832320ef8',
    84: '90c39fe79c87a494e31a8d7461ace6e613b4d8280bb344473386a349c4157284',
}
GUESTS = {
    137: '88ea483d06bcf05fa174545bb537e72e19546af64126da4c6ed4fbc2ec9de37b',
    138: '3d7ca14883fbd01bcca9305fc30655cb0e2ffca190a2dc58481063ccb7ee2025',
    139: 'db235c047fa88373b032f06c5356435a6cecc4bacbcdc49a785d92e08e14e071',
    140: 'e39e0bbec818a05701b4b416bd40df2f21d09cf784928d7ad99ad523c2d8ff3c',
    141: '3bc07c50b5bc9f2fa3f7295702fb898ada232a8107afc5359f95de344ca505e8',
}


def validate_rejected_multiwindow(number, report, guest, records, source, uart, trace):
    require(number in GUESTS, 'Unknown retained multiwindow attempt')
    native = 82 if number <= 139 else 83 if number == 140 else 84
    version = 6 if number <= 139 else 7 if number == 140 else 8
    clean, vm, manifest = report['disconnectCleanup'], report['ownedQemuControl'], source['manifest']
    require(report['driverBridgeSha256'] == source['nativeBuildReceipt']['bridgeSha256'] == BRIDGES[native] and
            report['initramfsSha256'] == manifest['initramfsSha256'] == GUESTS[number] and
            report['qemuSha256'] == '6bee9d9e18bc0e89757e0364a29e64f1057e4854ec9a7a816b063f8d8f3ed6a5' and
            source['managedMappingNegotiationVersion'] == version and
            manifest['managedGpuVaPolicy']['coordinatorObjectsVersion'] == version,
            'Mixed candidate, guest or negotiated version')
    require(source['traceMode'] == 'virtio' and source['sourceImageUnchanged'] is
            source['nativeSourcesUnchanged'] is source['qemuArtifactUnchanged'] is
            source['activeFrameProbeSourcesUnchanged'] is source['multiwindowSourcesUnchanged'] is
            source['traceProbeSourcesUnchanged'] is True and source['memoryGuardRaised'] is False and
            source['sourceImageSha256Before'] == source['sourceImageSha256After'] == ROOT and
            manifest['ordinaryUid'] == 1000 and manifest['expectedFrames'] == 1800 and
            manifest['desktopGeometryProbe'] == [3840, 2160], 'Changed owned workload or source guard')
    installed = dict(installedRuntimeMatchesSnapshot=True, driverVersion='32.0.16.2101',
                     driverStorePackage='nvami.inf_amd64_00e5dc1fb00a1261', libraryCount=2)
    require(source['installedNvidiaRuntimeBefore'] == source['installedNvidiaRuntimeAfter'] == installed,
            'Installed NVIDIA runtime changed')
    merged, integrity = merge_session(uart, trace)
    require(guest == merged.replace('\r\n', '\n').replace('\r', '\n'), 'Incomplete attempt trace')
    require(vm['ownedQemuExited'] is True and vm['qemuExit'] == 0 and
            vm['qemuForcedStop'] is vm['fenceControlFailed'] is False and
            all(v == 0 for k, v in vm.items() if k.startswith('live')),
            'Owned VM failed to exit naturally or retained mappings')
    require(clean['completedCompositorFrames'] == 0 and
            not any(r.get('nativeSdlCompositorQueued') for r in records) and
            'NORMAL_ROOT_GRAPHICS_EXIT result=125 normalOmarchySession=false' in guest and
            'HYPRLAND_HOST_QUEUE_END frames=1800 target=1800 failed=0' not in guest,
            'Rejected workload was promoted to a rendered desktop')
    agreements = [r for r in records if r.get('nativeCoordinatedObjectCapacity')]
    expected = dict(version=version, hwQueues=64 if version == 8 else 32,
                    syncObjects=768 if version == 8 else 384, kernelLeaseLimit=1536 if version >= 7 else 768,
                    vendorAllocationObjects=1024, cpuBytes=201326592, reportedGpuBytes=1073741824,
                    memoryBudgetsMultiplied=False, reservationMappingsSeparated=True)
    require(len(agreements) == 1 and all(agreements[0].get(k) == v for k, v in expected.items()),
            'Wrong exact object or physical memory agreement')
    require(clean['vendorCpuMappedByteLimit'] == 201326592 and clean['peakVendorCpuBytes'] <= 201326592 and
            clean['reportedGpuUsageLimitBytes'] == 1073741824 and clean['peakReportedGpuUsageBytes'] <= 1073741824 and
            clean['hwQueueLimit'] == expected['hwQueues'] and clean['syncObjectLimit'] == expected['syncObjects'] and
            clean['syncQuotaRejections'] == 0, 'A byte guard or object limit differs')
    checks = 4716 if version >= 7 else 1644
    require(f'APERTURE_CONTRACT_PASS checks={checks} activeLeases=0 activeCpuBytes=0' in guest,
            'Boot contract did not restore an empty aperture')
    snapshots = admission_snapshots(guest)
    reason = 'kernel-view-enospc-cause-unobserved'
    companion = False
    if number <= 139:
        require('BROKER_ROUTE rejected=true errno=28 operation=3 reason=Broker kernel view create: No space left on device' in guest and
                clean['hwQueueQuotaRejections'] == clean['vendorCpuByteQuotaRejections'] == 0,
                'Missing kernel admission failure')
        if number == 139:
            live = [r for r in snapshots if r['kind'] == 1 and r['activeLeases'] == r['leaseLimit'] == 768 and
                    0 < r['activeCpuBytes'] < r['cpuByteLimit'] and r['cpuBytesExceeded'] == r['cookieExhausted'] == 0]
            require(len(live) == 1 and live[0]['activeCpuBytes'] == clean['peakVendorCpuBytes'],
                    'Missing atomic runtime lease exhaustion; boot self-test is insufficient')
            reason = 'kernel-lease-records-exhausted'
    elif number == 140:
        require(clean['peakHwQueues'] == 32 and clean['hwQueueQuotaRejections'] == 1 and
                clean['vendorCpuByteQuotaRejections'] == 0 and
                'Native owner still has a CPU view: Device or resource busy' in guest,
                'Missing queue quota and guarded rollback')
        reason = 'hardware-queue-records-exhausted'
    else:
        require(clean['hwQueueQuotaRejections'] == 0 and clean['peakHwQueues'] > 32 and
                clean['vendorCpuByteQuotaRejections'] > 0 and clean['peakVendorCpuBytes'] > 201326592 - 65536 and
                'omarchy-gpu-companion[' in guest and
                any('omarchy-gpu-companion[' in l and 'Running on D3D12 (NVIDIA GeForce RTX 5090 Laptop GPU)' in l
                    for l in guest.splitlines()), 'Missing NVIDIA companion initialization or CPU quota')
        require(report['hostBridgeExit'] == 1 and clean['driverCleanupVerified'] is False and
                clean['pendingHwQueueSignals'] > 0 and clean['pendingGpuQueueWaits'] > 0 and
                clean['liveVendorCpuBytes'] > 0 and vm['allocationMappingsReleasedAfterVmExit'] > 0,
                'Lost the retained failure and incomplete native teardown')
        reason, companion = 'cpu-mapping-byte-quota-and-incomplete-native-teardown', True
    if number != 141:
        require(report['hostBridgeExit'] == 0 and clean['driverCleanupVerified'] is True and
                all(v == 0 for k, v in clean.items() if k.startswith(('live', 'pending'))) and
                all(v == 0 for k, v in vm.items() if k.endswith('ReleasedAfterVmExit')),
                'Clean rejection retained native or QEMU ownership')
    return dict(run=number, rejection=reason, exactAgreement=expected, traceIntegrity=integrity,
                atomicAdmissionSnapshots=snapshots, companionNvidiaRendererInitialized=companion,
                completedFrames=0, naturalVmExit=True, nativeCleanupVerified=number != 141,
                fullDesktopAccepted=False, multiwindowDesktopAccepted=False,
                physicalInputVerified=False, nearNativePerformanceVerified=False)
