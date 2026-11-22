"""Report delivered guest flips and estimated shortfall over marked phases."""
import re
import sys

trace, hz = sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 240.0
period = 1000.0 / hz
flips, marks, outputs = [], [], set()
for line in open(trace, errors='replace'):
    counter = re.search(r'entries-in-buffer/entries-written:\s*(\d+)/(\d+)', line)
    if (counter and counter[1] != counter[2]) or re.search(r'\bLOST \d+ EVENTS\b', line):
        sys.exit('trace lost events; discard this capture')
    m = re.search(r'\s(\d+\.\d+): (\w+):\s*(.*)', line)
    if not m:
        continue
    t = float(m.group(1))
    if m.group(2) == 'drm_vblank_event_delivered':
        flips.append(t)
        crtc = re.search(r'\bcrtc=(\d+)', m.group(3))
        if crtc:
            outputs.add(crtc[1])
    elif 'tracing_mark' in m.group(2):
        marks.append((t, m.group(3).strip()))
if len(outputs) > 1:
    sys.exit('multiple guest outputs in this trace; use one for the pacing comparison')
if not marks or len(marks) % 2:
    sys.exit('missing desktop phase start or end; discard this capture')
print(f"{'phase':10s} {'flips/s':>8s} {'est. missed':>11s} {'p99 ms':>8s} {'worst ms':>8s}")
for (t0, name), (t1, end_mark) in zip(marks[::2], marks[1::2]):
    if end_mark != 'end' or name == 'end' or t1 <= t0:
        sys.exit('invalid desktop phase boundaries; discard this capture')
    f = sorted(t for t in flips if t0 <= t < t1)
    # deskload animates continuously. Include both phase boundaries so a
    # sustained stall before the first or after the last flip stays visible.
    edges = [t0, *f, t1]
    gaps = sorted((b - a) * 1000 for a, b in zip(edges, edges[1:]))
    expected = round((t1 - t0) * hz)
    p99 = gaps[(99 * len(gaps) - 1) // 100]
    print(f"{name:10s} {len(f)/(t1-t0):8.1f} {max(0, expected-len(f)):11d} "
          f"{p99:8.2f} {gaps[-1]:8.2f}")
