#!/usr/bin/env python3
# 生成「逐帧重建几何」的两个回归 SWF（未压缩 FWS），各自只放一种场景：
#
#   testswf/morph_anim_test.swf  变形填充（DefineMorphShape）：
#        5 帧的精灵里把同一 depth 上的 Morph 比值 0 -> 100% 补间，
#        绿色条应当逐帧变宽。这条路径每帧都新建插值 Shape，网格缓存若只按
#        Shape 地址命中就会永远画第一帧（"进度条数字在走、条不动"）。
#   testswf/mask_anim_test.swf   逐帧平移的精灵遮罩：
#        5 帧的精灵把 clipDepth 遮罩每帧右移 100px，紫色条上可见的窗口跟着走
#        （真实影片 loading 进度条 cid721 的形态）。
#
# 分成两个文件是为了让回归测试每种场景都能单独落一套 CPU/GPU 图直接对比：
# 一帧里只放一种几何，哪条路径冻结都能一眼看出来。根时间线只负责 ShowFrame，
# 让子精灵按父帧推进。
import struct
import sys

sys.path.insert(0, '.')
from make_full_test_swf import (BW, tag, rect, matrix, define_shape3, rect_edges,
                                solid_fill, place_object2, shape_record_header,
                                straight)

PX = 20                     # 1 像素 = 20 twips
FRAMES = 5                  # 动画帧数（1..5）

MORPH_W0, MORPH_W1, MORPH_H = 40, 600, 120      # 变形条：40px -> 600px
BAR_W, BAR_H = 600, 120                          # 被遮罩的紫条
WIN_W = 120                                      # 遮罩窗口宽度
STEP = 100                                       # 遮罩每帧右移

ORIGIN_X, ORIGIN_Y = 40, 40                      # 内容在舞台上的位置（px）


def morph_section(w_twips, h_twips):
    """单段几何：从 (0,0) 起的矩形，返回已对齐到字节的记录流。

    NumFillBits/NumLineBits 由调用方在段前单独写 1 个字节（解码器先 readUI8
    再进记录位流），所以这里不再写那 8 位。
    """
    bw = BW()
    shape_record_header(bw, fill1=1, move=1)
    bw.u(1, 5)                      # MoveTo 的位宽（0/0 也够）
    bw.s(0, 1)
    bw.s(0, 1)
    bw.u(1, 1)                      # 填充索引 1
    straight(bw, w_twips, 0, 1, 0)
    straight(bw, 0, h_twips, 1, 0)
    straight(bw, -w_twips, 0, 1, 0)
    straight(bw, 0, -h_twips, 1, 0)
    bw.u(0, 6)                      # EndShapeRecord
    return bw.tobytes()


def define_morph_shape(shape_id, w0, w1, h, rgb):
    """tag 46（DefineMorphShape）：实色填充，起止两段共享同一套样式表。"""
    body = bytearray()
    body += struct.pack('<H', shape_id)
    body += rect(0, 0, w0, h)               # StartBounds
    body += rect(0, 0, w1, h)               # EndBounds
    body += struct.pack('<I', 0)            # Offset（解码器按顺序读，忽略此值）
    body += bytes([1])                      # 1 条填充样式
    # MORPHFILLSTYLE：类型字节只写一次，后面跟起始色 RGBA + 终止色 RGBA。
    # 用两个 solid_fill() 直接拼会多写一个类型字节，后续样式表/记录流整体错位 1 字节。
    body += bytes([0x00]) + bytes(rgb) + bytes([255]) + bytes(rgb) + bytes([255])
    body += bytes([0])                      # 0 条线样式
    body += bytes([0x10])                   # StartEdges 前：NumFillBits=1|NumLineBits=0
    body += morph_section(w0, h)            # StartEdges
    body += bytes([0x10])                   # EndEdges 前：同样的打包字节
    body += morph_section(w1, h)            # EndEdges
    return tag(46, body)


