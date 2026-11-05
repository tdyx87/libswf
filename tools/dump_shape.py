#!/usr/bin/env python3
"""独立解析 SWF 的 DefineShape(2/3/4)，用于与 C++ 解析器对照。

用法: python tools/dump_shape.py <file.swf> <shape_id>

只打印 ASCII，避免控制台编码问题。
"""
import os
import struct
import sys
import zlib

# 实验开关：StateNew 追加样式后，填充/线索引的位宽是否随之增长。
#   KEEP_BITS=1 -> 保持 SHAPEWITHSTYLE 头里的位宽不变
#   KEEP_BITS=0 -> 按 needed_bits(样式总数) 重新计算
KEEP_BITS = os.environ.get('KEEP_BITS', '0') == '1'


class Bits:
    def __init__(self, data):
        self.d = data
        self.pos = 0          # bit position

    def ub(self, n):
        v = 0
        for _ in range(n):
            byte = self.pos >> 3
            bit = 7 - (self.pos & 7)
            v = (v << 1) | ((self.d[byte] >> bit) & 1)
            self.pos += 1
        return v

    def sb(self, n):
        if n == 0:
            return 0
        v = self.ub(n)
        if v & (1 << (n - 1)):
            v -= (1 << n)
        return v

    def align(self):
        self.pos = (self.pos + 7) & ~7

    def byte_pos(self):
        return (self.pos + 7) >> 3


