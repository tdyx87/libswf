// SWF 解码器：把字节流还原成"字典 + 时间轴帧"，供软件光栅器渲染。
// 覆盖：DefineShape(1-4) / DefineBits(JPEG) / DefineBitsLossless(2) / PlaceObject(1/2/3)
//       / RemoveObject(2) / ShowFrame / SetBackgroundColor / JPEGTables。
#include "swf/Decoder.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

#include "swf/BitReader.hpp"
#include "swf/ByteView.hpp"
#include "swf/Decompress.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace swf {

// ---------- 小工具 ----------
static void skipTagData(ByteView& bv, std::size_t len) { bv.seek(bv.pos() + len); }

static int neededBits(std::size_t n);
static void decodeLossless(std::uint8_t fmt, bool withAlpha, std::uint16_t w, std::uint16_t h,
                          const std::vector<std::uint8_t>& raw, Bitmap& bmp);

Matrix Matrix::post(const Matrix& o) const {
    // this * o：先应用 o 再应用 this
    Matrix r;
    r.sx = sx * o.sx + r0 * o.r1;
    r.r0 = sx * o.r0 + r0 * o.sy;
    r.r1 = r1 * o.sx + sy * o.r1;
    r.sy = r1 * o.r0 + sy * o.sy;
    float otx = sx * o.tx + r0 * o.ty + tx;
    float oty = r1 * o.tx + sy * o.ty + ty;
    r.tx = otx; r.ty = oty;
    return r;
}
void Matrix::apply(float x, float y, float& ox, float& oy) const {
    ox = sx * x + r0 * y + tx;
    oy = r1 * x + sy * y + ty;
}
bool Matrix::invert(Matrix& out) const {
    float det = sx * sy - r0 * r1;
    if (det == 0) return false;
    float id = 1.0f / det;
    out.sx =  sy * id;
    out.sy =  sx * id;
    out.r0 = -r0 * id;
    out.r1 = -r1 * id;
    out.tx = -(out.sx * tx + out.r0 * ty);
    out.ty = -(out.r1 * tx + out.sy * ty);
    return true;
}

Color ColorTransform::apply(const Color& c) const {
    Color o = c;
    if (hasMult) {
        o.r = static_cast<std::uint8_t>(std::max(0.f, std::min(255.f, c.r * mr)));
        o.g = static_cast<std::uint8_t>(std::max(0.f, std::min(255.f, c.g * mg)));
        o.b = static_cast<std::uint8_t>(std::max(0.f, std::min(255.f, c.b * mb)));
        o.a = static_cast<std::uint8_t>(std::max(0.f, std::min(255.f, c.a * ma)));
    }
    if (hasAdd) {
        int r = o.r + static_cast<int>(ar);
        int g = o.g + static_cast<int>(ag);
        int b = o.b + static_cast<int>(ab);
        int a = o.a + static_cast<int>(aa);
        o.r = static_cast<std::uint8_t>(std::max(0, std::min(255, r)));
        o.g = static_cast<std::uint8_t>(std::max(0, std::min(255, g)));
        o.b = static_cast<std::uint8_t>(std::max(0, std::min(255, b)));
        o.a = static_cast<std::uint8_t>(std::max(0, std::min(255, a)));
    }
    return o;
}

std::shared_ptr<Character> Movie::getCharacter(std::uint16_t id) const {
    auto it = characters.find(id);
    return it == characters.end() ? nullptr : it->second;
}

// ---------- 基础读取（位流，确定性字节对齐，绕开 BitReader::finish 的偏差） ----------
static Rect readRect(ByteView& bv) {
    std::size_t start = bv.pos();
    BitReader br(bv);
    std::uint32_t n = br.readUB(5);
    Rect r;
    r.xMin = br.readSB(static_cast<int>(n));
    r.xMax = br.readSB(static_cast<int>(n));
    r.yMin = br.readSB(static_cast<int>(n));
    r.yMax = br.readSB(static_cast<int>(n));
    int bits = 5 + 4 * static_cast<int>(n);
    bv.seek(start + (bits + 7) / 8);   // 精确字节推进
    return r;
}

static Matrix readMatrix(ByteView& bv) {
    std::size_t start = bv.pos();
    BitReader br(bv);
    Matrix m;
    int total = 0;
    int hasScale = static_cast<int>(br.readUB(1)); total += 1;
    int n = 0, nr = 0, nt = 0;
    if (hasScale) {
        n = static_cast<int>(br.readUB(5)); total += 5;
        m.sx = br.readSB(n) / 65536.0f; m.sy = br.readSB(n) / 65536.0f; total += 2 * n;
    }
    int hasRot = static_cast<int>(br.readUB(1)); total += 1;
    if (hasRot) {
        nr = static_cast<int>(br.readUB(5)); total += 5;
        m.r0 = br.readSB(nr) / 65536.0f; m.r1 = br.readSB(nr) / 65536.0f; total += 2 * nr;
    }
    nt = static_cast<int>(br.readUB(5)); total += 5;
    m.tx = static_cast<float>(br.readSB(nt));   // twips
    m.ty = static_cast<float>(br.readSB(nt));   // twips
    total += 2 * nt;
    bv.seek(start + (total + 7) / 8);
    return m;
}

static ColorTransform readCXForm(ByteView& bv, bool withAlpha) {
    std::size_t start = bv.pos();
    BitReader br(bv);
    ColorTransform c;
    int total = 0;
    int hasAdd = static_cast<int>(br.readUB(1)); total += 1;
    int hasMult = static_cast<int>(br.readUB(1)); total += 1;
    int n = static_cast<int>(br.readUB(4)); total += 4;
    int steps = (withAlpha ? 4 : 3);
    if (hasMult) {
        c.hasMult = true;
        c.mr = br.readSB(n) / 256.0f; total += n;
        c.mg = br.readSB(n) / 256.0f; total += n;
        c.mb = br.readSB(n) / 256.0f; total += n;
        c.ma = withAlpha ? br.readSB(n) / 256.0f : 1.0f; total += (withAlpha ? n : 0);
    }
    if (hasAdd) {
        c.hasAdd = true;
        c.ar = br.readSB(n) / 256.0f; total += n;
        c.ag = br.readSB(n) / 256.0f; total += n;
        c.ab = br.readSB(n) / 256.0f; total += n;
        c.aa = withAlpha ? br.readSB(n) / 256.0f : 0.0f; total += (withAlpha ? n : 0);
    }
    bv.seek(start + (total + 7) / 8);
    return c;
}

static void readColor(ByteView& bv, bool alpha, Color& c) {
    c.r = bv.readUI8(); c.g = bv.readUI8(); c.b = bv.readUI8();
    c.a = alpha ? bv.readUI8() : 0xFF;
}

// ---------- 位图解码 ----------
static bool decodeJpeg(const std::uint8_t* data, int len, Bitmap& bmp) {
    int w = 0, h = 0, comp = 0;
    stbi_uc* px = stbi_load_from_memory(data, len, &w, &h, &comp, 4);
    if (!px) return false;
    bmp.width = w; bmp.height = h;
    bmp.rgba.assign(px, px + static_cast<std::size_t>(w) * h * 4);
    stbi_image_free(px);
    return true;
}

