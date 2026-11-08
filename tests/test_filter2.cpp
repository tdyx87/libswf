// 新增位图滤镜渲染测试（CTest: swfparser_filter2）。
// 此前 Bevel / GradientGlow / GradientBevel / Convolution / ColorMatrix 仅被解析消费字节、不生成
// Filter 对象，故从未渲染。本测试构造带各滤镜的 PlaceObject3，渲染后读回像素断言可见效果：
//   - ColorMatrix：红方块 -> 蓝方块（R/B 交换）
//   - Convolution：全零核 + bias，整层变灰（证明卷积管线运行）
//   - Bevel：方块外下沿出现暗色投影（几何体被扩展）
//   - GradientGlow / GradientBevel：方块包围盒外出现彩色光晕（几何体被扩展）
#include "swf/Decoder.hpp"
#include <algorithm>
#include "swf/Render.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_fail = 0;
static void check(bool c, const char* w) {
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", w);
    if (!c) ++g_fail;
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
    void fixed(double v) { std::int32_t raw = static_cast<std::int32_t>(v * 65536.0);
        bytes.push_back(raw & 0xFF); bytes.push_back((raw >> 8) & 0xFF);
        bytes.push_back((raw >> 16) & 0xFF); bytes.push_back((raw >> 24) & 0xFF); }
    void rect(int x0, int y0, int x1, int y1) {
        int m = std::max(std::max(std::abs(x0), std::abs(x1)), std::max(std::abs(y0), std::abs(y1)));
        int nb = 1; while ((1 << (nb - 1)) - 1 < m && nb < 30) ++nb;
        writeUB(static_cast<std::uint32_t>(nb), 5);
        writeUB(static_cast<std::uint32_t>(x0), nb); writeUB(static_cast<std::uint32_t>(x1), nb);
        writeUB(static_cast<std::uint32_t>(y0), nb); writeUB(static_cast<std::uint32_t>(y1), nb);
        align();
    }
};
static void addFixed(std::vector<std::uint8_t>& out, double v) {
    std::int32_t raw = static_cast<std::int32_t>(v * 65536.0);
    out.push_back(raw & 0xFF); out.push_back((raw >> 8) & 0xFF);
    out.push_back((raw >> 16) & 0xFF); out.push_back((raw >> 24) & 0xFF);
}
// FIXED8（SI16/256）：滤镜的 Strength 字段
static void addFixed8(std::vector<std::uint8_t>& out, double v) {
    std::int16_t raw = static_cast<std::int16_t>(v * 256.0);
    out.push_back(raw & 0xFF); out.push_back((raw >> 8) & 0xFF);
}
// FLOAT32：Convolution 的 divisor/bias/矩阵与 ColorMatrix 的 20 个值
static void addFloat(std::vector<std::uint8_t>& out, float v) {
    std::uint32_t bits; std::memcpy(&bits, &v, sizeof(bits));
    out.push_back(bits & 0xFF); out.push_back((bits >> 8) & 0xFF);
    out.push_back((bits >> 16) & 0xFF); out.push_back((bits >> 24) & 0xFF);
}
static void writeTag(std::vector<std::uint8_t>& out, int code, const std::vector<std::uint8_t>& data) {
    std::size_t len = data.size();
    std::uint16_t h = static_cast<std::uint16_t>((code << 6) | (len < 63 ? (len & 0x3F) : 0x3F));
    out.push_back(h & 0xFF); out.push_back((h >> 8) & 0xFF);
    if (len >= 63) for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>((len >> (8 * i)) & 0xFF));
    for (auto b : data) out.push_back(b);
}
static std::vector<std::uint8_t> filledRect(std::uint16_t id, int x0, int y0, int x1, int y1,
                                            std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    BitWriter w; w.u16(id); w.rect(x0, y0, x1, y1);
    w.u8(1); w.u8(0x00); w.u8(r); w.u8(g); w.u8(b); w.u8(0xFF); w.u8(0);
    w.writeUB(1, 4); w.writeUB(0, 4);
    w.writeUB(0,1); w.writeUB(0,1); w.writeUB(0,1); w.writeUB(0,1); w.writeUB(1,1); w.writeUB(1,1);
    int mb = 1; { int mm = std::max(std::abs(x0), std::abs(y0)); while ((1 << (mb - 1)) <= mm) ++mb; }
    w.writeUB(static_cast<std::uint32_t>(mb), 5); w.writeUB(static_cast<std::uint32_t>(x0), mb); w.writeUB(static_cast<std::uint32_t>(y0), mb); w.writeUB(1,1);
    int dxs[4] = {x1 - x0, 0, x0 - x1, 0}, dys[4] = {0, y1 - y0, 0, y0 - y1};
    int maxd = 0; for (int k = 0; k < 4; ++k) { maxd = std::max(maxd, std::abs(dxs[k])); maxd = std::max(maxd, std::abs(dys[k])); }
    int eb = 2; while ((1 << (eb - 1)) <= maxd) ++eb;
    for (int k = 0; k < 4; ++k) {
        w.writeUB(1,1); w.writeUB(1,1); w.writeUB(static_cast<std::uint32_t>(eb - 2), 4); w.writeUB(1,1);
        w.writeUB(static_cast<std::uint32_t>(dxs[k]), eb); w.writeUB(static_cast<std::uint32_t>(dys[k]), eb);
    }
    w.writeUB(0,1); w.writeUB(0,1); w.writeUB(0,1); w.writeUB(0,1); w.writeUB(0,1); w.align();
    return w.bytes;
}
static std::vector<std::uint8_t> buildSwf(const std::vector<std::uint8_t>& shape, const std::vector<std::uint8_t>& po3) {
    std::vector<std::uint8_t> body;
    { BitWriter hdr; hdr.rect(0, 0, 16000, 12000); for (auto b : hdr.bytes) body.push_back(b); }
    body.push_back(0x00); body.push_back(0x18); body.push_back(0x01); body.push_back(0x00);
    writeTag(body, 32, shape);
    writeTag(body, 70, po3);
    writeTag(body, 1, {}); writeTag(body, 0, {});
    std::vector<std::uint8_t> swf;
    swf.push_back('F'); swf.push_back('W'); swf.push_back('S'); swf.push_back(8);
    std::uint32_t total = 8 + static_cast<std::uint32_t>(body.size());
    for (int i = 0; i < 4; ++i) swf.push_back(static_cast<std::uint8_t>((total >> (8 * i)) & 0xFF));
    for (auto b : body) swf.push_back(b);
    return swf;
}

