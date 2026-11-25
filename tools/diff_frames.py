"""CPU/GPU frame parity scan.

Usage: python diff_frames.py <cpu_dir> <gpu_dir> [step_start] [step_end] [stride]

CPU files are p_%04d.png (step id in name), GPU files g_%04d.png with the same
step id. Differences are reported both whole-frame and as the worst 64x64 tile,
because a small wrong widget (progress bar, loading text) barely moves the
whole-frame mean while being a huge local error.
"""
import sys
import os
import numpy as np
from PIL import Image

cpu_dir, gpu_dir = sys.argv[1], sys.argv[2]
start = int(sys.argv[3]) if len(sys.argv) > 3 else 0
end = int(sys.argv[4]) if len(sys.argv) > 4 else 400
stride = int(sys.argv[5]) if len(sys.argv) > 5 else 1

TILE = 64
rows = []
for step in range(start, end, stride):
    cpath = os.path.join(cpu_dir, "p_%04d.png" % step)
    gpath = os.path.join(gpu_dir, "g_%04d.png" % step)
    if not (os.path.exists(cpath) and os.path.exists(gpath)):
        continue
    a = np.asarray(Image.open(cpath).convert("RGB"), dtype=np.int16)
    b = np.asarray(Image.open(gpath).convert("RGB"), dtype=np.int16)
    if a.shape != b.shape:
        print("step %d: size mismatch cpu=%s gpu=%s" % (step, a.shape, b.shape))
        continue
    d = np.abs(a - b).max(axis=2).astype(np.float32)
    mean = d.mean()
    h, w = d.shape
    th, tw = h // TILE, w // TILE
    tiles = d[:th * TILE, :tw * TILE].reshape(th, TILE, tw, TILE).mean(axis=(1, 3))
    ti = np.unravel_index(int(tiles.argmax()), tiles.shape)
    worst = float(tiles[ti])
    tx, ty = ti[1] * TILE, ti[0] * TILE
    rows.append((step, mean, worst, tx, ty))

rows.sort(key=lambda r: -r[2])
print("worst 20 steps by localized (64px tile) diff:")
for step, mean, worst, tx, ty in rows[:20]:
    print("  step %4d  frame_mean=%6.2f  worst_tile=%6.1f at (%4d,%4d)" % (step, mean, worst, tx, ty))
print("total compared: %d" % len(rows))
