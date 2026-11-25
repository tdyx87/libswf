#!/usr/bin/env python3
# 生成滤镜对比测试 SWF（未压缩 FWS）：
#   1. 无滤镜参照（实色矩形）
#   2. DropShadowFilter（向右下 5px 投影，模糊 4px）
#   3. GlowFilter（黄色辉光，模糊 6px）
#   4. BlurFilter（两轴模糊 4px）
# 用来对比软件光栅器与 D3D11 后端的离屏滤镜链。
import struct
import sys

sys.path.insert(0, '.')
from make_full_test_swf import BW, rect, tag, matrix, define_shape3, rect_edges

PX = 20                 # 1 像素 = 20 twips
FIXED = 65536.0
W, H = 60, 40           # 形状像素尺寸


def fixed(v):
    return struct.pack('<i', int(round(v * FIXED)))


def fixed8(v):
    return struct.pack('<h', int(round(v * 256.0)))


def drop_shadow(blur_px, distance_px, angle_rad, rgba, strength=1.0):
    return (bytes([0]) + bytes(rgba)
            + fixed(blur_px) + fixed(blur_px) + fixed(angle_rad) + fixed(distance_px)
            + fixed8(strength) + bytes([0x01]))          # passes=1


def glow(blur_px, rgba, strength=1.0):
    return (bytes([2]) + bytes(rgba) + fixed(blur_px) + fixed(blur_px)
            + fixed8(strength) + bytes([0x01]))


def blur(blur_px):
    return bytes([1]) + fixed(blur_px) + fixed(blur_px) + bytes([0x01])


def place_object3(depth, char_id, mat, filters):
    """PlaceObject3：HasCharacter|HasMatrix + FilterList。"""
    body = bytearray([0x06, 0x01])            # Flags1, Flags2(HasFilterList)
    body += struct.pack('<H', depth)
    body += struct.pack('<H', char_id)
    body += mat
    body += bytes([len(filters)]) + b''.join(filters)
    return bytes(body)


def solid(rgb):
    return bytes([0x00]) + bytes(rgb) + bytes([255])


shape = define_shape3(1, (0, 0, W * PX, H * PX), solid((80, 160, 220)),
                      rect_edges(W * PX, H * PX), (0, 0))

body = bytearray()
body += tag(9, bytes([40, 40, 48]))       # SetBackgroundColor
body += tag(32, shape)

gap = W * PX + 600
body += tag(70, place_object3(1, 1, matrix(tx=600, ty=600), []))                       # 参照
body += tag(70, place_object3(2, 1, matrix(tx=600 + gap, ty=600),
                              [drop_shadow(4.0, 5.0, 0.785398, (0, 0, 0, 255))]))     # 投影
body += tag(70, place_object3(3, 1, matrix(tx=600 + gap * 2, ty=600),
                              [glow(6.0, (255, 220, 0, 255))]))                        # 辉光
body += tag(70, place_object3(4, 1, matrix(tx=600 + gap * 3, ty=600),
                              [blur(4.0)]))                                            # 模糊
body += tag(1, b"")
body += tag(0, b"")

stage_w, stage_h = 600 + gap * 3 + W * PX + 600, 600 * 2 + H * PX
header = b"FWS" + bytes([8]) + struct.pack('<I', 8 + len(body))
header += rect(0, 0, stage_w, stage_h)
header += struct.pack('<H', 0x0C00)      # 12 fps
header += struct.pack('<H', 1)

with open('testswf/filter_test.swf', 'wb') as f:
    f.write(header + body)
print('wrote testswf/filter_test.swf, %d bytes, stage %dx%d twips'
      % (len(header) + len(body), stage_w, stage_h))
