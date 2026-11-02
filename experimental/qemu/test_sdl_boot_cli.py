"""Exercise boot-console ownership in stopped, disk-free, windowless real QEMU.

No guest executes and no GL frontend, GPU context, network or host input is used.
The native GPU transition requires separate hardware validation.
"""
import argparse
import hashlib
import json
import re
from pathlib import Path
import subprocess
import tempfile
from test_section import Qmp


SESSION = 'session=00112233445566778899aabbccddeeff,adapter-luid=0011223344556677'


def verify(qemu, firmware):
    common = [str(qemu.resolve()), '-L', str(firmware.resolve()),
              '-machine', 'q35,accel=tcg', '-m', '64M', '-display', 'none',
              '-nodefaults', '-vga', 'none', '-monitor', 'none', '-serial', 'none', '-S']
    controls = []

    def relay(name='relay', boot=None):
        value = 'wddm-sdl-relay,id=' + name + ',' + SESSION
        if boot is not None:
            value += ',boot-display=' + boot
        return ['-device', value]

    for name, devices, expected in (
        ('missing-display', relay(boot='absent'), "Device 'absent' not found"),
        ('non-display', ['-device', 'virtio-serial-pci,id=not-display'] + relay(boot='not-display'),
         'not bound to a QemuConsole'),
        ('non-vga', ['-device', 'secondary-vga,id=other'] + relay(boot='other'),
         'requires the primary standard VGA'),
        ('multiple-consoles', ['-device', 'VGA,id=boot'] + relay('other') + relay(boot='boot'),
         'requires one graphic console'),
        ('second-owner', ['-device', 'VGA,id=boot'] + relay('first', 'boot') + relay('second', 'boot'),
         'already owned'),
    ):
        result = subprocess.run(common + devices, capture_output=True, text=True, timeout=15,
                                creationflags=subprocess.CREATE_NO_WINDOW)
        if result.returncode == 0 or expected not in result.stderr:
            raise RuntimeError(f'{name}: incorrect rejection: {result.stderr}')
        controls.append(name)

    with tempfile.TemporaryDirectory(prefix='qemu-sdl-boot-') as temporary:
        directory = Path(temporary)
        for borrowed in (False, True):
            devices = (['-device', 'VGA,id=boot'] if borrowed else []) + relay(boot='boot' if borrowed else None)
            monitor = Qmp(common + devices + ['-qmp', 'stdio'])
            try:
                path = '/machine/peripheral/relay'

                def state():
                    value = json.loads(monitor.call('qom-get', {'path': path, 'property': 'relay-state'}))
                    if (value['bootConsoleBorrowed'] != borrowed or value['consoleIndex'] != 0 or
                            any(value[key] for key in ('attached', 'everAttached', 'poisoned',
                                'rootAdapterVerified', 'nativeConsoleActive', 'importContextLive',
                                'localScanoutLive', 'channelProgramLive', 'liveImportedObjects', 'lastQueuedFrame'))):
                        raise RuntimeError(f'Incorrect stopped console state: {value}')
                    return value

                initial = state()
                def pixels(label, default=False):
                    output = directory / f'{borrowed}-{label}.ppm'
                    arguments = {'filename': str(output)}
                    # The busless relay is not discoverable through the PCI/
                    # sysbus device-name lookup. QMP's default is console 0,
                    # whose identity is independently checked in relay-state.
                    if borrowed and not default:
                        arguments.update(device='boot', head=0)
                    monitor.call('screendump', arguments)
                    data = output.read_bytes()
                    header = re.match(rb'P6\s+(\d+)\s+(\d+)\s+255\n', data)
                    if (not header or not int(header[1]) or not int(header[2]) or
                            len(data) - header.end() != int(header[1]) * int(header[2]) * 3):
                        raise RuntimeError('Missing complete initial boot surface')
                    return hashlib.sha256(data).hexdigest()

                original = pixels('before')
                for index, command in enumerate(('attach:1', 'present:1:1', 'detach:1', 'attach:1')):
                    error = monitor.call('qom-set', {'path': path, 'property': 'relay-command', 'value': command}, error=True)
                    if not error.get('desc') or state() != initial or pixels(str(index)) != original:
                        raise RuntimeError('Rejected GPU operation changed the boot surface or console ownership')
                if borrowed:
                    # The relay reports borrowed ownership at index 0; both
                    # default and explicit VGA lookup must read that surface.
                    if pixels('default', default=True) != original:
                        raise RuntimeError('Relay and boot display do not share the primary console surface')
                if monitor.call('query-status')['running']:
                    raise RuntimeError('CPU unexpectedly started')
                monitor.call('quit')
                monitor.process.wait(timeout=10)
                if monitor.process.returncode:
                    raise RuntimeError('Console realization did not exit cleanly')
                controls.append('borrowed-stopped-surface' if borrowed else 'original-stopped-surface')
            finally:
                monitor.close()
    return dict(schema=1, controls=controls, qemuSha256=hashlib.sha256(qemu.read_bytes()).hexdigest(),
                cpuStopped=True, diskAttached=False, visibleWindowOpened=False, hostInputInjected=False,
                gpuTransitionVerified=False, normalGuestBootVerified=False)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', type=Path, required=True)
    parser.add_argument('--firmware', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    proof = verify(args.qemu, args.firmware)
    with args.report.open('x', encoding='utf-8') as output:
        json.dump(proof, output, indent=2)
        output.write('\n')
    print('PASS: seven stopped-QEMU console controls; boot pixels preserved; GPU transition unverified')
