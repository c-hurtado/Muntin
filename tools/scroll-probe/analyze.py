"""Reads the edge recording from scroll-probe.ps1 and reports how well the windows moved.

The recording is 64x720: each column samples the screen every 40 px, each row every 2 px.
Columns 0 and 1 of the probe layout hold Chrome windows of known colours. Page A's top edge and
page B's bottom edge give each window's offset per frame, so disagreement between windows within
one frame is the tearing a viewer would see.
"""
import sys
import numpy as np

raw, labels_path, width, page = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
labels = open(labels_path).read().split('\n')
a = np.fromfile(raw, np.uint8).reshape(-1, 720, 64, 4)[..., :3].astype(int)  # BGR
col = [int((490 + 504 * i) / width * 64) for i in range(2)]  # right of the page's moving square
COLS = {'A0': (col[0], (255, 0, 255), 'top'), 'A1': (col[1], (255, 255, 0), 'top'),
        'B0': (col[0], (0, 255, 255), 'bottom'), 'B1': (col[1], (0, 255, 0), 'bottom')}  # right of the page's moving square


def edge(col, bgr, side):
    m = (abs(a[:, :, col] - np.array(bgr)).sum(-1) < 40)
    has = m.sum(1) > 4
    y = (m.argmax(1) if side == 'top' else 719 - m[:, ::-1].argmax(1)).astype(float) * page / 720
    y[~has] = np.nan
    return y


edges = {k: edge(*v) for k, v in COLS.items()}
rest = {k: np.nanmedian(e[:60]) for k, e in edges.items()}  # A on screen; B parked, so measured below
b_home = {k: np.nanmedian(edges[k][(edges[k] > page * .55)]) for k in ('B0', 'B1')}
off = {'A0': edges['A0'] - rest['A0'], 'A1': edges['A1'] - rest['A1'],
       'B0': edges['B0'] - b_home['B0'] + page, 'B1': edges['B1'] - b_home['B1'] + page}
o = np.vstack(list(off.values()))
step = np.r_[0, np.nan_to_num(np.nanmedian(np.abs(np.diff(o, axis=1)), 0))]
moving = step >= 4
print(f'frames {len(a)}, recorded at 60 fps; offsets are full-resolution px, measured to {page / 720:g} px')

# Contiguous motion, allowing a few still frames inside one slide.
runs, start, gap = [], None, 0
for i, m in enumerate(moving):
    if m:
        start = i if start is None else start
        gap = 0
    elif start is not None:
        gap += 1
        if gap > 45:
            runs.append((start, i - gap))
            start = None
print(f'{len(runs)} slides found, {len(labels)} expected')
print(f'{"run":<12}{"frames":>7}{"still":>7}{"max step":>10}{"spread p50":>12}{"p95":>6}{"max":>6}{"blank":>7}')
for (s, e), label in zip(runs, labels):
    seg = o[:, s:e + 1]
    n = (~np.isnan(seg)).sum(0)
    spread = np.nanmax(seg[:, n >= 2], 0) - np.nanmin(seg[:, n >= 2], 0) if (n >= 2).any() else np.array([0.])
    print(f'{label:<12}{e - s + 1:>7}{int((~moving[s:e + 1]).sum()):>7}{step[s:e + 1].max():>10.0f}'
          f'{np.percentile(spread, 50):>12.0f}{np.percentile(spread, 95):>6.0f}{spread.max():>6.0f}{int(np.isnan(o[:, max(0, s - 6):e + 7]).all(0).sum()):>7}')