static void applyAlpha(Bitmap& bmp, const std::vector<std::uint8_t>& alpha) {
    std::size_t n = static_cast<std::size_t>(bmp.width) * bmp.height;
    if (alpha.size() < n) return;
    for (std::size_t i = 0; i < n; ++i) bmp.rgba[i * 4 + 3] = alpha[i];
}

// ---------- 形状解码 ----------
namespace {
struct Cur {
    std::vector<float> sx, sy;
    bool started = false;
};

struct ShapeBuilder {
    std::vector<FillStyle>* fills = nullptr;
    std::vector<LineStyle>* lines = nullptr;
    std::shared_ptr<Shape> shape;

    int actFill0 = 0, actFill1 = 0, actLine = 0;
    float penX = 0, penY = 0;
    Cur c0, c1, cl;

    void ensure(Cur& c, float x, float y) {
        if (!c.started) { c.sx.push_back(x); c.sy.push_back(y); c.started = true; }
    }
    void add(Cur& c, float x, float y) { ensure(c, x, y); c.sx.push_back(x); c.sy.push_back(y); }

    void finalizeAll() {
        // 防御：形状记录里的填充/线索引可能超出样式表范围（错位数据），
        // 直接 push_back 到越界元素会造成堆破坏，这里做边界检查。
        auto pushFill = [&](int idx, Cur& c) {
            if (idx > 0 && idx <= static_cast<int>(shape->fillPaths.size())
                && c.started && c.sx.size() >= 2) {
                Contour ct; ct.xs = c.sx; ct.ys = c.sy;
                shape->fillPaths[idx - 1].push_back(ct);
            }
            c = Cur{};
        };
        auto pushLine = [&](int idx, Cur& c) {
            if (idx > 0 && idx <= static_cast<int>(shape->strokePaths.size())
                && c.started && c.sx.size() >= 2) {
                Contour ct; ct.xs = c.sx; ct.ys = c.sy;
                shape->strokePaths[idx - 1].push_back(ct);
            }
            c = Cur{};
        };
        pushFill(actFill0, c0);
        pushFill(actFill1, c1);
        pushLine(actLine, cl);
        actFill0 = actFill1 = actLine = 0;
    }

    void addLineTo(float x, float y) {
        if (actFill0) add(c0, x, y);
        if (actFill1) add(c1, x, y);
        if (actLine)  add(cl, x, y);
    }
    void moveTo(float x, float y) {
        finalizeAll();
        penX = x; penY = y;
        ensure(c0, x, y); ensure(c1, x, y); ensure(cl, x, y);
    }
    void lineTo(float x, float y) {
        addLineTo(x, y);
        penX = x; penY = y;
    }
    void curveTo(float cx, float cy, float ax, float ay) {
        // 二次贝塞尔，拍平
        const int N = 16;
        for (int i = 1; i <= N; ++i) {
            float t = i / static_cast<float>(N);
            float u = 1 - t;
            float x = u * u * penX + 2 * u * t * cx + t * t * ax;
            float y = u * u * penY + 2 * u * t * cy + t * t * ay;
            addLineTo(x, y);
        }
        penX = ax; penY = ay;
    }
};
}  // namespace

static FillStyle readFillStyle(ByteView& bv, bool alpha);

static void readGradient(ByteView& bv, FillStyle& fs, bool alpha, bool focal) {
    fs.gradientMatrix = readMatrix(bv);
    std::uint8_t hdr = bv.readUI8();
    int n = hdr & 0x0F;
    for (int i = 0; i < n; ++i) {
        GradientStop s;
        s.ratio = bv.readUI8();
        readColor(bv, alpha, s.color);
        fs.gradient.push_back(s);
    }
    // 焦点渐变（0x13）在渐变记录之后还有 FocalPoint：两个 RatioFraction = SB[15]/32768
    if (focal) {
        BitReader br(bv);
        fs.focalX = static_cast<float>(br.readSB(15)) / 32768.0f;
        fs.focalY = static_cast<float>(br.readSB(15)) / 32768.0f;
        br.finish();
    }
}
static void readBitmapFill(ByteView& bv, FillStyle& fs) {
    fs.bitmapId = bv.readUI16();
    fs.gradientMatrix = readMatrix(bv);
}
static FillStyle readFillStyle(ByteView& bv, bool alpha) {
    FillStyle fs;
    std::uint8_t t = bv.readUI8();
    switch (t) {
        case 0x00: fs.kind = FillKind::Solid; readColor(bv, alpha, fs.color); break;
        case 0x10: fs.kind = FillKind::LinearGradient; readGradient(bv, fs, alpha, false); break;
        case 0x12: fs.kind = FillKind::RadialGradient;  readGradient(bv, fs, alpha, false); break;
        case 0x13: fs.kind = FillKind::FocalGradient;   readGradient(bv, fs, alpha, true); break;
        case 0x40: case 0x43: fs.kind = FillKind::Bitmap; fs.tiled = true;  readBitmapFill(bv, fs); break;
        case 0x41: case 0x44: fs.kind = FillKind::Bitmap; fs.tiled = false; readBitmapFill(bv, fs); break;
        default: fs.kind = FillKind::Solid; break;  // 未知样式，跳过后续未知长度（v1 近似）
    }
    return fs;
}

static LineStyle readLineStyle(ByteView& bv, int shapeVer) {
    LineStyle ls;
    ls.width = static_cast<float>(bv.readUI16());  // twips
    if (shapeVer < 3) {
        readColor(bv, false, ls.color);
        return ls;
    }
    // DefineShape3/4 线样式（位打包）
    BitReader br(bv);
    br.readUB(2);  // StartCapStyle
    int join = static_cast<int>(br.readUB(2));
    int hasFill = static_cast<int>(br.readUB(1));
    br.readUB(1);  // NoVScale
    br.readUB(1);  // NoHScale
    br.readUB(1);  // PixelHinting
    br.readUB(5);  // reserved
    br.readUB(1);  // NoClose
    br.readUB(2);  // EndCapStyle
    if (join == 2) br.readUB(8);  // miter limit
    if (shapeVer >= 4) {
        int hasBlur = static_cast<int>(br.readUB(1));
        // 模糊字段是位字段，必须用 br 读取；混用 bv 直读会破坏缓冲位置
        if (hasBlur) { br.readUB(16); br.readUB(16); br.readUB(8); }
    }
    br.finish();  // 统一对齐到字节边界，再按字节读颜色/填充
    if (hasFill) {
        readFillStyle(bv, true);  // 消费字节（v1 不使用线填充）
    } else {
        readColor(bv, true, ls.color);
    }
    return ls;
}

