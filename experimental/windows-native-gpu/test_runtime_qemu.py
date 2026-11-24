"""Verify partial LIVE runtime initialization in an owned disk-free QEMU guest.

An expected unsupported call is a diagnostic boundary, never graphics success.
The initramfs contains private installed libraries and must never be uploaded.
"""
import argparse
import hashlib
import json
import pathlib
import queue
import re
import subprocess
import sys
import threading


def sha256(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('qemu', 'kernel', 'initramfs', 'bridge', 'report'):
        parser.add_argument('--' + name, type=pathlib.Path, required=True)
    parser.add_argument('--firmware', type=pathlib.Path)
    parser.add_argument('--expected-unimplemented-ioctl', type=int, default=7)
    args = parser.parse_args()
    if sys.platform != 'win32':
        parser.error('Run this harness on the Windows NVIDIA host')
    for path in (args.qemu, args.kernel, args.initramfs, args.bridge):
        if not path.is_file():
            parser.error(f'Missing file: {path}')
    if not 0 <= args.expected_unimplemented_ioctl <= 255:
        parser.error('Invalid expected ioctl')
    if args.report.exists() or args.report.with_suffix('.log').exists():
        parser.error('Use a fresh report path')
    hidden = subprocess.CREATE_NO_WINDOW
    helper = subprocess.Popen([str(args.bridge.resolve()), '--listen', '0', '--driver-queries', '--driver-contexts'],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, creationflags=hidden)
    qemu = None
    try:
        ready_lines = queue.Queue()
        threading.Thread(target=lambda: ready_lines.put(helper.stdout.readline()), daemon=True).start()
        ready = json.loads(ready_lines.get(timeout=15))
        port = ready['port']
        if ready['transport'] != 'tcp-loopback' or not isinstance(port, int) or not 0 < port < 65536:
            raise RuntimeError('Invalid bridge endpoint')
        command = [str(args.qemu.resolve()), '-machine', 'q35,accel=whpx', '-cpu', 'host',
                   '-m', '512', '-smp', '1', '-display', 'none', '-monitor', 'none',
                   '-serial', 'stdio', '-nodefaults', '-no-reboot',
                   '-kernel', str(args.kernel.resolve()), '-initrd', str(args.initramfs.resolve()),
                   '-append', 'console=ttyS0 rdinit=/init panic=1',
                   '-device', 'virtio-serial-pci,id=bridge-serial',
                   '-chardev', f'socket,id=wddm,host=127.0.0.1,port={port}',
                   '-device', 'virtserialport,bus=bridge-serial.0,chardev=wddm,name=org.7wdev.wddm']
        if args.firmware:
            command += ['-L', str(args.firmware.resolve())]
        qemu = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, errors='replace', creationflags=hidden)
        log, _ = qemu.communicate(timeout=60)
        _, helper_errors = helper.communicate(timeout=10)
        cleanup = json.loads(helper_errors.strip()) if helper.returncode == 0 else {}
        unsupported = sorted(set(int(n) for n in re.findall(r'LINUX_BRIDGE unsupported nr=(\d+)', log)))
        completed = [{'type': int(t), 'bytes': int(b)} for t, b in re.findall(r'LINUX_BRIDGE queryCompleted type=(\d+) bytes=(\d+)', log)]
        identity = re.search(r'nvidia vendor=(\d+) device=(\d+)', log)
        native_device = 'LINUX_BRIDGE deviceCreated=true' in log
        loaded = 'LINUX_BRIDGE nvidiaUmdPresentDuringPrivateQuery=true' in log
        device_result = re.search(r'^device=([0-9a-f]{8})\s*$', log, re.MULTILINE)
        # This acceptance explicitly expects incomplete initialization. Do
        # not reuse this boolean as an application GPU-readiness decision.
        accepted = (qemu.returncode == 0 and helper.returncode == 0 and loaded and native_device and
                    'factory=00000000' in log and 'list=00000000' in log and
                    'LINUX_BRIDGE transport=virtio-port' in log and
                    'realDxgForwarding=false' in log and 'BRIDGE_RUNTIME_EXIT=1' in log and
                    identity is not None and int(identity[1]) == 0x10de and
                    device_result is not None and int(device_result[1], 16) & 0x80000000 and
                    args.expected_unimplemented_ioctl in unsupported and
                    {'type': 0, 'bytes': 50616} in completed and
                    cleanup.get('driverCleanupVerified') is True and
                    cleanup.get('completedAdapterQueries') == len(completed) and
                    cleanup.get('failedAdapterQueries') == 0)
        report = {'schema': 1, 'diagnosticAccepted': bool(accepted),
                  'runtimeInitializationComplete': False, 'stage': 'live NVIDIA runtime to native Windows device',
                  'hypervisor': 'QEMU/WHPX', 'transport': 'virtio-serial to loopback Windows worker',
                  'qemuExit': qemu.returncode, 'hostBridgeExit': helper.returncode,
                  'vendorId': int(identity[1]) if identity else None, 'deviceId': int(identity[2]) if identity else None,
                  'liveNvidiaLinuxUmdLoaded': loaded, 'nativeKmtDeviceCreatedByLiveRuntime': native_device,
                  'd3d12DeviceHresult': device_result[1] if device_result else None,
                  'realWslDxgForwarding': False, 'unsupportedIoctls': unsupported,
                  'expectedInitializationBoundary': args.expected_unimplemented_ioctl,
                  'completedNativeQueries': completed, 'disconnectCleanup': cleanup,
                  'capturedPrivateFixturesUsed': False, 'privateRuntimeImageRedistributable': False,
                  'kernelSha256': sha256(args.kernel), 'initramfsSha256': sha256(args.initramfs),
                  'driverBridgeSha256': sha256(args.bridge), 'qemuSha256': sha256(args.qemu),
                  'guestArbitraryGpuCommandSubmissionImplemented': False,
                  'guestDesktopAcceleratedByThisBackend': False, 'nearNativePerformanceMeasured': False}
        args.report.with_suffix('.log').write_text(log + '\nHOST BRIDGE:\n' + helper_errors, encoding='utf-8', newline='\n')
        args.report.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8', newline='\n')
        for line in log.splitlines():
            if any(marker in line for marker in ('factory=', 'list=', 'nvidia vendor=', 'device=', 'LINUX_BRIDGE', 'LINUX_RUNTIME', 'BRIDGE_RUNTIME')):
                print(line)
        print(json.dumps(report, indent=2))
        return 0 if accepted else 1
    finally:
        for process in (qemu, helper):
            if process is not None and process.poll() is None:
                process.kill(); process.communicate(timeout=10)


if __name__ == '__main__':
    raise SystemExit(main())