class Reader:
    """字节流读取器：位读与字节读可交替（与 SWF 规范一致）。"""

    def __init__(self, data, pos=0):
        self.d = data
        self.bitpos = pos * 8

    def ub(self, n):
        v = 0
        for _ in range(n):
            byte = self.bitpos >> 3
            bit = 7 - (self.bitpos & 7)
            v = (v << 1) | ((self.d[byte] >> bit) & 1)
            self.bitpos += 1
        return v

    def sb(self, n):
        if n == 0:
            return 0
        v = self.ub(n)
        if v & (1 << (n - 1)):
            v -= (1 << n)
        return v

    def align(self):
        self.bitpos = (self.bitpos + 7) & ~7

    def byte_align(self):
        # 丢弃当前字节剩余位，前进到下一字节（Ruffle byte_align 语义）
        if self.bitpos % 8:
            self.bitpos = (self.bitpos // 8 + 1) * 8

    def u8(self):
        self.align()
        p = self.bitpos >> 3
        self.bitpos += 8
        return self.d[p]

    def u16(self):
        self.align()
        p = self.bitpos >> 3
        v = struct.unpack_from('<H', self.d, p)[0]
        self.bitpos += 16
        return v

    def u32(self):
        self.align()
        p = self.bitpos >> 3
        v = struct.unpack_from('<I', self.d, p)[0]
        self.bitpos += 32
        return v

    def rect(self):
        nbits = self.ub(5)
        vals = [self.sb(nbits) for _ in range(4)]
        self.align()
        return vals


def read_matrix(r):
    sx = sy = 1.0
    r0 = r1 = 0.0
    tx = ty = 0.0
    has_scale = r.ub(1)
    if has_scale:
        nbits = r.ub(5)
        sx = r.sb(nbits) / 65536.0
        sy = r.sb(nbits) / 65536.0
    has_rot = r.ub(1)
    if has_rot:
        nbits = r.ub(5)
        r0 = r.sb(nbits) / 65536.0
        r1 = r.sb(nbits) / 65536.0
    nbits = r.ub(5)
    tx = r.sb(nbits) / 20.0
    ty = r.sb(nbits) / 20.0
    return (sx, sy, r0, r1, tx, ty)


def read_rgba(r, alpha):
    r.align()                       # 样式记录内部是字节结构
    p = r.bitpos >> 3
    if alpha:
        v = tuple(r.d[p:p + 4])
        r.bitpos += 32
    else:
        v = tuple(r.d[p:p + 3]) + (255,)
        r.bitpos += 24
    return v


def read_fill(r, alpha, ver):
    r.align()
    t = r.ub(8)
    if t == 0x00:
        return ('solid', read_rgba(r, alpha))
    if t in (0x10, 0x12, 0x13):
        read_matrix(r)
        hdr = r.ub(8)
        n = hdr & 0x0F
        stops = []
        for _ in range(n):
            ratio = r.ub(8)
            stops.append((ratio, read_rgba(r, alpha)))
        if t == 0x13:
            r.ub(15)
            r.ub(15)
        return ('grad%d' % t, n, stops[:2])
    if t in (0x40, 0x41, 0x43, 0x44):
        bid = r.ub(16)
        read_matrix(r)
        return ('bitmap', bid)
    return ('unknown', hex(t))


def read_line(r, alpha, ver):
    w = r.ub(16)
    if ver < 4:
        # DefineShape1/2/3 的线样式都是简单形式：Width UI16 + Color(RGB/RGBA)
        # （LINESTYLE2 位打包形式是 DefineShape4 才引入的）
        col = read_rgba(r, alpha)
        return (w, col)
    r.ub(2)
    join = r.ub(2)
    has_fill = r.ub(1)
    r.ub(1)
    r.ub(1)
    r.ub(1)
    r.ub(5)
    r.ub(1)
    r.ub(2)
    if join == 2:
        r.ub(8)
    if ver >= 4:
        has_blur = r.ub(1)
        if has_blur:
            r.ub(16)
            r.ub(16)
            r.ub(8)
    r.align()
    if has_fill:
        read_fill(r, alpha, ver)
    else:
        read_rgba(r, alpha)
    return w


def needed_bits(n):
    if n == 0:
        return 0
    b = 1
    while n > 1:
        n >>= 1
        b += 1
    return b


def dump_shape(data, ver):
    total_len = len(data)
    r = Reader(data)
    bounds = r.rect()
    edge_bounds = None
    winding = 0
    if ver >= 4:
        edge_bounds = r.rect()
        r.ub(5)
        winding = r.ub(1)
        r.ub(1)
        r.ub(1)
        r.align()

    alpha = ver >= 3
    if os.environ.get('FORCE_ALPHA') == '1':
        alpha = True
    elif os.environ.get('FORCE_ALPHA') == '0':
        alpha = False
    fc = r.u8()
    if fc == 0xFF:
        fc = r.u16()
    fills = [read_fill(r, alpha, ver) for _ in range(fc)]
    lc = r.u8()
    if lc == 0xFF:
        lc = r.u16()
    lines = [read_line(r, alpha, ver) for _ in range(lc)]

    num_fill_bits = r.ub(4)
    num_line_bits = r.ub(4)

    print("bounds=%s edge=%s fills=%d lines=%d fillBits=%d lineBits=%d winding=%d"
          % (bounds, edge_bounds, len(fills), len(lines), num_fill_bits, num_line_bits, winding))
    print("fill styles:", fills[:6])
    print("line styles (width):", lines[:8])

    # 边记录
    paths = {}
    cur = {}
    act0 = act1 = actline = 0
    px = py = 0
    recs = 0
    style_changes = 0
    edges = 0
    ex0 = ey0 = 1 << 30
    ex1 = ey1 = -(1 << 30)   # 实际几何范围（含曲线控制点）
    while True:
        recs += 1
        if recs > 200000:
            print("guard hit")
            break
        type_flag = r.ub(1)
        if type_flag == 0:
            st_new = r.ub(1)
            st_line = r.ub(1)
            st_f1 = r.ub(1)
            st_f0 = r.ub(1)
            st_move = r.ub(1)
            if not (st_new or st_line or st_f1 or st_f0 or st_move):
                break
            if st_move:
                mb = r.ub(5)
                px += r.sb(mb)
                py += r.sb(mb)
            if st_f0:
                act0 = r.ub(num_fill_bits) if num_fill_bits else 0
            if st_f1:
                act1 = r.ub(num_fill_bits) if num_fill_bits else 0
            if st_line:
                actline = r.ub(num_line_bits) if num_line_bits else 0
            if st_new or st_f0 or st_f1 or st_line:
                print("  rec%d@%d new=%d line=%d f1=%d f0=%d move=%d act=(%d,%d,%d) bits=(%d,%d)"
                      % (recs, r.bitpos >> 3, st_new, st_line, st_f1, st_f0, st_move,
                         act0, act1, actline, num_fill_bits, num_line_bits))
            if st_new:
                style_changes += 1
                nfc = r.u8()
                if nfc == 0xFF:
                    nfc = r.u16()
                start_pos = r.bitpos >> 3
                newf = [read_fill(r, alpha, ver) for _ in range(nfc)]
                print("    new fills @%d..%d types=%s"
                      % (start_pos, r.bitpos >> 3, [f[0] for f in newf[:8]]))
                fills.extend(newf)
                nlc = r.u8()
                if nlc == 0xFF:
                    nlc = r.u16()
                print("    after fills @%d: nlc=%d" % (r.bitpos >> 3, nlc))
                newl = [read_line(r, alpha, ver) for _ in range(nlc)]
                lines.extend(newl)
                # 实验：SWF 的 NumFillBits/NumLineBits 是 SHAPEWITHSTYLE 头里的固定值，
                # StateNew 只是追加样式，**不改变索引位宽**。
                nb = r.u8()
                num_fill_bits = nb >> 4
                num_line_bits = nb & 0x0F
                print("  record %d: StateNew +%d fills (total %d) -> fillBits=%d"
                      % (recs, nfc, len(fills), num_fill_bits))
        else:
            straight = r.ub(1)
            nb = r.ub(4) + 2
            if straight:
                general = r.ub(1)
                if general:
                    dx = r.sb(nb)
                    dy = r.sb(nb)
                else:
                    if r.ub(1):
                        dx = r.sb(nb)
                        dy = 0
                    else:
                        dx = 0
                        dy = r.sb(nb)
                px += dx
                py += dy
            else:
                cx = px + r.sb(nb)
                cy = py + r.sb(nb)
                px = cx + r.sb(nb)
                py = cy + r.sb(nb)
                ex0 = min(ex0, cx); ex1 = max(ex1, cx)
                ey0 = min(ey0, cy); ey1 = max(ey1, cy)
            ex0 = min(ex0, px); ex1 = max(ex1, px)
            ey0 = min(ey0, py); ey1 = max(ey1, py)
            if os.environ.get('EDGE_DUMP') == '1':
                print("  edge rec%d pos=(%d,%d) straight=%d act0=%d act1=%d line=%d"
                      % (recs, px, py, straight, act0, act1, actline))
            edges += 1
            if act0:
                paths.setdefault(act0, 0)
                paths[act0] += 1
            if act1:
                paths.setdefault(-act1, 0)
                paths[-act1] += 1

    print("records=%d edges=%d styleNewRecords=%d finalFills=%d endPos=%d/%d%s"
          % (recs, edges, style_changes, len(fills), r.bitpos >> 3, total_len,
             "  <== CLEAN" if abs((r.bitpos >> 3) - total_len) <= 1 else "  <== DESYNC"))
    print("actual geometry extent(twips): x[%d..%d] y[%d..%d]"
          % (ex0, ex1, ey0, ey1))
    print("declared bounds: %s" % (bounds,))
    if ex0 < bounds[0] or ex1 > bounds[1] or ey0 < bounds[2] or ey1 > bounds[3]:
        print("  <== GEOMETRY EXCEEDS DECLARED BOUNDS")
    used = sorted(set(abs(k) for k in paths))
    print("referenced fill styles: %d  -> %s" % (len(used), used[:20]))
    print("total fill styles: %d" % len(fills))


def main():
    if len(sys.argv) < 3:
        print("usage: dump_shape.py <file.swf> <shape_id>")
        return 1
    path = sys.argv[1]
    want = int(sys.argv[2])
    data = open(path, 'rb').read()
    if data[:3] == b'CWS':
        body = zlib.decompress(data[8:])
    else:
        body = data[8:]
    r = Reader(body)
    r.rect()
    r.align()
    fps = struct.unpack_from('<H', body, r.bitpos >> 3)[0]
    r.bitpos += 16
    frames = struct.unpack_from('<H', body, r.bitpos >> 3)[0]
    r.bitpos += 16
    print("file: fps=%.1f frames=%d" % (fps / 256.0, frames))

    pos = r.bitpos >> 3
    while pos + 2 <= len(body):
        code_len = struct.unpack_from('<H', body, pos)[0]
        pos += 2
        code = code_len >> 6
        length = code_len & 0x3F
        if length == 63:
            length = struct.unpack_from('<I', body, pos)[0]
            pos += 4
        if code == 0:
            break
        if code in (2, 22, 32, 83):
            sid = struct.unpack_from('<H', body, pos)[0]
            if sid == want:
                ver = {2: 1, 22: 2, 32: 3, 83: 4}[code]
                print("found shape id=%d tag=%d ver=%d len=%d" % (sid, code, ver, length))
                try:
                    dump_shape(body[pos + 2:pos + length], ver)
                except Exception as exc:      # 越界即说明位流解读错位
                    print("PARSE ERROR: %s  <== DESYNC" % exc)
                return 0
        pos += length
    print("shape %d not found" % want)
    return 1


if __name__ == '__main__':
    sys.exit(main())
