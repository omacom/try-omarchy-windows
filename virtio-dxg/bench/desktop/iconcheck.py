"""iconcheck.py PNG...: in Files captures of the navtest folder (every entry
the same text-document icon, 5 columns), cuts out every fully visible icon
cell and counts those that differ from the median icon: corrupted icons.
Host side (PIL, numpy)."""
import sys

import numpy as np
from PIL import Image

CENTERS = (399, 577, 755, 933, 1111)   # icon column centres at this window size
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


def cells(path):
    a = np.asarray(Image.open(path).convert('L'), dtype=np.int16)
    tops = set()
    for x in CENTERS:
        strip = (a[:, x - 40:x + 40] > 120).sum(axis=1) > 20
        for y, n in runs(strip):
            if 86 <= n <= 94 and y > 50 and y + H < a.shape[0]:
                tops.add(y)
    # Rows found in any column (a corrupted icon can hide its own row).
    rows = sorted(tops)
    merged = [t for i, t in enumerate(rows) if i == 0 or t - rows[i - 1] > 20]
    # The mouse pointer (drawn into captures) rests over the first column
    # near y 893 of the window: skip the cell under it.
    return [a[t - 1:t - 1 + H, x - W // 2:x + W // 2] for t in merged for x in CENTERS
            if not (x == CENTERS[0] and t - 1 <= 905 and t - 1 + H >= 880)]


per_file = {p: cells(p) for p in sys.argv[1:]}
everything = [c for cs in per_file.values() for c in cs]
if not everything:
    sys.exit('no icons found')
ref = np.median(np.stack(everything), axis=0)
total = 0
for p, cs in per_file.items():
    diffs = [np.abs(c - ref).mean() for c in cs]
    bad = sum(d > 25 for d in diffs)
    total += bad
    print(f'{p.replace(chr(92), "/").split("/")[-1]:20} icons {len(cs):3d}  corrupted {bad}  '
          f'(worst {max(diffs):.0f})')
print(f'total corrupted {total} of {len(everything)}')
