// DefineEditText(tag 37) 解析的端到端测试（CTest: swfparser_edittext）。
// 构造一个最小 FWS（含 DefineEditText + PlaceObject2 + ShowFrame + End），
// 验证 flags 位布局、InitialText/TextColor/FontHeight/FontId/Bounds 的解析是否正确。
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

// MSB-first 位写入器（SWF RECT 等位字段）
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
        writeUB(static_cast<std::uint32_t>(x0), nbits);
        writeUB(static_cast<std::uint32_t>(x1), nbits);
        writeUB(static_cast<std::uint32_t>(y0), nbits);
        writeUB(static_cast<std::uint32_t>(y1), nbits);
        align();
    }
};

static void writeTag(std::vector<std::uint8_t>& out, int code, const std::vector<std::uint8_t>& data) {
    std::size_t len = data.size();
    std::uint16_t h;
    if (len < 63) { h = static_cast<std::uint16_t>((code << 6) | (len & 0x3F)); }
    else { h = static_cast<std::uint16_t>((code << 6) | 0x3F); }
    out.push_back(h & 0xFF); out.push_back((h >> 8) & 0xFF);
    if (len >= 63) for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>((len >> (8 * i)) & 0xFF));
    for (auto b : data) out.push_back(b);
}

int main() {
    // ---- 构造 DefineEditText 数据 ----
    BitWriter et;
    et.u16(1);                 // CharacterID = 1
    et.rect(0, 0, 2000, 400);  // Bounds (twips)
    et.u8(0x85);               // flags byte0: HasText(0x80) | HasTextColor(0x04) | HasFont(0x01)
    et.u8(0x00);               // flags byte1
    // 字段顺序（依 SWF 规范 / 与解码器一致）：FontID → FontHeight → TextColor
    //   → VariableName → InitialText（注意 VariableName 在 InitialText **之前**）。
    et.u16(1);                 // FontId
    et.u16(480);               // FontHeight (twips)
    et.u8(0xFF); et.u8(0x00); et.u8(0x00); et.u8(0xFF);                      // TextColor RGBA (red)
    et.bytes.push_back(0);     // VariableName ""（空串，以 0 结尾）
    et.bytes.push_back('H'); et.bytes.push_back('i'); et.bytes.push_back(0);  // InitialText "Hi"
    std::vector<std::uint8_t> editTextData = et.bytes;

    // ---- 构造 PlaceObject2：放置角色 1 于 depth 1 ----
    std::vector<std::uint8_t> po2;
    po2.push_back(0x02);               // flags: HasCharacter
    po2.push_back(0x01); po2.push_back(0x00);  // depth = 1
    po2.push_back(0x01); po2.push_back(0x00);  // characterId = 1

    // ---- 组装 body ----
    std::vector<std::uint8_t> body;
    { BitWriter hdr; hdr.rect(0, 0, 550, 400); for (auto b : hdr.bytes) body.push_back(b); }  // frameSize
    body.push_back(0x00); body.push_back(0x18);   // frameRate 24.0 (0x1800)
    body.push_back(0x01); body.push_back(0x00);   // frameCount = 1
    writeTag(body, 37, editTextData);   // DefineEditText
    writeTag(body, 26, po2);            // PlaceObject2
    writeTag(body, 1, {});              // ShowFrame
    writeTag(body, 0, {});              // End

    // ---- FWS 头 + body ----
    std::vector<std::uint8_t> swf;
    swf.push_back('F'); swf.push_back('W'); swf.push_back('S'); swf.push_back(8);
    std::uint32_t total = 8 + static_cast<std::uint32_t>(body.size());
    for (int i = 0; i < 4; ++i) swf.push_back(static_cast<std::uint8_t>((total >> (8 * i)) & 0xFF));
    for (auto b : body) swf.push_back(b);

    swf::Movie movie; std::string err;
    bool ok = swf::decodeMovie(swf.data(), swf.size(), movie, err);
    check(ok, "decodeMovie(minimal SWF) succeeds");
    if (!ok) { std::printf("  err=%s\n", err.c_str()); std::printf("\nFAILED\n"); return 1; }

    auto it = movie.characters.find(1);
    check(it != movie.characters.end() && it->second &&
          it->second->kind == swf::Character::Kind::EditText && it->second->editText,
          "character 1 is EditText");
    if (it != movie.characters.end() && it->second && it->second->editText) {
        const auto& e = *it->second->editText;
        check(e.text == "Hi", "InitialText == \"Hi\"");
        check(e.fontId == 1, "fontId == 1");
        check(e.fontHeight == 480, "fontHeight == 480");
        check(e.color.r == 255 && e.color.g == 0 && e.color.b == 0 && e.color.a == 255, "textColor == red");
        check(e.bounds.x1 == 2000 && e.bounds.y1 == 400, "bounds == (0,0)-(2000,400)");
        check(!e.wordWrap && !e.multiline && !e.readonly, "flags default false");
    }
    check(movie.frames.size() == 1, "one frame");
    check(movie.frames.size() == 1 && movie.frames[0].items.size() == 1 &&
          movie.frames[0].items[0].character &&
          movie.frames[0].items[0].character->kind == swf::Character::Kind::EditText,
          "frame 0 places the EditText");

    std::printf("\nDefineEditText parse test: %s (%d failures)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
