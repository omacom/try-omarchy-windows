"""gdkframes.py [LOG]: GTK's own frame records (GDK_DEBUG=frames) of a run
(default /tmp/navscroll.log): for the painted frames, the interval between
frames, GTK's work per frame (frame start to frame_end: layout, snapshot,
render, swap), and how many frames missed a 240 Hz refresh."""
import re
import sys

path = sys.argv[1] if len(sys.argv) > 1 else '/tmp/navscroll.log'
rows = []
for line in open(path, errors='replace'):
    m = re.search(r'Gdk-Message: [\d:.]+:\s+(\d+): (.*)', line)
    if not m:
        continue
    kv = dict(re.findall(r'(\w+)=([\d.]+)', m.group(2)))
    f = {k: float(v) for k, v in kv.items() if float(v) < 1e9}
    rows.append(f)
painted = [r for r in rows if 'paint_start' in r and 'frame_end' in r and 'interval' in r]
# The steady part: drop the first frames (startup) and frames after a long idle.
steady = [r for r in painted[5:] if r['interval'] < 100]
if not steady:
    print('no painted frames')
    sys.exit(1)


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(len(v) * p))]


iv = [r['interval'] for r in steady]
print(f'painting: {len(iv) * 1000 / sum(iv):.0f} fps over {sum(iv) / 1000:.2f} s; '
      f'within 4.6 ms of the previous frame: {sum(1 for x in iv if x <= 4.6) * 100 / len(iv):.0f}%')
work = [r['frame_end'] for r in steady]
layout = [r['paint_start'] - r.get('layout_start', 0) for r in steady]
paint = [r['frame_end'] - r['paint_start'] for r in steady]
print(f'{len(steady)} painted frames: interval p50 {pct(iv, .5):.1f} p90 {pct(iv, .9):.1f} '
      f'p99 {pct(iv, .99):.1f} max {max(iv):.1f} ms; '
      f'over 4.6 ms: {sum(1 for x in iv if x > 4.6) * 100 / len(iv):.0f}%')
print(f'GTK work per frame: p50 {pct(work, .5):.2f} p90 {pct(work, .9):.2f} p99 {pct(work, .99):.2f} '
      f'max {max(work):.1f} ms (layout p50 {pct(layout, .5):.2f}, paint+render p50 {pct(paint, .5):.2f}, '
      f'p90 {pct(paint, .9):.2f})')
over = [r for r in steady if r['frame_end'] > 3.5]
print(f'frames whose GTK work exceeds 3.5 ms: {len(over)} ({len(over) * 100 / len(steady):.0f}%)')
