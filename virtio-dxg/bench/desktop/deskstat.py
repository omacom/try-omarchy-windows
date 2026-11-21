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
print(f"{'phase':10s} {'flips/s':>8s} {'missed':>7s} {'worst gaps (ms)':s}")
for i, (t0, name) in enumerate(marks[:-1]):
    t1 = marks[i + 1][0]
    f = [t for t in flips if t0 + 1.0 <= t < t1]  # skip the load's start-up second
    gaps = [(b - a) * 1000 for a, b in zip(f, f[1:])]
    # Hyprland renders only when something changed: between workspace
    # switches it may idle; count gaps inside animation bursts only.
    burst = [g for g in gaps if g < 100]
    missed = sum(1 for g in burst if g > 1.5 * period)
    span = (f[-1] - f[0]) if len(f) > 1 else 1
    worst = sorted(burst, reverse=True)[:5]
    print(f"{name:10s} {len(f) / span:8.1f} {missed:7d} {[round(g, 1) for g in worst]}")
