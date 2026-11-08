// DefineFont2 代码表解析测试（CTest: swfparser_font）。
// 验证 FontFlags 位（HasLayout/WideOffsets/WideCodes）、CodeTableOffset 相对记录起始解析、
// 以及字符码->字形映射。此前 WideCodes 误用 bit7、CodeTableOffset 以整个 SWF 为基准，均会错位。
#include "swf/Decoder.hpp"
#include <algorithm>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

static int g_fail = 0;
static void check(bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) ++g_fail;
}

static void writeTag(std::vector<std::uint8_t>& out, int code, const std::vector<std::uint8_t>& data) {
    std::size_t len = data.size();
    std::uint16_t h = static_cast<std::uint16_t>((code << 6) | (len < 63 ? (len & 0x3F) : 0x3F));
    out.push_back(h & 0xFF); out.push_back((h >> 8) & 0xFF);
    if (len >= 63) for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>((len >> (8 * i)) & 0xFF));
    for (auto b : data) out.push_back(b);
}
static void u16(std::vector<std::uint8_t>& v, std::uint16_t x) { v.push_back(x & 0xFF); v.push_back((x >> 8) & 0xFF); }
static void u32(std::vector<std::uint8_t>& v, std::uint32_t x) { for (int i=0;i<4;++i) v.push_back((x>>(8*i))&0xFF); }
static int bitBuf = 0, bitCnt = 0;
static void rect(std::vector<std::uint8_t>& v, int x0, int y0, int x1, int y1) {
    int m = std::max(std::max(std::abs(x0), std::abs(x1)), std::max(std::abs(y0), std::abs(y1)));
    int nb = 1; while ((1 << (nb - 1)) - 1 < m && nb < 30) ++nb;
    auto put = [&](std::uint32_t val, int n){ for (int i=n-1;i>=0;--i){ bitBuf=(bitBuf<<1)|(int)((val>>i)&1u); if(++bitCnt==8){v.push_back((std::uint8_t)bitBuf);bitBuf=0;bitCnt=0;} } };
    put((std::uint32_t)nb, 5);
    put((std::uint32_t)x0, nb); put((std::uint32_t)x1, nb); put((std::uint32_t)y0, nb); put((std::uint32_t)y1, nb);
    if (bitCnt > 0) { bitBuf <<= (8 - bitCnt); v.push_back((std::uint8_t)bitBuf); bitBuf = 0; bitCnt = 0; }
}

// 构造一个完整 SWF（version=8），内含一个 DefineFont2(48) 或 DefineFont3(75)
static bool buildSwf(const std::vector<std::uint8_t>& f, std::vector<std::uint8_t>& swf,
                     int fontTag = 48) {
    std::vector<std::uint8_t> body;
    rect(body, 0, 0, 11000, 8000);                 // frameSize 550x400
    body.push_back(0x00); body.push_back(0x18);   // frameRate 24.0
    body.push_back(0x01); body.push_back(0x00);   // frameCount=1
    writeTag(body, fontTag, f);   // DefineFont2 / DefineFont3
    writeTag(body, 1, {});   // ShowFrame
    writeTag(body, 0, {});   // End
    swf.clear();
    swf.push_back('F'); swf.push_back('W'); swf.push_back('S'); swf.push_back(8);
    std::uint32_t total = 8 + static_cast<std::uint32_t>(body.size());
    for (int i = 0; i < 4; ++i) swf.push_back(static_cast<std::uint8_t>((total >> (8 * i)) & 0xFF));
    for (auto b : body) swf.push_back(b);
    return true;
}

// 字形为“裸 SHAPE”：NumFillBits(UB[4]) + NumLineBits(UB[4]) + SHAPERECORD。
// empty=true  -> 空字形（如空格）：numBits 全 0 + EndShapeRecord
// empty=false -> 带一条填充轮廓的字形，用于验证轮廓确实被解析出来
static std::vector<std::uint8_t> makeGlyph(bool empty) {
    std::vector<std::uint8_t> g;
    if (empty) { g.push_back(0x00); g.push_back(0x00); return g; }
    g.push_back(0x10);              // NumFillBits=1, NumLineBits=0
    const int bits[] = {
        0,0,0,0,1,1,        // StyleChange: typeFlag=0,new=0,line=0,fill1=0,fill0=1,move=1
        0,0,0,0,0,          // MoveBitCount=0 -> x,y 各占 0 位
        1,                  // FillStyle0 (NumFillBits=1) = 1
        1,1,0,0,0,0,1,0,1,0,0,  // 直线边：typeFlag=1 straight=1 nbits=0(+2) general=1 dx=1 dy=0
        0,0,0,0,0,0         // EndShapeRecord
    };
    const int n = (int)(sizeof(bits) / sizeof(bits[0]));
    int acc = 0, cnt = 0;
    for (int i = 0; i < n; ++i) {
        acc = (acc << 1) | bits[i];
        if (++cnt == 8) { g.push_back((std::uint8_t)acc); acc = 0; cnt = 0; }
    }
    if (cnt > 0) { acc <<= (8 - cnt); g.push_back((std::uint8_t)acc); }  // 补齐到字节边界
    return g;
}

