#!/usr/bin/env python3
# 生成综合功能测试 SWF（未压缩 FWS）：覆盖
#   - 渐变填充（线性 red->blue）
#   - 位图填充（DefineBitsLossless2 + 平铺纹理）
#   - 裁剪（clipDepth 圆形遮罩裁剪位图矩形）
#   - 多帧动画（第 2 帧移动渐变矩形）
# 纯手工拼装字节，不依赖任何外部库（仅标准库 + zlib）。
import struct, zlib

class BW:
    def __init__(self):
        self.bits = []
    def u(self, val, n):
        for i in range(n - 1, -1, -1):
            self.bits.append((val >> i) & 1)
    def s(self, val, n):
        v = val
        if v < 0:
            v = (1 << n) + v
        self.u(v & ((1 << n) - 1), n)
    def tobytes(self):
        b = self.bits[:]
        while len(b) % 8:
            b.append(0)
        out = bytearray()
        for i in range(0, len(b), 8):
            x = 0
            for j in range(8):
                x = (x << 1) | b[i + j]
            out.append(x)
        return bytes(out)

def nbits_signed(v):
    n = 1
    while (1 << (n - 1)) <= abs(v):
        n += 1
    return n

def rect(x0, y0, x1, y1):
    n = max(nbits_signed(x0), nbits_signed(x1), nbits_signed(y0), nbits_signed(y1))
    bw = BW()
    bw.u(n, 5)
    bw.s(x0, n); bw.s(x1, n); bw.s(y0, n); bw.s(y1, n)
    return bw.tobytes()

def shape_record_header(bw, new=0, line=0, fill1=0, fill0=0, move=0):
    bw.u(0, 1); bw.u(new, 1); bw.u(line, 1); bw.u(fill1, 1); bw.u(fill0, 1); bw.u(move, 1)

def curve(bw, dxc, dyc, dxa, dya, nfill, nline):
    nb = 2
    for v in (dxc, dyc, dxa, dya):
        while (1 << (nb - 1)) <= abs(v):
            nb += 1
    field = nb - 2
    bw.u(1, 1); bw.u(0, 1); bw.u(field, 4)
    bw.s(dxc, nb); bw.s(dyc, nb); bw.s(dxa, nb); bw.s(dya, nb)

def straight(bw, dx, dy, nfill, nline):
    nb = 2
    for v in (dx, dy):
        while (1 << (nb - 1)) <= abs(v):
            nb += 1
    general = 1 if (dx != 0 and dy != 0) else 0
    field = nb - 2
    bw.u(1, 1); bw.u(1, 1); bw.u(field, 4)
    bw.u(general, 1)   # GeneralLineFlag（SWF 规范要求的标志位）
    if general:
        bw.s(dx, nb); bw.s(dy, nb)
    else:
        vert = 1 if dy != 0 else 0
        bw.u(vert, 1)
        if vert: bw.s(dy, nb)
        else: bw.s(dx, nb)

def rect_edges(w, h):
    return [('s', w, 0), ('s', 0, h), ('s', -w, 0), ('s', 0, -h)]

def circle_edges(cx, cy, R):
    k = int(0.5523 * R)
    P = [(cx + R, cy), (cx, cy + R), (cx - R, cy), (cx, cy - R)]
    C = [(cx + R, cy + k), (cx + k, cy + R), (cx - R, cy - k), (cx - k, cy - R)]
    e = []
    for i in range(4):
        nx = (i + 1) % 4
        e.append(('c', C[i][0] - P[i][0], C[i][1] - P[i][1], P[nx][0] - P[i][0], P[nx][1] - P[i][1]))
    return e

def tag(code, data):
    length = len(data)
    if length < 63:
        hdr = struct.pack("<H", (code << 6) | length)
    else:
        hdr = struct.pack("<H", (code << 6) | 63) + struct.pack("<I", length)
    return hdr + data

# ---- MATRIX（16.16 定点） ----
def matrix(sx=None, sy=None, r0=None, r1=None, tx=0, ty=0):
    bw = BW()
    hasScale = 1 if (sx is not None or sy is not None) else 0
    bw.u(hasScale, 1)
    if hasScale:
        sxv = sx if sx is not None else 1.0
        syv = sy if sy is not None else 1.0
        n = max(nbits_signed(int(round(sxv * 65536))), nbits_signed(int(round(syv * 65536))))
        bw.u(n, 5)
        bw.s(int(round(sxv * 65536)), n)
        bw.s(int(round(syv * 65536)), n)
    hasRot = 1 if (r0 is not None or r1 is not None) else 0
    bw.u(hasRot, 1)
    if hasRot:
        n = max(nbits_signed(int(round(r0 * 65536))), nbits_signed(int(round(r1 * 65536))))
        bw.u(n, 5)
        bw.s(int(round(r0 * 65536)), n)
        bw.s(int(round(r1 * 65536)), n)
    n = max(nbits_signed(tx), nbits_signed(ty))
    bw.u(n, 5); bw.s(tx, n); bw.s(ty, n)
    return bw.tobytes()

# ---- 填充样式 ----
def solid_fill(rgb, a=255):
    return bytes([0x00]) + bytes(rgb) + bytes([a])

def linear_gradient_fill(xspan, yspan, c0, c1):
    m = matrix(sx=1.0 / xspan, sy=1.0 / yspan)
    b = bytearray([0x10]) + m
    b += bytes([0x02])  # spread=0 interp=0 num=2
    b += bytes([0]) + bytes(c0) + bytes([255])
    b += bytes([255]) + bytes(c1) + bytes([255])
    return bytes(b)