static std::shared_ptr<Shape> decodeShape(ByteView& bv, int shapeVer) {
    auto shape = std::make_shared<Shape>();
    bool alpha = (shapeVer >= 3);

    { Rect rb = readRect(bv);
      shape->bounds.x0 = rb.xMin; shape->bounds.x1 = rb.xMax;
      shape->bounds.y0 = rb.yMin; shape->bounds.y1 = rb.yMax; }

    // 填充样式（防御：数量异常/溢出时提前结束，避免真实文件触发内存爆炸）
    int fc = bv.readUI8(); if (fc == 0xFF) fc = bv.readUI16();
    if (fc < 0 || fc > 4096 || bv.overflow()) return shape;
    shape->fillStyles.resize(fc);
    shape->fillPaths.resize(fc);
    for (int i = 0; i < fc; ++i) shape->fillStyles[i] = readFillStyle(bv, alpha);
    if (bv.overflow()) return shape;

    // 线样式
    int lc = bv.readUI8(); if (lc == 0xFF) lc = bv.readUI16();
    if (lc < 0 || lc > 4096 || bv.overflow()) return shape;
    shape->lineStyles.resize(lc);
    shape->strokePaths.resize(lc);
    for (int i = 0; i < lc; ++i) shape->lineStyles[i] = readLineStyle(bv, shapeVer);

    // 形状记录
    ShapeBuilder sb;
    sb.fills = &shape->fillStyles;
    sb.lines = &shape->lineStyles;
    sb.shape = shape;

    BitReader br(bv);
    int numFillBits = static_cast<int>(br.readUB(4));
    int numLineBits = static_cast<int>(br.readUB(4));

    bool done = false;
    int guard = 0;
    while (!done) {
        if (++guard > 100000) break;
        if (bv.overflow()) break;   // 字节流耗尽：停止解析该形状
        int typeFlag = static_cast<int>(br.readUB(1));
        if (typeFlag == 0) {
            int stateNew = static_cast<int>(br.readUB(1));
            int stateLine = static_cast<int>(br.readUB(1));
            int stateFill1 = static_cast<int>(br.readUB(1));
            int stateFill0 = static_cast<int>(br.readUB(1));
            int stateMove = static_cast<int>(br.readUB(1));
            if (!(stateNew || stateLine || stateFill1 || stateFill0 || stateMove)) {
                sb.finalizeAll();
                break;  // EndShapeRecord
            }
            if (stateMove) {
                int mb = static_cast<int>(br.readUB(5));
                float x = static_cast<float>(br.readSB(mb));
                float y = static_cast<float>(br.readSB(mb));
                sb.moveTo(x, y);
            } else {
                sb.finalizeAll();   // 继续当前笔位置（首个边会 ensure 起始点）
            }
            if (stateFill0) sb.actFill0 = static_cast<int>(br.readUB(numFillBits));
            if (stateFill1) sb.actFill1 = static_cast<int>(br.readUB(numFillBits));
            if (stateLine)  sb.actLine  = static_cast<int>(br.readUB(numLineBits));
            if (stateNew) {
                int nfc = bv.readUI8(); if (nfc == 0xFF) nfc = bv.readUI16();
                int old = static_cast<int>(shape->fillStyles.size());
                int nlc = bv.readUI8(); if (nlc == 0xFF) nlc = bv.readUI16();
                int oldl = static_cast<int>(shape->lineStyles.size());
                // 防御：样式总数设上限，避免错位导致的失控增长
                if (nfc < 0 || nfc > 1024 || nlc < 0 || nlc > 1024 ||
                    old + nfc > 4096 || oldl + nlc > 4096) { sb.finalizeAll(); break; }
                shape->fillStyles.resize(old + nfc);
                shape->fillPaths.resize(old + nfc);
                for (int i = 0; i < nfc; ++i) shape->fillStyles[old + i] = readFillStyle(bv, alpha);
                shape->lineStyles.resize(oldl + nlc);
                shape->strokePaths.resize(oldl + nlc);
                for (int i = 0; i < nlc; ++i) shape->lineStyles[oldl + i] = readLineStyle(bv, shapeVer);
                if (bv.overflow()) { sb.finalizeAll(); break; }
                numFillBits = neededBits(shape->fillStyles.size());
                numLineBits = neededBits(shape->lineStyles.size());
            }
        } else {
            int straight = static_cast<int>(br.readUB(1));
            int nbits = static_cast<int>(br.readUB(4)) + 2;
            if (straight) {
                int general = static_cast<int>(br.readUB(1));
                float dx = 0, dy = 0;
                if (general) { dx = static_cast<float>(br.readSB(nbits)); dy = static_cast<float>(br.readSB(nbits)); }
                else {
                    int vert = static_cast<int>(br.readUB(1));
                    if (vert) dy = static_cast<float>(br.readSB(nbits));
                    else      dx = static_cast<float>(br.readSB(nbits));
                }
                sb.lineTo(sb.penX + dx, sb.penY + dy);
            } else {
                float cx = static_cast<float>(br.readSB(nbits));
                float cy = static_cast<float>(br.readSB(nbits));
                float ax = static_cast<float>(br.readSB(nbits));
                float ay = static_cast<float>(br.readSB(nbits));
                sb.curveTo(sb.penX + cx, sb.penY + cy, sb.penX + ax, sb.penY + ay);
            }
        }
    }
    br.finish();
    return shape;
}

static int neededBits(std::size_t n) {
    if (n == 0) return 0;
    int b = 1;
    while (n > 1) { n >>= 1; ++b; }
    return b;
}

