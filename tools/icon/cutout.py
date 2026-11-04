"""Cuts the flat background out of the generated Muntin icon art.

python cutout.py <source.png> <out-dir>

The source has a flat grey background. A pixel is background when it is close to that grey, or when it is a neutral
grey darker than it (the drop shadows). Holes inside the artwork are filled, so the glass of the lens stays opaque,
and the edge pixels are un-mixed from the grey so no grey fringe is left on a light or dark backdrop.

Writes:
  icon-clean.png  the artwork on a transparent square
  preview.png     on dark and light backgrounds, large and at taskbar sizes
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

NEAR, FAR = 10, 34  # Colour distance from the background: below NEAR is background, above FAR is fully artwork
# Measured on the 1024 px source: the glass of the lens (centre, radius) and the columns of the gap seen through it
GLASS = (523, 352, 145)
GAP = (498, 518)

source, out = Path(sys.argv[1]), Path(sys.argv[2])
out.mkdir(parents=True, exist_ok=True)
rgb = np.asarray(Image.open(source).convert("RGB")).astype(np.float32)
border = np.concatenate([rgb[:8].reshape(-1, 3), rgb[-8:].reshape(-1, 3), rgb[:, :8].reshape(-1, 3), rgb[:, -8:].reshape(-1, 3)])
background = np.median(border, axis=0)

distance = np.abs(rgb - background).max(axis=2)
spread = rgb.max(axis=2) - rgb.min(axis=2)
shadow = (spread < 14) & (rgb.max(axis=2) <= background.max() + 2)  # Neutral and darker: a drop shadow, not artwork
alpha = np.clip((distance - NEAR) / (FAR - NEAR), 0, 1)
alpha[shadow] = 0

# Fill holes so nothing inside the artwork is see-through, and drop specks
solid = ndimage.binary_fill_holes(ndimage.binary_opening(alpha > 0.5, iterations=2))
labels, count = ndimage.label(solid)
sizes = ndimage.sum(solid, labels, range(1, count + 1))
solid = np.isin(labels, 1 + np.flatnonzero(sizes > 400))
alpha = np.where(solid, 1.0, np.where(ndimage.binary_dilation(solid, iterations=2), alpha, 0))
alpha = ndimage.gaussian_filter(alpha, 0.5) * ndimage.binary_dilation(solid, iterations=3)

# Un-mix the edge pixels from the background grey: c = a * art + (1 - a) * background
a = np.maximum(alpha[..., None], 1e-3)
colour = np.clip((rgb - (1 - a) * background) / a, 0, 255)

# Inside the lens the glass shows the grey gap between the panes. Make that glass see-through too: each pixel there
# was glass over grey, so un-mix it from the grey with the least alpha that keeps the colour in range.
glass_x, glass_y, glass_r = GLASS
yy, xx = np.mgrid[: rgb.shape[0], : rgb.shape[1]]
in_glass = (xx - glass_x) ** 2 + (yy - glass_y) ** 2 <= glass_r**2
stripe = in_glass & (xx >= GAP[0]) & (xx <= GAP[1])
see_through = np.clip(((rgb - background) / (255 - background)).max(axis=2), 0.05, 1)
alpha = np.where(stripe, see_through, alpha)
a = np.maximum(alpha[..., None], 1e-3)
colour = np.where(stripe[..., None], np.clip((rgb - (1 - a) * background) / a, 0, 255), colour)

image = Image.fromarray(np.dstack([colour, alpha * 255]).round().astype(np.uint8), "RGBA")
box = image.getbbox()
side = max(box[2] - box[0], box[3] - box[1])
clean = Image.new("RGBA", (side, side))
clean.paste(image.crop(box), ((side - (box[2] - box[0])) // 2, (side - (box[3] - box[1])) // 2))
clean.save(out / "icon-clean.png")

# Preview: dark and light backgrounds, large then 48, 32, 24, 16
backgrounds = [(32, 32, 36), (243, 243, 243)]
large, smalls = 256, [48, 32, 24, 16]
cell = large + 24 + sum(smalls) + 12 * len(smalls) + 24
sheet = Image.new("RGB", (cell * 2, large + 40))
for column, colour_bg in enumerate(backgrounds):
    x0 = column * cell
    sheet.paste(Image.new("RGB", (cell, large + 40), colour_bg), (x0, 0))
    tile = clean.resize((large, large), Image.LANCZOS)
    sheet.paste(tile, (x0 + 12, 20), tile)
    x = x0 + 12 + large + 24
    for small in smalls:
        tile = clean.resize((small, small), Image.LANCZOS)
        sheet.paste(tile, (x, 20 + (large - small) // 2), tile)
        x += small + 12
sheet.save(out / "preview.png")
print(out / "preview.png")