def bitmap_fill(bmp_id, twips_per_pixel=256):
    m = matrix(sx=1.0 / twips_per_pixel, sy=1.0 / twips_per_pixel)
    return bytes([0x40]) + struct.pack("<H", bmp_id) + m  # 0x40 = 平铺

# ---- DefineShape3（ver3, 颜色带 alpha） ----
def define_shape3(shape_id, bounds, fill_bytes, edges, move_to, with_line=False):
    body = bytearray()
    body += struct.pack("<H", shape_id)
    body += rect(*bounds)
    if fill_bytes:
        body += bytes([1]) + fill_bytes          # 1 填充样式
    else:
        body += bytes([0])
    if with_line:
        body += bytes([1]) + struct.pack("<H", 40) + bytes((0, 0, 0))  # 黑描边 40twips
    else:
        body += bytes([0])
    nfill = 1 if fill_bytes else 0
    nline = 1 if with_line else 0
    bw = BW()
    bw.u(nfill, 4); bw.u(nline, 4)
    shape_record_header(bw, line=1 if with_line else 0, fill1=1 if nfill else 0, move=1)
    mb = max(nbits_signed(move_to[0]), nbits_signed(move_to[1]))
    bw.u(mb, 5); bw.s(move_to[0], mb); bw.s(move_to[1], mb)
    if nfill: bw.u(1, nfill)
    if with_line: bw.u(1, nline)
    for e in edges:
        if e[0] == 'c':
            curve(bw, *e[1:], nfill, nline)
        else:
            straight(bw, e[1], e[2], nfill, nline)
    bw.u(0, 6)  # EndShapeRecord
    body += bw.tobytes()
    return body

# ---- DefineBitsLossless2（format 5 = 32bit RGBA） ----
def define_lossless2(char_id, w, h, rgba):
    raw = zlib.compress(bytes(rgba))
    body = struct.pack("<H", char_id) + bytes([5]) + struct.pack("<H", w) + struct.pack("<H", h) + raw
    return tag(36, body)

def place_object2(depth, char_id=None, mat=None, clip_depth=None, move=False):
    flags = 0
    if move: flags |= 0x01
    if char_id is not None: flags |= 0x02
    if mat is not None: flags |= 0x04
    if clip_depth is not None: flags |= 0x40
    b = bytearray([flags]) + struct.pack("<H", depth)
    if char_id is not None: b += struct.pack("<H", char_id)
    if mat is not None: b += mat
    if clip_depth is not None: b += struct.pack("<H", clip_depth)
    return b

# ================= 组装影片 =================
# 位图角色 id=10：64x64，左半青色、右半品红
BW_, BH_ = 64, 64
px = bytearray()
for j in range(BH_):
    for i in range(BW_):
        if i < BW_ // 2:
            px += bytes((0, 255, 255, 255))   # cyan
        else:
            px += bytes((255, 0, 255, 255))   # magenta
bmp_tag = define_lossless2(10, BW_, BH_, px)

# 形状 id=1：渐变矩形 (0,0)-(2000,1500)，线性 red->blue
grad = linear_gradient_fill(2000, 1500, (255, 0, 0), (0, 0, 255))
shape1 = define_shape3(1, (0, 0, 2000, 1500), grad, rect_edges(2000, 1500), (0, 0))

# 形状 id=2：裁剪圆（实心）半径 1000 中心 (2000,1500)
shape2 = define_shape3(2, (1000, 500, 3000, 2500), solid_fill((255, 255, 255)),
                       circle_edges(2000, 1500, 1000), (2000 + 1000, 1500))

# 形状 id=3：位图填充大矩形 (0,0)-(4000,3000)，被圆裁剪
shape3 = define_shape3(3, (0, 0, 4000, 3000), bitmap_fill(10), rect_edges(4000, 3000), (0, 0))

body = bytearray()
body += tag(9, bytes([0x33, 0x66, 0x99]))      # SetBackgroundColor
body += bmp_tag
body += tag(32, shape1)                         # DefineShape3 id=1
body += tag(32, shape2)                         # DefineShape3 id=2 (clip)
body += tag(32, shape3)                         # DefineShape3 id=3 (bitmap)
# 第 0 帧
body += tag(26, place_object2(1, char_id=1))               # 渐变矩形 depth1
body += tag(26, place_object2(5, char_id=2, clip_depth=6)) # 裁剪圆 depth5 clipDepth6
body += tag(26, place_object2(6, char_id=3))               # 位图矩形 depth6（被裁剪）
body += tag(1, b"")                                       # ShowFrame
# 第 1 帧：移动渐变矩形（动画）
body += tag(26, place_object2(1, char_id=1, mat=matrix(tx=1000, ty=500), move=True))
body += tag(1, b"")                                       # ShowFrame
body += tag(0, b"")                                       # End

version = 3
file_length = 8 + len(body)
header = b"FWS" + bytes([version]) + struct.pack("<I", file_length)
header += rect(0, 0, 4000, 3000)
header += struct.pack("<H", 0x0C00)   # 12 fps
header += struct.pack("<H", 2)         # frameCount=2

with open("testswf/full_test.swf", "wb") as f:
    f.write(header + body)
print("wrote testswf/full_test.swf, %d bytes" % (len(header) + len(body)))