// DefineMorphShape(46) / DefineMorphShape2(84)：变形形状。
// 记录结构与 DefineShape 相同，但每条记录多一个 MorphFlag 位；填充/线样式只经
// StyleChangeRecord(stateNew) 内联定义（无独立样式表头）。
// 静态渲染取 t=0 的起始形状（end 记录跳过）。
static std::shared_ptr<Shape> decodeMorphShape(ByteView& bv, int shapeVer) {
    auto shape = std::make_shared<Shape>();
    { Rect rb = readRect(bv);  // start bounds
      shape->bounds.x0 = rb.xMin; shape->bounds.x1 = rb.xMax;
      shape->bounds.y0 = rb.yMin; shape->bounds.y1 = rb.yMax; }
    { Rect eb = readRect(bv); }  // end bounds（t=0 不用）
    if (shapeVer >= 2) { readRect(bv); readRect(bv); bv.readUI32(); }  // edge bounds + offset

    ShapeBuilder sb;
    sb.fills = &shape->fillStyles; sb.lines = &shape->lineStyles; sb.shape = shape;
    BitReader br(bv);
    int numFillBits = 0, numLineBits = 0;
    bool firstRec = true;
    int guard = 0;
    for (;;) {
        if (++guard > 100000 || bv.overflow()) break;
        int typeFlag = static_cast<int>(br.readUB(1));
        if (typeFlag == 0) {
            int stateNew   = static_cast<int>(br.readUB(1));
            int stateLine  = static_cast<int>(br.readUB(1));
            int stateFill1 = static_cast<int>(br.readUB(1));
            int stateFill0 = static_cast<int>(br.readUB(1));
            int stateMove  = static_cast<int>(br.readUB(1));
            br.readUB(1);  // MorphLineFlag（起始形状为 0）
            if (!(stateNew || stateLine || stateFill1 || stateFill0 || stateMove)) { sb.finalizeAll(); break; }
            if (stateMove) {
                int mb = static_cast<int>(br.readUB(5));
                float x = static_cast<float>(br.readSB(mb));
                float y = static_cast<float>(br.readSB(mb));
                sb.moveTo(x, y);
            } else if (!firstRec) {
                sb.finalizeAll();
            }
            if (stateFill0 && numFillBits > 0) sb.actFill0 = static_cast<int>(br.readUB(numFillBits));
            if (stateFill1 && numFillBits > 0) sb.actFill1 = static_cast<int>(br.readUB(numFillBits));
            if (stateLine  && numLineBits > 0) sb.actLine  = static_cast<int>(br.readUB(numLineBits));
            if (stateNew) {
                int nfc = bv.readUI8(); if (nfc == 0xFF) nfc = bv.readUI16();
                if (nfc < 0 || nfc > 1024) break;
                int old = static_cast<int>(shape->fillStyles.size());
                shape->fillStyles.resize(old + nfc); shape->fillPaths.resize(old + nfc);
                for (int i = 0; i < nfc; ++i) shape->fillStyles[old + i] = readFillStyle(bv, true);
                int nlc = bv.readUI8(); if (nlc == 0xFF) nlc = bv.readUI16();
                if (nlc < 0 || nlc > 1024) break;
                int oldl = static_cast<int>(shape->lineStyles.size());
                shape->lineStyles.resize(oldl + nlc); shape->strokePaths.resize(oldl + nlc);
                for (int i = 0; i < nlc; ++i) shape->lineStyles[oldl + i] = readLineStyle(bv, shapeVer);
                numFillBits = neededBits(shape->fillStyles.size());
                numLineBits = neededBits(shape->lineStyles.size());
            }
            firstRec = false;
        } else {
            int straight = static_cast<int>(br.readUB(1));
            int nbits    = static_cast<int>(br.readUB(4)) + 2;
            br.readUB(1);  // MorphShapeFlag（起始形状为 0）
            if (straight) {
                int general = static_cast<int>(br.readUB(1));
                float dx = 0, dy = 0;
                if (general) { dx = static_cast<float>(br.readSB(nbits)); dy = static_cast<float>(br.readSB(nbits)); }
                else { int vert = static_cast<int>(br.readUB(1)); if (vert) dy = static_cast<float>(br.readSB(nbits)); else dx = static_cast<float>(br.readSB(nbits)); }
                sb.lineTo(sb.penX + dx, sb.penY + dy);
            } else {
                float cx = static_cast<float>(br.readSB(nbits));
                float cy = static_cast<float>(br.readSB(nbits));
                float ax = static_cast<float>(br.readSB(nbits));
                float ay = static_cast<float>(br.readSB(nbits));
                sb.curveTo(sb.penX + cx, sb.penY + cy, sb.penX + ax, sb.penY + ay);
            }
        }
    }
    br.finish();
    return shape;
}

// 字形轮廓：字形内嵌于字体表，格式同 DefineShape 记录流但无 bounds、无填充/线数量表头，
// 样式只经 StyleChangeRecord(stateNew) 内联定义。
static void parseGlyphShape(ByteView& bv, std::shared_ptr<Shape>& shape, int shapeVer) {
    ShapeBuilder sb;
    sb.fills = &shape->fillStyles; sb.lines = &shape->lineStyles; sb.shape = shape;
    BitReader br(bv);
    int numFillBits = 0, numLineBits = 0;
    bool firstRec = true;
    int guard = 0;
    for (;;) {
        if (++guard > 20000 || bv.overflow()) break;
        int typeFlag = static_cast<int>(br.readUB(1));
        if (typeFlag == 0) {
            int stateNew   = static_cast<int>(br.readUB(1));
            int stateLine  = static_cast<int>(br.readUB(1));
            int stateFill1 = static_cast<int>(br.readUB(1));
            int stateFill0 = static_cast<int>(br.readUB(1));
            int stateMove  = static_cast<int>(br.readUB(1));
            if (!(stateNew || stateLine || stateFill1 || stateFill0 || stateMove)) { sb.finalizeAll(); break; }
            if (stateMove) {
                int mb = static_cast<int>(br.readUB(5));
                float x = static_cast<float>(br.readSB(mb));
                float y = static_cast<float>(br.readSB(mb));
                sb.moveTo(x, y);
            } else if (!firstRec) {
                sb.finalizeAll();
            }
            if (stateFill0 && numFillBits > 0) sb.actFill0 = static_cast<int>(br.readUB(numFillBits));
            if (stateFill1 && numFillBits > 0) sb.actFill1 = static_cast<int>(br.readUB(numFillBits));
            if (stateLine  && numLineBits > 0) sb.actLine  = static_cast<int>(br.readUB(numLineBits));
            if (stateNew) {
                int nfc = bv.readUI8(); if (nfc == 0xFF) nfc = bv.readUI16();
                if (nfc < 0 || nfc > 256) break;
                int old = static_cast<int>(shape->fillStyles.size());
                shape->fillStyles.resize(old + nfc); shape->fillPaths.resize(old + nfc);
                for (int i = 0; i < nfc; ++i) shape->fillStyles[old + i] = readFillStyle(bv, true);
                int nlc = bv.readUI8(); if (nlc == 0xFF) nlc = bv.readUI16();
                if (nlc < 0 || nlc > 256) break;
                int oldl = static_cast<int>(shape->lineStyles.size());
                shape->lineStyles.resize(oldl + nlc); shape->strokePaths.resize(oldl + nlc);
                for (int i = 0; i < nlc; ++i) shape->lineStyles[oldl + i] = readLineStyle(bv, shapeVer);
                numFillBits = neededBits(shape->fillStyles.size());
                numLineBits = neededBits(shape->lineStyles.size());
            }
            firstRec = false;
        } else {
            int straight = static_cast<int>(br.readUB(1));
            int nbits    = static_cast<int>(br.readUB(4)) + 2;
            if (straight) {
                int general = static_cast<int>(br.readUB(1));
                float dx = 0, dy = 0;
                if (general) { dx = static_cast<float>(br.readSB(nbits)); dy = static_cast<float>(br.readSB(nbits)); }
                else { int vert = static_cast<int>(br.readUB(1)); if (vert) dy = static_cast<float>(br.readSB(nbits)); else dx = static_cast<float>(br.readSB(nbits)); }
                sb.lineTo(sb.penX + dx, sb.penY + dy);
            } else {
                float cx = static_cast<float>(br.readSB(nbits));
                float cy = static_cast<float>(br.readSB(nbits));
                float ax = static_cast<float>(br.readSB(nbits));
                float ay = static_cast<float>(br.readSB(nbits));
                sb.curveTo(sb.penX + cx, sb.penY + cy, sb.penX + ax, sb.penY + ay);
            }
        }
    }
    br.finish();
}

