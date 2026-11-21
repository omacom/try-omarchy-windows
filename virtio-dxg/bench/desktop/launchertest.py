"""launchertest.py [ROUNDS] [FILTER]: every app the launcher lists, opened
the way a user does it: SUPER+SPACE (Omarchy menu), type the app's name,
Enter. Times, from the key presses (sent through QEMU's input, so they go
through the guest's input stack): menu shown (openlayer), and Enter to the
app's window mapped (openwindow). Then closes the window and stops what the
launch started (its systemd scopes). Round 1 is the first launch since boot
(unless something was opened before), later rounds are warm launches.
Runs on workspace 7. As the desktop user with sessenv.sh."""
import configparser
import glob
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
SKIP = {'foot-server', 'try-omarchy-follow-timezone', 'disk usage', 'docker', 'uuctl'}


def apps():
    seen, out = set(), []
    for d in (os.path.expanduser('~/.local/share/applications'), '/usr/share/applications'):
        for f in sorted(glob.glob(d + '/*.desktop')):
            did = os.path.basename(f)[:-8]
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
            if did.lower() in SKIP or name.lower() in SKIP:
                continue
            if FILTER and not any(f in name.lower() or f in did.lower() for f in FILTER):
                continue
            out.append((did, name))
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
            buf += hs.recv(65536)
    except BlockingIOError:
        pass


def wait_event(prefix, timeout, t0):
    """First compositor event starting with prefix after now; (ms since t0, payload)."""
    global buf
    end = time.time() + timeout
    while time.time() < end:
        try:
            buf += hs.recv(65536)
        except BlockingIOError:
            time.sleep(0.001)
        while b'\n' in buf:
            line, buf = buf.split(b'\n', 1)
            if line.startswith(prefix):
                return (time.time() - t0) * 1000, line[len(prefix):].decode(errors='replace')
    return None, None


def scopes():
    r = subprocess.run(['systemctl', '--user', 'list-units', '--type=scope', '--plain',
                        '--no-legend', '--all'], capture_output=True, text=True).stdout
    return {l.split()[0] for l in r.splitlines() if l.strip()}


def hypr(lua):
    subprocess.run(['hyprctl', 'eval', lua], capture_output=True)


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
results = {}
items = apps()
print(f'{len(items)} apps', flush=True)
for rnd in range(ROUNDS):
    for did, name in items:
        close_menu()
        before = scopes()
        drain()
        t0 = time.time()
        k.combo('meta_l', 'spc')
        menu_ms, _ = wait_event(b'openlayer>>omarchy-menu', 3, t0)
        time.sleep(0.15)
        k.type(term(name))
        time.sleep(0.45)
        drain()
        t1 = time.time()
        k.combo('ret')
        win_ms, payload = wait_event(b'openwindow>>', 25, t1)
        cls = ''
        if payload:
            f = payload.split(',', 3)
            cls = f[2] if len(f) > 2 else ''
            time.sleep(1.0)
            hypr(f'hl.dispatch(hl.dsp.window.close({{ window = "address:0x{f[0]}" }}))')
            subprocess.run(['hyprctl', 'dispatch', 'closewindow', 'address:0x' + f[0]],
                           capture_output=True)
        else:
            close_menu()
        time.sleep(1.0)
        for s in scopes() - before:
            if s.startswith('app-'):
                subprocess.run(['systemctl', '--user', 'stop', s], capture_output=True)
        time.sleep(0.5)
        results.setdefault((did, name), []).append((menu_ms, win_ms, cls))
        m = '-' if menu_ms is None else f'{menu_ms:.0f}'
        w = 'no window' if win_ms is None else f'{win_ms:.0f}'
        print(f'round {rnd + 1}  {name[:34]:34}  menu {m:>4} ms  enter->window {w:>9} ms  [{cls[:30]}]',
              flush=True)
hypr('hl.dispatch(hl.dsp.focus({ workspace = 1 }))')
