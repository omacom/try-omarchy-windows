"""menutest.py OUTDIR: Files' main menu and a file's context menu, through
QEMU's tablet (QMP_PORT): captures each open menu (OUTDIR/main.png,
OUTDIR/context.png) and reports GTK's frames while they open and close
(GDK_DEBUG=frames; intervals of the painted frames). Session environment
(sessenv.sh) and HYPRLAND_INSTANCE_SIGNATURE set."""
import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmpkeys import Keys  # noqa: E402

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
subprocess.run(['pkill', '-x', 'nautilus'])
time.sleep(0.8)
log = open('/tmp/menutest.log', 'w')
env = dict(os.environ, GDK_DEBUG='frames')
p = subprocess.Popen(['nautilus', '--new-window', '/tmp/navtest'], stdout=subprocess.DEVNULL,
                     stderr=log, env=env, start_new_session=True)
k = Keys()
c = None
for _ in range(80):
    time.sleep(0.1)
    rows = json.loads(subprocess.run(['hyprctl', '-j', 'clients'], capture_output=True, text=True).stdout)
    c = next((r for r in rows if r.get('pid') == p.pid), None)
    if c:
        break
subprocess.run(['hyprctl', 'eval', f'hl.dispatch(hl.dsp.focus({{ window = "address:{c["address"]}" }}))'],
               capture_output=True)
time.sleep(2.5)
m = json.loads(subprocess.run(['hyprctl', '-j', 'monitors'], capture_output=True, text=True).stdout)[0]
wx, wy = c['at'][0] - m['x'], c['at'][1] - m['y']


def move(x, y):
    k.cmd('input-send-event', events=[
        {'type': 'abs', 'data': {'axis': 'x', 'value': x * 32767 // m['width']}},
        {'type': 'abs', 'data': {'axis': 'y', 'value': y * 32767 // m['height']}}])


def click(x, y, button='left'):
    move(x - 40, y - 40)
    time.sleep(0.15)
    move(x, y)
    time.sleep(0.15)
    k.cmd('input-send-event', events=[{'type': 'btn', 'data': {'down': True, 'button': button}}])
    time.sleep(0.05)
    k.cmd('input-send-event', events=[{'type': 'btn', 'data': {'down': False, 'button': button}}])


def grab(name):
    subprocess.run(['grim', '-g', f'{c["at"][0]},{c["at"][1]} {c["size"][0]}x{c["size"][1]}',
                    f'{out}/{name}.png'])


def frames(since):
    rows = []
    text = open('/tmp/menutest.log', errors='replace').read()[since:]
    for line in text.splitlines():
        mm = re.search(r'Gdk-Message: [\d:.]+:\s+\d+: (.*)', line)
        if not mm:
            continue
        f = {a: float(b) for a, b in re.findall(r'(\w+)=([\d.]+)', mm.group(1)) if float(b) < 1e9}
        if 'paint_start' in f and 'interval' in f:
            rows.append(f)
    iv = [r['interval'] for r in rows if r['interval'] < 100]
    work = sorted(r.get('frame_end', 0) for r in rows)
    if not iv:
        return 'no painted frames'
    return (f'{len(rows)} painted frames, interval median {sorted(iv)[len(iv) // 2]:.1f} ms, '
            f'max {max(iv):.1f} ms, over 4.6 ms {sum(1 for x in iv if x > 4.6)}; '
            f'GTK work median {work[len(work) // 2]:.1f} ms, max {work[-1]:.1f} ms')


# The main menu: the button left of the sidebar's edge in the header bar.
log.flush()
mark = os.path.getsize('/tmp/menutest.log')
click(wx + 271, wy + 22)
time.sleep(0.6)
grab('main')
k.combo('esc')
time.sleep(0.6)
print('main menu open+close:', frames(mark))
# A file's context menu.
mark = os.path.getsize('/tmp/menutest.log')
click(wx + 400, wy + 300, 'right')
time.sleep(0.6)
grab('context')
k.combo('esc')
time.sleep(0.6)
print('context menu open+close:', frames(mark))
os.killpg(p.pid, 15)
