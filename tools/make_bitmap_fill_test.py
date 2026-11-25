#!/usr/bin/env python3
# 生成位图填充对比测试 SWF（未压缩 FWS）：
#   - 形状 1：平铺位图填充（FillStyle1 type 0x40），贴图按 32 像素周期重复
#   - 形状 2：裁剪位图填充（FillStyle1 type 0x41），贴图范围外必须留空（透明）
# 两个形状几何完全相同、并排放置，唯一的差别就是 tiled 标志。软件光栅器与
# D3D11 后端对同一帧的渲染结果应当一致。
import struct
import sys

sys.path.insert(0, '.')
from make_full_test_swf import (BW, rect, tag, matrix, define_shape3,
                                rect_edges, place_object2, define_lossless2)

TILE = 32                      # 贴图像素尺寸
TWIPS_PER_PIXEL = 20           # SWF 规范：1 像素 = 20 twips
SHAPE_W, SHAPE_H = 2000, 1200  # 形状尺寸（twips）：100x60 像素


def bitmap_fill(bmp_id, tiled):
    # bitmapMatrix 是「位图像素 -> 形状 twips」：1 像素 = 20 twips。
    m = matrix(sx=float(TWIPS_PER_PIXEL), sy=float(TWIPS_PER_PIXEL))
    return bytes([0x40 if tiled else 0x41]) + struct.pack("<H", bmp_id) + m


# 32x32 四象限贴图：左上红、右上绿、左下蓝、右下黄
px = bytearray()
for j in range(TILE):
    for i in range(TILE):
        if j < TILE // 2:
            px += bytes((255, 0, 0, 255)) if i < TILE // 2 else bytes((0, 255, 0, 255))
        else:
            px += bytes((0, 0, 255, 255)) if i < TILE // 2 else bytes((255, 255, 0, 255))
bmp_tag = define_lossless2(10, TILE, TILE, px)

shape1 = define_shape3(1, (0, 0, SHAPE_W, SHAPE_H), bitmap_fill(10, True),
                       rect_edges(SHAPE_W, SHAPE_H), (0, 0))
shape2 = define_shape3(2, (0, 0, SHAPE_W, SHAPE_H), bitmap_fill(10, False),
                       rect_edges(SHAPE_W, SHAPE_H), (0, 0))

body = bytearray()
body += tag(9, bytes([0x33, 0x66, 0x99]))      # SetBackgroundColor
body += bmp_tag
body += tag(32, shape1)
body += tag(32, shape2)
body += tag(26, place_object2(1, char_id=1))
body += tag(26, place_object2(2, char_id=2, mat=matrix(tx=SHAPE_W + 400)))
body += tag(1, b"")
body += tag(0, b"")

STAGE_W = SHAPE_W * 2 + 400
STAGE_H = SHAPE_H + 400
header = b"FWS" + bytes([3]) + struct.pack("<I", 8 + len(body))
header += rect(0, 0, STAGE_W, STAGE_H)
header += struct.pack("<H", 0x0C00)   # 12 fps
header += struct.pack("<H", 1)        # frameCount

with open("testswf/bitmap_fill_test.swf", "wb") as f:
    f.write(header + body)
print("wrote testswf/bitmap_fill_test.swf, %d bytes" % (len(header) + len(body)))
