"""Strict isolation/teardown evidence for the single-client runtime broker."""
import re


def broker_runtime_complete(log):
    lines = log.replace('\r\n', '\n').splitlines()

    def one(prefix, pattern):
        rows = [line for line in lines if line.startswith(prefix + ' ')]
        if len(rows) != 1 or len(rows[0]) > 512:
            return None
        return re.fullmatch(pattern, rows[0])

    if 'transport=virtio-port' in log:
        return False
    required = (
        ('KERNEL_APERTURE_START', r'KERNEL_APERTURE_START loaded=true contracts=true rawSysfsFallback=false'),
        ('BROKER_RENDER_CLIENT', r'BROKER_RENDER_CLIENT uid=65534 gid=65534 effectiveCapabilities=0 noNewPrivs=true controllerOpen=denied nativePortOpen=denied'),
        ('BROKER_ROUTE_START', r'BROKER_ROUTE_START protocol=3 clientPid=([1-9][0-9]*) clientUid=65534 nativeWorkerClients=1 kernelControllersClient=false'),
        ('BROKER_OBJECT_DRAIN', r'BROKER_OBJECT_DRAIN producerExited=true retained=0'),
        ('BROKER_ROUTE_REAP', r'BROKER_ROUTE_REAP exit=0'),
        ('KERNEL_APERTURE_EXIT', r'KERNEL_APERTURE_EXIT activeLeases=0 activeCpuBytes=0 mmioReads=0 mmioWrites=0 unloaded=true'),
    )
    matches = [one(prefix, pattern) for prefix, pattern in required]
    if any(match is None for match in matches):
        return False
    if int(matches[2][1]) > (1 << 31) - 1:
        return False
    transports = [line for line in lines if line.startswith('LINUX_BRIDGE transport=')]
    if transports != ['LINUX_BRIDGE transport=authenticated-broker nativeWorkerClients=1']:
        return False
    end = one('BROKER_ROUTE_EXIT', r'BROKER_ROUTE_EXIT processExited=true retainedViews=0 nativeCalls=([1-9][0-9]*) viewsCreated=([0-9]+) viewsReleased=([0-9]+) rejected=0 multiClient=false')
    if end is None:
        return False
    calls, created, released = map(int, end.groups())
    ordered = [matches[0][0], matches[1][0], matches[2][0], transports[0],
               matches[3][0], end[0], matches[4][0], matches[5][0]]
    positions = [lines.index(row) for row in ordered]
    return (positions == sorted(positions) and 0 < calls < (1 << 64) and
            0 < created == released < (1 << 64))


def runtime_transport_complete(log):
    # Seeing any broker transport label forbids falling back to a direct-port
    # success token when the broker's isolation or teardown evidence is invalid.
    if 'transport=authenticated-broker' in log:
        return broker_runtime_complete(log)
    return 'transport=virtio-port' in log


def broker_aware_initialization_complete(log, owner_exit, control, cleanup):
    """Explicit broker-aware predicate; the digest-pinned legacy harness stays intact."""
    return (owner_exit == 0 and all(marker in log for marker in (
        'factory=00000000', 'list=00000000', 'nvidia vendor=4318',
        'nvidiaUmdPresentDuringPrivateQuery=true', 'LINUX_RUNTIME_NVIDIA_UMD_STILL_LOADED=true',
        'realDxgForwarding=false')) and runtime_transport_complete(log) and
        all(re.findall(r'^' + re.escape(prefix) + r'=([0-9a-f]{8})[ \t\r]*$', log, re.MULTILINE) == ['00000000']
            for prefix in ('device', 'copyQueue')) and
        re.findall(r'^BRIDGE_RUNTIME_EXIT=(\d+)[ \t\r]*$', log, re.MULTILINE) == ['0'] and
        control.get('ownedQemuExited') is True and control.get('qemuExit') == 0 and
        control.get('qemuForcedStop') is False and control.get('fenceControlFailed') is False and
        cleanup.get('driverCleanupVerified') is True)
