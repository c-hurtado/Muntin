# Builds FancyZonesLib\icon.ico from icon.png (the artwork, transparent, 512 px) and, for 24 px and below, a simpler
# drawing without the chain: the panes and a gold ring, which still read at tray size.
# Usage: python build.py [folder for the small preview PNGs]
import os, sys
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ICO = os.path.join(HERE, '..', '..', 'src', 'modules', 'fancyzones', 'FancyZonesLib', 'icon.ico')
SIZES = [16, 20, 24, 32, 40, 48, 64, 256]  # The same sizes as the stock icon
SMALL = 24

art = Image.open(os.path.join(HERE, 'icon.png')).convert('RGBA')
assert art.size == (512, 512), 'icon.png must be the 512 px artwork'

# Measured on icon.png: pane boxes, and the lens centre with its outer and glass radius
PANES = [(20, 22, 247, 495), (262, 22, 493, 251), (262, 266, 493, 495)]
LENS = (258, 165, 115, 88)
SAMPLE_X = [50, 463, 463]  # Columns of each pane clear of the lens and the chain


def small_art():
    """The panes as vertical gradients of their own colours (sampled clear of the lens and chain), and a plain ring."""
    s = 4
    big = Image.new('RGBA', (512 * s, 512 * s))
    for (left, top, right, bottom), x in zip(PANES, SAMPLE_X):
        top_colour, bottom_colour = art.getpixel((x, top + 30)), art.getpixel((x, bottom - 30))
        gradient = Image.new('RGBA', (1, 2))
        gradient.putdata([top_colour, bottom_colour])
        gradient = gradient.resize(((right - left + 1) * s, (bottom - top + 1) * s), Image.BILINEAR)
        mask = Image.new('L', gradient.size)
        ImageDraw.Draw(mask).rounded_rectangle((0, 0, *mask.size), 40 * s, fill=255)
        big.paste(gradient, (left * s, top * s), mask)
    draw = ImageDraw.Draw(big)
    x, y, outer, glass = (v * s for v in LENS)
    draw.ellipse((x - outer, y - outer, x + outer, y + outer), fill=(60, 34, 8, 255))  # Dark outline
    ring = outer - 10 * s
    draw.ellipse((x - ring, y - ring, x + ring, y + ring), fill=(226, 176, 80, 255))  # Gold
    draw.ellipse((x - glass, y - glass, x + glass, y + glass), fill=(60, 34, 8, 255))
    inner = glass - 8 * s
    draw.ellipse((x - inner, y - inner, x + inner, y + inner), fill=(205, 215, 235, 255))  # Pale glass
    return big


small = small_art()
images = [(small if size <= SMALL else art).resize((size, size), Image.LANCZOS) for size in SIZES]
for image in images:
    assert image.getpixel((0, 0))[3] < 64, 'the corners must stay (nearly) transparent'
if len(sys.argv) > 1:
    os.makedirs(sys.argv[1], exist_ok=True)
    for image in images:
        image.save(os.path.join(sys.argv[1], f'{image.width}.png'))

images[-1].save(ICO, sizes=[i.size for i in images], append_images=images[:-1])
print(os.path.normpath(ICO), os.path.getsize(ICO))
