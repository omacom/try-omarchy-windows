"""Audit guarded, on-demand staging in a real UWSM/NVIDIA desktop capture."""
import re
from verify_hyprland_lifecycle_capture import require
from verify_uwsm_gpu_session import validate_uwsm_gpu_capture


LIMIT = 1073741824


def staging_admission(records, backpressure):
    positions = {id(row): index for index, row in enumerate(records)}
    position = lambda row: positions[id(row)]
    checks = [row for row in records if 'nativePresentationMemoryGuard' in row]
    require(checks and all(row.get('nativePresentationMemoryGuard') is True and row.get('accepted') is True and
            row.get('limitBytes') == LIMIT and
            all(type(row.get(key)) is int and 0 <= row[key] <= LIMIT for key in ('usageBytes', 'additionalBytes')) and
            row['usageBytes'] + row['additionalBytes'] <= LIMIT for row in checks),
            'Missing, failed or overflowing staging/presentation admission')
    prefix = ['device-before', 'device-after', 'staging-before', 'staging-after',
              'readback-before', 'readback-after', 'swapchain-before', 'swapchain-after']
    stages = [row.get('stage') for row in checks]
    require(stages[:len(prefix)] == prefix and set(stages) <= set(prefix + [
            'frame-before', 'frame-admitted', 'import-before', 'import-after']),
            'Missing/reordered/unknown demand-staging guard')
    require(type(backpressure) is int and 0 <= backpressure <= 4096 and
            stages.count('frame-before') == 12 + backpressure and stages.count('frame-admitted') == 12 and
            1 <= stages.count('import-before') == stages.count('import-after') <= 3,
            'Missing per-frame/import guard')
    for row in checks:
        stage, size = row['stage'], row['additionalBytes']
        require(33177600 <= size <= 64 * 1024 * 1024 if stage == 'staging-before' else
                256 * 3840 * 4 <= size <= 4 * 1024 * 1024 if stage == 'readback-before' else
                2 * 33177600 <= size <= 128 * 1024 * 1024 if stage == 'swapchain-before' else size == 0,
                'Missing heap estimate or double-charged source import')
    opened = [row for row in records if 'nativeCompositorRingOpened' in row]
    require(len(opened) == 1 and opened[0].get('nativeCompositorRingOpened') is True and
            opened[0].get('slots') == opened[0].get('sourceCacheLimit') == 3 and
            opened[0].get('onDemandStaging') is True and opened[0].get('initialStagingTextures') == 1 and
            opened[0].get('ordinaryCpuReadback') is False,
            'Missing demand-staging agreement or reduced ring capacity')
    allocations = [row for row in records if 'nativeCompositorStagingAllocated' in row]
    require(1 <= len(allocations) <= 3 and [row.get('slot') for row in allocations] == list(range(len(allocations))),
            'Duplicate, missing or out-of-order staging allocation')
    staging_checks = [row for row in checks if row['stage'].startswith('staging-')]
    require(len(staging_checks) == 2 * len(allocations), 'Unaccounted staging allocation/guard')
    total = 0
    for index, row in enumerate(allocations):
        before, after = staging_checks[index * 2:index * 2 + 2]
        require(row.get('nativeCompositorStagingAllocated') is True and row.get('onDemand') is True and
                row.get('capacity') == 3 and type(row.get('bytes')) is int and
                row['bytes'] == before['additionalBytes'] and
                (before['stage'], after['stage']) == ('staging-before', 'staging-after') and
                position(before) < position(after) < position(row),
                'Staging published before complete native memory admission')
        total += row['bytes']
        require(row.get('preparedTextures') == index + 1 and row.get('preparedBytes') == total,
                'Staging allocation totals do not reconcile')
        require(row.get('phase') == ('initialization' if index == 0 else 'serialized-submit') and
                (position(row) < position(opened[0]) if index == 0 else position(opened[0]) < position(row)),
                'Staging created at the wrong initialization/submit phase')
    inputs = [row for row in records if row.get('nativeCompositorRingInput') is True]
    used = {row.get('slot') for row in inputs}
    require(len(inputs) == 12 and used == {row['slot'] for row in allocations} and
            all(type(row.get('slot')) is int and 0 <= row['slot'] < 3 and
                position(allocations[row['slot']]) < position(row) for row in inputs),
            'Unused eager texture or frame references unprepared staging')
    previous = position(checks[len(prefix) - 1])
    for row in inputs:
        block = ';'.join(check['stage'] for check in checks if previous < position(check) < position(row)) + ';'
        require(re.fullmatch(r'(?:frame-before;)+(?:staging-before;staging-after;)?(?:import-before;import-after;)?frame-admitted;', block),
                'Frame copy precedes its own ordered admission or allocation guards')
        previous = position(row)
    require(position(checks[-1]) < previous, 'Uncompleted presentation admission after final handoff')
    stopped = [row for row in records if 'nativeCompositorRingStopped' in row]
    require(len(stopped) == 1 and stopped[0].get('nativeCompositorRingStopped') is True and
            stopped[0].get('onDemandStaging') is True and
            stopped[0].get('stagingTexturesCreated') == stopped[0].get('stagingTexturesReleased') == len(allocations) and
            stopped[0].get('stagingTextureBytes') == total and stopped[0].get('liveGpuResources') == 0,
            'Staging not fully reclaimed after actual GPU retirement')
    return dict(onDemandStagingVerified=True, capacity=3, initialTextures=1,
                preparedTextures=len(allocations), preparedTextureBytes=total,
                startupTextureBytesAvoided=2 * allocations[0]['bytes'],
                unusedCapacityBytesAvoided=3 * allocations[0]['bytes'] - total,
                stagingTexturesReleased=len(allocations), gpuByteGuard=LIMIT,
                physicalGpuQuotaVerified=False)


