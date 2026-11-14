"""Verify synthetic QMP delivery in the real GPU guest; Windows events remain unproven."""
import json
import re

from verify_hyprland_lifecycle_capture import require
from verify_uwsm_native_input_devices import validate_gpu_input_discovery

INPUT_SOURCES = {
    'driver_bridge.cpp': '98c46ee2a8b3f57ba404e20a2166698b481641468569ae0fdf780cb14c54a416',
    'qemu_runtime_host.h': '67fffa172f3b77218494895b67b1ca45aac02cc744f0783a6ace38804fcc2c66',
    'bounded_input_channel.h': '90ce9b3969afb5e9397455ee5949f831abc0b8c469427771b9410da53065761a',
    'input_delivery_diagnostic.h': '4aeaa2c4787754d7e06739af24cf57023ed66668220431fb9032b21a7f7aa13c',
    'shared_texture_presenter.h': '431532970ff3fbea37977c30a879f599e1171be4d42ab4d786c922cd77c8bb82',
    'compositor_presenter.h': '088151a64dadab7241e54925ef07ce7c2cd726149a96a2581d51f8f1166f59b2',
    'tcp_peer_owner.h': '12e9683429d852a050fbf2433d7dedde8336b953f1f3a1f74387d12d12304f2a',
}


GUEST_SOURCES = {'hyprland_startup_probe.c': '4b1e9d19c3999192c3d4b0a808d699e3268597bf6d68c6984311814826ae131c', 'gpu-terminal-scene.sh': '7215e2bfbe07536fa9e886b81a9aa39611f04737a2692edf8580c45e72d1bb3d'}

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


def validate_qmp_input_delivery(report, guest, records, frames, source, wallpaper_reference, owner_output):
    capture = validate_gpu_input_discovery(report, guest, records, frames, source, wallpaper_reference)
    delivery = delivery_metadata(guest, records, source, owner_output)
    return dict(capture=capture, inputDelivery=delivery, gpuCaptureWithQmpInputDeliveryVerified=True,
                hostWindowIntegrationVerified=False, normalLauncherVerified=False, fullDesktopAccepted=False,
                nearNativePerformanceVerified=False)