// DefineFont2(48) / DefineFont3(75)。注意：FontID 已由调用方读出，本函数从 flags 开始。
static std::shared_ptr<FontGlyphs> decodeFont(ByteView& bv) {
    auto fg = std::make_shared<FontGlyphs>();
    bv.readUI8();               // flags
    int nameLen = bv.readUI8();
    bv.skip(static_cast<std::size_t>(nameLen));
    int numGlyphs = bv.readUI16();
    if (numGlyphs <= 0 || numGlyphs > 65535 || bv.overflow()) return fg;
    std::vector<std::uint32_t> offsets(static_cast<std::size_t>(numGlyphs));
    for (int i = 0; i < numGlyphs; ++i) offsets[i] = bv.readUI32();
    bv.readUI32();              // CodeTableOffset
    // 字形表基址：OffsetTable[0] 相对于其后紧邻的字形表起始处
    std::size_t tableBase = bv.pos();
    fg->shapes.resize(static_cast<std::size_t>(numGlyphs));
    fg->advances.assign(static_cast<std::size_t>(numGlyphs), 0);
    for (int i = 0; i < numGlyphs; ++i) {
        if (offsets[i] == 0) continue;
        std::size_t gpos = tableBase + offsets[i];
        if (gpos >= bv.size()) continue;
        ByteView gv(bv.base(), bv.size()); gv.seek(gpos);
        auto shp = std::make_shared<Shape>();
        parseGlyphShape(gv, shp, 3);
        fg->shapes[static_cast<std::size_t>(i)] = shp;
    }
    return fg;
}

// DefineText(11) / DefineText2(33)。注意：CharacterID 已由调用方读出，本函数从 TextBounds 开始。
static std::shared_ptr<TextDef> decodeText(ByteView& bv, bool alpha) {
    auto td = std::make_shared<TextDef>();
    readRect(bv);               // TextBounds
    td->textMatrix = readMatrix(bv);
    int glyphBits = bv.readUI8();
    int advanceBits = bv.readUI8();
    if (glyphBits <= 0 || glyphBits > 32 || advanceBits < 0 || advanceBits > 32) return td;
    std::uint16_t curFont = 0;
    Color curColor(0, 0, 0);
    int penX = 0, penY = 0;
    BitReader br(bv);
    int guard = 0;
    for (;;) {
        if (++guard > 65535 || bv.overflow()) break;
        int recordType = static_cast<int>(br.readUB(1));
        br.readUB(3);                                     // reserved
        int hasFont  = static_cast<int>(br.readUB(1));
        int hasColor = static_cast<int>(br.readUB(1));
        int hasY     = static_cast<int>(br.readUB(1));
        int hasX     = static_cast<int>(br.readUB(1));
        if (recordType == 1) { br.finish(); break; }     // 结束记录
        if (hasFont)  curFont = static_cast<std::uint16_t>(br.readUB(16));
        if (hasColor) readColor(bv, alpha, curColor);
        if (hasX) penX = br.readSB(16);
        if (hasY) penY = br.readSB(16);
        if (hasFont) {
            int glyphCount = bv.readUI8();
            for (int g = 0; g < glyphCount; ++g) {
                TextGlyph tg;
                tg.fontId = curFont;
                tg.glyphIndex = static_cast<std::uint16_t>(br.readUB(glyphBits));
                tg.advance = (advanceBits > 0) ? br.readSB(advanceBits) : 0;
                tg.color = curColor;
                tg.x = penX; tg.y = penY;
                td->glyphs.push_back(tg);
                penX += tg.advance;
            }
        }
    }
    br.finish();
    return td;
}

// DefineButton2(34)：解码为单帧精灵（取 Up 状态），用于静态渲染可见内容。
static std::shared_ptr<Character> decodeButton2(ByteView& bv,
                                                std::map<std::uint16_t, std::shared_ptr<Character>>& chars) {
    auto sp = std::make_shared<Character>();
    sp->kind = Character::Kind::Sprite;
    bv.readUI16();                 // CharacterID
    std::uint16_t trackButton = bv.readUI16();
    bv.readUI16();                 // ButtonTrackDepth
    bv.readUI8();                  // flags
    bv.readUI32();                 // ButtonActionOffset
    std::map<std::uint16_t, DisplayItem> upList;
    int guard = 0;
    while (bv.pos() + 2 <= bv.size()) {
        if (++guard > 4096 || bv.overflow()) break;
        std::uint8_t recFlags = bv.readUI8();
        std::uint16_t cid = bv.readUI16();
        std::uint16_t pdepth = bv.readUI16();
        bool isEnd = (recFlags == 0 && cid == 0 && pdepth == 0);
        if (cid == 0) { bv.readUI8(); bv.readUI32(); break; }  // Button2ActionRecord / 结束
        DisplayItem it; it.depth = pdepth; it.matrix = Matrix{};
        { auto ic = chars.find(cid); it.character = (ic != chars.end()) ? ic->second : nullptr; }
        if (recFlags & 0x10) {  // StateUp
            it.matrix = readMatrix(bv);
            if (trackButton == 0) {
                it.cxform = readCXForm(bv, true);
                // FilterList
                std::uint8_t nf = bv.readUI8();
                for (int k = 0; k < nf && bv.pos() + 3 <= bv.size(); ++k) {
                    bv.readUI8(); std::uint16_t fsz = bv.readUI16(); bv.skip(fsz);
                }
            }
            upList[pdepth] = it;
        } else {
            // 非 Up 状态：跳过其矩阵/颜色/滤镜
            readMatrix(bv);
            if (trackButton == 0) {
                readCXForm(bv, true);
                std::uint8_t nf = bv.readUI8();
                for (int k = 0; k < nf && bv.pos() + 3 <= bv.size(); ++k) {
                    bv.readUI8(); std::uint16_t fsz = bv.readUI16(); bv.skip(fsz);
                }
            }
        }
        (void)isEnd;
        if (bv.overflow()) break;
    }
    Frame fr;
    for (auto& kv : upList) fr.items.push_back(kv.second);
    std::sort(fr.items.begin(), fr.items.end(),
              [](const DisplayItem& a, const DisplayItem& b) { return a.depth < b.depth; });
    sp->spriteFrames.push_back(std::move(fr));
    return sp;
}

// ---------- 主解码 ----------
namespace {
struct Ctx {
    Movie movie;
    std::map<std::uint16_t, std::shared_ptr<Character>> characters;
    std::vector<std::uint8_t> jpegTables;
    std::map<std::uint16_t, DisplayItem> displayList;
    bool hasShowFrame = false;
    int curFrame = 0;                                   // 当前时间轴帧号
    std::map<std::uint16_t, std::shared_ptr<Character>> placedChar;  // depth -> 上次放置的角色

    void snapshotFrame() {
        Frame f;
        for (auto& kv : displayList) {
            DisplayItem it = kv.second;
            // 记录该 depth 上此角色的放置帧（精灵据此对齐内部帧）
            if (placedChar.find(it.depth) == placedChar.end() || placedChar[it.depth] != it.character) {
                placedChar[it.depth] = it.character;
                it.birthFrame = curFrame;
            }
            f.items.push_back(it);
        }
        std::sort(f.items.begin(), f.items.end(),
                  [](const DisplayItem& a, const DisplayItem& b) { return a.depth < b.depth; });
        movie.frames.push_back(std::move(f));
        ++curFrame;
    }
};
}  // namespace

