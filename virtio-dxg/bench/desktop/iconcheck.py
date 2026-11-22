"""iconcheck.py PNG...: in Files captures of the navtest folder (every entry
the same text-document icon), cuts out every fully visible icon cell and
counts those that differ from the median icon: corrupted icons.
Icon columns are found in the captures (ICON_CENTERS=x,x,... overrides);
the cell under the pointer (navscroll puts it at 35% of the width, 2/3 of
the height of the window) is skipped. Host side (PIL, numpy)."""
import os
import sys

import numpy as np
from PIL import Image

W, H = 84, 92


def runs(mask):
    out, y = [], 0
    while y < len(mask):
        if mask[y]:
            y1 = y
            while y1 < len(mask) and mask[y1]:
                y1 += 1
            out.append((y, y1 - y))
            y = y1
        else:
            y += 1
    return out


def find_centers(images):
    """Icon columns: x ranges 50..100 px wide where many rows are bright."""
    a = np.stack(images)
    prof = (a > 120).sum(axis=(0, 1)) / len(images)
    cols = prof > 0.25 * prof.max()
    return tuple(x + n // 2 for x, n in runs(cols) if 50 <= n <= 100)


def cells(a, centers, pointer):
    tops = set()
    for x in centers:
        strip = (a[:, x - 40:x + 40] > 120).sum(axis=1) > 20
        for y, n in runs(strip):
            if 86 <= n <= 94 and y > 50 and y + H < a.shape[0]:
                tops.add(y)
    # Rows found in any column (a corrupted icon can hide its own row).
    rows = sorted(tops)
    merged = [t for i, t in enumerate(rows) if i == 0 or t - rows[i - 1] > 20]
    px, py = pointer
    return [a[t - 1:t - 1 + H, x - W // 2:x + W // 2] for t in merged for x in centers
            if not (x - W // 2 - 24 <= px <= x + W // 2 and t - 25 <= py <= t + H)]


images = {p: np.asarray(Image.open(p).convert('L'), dtype=np.int16) for p in sys.argv[1:]}
if not images:
    sys.exit('usage: iconcheck.py PNG...')
first = next(iter(images.values()))
env = os.environ.get('ICON_CENTERS')
centers = tuple(int(v) for v in env.split(',')) if env else find_centers(list(images.values()))
pointer = (first.shape[1] * 35 // 100, first.shape[0] * 2 // 3)
print(f'columns {centers}, pointer cell near {pointer}')
per_file = {p: cells(a, centers, pointer) for p, a in images.items()}
everything = [c for cs in per_file.values() for c in cs]
if not everything:
    sys.exit('no icons found')
ref = np.median(np.stack(everything), axis=0)
total = 0
for p, cs in per_file.items():
    diffs = [np.abs(c - ref).mean() for c in cs] or [0]
    bad = sum(d > 25 for d in diffs)
    total += bad
    print(f'{p.replace(chr(92), "/").split("/")[-1]:20} icons {len(cs):3d}  corrupted {bad}  '
          f'(worst {max(diffs):.0f})')
print(f'total corrupted {total} of {len(everything)}')
