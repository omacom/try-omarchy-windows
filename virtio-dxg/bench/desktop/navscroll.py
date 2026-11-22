"""navscroll.py [APP_CMD...]: frames an application draws while it scrolls.
Opens Nautilus (default) on a folder of 3000 files, moves the pointer over
its window and scrolls with the mouse wheel through QEMU's input (one notch
every 25 ms for 2 s down, 2 s up), and counts the window's surface commits
(WAYLAND_DEBUG) during the scroll: frames per second and the longest gaps.
At 240 Hz a smooth scroll commits about every 4.2 ms."""
import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmpkeys import Keys  # noqa: E402

d = '/tmp/navtest'
if not os.path.isdir(d):
    os.makedirs(d)
    for i in range(3000):
        open(f'{d}/file-{i:04d}-{"abcdefgh"[i % 8] * (i % 13 + 3)}.txt', 'w').close()
cmd = sys.argv[1:] or ['nautilus', '--new-window', d]
env = dict(os.environ, WAYLAND_DEBUG='client')
subprocess.run(['pkill', '-x', 'nautilus'])
for _ in range(20):
    if subprocess.run(['pgrep', '-x', 'nautilus'], capture_output=True).returncode:
        break
    subprocess.run(['pkill', '-9', '-x', 'nautilus'])
    time.sleep(0.25)
time.sleep(0.5)
log = open('/tmp/navscroll.log', 'w')
p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=log, env=env, start_new_session=True)
k = Keys()
# This process's window (not one another instance owns), focused, and its
# geometry in the tablet's 0..32767 range of the layout.
c = None
for _ in range(80):
    time.sleep(0.1)
    rows = json.loads(subprocess.run(['hyprctl', '-j', 'clients'], capture_output=True, text=True).stdout)
    c = next((r for r in rows if r.get('pid') == p.pid and r.get('mapped', True)), None)
    if c:
        break
if not c:
    print('no window of the launched process (another instance took the request?)')
    sys.exit(1)
subprocess.run(['hyprctl', 'eval', f'hl.dispatch(hl.dsp.focus({{ window = "address:{c["address"]}" }}))'],
               capture_output=True)
time.sleep(float(os.environ.get('NAVSCROLL_SETTLE', '3')))   # the folder finishes loading
c = next((r for r in json.loads(subprocess.run(['hyprctl', '-j', 'clients'], capture_output=True,
                                                 text=True).stdout) if r['address'] == c['address']), c)
mons = json.loads(subprocess.run(['hyprctl', '-j', 'monitors'], capture_output=True, text=True).stdout)
m = mons[0]
print('window', c.get('class'), c.get('at'), c.get('size'), 'monitor', m['width'], m['height'], flush=True)
x = c['at'][0] + c['size'][0] * 35 // 100 - m['x']   # left of the notification column
y = c['at'][1] + c['size'][1] * 2 // 3 - m['y']
# Move there from elsewhere: a pointer already resting on the spot sends no
# motion, and a window that opened under it gets no pointer focus.
for dx in (-60, 0):
    k.cmd('input-send-event', events=[
        {'type': 'abs', 'data': {'axis': 'x', 'value': (x + dx) * 32767 // m['width']}},
        {'type': 'abs', 'data': {'axis': 'y', 'value': (y + dx) * 32767 // m['height']}}])
    time.sleep(0.15)
time.sleep(0.5)
pass
log.flush()
t_start = time.time()
mark = os.path.getsize('/tmp/navscroll.log')
shots = os.environ.get('NAVSCROLL_SHOTS')   # directory: screenshots during the scroll
for direction in ('wheel-down', 'wheel-up'):
    # NAVSCROLL_INTERVAL_MS: time between wheel notches (25: a mouse wheel
    # turned briskly; 4: a high-rate wheel or touchpad stream).
    iv = float(os.environ.get('NAVSCROLL_INTERVAL_MS', '25')) / 1000
    n = int(float(os.environ.get('NAVSCROLL_SECS', '2')) / iv)   # per direction
    for i in range(n):
        k.cmd('input-send-event', events=[{'type': 'btn', 'data': {'down': True, 'button': direction}},
                                          {'type': 'btn', 'data': {'down': False, 'button': direction}}])
        time.sleep(iv)
        if shots and i % max(1, n // 4) == n // 8:
            subprocess.run(['grim', '-g', f'{c["at"][0]},{c["at"][1]} {c["size"][0]}x{c["size"][1]}',
                            f'{shots}/{direction}-{i:02d}.png'])
time.sleep(0.5)
t_end = time.time()
os.killpg(p.pid, 15)
time.sleep(0.5)
text = open('/tmp/navscroll.log', errors='replace').read()[mark:]
# Commits of the toplevel's surface: the surface committed most often.
commits = {}
for m_ in re.finditer(r'\[(\d+):(\d+):([\d.]+)\]\s+(?:\{[^}]*\}\s+)?-> wl_surface#(\d+)\.commit\(\)', text):
    t = (int(m_.group(1)) * 3600 + int(m_.group(2)) * 60 + float(m_.group(3))) * 1000
    commits.setdefault(m_.group(4), []).append(t)
if not commits:
    print('no commits seen')
    sys.exit(1)
sid, ts = max(commits.items(), key=lambda kv: len(kv[1]))
gaps = [b - a for a, b in zip(ts, ts[1:])]
span = (ts[-1] - ts[0]) / 1000 if len(ts) > 1 else 0
gaps.sort()
print(f'surface {sid}: {len(ts)} frames in {span:.2f} s = {len(ts) / span if span else 0:.0f} fps; '
      f'median gap {gaps[len(gaps) // 2]:.1f} ms, 95% {gaps[int(len(gaps) * .95)]:.1f} ms, '
      f'longest {gaps[-3:]} ms')
