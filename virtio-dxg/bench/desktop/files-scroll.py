"""Scroll an identified Files window in a disposable single-output guest.

Start on an empty guest workspace after deskbench.sh start. Pass a directory
of 3,000 files and a fresh output directory. All input goes to the test VM.
"""
import argparse
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import time

from qmpkeys import Keys, NAMES


def hypr(command):
    return json.loads(subprocess.check_output(['hyprctl', '-j', command], timeout=5))


def launch_files(keys, folder):
    if hypr('activewindow').get('address'):
        raise RuntimeError('start on an empty workspace in the disposable guest')
    if not re.fullmatch(r'[a-z0-9./ -]+', str(folder)):
        raise ValueError('use a lowercase test directory reachable with the guest keyboard')
    ipc = Path(os.environ['XDG_RUNTIME_DIR'], 'hypr', os.environ['HYPRLAND_INSTANCE_SIGNATURE'])
    with socket.socket(socket.AF_UNIX) as sock:
        sock.connect(str(ipc / '.socket2.sock'))
        with sock.makefile('rb') as stream:
            def event(start, match):
                deadline = start + 15
                while time.monotonic() < deadline:
                    sock.settimeout(max(0.001, deadline - time.monotonic()))
                    line = stream.readline()
                    if not line:
                        raise ConnectionError('compositor event stream closed')
                    if match(line.rstrip(b'\n')):
                        return (time.monotonic() - start) * 1000
                raise TimeoutError('expected guest compositor event did not arrive')

            start = time.monotonic()
            keys.combo('meta_l', 'spc')
            menu_ms = event(start, lambda line: line == b'openlayer>>omarchy-menu')
            time.sleep(0.15)
            keys.type('files')
            time.sleep(0.45)
            start = time.monotonic()
            keys.combo('ret')
            window_ms = event(start, lambda line: re.match(
                rb'^openwindow>>[0-9a-f]+,[^,]*,(?:org\.gnome\.)?nautilus,', line, re.I))
    until = time.monotonic() + 5
    while hypr('activewindow').get('class', '').lower() not in {'nautilus', 'org.gnome.nautilus'}:
        if time.monotonic() >= until:
            raise RuntimeError('the mapped Files window did not receive focus')
        time.sleep(0.05)
    keys.combo('ctrl', 'l')
    time.sleep(0.1)
    for char in str(folder):
        keys.combo(NAMES.get(char, char), hold=10)
        time.sleep(0.02)
    keys.combo('ret')
    until = time.monotonic() + 10
    while hypr('activewindow').get('title') != folder.name:
        if time.monotonic() >= until:
            raise RuntimeError('Files did not open the benchmark directory')
        time.sleep(0.1)
    return menu_ms, window_ms


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    folder = args.directory.resolve(strict=True)
    entries = list(folder.iterdir())
    if len(entries) != 3000 or not all(item.is_file() for item in entries):
        parser.error('expected the 3,000-file benchmark directory')
    monitors = hypr('monitors')
    if len(monitors) != 1 or monitors[0]['transform'] != 0:
        parser.error('use one unrotated guest output')
    monitor = monitors[0]
    args.output.mkdir(exist_ok=False)
    keys = Keys()
    menu_ms, window_ms = launch_files(keys, folder)
    window = hypr('activewindow')
    if (window['class'].lower() not in {'nautilus', 'org.gnome.nautilus'}
            or window['title'] != folder.name):
        parser.error('focus the Files window showing the benchmark directory')
    x, y = window['at']
    width, height = window['size']
    axes = [(x + width * 0.75 - monitor['x'], monitor['width'], 'x'),
            (y + height * 0.6 - monitor['y'], monitor['height'], 'y')]
    move = [{'type': 'abs', 'data': {'axis': axis, 'value': round(pos * monitor['scale'] * 32767 / size)}}
            for pos, size, axis in axes]
    keys.cmd('input-send-event', events=move)
    time.sleep(0.5)
    cursor = hypr('cursorpos')
    if not (x + width // 2 <= cursor['x'] < x + width - 5
            and y + height // 3 <= cursor['y'] < y + height - 5):
        parser.error('the guest pointer did not reach the file grid')
    rect = f'{x},{y} {width}x{height}'

    def capture(filename):
        if hypr('activewindow').get('address') != window['address']:
            raise RuntimeError('the focused window changed; discard this run')
        subprocess.run(['grim', '-g', rect, str(args.output / filename)], check=True, timeout=5)

    events = [{'type': 'btn', 'data': {'button': 'wheel-down', 'down': down}}
              for down in (True, False)]
    capture('before.png')
    times = []
    with Path('/sys/kernel/tracing/trace_marker').open('w', buffering=1) as marker:
        marker.write('files-scroll\n')
        start = time.monotonic()
        try:
            # Never replay a backlog in a burst. Report input pacing separately.
            until = start + 15
            while time.monotonic() < until:
                sent = time.monotonic()
                keys.cmd('input-send-event', events=events)
                times.append(sent - start)
                time.sleep(max(0, min(until, sent + 0.1) - time.monotonic()))
        finally:
            marker.write('end\n')
            keys.f.close()
            keys.s.close()
    capture('after.png')
    changed = (args.output / 'before.png').read_bytes() != (args.output / 'after.png').read_bytes()
    report = dict(kernel=Path('/proc/sys/kernel/osrelease').read_text().strip(),
                  pid=window['pid'], window_size=window['size'], refresh_hz=monitors[0]['refreshRate'],
                  scale=monitor['scale'], menu_ms=menu_ms, enter_to_files_ms=window_ms,
                  wheel_seconds=times, capture_bytes_changed=changed)
    (args.output / 'scroll.json').write_text(json.dumps(report, indent=2) + '\n')
    if not changed:
        raise SystemExit('both captures are identical; inspect the scroll target before using this trace')


if __name__ == '__main__':
    main()
