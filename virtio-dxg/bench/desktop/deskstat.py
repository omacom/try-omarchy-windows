"""deskstat.py TRACE [HZ]: per marked phase, page flips per second and the
frames that missed a vblank (gap above 1.5 refresh periods)."""
import re
import sys

trace, hz = sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 240.0
period = 1000.0 / hz
flips, marks = [], []
for line in open(trace, errors='replace'):
    m = re.search(r'\s(\d+\.\d+): (\w+):\s*(.*)', line)
    if not m:
        continue
    t = float(m.group(1))
    if m.group(2) == 'drm_vblank_event_delivered':
        flips.append(t)
    elif 'tracing_mark' in m.group(2):
        marks.append((t, m.group(3).strip()))
print(f"{'phase':10s} {'flips/s':>8s} {'est. missed':>11s} {'p99 ms':>8s} {'worst ms':>8s}")
for i, (t0, name) in enumerate(marks[:-1]):
    t1 = marks[i + 1][0]
    if t1 <= t0 or name == 'end':
        continue
    f = sorted(t for t in flips if t0 <= t < t1)
    # deskload animates continuously. Include both phase boundaries so a
    # sustained stall before the first or after the last flip stays visible.
    edges = [t0, *f, t1]
    gaps = sorted((b - a) * 1000 for a, b in zip(edges, edges[1:]))
    expected = round((t1 - t0) * hz)
    p99 = gaps[(99 * len(gaps) - 1) // 100]
    print(f"{name:10s} {len(f)/(t1-t0):8.1f} {max(0, expected-len(f)):11d} "
          f"{p99:8.2f} {gaps[-1]:8.2f}")
