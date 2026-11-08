// 位图滤镜渲染测试（CTest: swfparser_filter）。
// 构造带 DropShadow 滤镜的 PlaceObject3，渲染后读回像素：形状本体为原色，
// 形状外偏移处出现投影（暗色）。验证滤镜解析 + 投影生成/模糊/合成。
#include "swf/Decoder.hpp"
#include <algorithm>
#include "swf/Render.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "stb_image.h"   // 声明；实现由 Decoder.cpp 提供

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
    void u8(std::uint8_t v) { align(); bytes.push_back(v); }
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
static std::vector<std::uint8_t> filledRect(std::uint16_t id, int x0, int y0, int x1, int y1, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    BitWriter w; w.u16(id); w.rect(x0, y0, x1, y1);
    w.u8(1); w.u8(0x00); w.u8(r); w.u8(g); w.u8(b); w.u8(0xFF); w.u8(0);
    w.writeUB(1, 4); w.writeUB(0, 4);
    w.writeUB(0,1);w.writeUB(0,1);w.writeUB(0,1);w.writeUB(0,1);w.writeUB(1,1);w.writeUB(1,1);
    int mb=1;{int mm=std::max(std::abs(x0),std::abs(y0));while((1<<(mb-1))<=mm)++mb;}
    w.writeUB((std::uint32_t)mb,5); w.writeUB((std::uint32_t)x0,mb); w.writeUB((std::uint32_t)y0,mb);
    w.writeUB(1,1);
    int dxs[4]={x1-x0,0,x0-x1,0}, dys[4]={0,y1-y0,0,y0-y1};
    int maxd=0; for(int k=0;k<4;++k){maxd=std::max(maxd,std::abs(dxs[k]));maxd=std::max(maxd,std::abs(dys[k]));}
    int eb=2; while((1<<(eb-1))<=maxd)++eb;
    for(int k=0;k<4;++k){ w.writeUB(1,1);w.writeUB(1,1);w.writeUB((std::uint32_t)(eb-2),4);w.writeUB(1,1);
        w.writeUB((std::uint32_t)dxs[k],eb); w.writeUB((std::uint32_t)dys[k],eb);}
    w.writeUB(0,1);w.writeUB(0,1);w.writeUB(0,1);w.writeUB(0,1);w.writeUB(0,1);
    w.align(); return w.bytes;
}

int main() {
    // 形状（twips）：(0,0)-(2000,1000) = 100x50 px，红色
    std::vector<std::uint8_t> shape = filledRect(1, 0, 0, 2000, 1000, 255, 0, 0);

    // PlaceObject3：HasCharacter + HasFilterList(DropShadow, 黑色, 距离200twips=10px, 45°, 模糊4)
    // 结构顺序遵循规范：Depth → CharacterId → ... → FilterList。
    std::vector<std::uint8_t> po3;
    po3.push_back(0x02);   // PlaceFlag: HasCharacter
    po3.push_back(0x01);   // PlaceFlag2: HasFilterList
    po3.push_back(0x01); po3.push_back(0x00);  // depth=1
    po3.push_back(0x01); po3.push_back(0x00);  // CharacterId=1
    po3.push_back(0x01);   // FilterCount=1
    po3.push_back(0x00);   // DropShadow
    po3.push_back(0x00); po3.push_back(0x00); po3.push_back(0x00); po3.push_back(0xFF);  // RGBA 黑
    auto fixed = [&](double v){ std::int32_t raw=(std::int32_t)(v*65536.0);
        po3.push_back(raw&0xFF); po3.push_back((raw>>8)&0xFF); po3.push_back((raw>>16)&0xFF); po3.push_back((raw>>24)&0xFF); };
    fixed(4.0);   // BlurX
    fixed(4.0);   // BlurY
    fixed(45.0);  // Angle
    fixed(200.0); // Distance (10px)
    po3.push_back(0x00); po3.push_back(0x01);   // Strength = FIXED8(1.0) = 256
    po3.push_back(0x01);   // flags: passes=1

    std::vector<std::uint8_t> body;
    { BitWriter hdr; hdr.rect(0, 0, 16000, 12000); for (auto b : hdr.bytes) body.push_back(b); }  // 800x600
    body.push_back(0x00); body.push_back(0x18); body.push_back(0x01); body.push_back(0x00);
    writeTag(body, 32, shape);   // DefineShape3 id=1
    writeTag(body, 70, po3);     // PlaceObject3 (drop shadow)
    writeTag(body, 1, {}); writeTag(body, 0, {});

    std::vector<std::uint8_t> swf;
    swf.push_back('F'); swf.push_back('W'); swf.push_back('S'); swf.push_back(8);
    std::uint32_t total = 8 + static_cast<std::uint32_t>(body.size());
    for (int i = 0; i < 4; ++i) swf.push_back(static_cast<std::uint8_t>((total >> (8 * i)) & 0xFF));
    for (auto b : body) swf.push_back(b);

    swf::Movie movie; std::string err;
    bool ok = swf::decodeMovie(swf.data(), swf.size(), movie, err);
    check(ok, "decodeMovie(drop shadow) succeeds");
    if (!ok) { std::printf("  err=%s\nFAILED\n", err.c_str()); return 1; }
    check(!movie.frames.empty() && !movie.frames[0].items.empty() && !movie.frames[0].items[0].filters.empty(),
          "PlaceObject3 filter parsed (1 filter)");

    const char* outPng = "filter_test.png";
    check(swf::renderMovieToPNG(movie, 0, outPng), "render succeeds");
    int w=0,h=0,ch=0; std::uint8_t* px = stbi_load(outPng, &w, &h, &ch, 4);
    check(px != nullptr && w == 800 && h == 600, "PNG 800x600");
    if (px) {
        auto at=[&](int x,int y,int c){return px[((std::size_t)y*w+x)*4+c];};
        std::printf("  shape(50,25)=%d,%d,%d  sh(103,50)=%d,%d,%d  sh(100,50)=%d,%d,%d  far(300,300)=%d,%d,%d\n",
                    at(50,25,0),at(50,25,1),at(50,25,2), at(103,50,0),at(103,50,1),at(103,50,2),
                    at(100,50,0),at(100,50,1),at(100,50,2), at(300,300,0),at(300,300,1),at(300,300,2));
        check(at(50,25,0)>150 && at(50,25,1)<100, "shape body is red");
        check(at(103,50,0)<120 && at(103,50,1)<120 && at(103,50,2)<120, "offset shadow is dark");
        check(at(300,300,0)>200 && at(300,300,1)>200, "far area is background (no shadow)");
        stbi_image_free(px);
    }
    std::remove(outPng);
    std::printf("\nFilter render test: %s (%d failures)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