int main() {
    // ---- 用例 1：HasLayout|WideOffsets|WideCodes（32 位偏移 + 32 位 CodeTableOffset）----
    // SWF>=6 时 FontFlags 之后还有一个 LanguageCode 字节（此前漏读，导致 NumGlyphs 整体错位）。
    // 偏移与代码表位置均以 table_base（NumGlyphs 之后）为基准。
    std::vector<std::uint8_t> glyph0 = makeGlyph(true), glyph1 = makeGlyph(false);
    std::vector<std::uint8_t> f;
    u16(f, 1);            // FontId                     (0-1)
    f.push_back(0x8C);    // FontFlags HasLayout|WideOffsets|WideCodes  (2)
    f.push_back(0x01);    // LanguageCode（SWF>=6）      (3)
    f.push_back(0);       // FontNameLen=0               (4)
    u16(f, 2);            // NumGlyphs=2                 (5-6)
    // tableBase = 7
    u32(f, 12);           // OffsetTable[0]=12 -> glyph0 @19   (7-10)
    u32(f, 14);           // OffsetTable[1]=14 -> glyph1 @21   (11-14)
    u32(f, 19);           // CodeTableOffset=19 -> 代码表 @26   (15-18)
    for (auto b : glyph0) f.push_back(b);   // (19-20)
    for (auto b : glyph1) f.push_back(b);   // (21-25)
    u16(f, 65);           // CodeTable[0] = 'A'          (26-27)
    u16(f, 66);           // CodeTable[1] = 'B'          (28-29)
    // FontFlagsHasLayout 的布局表紧跟 CodeTable（SI16/UI16 均为 EM square 设计单位）
    u16(f, 800);          // FontAscent
    u16(f, 200);          // FontDescent
    u16(f, 50);           // FontLeading
    u16(f, 500);          // FontAdvanceTable[0]
    u16(f, 600);          // FontAdvanceTable[1]
    f.push_back(0x00);    // FontBoundsTable[0]: RECT(NBits=0)
    f.push_back(0x00);    // FontBoundsTable[1]: RECT(NBits=0)
    u16(f, 1);            // KerningCount
    u16(f, 65); u16(f, 66); u16(f, static_cast<std::uint16_t>(-40));  // 'A','B' -> -40
    check(f.size() == 50, "case1: font tag data with layout is 50 bytes");
    check(f[26] == 65 && f[27] == 0 && f[28] == 66, "case1: code table at offset 26 = 'A','B'");

    // ---- 用例 2：仅 WideCodes（16 位偏移 + 16 位 CodeTableOffset）----
    // 这是真实 swf.swf / swf2.swf 里字体的形态（WideOffsets=0）。
    std::vector<std::uint8_t> g;
    u16(g, 2);            // FontId                     (0-1)
    g.push_back(0x04);    // FontFlags = WideCodes(0x04) (2)
    g.push_back(0x01);    // LanguageCode               (3)
    g.push_back(0);       // FontNameLen=0              (4)
    u16(g, 2);            // NumGlyphs=2                (5-6)
    // tableBase = 7
    u16(g, 6);            // OffsetTable[0]=6 -> glyph0 @13  (7-8)
    u16(g, 8);            // OffsetTable[1]=8 -> glyph1 @15  (9-10)
    u16(g, 13);           // CodeTableOffset=13 -> 代码表 @20 (11-12)
    for (auto b : glyph0) g.push_back(b);   // (13-14)
    for (auto b : glyph1) g.push_back(b);   // (15-19)
    u16(g, 0x4E00);       // CodeTable[0] = U+4E00      (20-21)
    u16(g, 0x4E0B);       // CodeTable[1] = U+4E0B      (22-23)
    check(g.size() == 24, "case2: font tag data is 24 bytes");
    check(g[20] == 0x00 && g[21] == 0x4E, "case2: code table at offset 20 = U+4E00");

    for (int c = 0; c < 2; ++c) {
        std::vector<std::uint8_t> swf;
        buildSwf(c == 0 ? f : g, swf);
        swf::Movie movie; std::string err;
        bool ok = swf::decodeMovie(swf.data(), swf.size(), movie, err);
        check(ok, c == 0 ? "case1: decodeMovie succeeds" : "case2: decodeMovie succeeds");
        if (!ok) { std::printf("  err=%s\n", err.c_str()); continue; }
        auto cit = movie.characters.find(c == 0 ? 1 : 2);
        check(cit != movie.characters.end() && cit->second && cit->second->font,
              c == 0 ? "case1: font character exists" : "case2: font character exists");
        if (cit == movie.characters.end() || !cit->second || !cit->second->font) continue;
        const auto& c2g = cit->second->font->codeToGlyph;
        check(cit->second->font->shapes.size() == 2,
              c == 0 ? "case1: 2 glyphs" : "case2: 2 glyphs");
        check(c2g.size() == 2, c == 0 ? "case1: codeToGlyph 2 entries" : "case2: codeToGlyph 2 entries");
        if (c == 0) {
            auto a = c2g.find(65), b = c2g.find(66);
            check(a != c2g.end() && a->second == 0, "case1: 'A'(65) -> glyph 0");
            check(b != c2g.end() && b->second == 1, "case1: 'B'(66) -> glyph 1");
            const auto& font = *cit->second->font;
            check(font.hasLayout, "case1: FontFlagsHasLayout decoded");
            check(font.ascent == 800 && font.descent == 200 && font.leading == 50,
                  "case1: FontAscent/Descent/Leading from layout table");
            check(font.advances.size() == 2 && font.advances[0] == 500 && font.advances[1] == 600,
                  "case1: FontAdvanceTable decoded");
            auto kit = font.kerning.find((65u << 16) | 66u);
            check(kit != font.kerning.end() && kit->second == -40,
                  "case1: KerningTable decoded (signed adjustment)");
            check(font.unitsPerEm == swf::kFontEmSquare && !font.glyphsInTwips,
                  "case1: DefineFont2 uses the EM square coordinate system");
        } else {
            auto a = c2g.find(0x4E00), b = c2g.find(0x4E0B);
            check(a != c2g.end() && a->second == 0, "case2: U+4E00 -> glyph 0");
            check(b != c2g.end() && b->second == 1, "case2: U+4E0B -> glyph 1");
        }
        // 字形轮廓：glyph0 为空字形（空格），glyph1 有 1 条填充轮廓
        const auto& shapes = cit->second->font->shapes;
        bool g0empty = true, g1hasContour = false;
        if (shapes.size() == 2 && shapes[0] && shapes[1]) {
            for (auto& cs : shapes[0]->fillPaths) for (auto& ct : cs) if (!ct.xs.empty()) g0empty = false;
            for (auto& cs : shapes[1]->fillPaths) for (auto& ct : cs) if (!ct.xs.empty()) g1hasContour = true;
        } else g1hasContour = false;
        check(g0empty, c == 0 ? "case1: glyph0 (space) has no contour" : "case2: glyph0 (space) has no contour");
        check(g1hasContour, c == 0 ? "case1: glyph1 outline decoded" : "case2: glyph1 outline decoded");
    }

    // ---- 用例 3：DefineFont3(75) 字形坐标为 DefineFont2 的 20 倍分辨率 ----
    // swf2.swf 首帧的 "LOADING"/"0%" 用 DefineFont3；unitsPerEm 若仍取 1024，
    // 嵌入字形会被放大 20 倍，铺满整个画面。
    {
        std::vector<std::uint8_t> swf;
        buildSwf(g, swf, 75);
        swf::Movie movie; std::string err;
        bool ok = swf::decodeMovie(swf.data(), swf.size(), movie, err);
        check(ok, "case3: decodeMovie(DefineFont3) succeeds");
        if (ok) {
            auto cit = movie.characters.find(2);
            check(cit != movie.characters.end() && cit->second && cit->second->font &&
                  cit->second->font->unitsPerEm == swf::kFontEmSquare * 20 &&
                  !cit->second->font->glyphsInTwips,
                  "case3: DefineFont3 uses 20x EM square coordinates");
        }
    }
    std::printf("\nDefineFont2 code-table test: %s (%d failures)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
