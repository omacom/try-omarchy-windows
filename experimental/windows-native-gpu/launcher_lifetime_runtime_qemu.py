"""Opt-in hidden lifetime/identity consumer around the exact historical GPU harness.

This is an integration test consumer, not the released Go launcher. It appends
one private inherited cancellation capability to the actual native command;
the original harness, GPU options, readers and acceptance oracles stay intact.
"""
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

from broker_runtime_qemu import load_harness


class WindowsIdentity:
    def __init__(self):
        self.api = ctypes.WinDLL('kernel32', use_last_error=True)
        declarations = {
            'CreateEventW': ([ctypes.c_void_p, wintypes.BOOL, wintypes.BOOL, wintypes.LPCWSTR], wintypes.HANDLE),
            'SetEvent': ([wintypes.HANDLE], wintypes.BOOL),
            'CloseHandle': ([wintypes.HANDLE], wintypes.BOOL),
            'OpenProcess': ([wintypes.DWORD, wintypes.BOOL, wintypes.DWORD], wintypes.HANDLE),
            'GetProcessId': ([wintypes.HANDLE], wintypes.DWORD),
            'GetProcessTimes': ([wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4, wintypes.BOOL),
            'WaitForSingleObject': ([wintypes.HANDLE, wintypes.DWORD], wintypes.DWORD),
            'GetExitCodeProcess': ([wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)], wintypes.BOOL),
            'QueryFullProcessImageNameW': ([wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)], wintypes.BOOL),
            'CreateToolhelp32Snapshot': ([wintypes.DWORD, wintypes.DWORD], wintypes.HANDLE),
        }
        for name, (arguments, result) in declarations.items():
            function = getattr(self.api, name); function.argtypes = arguments; function.restype = result
        class Entry(ctypes.Structure):
            _fields_ = [('dwSize', wintypes.DWORD), ('cntUsage', wintypes.DWORD), ('th32ProcessID', wintypes.DWORD),
                ('th32DefaultHeapID', ctypes.c_size_t), ('th32ModuleID', wintypes.DWORD), ('cntThreads', wintypes.DWORD),
                ('th32ParentProcessID', wintypes.DWORD), ('pcPriClassBase', wintypes.LONG),
                ('dwFlags', wintypes.DWORD), ('szExeFile', wintypes.WCHAR * 260)]
        self.entry = Entry
        for name in ('Process32FirstW', 'Process32NextW'):
            function = getattr(self.api, name); function.argtypes = [wintypes.HANDLE, ctypes.POINTER(Entry)]; function.restype = wintypes.BOOL

    def open(self, pid):
        handle = self.api.OpenProcess(0x00101000, False, pid)
        if not handle: raise ctypes.WinError(ctypes.get_last_error())
        return handle

    def creation(self, handle):
        created, exited, kernel, user = (wintypes.FILETIME() for _ in range(4))
        if not self.api.GetProcessTimes(handle, ctypes.byref(created), ctypes.byref(exited), ctypes.byref(kernel), ctypes.byref(user)):
            raise ctypes.WinError(ctypes.get_last_error())
        return (created.dwHighDateTime << 32) | created.dwLowDateTime

    def parent(self, pid):
        handle = self.api.CreateToolhelp32Snapshot(2, 0)
        if handle == ctypes.c_void_p(-1).value: raise ctypes.WinError(ctypes.get_last_error())
        try:
            entry = self.entry(); entry.dwSize = ctypes.sizeof(entry)
            valid = self.api.Process32FirstW(handle, ctypes.byref(entry))
            while valid:
                if entry.th32ProcessID == pid: return entry.th32ParentProcessID
                valid = self.api.Process32NextW(handle, ctypes.byref(entry))
            raise RuntimeError('Held process missing from identity snapshot')
        finally: self.api.CloseHandle(handle)

    def image(self, handle):
        name = ctypes.create_unicode_buffer(32768); size = wintypes.DWORD(len(name))
        if not self.api.QueryFullProcessImageNameW(handle, 0, name, ctypes.byref(size)):
            raise ctypes.WinError(ctypes.get_last_error())
        return Path(name.value)

    def terminal(self, handle):
        if self.api.WaitForSingleObject(handle, 0) != 0: return None
        code = wintypes.DWORD()
        if not self.api.GetExitCodeProcess(handle, ctypes.byref(code)): raise ctypes.WinError(ctypes.get_last_error())
        return code.value


def validate_ready(message, bridge_pid, launcher_pid, launcher_time, qemu_pid, qemu_time, qemu_parent):
    """Strict protocol validation; timestamps remain decimal strings past 2**53."""
    expected = dict(nativeLauncherVmReady=True, protocol=1, launchMode='backend-owned',
        bridgePid=bridge_pid, launcherPid=launcher_pid, launcherCreationTime100ns=str(launcher_time),
        qemuPid=qemu_pid, qemuCreatorPid=bridge_pid, qemuCreationTime100ns=str(qemu_time),
        heldProcessHandle=True, privateJobAssigned=True, imageFilePinned=True, qmpReady=True,
        guestGraphicsVerified=False, launcherInputVerified=False, dailyUseAccepted=False)
    if set(message) != set(expected) or any(type(message[k]) is not type(v) or message[k] != v for k, v in expected.items()):
        raise RuntimeError('Native launcher VM identity protocol mismatch')
    if not qemu_pid or qemu_pid in (bridge_pid, launcher_pid) or qemu_parent != bridge_pid or qemu_time < launcher_time:
        raise RuntimeError('Native launcher VM process relationship mismatch')