bool decodeMovie(const std::uint8_t* data, std::size_t size, Movie& out, std::string& err) {
    if (size < 8) { err = "file too small"; return false; }
    // 解压成 FWS 缓冲
    std::vector<std::uint8_t> full;
    char sig0 = static_cast<char>(data[0]);
    if (sig0 == 'C') {
        if (!inflateZlib(data + 8, size - 8, full)) { err = "zlib inflate failed"; return false; }
    } else if (sig0 == 'Z') {
        if (!decodeLzmaAlone(data + 8, size - 8, full)) { err = "lzma failed"; return false; }
    } else if (sig0 == 'F') {
        full.assign(data + 8, data + size);   // 未压缩：body = 跳过 8 字节头
    } else { err = "bad signature"; return false; }
    // 还原 8 字节头
    std::vector<std::uint8_t> buf;
    buf.reserve(8 + full.size());
    buf.push_back('F'); buf.push_back('W'); buf.push_back('S'); buf.push_back(data[3]);
    for (int i = 4; i < 8; ++i) buf.push_back(static_cast<std::uint8_t>(data[i]));
    buf.insert(buf.end(), full.begin(), full.end());

    const std::uint8_t* base = buf.data();
    std::size_t avail = buf.size();

    // body 头：RECT + 帧率 + 帧数
    {
        ByteView bv(base, avail); bv.seek(8);
        Rect rb = readRect(bv);
        out.frameSize.x0 = rb.xMin; out.frameSize.x1 = rb.xMax;
        out.frameSize.y0 = rb.yMin; out.frameSize.y1 = rb.yMax;
        out.frameRate = bv.readUI16() / 256.0f;
        out.frameCount = bv.readUI16();
    }

    Ctx ctx;
    ctx.movie.backgroundColor = Color(255, 255, 255);
    ctx.movie.frameSize = out.frameSize;
    ctx.movie.frameRate = out.frameRate;
    ctx.movie.frameCount = out.frameCount;

    std::size_t pos = 0;
    // 找到标签起点：复用 body 头已消费，重新定位
    {
        ByteView bv(base, avail); bv.seek(8);
        readRect(bv);
        bv.readUI16();   // frameRate
        bv.readUI16();   // frameCount
        pos = bv.pos();
    }

    for (;;) {
        if (pos + 2 > avail) break;
        std::uint16_t v = static_cast<std::uint16_t>(base[pos] | (base[pos + 1] << 8));
        std::uint32_t len = v & 0x3F;
        std::size_t cursor = pos + 2;
        if (len == 63) {
            if (cursor + 4 > avail) break;
            len = static_cast<std::uint32_t>(base[cursor]) | (static_cast<std::uint32_t>(base[cursor+1])<<8)
                | (static_cast<std::uint32_t>(base[cursor+2])<<16) | (static_cast<std::uint32_t>(base[cursor+3])<<24);
            cursor += 4;
        }
        if (cursor + len > avail) break;
        std::uint16_t code = static_cast<std::uint16_t>(v >> 6);
        std::size_t doff = cursor;
        ByteView bv(base, avail); bv.seek(doff);

        switch (code) {
            case 9: {  // SetBackgroundColor
                readColor(bv, false, ctx.movie.backgroundColor);
                break;
            }
            case 39: {  // DefineSprite：内含独立时间轴（子显示列表 + 子帧）
                std::uint16_t sid = bv.readUI16();
                bv.readUI16();  // 精灵帧数（渲染时按需取帧）
                auto sp = std::make_shared<Character>();
                sp->kind = Character::Kind::Sprite;
                // 解析精灵内部标签流，直到 End(0) 或到达精灵数据末尾
                std::map<std::uint16_t, DisplayItem> sub;
                std::map<std::uint16_t, std::shared_ptr<Character>> subPlaced;
                int subFrame = 0;
                std::size_t subEnd = doff + len;
                while (bv.pos() + 2 <= subEnd) {
                    std::uint16_t tv = bv.readUI16();
                    std::uint32_t tlen = tv & 0x3F;
                    if (tlen == 63) tlen = bv.readUI32();
                    std::size_t tStart = bv.pos();
                    std::size_t tEnd = tStart + tlen;
                    std::uint16_t tc = static_cast<std::uint16_t>(tv >> 6);
                    if (tEnd > subEnd) break;
                    if (tc == 2 || tc == 22 || tc == 32 || tc == 83) {  // 嵌套定义形状
                        int ver = (tc == 2) ? 1 : (tc == 22) ? 2 : (tc == 32) ? 3 : 4;
                        std::uint16_t cid = bv.readUI16();
                        auto shp = decodeShape(bv, ver);
                        auto ch = std::make_shared<Character>();
                        ch->kind = Character::Kind::Shape;
                        ch->shape = shp;
                        ctx.characters[cid] = ch;
                    } else if (tc == 4) {  // PlaceObject
                        std::uint16_t cid = bv.readUI16();
                        std::uint16_t depth = bv.readUI16();
                        DisplayItem it; it.depth = depth; it.matrix = Matrix{};
                        auto ic = ctx.characters.find(cid);
                        it.character = (ic != ctx.characters.end()) ? ic->second : nullptr;
                        sub[depth] = it;
                    } else if (tc == 26) {  // PlaceObject2
                        int f = bv.readUI8();
                        DisplayItem it; it.depth = bv.readUI16();
                        if (f & 0x02) { std::uint16_t cid = bv.readUI16(); auto ic = ctx.characters.find(cid); it.character = (ic != ctx.characters.end()) ? ic->second : nullptr; }
                        if (f & 0x04) it.matrix = readMatrix(bv);
                        if (f & 0x08) it.cxform = readCXForm(bv, true);
                        if (f & 0x10) bv.readUI16();
                        if (f & 0x20) { while (bv.pos() < tEnd && bv.readUI8() != 0) {} }
                        if (f & 0x40) it.clipDepth = bv.readUI16();
                        if (f & 0x80) { std::uint16_t sz = bv.readUI16(); bv.skip(sz); }
                        sub[it.depth] = it;
                    } else if (tc == 70) {  // PlaceObject3
                        int f = bv.readUI8(); int f2 = bv.readUI8();
                        DisplayItem it; it.depth = bv.readUI16();
                        if (f2 & 0x04) { while (bv.pos() < tEnd && bv.readUI8() != 0) {} }
                        if (f2 & 0x08) { std::uint8_t nf = bv.readUI8(); for (int k = 0; k < nf && bv.pos() < tEnd; ++k) { bv.readUI8(); std::uint16_t fsz = bv.readUI16(); bv.skip(fsz); } }
                        if (f2 & 0x10) bv.readUI8();
                        if (f2 & 0x20) bv.readUI8();
                        if (f & 0x02) { std::uint16_t cid = bv.readUI16(); auto ic = ctx.characters.find(cid); it.character = (ic != ctx.characters.end()) ? ic->second : nullptr; }
                        if (f & 0x04) it.matrix = readMatrix(bv);
                        if (f & 0x08) it.cxform = readCXForm(bv, true);
                        if (f & 0x10) bv.readUI16();
                        if (f & 0x20) { while (bv.pos() < tEnd && bv.readUI8() != 0) {} }
                        if (f & 0x40) it.clipDepth = bv.readUI16();
                        if (f & 0x80) { std::uint16_t sz = bv.readUI16(); bv.skip(sz); }
                        sub[it.depth] = it;
                    } else if (tc == 5) { bv.readUI16(); std::uint16_t d = bv.readUI16(); sub.erase(d); }
                    else if (tc == 28) { std::uint16_t d = bv.readUI16(); sub.erase(d); }
                    else if (tc == 1) {  // ShowFrame：快照精灵帧（记录各 depth 放置帧）
                        Frame fr;
                        for (auto& kv : sub) {
                            DisplayItem it = kv.second;
                            if (subPlaced.find(it.depth) == subPlaced.end() || subPlaced[it.depth] != it.character) {
                                subPlaced[it.depth] = it.character;
                                it.birthFrame = subFrame;
                            }
                            fr.items.push_back(it);
                        }
                        std::sort(fr.items.begin(), fr.items.end(),
                                  [](const DisplayItem& a, const DisplayItem& b) { return a.depth < b.depth; });
                        sp->spriteFrames.push_back(std::move(fr));
                        ++subFrame;
                    }
                    else if (tc == 0) { bv.seek(tEnd); break; }  // End of sprite
                    bv.seek(tEnd);
                }
                ctx.characters[sid] = sp;
                break;
            }
            case 2: case 22: case 32: case 83: {  // DefineShape 1/2/3/4
                int ver = (code == 2) ? 1 : (code == 22) ? 2 : (code == 32) ? 3 : 4;
                std::uint16_t id = bv.readUI16();
                auto shp = decodeShape(bv, ver);
                auto ch = std::make_shared<Character>();
                ch->kind = Character::Kind::Shape;
                ch->shape = shp;
                ctx.characters[id] = ch;
                break;
            }
            case 76: case 77: {  // SymbolClass：类名 <-> 角色 id 映射
                std::uint16_t num = bv.readUI16();
                for (std::uint16_t k = 0; k < num; ++k) {
                    std::uint16_t cid = bv.readUI16();
                    std::string nm;
                    int c;
                    while ((c = bv.readUI8()) != 0 && c != -1) nm.push_back(static_cast<char>(c));
                    // 类名可能用 '.' 或 '::' 分隔，统一替换 '::' 为 '.'
                    std::string dot = nm;
                    size_t pos;
                    while ((pos = dot.find("::")) != std::string::npos) dot.replace(pos, 2, ".");
                    ctx.movie.symbolClasses[dot] = cid;
                }
                break;
            }
            case 82: {  // DoABC：AVM2 字节码
                bv.readUI32();          // flags (lazy)
                // Name: null-terminated string
                while (bv.pos() < doff + len && bv.readUI8() != 0) {}
                std::size_t abcStart = bv.pos();
                if (abcStart < doff + len) {
                    abc::AbcFile f;
                    std::string aerr;
                    if (abc::parseAbc(base + abcStart, (doff + len) - abcStart, f, aerr))
                        ctx.movie.abcFiles.push_back(std::move(f));
                }
                break;
            }
            case 34: {  // DefineButton2（取 Up 状态作静态渲染）
                std::uint16_t id = bv.readUI16();
                auto btn = decodeButton2(bv, ctx.characters);
                ctx.characters[id] = btn;
                break;
            }
            case 48: case 75: {  // DefineFont2 / DefineFont3
                ByteView fv(base, avail); fv.seek(doff);
                std::uint16_t id = fv.readUI16();
                auto fg = decodeFont(fv);
                auto ch = std::make_shared<Character>();
                ch->kind = Character::Kind::Font;
                ch->font = fg;
                ctx.characters[id] = ch;
                break;
            }
            case 11: case 33: {  // DefineText / DefineText2
                bool alpha = (code == 33);
                std::uint16_t id = bv.readUI16();
                auto td = decodeText(bv, alpha);
                auto ch = std::make_shared<Character>();
                ch->kind = Character::Kind::Text;
                ch->text = td;
                ctx.characters[id] = ch;
                break;
            }
            case 46: case 84: {  // DefineMorphShape / DefineMorphShape2
                int ver = (code == 46) ? 1 : 2;
                std::uint16_t id = bv.readUI16();
                auto shp = decodeMorphShape(bv, ver);
                auto ch = std::make_shared<Character>();
                ch->kind = Character::Kind::Shape;   // t=0 起始形状按普通形状渲染
                ch->shape = shp;
                ctx.characters[id] = ch;
                break;
            }
            case 6: {  // DefineBits (JPEG, 依赖 JPEGTables)
                std::uint16_t id = bv.readUI16();
                int rest = static_cast<int>(len) - 2;
                auto ch = std::make_shared<Character>();
                ch->kind = Character::Kind::Bitmap;
                ch->bitmap = std::make_shared<Bitmap>();
                std::vector<std::uint8_t> blob;
                if (!ctx.jpegTables.empty()) {
                    blob.insert(blob.end(), ctx.jpegTables.begin(), ctx.jpegTables.end());
                    blob.insert(blob.end(), base + doff + 2, base + doff + 2 + rest);
                } else {
                    blob.assign(base + doff + 2, base + doff + 2 + rest);
                }
                decodeJpeg(blob.data(), static_cast<int>(blob.size()), *ch->bitmap);
                ctx.characters[id] = ch;
                break;
            }
            case 8: {  // JPEGTables
                ctx.jpegTables.assign(base + doff, base + doff + len);
                break;
            }
            case 21: {  // DefineBitsJPEG2
                std::uint16_t id = bv.readUI16();
                int rest = static_cast<int>(len) - 2;
                auto ch = std::make_shared<Character>();
                ch->kind = Character::Kind::Bitmap;
                ch->bitmap = std::make_shared<Bitmap>();
                decodeJpeg(base + doff + 2, rest, *ch->bitmap);
                ctx.characters[id] = ch;
                break;
            }
            case 35: {  // DefineBitsJPEG3 (+alpha)
                std::uint16_t id = bv.readUI16();
                std::uint32_t jpegSize = bv.readUI32();
                auto ch = std::make_shared<Character>();
                ch->kind = Character::Kind::Bitmap;
                ch->bitmap = std::make_shared<Bitmap>();
                decodeJpeg(base + bv.pos(), static_cast<int>(jpegSize), *ch->bitmap);
                const std::uint8_t* alphaPtr = base + bv.pos() + jpegSize;
                std::size_t alphaLen = (doff + len) - (bv.pos() + jpegSize);
                std::vector<std::uint8_t> alpha;
                inflateZlib(alphaPtr, alphaLen, alpha);
                applyAlpha(*ch->bitmap, alpha);
                ctx.characters[id] = ch;
                break;
            }
            case 20: case 36: {  // DefineBitsLossless / Lossless2
                bool withAlpha = (code == 36);
                std::uint16_t id = bv.readUI16();
                std::uint8_t fmt = bv.readUI8();
                std::uint16_t w = bv.readUI16();
                std::uint16_t h = bv.readUI16();
                std::vector<std::uint8_t> raw;
                inflateZlib(base + bv.pos(), (doff + len) - bv.pos(), raw);
                auto ch = std::make_shared<Character>();
                ch->kind = Character::Kind::Bitmap;
                ch->bitmap = std::make_shared<Bitmap>();
                ch->bitmap->width = w; ch->bitmap->height = h;
                ch->bitmap->rgba.resize(static_cast<std::size_t>(w) * h * 4, 0);
                decodeLossless(fmt, withAlpha, w, h, raw, *ch->bitmap);
                ctx.characters[id] = ch;
                break;
            }
            case 4: {  // PlaceObject
                std::uint16_t cid = bv.readUI16();
                std::uint16_t depth = bv.readUI16();
                DisplayItem it;
                it.depth = depth;
                auto itc = ctx.characters.find(cid);
                it.character = (itc != ctx.characters.end()) ? itc->second : nullptr;
                it.matrix = Matrix{};
                ctx.displayList[depth] = it;
                break;
            }
            case 26: {  // PlaceObject2
                int f = bv.readUI8();
                DisplayItem it;
                it.depth = bv.readUI16();
                if (f & 0x02) {
                    std::uint16_t cid = bv.readUI16();
                    auto itc = ctx.characters.find(cid);
                    it.character = (itc != ctx.characters.end()) ? itc->second : nullptr;
                }
                if (f & 0x04) it.matrix = readMatrix(bv);
                if (f & 0x08) it.cxform = readCXForm(bv, true);
                if (f & 0x10) bv.readUI16();                       // ratio
                if (f & 0x20) { while (bv.readUI8() != 0) {} }     // name
                if (f & 0x40) it.clipDepth = bv.readUI16();
                if (f & 0x80) { std::uint16_t sz = bv.readUI16(); bv.skip(sz); }  // clip actions
                ctx.displayList[it.depth] = it;
                break;
            }
            case 70: {  // PlaceObject3
                int f = bv.readUI8();
                int f2 = bv.readUI8();
                DisplayItem it;
                it.depth = bv.readUI16();
                if (f2 & 0x04) { while (bv.readUI8() != 0) {} }    // className
                if (f2 & 0x08) {                                   // filter list
                    std::uint8_t nf = bv.readUI8();
                    for (int k = 0; k < nf; ++k) { bv.readUI8(); std::uint16_t fsz = bv.readUI16(); bv.skip(fsz); }
                }
                if (f2 & 0x10) bv.readUI8();                       // blend mode
                if (f2 & 0x20) bv.readUI8();                       // cacheAsBitmap
                if (f & 0x02) {
                    std::uint16_t cid = bv.readUI16();
                    auto itc = ctx.characters.find(cid);
                    it.character = (itc != ctx.characters.end()) ? itc->second : nullptr;
                }
                if (f & 0x04) it.matrix = readMatrix(bv);
                if (f & 0x08) it.cxform = readCXForm(bv, true);
                if (f & 0x10) bv.readUI16();                       // ratio
                if (f & 0x20) { while (bv.readUI8() != 0) {} }     // name
                if (f & 0x40) it.clipDepth = bv.readUI16();
                if (f & 0x80) { std::uint16_t sz = bv.readUI16(); bv.skip(sz); }  // clip actions
                ctx.displayList[it.depth] = it;
                break;
            }
            case 5: case 28: {  // RemoveObject / RemoveObject2
                if (code == 5) { bv.readUI16(); std::uint16_t depth = bv.readUI16(); ctx.displayList.erase(depth); }
                else { std::uint16_t depth = bv.readUI16(); ctx.displayList.erase(depth); }
                break;
            }
            case 1: {  // ShowFrame
                ctx.hasShowFrame = true;
                ctx.snapshotFrame();
                break;
            }
            case 0: {  // End
                if (!ctx.hasShowFrame) ctx.snapshotFrame();  // 无 ShowFrame 时补一帧
                goto done;
            }
            default:
                break;  // 其它标签暂忽略
        }
        pos = cursor + len;
    }
done:
    if (ctx.movie.frames.empty()) ctx.snapshotFrame();
    ctx.movie.characters = std::move(ctx.characters);
    out = std::move(ctx.movie);
    return true;
}

