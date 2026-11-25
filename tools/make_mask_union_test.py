#!/usr/bin/env python3
# 生成「同一裁剪组多条遮罩形状」的对比测试 SWF（未压缩 FWS）：
#   裁剪层是一个精灵，第 1 帧里放了两块互不相邻的矩形，语义是两块区域的并集。
#   被裁剪的是一整块红色矩形，所以正确画面 = 红块只在两个窗口里出现。
# 只画一个窗口（旧行为：GPU 只取第一条遮罩形状）会让第二块窗口整块变暗。
import struct
import sys

sys.path.insert(0, '.')
from make_full_test_swf import (tag, rect, matrix, define_shape3, rect_edges,
                                solid_fill, place_object2)

PX = 20                 # 1 像素 = 20 twips
CELL_W, CELL_H = 60, 40 # 单个窗口的像素尺寸
GAP = 20                # 两个窗口之间的像素间隔

shape = define_shape3(1, (0, 0, CELL_W * PX, CELL_H * PX), solid_fill((80, 160, 220)),
                      rect_edges(CELL_W * PX, CELL_H * PX), (0, 0))
# 被裁剪的内容：盖住两个窗口的一整块红条。
content_w = CELL_W * 2 + GAP
content = define_shape3(2, (0, 0, content_w * PX, CELL_H * PX), solid_fill((235, 70, 70)),
                        rect_edges(content_w * PX, CELL_H * PX), (0, 0))

# 遮罩精灵：同一形状放两次，位置相差 CELL_W + GAP。
sprite = bytearray()
sprite += struct.pack('<HH', 10, 1)                       # SpriteId, FrameCount
sprite += tag(26, place_object2(1, char_id=1, mat=matrix(tx=0, ty=0)))
sprite += tag(26, place_object2(2, char_id=1,
                                mat=matrix(tx=(CELL_W + GAP) * PX, ty=0)))
sprite += tag(1, b"")                                     # ShowFrame
sprite += tag(0, b"")                                     # End

body = bytearray()
body += tag(9, bytes([40, 40, 48]))                       # SetBackgroundColor
body += tag(32, shape)                                    # DefineShape3 id=1
body += tag(32, content)                                  # DefineShape3 id=2
body += tag(39, sprite)                                   # DefineSprite id=10
body += tag(26, place_object2(1, char_id=10, clip_depth=2,
                              mat=matrix(tx=600, ty=600)))          # 遮罩 depth1
body += tag(26, place_object2(2, char_id=2, mat=matrix(tx=600, ty=600)))  # depth2
body += tag(1, b"")
body += tag(0, b"")

stage_w, stage_h = 600 * 2 + content_w * PX, 600 * 2 + CELL_H * PX
header = b"FWS" + bytes([8]) + struct.pack('<I', 8 + len(body))
header += rect(0, 0, stage_w, stage_h)
header += struct.pack('<H', 0x0C00)      # 12 fps
header += struct.pack('<H', 1)

with open('testswf/mask_union_test.swf', 'wb') as f:
    f.write(header + body)
print('wrote testswf/mask_union_test.swf, %d bytes, stage %dx%d twips'
      % (len(header) + len(body), stage_w, stage_h))
