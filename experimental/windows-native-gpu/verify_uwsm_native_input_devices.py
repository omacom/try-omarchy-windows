"""Audit real guest input-device discovery alongside the complete NVIDIA capture."""
import json
import re

from verify_hyprland_lifecycle_capture import require
from verify_uwsm_demand_staging import validate_terminal_demand_staging

BACKEND = '74d4cba8989336a259026d8c96694bf406d7d56aec820e35b9c95ce291ff9e80'
LIBRARY = '/usr/lib/x86_64-linux-gnu/libaquamarine.so.14'
REUSED = 'Private native-render existing seat input reused; libseat=true libinput=true KMS=false'


def input_discovery(guest, source):
    manifest = source['manifest']
    require(manifest.get('nativeInputProbe') is True and manifest.get('uwsmManagedProbe') is True,
            'Missing controlled native input fixture')
    build = manifest.get('aquamarineInputBuild', {})
    require(build.get('sourceHashes', {}).get('src/backend/Backend.cpp') == BACKEND,
            'Input build lacks the verified seat reuse and deferred device dispatch')
    digest = build.get('librarySha256')
    require(isinstance(digest, str) and re.fullmatch('[0-9a-f]{64}', digest) and
            all(manifest['files'].get('lib/' + name, {}).get('sha256') == digest for name in
                ('libaquamarine.so', 'libaquamarine.so.14', 'libaquamarine.so.0.15.1')),
            'Input library aliases do not match the private build receipt')
    snapshots = re.findall(r'^OMARCHY_NATIVE_INPUT_DEVICES_HEX ([0-9a-f]+)$', guest, re.M)
    libraries = re.findall(r'^OMARCHY_NATIVE_INPUT_LIBRARY_HEX ([0-9a-f]+)$', guest, re.M)
    require(len(snapshots) == len(libraries) == 1 and bytes.fromhex(libraries[0]).decode() == LIBRARY,
            'Missing/duplicate compositor input or loaded-library snapshot')
    devices = json.loads(bytes.fromhex(snapshots[0]))
    require(isinstance(devices, dict) and set(devices) == {'mice', 'keyboards', 'tablets', 'touch', 'switches'} and
            all(isinstance(rows, list) for rows in devices.values()) and
            all(isinstance(row, dict) for rows in devices.values() for row in rows) and
            len(devices['mice']) == 2 and len(devices['keyboards']) == 3 and
            devices['tablets'] == devices['touch'] == devices['switches'] == [],
            'Missing actual guest keyboard/pointer devices')
    keyboard = [row for row in devices['keyboards'] if row.get('name') == 'qemu-virtio-keyboard']
    pointer = [row for row in devices['mice'] if row.get('name') == 'qemu-virtio-tablet']
    require(len(keyboard) == len(pointer) == 1 and keyboard[0].get('layout') == 'us' and
            keyboard[0].get('active_keymap') == 'English (US)',
            'Missing the QEMU virtio keyboard/tablet or expected keymap')
    addresses = [row.get('address') for row in devices['mice'] + devices['keyboards']]
    require(all(isinstance(value, str) and re.fullmatch(r'0x[1-9a-f][0-9a-f]*', value) for value in addresses) and len(set(addresses)) == 5,
            'Replayed/invalid compositor input object identities')
    logs = [bytes.fromhex(row).decode() for row in re.findall(
        r'^OMARCHY_NATIVE_INPUT_BACKEND_LOG_HEX ([0-9a-f]+)$', guest, re.M)]
    require(logs.count(REUSED) == 1 and logs.count('Private native-render input device count 5') == 1 and
            sum("Seat opened with backend 'logind'" in line for line in logs) == 1 and
            logs.count('libinput: New device QEMU Virtio Keyboard: 1575-1') == 1 and
            logs.count('libinput: New device QEMU Virtio Tablet: 1575-3') == 1 and
            not any('Device already taken' in line or "opening input device" in line and 'failed' in line for line in logs),
            'Duplicate seat ownership or incomplete actual libinput discovery')
    require(re.search(r'systemd-logind\[\d+\]: Watching system buttons on /dev/input/event\d+ \(QEMU Virtio Keyboard\)', guest),
            'Missing independent kernel/logind virtual-keyboard observation')
    return dict(guestInputDevicesEnumerated=True, keyboard='qemu-virtio-keyboard',
                pointer='qemu-virtio-tablet', keymap='English (US)', keyboards=3, pointers=2,
                existingLogindSeatReused=True, eventLoopDiscoveryVerified=True, loadedLibrary=LIBRARY,
                typedInputVerified=False, pointerDeliveryVerified=False,
                hostWindowIntegrationVerified=False, normalLauncherVerified=False, gpuProof=False)


def validate_gpu_input_discovery(report, guest, records, frames, source, wallpaper_reference):
    capture = validate_terminal_demand_staging(report, guest, records, frames, source, wallpaper_reference)
    devices = input_discovery(guest, source)
    return dict(capture=capture, inputDiscovery=devices, gpuCaptureWithInputEnumerationVerified=True,
                typedInputVerified=False, pointerDeliveryVerified=False, hostWindowIntegrationVerified=False,
                normalLauncherVerified=False, fullDesktopAccepted=False, nearNativePerformanceVerified=False)
