"""Audit safe staging reclamation with the complete GPU input response."""
import json
import re

from verify_hyprland_lifecycle_capture import require
from verify_uwsm_native_input_devices import input_discovery
from verify_uwsm_gpu_session import validate_uwsm_gpu_capture
from verify_uwsm_demand_staging import terminal_retirement
from verify_qmp_input_response import GUEST_SOURCES, response_metadata

INPUT_SOURCES = {
    'driver_bridge.cpp': '3653623b88d2044ca97ea1e35654b8d39eb46c520f3ed4efc5c0b152db55c680',
    'qemu_runtime_host.h': '67fffa172f3b77218494895b67b1ca45aac02cc744f0783a6ace38804fcc2c66',
    'bounded_input_channel.h': '90ce9b3969afb5e9397455ee5949f831abc0b8c469427771b9410da53065761a',
    'input_delivery_diagnostic.h': '4aeaa2c4787754d7e06739af24cf57023ed66668220431fb9032b21a7f7aa13c',
    'shared_texture_presenter.h': '431532970ff3fbea37977c30a879f599e1171be4d42ab4d786c922cd77c8bb82',
    'compositor_presenter.h': '7ef89aecdfa584416b80195a180ae3bd09dd263598d88933e14f46d69ccb8089',
    'staging_reclaim.h': '5b0ed5493643c65e4f7ab15a98ce37713b6d6e27646779114c89555dbc1bdfa7',
    'tcp_peer_owner.h': '12e9683429d852a050fbf2433d7dedde8336b953f1f3a1f74387d12d12304f2a',
}



def snapshot(guest, label):
    values = re.findall(r'^' + re.escape(label) + r' ([0-9a-f]+)$', guest, re.M)
    require(len(values) == 1, 'Missing/duplicate actual input receipt: ' + label)
    return json.loads(bytes.fromhex(values[0]))


