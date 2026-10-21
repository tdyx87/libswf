// PlaceObject3(tag 70) 解析测试（CTest: swfparser_placeobject3）。
// 验证 PlaceFlag2 位定义（HasBlendMode=0x02 等）与字段顺序：
// 带 BlendMode 的 PlaceObject3 必须消费该字节，后续 CharacterId 才能对齐（旧实现位错误会错位→角色为空）。
#include "swf/Decoder.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

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

// 最小空形状（0 填充/0 线 + EndShapeRecord）
static std::vector<std::uint8_t> minimalShape(std::uint16_t id) {
    BitWriter sh;
    sh.u16(id);
    sh.rect(0, 0, 100, 100);
    sh.u8(0x00);   // fillStyleCount = 0
    sh.u8(0x00);   // lineStyleCount = 0
    sh.u8(0x00);   // numFillBits(4)=0, numLineBits(4)=0
    sh.u8(0x00);   // EndShapeRecord (6 zero bits + pad)
    return sh.bytes;
}

int main() {
    // DefineShape(2) id=1
    std::vector<std::uint8_t> shapeData = minimalShape(1);

    // PlaceObject3(70): HasCharacter(0x02) + HasBlendMode(0x02)
    std::vector<std::uint8_t> po3;
    po3.push_back(0x02);               // PlaceFlag: HasCharacter
    po3.push_back(0x02);               // PlaceFlag2: HasBlendMode
    po3.push_back(0x01); po3.push_back(0x00);  // depth = 1
    po3.push_back(0x02);               // BlendMode byte (must be consumed)
    po3.push_back(0x01); po3.push_back(0x00);  // CharacterId = 1

    // body
    std::vector<std::uint8_t> body;
    { BitWriter hdr; hdr.rect(0, 0, 550, 400); for (auto b : hdr.bytes) body.push_back(b); }
    body.push_back(0x00); body.push_back(0x18);   // frameRate 24.0
    body.push_back(0x01); body.push_back(0x00);   // frameCount = 1
    writeTag(body, 2, shapeData);    // DefineShape
    writeTag(body, 70, po3);         // PlaceObject3 (with blend)
    writeTag(body, 1, {});           // ShowFrame
    writeTag(body, 0, {});           // End

    std::vector<std::uint8_t> swf;
    swf.push_back('F'); swf.push_back('W'); swf.push_back('S'); swf.push_back(8);
    std::uint32_t total = 8 + static_cast<std::uint32_t>(body.size());
    for (int i = 0; i < 4; ++i) swf.push_back(static_cast<std::uint8_t>((total >> (8 * i)) & 0xFF));
    for (auto b : body) swf.push_back(b);

    swf::Movie movie; std::string err;
    bool ok = swf::decodeMovie(swf.data(), swf.size(), movie, err);
    check(ok, "decodeMovie(PlaceObject3+blend) succeeds");
    if (!ok) { std::printf("  err=%s\nFAILED\n", err.c_str()); return 1; }

    check(movie.frames.size() == 1 && movie.frames[0].items.size() == 1, "one placed item");
    if (!movie.frames.empty() && !movie.frames[0].items.empty()) {
        const auto& item = movie.frames[0].items[0];
        check(item.depth == 1, "depth == 1");
        check(item.character != nullptr, "character resolved (blend byte consumed -> CharacterId aligned)");
        check(item.blend != swf::BlendMode::Normal, "blend mode was read (non-default)");
    }

    // 对照：不带 PlaceFlag2 的 PlaceObject3 也应正常解析
    std::vector<std::uint8_t> po3b;
    po3b.push_back(0x02); po3b.push_back(0x00);   // HasCharacter, no flags2
    po3b.push_back(0x01); po3b.push_back(0x00);   // depth=1
    po3b.push_back(0x01); po3b.push_back(0x00);   // characterId=1
    std::vector<std::uint8_t> body2;
    { BitWriter hdr; hdr.rect(0, 0, 550, 400); for (auto b : hdr.bytes) body2.push_back(b); }
    body2.push_back(0x00); body2.push_back(0x18); body2.push_back(0x01); body2.push_back(0x00);
    writeTag(body2, 2, shapeData);
    writeTag(body2, 70, po3b);
    writeTag(body2, 1, {}); writeTag(body2, 0, {});
    std::vector<std::uint8_t> swf2;
    swf2.push_back('F'); swf2.push_back('W'); swf2.push_back('S'); swf2.push_back(8);
    std::uint32_t t2 = 8 + static_cast<std::uint32_t>(body2.size());
    for (int i = 0; i < 4; ++i) swf2.push_back(static_cast<std::uint8_t>((t2 >> (8 * i)) & 0xFF));
    for (auto b : body2) swf2.push_back(b);
    swf::Movie m2; std::string e2;
    bool ok2 = swf::decodeMovie(swf2.data(), swf2.size(), m2, e2);
    check(ok2 && !m2.frames.empty() && !m2.frames[0].items.empty() &&
          m2.frames[0].items[0].character != nullptr, "PlaceObject3 without flags2 places character");

    std::printf("\nPlaceObject3 parse test: %s (%d failures)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
