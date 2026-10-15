#!/usr/bin/env python3
# 生成一个用于验证软件光栅器的测试 SWF（未压缩 FWS）。
# 内容：背景色 + 一个红色填充矩形（直线边）+ 一个蓝色填充圆形（曲线边）+ ShowFrame。
# 纯手工拼装字节，不依赖任何外部库。
import struct

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
    bw.u(0, 1)            # TypeFlag = StyleChange
    bw.u(new, 1)
    bw.u(line, 1)
    bw.u(fill1, 1)
    bw.u(fill0, 1)
    bw.u(move, 1)

def curve(bw, dxc, dyc, dxa, dya, nfill, nline):
    # returns nothing; writes a curved edge record
    nb = 2
    for v in (dxc, dyc, dxa, dya):
        while (1 << (nb - 1)) <= abs(v):
            nb += 1
    field = nb - 2
    bw.u(1, 1)    # TypeFlag = edge
    bw.u(0, 1)    # StraightFlag = 0 (curve)
    bw.u(field, 4)
    bw.s(dxc, nb); bw.s(dyc, nb); bw.s(dxa, nb); bw.s(dya, nb)

def straight(bw, dx, dy, nfill, nline):
    nb = 2
    for v in (dx, dy):
        while (1 << (nb - 1)) <= abs(v):
            nb += 1
    general = 1 if (dx != 0 and dy != 0) else 0
    field = nb - 2
    bw.u(1, 1)    # edge
    bw.u(1, 1)    # straight
    bw.u(field, 4)
    bw.u(general, 1)   # GeneralLineFlag（SWF 规范要求）
    if general:
        bw.s(dx, nb); bw.s(dy, nb)
    else:
        vert = 1 if dy != 0 else 0
        bw.u(vert, 1)
        if vert:
            bw.s(dy, nb)
        else:
            bw.s(dx, nb)

def define_shape(shape_id, bounds, fill_rgb, outline_rgb, edges, move_to):
    body = bytearray()
    body += struct.pack("<H", shape_id)
    body += rect(*bounds)
    # FillStyles
    body += bytes([1])                       # count = 1
    body += bytes([0x00]) + bytes(fill_rgb)  # solid color (RGB)
    # LineStyles
    if outline_rgb:
        body += bytes([1])                   # count = 1
        body += struct.pack("<H", 40)        # width 40 twips
        body += bytes(outline_rgb)
    else:
        body += bytes([0])                   # count = 0
    # shape records
    bw = BW()
    nfill = 1 if fill_rgb else 0
    nline = 1 if outline_rgb else 0
    bw.u(nfill, 4)          # NumFillBits
    bw.u(nline, 4)          # NumLineBits
    # initial style change: move + set fill1 + line
    shape_record_header(bw, new=0, line=1 if outline_rgb else 0,
                        fill1=1 if fill_rgb else 0, fill0=0, move=1)
    mb = max(nbits_signed(move_to[0]), nbits_signed(move_to[1]))
    bw.u(mb, 5)
    bw.s(move_to[0], mb); bw.s(move_to[1], mb)
    if fill_rgb:
        bw.u(1, nfill)
    if outline_rgb:
        bw.u(1, nline)
    for e in edges:
        if e[0] == 'c':
            curve(bw, e[1], e[2], e[3], e[4], nfill, nline)
        else:
            straight(bw, e[1], e[2], nfill, nline)
    # EndShapeRecord
    bw.u(0, 6)
    body += bw.tobytes()
    return body

def tag(code, data):
    length = len(data)
    if length < 63:
        hdr = struct.pack("<H", (code << 6) | length)
    else:
        hdr = struct.pack("<H", (code << 6) | 63) + struct.pack("<I", length)
    return hdr + data

# ---- 矩形（直线边，红色填充 + 黑描边）----
rect_edges = [
    ('s', 2000, 0),
    ('s', 0, 1500),
    ('s', -2000, 0),
    ('s', 0, -1500),
]
rect_body = define_shape(1, (0, 0, 2000, 1500), (0xFF, 0x00, 0x00), (0, 0, 0), rect_edges, (0, 0))

# ---- 圆形（4 段二次贝塞尔曲线，蓝色填充，无描边）----
cx, cy, R = 2500, 2000, 1000
k = int(0.5523 * R)
P = [(cx + R, cy), (cx, cy + R), (cx - R, cy), (cx, cy - R)]
C = [(cx + R, cy + k), (cx + k, cy + R), (cx - R, cy - k), (cx - k, cy - R)]
circ_edges = []
for i in range(4):
    nx = (i + 1) % 4
    dxc, dyc = C[i][0] - P[i][0], C[i][1] - P[i][1]
    dxa, dya = P[nx][0] - P[i][0], P[nx][1] - P[i][1]
    circ_edges.append(('c', dxc, dyc, dxa, dya))
circ_body = define_shape(2, (cx - R, cy - R, cx + R, cy + R), (0x00, 0x40, 0xFF), None, circ_edges, P[0])

# ---- 组装影片 ----
body = bytearray()
body += tag(9, bytes([0x33, 0x66, 0x99]))          # SetBackgroundColor
body += tag(22, rect_body)                          # DefineShape2 (id=1)
body += tag(22, circ_body)                          # DefineShape2 (id=2)
body += tag(26, struct.pack("<B", 0x02) + struct.pack("<H", 1) + struct.pack("<H", 1))  # PlaceObject2 depth1 char1
body += tag(26, struct.pack("<B", 0x02) + struct.pack("<H", 2) + struct.pack("<H", 2))  # PlaceObject2 depth2 char2
body += tag(1, b"")                                 # ShowFrame
body += tag(0, b"")                                 # End

# ---- 文件头 ----
version = 2
file_length = 8 + len(body)
header = b"FWS" + bytes([version]) + struct.pack("<I", file_length)
header += rect(0, 0, 4000, 3000)
header += struct.pack("<H", 0x0C00)   # 12 fps
header += struct.pack("<H", 1)         # frameCount

with open("testswf/shape_test.swf", "wb") as f:
    f.write(header + body)
print("wrote testswf/shape_test.swf, %d bytes" % (len(header) + len(body)))
