#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成含视频流的测试 SWF（tag 60 DefineVideoStream + tag 61 VideoFrame）。

为什么用 FLV 作为码流来源：
    FLV 的 video codecId 与 SWF DefineVideoStream 的 codecId **完全一致**：
        2 = Sorenson H.263 (flv1)   4 = VP6   5 = VP6+alpha   7 = AVC
    而且 FLV 的 AVC sequence header 本身就是 AVCDecoderConfigurationRecord，
    正好对应 SWF 里「第一帧 VideoFrame 存extradata」的约定。
    所以直接把 FLV 的 video tag 逐帧搬进 SWF 即可，无需任何转码。

用法：
    python tools/make_video_test_swf.py
    # 需要 ffmpeg 可执行文件来产码流；默认取third_party 下的预编译包，
    # 也可用环境变量 FFMPEG_BIN 覆盖。
    # VP6 无法生成：FFmpeg 只有 VP6 解码器，没有编码器。
输出：
    testsw/video/video_flv1.swf     codecId=2  (H.263)
    testsw/video/video_avc.swf      codecId=7  (AVC / H.264)
"""
import os
import struct
import subprocess
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# ---- 定位 ffmpeg（只用于产码流；解析/播放由 swfparser 侧的FFmpeg 库完成）----
def find_ffmpeg():
    env = os.environ.get("FFMPEG_BIN")
    if env and os.path.isfile(env):
        return env
    cand = os.path.join(
        ROOT, "third_party",
        "ffmpeg-n8.1-latest-win64-gpl-shared-8.1", "bin", "ffmpeg.exe")
    if os.path.isfile(cand):
        return cand
    cand = os.path.join(
        ROOT, "third_party",
        "ffmpeg-n8.1-latest-win64-gpl-shared-8.1", "bin", "ffmpeg")
    if os.path.isfile(cand):
        return cand
    return "ffmpeg"          # 退化为 PATH 查找


FFMPEG = find_ffmpeg()
OUTDIR = os.path.join(ROOT, "..", "testsw", "video")
W, H, FPS, DURATION = 128, 96, 10, 1.0

# SWF codecId（与 FLV 相同）
CODEC_FLV1, CODEC_VP6, CODEC_VP6A, CODEC_AVC = 2, 4, 5, 7
CODEC_NAMES = {2: "H.263(Sorenson)", 4: "VP6", 5: "VP6+alpha", 7: "AVC/H.264"}


def encode(src: str, out_flv: str, encoder_args: list):
    """用 ffmpeg 生成 FLV（视频仅 1 轨，无音频）。"""
    cmd = [FFMPEG, "-hide_banner", "-loglevel", "error", "-y",
           "-f", "lavfi", "-i", src, "-an"] + encoder_args + ["-f", "flv", out_flv]
    subprocess.run(cmd, check=True)
    print("  ffmpeg -> %s (%d bytes)" % (os.path.basename(out_flv),
                                          os.path.getsize(out_flv)))


def parse_flv_video(path: str):
    """解析 FLV，取出所有 video tag -> (codecId, payload)。

    返回的 payload 语义与 SWF tag 61 的载荷一致：
      - codecId 7：第 0 个 payload 是 AVCDecoderConfigurationRecord，
                   其余是长度前缀（4 字节）NALU 组。
      - 其他：直接是压缩帧数据。
    """
    data = open(path, "rb").read()
    if data[:3] != b"FLV":
        raise ValueError("not an FLV file")
    # FLV header = 'FLV' + version + flags + DataOffset(UI32)；DataOffset 指向
    #第一个 PreviousTagSize0（UI32），跳过它才是第一个 tag 的 RECORDHEADER。
    data_offset = struct.unpack_from(">I", data, 5)[0]
    pos = data_offset + 4
    frames = []
    while pos + 15 <= len(data):
        tag_type = data[pos] & 0x1F
        size = struct.unpack_from(">I", b"\x00" + data[pos + 1:pos + 4])[0]
        payload = data[pos + 11: pos + 11 + size]
        pos += 11 + size + 4          # +PreviousTagSize
        if tag_type != 9:             # 只取 video tag
            continue
        if not payload:
            continue
        codec_id = payload[0] & 0x0F
        # 对 AVC：payload[1]=AVCPacketType(0=seq header,1=NALU,2=end)，
        # payload[2:5]=CompositionTime，需要剥掉这 5 字节头。
        if codec_id == CODEC_AVC:
            avc_packet_type = payload[1]
            if avc_packet_type == 2:      # end of sequence，无数据
                continue
            body = payload[5:]
        else:
            body = payload[1:]
        frames.append((codec_id, body))
    return frames


def swf_rect(xmin, ymin, xmax, ymax):
    """SWF RECT。

    位布局与本项目解析器（BitReader::readRect / readSB）严格一致：
        NBits = UB[5]，随后 4 个 **SB[NBits]**：Xmin、Xmax、Ymin、Ymax
    也就是每个字段用 NBits 位、其自身最高位作为符号，**没有**额外的符号位。
    （规范文档里描述的“1 位符号 + NBits 数据”与此不同，务必与解析器保持一致，
      否则整个 SWF 头会错位。tools/make_sample_swf.py::write_rect 也是这个约定。）

    NBits 是四个字段**统一**的位宽，取能以补码容纳全部取值的最小值，下限 5。
    """
    vals = [xmin, xmax, ymin, ymax]
    nbits = 5
    while not all(-(1 << (nbits - 1)) <= v <= (1 << (nbits - 1)) - 1 for v in vals):
        nbits += 1
    bits = bin(nbits)[2:].zfill(5)
    for v in vals:
        bits += bin(v & ((1 << nbits) - 1))[2:].zfill(nbits)
    n = (len(bits) + 7) // 8
    return int(bits.ljust(n * 8, "0"), 2).to_bytes(n, "big")


def tag(code: int, payload: bytes) -> bytes:
    """SWF RECORDHEADER：短格式（<63 字节）或长格式。"""
    n = len(payload)
    if n < 63:
        return struct.pack("<H", (code << 6) | n) + payload
    return struct.pack("<H", (code << 6) | 63) + struct.pack("<I", n) + payload


def place_object2(char_id: int, depth: int) -> bytes:
    """PlaceObject2（tag 26）最小形式：只放角色、不带矩阵/变换。

    字段顺序必须与本项目解析器（Decoder.cpp 的 case 26）一致：
        UI8  flags   0x02=HasCharacter, 0x04=HasMatrix, 0x08=HasCxform,
                      0x10=HasRatio, 0x20=HasName, 0x40=HasClipDepth, 0x80=HasClipActions
        UI16 Depth
        UI16 CharacterID      （仅当 HasCharacter）
    注意：这里 flags 只有 1 个字节且在 depth **之前**；写反了角色不会被摆上舞台。
    """
    p = bytes([0x02]) + struct.pack("<HH", depth, char_id)
    return tag(26, p)


def build_swf(frames, codec_id, char_id=1, width=W, height=H):
    """组装含视频流的 FWS(未压缩) SWF。"""
    nframes = len(frames)
    body = swf_rect(0, 0, width * 20, height * 20)     # 单位 twips
    body += struct.pack("<H", int(FPS * 256))         # 8.8 定点帧率
    body += struct.pack("<H", nframes)                # frameCount
    body += tag(9, bytes([0x00, 0x00, 0x00]))         # SetBackgroundColor 黑底

    # DefineVideoStream（tag 60）
    #   UI16 CharacterID, UI16 NumFrames, UI16 Width, UI16 Height, UI8 VideoFlags
    #   VideoFlags 低 4 位= codecId，第 4 位= SmoothingFlag
    vf = (codec_id & 0x0F) | 0x10                     # 开Smoothing 让画面不抖
    body += tag(60, struct.pack("<HHHHB", char_id, nframes, width, height, vf))

    # 每个 VideoFrame（tag 61）= UI16 StreamID + UI16 FrameNum + 压缩数据
    for i, (_cid, body_data) in enumerate(frames):
        body += tag(61, struct.pack("<HH", char_id, i) + body_data)
        body += place_object2(char_id, 0)
        body += tag(1, b"")                           # ShowFrame

    body += tag(0, b"")                               # End
    file_len = 8 + len(body)
    return b"FWS" + bytes([6]) + struct.pack("<I", file_len) + body


def main():
    os.makedirs(OUTDIR, exist_ok=True)
    src = "testsrc=size=%dx%d:rate=%d:duration=%s" % (W, H, FPS, DURATION)

    jobs = [
        ("video_flv1.swf", "flv1.flv", CODEC_FLV1, ["-c:v", "flv", "-b:v", "200k"]),
        ("video_avc.swf", "avc.flv", CODEC_AVC, ["-c:v", "libx264", "-profile:v", "baseline",
                                                "-pix_fmt", "yuv420p", "-b:v", "200k"]),
    ]

    for swf_name, flv_name, expect_cid, enc_args in jobs:
        print("生成 %s ..." % swf_name)
        tmp = os.path.join(OUTDIR, flv_name)
        encode(src, tmp, enc_args)
        frames = parse_flv_video(tmp)
        assert frames, "FLV 里没有 video tag"
        cid = frames[0][0]
        assert cid == expect_cid, "codecId 期望 %d，实际 %d" % (expect_cid, cid)
        swf = build_swf(frames, cid)
        out = os.path.join(OUTDIR, swf_name)
        open(out, "wb").write(swf)
        print("  codecId=%d (%s) 帧数=%d  -> %s (%d bytes)"
              % (cid, CODEC_NAMES.get(cid, "?"), len(frames), out, len(swf)))


if __name__ == "__main__":
    main()