// Lossless 位图：format 3(调色板) / 4(15/16bit) / 5(24/32bit)
static void decodeLossless(std::uint8_t fmt, bool withAlpha, std::uint16_t w, std::uint16_t h,
                          const std::vector<std::uint8_t>& raw, Bitmap& bmp) {
    std::size_t n = static_cast<std::size_t>(w) * h;
    std::size_t p = 0;
    if (fmt == 3) {
        int entries = raw[p++] + 1;
        int ch = withAlpha ? 4 : 3;
        std::vector<Color> pal(entries);
        for (int i = 0; i < entries; ++i) {
            pal[i].r = raw[p++]; pal[i].g = raw[p++]; pal[i].b = raw[p++];
            pal[i].a = withAlpha ? raw[p++] : 0xFF;
        }
        for (std::size_t i = 0; i < n; ++i) {
            std::uint8_t idx = raw[p++];
            if (idx >= entries) idx = 0;
            bmp.rgba[i*4+0] = pal[idx].r; bmp.rgba[i*4+1] = pal[idx].g;
            bmp.rgba[i*4+2] = pal[idx].b; bmp.rgba[i*4+3] = pal[idx].a;
        }
    } else if (fmt == 4) {
        int bpp = withAlpha ? 4 : 2;  // 4444 or 555
        for (std::size_t i = 0; i < n; ++i) {
            if (withAlpha) {
                std::uint16_t v = static_cast<std::uint16_t>(raw[p] | (raw[p+1]<<8)); p += 2;
                bmp.rgba[i*4+0] = static_cast<std::uint8_t>((v & 0x0F00) >> 4);
                bmp.rgba[i*4+1] = static_cast<std::uint8_t>((v & 0x00F0) << 0);
                bmp.rgba[i*4+2] = static_cast<std::uint8_t>((v & 0x000F) << 4);
                bmp.rgba[i*4+3] = static_cast<std::uint8_t>((v & 0xF000) >> 8);
            } else {
                std::uint16_t v = static_cast<std::uint16_t>(raw[p] | (raw[p+1]<<8)); p += 2;
                bmp.rgba[i*4+0] = static_cast<std::uint8_t>((v & 0x7C00) >> 7);
                bmp.rgba[i*4+1] = static_cast<std::uint8_t>((v & 0x03E0) >> 2);
                bmp.rgba[i*4+2] = static_cast<std::uint8_t>((v & 0x001F) << 3);
                bmp.rgba[i*4+3] = 0xFF;
            }
        }
    } else if (fmt == 5) {
        int bpp = withAlpha ? 4 : 3;
        for (std::size_t i = 0; i < n; ++i) {
            bmp.rgba[i*4+0] = raw[p++];
            bmp.rgba[i*4+1] = raw[p++];
            bmp.rgba[i*4+2] = raw[p++];
            bmp.rgba[i*4+3] = withAlpha ? raw[p++] : 0xFF;
        }
    }
}

}  // namespace swf
