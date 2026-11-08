// 文字渲染端到端测试（CTest: swfparser_render）。
// 此前字形轮廓已能正确解析，但从未在像素层面验证"文字确实被画出来"。
// 本测试在内存中构造 字体(实心方块字形) + Text，渲染后断言出现非背景的红色像素，
// 从而证明 字形轮廓 -> fillContours -> 软件光栅器 整条链路打通。
#include "swf/Decoder.hpp"
#include "swf/Render.hpp"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

static int g_fail = 0;
static void check(bool c, const char* w) {
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", w);
    if (!c) ++g_fail;
}

// 一个 em(1024) 设计空间内的实心方块字形（200..800），用于验证可见性。
static std::shared_ptr<swf::Shape> squareGlyph() {
    auto s = std::make_shared<swf::Shape>();
    s->fillStyles.resize(1);
    s->fillStyles[0].kind = swf::FillKind::Solid;
    s->fillStyles[0].color = swf::Color(255, 0, 0, 255);
    swf::Contour ct;
    ct.xs = {200, 800, 800, 200};
    ct.ys = {200, 200, 800, 800};
    s->fillPaths.resize(1);
    s->fillPaths[0].push_back(ct);
    return s;
}

int main() {
    swf::Movie m;
    m.frameSize = swf::RectB{0, 0, 2000, 2000};          // 100x100 设备像素
    m.backgroundColor = swf::Color(255, 255, 255, 255);   // 白底

    // 字体：1 个方块字形，'A'(65) -> 字形 0
    auto fg = std::make_shared<swf::FontGlyphs>();
    fg->shapes = {squareGlyph()};
    fg->codeToGlyph[65] = 0;
    auto fontCh = std::make_shared<swf::Character>();
    fontCh->kind = swf::Character::Kind::Font;
    fontCh->font = fg;
    m.characters[1] = fontCh;

    // 文本：引用字体 1 的字形 0，红色，原点放置
    auto td = std::make_shared<swf::TextDef>();
    td->textMatrix = swf::Matrix{};                       // 单位矩阵：字形 em 坐标 1:1 映射 twips
    swf::TextGlyph tg;
    tg.fontId = 1; tg.glyphIndex = 0;
    tg.color = swf::Color(255, 0, 0, 255);
    tg.x = 0; tg.y = 0; tg.advance = 200;
    // TEXTRECORD 的 FontHeight 是字形缩放的唯一依据（twips）。
    // 字形画在 EM square 坐标系里，字号取同样数值即 1:1 映射。
    tg.fontHeight = static_cast<std::uint16_t>(swf::kFontEmSquare);
    td->glyphs.push_back(tg);
    auto textCh = std::make_shared<swf::Character>();
    textCh->kind = swf::Character::Kind::Text;
    textCh->text = td;
    m.characters[2] = textCh;

    // 帧 0：放置文本（单位矩阵）
    swf::Frame fr;
    swf::DisplayItem it;
    it.depth = 1; it.character = textCh; it.matrix = swf::Matrix{};
    fr.items.push_back(it);
    m.frames.push_back(fr);

    std::vector<std::uint8_t> buf; int W = 0, H = 0;
    check(swf::renderMovieToBuffer(m, 0, buf, W, H), "renderMovieToBuffer succeeds");
    check(W == 100 && H == 100, "frame is 100x100 px (2000 twips / 20)");

    // 字形 em 200..800 -> 设备 10..40 px；统计非背景像素与红色像素
    int nonBg = 0, red = 0;
    for (int i = 0; i < W * H; ++i) {
        int r = buf[i*4], g = buf[i*4+1], b = buf[i*4+2];
        if (!(r == 255 && g == 255 && b == 255)) ++nonBg;
        if (r == 255 && g == 0 && b == 0) ++red;
    }
    check(nonBg > 0, "text rendered produces non-background pixels");
    check(red > 0, "glyph pixels are red (fill style applied)");
    // 字形中心像素 (15,15) -> twips(300,300) 落在方块(200..800)内，应为红
    int ci = (15 * W + 15) * 4;
    check(buf[ci] == 255 && buf[ci+1] == 0 && buf[ci+2] == 0, "pixel(15,15) inside glyph is red");

    std::printf("\nText render test: %s (%d failures)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
