"""applaunch.py [RUNS]: time from starting an app to its window being mapped
(Hyprland's openwindow event), first launch after boot and warm launches.
Run as the desktop user, in the desktop session."""
import os
import shlex
import socket
import subprocess
import sys
import time

RUNS = int(sys.argv[1]) if len(sys.argv) > 1 else 3
APPS = [  # command, window class (substring, lower case)
    ('alacritty', 'alacritty'),
    ('foot', 'foot'),
    ('nautilus --new-window', 'nautilus'),
    ('gnome-calculator', 'calculator'),
    ('chromium --new-window about:blank', 'chromium'),
    ('blender --factory-startup', 'blender'),
]
sock_path = os.path.join(os.environ['XDG_RUNTIME_DIR'], 'hypr',
                         os.environ['HYPRLAND_INSTANCE_SIGNATURE'], '.socket2.sock')


def have(cmd):
    return subprocess.run(['which', cmd.split()[0]], capture_output=True).returncode == 0


def launch(cmd, cls, timeout=20.0):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sock_path)
    s.settimeout(0.05)
    t0 = time.monotonic()
    p = subprocess.Popen(shlex.split(cmd), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         start_new_session=True)
    buf, addr = b'', None
    while time.monotonic() - t0 < timeout and addr is None:
        try:
            buf += s.recv(65536)
        except socket.timeout:
            continue
        while b'\n' in buf:
            line, buf = buf.split(b'\n', 1)
            if line.startswith(b'openwindow>>'):
                fields = line[12:].decode(errors='replace').split(',', 3)
                if len(fields) >= 3 and cls in fields[2].lower():
                    addr = fields[0]
                    break
    ms = (time.monotonic() - t0) * 1000 if addr else None
    s.close()
    if addr:
        subprocess.run(['hyprctl', 'dispatch', 'closewindow', 'address:0x' + addr],
                       capture_output=True)
    try:
        p.wait(timeout=5)
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, 9)
    time.sleep(0.5)
    return ms


for cmd, cls in APPS:
    if not have(cmd):
        print(f'{cmd.split()[0]:18s} not installed')
        continue
    times = [launch(cmd, cls) for _ in range(RUNS + 1)]
    fmt = lambda t: f'{t:7.0f}' if t is not None else '   fail'
    print(f'{cmd.split()[0]:18s} first {fmt(times[0])} ms, then ' +
          ' '.join(fmt(t) for t in times[1:]) + ' ms')
