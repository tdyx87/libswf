#!/usr/bin/env python3
# 验证 full_test.swf 的渲染结果（渐变填充 / 位图裁剪 / 多帧动画）。
# 用法： py tools/check_full_png.py <frame_000.png> <frame_001.png>
import sys, zlib, struct

def load_png(path):
    d = open(path, 'rb').read()
    assert d[:8] == b'\x89PNG\r\n\x1a\n', "not png"
    pos = 8; width = height = colortype = None; idat = b''
    while pos < len(d):
        ln = struct.unpack('>I', d[pos:pos+4])[0]; typ = d[pos+4:pos+8]
        chunk = d[pos+8:pos+8+ln]
        if typ == b'IHDR': width, height, _, colortype = struct.unpack('>IIBB', chunk[:10])
        elif typ == b'IDAT': idat += chunk
        elif typ == b'IEND': break
        pos += 12 + ln
    raw = zlib.decompress(idat)
    ch = {0:1,2:3,3:1,4:2,6:4}[colortype]; stride = width*ch
    out = bytearray(width*height*ch); prev = bytearray(stride); p = 0
    for y in range(height):
        f = raw[p]; p += 1
        line = bytearray(raw[p:p+stride]); p += stride
        for i in range(stride):
            a = line[i-ch] if i >= ch else 0; b = prev[i]; c = prev[i-ch] if i >= ch else 0
            if f == 1: line[i] = (line[i]+a)&255
            elif f == 2: line[i] = (line[i]+b)&255
            elif f == 3: line[i] = (line[i]+(a+b)//2)&255
            elif f == 4:
                pp=a+b-c; pa=abs(pp-a); pb=abs(pp-b); pc=abs(pp-c)
                pr=a if (pa<=pb and pa<=pc) else (b if pb<=pc else c)
                line[i]=(line[i]+pr)&255
            out[y*stride+i]=line[i]
        prev=line
    return width, height, ch, out

BG = (51,102,153)
def px(img,x,y,ch): i=(y*img[0]+x)*ch; return (img[3][i],img[3][i+1],img[3][i+2],img[3][i+3] if ch==4 else 255)

fails=[]
def check(name, cond):
    print(("PASS" if cond else "FAIL"), name)
    if not cond: fails.append(name)

f0 = load_png(sys.argv[1])
f1 = load_png(sys.argv[2])
print(f"frame0 {f0[0]}x{f0[1]}  frame1 {f1[0]}x{f1[1]}")

# 渐变：左红右蓝
gl = px(f0,5,10,f0[2]); gr = px(f0,95,10,f0[2])
check("gradient left is red",  gl[0]>150 and gl[2]<120)
check("gradient right is blue", gr[2]>120 and gr[0]<120)

# 裁剪：圆内应出现位图色（青/品红），圆外且渐变矩形外的角落应为背景
# 圆形裁剪中心(100,75)半径50；渐变矩形覆盖(0,0)-(100,75)。
# 采样点(190,140)：在渐变矩形之外、也在圆之外 -> 位图被裁剪掉 -> 背景。
center = px(f0,100,75,f0[2])
corner = px(f0,190,140,f0[2])
check("clip center shows bitmap color (cyan/magenta)",
      (center[1]>200 and center[2]>200) or (center[0]>200 and center[2]>200))
check("clip corner is background (clipped away)",
      abs(corner[0]-BG[0])<10 and abs(corner[1]-BG[1])<10 and abs(corner[2]-BG[2])<10)

# 多帧动画：第0帧 (10,10) 在渐变矩形内（偏红），第1帧矩形平移后该点变背景
a0 = px(f0,10,10,f0[2]); a1 = px(f1,10,10,f1[2])
check("frame0 (10,10) inside gradient (reddish)", a0[0]>150 and a0[2]<120)
check("frame1 (10,10) moved out (background)", abs(a1[0]-BG[0])<10 and abs(a1[1]-BG[1])<10 and abs(a1[2]-BG[2])<10)

print("\n%d failures" % len(fails))
sys.exit(1 if fails else 0)
