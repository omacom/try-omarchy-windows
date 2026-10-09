#!/usr/bin/env python3
"""Exercise the real trace API without initializing a renderer or GPU."""
import ctypes
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import threading
import time


def child(library, mode):
    lib = ctypes.CDLL(str(library), use_errno=True, use_last_error=True)
    lib.winq_venus_trace_enabled.restype = ctypes.c_bool
    lib.winq_venus_trace_enter.restype = ctypes.c_uint64
    lib.winq_venus_trace_enter.argtypes = [ctypes.c_uint32, ctypes.c_uint64,
                                          ctypes.c_char_p, ctypes.c_char_p]
    lib.winq_venus_trace_exit.argtypes = [ctypes.c_uint64, ctypes.c_char_p]
    if mode == 'off':
        assert not lib.winq_venus_trace_enabled()
        assert lib.winq_venus_trace_enter(1, 2, b'off', b'') == 0
        return
    ctypes.set_errno(123)
    if os.name == "nt":
        ctypes.set_last_error(456)
    assert lib.winq_venus_trace_enabled()
    assert ctypes.get_errno() == 123
    if os.name == "nt":
        assert ctypes.get_last_error() == 456
    if mode == 'rotation':
        for _ in range(45000):
            call = lib.winq_venus_trace_enter(7, 9, b'rotation', b'x' * 700)
            assert call
            lib.winq_venus_trace_exit(call, b'returned=1')
        return
    outer = lib.winq_venus_trace_enter(7, 9, b'outer', b'size=4096\nflags=1')
    ready = threading.Event()
    release = threading.Event()
    def worker():
        inner = lib.winq_venus_trace_enter(8, 10, b'worker', b'handle=123')
        ready.set()
        assert release.wait(15)
        lib.winq_venus_trace_exit(inner, b'VkResult=0')
    thread = threading.Thread(target=worker)
    thread.start()
    assert ready.wait(5)
    time.sleep(6.6)
    # The watchdog must already have flushed, while calls are still open.
    files = list(Path(os.environ['LOCALAPPDATA']).rglob('*.inflight'))
    assert len(files) == 1 and 'op=outer' in files[0].read_text()
    assert 'op=worker' in files[0].read_text()
    release.set()
    thread.join(5)
    assert not thread.is_alive()
    lib.winq_venus_trace_exit(outer, b'VkResult=0')
    time.sleep(1.2)


def main():
    if len(sys.argv) == 4 and sys.argv[2] == '--child':
        child(Path(sys.argv[1]), sys.argv[3])
        return
    library = Path(sys.argv[1]).resolve()
    for mode in ('off', 'on', 'rotation'):
        with tempfile.TemporaryDirectory(prefix='venus-trace-') as temp:
            base = Path(temp) / 'trace-\u6d4b\u8bd5'
            base.mkdir()
            env = {**os.environ, 'LOCALAPPDATA': str(base)}
            env.pop('WINQ_VENUS_TRACE', None)
            if mode != 'off':
                env['WINQ_VENUS_TRACE'] = '1'
            subprocess.run([sys.executable, __file__, str(library), '--child', mode],
                           env=env, check=True, timeout=60)
            files = list((base / 'winq-emu' / 'venus-trace').glob('*'))
            if mode == 'off':
                assert not files, files
                print('ok - disabled trace creates no files')
                continue
            logs = [p for p in files if '.inflight' not in p.name]
            assert 1 <= len(logs) <= 4
            assert all(p.stat().st_size <= 16 * 1024 * 1024 + 4096 for p in files)
            if mode == 'rotation':
                assert len(logs) == 4, [p.name for p in files]
                print('ok - rotation caps active log and three backups')
                continue
            log = logs[0].read_text()
            rows = [line for line in log.splitlines() if 'op=trace.init' not in line]
            assert all(re.match(r'ms=\d+ pid=\d+ tid=\d+ ctx=\d+ dev=0x[0-9a-f]+ call=\d+ ', line)
                       for line in rows)
            entries = [line for line in rows if ' ENTER ' in line]
            assert len(entries) == 2
            assert len({re.search(r'tid=(\d+)', line)[1] for line in entries}) == 2
            assert len([line for line in rows if ' EXIT ' in line]) == 2
            assert len([line for line in rows if ' IN-FLIGHT ' in line]) == 2, log
            flight = next(p for p in files if p.name.endswith('.inflight')).read_text()
            triggers = re.findall(r'dump_trigger=(\d+)', flight)
            assert len(set(triggers)) == 2 and len(triggers) == 4, flight
            assert 'size=4096 flags=1' in log
            print('ok - flushed ENTER/EXIT, native thread IDs, full once-per-call watchdog dumps')


if __name__ == '__main__':
    main()
