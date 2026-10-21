// 精灵内 clipDepth 遮罩渲染测试（CTest: swfparser_spriteclip）。
// 构造一个 DefineSprite，内部放置“遮罩形状(clipDepth) + 被裁内容”，放到主时间轴；
// 渲染后读回 PNG 像素，验证内容被裁剪到遮罩区域内（此前精灵内不处理 clipDepth）。
#include "swf/Decoder.hpp"
#include "swf/Render.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "stb_image.h"   // 声明即可；实现由 Decoder.cpp 的 STB_IMAGE_IMPLEMENTATION 提供

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

// 实心矩形形状（twips 坐标）
static std::vector<std::uint8_t> filledRect(std::uint16_t id, int x0, int y0, int x1, int y1,
                                           std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    BitWriter w;
    w.u16(id);
    w.rect(x0, y0, x1, y1);
    w.u8(1);                              // fillStyleCount = 1
    w.u8(0x00); w.u8(r); w.u8(g); w.u8(b); w.u8(0xFF);  // solid RGBA
    w.u8(0);                              // lineStyleCount = 0
    w.writeUB(1, 4); w.writeUB(0, 4);     // numFillBits=1, numLineBits=0
    // StyleChangeRecord: stateFill0=1, stateMove=1
    w.writeUB(0, 1); w.writeUB(0, 1); w.writeUB(0, 1); w.writeUB(0, 1); w.writeUB(1, 1); w.writeUB(1, 1);
    int mbits = 1; { int mm = std::max(std::abs(x0), std::abs(y0)); while ((1 << (mbits - 1)) <= mm) ++mbits; }
    w.writeUB(static_cast<std::uint32_t>(mbits), 5);
    w.writeUB(static_cast<std::uint32_t>(x0), mbits);
    w.writeUB(static_cast<std::uint32_t>(y0), mbits);
    w.writeUB(1, 1);                      // fill0 = 样式索引 1（SWF 形状记录中填充索引从 1 开始，0=无填充）
    // 4 条直线边（general line：typeFlag,straight,nbits,general,dx,dy）
    int dxs[4] = { x1 - x0, 0, x0 - x1, 0 }, dys[4] = { 0, y1 - y0, 0, y0 - y1 };
    int maxd = 0; for (int k = 0; k < 4; ++k) { maxd = std::max(maxd, std::abs(dxs[k])); maxd = std::max(maxd, std::abs(dys[k])); }
    int ebits = 2; while ((1 << (ebits - 1)) <= maxd) ++ebits;
    for (int k = 0; k < 4; ++k) {
        w.writeUB(1, 1); w.writeUB(1, 1); w.writeUB(static_cast<std::uint32_t>(ebits - 2), 4); w.writeUB(1, 1);
        w.writeUB(static_cast<std::uint32_t>(dxs[k]), ebits);
        w.writeUB(static_cast<std::uint32_t>(dys[k]), ebits);
    }
    w.writeUB(0, 1); w.writeUB(0, 1); w.writeUB(0, 1); w.writeUB(0, 1); w.writeUB(0, 1);  // EndShapeRecord
    w.align();
    return w.bytes;
}

// PlaceObject2 数据
static std::vector<std::uint8_t> placeObject2(int depth, std::uint16_t cid, int clipDepth) {
    std::vector<std::uint8_t> d;
    std::uint8_t flags = 0x02;                 // HasCharacter
    if (clipDepth > 0) flags |= 0x40;          // HasClipDepth
    d.push_back(flags);
    d.push_back(depth & 0xFF); d.push_back((depth >> 8) & 0xFF);
    d.push_back(cid & 0xFF); d.push_back((cid >> 8) & 0xFF);
    if (clipDepth > 0) { d.push_back(clipDepth & 0xFF); d.push_back((clipDepth >> 8) & 0xFF); }
    return d;
}

