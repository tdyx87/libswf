#!/usr/bin/env python3
"""把逐帧 PNG 序列合成 GIF 动画（对比动画的生成入口）。

用法:
  python tools/make_gif.py <frames_dir> <out.gif> [--fps 24] [--scale 0.5] [--prefix rec_] [--start 0] [--count N]
  python tools/make_gif.py --stack <dirA> <dirB> <out.gif> [--fps 24] [--scale 0.5]
      上下堆叠两序列逐帧对比（A=上/OURS, B=下/REF），可配合 ffmpeg/手工参照帧。

帧文件按文件名排序取用（rec_0000.png / p_0000.png / frame_0.png 均可）。
GIF 用 P 模式 256 色 + 帧差优化（Pillow 自动 disposal），播放速率 1/fps 秒每帧。
"""
import argparse
import os
import sys

from PIL import Image, ImageDraw


def list_frames(d, prefix=""):
    fs = [f for f in os.listdir(d)
          if f.lower().endswith('.png') and f.startswith(prefix)]
    fs.sort()
    return [os.path.join(d, f) for f in fs]


def load_scaled(paths, scale):
    imgs = []
    for p in paths:
        im = Image.open(p).convert('RGB')
        if scale != 1.0:
            im = im.resize((max(1, int(im.width * scale)),
                            max(1, int(im.height * scale))), Image.LANCZOS)
        imgs.append(im)
    return imgs


def save_gif(imgs, out, fps, label=None):
    if not imgs:
        print('no frames'); return 1
    d = 1000 // max(1, fps)
    if label:
        bar = 20
        out_imgs = []
        for im in imgs:
            canvas = Image.new('RGB', (im.width, im.height + bar), (15, 15, 15))
            canvas.paste(im, (0, bar))
            ImageDraw.Draw(canvas).text((4, 3), label, fill=(255, 200, 0))
            out_imgs.append(canvas)
        imgs = out_imgs
    imgs[0].save(out, save_all=True, append_images=imgs[1:],
                 duration=d, loop=0, optimize=True)
    print(f'{out}: {len(imgs)} frames @ {fps}fps  {imgs[0].width}x{imgs[0].height}')
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--stack', action='store_true', help='两序列上下堆叠对比')
    ap.add_argument('--fps', type=int, default=24)
    ap.add_argument('--scale', type=float, default=0.5)
    ap.add_argument('--prefix', default='')
    ap.add_argument('--start', type=int, default=0)
    ap.add_argument('--count', type=int, default=0)
    ap.add_argument('--label-a', default='OURS swfplayer')
    ap.add_argument('--label-b', default='REF')
    ap.add_argument('dirs_out', nargs='+')
    a = ap.parse_args()

    if a.stack:
        if len(a.dirs_out) != 3:
            print('usage: --stack <dirA> <dirB> <out.gif>'); return 1
        fa = list_frames(a.dirs_out[0], a.prefix)
        fb = list_frames(a.dirs_out[1], a.prefix)
        n = min(len(fa), len(fb))
        if a.start: fa, fb = fa[a.start:], fb[a.start:]
        if a.count: n = min(n, a.count); fa, fb = fa[:n], fb[:n]
        if n == 0:
            print('no common frames'); return 1
        ia, ib = load_scaled(fa[:n], a.scale), load_scaled(fb[:n], a.scale)
        pairs = []
        bar = 20
        for x, y in zip(ia, ib):
            w = max(x.width, y.width)
            canvas = Image.new('RGB', (w, x.height + y.height + bar * 2), (15, 15, 15))
            ImageDraw.Draw(canvas).text((4, 3), f'{a.label_a}', fill=(255, 200, 0))
            canvas.paste(x, (0, bar))
            ImageDraw.Draw(canvas).text((4, bar + x.height + 3), f'{a.label_b}', fill=(0, 255, 128))
            canvas.paste(y, (0, bar * 2 + x.height))
            pairs.append(canvas)
        return save_gif(pairs, a.dirs_out[2], a.fps)
    else:
        if len(a.dirs_out) != 2:
            print('usage: <frames_dir> <out.gif>'); return 1
        fs = list_frames(a.dirs_out[0], a.prefix)
        if a.start: fs = fs[a.start:]
        if a.count: fs = fs[:a.count]
        return save_gif(load_scaled(fs, a.scale), a.dirs_out[1], a.fps)


if __name__ == '__main__':
    sys.exit(main())
