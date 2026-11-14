"""Verify a real Windows QEMU section mapping, bounds, collision and lifetime."""
import argparse
import ctypes
from ctypes import wintypes
import json
import pathlib
import queue
import subprocess
import sys
import tempfile
import threading
import time
import uuid


class Sections:
    def __init__(self):
        self.api = ctypes.WinDLL('kernel32', use_last_error=True)
        self.api.OpenFileMappingW.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR]
        self.api.OpenFileMappingW.restype = wintypes.HANDLE
        self.api.MapViewOfFile.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, ctypes.c_size_t]
        self.api.MapViewOfFile.restype = ctypes.c_void_p
        self.api.UnmapViewOfFile.argtypes = [ctypes.c_void_p]
        self.api.UnmapViewOfFile.restype = wintypes.BOOL
        self.api.CloseHandle.argtypes = [wintypes.HANDLE]
        self.api.CloseHandle.restype = wintypes.BOOL

    def open(self, name):
        return self.api.OpenFileMappingW(6, False, name)  # FILE_MAP_READ | FILE_MAP_WRITE

    def exists(self, name):
        handle = self.open(name)
        if handle:
            self.api.CloseHandle(handle)
        return bool(handle)


class Qmp:
    def __init__(self, command):
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, text=True,
                                        creationflags=subprocess.CREATE_NO_WINDOW)
        self.messages = queue.Queue()
        self.counter = 0
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()
        try:
            greeting = self.messages.get(timeout=15)
            if 'QMP' not in greeting:
                raise RuntimeError(f'Invalid QMP greeting: {greeting}')
            self.call('qmp_capabilities')
        except BaseException:
            self.close()
            raise

    def read(self):
        try:
            for line in self.process.stdout:
                self.messages.put(json.loads(line))
        except Exception as error:
            self.messages.put({'reader_error': str(error)})

    def call(self, name, arguments=None, error=False):
        self.counter += 1
        request = {'execute': name, 'id': self.counter}
        if arguments is not None:
            request['arguments'] = arguments
        self.process.stdin.write(json.dumps(request) + '\n')
        self.process.stdin.flush()
        deadline = time.monotonic() + 15
        while True:
            message = self.messages.get(timeout=max(0.01, deadline - time.monotonic()))
            if 'reader_error' in message:
                raise RuntimeError(message['reader_error'])
            if message.get('id') != self.counter:
                if time.monotonic() >= deadline:
                    raise TimeoutError('QMP reply timed out')
                continue
            if ('error' in message) != error:
                raise RuntimeError(f'Unexpected QMP result: {message}')
            return message.get('error' if error else 'return')

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait(timeout=10)
        self.reader.join(timeout=2)
        errors = self.process.stderr.read()
        if self.process.returncode:
            print(errors[-4000:], file=sys.stderr)
        for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
            stream.close()


def verify(qemu, firmware):
    sections = Sections()
    name = 'Local\\7Wdev-WDDM-' + uuid.uuid4().hex
    size = 16 * 1024 * 1024
    process = None
    handle = None
    view = None
    with tempfile.TemporaryDirectory(prefix='qemu-section-') as directory:
        scratch = pathlib.Path(directory)
        command = [str(qemu.resolve()), '-L', str(firmware.resolve()), '-machine',
                   'q35,accel=tcg,memory-backend=ram', '-m', '16M', '-display', 'none',
                   '-nodefaults', '-monitor', 'none', '-serial', 'none', '-S', '-qmp', 'stdio',
                   '-object', f'memory-backend-win32-section,id=ram,size={size},section-name={name}']
        try:
            process = Qmp(command)
            handle = sections.open(name)
            if not handle:
                raise ctypes.WinError(ctypes.get_last_error())
            view = sections.api.MapViewOfFile(handle, 6, 0, 0, size)
            if not view:
                raise ctypes.WinError(ctypes.get_last_error())
            pattern = bytes(range(31, 47))
            ctypes.memmove(view + 0x100000, pattern, len(pattern))
            dump = scratch / 'gpa.bin'
            process.call('pmemsave', {'val': 0x100000, 'size': len(pattern), 'filename': str(dump)})
            if dump.read_bytes() != pattern:
                raise RuntimeError('QEMU did not read the external Windows view at guest physical address')
            for properties in ({'size': 4097}, {'share': False}, {'section-name': 'Global\\invalid'},
                               {'section-name': name}):
                candidate = {'qom-type': 'memory-backend-win32-section', 'id': 'bad',
                             'size': 4096, 'section-name': 'Local\\7Wdev-WDDM-' + uuid.uuid4().hex}
                candidate.update(properties)
                process.call('object-add', candidate, error=True)
            process.call('qom-set', {'path': '/objects/ram', 'property': 'section-name',
                                    'value': 'Local\\7Wdev-WDDM-' + uuid.uuid4().hex}, error=True)
            process.call('object-del', {'id': 'ram'}, error=True)
            blocker = process.call('migrate', {'uri': 'file:' + str(scratch / 'migration.bin')}, error=True)
            if 'not migratable' not in blocker['desc']:
                raise RuntimeError(f'Migration did not fail for the intended reason: {blocker}')
            process.call('quit')
            if process.process.wait(timeout=10) != 0:
                raise RuntimeError('QEMU did not exit cleanly')
        finally:
            if process:
                process.close()
            if view and not sections.api.UnmapViewOfFile(view):
                raise ctypes.WinError(ctypes.get_last_error())
            if handle:
                sections.api.CloseHandle(handle)
        if sections.exists(name):
            raise RuntimeError('Section handle leaked after QEMU and test mapping closed')
    print('PASS: real QEMU Windows section alias, size/share/name/collision checks, migration blocker, cleanup')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', type=pathlib.Path, required=True)
    parser.add_argument('--firmware', type=pathlib.Path, required=True)
    args = parser.parse_args()
    if sys.platform != 'win32':
        parser.error('Run on Windows; no NVIDIA hardware is required for this memory test')
    verify(args.qemu, args.firmware)