// 渲染并取像素统计
struct Raster {
    std::vector<std::uint8_t> buf; int W = 0, H = 0;
    bool ok = false;
    int r(int x,int y) const { return buf[((std::size_t)y*W+x)*4]; }
    int g(int x,int y) const { return buf[((std::size_t)y*W+x)*4+1]; }
    int b(int x,int y) const { return buf[((std::size_t)y*W+x)*4+2]; }
    bool nonWhite(int x,int y) const { return !(r(x,y)>250 && g(x,y)>250 && b(x,y)>250); }
    int outsideNonWhite() const { int c=0; for(int y=0;y<H;++y) for(int x=0;x<W;++x)
        if (nonWhite(x,y) && (x>=102 || y>=52)) ++c; return c; }
};
static Raster raster(const std::vector<std::uint8_t>& swf, const char* name) {
    Raster R; swf::Movie m; std::string err;
    if (!swf::decodeMovie(swf.data(), swf.size(), m, err)) { std::printf("  decode fail %s\n", err.c_str()); return R; }
    if (m.frames.empty() || m.frames[0].items.empty() || m.frames[0].items[0].filters.empty()) {
        std::printf("  filter not parsed\n"); return R;
    }
    R.ok = swf::renderMovieToBuffer(m, 0, R.buf, R.W, R.H);
    return R;
}

