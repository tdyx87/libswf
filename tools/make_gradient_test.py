import struct, sys
sys.path.insert(0, '.')
from make_full_test_swf import (BW, rect, tag, matrix, define_shape3,
                                rect_edges, place_object2)

RED = (255, 0, 0); BLUE = (0, 0, 255)
W = H = 1000

def _grad(kind, c0, c1, m, focal=None):
    b = bytearray([kind]) + m
    b += bytes([0x02])
    b += bytes([0]) + bytes(c0) + bytes([255])
    b += bytes([255]) + bytes(c1) + bytes([255])
    if focal is not None:
        bw = BW()
        bw.s(int(round(focal[0] * 32768)), 15)
        bw.s(int(round(focal[1] * 32768)), 15)
        b += bw.tobytes()
    return bytes(b)

lin = _grad(0x10, RED, BLUE, matrix(sx=1.0/W, sy=1.0/H))
rad = _grad(0x12, RED, BLUE, matrix(sx=1.0/500, sy=1.0/500, tx=-1, ty=-1))
foc = _grad(0x13, RED, BLUE, matrix(sx=1.0/500, sy=1.0/500, tx=-1, ty=-1), focal=(0.35, 0.0))

body = bytearray()
body += tag(9, bytes([0x22, 0x22, 0x22]))
body += tag(32, define_shape3(1, (0, 0, W, H), lin, rect_edges(W, H), (0, 0)))
body += tag(32, define_shape3(2, (0, 0, W, H), rad, rect_edges(W, H), (0, 0)))
body += tag(32, define_shape3(3, (0, 0, W, H), foc, rect_edges(W, H), (0, 0)))
body += tag(26, place_object2(1, char_id=1, mat=matrix(tx=200,  ty=500)))
body += tag(26, place_object2(2, char_id=2, mat=matrix(tx=1500, ty=500)))
body += tag(26, place_object2(3, char_id=3, mat=matrix(tx=2800, ty=500)))
body += tag(1, b"")
body += tag(0, b"")

version = 3
file_length = 8 + len(body)
header = b"FWS" + bytes([version]) + struct.pack("<I", file_length)
header += rect(0, 0, 4000, 3000)
header += struct.pack("<H", 0x0C00)
header += struct.pack("<H", 1)
open("testswf/gradient_test.swf", "wb").write(header + body)
print("wrote testswf/gradient_test.swf")
