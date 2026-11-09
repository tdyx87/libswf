#!/usr/bin/env python3
"""加载转盘对比：官方 Flash Player (cmp_ref/burst_*) vs 我方 swfparser (cmp_ours/p_*)。

产出：
  - 逐序列"运动信号"（相邻帧平均绝对差），用于判断转盘是否旋转 / 何时停转；
  - 帧间最佳匹配偏移（量化官方与我方时序错位）；
  - 一张并排缩略图（官方上排、我方下排），便于目视核对。
"""
import glob
import os

import numpy as np
from PIL import Image, ImageDraw

OUR = r'build\compare\cmp_ours'
REF = r'build\compare\cmp_ref'
OUT = r'build\compare\cmp_loading'


def load_seq(d, pat):
    files = sorted(glob.glob(os.path.join(d, pat)))
    imgs = [np.asarray(Image.open(f).convert('L')) for f in files]
    return files, imgs


def motion_signal(imgs):
    sig = []
    for i in range(1, len(imgs)):
        d = np.abs(imgs[i].astype(int) - imgs[i - 1].astype(int))
        sig.append(float(d.mean()))
    return sig


def small(imgs, w=160, h=96):
    return [np.asarray(Image.fromarray(x).resize((w, h))) for x in imgs]


def best_match(ours_s, ref_s):
    """对每帧我方，找官方最小 SSD 帧；返回 (偏移列表, 平均偏移)。"""
    offs = []
    for o in ours_s:
        best, bi = 1e18, 0
        for j, r in enumerate(ref_s):
            ssd = np.abs(o.astype(int) - r.astype(int)).sum()
            if ssd < best:
                best, bi = ssd, j
        offs.append(bi)
    return offs


def main():
    os.makedirs(OUT, exist_ok=True)
    of, oi = load_seq(OUR, 'p_*.png')
    rf, ri = load_seq(REF, 'burst_*.png')
    print('OUR frames=%d  REF frames=%d' % (len(oi), len(ri)))

    osig = motion_signal(oi)
    rsig = motion_signal(ri)
    print('\n[运动信号] 相邻帧平均绝对差 (0=静止/卡住, 越大越动):')
    print('  OURS step :', ' '.join('%4.1f' % v for v in osig[::5]))
    print('  REF  frame: ', ' '.join('%4.1f' % v for v in rsig[::10]))

    os_ = small(oi)
    rs_ = small(ri)
    offs = best_match(os_, rs_)
    mean_off = sum(offs) / max(1, len(offs))
    print('\n[帧匹配] 我方每步最佳对应的官方帧偏移 (前20):')
    print(' ', offs[:20])
    print('  平均偏移=%.1f 帧 (官方帧率24fps => 约 %.2fs 时序错位)' % (mean_off, mean_off / 24.0))

    # 并排缩略图：官方上排(取样12) + 我方下排(取样12)
    def sample(files, n=12):
        step = max(1, len(files) // n)
        return [files[min(i * step, len(files) - 1)] for i in range(n)]

    o_s = sample(of, 12)
    r_s = sample(rf, 12)
    cw, ch = 280, 210
    lab = 16
    sheet = Image.new('RGB', (12 * cw, 2 * (ch + lab)), (24, 24, 24))
    d = ImageDraw.Draw(sheet)
    for i, p in enumerate(r_s):
        im = Image.open(p).convert('RGB').resize((cw, ch), Image.LANCZOS)
        sheet.paste(im, (i * cw, lab))
        d.text((i * cw + 3, 3), os.path.basename(p), fill=(0, 255, 128))
    for i, p in enumerate(o_s):
        im = Image.open(p).convert('RGB').resize((cw, ch), Image.LANCZOS)
        y = ch + lab * 2
        sheet.paste(im, (i * cw, y))
        d.text((i * cw + 3, y - lab + 2), os.path.basename(p), fill=(255, 200, 0))
    d.text((3, ch + 2), 'REF flashplayer (上)', fill=(0, 255, 128))
    d.text((3, 2 * ch + lab + 2), 'OURS swfparser (下)', fill=(255, 200, 0))
    out = os.path.join(OUT, 'loading_sidebyside.png')
    sheet.save(out)
    print('\n[saved]', out)

    # 各自 contact sheet
    from subprocess import run
    run(['python', 'tools/contact_sheet.py', OUR, os.path.join(OUT, 'ours_sheet.png'), '6', '2'])
    run(['python', 'tools/contact_sheet.py', REF, os.path.join(OUT, 'ref_sheet.png'), '8', '3'])


if __name__ == '__main__':
    main()