def place_ratio(depth, char_id, ratio, move=False):
    """PlaceObject2 + HasRatio（0x10）；ratio 是 0..255 的 UI8，**写成 UI16**。

    规范里 PlaceObject2 的 Ratio 字段是 UI16（0..65535），解码器按 /65535 归一化；
    写成 1 字节会让后面紧跟的 ShowFrame 标签头被当成 Ratio 高字节。
    """
    flags = 0x10 | (0x01 if move else 0) | (0x00 if move else 0x02)
    b = bytearray([flags]) + struct.pack('<H', depth)
    if not move:
        b += struct.pack('<H', char_id)
    b += struct.pack('<H', int(round(ratio / 255.0 * 65535)))
    return bytes(b)


def write_swf(path, stage_w_px, stage_h_px, definition_tags, place_tags):
    """写一个根时间线只有 FRAMES 个空帧的影片；子精灵靠父帧推进。"""
    body = bytearray()
    body += tag(9, bytes([32, 32, 40]))          # SetBackgroundColor
    body += definition_tags
    body += place_tags
    for _ in range(FRAMES):
        body += tag(1, b'')                      # ShowFrame
    body += tag(0, b'')

    stage_rect = rect(0, 0, stage_w_px * PX, stage_h_px * PX)
    header = b'FWS' + bytes([8]) + struct.pack('<I', 8 + len(stage_rect) + 4 + len(body))
    header += stage_rect
    header += struct.pack('<H', 0x0C00)          # 12 fps
    header += struct.pack('<H', FRAMES)

    with open(path, 'wb') as f:
        f.write(header + body)
    print('wrote %s, %d bytes, stage %dx%d px' % (path, len(header) + len(body),
                                                  stage_w_px, stage_h_px))


# ---- 场景一：变形填充逐帧变宽 ----
morph = define_morph_shape(2, MORPH_W0 * PX, MORPH_W1 * PX, MORPH_H * PX, (60, 200, 110))

# 按真实 Flash 的 shape tween 写法：首尾关键帧各 PlaceObject 一次（带 Ratio），
# 中间帧只有 ShowFrame；中间比值由解码器的 expandShapeTweens 按播放头线性插值。
morph_sprite = bytearray()
morph_sprite += struct.pack('<HH', 10, FRAMES)
for f in range(FRAMES):
    if f == 0:
        morph_sprite += tag(26, place_ratio(1, 2, 0, move=False))
    elif f == FRAMES - 1:
        morph_sprite += tag(26, place_ratio(1, 2, 255, move=True))
    morph_sprite += tag(1, b'')                  # ShowFrame
morph_sprite += tag(0, b'')

write_swf('testswf/morph_anim_test.swf', ORIGIN_X + MORPH_W1 + 40, ORIGIN_Y + MORPH_H + 40,
          morph + tag(39, morph_sprite),
          tag(26, place_object2(1, char_id=10, mat=matrix(tx=ORIGIN_X * PX,
                                                          ty=ORIGIN_Y * PX))))

# ---- 场景二：遮罩窗口逐帧右移 ----
window = define_shape3(1, (0, 0, WIN_W * PX, BAR_H * PX), solid_fill((255, 255, 255)),
                       rect_edges(WIN_W * PX, BAR_H * PX), (0, 0))
bar = define_shape3(3, (0, 0, BAR_W * PX, BAR_H * PX), solid_fill((150, 60, 200)),
                    rect_edges(BAR_W * PX, BAR_H * PX), (0, 0))

mask_sprite = bytearray()
mask_sprite += struct.pack('<HH', 11, FRAMES)
for f in range(FRAMES):
    if f == 0:
        mask_sprite += tag(26, place_object2(1, char_id=1, clip_depth=2,
                                             mat=matrix(tx=0, ty=0)))
        mask_sprite += tag(26, place_object2(2, char_id=3, mat=matrix(tx=0, ty=0)))
    else:
        mask_sprite += tag(26, place_object2(1, mat=matrix(tx=f * STEP * PX, ty=0),
                                             move=True))
    mask_sprite += tag(1, b'')                   # ShowFrame
mask_sprite += tag(0, b'')

write_swf('testswf/mask_anim_test.swf',
          ORIGIN_X + BAR_W + 40, ORIGIN_Y + BAR_H + 40,
          tag(32, window) + tag(32, bar) + tag(39, mask_sprite),
          tag(26, place_object2(1, char_id=11, mat=matrix(tx=ORIGIN_X * PX,
                                                          ty=ORIGIN_Y * PX))))
