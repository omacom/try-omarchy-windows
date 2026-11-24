"""launchertest.py [ROUNDS] [FILTER]: every app the launcher lists, opened
the way a user does it: SUPER+SPACE (Omarchy menu), type the app's name,
Enter. Times, from the key presses (sent through QEMU's input, so they go
through the guest's input stack): menu shown (openlayer), and Enter to the
app's window mapped (openwindow). Closes verified windows on workspace 7.
Unidentified windows are reported separately. Round 1 is the first launch since boot
(unless something was opened before), later rounds are warm launches.
Runs on workspace 7. As the desktop user with sessenv.sh."""
import configparser
from collections import defaultdict
import glob
import json
import os
import re
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmpkeys import Keys  # noqa: E402

ROUNDS = int(sys.argv[1]) if len(sys.argv) > 1 else 2
FILTER = [f for f in (sys.argv[2].lower().split(',') if len(sys.argv) > 2 else []) if f]


def apps():
    seen, out = set(), []
    data_home = os.environ.get('XDG_DATA_HOME') or os.path.expanduser('~/.local/share')
    data_dirs = os.environ.get('XDG_DATA_DIRS') or '/usr/local/share:/usr/share'
    for root in [data_home, *data_dirs.split(':')]:
        if not os.path.isabs(root):
            continue
        d = os.path.join(root, 'applications')
        for f in sorted(glob.glob(d + '/**/*.desktop', recursive=True)):
            did = os.path.relpath(f, d)[:-8].replace(os.sep, '-')
            if did in seen:
                continue
            seen.add(did)
            cp = configparser.RawConfigParser(strict=False, interpolation=None)
            try:
                cp.read(f, encoding='utf-8')
                e = cp['Desktop Entry']
            except Exception:
                continue
            if e.get('NoDisplay', '').lower() == 'true' or e.get('Hidden', '').lower() == 'true':
                continue
            if e.get('Type', 'Application') != 'Application':
                continue
            name = e.get('Name', did)
            out.append((did, name, e.get('StartupWMClass', '')))
    return out


hs = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
hs.connect(os.path.join(os.environ['XDG_RUNTIME_DIR'], 'hypr',
                        os.environ['HYPRLAND_INSTANCE_SIGNATURE'], '.socket2.sock'))
hs.setblocking(False)
buf = b''


def drain():
    global buf
    try:
        while True:
            data = hs.recv(65536)
            if not data:
                raise ConnectionError('Hyprland event socket closed')
            buf += data
    except BlockingIOError:
        # Discard complete stale events. Retain a partial trailing line.
        buf = buf.rsplit(b'\n', 1)[-1]


def wait_event(prefix, timeout, t0, *, exact=False):
    """First compositor event starting with prefix after now; (ms since t0, payload)."""
    global buf
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        try:
            data = hs.recv(65536)
            if not data:
                raise ConnectionError('Hyprland event socket closed')
            buf += data
        except BlockingIOError:
            time.sleep(0.001)
        while b'\n' in buf:
            line, buf = buf.split(b'\n', 1)
            if line.startswith(prefix) and (not exact or line == prefix):
                return (time.monotonic() - t0) * 1000, line[len(prefix):].decode(errors='replace')
    return None, None


def clients():
    rows = json.loads(subprocess.check_output(['hyprctl', '-j', 'clients'], timeout=5))
    return {row['address']: row for row in rows}


def window_owners(items):
    owners = defaultdict(set)
    for did, _, wm_class in items:
        for label in (did, did.rsplit('.', 1)[-1], wm_class):
            if label:
                owners[label.casefold()].add(did)
    return owners


def hypr(lua):
    subprocess.run(['hyprctl', 'eval', lua], capture_output=True, check=True, timeout=5)


def menu_open():
    r = subprocess.run(['hyprctl', 'layers'], capture_output=True, text=True).stdout
    return 'omarchy-menu' in r


def close_menu():
    for _ in range(4):
        if not menu_open():
            return
        k.combo('esc')
        time.sleep(0.25)


def term(name):
    t = re.sub(r'[^a-z0-9 ]', '', name.lower()).strip()
    return t[:24]


k = Keys()
hypr('hl.dispatch(hl.dsp.focus({ workspace = 7 }))')
time.sleep(0.5)
items = apps()
owners = window_owners(items)
if FILTER:
    items = [item for item in items if any(f in item[0].lower() or f in item[1].lower() for f in FILTER)]
print(f'{len(items)} apps', flush=True)
for rnd in range(ROUNDS):
    for did, name, wm_class in items:
        close_menu()
        before = clients()
        drain()
        t0 = time.monotonic()
        k.combo('meta_l', 'spc')
        menu_ms, _ = wait_event(b'openlayer>>omarchy-menu', 3, t0, exact=True)
        if menu_ms is None:
            print(f'round {rnd + 1}  {did} ({name}): menu did not appear', flush=True)
            continue
        time.sleep(0.15)
        k.type(term(name))
        time.sleep(0.45)
        drain()
        t1 = time.monotonic()
        k.combo('ret')
        win_ms = None
        observed = set()
        deadline = t1 + 25
        while time.monotonic() < deadline:
            elapsed, payload = wait_event(b'openwindow>>', deadline - time.monotonic(), t1)
            if payload is None:
                break
            fields = payload.split(',', 3)
            if len(fields) != 4 or not re.fullmatch(r'[a-fA-F0-9]+', fields[0]):
                continue
            address = '0x' + fields[0]
            if address in before or fields[1] != '7':
                continue
            observed.add(fields[2])
            if owners.get(fields[2].casefold()) != {did}:
                continue
            time.sleep(1.0)
            row = clients().get(address, {})
            if row.get('workspace', {}).get('id') != 7 or row.get('class') != fields[2]:
                continue
            win_ms = elapsed
            hypr(f'hl.dispatch(hl.dsp.window.close({{ window = "address:{address}" }}))')
            break
        close_menu()
        time.sleep(1.0)
        m = '-' if menu_ms is None else f'{menu_ms:.0f}'
        w = f'{win_ms:.0f} ms' if win_ms is not None else ('unverified window' if observed else 'no window')
        print(f'round {rnd + 1}  {did} ({name})  menu {m:>4} ms  enter->window {w}  {sorted(observed)}',
              flush=True)
hypr('hl.dispatch(hl.dsp.focus({ workspace = 1 }))')
