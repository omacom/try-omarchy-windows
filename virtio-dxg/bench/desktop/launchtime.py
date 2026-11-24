"""launchtime.py RUNS LABEL=ENV=VALUE[,ENV=VALUE]... -- CMD...: time from
starting CMD to its first frame (the first wl_surface commit with a buffer
attached, from WAYLAND_DEBUG), alternating between labelled environments.
Run as the desktop user with the session environment (sessenv.sh)."""
import os
import re
import statistics
import subprocess
import sys
import time

runs = int(sys.argv[1])
sep = sys.argv.index('--')
variants = []
for spec in sys.argv[2:sep]:
    label, _, envs = spec.partition('=')
    env = dict(kv.split('=', 1) for kv in envs.split(',') if kv)
    variants.append((label, env))
cmd = sys.argv[sep + 1:]
name = os.path.basename(cmd[0])


def first_frame(log):
    """Seconds within the hour of the first commit after a buffer attach."""
    attached = set()
    for line in open(log, errors='replace'):
        m = re.match(r'\[(\d+):(\d+):([\d.]+)\]', line)
        if not m:
            continue
        a = re.search(r'-> wl_surface#(\d+)\.attach\(wl_buffer#', line)
        if a:
            attached.add(a.group(1))
            continue
        c = re.search(r'-> wl_surface#(\d+)\.commit\(\)', line)
        if c and c.group(1) in attached:
            return int(m.group(2)) * 60 + float(m.group(3))
    return None


results = {label: [] for label, _ in variants}
for r in range(runs):
    for label, extra in variants:
        subprocess.run(['pkill', '-x', name])
        time.sleep(0.8)
        env = dict(os.environ, WAYLAND_DEBUG='client', **extra)
        log = f'/tmp/launchtime-{label}.log'
        t0 = time.time()
        p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=open(log, 'w'), env=env)
        t = None
        for _ in range(100):
            time.sleep(0.1)
            t = first_frame(log)
            if t is not None:
                break
        p.terminate()
        p.wait()
        if t is not None:
            ms = ((t - t0 % 3600) % 3600) * 1000
            results[label].append(ms)
for label, v in results.items():
    if v:
        print(f'{label}: first frame median {statistics.median(v):.0f} ms, runs ' +
              ' '.join(f'{x:.0f}' for x in v))