int main() {
    // 红色方块 (0,0)-(2000,1000)twips = (0,0)-(100,50)px
    std::vector<std::uint8_t> shape = filledRect(1, 0, 0, 2000, 1000, 255, 0, 0);

    // ---- ColorMatrix：R<->B 交换 ----
    {
        // 结构顺序：Depth → CharacterId → ... → FilterList
        std::vector<std::uint8_t> po3;
        po3.push_back(0x02); po3.push_back(0x01); po3.push_back(0x01); po3.push_back(0x00);  // flags, depth
        po3.push_back(0x01); po3.push_back(0x00);    // CharacterId=1
        po3.push_back(0x01); po3.push_back(0x06);   // 1 filter, ColorMatrix
        float m[20] = {0,0,1,0,0, 0,1,0,0,0, 1,0,0,0,0, 0,0,0,1,0};
        for (float v : m) addFloat(po3, v);
        Raster R = raster(buildSwf(shape, po3), "colormatrix");
        check(R.ok && R.W == 800 && R.H == 600, "colormatrix: renders 800x600");
        check(R.r(50,25) < 80 && R.b(50,25) > 150, "colormatrix: red(255,0,0) -> blue(0,0,255)");
    }
    // ---- Convolution：全零核 + bias=0.5 -> 整层灰 ----
    {
        std::vector<std::uint8_t> po3;
        po3.push_back(0x02); po3.push_back(0x01); po3.push_back(0x01); po3.push_back(0x00);  // flags, depth
        po3.push_back(0x01); po3.push_back(0x00);    // CharacterId=1
        po3.push_back(0x01); po3.push_back(0x05);   // Convolution
        po3.push_back(3); po3.push_back(3);          // 3x3
        addFloat(po3, 1.0f); addFloat(po3, 0.5f);    // divisor, bias
        for (int i = 0; i < 9; ++i) addFloat(po3, 0.0f);
        po3.push_back(0); po3.push_back(0); po3.push_back(0); po3.push_back(0);  // default color
        po3.push_back(0x02);                         // clamp=1, preserveAlpha=0
        Raster R = raster(buildSwf(shape, po3), "convolution");
        bool gray = R.ok && R.r(50,25) > 160 && R.g(50,25) > 160 && R.b(50,25) > 160
                    && std::abs(R.r(400,300)-R.g(400,300)) < 12 && std::abs(R.g(400,300)-R.b(400,300)) < 12;
        check(R.ok, "convolution: renders");
        check(gray, "convolution: uniform gray (kernel+bias applied)");
    }
    // ---- Bevel：方块外下沿出现暗色投影 ----
    {
        std::vector<std::uint8_t> po3;
        po3.push_back(0x02); po3.push_back(0x01); po3.push_back(0x01); po3.push_back(0x00);  // flags, depth
        po3.push_back(0x01); po3.push_back(0x00);    // CharacterId=1
        po3.push_back(0x01); po3.push_back(0x03);   // Bevel
        po3.push_back(0); po3.push_back(0); po3.push_back(0); po3.push_back(255);   // shadow black
        po3.push_back(255); po3.push_back(255); po3.push_back(255); po3.push_back(255); // highlight white
        addFixed(po3, 0.0); addFixed(po3, 0.0); addFixed(po3, 45.0); addFixed(po3, 200.0);
        addFixed8(po3, 1.0);                         // strength (FIXED8)
        po3.push_back(0x01);                         // passes=1
        Raster R = raster(buildSwf(shape, po3), "bevel");
        check(R.ok, "bevel: renders");
        check(R.r(50,25) > 150 && R.g(50,25) < 100, "bevel: object body still red");
        check(R.r(105,55) < 90 && R.g(105,55) < 90, "bevel: offset shadow drawn outside shape (dark)");
    }
    // ---- GradientGlow：包围盒外出现彩色光晕 ----
    {
        std::vector<std::uint8_t> po3;
        po3.push_back(0x02); po3.push_back(0x01); po3.push_back(0x01); po3.push_back(0x00);  // flags, depth
        po3.push_back(0x01); po3.push_back(0x00);    // CharacterId=1
        po3.push_back(0x01); po3.push_back(0x04);   // GradientGlow
        po3.push_back(2);                            // numColors
        po3.push_back(255); po3.push_back(0); po3.push_back(0); po3.push_back(255);     // red
        po3.push_back(255); po3.push_back(255); po3.push_back(0); po3.push_back(255);   // yellow
        po3.push_back(0); po3.push_back(255);        // ratios
        addFixed(po3, 160.0); addFixed(po3, 160.0);  // blurX/Y = 8px
        addFixed(po3, 0.0); addFixed(po3, 0.0);
        addFixed8(po3, 1.0);                         // angle, distance, strength (FIXED8)
        po3.push_back(0x01);                         // passes=1
        Raster R = raster(buildSwf(shape, po3), "gradientglow");
        check(R.ok, "gradientglow: renders");
        check(R.outsideNonWhite() > 0, "gradientglow: colored halo outside shape bbox");
    }
    // ---- GradientBevel：包围盒外出现光晕 ----
    {
        std::vector<std::uint8_t> po3;
        po3.push_back(0x02); po3.push_back(0x01); po3.push_back(0x01); po3.push_back(0x00);  // flags, depth
        po3.push_back(0x01); po3.push_back(0x00);    // CharacterId=1
        po3.push_back(0x01); po3.push_back(0x07);   // GradientBevel
        po3.push_back(2);
        po3.push_back(0); po3.push_back(0); po3.push_back(0); po3.push_back(255);       // shadow black
        po3.push_back(255); po3.push_back(255); po3.push_back(255); po3.push_back(255); // highlight white
        po3.push_back(0); po3.push_back(255);
        addFixed(po3, 160.0); addFixed(po3, 160.0);
        addFixed(po3, 45.0); addFixed(po3, 200.0);
        addFixed8(po3, 1.0);                         // strength (FIXED8)
        po3.push_back(0x01);
        Raster R = raster(buildSwf(shape, po3), "gradientbevel");
        check(R.ok, "gradientbevel: renders");
        check(R.outsideNonWhite() > 0, "gradientbevel: halo outside shape bbox");
    }

    std::printf("\nNew-filter render test: %s (%d failures)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