int main() {
    // 形状（twips）：遮罩=左半 (0,0)-(5500,8000)[x0..275px]，内容=右段 (2000,0)-(11000,8000)[x100..550px]
    std::vector<std::uint8_t> maskShape = filledRect(1, 0, 0, 5500, 8000, 255, 0, 0);      // 红（遮罩区域 + 自身绘制）
    std::vector<std::uint8_t> contentShape = filledRect(2, 2000, 0, 11000, 8000, 0, 0, 255);  // 蓝（被裁内容）

    // 精灵内部子标签：遮罩(depth1,clipDepth2) + 内容(depth2) + ShowFrame + End
    std::vector<std::uint8_t> spriteBody;
    writeTag(spriteBody, 26, placeObject2(1, 1, 2));  // 遮罩 clipDepth=2
    writeTag(spriteBody, 26, placeObject2(2, 2, 0));  // 内容
    writeTag(spriteBody, 1, {});                      // ShowFrame
    writeTag(spriteBody, 0, {});                      // End
    std::vector<std::uint8_t> spriteData;
    spriteData.push_back(3); spriteData.push_back(0);            // spriteId=3
    spriteData.push_back(1); spriteData.push_back(0);            // frameCount=1
    for (auto b : spriteBody) spriteData.push_back(b);

    // 主时间轴 body
    std::vector<std::uint8_t> body;
    { BitWriter hdr; hdr.rect(0, 0, 11000, 8000); for (auto b : hdr.bytes) body.push_back(b); }  // 550x400px
    body.push_back(0x00); body.push_back(0x18);   // frameRate 24.0
    body.push_back(0x01); body.push_back(0x00);   // frameCount = 1
    writeTag(body, 32, maskShape);      // DefineShape3 id=1 (遮罩, RGBA)
    writeTag(body, 32, contentShape);   // DefineShape3 id=2 (内容, RGBA)
    writeTag(body, 39, spriteData);     // DefineSprite id=3
    writeTag(body, 26, placeObject2(1, 3, 0));  // 放置精灵于 depth1
    writeTag(body, 1, {});              // ShowFrame
    writeTag(body, 0, {});              // End

    std::vector<std::uint8_t> swf;
    swf.push_back('F'); swf.push_back('W'); swf.push_back('S'); swf.push_back(8);
    std::uint32_t total = 8 + static_cast<std::uint32_t>(body.size());
    for (int i = 0; i < 4; ++i) swf.push_back(static_cast<std::uint8_t>((total >> (8 * i)) & 0xFF));
    for (auto b : body) swf.push_back(b);

    swf::Movie movie; std::string err;
    bool ok = swf::decodeMovie(swf.data(), swf.size(), movie, err);
    check(ok, "decodeMovie(sprite with clip group) succeeds");
    if (!ok) { std::printf("  err=%s\nFAILED\n", err.c_str()); return 1; }

    // 结构校验：精灵第 0 帧含 clipDepth 项
    auto sit = movie.characters.find(3);
    bool hasClip = sit != movie.characters.end() && sit->second &&
                   sit->second->kind == swf::Character::Kind::Sprite && !sit->second->spriteFrames.empty() &&
                   !sit->second->spriteFrames[0].items.empty() &&
                   sit->second->spriteFrames[0].items[0].clipDepth == 2;
    check(hasClip, "sprite frame 0 has clipDepth item (=2)");

    // 渲染并读回像素
    const char* outPng = "spriteclip_test.png";
    bool rendered = swf::renderMovieToPNG(movie, 0, outPng);
    check(rendered, "renderMovieToPNG succeeds");
    if (!rendered) { std::printf("\nFAILED\n"); return 1; }

    int w = 0, h = 0, ch = 0;
    std::uint8_t* px = stbi_load(outPng, &w, &h, &ch, 4);
    check(px != nullptr && w == 550 && h == 400, "PNG is 550x400");
    if (px) {
        auto pxAt = [&](int x, int y, int c) { return px[(static_cast<std::size_t>(y) * w + x) * 4 + c]; };
        std::printf("  maskOnly(50,200)=%d,%d,%d  content(200,200)=%d,%d,%d  clippedOut(400,200)=%d,%d,%d\n",
                    pxAt(50,200,0), pxAt(50,200,1), pxAt(50,200,2),
                    pxAt(200,200,0), pxAt(200,200,1), pxAt(200,200,2),
                    pxAt(400,200,0), pxAt(400,200,1), pxAt(400,200,2));
        // 遮罩形状自身被绘制：x∈[0,100) 只有遮罩(红)
        check(pxAt(50, 200, 0) > 150 && pxAt(50, 200, 1) < 100 && pxAt(50, 200, 2) < 100, "mask-only region shows mask shape (red)");
        // 遮罩∩内容区域：内容(蓝)覆盖在遮罩之上
        check(pxAt(200, 200, 2) > 150 && pxAt(200, 200, 0) < 100, "mask∩content region shows content (blue)");
        // 遮罩外且内容被裁掉：背景(白)
        check(pxAt(400, 200, 0) > 200 && pxAt(400, 200, 1) > 200 && pxAt(400, 200, 2) > 200, "outside mask = content clipped out (white)");
        stbi_image_free(px);
    }
    std::remove(outPng);

    std::printf("\nSprite clipDepth render test: %s (%d failures)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
