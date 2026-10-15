#!/usr/bin/env python3
"""生成用于验证 swfparser 的样本 SWF（未压缩 FWS 与 zlib 压缩 CWS）。

覆盖点：
  * RECT 位打包（NBits=UB[5] + 4×SB[NBits]）
  * 标签头短格式（length < 63）与长格式（length == 63 -> UI32）
  * SetBackgroundColor(9) / ShowFrame(1) / End(0) / DefineBinaryData(87)
"""
import os
import struct
import zlib


def bits_of(value: int, n: int):
    return [(value >> (n - 1 - i)) & 1 for i in range(n)]


def signed_bits_needed(v: int) -> int:
    n = 1
    while True:
        lo = -(1 << (n - 1))
        hi = (1 << (n - 1)) - 1
        if lo <= v <= hi:
            return n
        n += 1


def write_rect(x_min, x_max, y_min, y_max) -> bytes:
    vals = [x_min, x_max, y_min, y_max]
    nbits = max(signed_bits_needed(v) for v in vals)
    bits = bits_of(nbits, 5)
    for v in vals:
        bits += bits_of(v & ((1 << nbits) - 1), nbits)
    while len(bits) % 8:
        bits.append(0)
    out = bytearray()
    for i in range(0, len(bits), 8):
        b = 0
        for bit in bits[i:i + 8]:
            b = (b << 1) | bit
        out.append(b)
    return bytes(out)


def tag(code: int, payload: bytes) -> bytes:
    """标签头：UI16 = (code << 6) | length；length >= 63 时用长格式 UI32"""
    n = len(payload)
    if n < 63:
        return struct.pack("<H", (code << 6) | n) + payload
    return struct.pack("<H", (code << 6) | 63) + struct.pack("<I", n) + payload


def build_body() -> bytes:
    body = bytearray()
    body += write_rect(0, 11000, 0, 8000)     # 550x400 twips
    body += struct.pack("<H", 24 << 8)        # frameRate 24.0 (8.8)
    body += struct.pack("<H", 1)              # frameCount

    body += tag(9, bytes([0x33, 0x66, 0x99]))          # SetBackgroundColor
    body += tag(87, bytes(range(256))[:100])           # DefineBinaryData -> 长格式
    body += tag(1, b"")                                # ShowFrame
    body += tag(0, b"")                                # End
    return bytes(body)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out_dir = os.path.join(os.path.dirname(here), "testswf")
    os.makedirs(out_dir, exist_ok=True)

    body = build_body()
    file_len = 8 + len(body)

    fws = b"FWS" + bytes([13]) + struct.pack("<I", file_len) + body
    cws = b"CWS" + bytes([13]) + struct.pack("<I", file_len) + zlib.compress(body, 9)

    with open(os.path.join(out_dir, "sample_fws.swf"), "wb") as f:
        f.write(fws)
    with open(os.path.join(out_dir, "sample_cws.swf"), "wb") as f:
        f.write(cws)

    print("wrote sample_fws.swf (%d bytes) and sample_cws.swf (%d bytes) to %s"
          % (len(fws), len(cws), out_dir))


if __name__ == "__main__":
    main()