def validate_demand_staging(report, guest, records, frames, source, wallpaper_reference):
    # The frozen UWSM/GPU/pixel/lifetime auditor remains mandatory and unchanged.
    capture = validate_uwsm_gpu_capture(report, guest, records, frames, source, wallpaper_reference)
    staging = staging_admission(records, report['disconnectCleanup']['compositorBackpressure'])
    return dict(uwsm=capture, staging=staging, guardedDemandStagingCaptureVerified=True,
                normalLauncherVerified=False, fullDesktopAccepted=False,
                nearNativePerformanceVerified=False)
def terminal_retirement(guest, records, compositor_pid):
    positions = {id(row): index for index, row in enumerate(records)}
    position = lambda row: positions[id(row)]
    stopped = re.findall(r'^BROKER_COORDINATOR_GROUP_STOPPED primaryPid=(\d+) workersJoined=true listenerClosed=true primaryPidfdExited=true registryEmpty=true$', guest, re.M)
    completed = re.findall(r'^BROKER_COORDINATOR_PRIMARY_RETIRED destroyedWireObjects=(\d+) steps=(\d+) groupSealed=true$', guest, re.M)
    require(stopped == [str(compositor_pid)] and len(completed) == 1,
            'Missing independently pinned terminal group lifecycle')
    actor = re.findall(r'^BROKER_COORDINATOR_ACTOR actor=(\d+) pid=' + str(compositor_pid) +
                       r' primary=true pidfdAuthenticated=true nativeDriverCount=1$', guest, re.M)
    require(len(actor) == 1, 'Missing authenticated presentation owner')
    actor_id = int(actor[0])
    root_stop = guest.index('BROKER_COORDINATOR_GROUP_STOPPED ')
    root_complete = guest.index('BROKER_COORDINATOR_PRIMARY_RETIRED ')
    actor_exits = list(re.finditer(r'^BROKER_COORDINATOR_ACTOR_EXIT actor=\d+ empty=(?:true|false) retainedUntilVmExit=(?:true|false)$', guest, re.M))
    require(len(actor_exits) == 7 and all(row.end() < root_stop for row in actor_exits) and
            root_stop < root_complete < guest.index('BROKER_COORDINATOR_EXIT ') <
            guest.index('NORMAL_ROOT_GRAPHICS_EXIT result=0 normalOmarchySession=false'),
            'Terminal group control precedes actual actor drainage or follows service exit')
    steps = [row for row in records if 'nativeStoppedGroupRetirement' in row]
    rings = [row for row in records if row.get('nativeCompositorRingStopped') is True]
    require(len(rings) == 1 and 1 <= len(steps) <= 512, 'Missing bounded terminal GPU retirement')
    previous, phase, destroyed = None, 0, 0
    for row in steps:
        require(row.get('nativeStoppedGroupRetirement') is True and row.get('primaryActor') == actor_id and
                row.get('presentationDrained') is True and row.get('groupSealed') is True and
                row.get('ordinaryClientOpcode') is False and position(rings[0]) < position(row) and
                all(type(row.get(key)) is int for key in ('phase', 'remaining', 'attempted', 'destroyed', 'sharedReferences', 'flags', 'ntstatus')),
                'Terminal retirement lacks a prior actual presentation drain')
        require(phase <= row['phase'] <= 4 and 0 <= row['remaining'] <= 1024 and
                0 <= row['destroyed'] <= row['attempted'] <= 16 and row['sharedReferences'] == 0 and
                row['flags'] in (0, 1, 4) and row['ntstatus'] <= 0 and
                (bool(row['flags'] & 4) == bool(row['ntstatus'])) and
                (row['flags'] != 1 or row['attempted'] == row['destroyed'] == 0) and
                (previous is None or previous == row['remaining'] + row['destroyed']),
                'Unsafe, inconsistent or unbounded terminal retirement batch')
        previous, phase = row['remaining'], row['phase']
        destroyed += row['destroyed']
    require(previous == 0 and phase == 4 and not steps[-1]['flags'] and not steps[-1]['ntstatus'] and
            tuple(map(int, completed[0])) == (destroyed, len(steps)),
            'Terminal native ownership remains or broker/native receipts disagree')
    groups = [row for row in records if row.get('nativeCoordinatorSummary') is True]
    require(len(groups) == 1 and groups[0]['retainedActors'] == groups[0]['liveObjectsBeforeDisconnect'] == 0 and
            position(steps[-1]) < position(groups[0]),
            'Terminal cleanup still depends on the owner disconnect')
    return dict(terminalGroupRetirementVerified=True, presentationOwner=actor_id,
                primaryObjectsRetired=destroyed, steps=len(steps), retainedActors=0,
                presentationDrainedBeforePrimaryObjects=True, groupSealed=True,
                ordinaryClientOpcode=False)


def validate_terminal_demand_staging(report, guest, records, frames, source, wallpaper_reference):
    result = validate_demand_staging(report, guest, records, frames, source, wallpaper_reference)
    result['terminal'] = terminal_retirement(guest, records, result['uwsm']['session']['compositorPid'])
    result['terminalDemandStagingCaptureVerified'] = True
    return result
