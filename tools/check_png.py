#!/usr/bin/env python3
# 最小 PNG 解码（仅 RGBA/RGB、8bit），抽查渲染结果是否画出图形。
import sys, zlib, struct

def load_png(path):
    d = open(path, 'rb').read()
    assert d[:8] == b'\x89PNG\r\n\x1a\n', "not png"
    pos = 8
    width = height = bitdepth = colortype = None
    idat = b''
    while pos < len(d):
        ln = struct.unpack('>I', d[pos:pos+4])[0]
        typ = d[pos+4:pos+8]
        chunk = d[pos+8:pos+8+ln]
        if typ == b'IHDR':
            width, height, bitdepth, colortype = struct.unpack('>IIBB', chunk[:10])
        elif typ == b'IDAT':
            idat += chunk
        elif typ == b'IEND':
            break
        pos += 12 + ln
    raw = zlib.decompress(idat)
    ch = {0:1,2:3,3:1,4:2,6:4}[colortype]
    stride = width * ch
    out = bytearray(width * height * ch)
    prev = bytearray(stride)
    p = 0
    for y in range(height):
        f = raw[p]; p += 1
        line = bytearray(raw[p:p+stride]); p += stride
        for i in range(stride):
            a = line[i-ch] if i >= ch else 0
            b = prev[i]
            c = prev[i-ch] if i >= ch else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a+b)//2) & 255
            elif f == 4:
                pp = a + b - c
                pa = abs(pp-a); pb = abs(pp-b); pc = abs(pp-c)
                pr = a if (pa<=pb and pa<=pc) else (b if pb<=pc else c)
                line[i] = (line[i] + pr) & 255
            out[y*stride+i] = line[i]
        prev = line
    return width, height, ch, out

if __name__ == '__main__':
    w, h, ch, px = load_png(sys.argv[1])
    from collections import Counter
    cnt = Counter()
    for i in range(0, len(px), ch):
        cnt[(px[i], px[i+1], px[i+2])] += 1
    print(f"image {w}x{h} channels={ch}")
    print("top colors:")
    for col, n in cnt.most_common(6):
        print(f"  rgb{col} x{n}")
    # 检测非背景像素
    bg = (51,102,153)
    nonbg = sum(n for col, n in cnt.items() if col != bg)
    print(f"non-background pixels: {nonbg}")
    print("has red(rect):", any(c[0] > 180 and c[1] < 80 and c[2] < 80 for c in cnt))
    print("has blue(circle):", any(c[0] < 80 and c[2] > 180 for c in cnt))