def delivery_metadata(guest, records, source, owner_output, guest_sources=GUEST_SOURCES):
    manifest = source['manifest']
    require(manifest.get('nativeInputDeliveryProbe') is True and
            manifest.get('files', {}).get('native-input-delivery-probe', {}).get('sha256') ==
            '9429bdcef594087ceb6a335004009a9669abe0ab804c2f872fb86af5c9a4888e',
            'Missing the explicit disposable PTY delivery fixture')
    for name, digest in guest_sources.items():
        require(manifest['sources'].get(name) == digest, 'Input observer/PTY source differs from the verified guest: ' + name)
    require(manifest['files'].get('data/gpu-terminal-scene.sh', {}).get('sha256') == guest_sources['gpu-terminal-scene.sh'],
            'The installed terminal script is not the verified PTY reader')
    for name, digest in INPUT_SOURCES.items():
        require(manifest['sources'].get(name) == digest and
                source['nativeBuildReceipt']['sources'].get('experimental/windows-native-gpu/' + name) == digest,
                'Input source differs from the verified native/guest build: ' + name)
    require(source.get('ownedRuntimeHarnessSha256') == '3f87e607ddf7cbf119d9315448ce5f3f2f3051231c04f53b736d38cfffc7e49d' and
            all(source['command'].count(flag) == 1 for flag in ('--driver-window-input', '--driver-input-self-test', '--driver-coordinator')),
            'Missing owned input harness or explicit native opt-ins')
    owners = [json.loads(line) for line in owner_output.splitlines() if line.startswith('{') and '"ownedQemuPid"' in line]
    opened = [row for row in records if row.get('nativeInputChannelOpened') is True]
    proof = [row for row in records if row.get('nativeInputDeliveryDiagnosticQueued') is True]
    stopped = [row for row in records if 'nativeInputChannelStopped' in row]
    require(len(opened) == len(proof) == len(stopped) == 1, 'Missing/duplicate native input lifecycle')
    windows = [row for row in records if row.get('nativeSharedTexturePresenterOpened') is True]
    require(len(windows) == 1 and windows[0].get('windowInputEnabled') is False and windows[0].get('inputSelfTestIsolated') is True,
            'The fixed input test was not isolated from unrelated Windows events')
    owner_pids = {row.get('ownedQemuPid') for row in owners}
    require(len(owner_pids) == 1 and opened[0].get('ownedQemuPid') in owner_pids and
            isinstance(opened[0].get('ownedQemuPid'), int) and opened[0]['ownedQemuPid'] > 0 and
            opened[0].get('socketPeerVerified') is True and opened[0].get('dedicatedQmp') is True and
            opened[0].get('queueCapacity') == 256, 'Input endpoint is not bound to the owned QEMU process')
    require(proof[0].get('afterGpuFrame') == 1 and proof[0].get('syntheticAtChannel') is True and
            proof[0].get('windowsMessagesVerified') is False and
            records.index(proof[0]) > next(i for i, row in enumerate(records) if row.get('nativeCompositorRingFrame') is True),
            'Input diagnostic did not follow an actual GPU frame')
    end = stopped[0]
    require(end.get('nativeInputChannelStopped') is True and end.get('workerJoined') is True and
            end.get('beforeOwnedVmStop') is True and end.get('acknowledgements') == 33 and end.get('resetCount', 0) >= 1,
            'Input commands did not acknowledge or the input worker did not stop cleanly')
    terminal = snapshot(guest, 'OMARCHY_NATIVE_INPUT_PTY_HEX')
    before = snapshot(guest, 'OMARCHY_NATIVE_INPUT_BEFORE_ACTIVE_HEX')
    after = snapshot(guest, 'OMARCHY_NATIVE_INPUT_AFTER_ACTIVE_HEX')
    clients = snapshot(guest, 'OMARCHY_GPU_APPLICATION_READY_CLIENTS_HEX')
    require(isinstance(clients, list) and len(clients) == 1 and before.get('pid') == after.get('pid') == clients[0].get('pid') and
            before.get('address') == after.get('address') == clients[0].get('address') and
            before.get('class') == after.get('class') == 'omarchy-gpu-terminal', 'The input target/focus is not the authenticated GPU terminal')
    require(terminal.get('terminalPid') == before.get('pid') and isinstance(terminal.get('shellPid'), int) and
            terminal['shellPid'] > 0 and terminal['shellPid'] != terminal['terminalPid'] and terminal.get('uid') == 1000 and
            isinstance(terminal.get('pty'), str) and re.fullmatch(r'/dev/pts/[0-9]+', terminal['pty']) and
            terminal.get('token') == 'omarchyinput93', 'The real application PTY did not receive the exact QMP key sequence')
    old_cursor = snapshot(guest, 'OMARCHY_NATIVE_INPUT_BEFORE_CURSOR_HEX')
    cursor = snapshot(guest, 'OMARCHY_NATIVE_INPUT_AFTER_CURSOR_HEX')
    require(all(isinstance(row, dict) and set(row) == {'x', 'y'} and
                all(type(row[axis]) in (int, float) for axis in ('x', 'y')) for row in (old_cursor, cursor)) and
            cursor != old_cursor and abs(cursor['x'] - 20000 * 3840 / 32767) <= 2 and
            abs(cursor['y'] - 20000 * 2160 / 32767) <= 2,
            'Missing actual cursor motion to the independently expected absolute position')
    return dict(qmpPtyTextReceivedVerified=True, qmpAbsolutePointerMotionVerified=True, focusRetained=True,
                terminalPid=terminal['terminalPid'], shellPid=terminal['shellPid'], pty=terminal['pty'], cursor=cursor,
                acknowledgedCommands=33, inputWorkerJoined=True, dedicatedQmp=True, socketPeerVerified=True,
                syntheticAtChannel=True, windowsMessagesVerified=False, pointerButtonDeliveryVerified=False,
                hostWindowIntegrationVerified=False, normalLauncherVerified=False, gpuProof=False)