class ObservedOutput:
    def __init__(self, stream, observe): self.stream, self.observe = stream, observe
    def __iter__(self):
        for line in self.stream:
            self.observe(line)
            yield line
    def __getattr__(self, name): return getattr(self.stream, name)


def main():
    # Remove only this adapter's argument before the frozen parser runs.
    option = '--lifetime-control-report'
    if sys.argv.count(option) != 1: raise ValueError('One fresh lifetime control report is required')
    index = sys.argv.index(option); report_path = Path(sys.argv[index + 1]).resolve()
    if report_path.exists(): raise ValueError('Use a fresh lifetime report')
    del sys.argv[index:index + 2]
    bridge = Path(sys.argv[sys.argv.index('--bridge') + 1]).resolve(strict=True)
    qemu = Path(sys.argv[sys.argv.index('--qemu') + 1]).resolve(strict=True)
    original_popen = subprocess.Popen
    windows = WindowsIdentity(); api = windows.api
    event = api.CreateEventW(None, True, False, None)
    if not event: raise ctypes.WinError(ctypes.get_last_error())
    os.set_handle_inheritable(event, True)
    launcher = windows.open(os.getpid())
    state = dict(schema=1, scope='hidden native lifetime identity with unchanged GPU harness',
        launcherPid=os.getpid(), launcherCreationTime100ns=str(windows.creation(launcher)),
        adapterSha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        parentHarnessCommand=list(sys.argv), cancellationRequested=False, identityVerified=False,
        launcherFrontendIntegrated=False, officialInputModified=False, dailyUseAccepted=False)
    native = child = None
    owner = None

    def popen(command, *arguments, **options):
        nonlocal native, child, owner
        if not isinstance(command, (list, tuple)) or Path(command[0]).resolve() != bridge:
            return original_popen(command, *arguments, **options)
        if owner is not None or options.get('startupinfo') is not None or '--run-qemu' not in command:
            raise RuntimeError('Unexpected native launch boundary')
        actual = list(command) + ['--driver-launcher-cancel-handle', str(event)]
        startup = subprocess.STARTUPINFO(); startup.lpAttributeList = {'handle_list': [event]}
        options.update(startupinfo=startup, close_fds=True)
        owner = original_popen(actual, *arguments, **options)
        native = windows.open(owner.pid)
        if windows.parent(owner.pid) != os.getpid(): raise RuntimeError('Native process has wrong creator')
        state.update(actualNativeCommand=actual, bridgePid=owner.pid,
            bridgeCreationTime100ns=str(windows.creation(native)), explicitInheritedHandleCount=1)

        def observe(line):
            nonlocal child
            message = json.loads(line)
            if 'nativeLauncherVmReady' not in message: return
            try:
                if child is not None: raise RuntimeError('Duplicate native VM identity')
                pid = message.get('qemuPid')
                if type(pid) is not int or pid <= 0: raise RuntimeError('Invalid native VM PID')
                child = windows.open(pid)
                validate_ready(message, owner.pid, os.getpid(), windows.creation(launcher), pid,
                    windows.creation(child), windows.parent(pid))
                if api.WaitForSingleObject(child, 0) != 258 or not os.path.samefile(windows.image(child), qemu):
                    raise RuntimeError('Native VM image or liveness mismatch')
                state.update(identityVerified=True, ready=message, qemuImageVerified=True,
                    processIdentityHeldUntilExit=True)
            except Exception as error:
                state['identityError'] = str(error)
                if not api.SetEvent(event): raise ctypes.WinError(ctypes.get_last_error())
                state['cancellationRequested'] = True
                raise
        owner.stdout = ObservedOutput(owner.stdout, observe)
        return owner

    result = None
    try:
        subprocess.Popen = popen
        result = load_harness().main()
        return result
    finally:
        subprocess.Popen = original_popen
        state.update(genericHarnessExit=result, nativeExit=windows.terminal(native) if native else None,
            qemuExit=windows.terminal(child) if child else None)
        try:
            with report_path.open('x', encoding='utf-8', newline='\n') as stream:
                json.dump(state, stream, indent=2); stream.write('\n')
        finally:
            for handle in (child, native, launcher, event):
                if handle: api.CloseHandle(handle)
        if not state['identityVerified'] or state.get('identityError') or state['nativeExit'] != 0 or state['qemuExit'] != 0:
            raise RuntimeError('Native launcher lifetime integration failed; original evidence retained')


if __name__ == '__main__':
    raise SystemExit(main())
