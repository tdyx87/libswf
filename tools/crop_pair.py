"""Crop the same region out of a set of frames and stack them for visual comparison.

Usage: python crop_pair.py <out.png> x0 y0 x1 y1 <cpu.png> <gpu.png> [<cpu2.png> <gpu2.png> ...]
"""
import sys
import numpy as np
from PIL import Image, ImageDraw

out = sys.argv[1]
x0, y0, x1, y1 = (int(v) for v in sys.argv[2:6])
pairs = sys.argv[6:]

tiles = []
for i in range(0, len(pairs), 2):
    for label, path in (("CPU", pairs[i]), ("GPU", pairs[i + 1])):
        im = Image.open(path).convert("RGB").crop((x0, y0, x1, y1))
        canvas = Image.new("RGB", (im.width, im.height + 14), (0, 0, 0))
        canvas.paste(im, (0, 14))
        ImageDraw.Draw(canvas).text((2, 1), "%s %s" % (label, path.split("/")[-1]), fill=(255, 255, 0))
        tiles.append(canvas)

W = max(t.width for t in tiles)
H = sum(t.height for t in tiles)
sheet = Image.new("RGB", (W, H), (0, 0, 0))
y = 0
for t in tiles:
    sheet.paste(t, (0, y))
    y += t.height
sheet.save(out)
print(out, sheet.size)