def reclaimed_staging_admission(records, backpressure):
    limit, reserve = 1073741824, 64 * 1024 * 1024
    checks = [row for row in records if 'nativePresentationMemoryGuard' in row]
    require(checks and all(row.get('nativePresentationMemoryGuard') is True and row.get('accepted') is True and
            row.get('limitBytes') == limit and all(type(row.get(k)) is int and 0 <= row[k] <= limit for k in ('usageBytes', 'additionalBytes')) and
            row['usageBytes'] + row['additionalBytes'] <= limit for row in checks), 'Missing/overflowing presentation admission')
    prefix = ['device-before', 'device-after', 'staging-before', 'staging-after', 'readback-before', 'readback-after', 'swapchain-before', 'swapchain-after']
    stages = [row.get('stage') for row in checks]
    require(stages[:8] == prefix and set(stages) <= set(prefix + ['frame-before', 'frame-admitted', 'import-before', 'import-after']),
            'Missing/reordered presentation initialization')
    require(type(backpressure) is int and 0 <= backpressure <= 4096 and stages.count('frame-before') == 12 + backpressure and
            stages.count('frame-admitted') == 12 and 1 <= stages.count('import-before') == stages.count('import-after') <= 3,
            'Missing frame/import admission')
    for row in checks:
        size, stage = row['additionalBytes'], row['stage']
        require(33177600 <= size <= 64 * 1024 * 1024 if stage == 'staging-before' else
                256 * 3840 * 4 <= size <= 4 * 1024 * 1024 if stage == 'readback-before' else
                2 * 33177600 <= size <= 128 * 1024 * 1024 if stage == 'swapchain-before' else size == 0,
                'Wrong staging/readback/swapchain estimate')
    live, generations, last_inputs = {}, {}, {}
    created = trimmed = historical_bytes = live_bytes = pending_trim = 0
    staging_checks = [row for row in checks if row['stage'].startswith('staging-')]
    positions = {id(row): index for index, row in enumerate(records)}
    last_frame_position = positions[id(checks[7])]
    pressure_count = reallocated = 0
    opened = stopped = 0
    input_frames = []
    for position, row in enumerate(records):
        require(not stopped or not any(key in row for key in ('nativeCompositorStagingAllocated', 'nativeCompositorStagingTrimmed',
                'nativeCompositorRingInput', 'nativeResidencyStagingPressure')), 'Staging work after presentation teardown')
        if row.get('nativeCompositorStagingAllocated') is True:
            slot, size, generation = row.get('slot'), row.get('bytes'), row.get('generation')
            require(type(slot) is int and 0 <= slot < 3 and slot not in live and type(size) is int and size > 0 and type(generation) is int and
                    generation == generations.get(slot, 0) + 1 and row.get('capacity') == 3 and row.get('onDemand') is True,
                    'Staging creation collides with a live slot or replays its generation')
            require(created < 13 and len(staging_checks) >= (created + 1) * 2, 'Unbounded or unguarded staging creation')
            before, after = staging_checks[created * 2:created * 2 + 2]
            require(before['stage'] == 'staging-before' and after['stage'] == 'staging-after' and before['additionalBytes'] == size and
                    positions[id(before)] < positions[id(after)] < position, 'Staging published before memory admission')
            require((slot == 0 and generation == 1 and row.get('phase') == 'initialization') if created == 0 else
                    row.get('phase') == 'serialized-submit' and opened == 1, 'Staging created outside its serialized phase')
            created += 1
            historical_bytes += size
            live_bytes += size
            reallocated += int(generation > 1)
            live[slot] = (generation, size)
            generations[slot] = generation
            require(row.get('preparedTextures') == created and row.get('preparedBytes') == historical_bytes and
                    row.get('liveTextureBytes') == live_bytes, 'Staging byte accounting does not reconcile')
        if row.get('nativeCompositorRingOpened') is True:
            opened += 1
            require(opened == 1 and row.get('slots') == row.get('sourceCacheLimit') == 3 and row.get('onDemandStaging') is True and
                    row.get('initialStagingTextures') == 1 and row.get('ordinaryCpuReadback') is False and set(live) == {0},
                    'Changed staging capacity or eager startup allocation')
        if row.get('nativeCompositorRingInput') is True:
            slot, frame = row.get('slot'), row.get('frame')
            require(slot in live and type(frame) is int and 1 <= frame <= 12, 'Frame references unprepared staging')
            block = ';'.join(check['stage'] for check in checks if last_frame_position < positions[id(check)] < position) + ';'
            require(re.fullmatch(r'(?:frame-before;)+(?:staging-before;staging-after;)?(?:import-before;import-after;)?frame-admitted;', block),
                    'Input copy precedes its own memory admission')
            last_frame_position = position
            last_inputs[slot] = (live[slot][0], frame)
            input_frames.append(frame)
        if row.get('nativeCompositorStagingTrimmed') is True:
            slot, generation, size, reused = row.get('slot'), row.get('generation'), row.get('bytes'), row.get('reusedAfter')
            require(type(slot) is int and slot in (1, 2) and type(generation) is int and type(size) is int and
                    live.get(slot) == (generation, size) and last_inputs.get(slot) == (generation, reused) and
                    type(reused) is int and reused > 0 and row.get('slotOccupied') is False and row.get('capacity') == 3 and
                    row.get('serializedResidency') is True and row.get('commandsReset') is True and
                    all(type(row.get(k)) is int and reused <= row[k] <= 12 for k in ('inputCopyFenceObserved', 'outputCopyFenceObserved')),
                    'Staging reclaimed before actual copy retirement or with a stale owner')
            del live[slot]
            live_bytes -= size
            trimmed += 1
            pending_trim += size
            require(row.get('liveTextureBytes') == live_bytes and 0 in live, 'Reclamation removed the audit slot or miscounted bytes')
        if row.get('nativeResidencyStagingPressure') is True:
            usage, after = row.get('usageBefore'), row.get('usageAfter')
            require(type(usage) is int and limit - reserve < usage <= limit and type(after) is int and 0 <= after <= limit and
                    row.get('limitBytes') == limit and row.get('reserveBytes') == reserve and row.get('targetBytes') == reserve - (limit - usage) and
                    row.get('releasedBytes') == pending_trim and row.get('accepted') is True and row.get('beforeNativeResidency') is True,
                    'Pressure reclamation bypassed the unchanged residency guard or lost its trim receipt')
            pending_trim = 0
            pressure_count += 1
        if 'nativeCompositorRingStopped' in row:
            stopped += 1
            require(row.get('nativeCompositorRingStopped') is True and row.get('stagingTexturesCreated') == row.get('stagingTexturesReleased') == created and
                    row.get('stagingTextureBytes') == historical_bytes and row.get('stagingTexturesTrimmed') == trimmed and
                    row.get('liveStagingTextureBytes') == row.get('liveGpuResources') == 0, 'Staging not completely reclaimed after GPU retirement')
    require(opened == stopped == 1 and 1 <= created <= 13 and len(staging_checks) == created * 2 and pending_trim == 0 and
            input_frames == list(range(1, 13)) and positions[id(checks[-1])] < last_frame_position,
            'Incomplete staging initialization, trim attribution or teardown')
    return dict(capacity=3, initialTextures=1, createdTextures=created, trimmedTextures=trimmed,
                reallocatedTextures=reallocated, historicalTextureBytes=historical_bytes,
                residencyPressureObservations=pressure_count, safeReclamationObserved=trimmed > 0,
                gpuUsageLimitBytes=limit, physicalGpuQuotaVerified=False)


def validate_reclaimed_staging_input_response(report, guest, records, frames, source, wallpaper_reference, owner_output):
    capture = validate_uwsm_gpu_capture(report, guest, records, frames, source, wallpaper_reference)
    devices = input_discovery(guest, source)
    staging = reclaimed_staging_admission(records, report['disconnectCleanup']['compositorBackpressure'])
    terminal = terminal_retirement(guest, records, capture['capture']['compositorPid'])
    delivery = delivery_metadata(guest, records, source, owner_output)
    response = response_metadata(guest, frames)
    return dict(capture=capture, inputDiscovery=devices, staging=staging, terminal=terminal,
                inputDelivery=delivery, inputResponse=response, gpuInputResponseWithReclaimEnabledVerified=True,
                safeReclamationObserved=staging['safeReclamationObserved'], hostWindowIntegrationVerified=False,
                normalLauncherVerified=False, fullDesktopAccepted=False, allFramesDisplayedVerified=False,
                nearNativePerformanceVerified=False)

