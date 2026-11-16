"""Audit bounded Hyprland lifecycle only; never accept compositor pixels/display."""
import argparse
import json
from pathlib import Path
import re


def require(condition, message):
    if not condition:
        raise ValueError(message)


def validate(report, guest, *, profile=4,normal_root=False):
    require(type(profile) is int and profile in (4,5),'Unsupported expected compositor profile')
    require(type(normal_root) is bool,'Invalid expected root lifecycle')
    lines = guest.splitlines()
    def once(line):
        require(lines.count(line) == 1, 'Missing or duplicate lifecycle receipt: ' + line)
        return lines.index(line)

    require(report.get('liveNvidiaLinuxUmdLoaded') is True and
            report.get('realWslDxgForwarding') is False and
            report.get('capturedPrivateFixturesUsed') is False, 'Wrong GPU provenance')
    require(not any(text in guest for text in ('BRIDGE_RUNTIME_SIGNAL', 'HYPRLAND_STARTUP_STOP',
            'Oops:', 'BUG:', 'cleanup=false')), 'Compositor signal, forced stop or cleanup failure')
    uid=1000 if normal_root else 65534
    user = once(f'BROKER_RENDER_CLIENT uid={uid} gid={uid} effectiveCapabilities=0 noNewPrivs=true controllerOpen=denied nativePortOpen=denied')
    start = once(f'HYPRLAND_STARTUP_BEGIN version=0.56.2 boundedProfile={profile} actualCompositor=true windowsPresentation=false')
    main = once('HYPRLAND_STARTUP_MAIN_RETURN exit=0 windowsPresentation=false')
    exited = once('NORMAL_ROOT_GRAPHICS_EXIT result=0 normalOmarchySession=false' if normal_root else 'BRIDGE_RUNTIME_EXIT=0')
    commits = re.findall(r'^HYPRLAND_COMPOSITOR_COMMIT index=(\d+) width=(\d+) height=(\d+) format=(\d+) stride=(\d+) modifier=(\d+) ownedNativeBuffer=true pixelsVerified=false windowsPresentation=false$', guest, re.M)
    require(len(commits) >= 2 and guest.count('HYPRLAND_COMPOSITOR_COMMIT ') == len(commits), 'Incomplete/mixed buffer commits')
    for index, values in enumerate(commits, 1):
        require(tuple(map(int, values)) == (index, 640, 360, 875713112, 2560, 72057594037927935), 'Invalid compositor buffer metadata/order')
    first = next(i for i, line in enumerate(lines) if line.startswith('HYPRLAND_COMPOSITOR_COMMIT '))
    last = max(i for i, line in enumerate(lines) if line.startswith('HYPRLAND_COMPOSITOR_COMMIT '))
    require(user < start < first <= last < main < exited, 'Invalid compositor lifecycle order')
    clean, vm = report['disconnectCleanup'], report['ownedQemuControl']
    require(report.get('hostBridgeExit') == 0 and clean.get('driverCleanupVerified') is True, 'Native cleanup failed')
    require(vm.get('ownedQemuExited') is True and vm.get('qemuExit') == 0 and
            vm.get('qemuForcedStop') is False and vm.get('fenceControlFailed') is False, 'VM did not exit cleanly')
    require(all(type(value) is int and value == 0 for key, value in clean.items()
                if key.startswith(('failed', 'live', 'pending', 'forced')) or key.endswith(('Timeouts', 'ReleasedAfterVmExit'))), 'Native live/failure/timeout/deferred state')
    require(clean.get('completedNativeSubmissions', 0) > 0, 'No retired native GPU work')
    for kind in ('fence', 'allocation', 'allocationSlot'):
        require(vm[kind+'MappingsCreated'] > 0 and
                vm[kind+'MappingsCreated'] == vm[kind+'UnmapAcknowledgements'] and
                vm[kind+'MappingsReleasedAfterVmExit'] == 0, 'Incomplete explicit mapping cleanup: ' + kind)
    require(all(value == 0 for key, value in vm.items() if key.startswith('live')), 'VM retains GPU mappings')
    zero_views='LINUX_BRIDGE processExitViewsRetained=true pagingFences=0 hwQueueFences=0 syncFences=0 allocationViews=0 release=kernel-process-exit'
    if normal_root:
        # Installed helpers can inherit LD_PRELOAD without opening the GPU
        # broker. Check every shim finalizer, plus the exact routed view and
        # native unmap totals below; no nonzero/partial receipt is ignored.
        views=[line for line in lines if 'LINUX_BRIDGE processExitViewsRetained=' in line]
        require(len(views)>0 and all(line==zero_views for line in views),'Real-root shim retains views or has an incomplete receipt')
    else:once(zero_views)
    broker = re.findall(r'^BROKER_ROUTE_EXIT processExited=true retainedViews=0 nativeCalls=(\d+) viewsCreated=(\d+) viewsReleased=(\d+) rejected=0 multiClient=false$', guest, re.M)
    require(len(broker) == 1 and int(broker[0][0]) > 0 and int(broker[0][1]) > 0 and broker[0][1] == broker[0][2], 'Broker views not explicitly released')
    once('BROKER_ROUTE_REAP exit=0')
    once('KERNEL_RENDER_EXIT unloaded=true nodeRemoved=true')
    once('KERNEL_OBJECT_EXIT held=0 active=0 cleanup=true')
    once('KERNEL_APERTURE_EXIT activeLeases=0 activeCpuBytes=0 mmioReads=0 mmioWrites=0 unloaded=true')
    buffers = re.findall(r'^KERNEL_NATIVE_BUFFER_EXIT held=0 bytes=0 created=(\d+) released=(\d+) cleanup=true$', guest, re.M)
    sync = re.findall(r'^KERNEL_SYNC_FILE_EXIT held=0 pending=0 created=(\d+) signaled=(\d+) errors=0 released=(\d+) cleanup=true$', guest, re.M)
    require(len(buffers) == 1 and int(buffers[0][0]) > 0 and buffers[0][0] == buffers[0][1], 'Native buffer cleanup mismatch')
    require(len(sync) == 1 and int(sync[0][0]) > 0 and sync[0][0] == sync[0][1] == sync[0][2] and
            clean.get('acceptedHwQueueSignals') == clean.get('completedHwQueueSignals') == int(sync[0][0]), 'GPU sync-file retirement mismatch')
    imports = report.get('nativeSharedResourceImportChecks', [])
    require(len(imports) == 3 and all(item.get('verified') is True and
            all(item.get(key) == value for key, value in dict(ntstatus=0, deviceHresult=0, importHresult=0,
                width=640, height=360, format=87, dimension=3, flags=1).items()) and
            item.get('sharedNtHandleClosed') is True and item.get('importedResourceReleased') is True
            for item in imports), 'Swapchain import metadata mismatch')
    return dict(schema=1, compositorLifecycleVerified=True, compositorPixelsVerified=False,
        windowsPresentationVerified=False, desktopVerified=False, nearNativePerformanceVerified=False,
        actualCompositorVersion='0.56.2', width=640, height=360, bufferCommits=len(commits),
        completedNativeSubmissions=clean['completedNativeSubmissions'], syncFilesReleased=int(sync[0][2]),
        nativeBuffersReleased=int(buffers[0][1]), brokerViewsReleased=int(broker[0][2]),
        explicitFenceUnmaps=vm['fenceUnmapAcknowledgements'], explicitAllocationUnmaps=vm['allocationUnmapAcknowledgements'],
        explicitAllocationSlotUnmaps=vm['allocationSlotUnmapAcknowledgements'], forcedVmStop=False,
        cleanupDeferredUntilVmExit=False, generalOpenGlWorkloadAccepted=report.get('diagnosticAccepted') is True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = validate(json.loads(args.report.read_text()), args.report.with_suffix('.log').read_text())
    with args.output.open('x') as output:
        json.dump(result, output, indent=2)
        output.write('\n')
    print(json.dumps(result, indent=2))
