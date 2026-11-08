#!/usr/bin/env python3
"""并排对比图：参考帧（官方 Flash Player 截屏）在上，我方渲染在下。

用法: python tools/cmp_sheet.py <ref目录> <our目录> <输出目录> <pair...>
pair 格式: ref名:our帧号，例如 menu:60 select:130
每对生成一张 <ref名>_cmp.png（宽 960，上下两行带标签）。
"""
import os
import sys

from PIL import Image, ImageDraw

W = 960


def main():
    refdir, ourdir, outdir = sys.argv[1], sys.argv[2], sys.argv[3]
    os.makedirs(outdir, exist_ok=True)
    for pair in sys.argv[4:]:
        name, frame = pair.split(':')
        ref = Image.open(os.path.join(refdir, name + '.png')).convert('RGB')
        our = Image.open(os.path.join(ourdir, 'p_%04d.png' % int(frame))).convert('RGB')
        rh = int(W * ref.height / ref.width)
        oh = int(W * our.height / our.width)
        label = 22
        sheet = Image.new('RGB', (W, rh + oh + label * 2), (20, 20, 20))
        d = ImageDraw.Draw(sheet)
        sheet.paste(ref.resize((W, rh), Image.LANCZOS), (0, label))
        sheet.paste(our.resize((W, oh), Image.LANCZOS), (0, label * 2 + rh))
        d.text((4, 4), 'REF flashplayer: ' + name, fill=(0, 255, 128))
        d.text((4, label + rh + 4), 'OURS swfparser: p_%04d' % int(frame), fill=(255, 200, 0))
        out = os.path.join(outdir, name + '_cmp.png')
        sheet.save(out)
        print(out)


if __name__ == '__main__':
    main()
