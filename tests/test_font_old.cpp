// 旧字体（DefineFont(10) + DefineFontInfo(13/62)）解析测试（CTest: swfparser_font_old）。
// DefineFont(10) 仅含字形轮廓，不含字符码表；字形数由偏移表首个偏移(=nGlyphs*2)推出；
// 字符码表由随后出现的 DefineFontInfo(13)/DefineFontInfo2(62) 提供（Unicode 时为 UI16，否则 UI8）。
// 字形轮廓复用与 DefineFont2/3 相同的裸 SHAPE 解析路径。
#include "swf/Decoder.hpp"
#include <algorithm>

#include <cstdint>
#include <cstdio>
#include <vector>

static int g_fail = 0;
static void check(bool c, const char* w) {
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", w);
    if (!c) ++g_fail;
}

static void writeTag(std::vector<std::uint8_t>& out, int code, const std::vector<std::uint8_t>& data) {
    std::size_t len = data.size();
    std::uint16_t h = static_cast<std::uint16_t>((code << 6) | (len < 63 ? (len & 0x3F) : 0x3F));
    out.push_back(h & 0xFF); out.push_back((h >> 8) & 0xFF);
    if (len >= 63) for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>((len >> (8 * i)) & 0xFF));
    for (auto b : data) out.push_back(b);
}
static void u16(std::vector<std::uint8_t>& v, std::uint16_t x) { v.push_back(x & 0xFF); v.push_back((x >> 8) & 0xFF); }

// 裸 SHAPE 字形（与 test_font.cpp 同构）：empty=true 为空字形（空格），false 为带 1 条填充轮廓的字形。
static std::vector<std::uint8_t> makeGlyph(bool empty) {
    std::vector<std::uint8_t> g;
    if (empty) { g.push_back(0x00); g.push_back(0x00); return g; }
    g.push_back(0x10);
    const int bits[] = {0,0,0,0,1,1, 0,0,0,0,0, 1, 1,1,0,0,0,0,1,0,1,0,0, 0,0,0,0,0,0};
    const int n = static_cast<int>(sizeof(bits) / sizeof(bits[0]));
    int acc = 0, cnt = 0;
    for (int i = 0; i < n; ++i) {
        acc = (acc << 1) | bits[i];
        if (++cnt == 8) { g.push_back(static_cast<std::uint8_t>(acc)); acc = 0; cnt = 0; }
    }
    if (cnt > 0) { acc <<= (8 - cnt); g.push_back(static_cast<std::uint8_t>(acc)); }
    return g;
}

// 构造完整 SWF(version=8)，内含 DefineFont(10) 与 DefineFontInfo(13 或 62)。
static std::vector<std::uint8_t> buildSwf(const std::vector<std::uint8_t>& d10,
                                          const std::vector<std::uint8_t>& d13, int code13) {
    std::vector<std::uint8_t> body;
    auto rect = [&](int x0, int y0, int x1, int y1) {
        int m = std::max(std::max(std::abs(x0), std::abs(x1)), std::max(std::abs(y0), std::abs(y1)));
        int nb = 1; while ((1 << (nb - 1)) - 1 < m && nb < 30) ++nb;
        int bb = 0, bc = 0;
        auto put = [&](std::uint32_t val, int n) {
            for (int i = n - 1; i >= 0; --i) { bb = (bb << 1) | (int)((val >> i) & 1u); if (++bc == 8) { body.push_back((std::uint8_t)bb); bb = 0; bc = 0; } }
        };
        put((std::uint32_t)nb, 5);
        put((std::uint32_t)x0, nb); put((std::uint32_t)x1, nb); put((std::uint32_t)y0, nb); put((std::uint32_t)y1, nb);
        if (bc > 0) { bb <<= (8 - bc); body.push_back((std::uint8_t)bb); bb = 0; bc = 0; }
    };
    rect(0, 0, 11000, 8000);
    body.push_back(0x00); body.push_back(0x18);   // frameRate 24
    body.push_back(0x01); body.push_back(0x00);   // frameCount=1
    writeTag(body, 10, d10);
    writeTag(body, code13, d13);
    writeTag(body, 1, {});   // ShowFrame
    writeTag(body, 0, {});   // End
    std::vector<std::uint8_t> swf;
    swf.push_back('F'); swf.push_back('W'); swf.push_back('S'); swf.push_back(8);
    std::uint32_t total = 8 + static_cast<std::uint32_t>(body.size());
    for (int i = 0; i < 4; ++i) swf.push_back(static_cast<std::uint8_t>((total >> (8 * i)) & 0xFF));
    for (auto b : body) swf.push_back(b);
    return swf;
}

