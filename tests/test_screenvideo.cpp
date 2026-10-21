// ScreenVideo(codec 3) 解码测试（CTest: swfparser_screenvideo）。
// 构造 DefineVideoStream + 一个 ScreenVideo VideoFrame（zlib 块 + 16x16 瓦片位流），
// 经 decodeMovie 解码后校验 RGBA 像素。
#include "swf/Decoder.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <zlib.h>

static int g_fail = 0;
static void check(bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) ++g_fail;
}

struct BitWriter {
    std::vector<std::uint8_t> bytes;
    int bitBuf = 0, bitCnt = 0;
    void writeUB(std::uint32_t v, int n) {
        for (int i = n - 1; i >= 0; --i) {
            bitBuf = (bitBuf << 1) | static_cast<int>((v >> i) & 1u);
            if (++bitCnt == 8) { bytes.push_back(static_cast<std::uint8_t>(bitBuf)); bitBuf = 0; bitCnt = 0; }
        }
    }
    void align() { if (bitCnt > 0) { bitBuf <<= (8 - bitCnt); bytes.push_back(static_cast<std::uint8_t>(bitBuf)); bitBuf = 0; bitCnt = 0; } }
    void u16(std::uint16_t v) { align(); bytes.push_back(v & 0xFF); bytes.push_back((v >> 8) & 0xFF); }
    void rect(int x0, int y0, int x1, int y1) {
        int m = std::max(std::max(std::abs(x0), std::abs(x1)), std::max(std::abs(y0), std::abs(y1)));
        int nbits = 1; while ((1 << (nbits - 1)) - 1 < m && nbits < 30) ++nbits;
        writeUB(static_cast<std::uint32_t>(nbits), 5);
        writeUB(static_cast<std::uint32_t>(x0), nbits); writeUB(static_cast<std::uint32_t>(x1), nbits);
        writeUB(static_cast<std::uint32_t>(y0), nbits); writeUB(static_cast<std::uint32_t>(y1), nbits);
        align();
    }
};
static void writeTag(std::vector<std::uint8_t>& out, int code, const std::vector<std::uint8_t>& data) {
    std::size_t len = data.size();
    std::uint16_t h = static_cast<std::uint16_t>((code << 6) | (len < 63 ? (len & 0x3F) : 0x3F));
    out.push_back(h & 0xFF); out.push_back((h >> 8) & 0xFF);
    if (len >= 63) for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>((len >> (8 * i)) & 0xFF));
    for (auto b : data) out.push_back(b);
}

int main() {
    const int W = 32, H = 32;
    // 解压块：blockWidth, blockHeight + 每像素(1位flag=1 + RGB)
    BitWriter bw;
    bw.u16((std::uint16_t)W); bw.u16((std::uint16_t)H);
    for (int i = 0; i < W * H; ++i) { bw.writeUB(1, 1); bw.writeUB(0, 8); bw.writeUB(255, 8); bw.writeUB(0, 8); }  // 绿
    bw.align();
    // zlib 压缩该块
    std::vector<std::uint8_t> comp(compressBound(bw.bytes.size()));
    uLongf clen = static_cast<uLongf>(comp.size());
    compress2(comp.data(), &clen, bw.bytes.data(), bw.bytes.size(), Z_DEFAULT_COMPRESSION);
    comp.resize(clen);
    // ScreenVideo 帧 = blockSize(UI16) + 压缩块
    std::vector<std::uint8_t> sv;
    sv.push_back(static_cast<std::uint8_t>(clen & 0xFF)); sv.push_back(static_cast<std::uint8_t>((clen >> 8) & 0xFF));
    for (auto b : comp) sv.push_back(b);

    // DefineVideoStream(60): id=1, numFrames=1, WxH, flags(codec=3)
    std::vector<std::uint8_t> dvs;
    dvs.push_back(1); dvs.push_back(0);       // id=1
    dvs.push_back(1); dvs.push_back(0);       // numFrames=1
    dvs.push_back(W & 0xFF); dvs.push_back((W >> 8) & 0xFF);
    dvs.push_back(H & 0xFF); dvs.push_back((H >> 8) & 0xFF);
    dvs.push_back(0x03);                       // flags: codecId=3 (ScreenVideo)

    // VideoFrame(61): vid=1, fnum=0, data=sv
    std::vector<std::uint8_t> vf;
    vf.push_back(1); vf.push_back(0);          // vid=1
    vf.push_back(0); vf.push_back(0);          // frameNum=0
    for (auto b : sv) vf.push_back(b);

    std::vector<std::uint8_t> body;
    { BitWriter hdr; hdr.rect(0, 0, 6400, 4800); for (auto b : hdr.bytes) body.push_back(b); }  // 320x240 px
    body.push_back(0x00); body.push_back(0x18);   // frameRate 24.0
    body.push_back(0x01); body.push_back(0x00);   // frameCount=1
    writeTag(body, 60, dvs);   // DefineVideoStream
    writeTag(body, 61, vf);    // VideoFrame
    writeTag(body, 1, {});     // ShowFrame
    writeTag(body, 0, {});     // End

    std::vector<std::uint8_t> swf;
    swf.push_back('F'); swf.push_back('W'); swf.push_back('S'); swf.push_back(8);
    std::uint32_t total = 8 + static_cast<std::uint32_t>(body.size());
    for (int i = 0; i < 4; ++i) swf.push_back(static_cast<std::uint8_t>((total >> (8 * i)) & 0xFF));
    for (auto b : body) swf.push_back(b);

    swf::Movie movie; std::string err;
    bool ok = swf::decodeMovie(swf.data(), swf.size(), movie, err);
    check(ok, "decodeMovie(ScreenVideo) succeeds");
    if (!ok) { std::printf("  err=%s\nFAILED\n", err.c_str()); return 1; }

    auto vit = movie.videos.find(1);
    check(vit != movie.videos.end() && vit->second && !vit->second->decoded.empty(), "video decoded (1 frame)");
    if (vit != movie.videos.end() && vit->second && !vit->second->decoded.empty()) {
        const auto& bmp = vit->second->decoded[0];
        check(bmp && bmp->width == W && bmp->height == H, "decoded frame is 32x32");
        if (bmp) {
            const std::uint8_t* p = bmp->rgba.data();
            std::printf("  pixel(0,0)=%d,%d,%d,%d  pixel(16,16)=%d,%d,%d,%d\n",
                        p[0],p[1],p[2],p[3], p[(16*W+16)*4],p[(16*W+16)*4+1],p[(16*W+16)*4+2],p[(16*W+16)*4+3]);
            check(p[0]==0 && p[1]==255 && p[2]==0 && p[3]==255, "pixel(0,0) is green");
            const std::uint8_t* q = &bmp->rgba[(static_cast<std::size_t>(16)*W+16)*4];
            check(q[0]==0 && q[1]==255 && q[2]==0 && q[3]==255, "pixel(16,16) is green (2nd tile)");
        }
    }
    // 视频角色应可放置
    auto cit = movie.characters.find(1);
    check(cit != movie.characters.end() && cit->second && cit->second->kind == swf::Character::Kind::Video, "video registered as Character");

    std::printf("\nScreenVideo decode test: %s (%d failures)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
