#!/usr/bin/env python3
"""把导出帧拼成一张缩略图（contact sheet），用于快速检视整段动画。

用法: python tools/contact_sheet.py <帧目录> <输出.png> [列数] [行数]
按时间轴均匀取样，并在每格标注帧号与非黑像素数。
"""
import glob
import os
import sys

from PIL import Image, ImageDraw

src = sys.argv[1]
out = sys.argv[2]
cols = int(sys.argv[3]) if len(sys.argv) > 3 else 4
rows = int(sys.argv[4]) if len(sys.argv) > 4 else 3

files = sorted(glob.glob(os.path.join(src, '*.png')))
if not files:
    print('no frames in', src)
    sys.exit(1)

n = cols * rows
step = max(1, len(files) // n)
picked = [files[min(i * step, len(files) - 1)] for i in range(n)]

cell_w, cell_h = 320, 240
label_h = 18
sheet = Image.new('RGB', (cols * cell_w, rows * (cell_h + label_h)), (24, 24, 24))
draw = ImageDraw.Draw(sheet)

for i, path in enumerate(picked):
    im = Image.open(path).convert('RGB')
    scale = min(cell_w / im.width, cell_h / im.height)
    im = im.resize((int(im.width * scale), int(im.height * scale)), Image.LANCZOS)
    cx = (i % cols) * cell_w + (cell_w - im.width) // 2
    cy = (i // cols) * (cell_h + label_h) + label_h + (cell_h - im.height) // 2
    sheet.paste(im, (cx, cy))

    colors = Image.open(path).convert('RGB').getcolors(maxcolors=1 << 20)
    nonblack = sum(v for v, col in colors if col != (0, 0, 0)) if colors else 0
    name = os.path.basename(path).replace('frame_', '').replace('.png', '')
    draw.text(((i % cols) * cell_w + 4, (i // cols) * (cell_h + label_h) + 3),
              'frame %s  px=%d' % (name, nonblack), fill=(0, 255, 128))

sheet.save(out)
print('%d frames sampled -> %s (%dx%d)' % (n, out, sheet.width, sheet.height))