// 构造 DefineFont(10)：FontID=1, 2 个字形（g0 空, g1 有轮廓）。
// 偏移表 UI16[2]：offset[0]=4(=nGlyphs*2), offset[1]=6(glyph1 位于表起点+6)。
static std::vector<std::uint8_t> makeFont10() {
    std::vector<std::uint8_t> g0 = makeGlyph(true), g1 = makeGlyph(false);
    std::vector<std::uint8_t> d;
    u16(d, 1);        // FontID=1
    u16(d, 4);        // OffsetTable[0]=4 -> glyph0 @ 2+4=6
    u16(d, 6);        // OffsetTable[1]=6 -> glyph1 @ 2+6=8
    for (auto b : g0) d.push_back(b);   // (6-7)
    for (auto b : g1) d.push_back(b);   // (8-12), 共 13 字节
    return d;
}

static void runCase(const char* name, int code13, const std::vector<std::uint8_t>& d13,
                    bool wide) {
    std::vector<std::uint8_t> swf = buildSwf(makeFont10(), d13, code13);
    swf::Movie movie; std::string err;
    bool ok = swf::decodeMovie(swf.data(), swf.size(), movie, err);
    char msg[128];
    std::snprintf(msg, sizeof(msg), "%s: decodeMovie succeeds", name);
    check(ok, msg);
    if (!ok) { std::printf("  err=%s\n", err.c_str()); return; }
    auto cit = movie.characters.find(1);
    std::snprintf(msg, sizeof(msg), "%s: font character exists", name);
    check(cit != movie.characters.end() && cit->second && cit->second->font, msg);
    if (cit == movie.characters.end() || !cit->second || !cit->second->font) return;
    const auto& fg = *cit->second->font;
    const auto& c2g = fg.codeToGlyph;
    std::snprintf(msg, sizeof(msg), "%s: 2 glyphs", name);
    check(fg.shapes.size() == 2, msg);
    std::snprintf(msg, sizeof(msg), "%s: codeToGlyph 2 entries", name);
    check(c2g.size() == 2, msg);
    auto a = c2g.find(65), b = c2g.find(66);
    std::snprintf(msg, sizeof(msg), "%s: 'A'(65)->0, 'B'(66)->1", name);
    check(a != c2g.end() && a->second == 0 && b != c2g.end() && b->second == 1, msg);

    bool g0empty = true, g1has = false;
    if (fg.shapes.size() == 2 && fg.shapes[0] && fg.shapes[1]) {
        for (auto& cs : fg.shapes[0]->fillPaths) for (auto& ct : cs) if (!ct.xs.empty()) g0empty = false;
        for (auto& cs : fg.shapes[1]->fillPaths) for (auto& ct : cs) if (!ct.xs.empty()) g1has = true;
    }
    std::snprintf(msg, sizeof(msg), "%s: glyph0 (space) has no contour", name);
    check(g0empty, msg);
    std::snprintf(msg, sizeof(msg), "%s: glyph1 outline decoded", name);
    check(g1has, msg);
}

int main() {
    // 用例 A：DefineFontInfo(13) Unicode（代码表 UI16）
    {
        std::vector<std::uint8_t> d;
        u16(d, 1);          // FontID
        d.push_back(0);     // FontNameLen=0
        d.push_back(0x01);  // FontFlags WideCodes（UI16 代码表）
        u16(d, 65); u16(d, 66);
        runCase("caseA(info13-unicode)", 13, d, true);
    }
    // 用例 B：DefineFontInfo(13) ANSI（代码表 UI8）
    {
        std::vector<std::uint8_t> d;
        u16(d, 1);          // FontID
        d.push_back(0);     // FontNameLen=0
        d.push_back(0x00);  // FontFlags ANSI
        d.push_back(65); d.push_back(66);   // UI8 代码表
        runCase("caseB(info13-ansi)", 13, d, false);
    }
    // 用例 C：DefineFontInfo2(62) Unicode（多一个 LanguageCode 字节）
    {
        std::vector<std::uint8_t> d;
        u16(d, 1);          // FontID
        d.push_back(0);     // LanguageCode
        d.push_back(0);     // FontNameLen=0
        d.push_back(0x01);  // FontFlags WideCodes（UI16 代码表）
        u16(d, 65); u16(d, 66);
        runCase("caseC(info62-unicode)", 62, d, true);
    }

    std::printf("\nDefineFont(10)+DefineFontInfo test: %s (%d failures)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
