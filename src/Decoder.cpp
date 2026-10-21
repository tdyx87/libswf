// SWF 解码器：把字节流还原成"字典 + 时间轴帧"，供软件光栅器渲染。
// 覆盖：DefineShape(1-4) / DefineBits(JPEG) / DefineBitsLossless(2) / PlaceObject(1/2/3)
//       / RemoveObject(2) / ShowFrame / SetBackgroundColor / JPEGTables。
#include "swf/Decoder.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

#include "swf/BitReader.hpp"
#include "swf/ByteView.hpp"
#include "swf/Decompress.hpp"
#include "swf/FFmpeg.hpp"   // 跨平台视频解码（H.263/VP6/AVC）

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
        // SWF MATRIX 的两个旋转分量在文件里顺序为 RotateSkew0、RotateSkew1，
        // 但变换公式为  x' = ScaleX*x + RotateSkew1*y 、 y' = RotateSkew0*x + ScaleY*y，
        // 即矩阵布局 [[ScaleX, RotateSkew1],[RotateSkew0, ScaleY]] 与存储顺序**相反**。
        // 这里按布局存放（r0 = RotateSkew1、r1 = RotateSkew0），使 apply()/post() 自洽。
        float skew0 = br.readSB(nr);   // 文件第一个 = RotateSkew0
        float skew1 = br.readSB(nr);   // 文件第二个 = RotateSkew1
        m.r0 = skew1 / 65536.0f; m.r1 = skew0 / 65536.0f; total += 2 * nr;
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
        // CXForm 的**加**项是普通整数（直接加到颜色分量上），**不做** 8.8 定点缩放；
        // 只有乘项才是 8.8 定点（256 == 1.0）。此前两者都除以 256，会把加项抹成 0，
        // 导致依赖 ColorTransform 调色的对象整块错色（swf.swf 关卡顶栏 cid172：
        // 填充色 (23,37,94) 期望经 +(255,171,-42) 变成金色 (255,208,52)，却被渲染成深海军蓝）。
        c.hasAdd = true;
        c.ar = static_cast<float>(br.readSB(n)); total += n;
        c.ag = static_cast<float>(br.readSB(n)); total += n;
        c.ab = static_cast<float>(br.readSB(n)); total += n;
        c.aa = withAlpha ? static_cast<float>(br.readSB(n)) : 0.0f; total += (withAlpha ? n : 0);
    }
    bv.seek(start + (total + 7) / 8);
    return c;
}

static void readColor(ByteView& bv, bool alpha, Color& c) {
    c.r = bv.readUI8(); c.g = bv.readUI8(); c.b = bv.readUI8();
    c.a = alpha ? bv.readUI8() : 0xFF;
}

// ---------- 交互解析辅助（AVM1 播放需要） ----------
static std::string readStringZ(ByteView& bv, std::size_t limit) {
    std::string s;
    while (bv.pos() < limit && !bv.overflow()) {
        int c = bv.readUI8();
        if (c == 0 || c == -1) break;
        s.push_back(static_cast<char>(c));
    }
    return s;
}

// 反汇编一段以 End(0x00) 结尾的 AVM1 动作序列（ClipActions / ButtonActions 用）。
static avm1::Script readAvm1UntilEnd(ByteView& bv, std::size_t start, std::size_t limit) {
    if (start >= limit) return avm1::Script{};
    const std::uint8_t* base = bv.base();
    std::size_t p = start;
    std::size_t end = start;
    while (p < limit) {
        std::uint8_t c = base[p];
        ++p;
        if (c == 0x00) { end = p; break; }        // End：序列结束
        if (c & 0x80) {                           // 长动作：跳过 UI16 长度
            if (p + 2 > limit) { end = limit; break; }
            std::uint16_t ln = static_cast<std::uint16_t>(base[p] | (base[p + 1] << 8));
            p += 2 + ln;
            if (p > limit) { end = limit; break; }
        }
        end = p;
    }
    if (end <= start) return avm1::Script{};
    return avm1::disassemble(base + start, end - start);
}

// ClipActions（PlaceObject2/3 flags bit7）：事件位 -> 脚本。
// 布局（SWF 规范 CLIPACTIONS / CLIPACTIONRECORD，已用 swf.swf 实测逐条落到 tag 末尾验证）：
//   CLIPACTIONS  = Reserved UI16(0) + AllEventFlags CLIPEVENTFLAGS
//                  + CLIPACTIONRECORD... + ClipActionEndFlag(0)
//   CLIPACTIONRECORD = EventFlags CLIPEVENTFLAGS + ActionRecordSize UI32
//                      + [KeyCode UI8 当含 KeyPress 位] + Actions(以 End 0x00 结尾)
// CLIPEVENTFLAGS：SWF<6 为 UI16，SWF>=6 为 UI32；ActionRecordSize 自其字段之后算起，
// 含 KeyCode 与 Actions，指向下一条记录（或结束标志）。
static void readClipActions(ByteView& bv, std::size_t limit, int swfVer,
                            std::map<std::uint32_t, avm1::Script>& out) {
    if (bv.pos() + 2 > limit) return;
    bv.readUI16();                                              // Reserved（恒 0）
    if (swfVer >= 6) {                                          // AllEventFlags
        if (bv.pos() + 4 > limit) return;
        bv.readUI32();
    } else {
        if (bv.pos() + 2 > limit) return;
        bv.readUI16();
    }
    int guard = 0;
    while (++guard <= 1024) {
        std::uint32_t eventFlag;
        if (swfVer >= 6) {
            if (bv.pos() + 4 > limit) break;
            eventFlag = bv.readUI32();
        } else {
            if (bv.pos() + 2 > limit) break;
            eventFlag = bv.readUI16();
        }
        if (eventFlag == 0) break;                              // ClipActionEndFlag：列表结束
        if (bv.pos() + 4 > limit) break;
        std::uint32_t size = bv.readUI32();                     // ActionRecordSize
        std::size_t recStart = bv.pos();                        // 紧随本字段之后
        std::size_t recEnd = recStart + size;
        if (recEnd > limit) recEnd = limit;
        if (eventFlag & AVM1_EVENT_KEY_PRESS) bv.readUI8();     // KeyCode（仅 KeyPress 事件）
        if (bv.pos() < recEnd) {
            avm1::Script sc = readAvm1UntilEnd(bv, bv.pos(), recEnd);
            if (!sc.actions.empty()) out[eventFlag] = std::move(sc);
        }
        bv.seek(recEnd);
    }
}

// SoundInfo（StartSound / DefineButtonSound 内嵌）：跳过可选字段，返回 loop 标志。
// 位布局（UI8）：bit5=Stop bit4=NoMultiple bit3=HasEnvel bit2=HasLoops bit1=HasOutPoint bit0=HasInPoint
static bool readSoundInfoSkip(ByteView& bv, std::size_t limit, bool* stopOut) {
    if (bv.pos() >= limit) return false;
    std::uint8_t f = bv.readUI8();
    if (stopOut) *stopOut = (f & 0x20) != 0;
    if (f & 0x01) { if (bv.pos() + 4 <= limit) bv.readUI32(); }        // InPoint
    if (f & 0x02) { if (bv.pos() + 4 <= limit) bv.readUI32(); }        // OutPoint
    bool loop = false;
    if (f & 0x04) { if (bv.pos() + 2 <= limit) loop = bv.readUI16() > 0; }  // LoopCount
    if (f & 0x08) {                                                    // Envelopes
        if (bv.pos() >= limit) return loop;
        int n = bv.readUI8();
        for (int k = 0; k < n && bv.pos() + 6 <= limit; ++k) {
            bv.readUI32(); bv.readUI16();                             // Position, Level
        }
    }
    return loop;
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
    std::vector<float> sx, sy;      // 折线点（contour 回退 / 描边）
    std::vector<FillEdge> edges;    // 有向填充边（非零环绕的权威几何）
    float lastX = 0, lastY = 0;
    bool started = false;
};

struct ShapeBuilder {
    std::vector<FillStyle>* fills = nullptr;
    std::vector<LineStyle>* lines = nullptr;
    std::shared_ptr<Shape> shape;

    int actFill0 = 0, actFill1 = 0, actLine = 0;
    // 当前样式表的基址（1 基索引 = 基址 + 相对索引）。初始表为 0；StateNewStyles
    // 之后指向新表首个样式在累积数组中的 1 基位置 - 1。
    int fillBase = 0, lineBase = 0;
    float penX = 0, penY = 0;
    // 每个填充样式按"边所在侧"分成两路累积：
    //   key = style*2 + side，side 0 = fillStyle0（样式在边**左侧**，sign -1），
    //                           1 = fillStyle1（样式在边**右侧**，sign +1）。
    // 每条边同时写入它两侧样式的两条通路，这正对应 SWF 语义。用 key 分路还保证
    // 每条通路的折线是**单一朝向**的完整环，闭合不会产生跨图形的错误弦。
    std::map<int, Cur> openFill;
    std::map<int, Cur> openLine;

    static int keyOf(int style, int side) { return style * 2 + side; }

    void ensure(Cur& c, float x, float y) {
        if (!c.started) { c.sx.push_back(x); c.sy.push_back(y); c.lastX = x; c.lastY = y; c.started = true; }
    }
    void addPoint(Cur& c, float x, float y) {
        c.sx.push_back(x); c.sy.push_back(y); c.lastX = x; c.lastY = y;
    }

    // 记录某样式路径首次被刷新的位置（渲染顺序 = Flash 的记录流顺序）。
    void noteOp(bool isLine, int style) {
        for (const auto& op : shape->drawOrder)
            if (op.isLine == isLine && op.style == style) return;
        shape->drawOrder.push_back({isLine, style});
    }

    // 防御：形状记录里的填充/线索引可能超出样式表范围（错位数据），
    // 直接 push_back 到越界元素会造成堆破坏，这里做边界检查。
    void pushFill(int key, Cur& c) {
        int style = key >> 1;
        if (style >= 1 && style <= static_cast<int>(shape->fillPaths.size())) {
            if (c.sx.size() >= 2) {
                Contour ct; ct.xs = c.sx; ct.ys = c.sy;
                shape->fillPaths[style - 1].push_back(std::move(ct));
                noteOp(false, style - 1);
            }
            if (!c.edges.empty() && style <= static_cast<int>(shape->fillEdges.size())) {
                auto& dst = shape->fillEdges[style - 1];
                for (const auto& e : c.edges) dst.push_back(e);
                noteOp(false, style - 1);
                // 注意：**不要**给未闭合的子路径补"闭合弦"。
                // SWF 形状的边是这样生成的：同一样式的所有子路径合起来构成若干条
                // 闭合的有向环（每条子路径只是一段弧，其余部分由别的子路径首尾相接
                // 补全）。实测 swf.swf 中 596/597 个 (形状,样式) 组合满足顶点有向
                // 平衡（出边带号和 == 入边带号和）；若在末端强行补一条回到子路径
                // 起点的弦，反而会在图形内部切出一道并不存在的对角缺口
                // （frame 13 的袖口三角缺口即由此而来）。
            }
        }
        c = Cur{};
    }
    void pushLine(int idx, Cur& c) {
        if (idx > 0 && idx <= static_cast<int>(shape->strokePaths.size())
            && c.started && c.sx.size() >= 2) {
            Contour ct; ct.xs = c.sx; ct.ys = c.sy;
            shape->strokePaths[idx - 1].push_back(ct);
            noteOp(true, idx - 1);
        }
        c = Cur{};
    }

    void finalizeAll() {
        for (auto& kv : openFill) pushFill(kv.first, kv.second);
        openFill.clear();
        for (auto& kv : openLine) pushLine(kv.first, kv.second);
        openLine.clear();
        // 注意：**不能**在这里清零 actFill0/actFill1/actLine。
        // SWF 的 StyleChangeRecord 是"增量更新"——记录里没出现的样式必须沿用原值。
    }

    // 追加一条有向边到样式通路，并从上一个点连到 (x,y)。
    void addSeg(Cur& c, int side, float x, float y) {
        if (!c.started) { c.started = true; c.sx.push_back(penX); c.sy.push_back(penY);
                          c.lastX = penX; c.lastY = penY; }
        c.edges.push_back({c.lastX, c.lastY, x, y, side ? 1 : -1});
        addPoint(c, x, y);
    }

    void addLineTo(float x, float y) {
        if (actFill0) addSeg(openFill[keyOf(actFill0, 0)], 0, x, y);
        if (actFill1) addSeg(openFill[keyOf(actFill1, 1)], 1, x, y);
        if (actLine) {
            Cur& c = openLine[actLine];
            if (!c.started) { c.started = true; c.sx.push_back(penX); c.sy.push_back(penY); }
            addPoint(c, x, y);
        }
    }

    // 样式变更：SWF 语义是"当某条填充/描边样式的索引改变（含改到 0 = 停用）时，
    // 该样式当前的路径就此**结束并闭合**，之后若再次选中同一索引，则从当前笔
    // 位置**重开**一条新路径"。
    void styleChange(int nf0, int nf1, int nl, bool has0, bool has1, bool hasL) {
        if (has0 && nf0 != actFill0) {
            if (actFill0) pushFill(keyOf(actFill0, 0), openFill[keyOf(actFill0, 0)]);
            actFill0 = nf0;
            if (actFill0) ensure(openFill[keyOf(actFill0, 0)], penX, penY);
        }
        if (has1 && nf1 != actFill1) {
            if (actFill1) pushFill(keyOf(actFill1, 1), openFill[keyOf(actFill1, 1)]);
            actFill1 = nf1;
            if (actFill1) ensure(openFill[keyOf(actFill1, 1)], penX, penY);
        }
        if (hasL && nl != actLine) {
            if (actLine > 0) { pushLine(actLine, openLine[actLine]); openLine.erase(actLine); }
            actLine = nl;
            if (actLine > 0) ensure(openLine[actLine], penX, penY);
        }
    }
    void moveTo(float x, float y) {
        finalizeAll();
        penX = x; penY = y;
        // 新的子路径从笔位置起头；只给当前激活的样式开路径
        if (actFill0) ensure(openFill[keyOf(actFill0, 0)], x, y);
        if (actFill1) ensure(openFill[keyOf(actFill1, 1)], x, y);
        if (actLine)  ensure(openLine[actLine], x, y);
    }
    void lineTo(float x, float y) {
        addLineTo(x, y);
        penX = x; penY = y;
    }
    void curveTo(float cx, float cy, float ax, float ay) {
        // 二次贝塞尔拍平：段数由控制点到弦的偏离（矢高 sag，twips）决定，
        // N ≈ sqrt(2·sag)，等价于把折线相对曲线的最大偏差压到约 0.5 twip。
        // 这是按与 Flash Player 的逐像素误差标定的：固定 16 段（历史实现）
        // 明显偏大，矢高越大的曲线偏差越大；本式在大/小曲线上都更接近原版。
        float ddx = 2.0f * cx - penX - ax;
        float ddy = 2.0f * cy - penY - ay;
        float sag = 0.25f * std::sqrt(ddx * ddx + ddy * ddy);  // 曲线中点相对弦的偏离
        int N = static_cast<int>(std::lround(std::sqrt(2.0f * sag)));
        if (N < 1) N = 1;
        if (N > 64) N = 64;
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
        readColor(bv, false, ls.color);            // LINESTYLE：RGB
        return ls;
    }
    if (shapeVer == 3) {
        // DefineShape3 用的是**简单** LINESTYLE（Width UI16 + RGBA），
        // 不是 LINESTYLE2 的位打包形式（那是 DefineShape4 才引入的）。
        readColor(bv, true, ls.color);             // RGBA
        return ls;
    }
    // DefineShape4：LINESTYLE2（位打包）
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
    if (join == 2) br.readUB(16);  // MiterLimitFactor：FIXED8 -> UI16（2 字节）
    int hasBlur = static_cast<int>(br.readUB(1));
    // 模糊字段是位字段，必须用 br 读取；混用 bv 直读会破坏缓冲位置
    if (hasBlur) { br.readUB(16); br.readUB(16); br.readUB(8); }
    br.finish();  // 统一对齐到字节边界，再按字节读颜色/填充
    if (hasFill) {
        readFillStyle(bv, true);  // 消费字节（线填充）
    } else {
        readColor(bv, true, ls.color);
    }
    return ls;
}

static std::shared_ptr<Shape> decodeShape(ByteView& bv, int shapeVer, std::uint16_t shapeId = 0) {
    auto shape = std::make_shared<Shape>();
    bool alpha = (shapeVer >= 3);
    std::size_t entryPos = bv.pos();
    const char* trEnv = std::getenv("SWF_TRACE_SHAPE");
    bool trace = trEnv && static_cast<std::uint16_t>(std::atoi(trEnv)) == shapeId;

    { Rect rb = readRect(bv);
      shape->bounds.x0 = rb.xMin; shape->bounds.x1 = rb.xMax;
      shape->bounds.y0 = rb.yMin; shape->bounds.y1 = rb.yMax; }

    // DefineShape4 在 ShapeBounds 之后还有一段专属头部，**必须跳过**，否则后面
    // 的填充/线样式表与边记录全部错位（swf2.swf 有 138 个 DefineShape4）：
    //   RECT EdgeBounds
    //   UB[5] Reserved
    //   UB[1] UsesFillWindingRule   ← 该形状应使用非零环绕还是奇偶规则
    //   UB[1] UsesNonScalingStrokes
    //   UB[1] UsesScalingStrokes
    bool usesWinding = false;
    if (shapeVer >= 4) {
        readRect(bv);                       // EdgeBounds：仅用于裁剪优化，这里忽略
        BitReader hbr(bv);
        hbr.readUB(5);                      // Reserved
        usesWinding = hbr.readUB(1) != 0;   // UsesFillWindingRule
        hbr.readUB(1);                      // UsesNonScalingStrokes
        hbr.readUB(1);                      // UsesScalingStrokes
        hbr.finish();                       // 对齐到字节边界
    }
    shape->usesWinding = usesWinding;

    if (std::getenv("SWF_DUMP_SHAPES")) {
        const std::uint8_t* bp = bv.base();
        std::printf("SHAPE entry=%zu postHead=%zu ver=%d hdr=%02x %02x %02x %02x %02x %02x %02x %02x\n",
                    entryPos, bv.pos(), shapeVer,
                    bp[bv.pos()], bp[bv.pos()+1], bp[bv.pos()+2], bp[bv.pos()+3],
                    bp[bv.pos()+4], bp[bv.pos()+5], bp[bv.pos()+6], bp[bv.pos()+7]);
    }

    // 填充样式（防御：数量异常/溢出时提前结束，避免真实文件触发内存爆炸）
    int fc = bv.readUI8(); if (fc == 0xFF) fc = bv.readUI16();
    if (fc < 0 || fc > 4096 || bv.overflow()) return shape;
    shape->fillStyles.resize(fc);
    shape->fillPaths.resize(fc);
    shape->fillEdges.resize(fc);
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
    if (std::getenv("SWF_DUMP_SHAPES")) {
        std::printf("SHAPE headDone=%zu fillBits=%d lineBits=%d\n", bv.pos(), numFillBits, numLineBits);
    }



    bool done = false;
    int guard = 0;
    while (!done) {
        if (++guard > 100000) break;
        if (trace && guard <= 400) std::printf("  R i=%d bit=%zu\n", guard, br.bitPos());
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
                if (trace && guard <= 400) std::printf("     move mb=%d dx=%d dy=%d\n", mb, (int)x, (int)y);
                sb.moveTo(x, y);
            }
            // 样式索引是 1 基索引（0 = 停用）。StateNewStyles 追加新样式后，
            // 后续记录的索引相对**本批新表**（基址 = 追加前数组长度）。
            // 文件证据（cid14）：单 fill 批声明 fb=1 —— 绝对索引下该 fill 永不可达，
            // 只有批内相对索引才自洽；且边流按此解析恰好结束于 tag 末尾。
            //   1 基绝对下标 = fillBase + raw   （raw >= 1；raw == 0 保持 0）
            int raw0 = stateFill0 ? static_cast<int>(br.readUB(numFillBits)) : 0;
            int raw1 = stateFill1 ? static_cast<int>(br.readUB(numFillBits)) : 0;
            int rawL = stateLine ? static_cast<int>(br.readUB(numLineBits)) : 0;
            // 样式变更不结束子路径：只对"样式真的变了"的游标收尾并重开
            int nf0 = stateFill0 ? (raw0 ? sb.fillBase + raw0 : 0) : sb.actFill0;
            int nf1 = stateFill1 ? (raw1 ? sb.fillBase + raw1 : 0) : sb.actFill1;
            int nl = stateLine ? (rawL ? sb.lineBase + rawL : 0) : sb.actLine;
            sb.styleChange(nf0, nf1, nl, stateFill0 != 0, stateFill1 != 0, stateLine != 0);
            if (trace && guard <= 400) std::printf("     style new=%d line=%d f1=%d f0=%d mv=%d idx f0=%d f1=%d l=%d fb=%d lb=%d\n",
                stateNew, stateLine, stateFill1, stateFill0, stateMove, nf0, nf1, nl, numFillBits, numLineBits);
            if (stateNew) {
                // 与 Ruffle 的 Reader::read_shape_record 保持一致（SWF 规范的位/字节混排）：
                //   1) 位字段读完后先 byte_align：丢弃当前字节剩余位，前进到下一字节；
                //   2) 用**字节**读取器读 FILLSTYLEARRAY + LINESTYLEARRAY；
                //   3) 数组之后还有 1 个字节 = NumFillBits(高4位) + NumLineBits(低4位)，
                //      用它更新后续记录的索引位宽。
                // 之前漏掉第 3 步、且用 neededBits(样式总数) 推测位宽，会立刻错位；
                // 而直接用 bv 读（不对齐 br）更是错的——BitReader::refill 预读了 4 字节。
                br.alignToByte();
                int nfc = bv.readUI8();
                if (nfc == 0xFF) nfc = bv.readUI16();
                int old = static_cast<int>(shape->fillStyles.size());
                // StateNewStyles：本记录起样式表换成本记录新追加的这批，
                // 后续记录的索引相对本批首个样式（1 基 = old + raw）。
                sb.fillBase = old;
                // 防御：样式总数设上限，避免错位导致的失控增长
                if (nfc < 0 || nfc > 1024 || old + nfc > 4096) { sb.finalizeAll(); break; }
                shape->fillStyles.resize(old + nfc);
                shape->fillPaths.resize(old + nfc);
                shape->fillEdges.resize(old + nfc);
                for (int i = 0; i < nfc; ++i) shape->fillStyles[old + i] = readFillStyle(bv, alpha);
                // 线样式表的计数紧跟在**填充样式数据之后**，不能在读完 nfc 后立刻读
                int nlc = bv.readUI8();
                if (nlc == 0xFF) nlc = bv.readUI16();
                int oldl = static_cast<int>(shape->lineStyles.size());
                sb.lineBase = oldl;
                if (nlc < 0 || nlc > 1024 || oldl + nlc > 4096) { sb.finalizeAll(); break; }
                shape->lineStyles.resize(oldl + nlc);
                shape->strokePaths.resize(oldl + nlc);
                for (int i = 0; i < nlc; ++i) shape->lineStyles[oldl + i] = readLineStyle(bv, shapeVer);
                if (bv.overflow()) { sb.finalizeAll(); break; }
                int nb = bv.readUI8();               // NumFillBits | NumLineBits

                if (trace && guard <= 400) std::printf("     stateNew nfc=%d nlc=%d totalF=%zu totalL=%zu nb=%02x -> fb=%d lb=%d\n",
                    nfc, nlc, shape->fillStyles.size(), shape->lineStyles.size(), nb, nb >> 4, nb & 0x0F);
                numFillBits = nb >> 4;
                numLineBits = nb & 0x0F;
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
                if (trace && guard <= 400)
                    std::printf("     curve nbits=%d cd=(%d,%d) ad=(%d,%d) pen=(%d,%d) ctrl=(%d,%d) anchor=(%d,%d)\n",
                        nbits, (int)cx, (int)cy, (int)ax, (int)ay, (int)sb.penX, (int)sb.penY,
                        (int)(sb.penX + cx), (int)(sb.penY + cy), (int)(sb.penX + ax), (int)(sb.penY + ay));
                // SWF 曲线边：AnchorDelta 相对**控制点**（而非笔点），
                // anchor = pen + ctrlDelta + anchorDelta
                sb.curveTo(sb.penX + cx, sb.penY + cy,
                           sb.penX + cx + ax, sb.penY + cy + ay);
            }
        }
    }
    br.finish();
    if (std::getenv("SWF_DUMP_SHAPES")) {
        std::printf("SHAPE end=%zu id=%u ver=%d fc=%zu lc=%zu bounds=(%d,%d,%d,%d) fillBits=%d lineBits=%d\n",
                    bv.pos(), shapeId, shapeVer, shape->fillStyles.size(), shape->lineStyles.size(),
                    (int)shape->bounds.x0, (int)shape->bounds.x1, (int)shape->bounds.y0, (int)shape->bounds.y1,
                    numFillBits, numLineBits);
        for (std::size_t i = 0; i < shape->fillStyles.size(); ++i) {
            const FillStyle& fs = shape->fillStyles[i];
            if (!shape->fillPaths[i].empty())
            std::printf("   fs id=%u [%zu] kind=%d rgba=(%d,%d,%d,%d) stops=%zu bmp=%u m=(%.3f,%.3f,%.3f,%.3f t=%.1f,%.1f) bbox=",
                shapeId, i, (int)fs.kind, fs.color.r, fs.color.g, fs.color.b, fs.color.a,
                fs.gradient.size(), fs.bitmapId, fs.gradientMatrix.sx, fs.gradientMatrix.sy,
                fs.gradientMatrix.r0, fs.gradientMatrix.r1, fs.gradientMatrix.tx, fs.gradientMatrix.ty);
            for (const auto& ct : shape->fillPaths[i]) {
                float x0=1e9f,y0=1e9f,x1=-1e9f,y1=-1e9f;
                for (std::size_t k = 0; k < ct.xs.size(); ++k) {
                    x0=std::min(x0,ct.xs[k]); x1=std::max(x1,ct.xs[k]);
                    y0=std::min(y0,ct.ys[k]); y1=std::max(y1,ct.ys[k]);
                }
                if (!shape->fillPaths[i].empty())
                std::printf("(%.0f,%.0f)-(%.0f,%.0f)n%zu ", x0,y0,x1,y1,ct.xs.size());
            }
            if (!shape->fillPaths[i].empty()) std::printf("\n");
        }
        for (std::size_t i = 0; i < shape->lineStyles.size(); ++i) {
            std::printf("   ls id=%u [%zu] w=%.1f rgba=(%d,%d,%d,%d)\n", shapeId, i,
                shape->lineStyles[i].width, shape->lineStyles[i].color.r, shape->lineStyles[i].color.g,
                shape->lineStyles[i].color.b, shape->lineStyles[i].color.a);
        }
    }
    // 临时调试：把某形状的每个填充样式的有向边转储为文本，便于外部可视化核对几何。
    {
        const char* edEnv = std::getenv("SWF_DUMP_EDGES");
        if (edEnv && static_cast<std::uint16_t>(std::atoi(edEnv)) == shapeId) {
            char fn[64];
            std::snprintf(fn, sizeof(fn), "edges_%u.txt", static_cast<unsigned>(shapeId));
            FILE* f = std::fopen(fn, "w");
            if (f) {
                std::fprintf(f, "bounds %d %d %d %d\n", (int)shape->bounds.x0, (int)shape->bounds.x1,
                             (int)shape->bounds.y0, (int)shape->bounds.y1);
                for (std::size_t i = 0; i < shape->fillStyles.size(); ++i) {
                    const FillStyle& fs = shape->fillStyles[i];
                    std::fprintf(f, "style %zu kind %d rgba %d %d %d %d stops %zu focal %.3f %.3f m %.4f %.4f %.4f %.4f %.1f %.1f\n",
                                 i, (int)fs.kind, fs.color.r, fs.color.g, fs.color.b, fs.color.a,
                                 fs.gradient.size(), fs.focalX, fs.focalY,
                                 fs.gradientMatrix.sx, fs.gradientMatrix.sy, fs.gradientMatrix.r0,
                                 fs.gradientMatrix.r1, fs.gradientMatrix.tx, fs.gradientMatrix.ty);
                    for (const auto& gs : fs.gradient)
                        std::fprintf(f, "g %d %d %d %d %d\n", gs.ratio, gs.color.r, gs.color.g, gs.color.b, gs.color.a);
                    for (const auto& e : shape->fillEdges[i])
                        std::fprintf(f, "e %.2f %.2f %.2f %.2f %d\n", e.x0, e.y0, e.x1, e.y1, e.sign);
                }
                std::fclose(f);
            }
        }
    }
    return shape;
}

static int neededBits(std::size_t n) {
    if (n == 0) return 0;
    int b = 1;
    while (n > 1) { n >>= 1; ++b; }
    return b;
}

// 解析变形形状的单段（起始段 MorphFlag=0 / 终止段 MorphFlag=1），写入各自 ShapeBuilder。
static void parseMorphSection(ByteView& bv, int shapeVer, ShapeBuilder& sb,
                              int& numFillBits, int& numLineBits,
                              bool startSection, bool allowNew) {
    BitReader br(bv);
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
            int morphFlag  = static_cast<int>(br.readUB(1));
            bool relevant  = startSection ? (morphFlag == 0) : (morphFlag == 1);
            if (!(stateNew || stateLine || stateFill1 || stateFill0 || stateMove)) {
                if (relevant) sb.finalizeAll();
                break;   // EndShapeRecord
            }
            if (relevant) {
                if (stateMove) {
                    int mb = static_cast<int>(br.readUB(5));
                    float x = static_cast<float>(br.readSB(mb));
                    float y = static_cast<float>(br.readSB(mb));
                    sb.moveTo(x, y);
                }
                // 样式变更不结束子路径：按游标增量更新
                bool has0 = stateFill0 && numFillBits > 0;
                bool has1 = stateFill1 && numFillBits > 0;
                bool hasL = stateLine && numLineBits > 0;
                int nf0 = has0 ? static_cast<int>(br.readUB(numFillBits)) : sb.actFill0;
                int nf1 = has1 ? static_cast<int>(br.readUB(numFillBits)) : sb.actFill1;
                int nl = hasL ? static_cast<int>(br.readUB(numLineBits)) : sb.actLine;
                sb.styleChange(nf0, nf1, nl, has0, has1, hasL);
            } else {
                // 另一段的同名记录：消费等长的位以与位流同步
                if (stateMove) { int mb = static_cast<int>(br.readUB(5)); br.readSB(mb); br.readSB(mb); }
                if (stateFill0 && numFillBits > 0) br.readUB(numFillBits);
                if (stateFill1 && numFillBits > 0) br.readUB(numFillBits);
                if (stateLine  && numLineBits > 0) br.readUB(numLineBits);
            }
            if (stateNew) {
                int nfc = bv.readUI8(); if (nfc == 0xFF) nfc = bv.readUI16();
                if (nfc < 0 || nfc > 1024) break;
                if (allowNew) {
                    int old = static_cast<int>(sb.shape->fillStyles.size());
                    sb.shape->fillStyles.resize(old + nfc); sb.shape->fillPaths.resize(old + nfc);
                    sb.shape->fillEdges.resize(old + nfc);
                    for (int i = 0; i < nfc; ++i) sb.shape->fillStyles[old + i] = readFillStyle(bv, true);
                    int nlc = bv.readUI8(); if (nlc == 0xFF) nlc = bv.readUI16();
                    if (nlc < 0 || nlc > 1024) break;
                    int oldl = static_cast<int>(sb.shape->lineStyles.size());
                    sb.shape->lineStyles.resize(oldl + nlc); sb.shape->strokePaths.resize(oldl + nlc);
                    for (int i = 0; i < nlc; ++i) sb.shape->lineStyles[oldl + i] = readLineStyle(bv, shapeVer);
                    numFillBits = neededBits(sb.shape->fillStyles.size());
                    numLineBits = neededBits(sb.shape->lineStyles.size());
                } else {
                    // 终止段不应再定义样式，仅消费字节以对齐
                    for (int i = 0; i < nfc; ++i) readFillStyle(bv, true);
                    int nlc = bv.readUI8(); if (nlc == 0xFF) nlc = bv.readUI16();
                    for (int i = 0; i < nlc; ++i) readLineStyle(bv, shapeVer);
                }
            }
            firstRec = false;
        } else {
            int straight = static_cast<int>(br.readUB(1));
            int nbits    = static_cast<int>(br.readUB(4)) + 2;
            int morphFlag = static_cast<int>(br.readUB(1));
            bool relevant = startSection ? (morphFlag == 0) : (morphFlag == 1);
            if (straight) {
                int general = static_cast<int>(br.readUB(1));
                float dx = 0, dy = 0;
                if (general) { dx = static_cast<float>(br.readSB(nbits)); dy = static_cast<float>(br.readSB(nbits)); }
                else { int vert = static_cast<int>(br.readUB(1)); if (vert) dy = static_cast<float>(br.readSB(nbits)); else dx = static_cast<float>(br.readSB(nbits)); }
                if (relevant) sb.lineTo(sb.penX + dx, sb.penY + dy);
            } else {
                float cx = static_cast<float>(br.readSB(nbits)), cy = static_cast<float>(br.readSB(nbits)), ax = static_cast<float>(br.readSB(nbits)), ay = static_cast<float>(br.readSB(nbits));
                if (relevant) sb.curveTo(sb.penX + cx, sb.penY + cy, sb.penX + ax, sb.penY + ay);
            }
        }
    }
    br.finish();
}

// DefineMorphShape(46) / DefineMorphShape2(84)：变形形状。
// 记录流含起始(段)与终止(Offset 处)两套几何，样式索引对齐。产出 t=0/t=1 两形状供插值。
static std::shared_ptr<MorphShape> decodeMorphShape(ByteView& bv, int shapeVer) {
    std::size_t recStart = bv.pos() - 2;   // 记录起点（CharacterID 之前 2 字节），Offset 相对此处
    auto ms = std::make_shared<MorphShape>();
    ms->start = std::make_shared<Shape>();
    ms->end   = std::make_shared<Shape>();
    {
        Rect rb = readRect(bv);
        ms->start->bounds.x0 = rb.xMin; ms->start->bounds.x1 = rb.xMax;
        ms->start->bounds.y0 = rb.yMin; ms->start->bounds.y1 = rb.yMax;
        ms->end->bounds = ms->start->bounds;
    }
    { Rect eb = readRect(bv);
      ms->end->bounds.x0 = eb.xMin; ms->end->bounds.x1 = eb.xMax;
      ms->end->bounds.y0 = eb.yMin; ms->end->bounds.y1 = eb.yMax; }
    std::uint32_t endOffset = 0;
    if (shapeVer >= 2) { readRect(bv); readRect(bv); endOffset = bv.readUI32(); }   // edge bounds + flags
    else { endOffset = bv.readUI32(); }

    ShapeBuilder sbS, sbE;
    sbS.fills = &ms->start->fillStyles; sbS.lines = &ms->start->lineStyles; sbS.shape = ms->start;
    sbE.fills = &ms->end->fillStyles;   sbE.lines = &ms->end->lineStyles;   sbE.shape = ms->end;

    int fc = bv.readUI8(); if (fc == 0xFF) fc = bv.readUI16();
    if (fc < 0 || fc > 4096 || bv.overflow()) return ms;
    ms->start->fillStyles.resize(fc); ms->start->fillPaths.resize(fc);
    ms->end->fillStyles.resize(fc);   ms->end->fillPaths.resize(fc);
    ms->start->fillEdges.resize(fc);  ms->end->fillEdges.resize(fc);
    for (int i = 0; i < fc; ++i) {
        ms->start->fillStyles[i] = readFillStyle(bv, true);
        ms->end->fillStyles[i]   = readFillStyle(bv, true);   // 紧跟其后的终止样式
    }
    int lc = bv.readUI8(); if (lc == 0xFF) lc = bv.readUI16();
    if (lc < 0 || lc > 4096 || bv.overflow()) return ms;
    ms->start->lineStyles.resize(lc); ms->start->strokePaths.resize(lc);
    ms->end->lineStyles.resize(lc);   ms->end->strokePaths.resize(lc);
    for (int i = 0; i < lc; ++i) {
        ms->start->lineStyles[i] = readLineStyle(bv, shapeVer);
        ms->end->lineStyles[i]   = readLineStyle(bv, shapeVer);
    }
    int numFillBits = neededBits(ms->start->fillStyles.size());
    int numLineBits = neededBits(ms->start->lineStyles.size());

    parseMorphSection(bv, shapeVer, sbS, numFillBits, numLineBits, true, true);   // 起始段
    if (endOffset > 0) bv.seek(recStart + endOffset);
    parseMorphSection(bv, shapeVer, sbE, numFillBits, numLineBits, false, false); // 终止段
    return ms;
}

// 字形轮廓：字形内嵌于字体表，格式同 DefineShape 记录流但无 bounds、无填充/线数量表头，
// 样式只经 StyleChangeRecord(stateNew) 内联定义。
static void parseGlyphShape(ByteView& bv, std::shared_ptr<Shape>& shape, int shapeVer) {
    // 字形是“裸 SHAPE”：NumFillBits(UB[4]) + NumLineBits(UB[4]) + SHAPERECORD。
    // 没有 ShapeBounds，也没有 FillStyleCount/LineStyleCount 与样式数组 —— 填充样式由渲染时
    // 外部提供（文本颜色）。但渲染器是遍历 fillPaths[i] 用文本颜色填充的，故必须预置 1 个
    // 填充样式槽来承接字形轮廓，否则 fill0 引用的样式不存在、轮廓会被整体丢弃。
    shape->fillStyles.resize(1);
    shape->fillPaths.resize(1);
    shape->fillEdges.resize(1);
    ShapeBuilder sb;
    sb.fills = &shape->fillStyles; sb.lines = &shape->lineStyles; sb.shape = shape;
    int numFillBits = 0, numLineBits = 0;
    {
        BitReader b0(bv);
        numFillBits = static_cast<int>(b0.readUB(4));
        numLineBits = static_cast<int>(b0.readUB(4));
        b0.finish();
    }
    std::unique_ptr<BitReader> brp(new BitReader(bv));
    bool firstRec = true;
    int guard = 0;
    for (;;) {
        if (++guard > 20000 || bv.overflow()) break;
        int typeFlag = static_cast<int>(brp->readUB(1));
        if (typeFlag == 0) {
            int stateNew   = static_cast<int>(brp->readUB(1));
            int stateLine  = static_cast<int>(brp->readUB(1));
            int stateFill1 = static_cast<int>(brp->readUB(1));
            int stateFill0 = static_cast<int>(brp->readUB(1));
            int stateMove  = static_cast<int>(brp->readUB(1));
            if (!(stateNew || stateLine || stateFill1 || stateFill0 || stateMove)) { sb.finalizeAll(); break; }
            if (stateMove) {
                int mb = static_cast<int>(brp->readUB(5));
                float x = static_cast<float>(brp->readSB(mb));
                float y = static_cast<float>(brp->readSB(mb));
                sb.moveTo(x, y);
            }
            // 样式变更不结束子路径：按游标增量更新
            {
                bool has0 = stateFill0 && numFillBits > 0;
                bool has1 = stateFill1 && numFillBits > 0;
                bool hasL = stateLine && numLineBits > 0;
                int nf0 = has0 ? static_cast<int>(brp->readUB(numFillBits)) : sb.actFill0;
                int nf1 = has1 ? static_cast<int>(brp->readUB(numFillBits)) : sb.actFill1;
                int nl = hasL ? static_cast<int>(brp->readUB(numLineBits)) : sb.actLine;
                sb.styleChange(nf0, nf1, nl, has0, has1, hasL);
            }
            if (stateNew) {
                int nfc = bv.readUI8(); if (nfc == 0xFF) nfc = bv.readUI16();
                if (nfc < 0 || nfc > 256) break;
                int old = static_cast<int>(shape->fillStyles.size());
                shape->fillStyles.resize(old + nfc); shape->fillPaths.resize(old + nfc);
                shape->fillEdges.resize(old + nfc);
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
            int straight = static_cast<int>(brp->readUB(1));
            int nbits    = static_cast<int>(brp->readUB(4)) + 2;
            if (straight) {
                int general = static_cast<int>(brp->readUB(1));
                float dx = 0, dy = 0;
                if (general) { dx = static_cast<float>(brp->readSB(nbits)); dy = static_cast<float>(brp->readSB(nbits)); }
                else { int vert = static_cast<int>(brp->readUB(1)); if (vert) dy = static_cast<float>(brp->readSB(nbits)); else dx = static_cast<float>(brp->readSB(nbits)); }
                sb.lineTo(sb.penX + dx, sb.penY + dy);
            } else {
                float cx = static_cast<float>(brp->readSB(nbits));
                float cy = static_cast<float>(brp->readSB(nbits));
                float ax = static_cast<float>(brp->readSB(nbits));
                float ay = static_cast<float>(brp->readSB(nbits));
                // SWF 曲线边：AnchorDelta 相对**控制点**（而非笔点），
                // anchor = pen + ctrlDelta + anchorDelta
                sb.curveTo(sb.penX + cx, sb.penY + cy,
                           sb.penX + cx + ax, sb.penY + cy + ay);
            }
        }
    }
    brp->finish();
}

// DefineFont2(48) / DefineFont3(75)。注意：FontID 已由调用方读出，本函数从 flags 开始。
static std::shared_ptr<FontGlyphs> decodeFont(ByteView& bv, int swfVersion) {
    auto fg = std::make_shared<FontGlyphs>();
    std::uint8_t flags = bv.readUI8();
    bool hasLayout   = (flags & 0x80) != 0;   // FontFlagsHasLayout
    bool wideOffsets = (flags & 0x08) != 0;   // FontFlagsWideOffsets（32 位偏移）
    bool wideCodes   = (flags & 0x04) != 0;   // FontFlagsWideCodes（16 位字符码）
    // FontFlagsItalic / FontFlagsBold（MSB→LSB 顺序的最后两位）
    fg->italic = (flags & 0x02) != 0;
    fg->bold   = (flags & 0x01) != 0;
    if (swfVersion >= 6) bv.readUI8();        // LanguageCode（SWF 6+ 才有）
    int nameLen = bv.readUI8();
    {   // 字体族名：嵌入字体缺字形时按此名回退系统（设备）字体（Flash 的字体替换）
        std::size_t nameStart = bv.pos();
        for (int i = 0; i < nameLen; ++i) {
            int c = bv.readUI8();
            if (c == 0) break;
            fg->name.push_back(static_cast<char>(c));
        }
        bv.seek(nameStart + static_cast<std::size_t>(nameLen));
    }
    int numGlyphs = bv.readUI16();
    if (numGlyphs <= 0 || numGlyphs > 65535 || bv.overflow()) return fg;
    fg->shapes.resize(static_cast<std::size_t>(numGlyphs));
    fg->advances.assign(static_cast<std::size_t>(numGlyphs), 0);
    // 偏移表与 CodeTableOffset 是无条件存在的（HasLayout 只影响代码表之后的
    // Ascent/Descent/Leading/Advance/Bounds/Kerning 布局字段）。两者宽度一致，
    // 由 WideOffsets 决定 UI16 / UI32；偏移与代码表位置均以 table_base 为基准。
    // （此前 CodeTableOffset 固定按 UI32 读取，导致取到垃圾值、代码表整体错位。）
    std::size_t tableBase = bv.pos();   // NumGlyphs 之后即偏移表起点
    std::vector<std::uint32_t> offsets(static_cast<std::size_t>(numGlyphs));
    for (int i = 0; i < numGlyphs; ++i)
        offsets[i] = wideOffsets ? bv.readUI32() : static_cast<std::uint32_t>(bv.readUI16());
    std::uint32_t codeOffset = wideOffsets ? bv.readUI32() : static_cast<std::uint32_t>(bv.readUI16());
    if (bv.overflow()) return fg;

    // 字形：位于 table_base + offsets[i]，每个都是裸 SHAPE（见 parseGlyphShape）
    for (int i = 0; i < numGlyphs; ++i) {
        std::size_t gpos = tableBase + offsets[static_cast<std::size_t>(i)];
        if (gpos >= bv.size()) continue;
        ByteView gv(bv.base(), bv.size()); gv.seek(gpos);
        auto shp = std::make_shared<Shape>();
        parseGlyphShape(gv, shp, 3);
        fg->shapes[static_cast<std::size_t>(i)] = shp;
    }
    std::size_t codeTablePos = tableBase + codeOffset;
    (void)hasLayout;   // 布局字段（Ascent/Descent/Advance 等）暂不需要，跳过
    // 代码表：每字形对应一个字符码（1 或 2 字节，取决于 WideCodes）。
    // 用全新的 ByteView 读取：字形解析可能已置位 overflow_（该标志是粘性的，seek 不会清除）。
    std::size_t need = static_cast<std::size_t>(numGlyphs) * (wideCodes ? 2 : 1);
    if (codeTablePos + need <= bv.size()) {
        ByteView cv(bv.base(), bv.size());
        cv.seek(codeTablePos);
        for (int i = 0; i < numGlyphs && !cv.overflow(); ++i) {
            int code = wideCodes ? static_cast<int>(cv.readUI16())
                                 : static_cast<int>(cv.readUI8());
            fg->codeToGlyph[code] = i;
        }
    }
    return fg;
}

// DefineText(11) / DefineText2(33)。注意：CharacterID 已由调用方读出，本函数从 TextBounds 开始。
//
// 实测（swf.swf，SWF v6）确认的位布局：
//   RECT(TextBounds) -> 按字节对齐 -> MATRIX -> 按字节对齐
//   -> GlyphBits(UI8) -> AdvanceBits(UI8)
//   -> 若干 TEXTRECORD，且**每条 TEXTRECORD 起始于字节边界**（记录内部按位打包）：
//        0x00                       = END（整字节）
//        bit7=1                     = setup 记录
//          bit3=hasFont bit2=hasColor bit1=hasMoveY bit0=hasMoveX
//          字段顺序：FontID(UI16) -> Color(RGB/RGBA) -> MoveX(SI16) -> MoveY(SI16) -> FontHeight(UI16)
//        bit7=0                     = glyph 记录，count = 低 7 位
//          每个字形：GlyphIndex(UB[GlyphBits]) + GlyphAdvance(SB[AdvanceBits])
//   记录结束后补齐到下一字节边界。
// 注意：FontID/MoveX/MoveY/FontHeight 在字节流里都是小端（ByteView::readUI16 已按小端）。
static std::int16_t readSI16(ByteView& bv) { return static_cast<std::int16_t>(bv.readUI16()); }

static std::shared_ptr<TextDef> decodeText(ByteView& bv, bool alpha) {
    auto td = std::make_shared<TextDef>();
    readRect(bv);               // TextBounds
    td->textMatrix = readMatrix(bv);
    int glyphBits = bv.readUI8();
    int advanceBits = bv.readUI8();
    if (glyphBits < 0 || glyphBits > 32 || advanceBits < 0 || advanceBits > 32) return td;
    std::uint16_t curFont = 0;
    Color curColor(0, 0, 0);
    int curFontHeight = 0;
    int penX = 0, penY = 0;
    int guard = 0;
    while (!bv.overflow() && bv.remaining() > 0 && ++guard < 65535) {
        std::uint8_t hdr = bv.readUI8();     // 记录头总在字节边界上
        if (hdr == 0x00) break;              // END
        if (hdr & 0x80) {                    // setup（样式）记录
            bool hasFont  = (hdr & 0x08) != 0;
            bool hasColor = (hdr & 0x04) != 0;
            bool hasMoveY = (hdr & 0x02) != 0;
            bool hasMoveX = (hdr & 0x01) != 0;
            if (hasFont)  curFont = bv.readUI16();
            if (hasColor) readColor(bv, alpha, curColor);
            if (hasMoveX) penX = readSI16(bv);
            if (hasMoveY) penY = readSI16(bv);
            if (hasFont)  curFontHeight = bv.readUI16();   // FontHeight（该 run 的字号，twips）
        } else {                             // glyph 记录
            int count = hdr & 0x7F;
            std::size_t start = bv.pos();
            int bits = 0;
            {
                BitReader gb(bv);
                for (int g = 0; g < count && !bv.overflow(); ++g) {
                    TextGlyph tg;
                    tg.fontId = curFont;
                    tg.glyphIndex = (glyphBits > 0)
                        ? static_cast<std::uint16_t>(gb.readUB(glyphBits)) : 0;
                    tg.advance = (advanceBits > 0) ? gb.readSB(advanceBits) : 0;
                    tg.fontHeight = curFontHeight;
                    tg.color = curColor;
                    tg.x = penX; tg.y = penY;
                    td->glyphs.push_back(tg);
                    penX += tg.advance;
                    bits += glyphBits + advanceBits;
                }
            }
            bv.seek(start + static_cast<std::size_t>((bits + 7) / 8));  // 补齐到字节边界
        }
    }
    return td;
}

// DefineEditText(37)：时间轴文本框。
// Flags 是 16 个按位流顺序（MSB-first）读取的标志位，**不是**小端 UI16——
// 按字节 b0、b1 从高位到低位依次赋值：b0=HasText,WordWrap,Multiline,Password,
// ReadOnly,HasTextColor,HasMaxLength,HasFont；b1=HasFontClass,AutoSize,HasLayout,
// NoSelect,Border,WasStatic,HTML,UseOutlines。
// 字段顺序（与 Adobe 文档列出的顺序不同，以 JPEXS 实现为准）：
//   FontID → FontClass → FontHeight → TextColor → MaxLength → Layout
//   → VariableName → InitialText（注意：VariableName 在 InitialText **之前**）
static std::shared_ptr<EditText> decodeEditText(ByteView& bv) {
    auto et = std::make_shared<EditText>();
    Rect rb = readRect(bv);
    et->bounds.x0 = rb.xMin; et->bounds.x1 = rb.xMax;
    et->bounds.y0 = rb.yMin; et->bounds.y1 = rb.yMax;
    std::uint8_t b0 = bv.readUI8();
    std::uint8_t b1 = bv.readUI8();
    bool hasText      = (b0 & 0x80) != 0;
    et->wordWrap      = (b0 & 0x40) != 0;
    et->multiline     = (b0 & 0x20) != 0;
    et->password      = (b0 & 0x10) != 0;
    et->readonly      = (b0 & 0x08) != 0;
    bool hasTextColor = (b0 & 0x04) != 0;
    bool hasMaxLength = (b0 & 0x02) != 0;
    bool hasFont      = (b0 & 0x01) != 0;
    bool hasFontClass = (b1 & 0x80) != 0;
    et->autoSize      = (b1 & 0x40) != 0;
    bool hasLayout    = (b1 & 0x20) != 0;
    et->border        = (b1 & 0x08) != 0;
    auto readStr = [&]() {
        std::string s; int c;
        while (bv.remaining() > 1 && (c = bv.readUI8()) != 0 && c != -1) s.push_back(static_cast<char>(c));
        return s;
    };
    if (hasFont) et->fontId = bv.readUI16();
    if (hasFontClass) { (void)readStr(); }   // 设备字体类名，忽略
    if (hasFont || hasFontClass) et->fontHeight = bv.readUI16();
    if (hasTextColor) { auto c = bv.readRGBA(true); et->color = Color(c.r, c.g, c.b, c.a); }
    if (hasMaxLength) et->maxLength = bv.readUI16();
    if (hasLayout) {
        (void)bv.readUI8();    // Align(UB[2]) + 保留位
        (void)bv.readUI16();   // LeftMargin
        (void)bv.readUI16();   // RightMargin
        (void)bv.readUI16();   // Indent
        (void)bv.readUI16();   // Leading(SI16)
    }
    // VariableName 在 InitialText 之前（脚本写同名变量时文本随之更新）。
    et->variableName = readStr();
    if (hasText) et->text = readStr();
    return et;
}

// 解析 FILTERLIST：DropShadow/Blur/Glow 存入 out（供渲染）；其余滤镜按精确结构跳过。
static void parseFilterList(ByteView& bv, std::vector<Filter>& out) {
    std::uint8_t n = bv.readUI8();
    auto fx = [&]() { return bv.readSI32() / 65536.0f; };  // FIXED 16.16 -> float
    for (int i = 0; i < n && !bv.overflow(); ++i) {
        std::uint8_t id = bv.readUI8();
        Filter f;
        switch (id) {
            case 0: {  // DropShadow
                auto c = bv.readRGBA(true); f.kind = FilterKind::DropShadow; f.color = Color(c.r, c.g, c.b, c.a);
                f.blurX = fx(); f.blurY = fx(); f.angle = fx(); f.distance = fx(); f.strength = fx();
                std::uint8_t fl = bv.readUI8(); f.inner = (fl & 0x80) != 0; f.knockout = (fl & 0x40) != 0; f.passes = (fl & 0x1F);
                out.push_back(f); } break;
            case 1: {  // Blur
                f.kind = FilterKind::Blur; f.blurX = fx(); f.blurY = fx();
                std::uint8_t fl = bv.readUI8(); f.passes = (fl & 0x1F);
                out.push_back(f); } break;
            case 2: {  // Glow
                auto c = bv.readRGBA(true); f.kind = FilterKind::Glow; f.color = Color(c.r, c.g, c.b, c.a);
                f.blurX = fx(); f.blurY = fx(); f.strength = fx();
                std::uint8_t fl = bv.readUI8(); f.inner = (fl & 0x80) != 0; f.knockout = (fl & 0x40) != 0; f.passes = (fl & 0x1F);
                out.push_back(f); } break;
            case 3: {  // Bevel
                Filter f; f.kind = FilterKind::Bevel;
                auto sh = bv.readRGBA(true); f.color = Color(sh.r, sh.g, sh.b, sh.a);     // 阴影像素
                auto hi = bv.readRGBA(true); f.highlight = Color(hi.r, hi.g, hi.b, hi.a); // 高光像素
                f.blurX = fx(); f.blurY = fx(); f.angle = fx(); f.distance = fx(); f.strength = fx();
                std::uint8_t fl = bv.readUI8();
                f.inner = (fl & 0x80) != 0; f.knockout = (fl & 0x40) != 0; f.passes = (fl & 0x1F);
                out.push_back(f); } break;
            case 4: case 7: {  // GradientGlow / GradientBevel
                Filter f; f.kind = (id == 4) ? FilterKind::GradientGlow : FilterKind::GradientBevel;
                std::uint8_t nc = bv.readUI8();
                for (int k = 0; k < nc && !bv.overflow(); ++k) {
                    auto c = bv.readRGBA(true); f.gradientColors.emplace_back(c.r, c.g, c.b, c.a);
                }
                for (int k = 0; k < nc && !bv.overflow(); ++k) f.gradientRatios.push_back(bv.readUI8());
                f.blurX = fx(); f.blurY = fx(); f.angle = fx(); f.distance = fx(); f.strength = fx();
                std::uint8_t fl = bv.readUI8();
                f.inner = (fl & 0x80) != 0; f.knockout = (fl & 0x40) != 0; f.passes = (fl & 0x1F);
                if (!bv.overflow()) f.gradientType = bv.readUI8();   // 0=Inner 1=Outer 2=Full
                out.push_back(f); } break;
            case 5: {  // Convolution
                Filter f; f.kind = FilterKind::Convolution;
                std::uint8_t mx = bv.readUI8(), my = bv.readUI8();
                f.convW = mx; f.convH = my;
                f.convDivisor = fx(); f.convBias = fx();
                int cnt = (int)mx * (int)my; f.convMatrix.resize(cnt);
                for (int k = 0; k < cnt && !bv.overflow(); ++k) f.convMatrix[k] = fx();
                auto def = bv.readRGBA(true); f.convColor = Color(def.r, def.g, def.b, def.a);
                if (!bv.overflow()) {
                    std::uint8_t fl = bv.readUI8();
                    f.convClamp = (fl & 0x20) != 0;          // clamp 位
                    f.convPreserveAlpha = (fl & 0x80) != 0;  // 保留 alpha 位
                }
                out.push_back(f); } break;
            case 6: {  // ColorMatrix（4x5 = 20 个 FLOAT16）
                Filter f; f.kind = FilterKind::ColorMatrix;
                for (int k = 0; k < 20; ++k) f.colorMatrix[k] = fx();
                out.push_back(f); } break;
            default: return;
        }
    }
}

// 解码 ScreenVideo(codec 3) 一帧为 RGBA。帧由若干 zlib 压缩块组成，每块解压后为
// blockWidth/blockHeight + 16x16 瓦片（每像素 1 位“是否变化”，变化则 3 字节 RGB）；
// 未变化像素沿用上一帧（增量编码）。
static std::shared_ptr<Bitmap> decodeScreenVideo(const std::vector<std::uint8_t>& data,
                                                 std::uint16_t width, std::uint16_t height,
                                                 const Bitmap* prev) {
    auto bmp = std::make_shared<Bitmap>();
    bmp->width = width; bmp->height = height;
    bmp->rgba.assign(static_cast<std::size_t>(width) * height * 4, 0);
    if (prev && prev->width == width && prev->height == height) {
        bmp->rgba = prev->rgba;   // 增量：未变化像素沿用上一帧
    } else {
        for (std::size_t i = 0; i < bmp->rgba.size(); i += 4) { bmp->rgba[i]=0; bmp->rgba[i+1]=0; bmp->rgba[i+2]=0; bmp->rgba[i+3]=255; }
    }
    std::size_t pos = 0;
    while (pos + 2 <= data.size()) {
        std::uint16_t blockSize = static_cast<std::uint16_t>(data[pos] | (data[pos + 1] << 8));
        pos += 2;
        if (blockSize == 0 || pos + blockSize > data.size()) break;
        std::vector<std::uint8_t> dec;
        if (!inflateZlib(data.data() + pos, blockSize, dec)) { pos += blockSize; continue; }
        pos += blockSize;
        if (dec.size() < 4) continue;
        std::uint16_t bw = static_cast<std::uint16_t>(dec[0] | (dec[1] << 8));
        std::uint16_t bh = static_cast<std::uint16_t>(dec[2] | (dec[3] << 8));
        if (bw == 0 || bh == 0) continue;
        ByteView dbv(dec.data(), dec.size()); dbv.seek(4);
        BitReader br(dbv);
        int tilesX = (bw + 15) / 16, tilesY = (bh + 15) / 16;
        for (int ty = 0; ty < tilesY && !dbv.overflow(); ++ty)
            for (int tx = 0; tx < tilesX; ++tx)
                for (int py = 0; py < 16; ++py)
                    for (int pxx = 0; pxx < 16; ++pxx) {
                        int flag = static_cast<int>(br.readUB(1));
                        int X = tx * 16 + pxx, Y = ty * 16 + py;
                        if (X >= width || Y >= height) { if (flag) { br.readUB(8); br.readUB(8); br.readUB(8); } continue; }
                        if (flag) {
                            int r = static_cast<int>(br.readUB(8));
                            int g = static_cast<int>(br.readUB(8));
                            int b = static_cast<int>(br.readUB(8));
                            std::size_t idx = (static_cast<std::size_t>(Y) * width + X) * 4;
                            bmp->rgba[idx] = static_cast<std::uint8_t>(r);
                            bmp->rgba[idx + 1] = static_cast<std::uint8_t>(g);
                            bmp->rgba[idx + 2] = static_cast<std::uint8_t>(b);
                            bmp->rgba[idx + 3] = 255;
                        }
                    }
    }
    return bmp;
}

// DefineButton2(34)：完整解析四态显示列表 + 按钮动作脚本。
// 字节布局（对照 swf.swf 实测）：外层已读 ButtonId(UI16)，剩余：
// flags(UI8) ActionOffset(UI16) BUTTONRECORD 列表（CharacterID==0 终止）按钮动作。
static std::shared_ptr<Character> decodeButton2(ByteView& bv,
                                                std::map<std::uint16_t, std::shared_ptr<Character>>& chars,
                                                int swfVer, std::size_t tagEnd) {
    auto ch = std::make_shared<Character>();
    ch->kind = Character::Kind::Button;
    auto btn = std::make_shared<ButtonDef>();
    ch->button = btn;
    std::uint8_t flags = bv.readUI8();
    std::uint16_t actionOffset = bv.readUI16();
    (void)flags; (void)swfVer;
    std::size_t recordsStart = bv.pos();
    int guard = 0;
    while (bv.pos() + 3 <= tagEnd && !bv.overflow()) {
        if (++guard > 256) break;
        std::uint8_t recFlags = bv.readUI8();
        // BUTTONRECORD 列表以"状态位全 0 的单字节"结束（ButtonRecordEnd），
        // 该字节之后紧跟按钮动作。若按 CharacterId==0 判断会越过终止符，
        // 把动作字节当成记录继续解析，凭空造出 cid/depth/矩阵全错的状态项
        // （swf.swf 商店的按钮 104 因此多出 cid=1 的黑色挖洞矩形，遮住整个商店）。
        if (recFlags == 0) break;
        std::uint16_t cid = bv.readUI16();
        if (cid == 0) break;
        std::uint16_t pdepth = bv.readUI16();
        DisplayItem it; it.depth = pdepth; it.matrix = Matrix{};
        { auto ic = chars.find(cid); it.character = (ic != chars.end()) ? ic->second : nullptr; }
        it.characterId = cid;
        it.matrix = readMatrix(bv);
        it.cxform = readCXForm(bv, true);    // DefineButton2 记录恒含 CXFORMWITHALPHA
        // 状态位：0x01 Up 0x02 Over 0x04 Down 0x08 Hit（可组合，组合时归入各桶）
        if (recFlags & 0x01) btn->up.push_back(it);
        if (recFlags & 0x02) btn->over.push_back(it);
        if (recFlags & 0x04) btn->down.push_back(it);
        if (recFlags & 0x08) btn->hit.push_back(it);
        if (bv.overflow()) break;
    }
    // 注意：不能拿 Hit 态兜底渲染 Up 态。Flash 语义中只定义 Hit 的按钮是**不可见**按钮
    // （仅提供点击热区），把 Hit 形状当 Up 画出来会显示成一大块黑色区域——正是待机钩爪
    // 下方"黑球"的来源（cl 帧 1 的按钮 248 只有 Hit 态 cid=247）。
    // 按钮动作（Button2ActionRecord：CondActionSize(UI16) + CondFlags(UI16) + Actions）
    // ActionOffset 按规范相对 ActionOffset 字段自身（位于 tag 偏移 3）。
    // 实测其它基准读到的都是作者工具残留的垃圾字节（以 End 开头的假记录），仅用规范基准。
    if (actionOffset != 0) {
        std::size_t start = (recordsStart >= 2 ? recordsStart - 2 : 0) + actionOffset;
        std::size_t j = start;
        int nrec = 0;
        while (j + 4 <= tagEnd && nrec < 32) {
            std::uint16_t sz = static_cast<std::uint16_t>(bv.base()[j] | (bv.base()[j + 1] << 8));
            std::uint16_t cond = static_cast<std::uint16_t>(bv.base()[j + 2] | (bv.base()[j + 3] << 8));
            // CondActionSize==0 表示"最后一条 CondAction"——动作字节仍紧随其后，
            // 必须一路读到 End（此前直接 break 会漏掉主菜单"开始"按钮这类单记录动作）。
            std::size_t actEnd = (sz == 0 || j + sz > tagEnd) ? tagEnd : j + sz;
            avm1::Script sc = readAvm1UntilEnd(bv, j + 4, actEnd);
            if (!sc.actions.empty()) {
                // 鼠标状态迁移位：CondFlags 的 bits 0..8 各对应一条状态迁移；
                // keyPress：bits 9..15 为按键码（keyCode = CondFlags >> 9，0 表示无按键）。
                for (int bit = 0; bit < 9; ++bit)
                    if (cond & (1 << bit)) btn->actions[bit] = sc;
                if (cond & 0xFE00) btn->keyActions[(cond & 0xFE00) >> 9] = sc;
            }
            if (sz == 0 || sz < 4 || j + sz > tagEnd) break;   // 最后一条 / 尾部残留
            j += sz; ++nrec;
        }
    }
    // 先渲染 Up 态为单帧（保持向后兼容：Kind::Button 渲染器按 buttonState 选桶）
    Frame fr;
    for (auto& it : btn->up) fr.items.push_back(it);
    std::sort(fr.items.begin(), fr.items.end(),
              [](const DisplayItem& a, const DisplayItem& b) { return a.depth < b.depth; });
    ch->spriteFrames.push_back(std::move(fr));
    return ch;
}

// 用 FFmpeg 解码一帧视频并写入 VideoStream::decoded（渲染器按帧号取用）
static void decodeVideoFrameInto(VideoStream& vs, std::uint16_t fnum, FFmpegVideoDecoder& dec) {
    std::vector<std::uint8_t> rgba;
    int w = 0, h = 0;
    if (dec.decodeFrame(vs.frames[fnum], rgba, w, h) && !rgba.empty()) {
        auto bmp = std::make_shared<Bitmap>();
        bmp->width = w; bmp->height = h; bmp->rgba = std::move(rgba);
        vs.decoded[fnum] = std::move(bmp);
    }
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
    std::vector<avm1::Script> pendingFrameActions;      // 当前帧累积的 DoAction
    std::vector<SoundEvent> pendingSoundEvents;         // 当前帧累积的 StartSound / 流式块
    int swfVersion = 6;                                 // ClipActions 事件标志宽度（<8 用 UI16）
    int streamBlockCount = 0;                           // 已见 SoundStreamBlock 数（-> 帧内块下标）

    // 当前帧的 DoAction 累积器（主时间轴用）
    avm1::Script takePendingActions() {
        avm1::Script s;
        if (!pendingFrameActions.empty()) {
            // 多段 DoAction 合并为一段脚本（按顺序执行）
            std::vector<std::uint8_t> bytes;
            for (auto& a : pendingFrameActions)
                bytes.insert(bytes.end(), a.bytes.begin(), a.bytes.end());
            s = avm1::disassemble(bytes.data(), bytes.size());
            pendingFrameActions.clear();
        }
        return s;
    }

    // 视频流解码器：每个 VideoStream 一个（帧间有依赖，必须保持状态）
    std::map<std::uint16_t, std::unique_ptr<FFmpegVideoDecoder>> videoDecoders;
    std::map<std::uint16_t, std::string> videoDecodeError;

    void snapshotFrame() {
        Frame f;
        for (auto& kv : displayList) {
            // 记录该 depth 上此角色的放置帧（精灵据此对齐内部帧）。
            // 注意必须写回 kv.second：之前只写了局部拷贝 it，
            // 导致后续帧从显示列表复制到的 birthFrame 又变回默认 0，
            // 精灵内部帧号恒等于 frameIndex，动画内容整体错位。
            if (placedChar.find(kv.first) == placedChar.end() ||
                placedChar[kv.first] != kv.second.character) {
                placedChar[kv.first] = kv.second.character;
                kv.second.birthFrame = curFrame;
            }
            f.items.push_back(kv.second);
        }
        std::sort(f.items.begin(), f.items.end(),
                  [](const DisplayItem& a, const DisplayItem& b) { return a.depth < b.depth; });
        f.doActions.push_back(takePendingActions());
        f.soundEvents = std::move(pendingSoundEvents);
        pendingSoundEvents.clear();
        movie.frames.push_back(std::move(f));
        ++curFrame;
    }
};
}  // namespace

// 解析期按 cid 查角色表，SWF 允许**前向引用**（精灵里引用的形状可能定义在其后），
// 那时查不到就会留下空角色指针，表现为该显示项渲染不出任何像素
// （swf.swf 的地面精灵 #289/#306 就是这样整层空白）。
// 解码结束后角色表已完整，这里按 characterId 统一补查并递归到精灵内部帧。
static void resolveForwardRefs(std::vector<Frame>& frames,
                               const std::map<std::uint16_t, std::shared_ptr<Character>>& chars,
                               std::vector<const Character*>& seen, int depth) {
    if (depth > 32) return;                  // 防御自引用精灵
    for (auto& f : frames) {
        for (auto& it : f.items) {
            if (!it.character && it.characterId) {
                auto c = chars.find(it.characterId);
                if (c != chars.end() && c->second) it.character = c->second;
            }
            if (it.character && it.character->kind == Character::Kind::Sprite) {
                // 同一精灵只递归一次：精灵之间会互相引用，不去重会指数级重复遍历
                // （实测会让解码从 0.4 秒膨胀到分钟级，表现为播放器"卡死/没有窗口"）。
                const Character* p = it.character.get();
                bool dup = false;
                for (const auto* q : seen) if (q == p) { dup = true; break; }
                if (dup) continue;
                seen.push_back(p);
                resolveForwardRefs(it.character->spriteFrames, chars, seen, depth + 1);
            }
        }
    }
}

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
    ctx.swfVersion = static_cast<int>(data[3]);

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
                // 当前精灵帧累积的 DoAction。SWF 中一帧的 DoAction 位于该帧 ShowFrame **之前**，
                // 若直接挂到已快照的 spriteFrames.back() 会错位一帧，且首帧（spriteFrames 尚空时）
                // 的脚本会被整段丢弃——这正是"效果精灵放置瞬间的 Stop()/gotoAndStop() 未执行、
                // 精灵自动外播露出覆盖物"的根因。故与主时间轴一致：先累积，ShowFrame 时并入该帧。
                std::vector<avm1::Script> subPendingActions;
                // 当前精灵帧累积的 StartSound：与 DoAction 一样在 ShowFrame 时并入该帧。
                std::vector<SoundEvent> subPendingSounds;
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
                        auto shp = decodeShape(bv, ver, cid);
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
                        // Move 语义：不带 HasCharacter 的 PlaceObject2 只是移动/改样式，
                        // 必须继承该 depth 上已有的角色，否则会新建一个空项，整层渲染不出来。
                        { auto prev = sub.find(it.depth); if (prev != sub.end()) it = prev->second; }
                        if (f & 0x02) { std::uint16_t cid = bv.readUI16(); auto ic = ctx.characters.find(cid); it.character = (ic != ctx.characters.end()) ? ic->second : nullptr; it.characterId = cid; }
                        if (f & 0x04) it.matrix = readMatrix(bv);
                        if (f & 0x08) it.cxform = readCXForm(bv, true);
                        if (f & 0x10) it.ratio = static_cast<float>(bv.readUI16()) / 65535.0f;
                        if (f & 0x20) { std::string nm = readStringZ(bv, tEnd); it.name = nm; }
                        if (f & 0x40) it.clipDepth = bv.readUI16();
                        if (f & 0x80) readClipActions(bv, tEnd, ctx.swfVersion, it.clipEvents);
                        sub[it.depth] = it;
                    } else if (tc == 70) {  // PlaceObject3
                        int f = bv.readUI8(); int f2 = bv.readUI8();
                        DisplayItem it; it.depth = bv.readUI16();
                        if (f2 & 0x08) { while (bv.pos() < tEnd && bv.readUI8() != 0) {} }  // ClassName
                        if (f2 & 0x01) parseFilterList(bv, it.filters);                       // FilterList
                        if (f2 & 0x02) it.blend = static_cast<BlendMode>(bv.readUI8());       // BlendMode
                        if (f2 & 0x04) bv.readUI8();                                          // CacheAsBitmap
                        if (f2 & 0x20) bv.readUI8();                                          // Visible
                        if (f2 & 0x40) { bv.readUI8(); bv.readUI8(); bv.readUI8(); bv.readUI8(); }  // OpaqueBackground
                        { auto prev = sub.find(it.depth); if (prev != sub.end()) it = prev->second; }  // Move 语义
                        if (f & 0x02) { std::uint16_t cid = bv.readUI16(); auto ic = ctx.characters.find(cid); it.character = (ic != ctx.characters.end()) ? ic->second : nullptr; it.characterId = cid; }
                        if (f & 0x04) it.matrix = readMatrix(bv);
                        if (f & 0x08) it.cxform = readCXForm(bv, true);
                        if (f & 0x10) it.ratio = static_cast<float>(bv.readUI16()) / 65535.0f;
                        if (f & 0x20) it.name = readStringZ(bv, tEnd);
                        if (f & 0x40) it.clipDepth = bv.readUI16();
                        if (f & 0x80) readClipActions(bv, tEnd, ctx.swfVersion, it.clipEvents);
                        sub[it.depth] = it;
                    } else if (tc == 5) {
                        bv.readUI16(); std::uint16_t d = bv.readUI16();
                        sub.erase(d); subPlaced.erase(d);   // 同步清理，见 RemoveObject2 处说明
                    }
                    else if (tc == 28) {
                        std::uint16_t d = bv.readUI16();
                        sub.erase(d); subPlaced.erase(d);
                    }
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
                        // 把本帧累积的 DoAction 并入该帧（多个 DoAction 按顺序合并为一段脚本）。
                        if (!subPendingActions.empty()) {
                            std::vector<std::uint8_t> bytes;
                            for (auto& a : subPendingActions)
                                bytes.insert(bytes.end(), a.bytes.begin(), a.bytes.end());
                            fr.doActions.push_back(avm1::disassemble(bytes.data(), bytes.size()));
                            subPendingActions.clear();
                        }
                        // 本帧的 StartSound 事件并入该帧（精灵内部音效，如金币/按钮音）。
                        if (!subPendingSounds.empty()) {
                            fr.soundEvents = std::move(subPendingSounds);
                            subPendingSounds.clear();
                        }
                        sp->spriteFrames.push_back(std::move(fr));
                        ++subFrame;
                    }
                    else if (tc == 12) {  // DoAction（精灵帧脚本）：累积到本帧，ShowFrame 时并入
                        subPendingActions.push_back(
                            avm1::disassemble(bv.base() + tStart, tEnd - tStart));
                    }
                    else if (tc == 15 || tc == 89) {  // StartSound / StartSound2：累积到本帧
                        SoundEvent ev;
                        if (tc == 15) {
                            ev.soundId = bv.readUI16();
                        } else {
                            (void)readStringZ(bv, tEnd);   // 导出名（按名播放，暂不支持）
                        }
                        bool stop = false;
                        ev.loop = readSoundInfoSkip(bv, tEnd, &stop);
                        ev.stop = stop;
                        if (tc == 15) subPendingSounds.push_back(ev);
                    }
                    else if (tc == 43) {  // FrameLabel：给**紧随其后**的那一帧起名（1 基帧号）
                        // 与主时间轴一致：标签名指向 subFrame+1（正在构建的那一帧）。
                        std::string nm = readStringZ(bv, tEnd);
                        if (!nm.empty()) sp->frameLabels[nm] = subFrame + 1;
                    }
                    else if (tc == 59) {  // DoInitAction（精灵初始化脚本）
                        bv.readUI16();  // spriteId
                        avm1::Script sc = avm1::disassemble(bv.base() + bv.pos(), tEnd - bv.pos());
                        ctx.movie.initActions[sid] = std::move(sc);
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
                if (std::getenv("SWF_DUMP_SHAPES")) std::printf("SHAPE id=%u ver=%d tagDataStart=%zu\n", id, ver, bv.pos());
                auto shp = decodeShape(bv, ver, id);
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
            case 34: {  // DefineButton2（四态 + 动作脚本）
                std::uint16_t id = bv.readUI16();
                auto btn = decodeButton2(bv, ctx.characters, ctx.swfVersion, doff + len);
                ctx.characters[id] = btn;
                break;
            }
            case 17: {  // DefineButtonSound：按钮三态声音
                std::uint16_t id = bv.readUI16();
                auto itc = ctx.characters.find(id);
                if (itc != ctx.characters.end() && itc->second &&
                    itc->second->kind == Character::Kind::Button && itc->second->button) {
                    auto& btn = *itc->second->button;
                    // 依次：IdleToOverUp / OverUpToOverDown / OverDownToOverUp / OverUpToIdle
                    std::uint16_t* slots[4] = {&btn.soundUp, &btn.soundOver,
                                               &btn.soundDown, &btn.soundOut};
                    for (int k = 0; k < 4 && bv.pos() + 2 <= doff + len; ++k) {
                        std::uint16_t sid = bv.readUI16();
                        readSoundInfoSkip(bv, doff + len, nullptr);
                        if (sid) *slots[k] = sid;
                    }
                }
                break;
            }
            case 10: {  // DefineFont（旧版，SWF<6）：仅字形轮廓，字符码表由随后的 DefineFontInfo(13) 提供
                std::uint16_t fid = bv.readUI16();
                std::size_t offStart = bv.pos();
                if (offStart + 2 > doff + len || bv.overflow()) break;
                std::uint16_t o0 = bv.readUI16();          // 首个偏移 = 偏移表字节长度 = nGlyphs*2
                int nGlyphs = static_cast<int>(o0) / 2;
                if (nGlyphs <= 0 || nGlyphs > 65535) break;
                std::vector<std::uint32_t> offs(static_cast<std::size_t>(nGlyphs));
                offs[0] = o0;
                for (int i = 1; i < nGlyphs; ++i) offs[i] = bv.readUI16();
                auto fg = std::make_shared<FontGlyphs>();
                fg->shapes.resize(static_cast<std::size_t>(nGlyphs));
                fg->advances.assign(static_cast<std::size_t>(nGlyphs), 0);
                for (int i = 0; i < nGlyphs; ++i) {
                    std::size_t gstart = offStart + offs[static_cast<std::size_t>(i)];
                    std::size_t gend = (i + 1 < nGlyphs)
                        ? offStart + offs[static_cast<std::size_t>(i + 1)]
                        : doff + len;
                    if (gstart >= gend || gend > bv.size()) continue;
                    ByteView gv(base, bv.size()); gv.seek(gstart);
                    auto shp = std::make_shared<Shape>();
                    parseGlyphShape(gv, shp, 1);            // 字形为裸 SHAPE（DefineShape1 格式）
                    fg->shapes[static_cast<std::size_t>(i)] = shp;
                }
                auto ch = std::make_shared<Character>();
                ch->kind = Character::Kind::Font;
                ch->font = fg;                              // codeToGlyph 暂空，待 DefineFontInfo 填充
                ctx.characters[fid] = ch;
                break;
            }
            case 48: case 75: {  // DefineFont2 / DefineFont3
                ByteView fv(base + doff, len);   // 子视图：偏移/CodeTableOffset 相对字体记录起始解析
                std::uint16_t id = fv.readUI16();
                auto fg = decodeFont(fv, base[3]);
                auto ch = std::make_shared<Character>();
                ch->kind = Character::Kind::Font;
                ch->font = fg;
                ctx.characters[id] = ch;
                break;
            }
            case 13: case 62: {  // DefineFontInfo / DefineFontInfo2：为前面的 DefineFont(10) 提供字符码表
                std::uint16_t fid = bv.readUI16();
                if (code == 62) bv.readUI8();             // DefineFontInfo2 多一个 LanguageCode
                int nameLen = bv.readUI8();
                std::string fname;
                std::size_t nameStart = bv.pos();
                for (int i = 0; i < nameLen; ++i) {
                    int c = bv.readUI8();
                    if (c == 0) break;
                    fname.push_back(static_cast<char>(c));
                }
                bv.seek(nameStart + static_cast<std::size_t>(nameLen));
                std::uint8_t fflags = bv.readUI8();
                // DefineFontInfo 的 FontFlags（MSB→LSB）：Reserved[2]、SmallText(0x20)、
                // ShiftJIS(0x10)、ANSI(0x08)、Italic(0x04)、Bold(0x02)、WideCodes(0x01)。
                bool wide = (code == 62) || (fflags & 0x01) != 0;  // DefineFontInfo2 代码表恒为 UI16
                auto it = ctx.characters.find(fid);
                if (it != ctx.characters.end() && it->second && it->second->font) {
                    FontGlyphs& fg = *it->second->font;
                    if (fg.name.empty() && !fname.empty()) fg.name = fname;
                    fg.italic = fg.italic || (fflags & 0x04) != 0;
                    fg.bold = fg.bold || (fflags & 0x02) != 0;
                    if (fg.codeToGlyph.empty()) {          // 仅当尚无代码表（旧版 DefineFont(10)）时填充
                        int n = static_cast<int>(fg.shapes.size());
                        for (int i = 0; i < n && !bv.overflow(); ++i) {
                            int cc = wide ? static_cast<int>(bv.readUI16()) : static_cast<int>(bv.readUI8());
                            fg.codeToGlyph[cc] = i;
                        }
                    }
                }
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
            case 37: {  // DefineEditText：时间轴文本框
                std::uint16_t id = bv.readUI16();
                auto et = decodeEditText(bv);
                auto ch = std::make_shared<Character>();
                ch->kind = Character::Kind::EditText;
                ch->editText = et;
                ctx.characters[id] = ch;
                break;
            }
            case 46: case 84: {  // DefineMorphShape / DefineMorphShape2
                int ver = (code == 46) ? 1 : 2;
                std::uint16_t id = bv.readUI16();
                auto ms = decodeMorphShape(bv, ver);
                auto ch = std::make_shared<Character>();
                ch->kind = Character::Kind::Morph;   // 由渲染器按 ratio 插值
                ch->morph = ms;
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
                it.characterId = cid;
                it.matrix = Matrix{};
                ctx.displayList[depth] = it;
                break;
            }
            case 26: {  // PlaceObject2
                // flags（1 字节）位定义（与真实 SWF 数据核对得出）：
                //   0x01 Move   0x02 HasCharacter 0x04 HasMatrix  0x08 HasCxform
                //   0x10 HasRatio 0x20 HasName    0x40 HasClipDepth 0x80 HasClipActions
                int f = bv.readUI8();
                // 字段序：Flags, Depth(恒有), [CharacterId 若 HasCharacter], Matrix...
                // 注意：SWF 规范写的是 CharacterId 在 Depth 之前，但本文件实测为
                // Depth 在前——证据：按规范序会把 DefineFont2（cid22/25，tag48
                // 字体"黑体"/"方正简体"）放上显示列表（非法）、矿工 char14 只有
                // depth 序才被放置（dep14←cid14，矩阵与 Adobe 基准吻合）、
                // RemoveObject2 连贯性（F1 放 dep1←cid6/dep8←cid7，F9 恰清除二者）、
                // F9 连续九条 dep10←cid10…dep18←cid18 的典型生成器模式。
                // 按"规范序"渲染会几乎全空（与基准 mean diff 241.8）。
                std::uint16_t depth = bv.readUI16();
                std::uint16_t cid = 0;
                if (f & 0x02) cid = bv.readUI16();
                // Move=1 表示"在已有显示对象上修改"（只改矩阵/比例/色彩变换等），
                // 必须保留原 character，否则会把该 depth 上的内容整个抹掉。
                // swf.swf 里有 71 个 flags=0x05（Move+矩阵、不带角色）的标签，
                // 旧实现每次都用空 character 覆盖，导致大量角色凭空消失。
                DisplayItem it;
                if ((f & 0x01) != 0) {
                    auto e = ctx.displayList.find(depth);
                    if (e != ctx.displayList.end()) it = e->second;
                }
                it.depth = depth;
                if (f & 0x02) {
                    auto itc = ctx.characters.find(cid);
                    it.character = (itc != ctx.characters.end()) ? itc->second : nullptr;
                    it.characterId = cid;
                }
                if (f & 0x04) it.matrix = readMatrix(bv);
                if (f & 0x08) it.cxform = readCXForm(bv, true);
                if (f & 0x10) it.ratio = static_cast<float>(bv.readUI16()) / 65535.0f;  // ratio
                if (f & 0x20) it.name = readStringZ(bv, doff + len);          // 实例名
                if (f & 0x40) it.clipDepth = bv.readUI16();
                if (f & 0x80) readClipActions(bv, doff + len, ctx.swfVersion, it.clipEvents);
                ctx.displayList[depth] = it;
                break;
            }
            case 70: {  // PlaceObject3
                int f = bv.readUI8();
                int f2 = bv.readUI8();
                DisplayItem it;
                it.depth = bv.readUI16();
                // PlaceFlag2 字段（按字节流顺序；位定义见 CommonLib/Adobe 规范）
                if (f2 & 0x08) { while (bv.readUI8() != 0) {} }    // ClassName (HasClassName=0x08)
                if (f2 & 0x01) parseFilterList(bv, it.filters);    // FilterList (HasFilterList=0x01)
                if (f2 & 0x02) it.blend = static_cast<BlendMode>(bv.readUI8());  // BlendMode (0x02)
                if (f2 & 0x04) bv.readUI8();                       // CacheAsBitmap (0x04)
                if (f2 & 0x20) bv.readUI8();                       // Visible (0x20)
                if (f2 & 0x40) { bv.readUI8(); bv.readUI8(); bv.readUI8(); bv.readUI8(); }  // OpaqueBackground RGBA (0x40)
                if (f & 0x02) {
                    std::uint16_t cid = bv.readUI16();
                    auto itc = ctx.characters.find(cid);
                    it.character = (itc != ctx.characters.end()) ? itc->second : nullptr;
                    it.characterId = cid;
                }
                if (f & 0x04) it.matrix = readMatrix(bv);
                if (f & 0x08) it.cxform = readCXForm(bv, true);
                if (f & 0x10) it.ratio = static_cast<float>(bv.readUI16()) / 65535.0f;  // ratio
                if (f & 0x20) it.name = readStringZ(bv, doff + len);          // 实例名
                if (f & 0x40) it.clipDepth = bv.readUI16();
                if (f & 0x80) readClipActions(bv, doff + len, ctx.swfVersion, it.clipEvents);
                ctx.displayList[it.depth] = it;
                break;
            }
            case 5: case 28: {  // RemoveObject / RemoveObject2
                // 同时清掉 placedChar：否则同一 depth 之后重新放置同一角色时，
                // 比较结果相同而不会刷新 birthFrame，精灵内部帧号会一直按旧值算，
                // 取到空帧 —— 表现为整块内容消失（swf.swf 的角色就是这样丢的）。
                if (code == 5) {
                    bv.readUI16();
                    std::uint16_t depth = bv.readUI16();
                    ctx.displayList.erase(depth);
                    ctx.placedChar.erase(depth);
                } else {
                    std::uint16_t depth = bv.readUI16();
                    ctx.displayList.erase(depth);
                    ctx.placedChar.erase(depth);
                }
                break;
            }
            case 1: {  // ShowFrame
                ctx.hasShowFrame = true;
                ctx.snapshotFrame();
                break;
            }
            case 43: {  // FrameLabel：给**紧随其后**的那一帧起名（1 基帧号）
                // 格式：STRING（以 0 结尾）+ 可选 UI8 标志（SWF>=8，bit0=命名锚点）。
                // 标签通常紧跟在它所属帧的 ShowFrame 之后，故名字指向下一帧；
                // 若出现在帧内容中间或文件开头，指向的仍是**正在构建**的那一帧。
                std::string name;
                while (bv.pos() < doff + len && !bv.overflow()) {
                    int c = bv.readUI8();
                    if (c == 0 || c == -1) break;
                    name.push_back(static_cast<char>(c));
                }
                if (!name.empty()) ctx.movie.frameLabels[name] = ctx.curFrame + 1;
                break;
            }
            case 0: {  // End
                if (!ctx.hasShowFrame) ctx.snapshotFrame();  // 无 ShowFrame 时补一帧
                goto done;
            }
            default:
                break;  // 其它标签暂忽略
            case 12: {  // DoAction（主时间轴脚本）
                avm1::Script sc = avm1::disassemble(base + doff, len);
                ctx.pendingFrameActions.push_back(std::move(sc));
                break;
            }
            case 59: {  // DoInitAction
                std::uint16_t spriteId = static_cast<std::uint16_t>(base[doff] | (base[doff + 1] << 8));
                avm1::Script sc = avm1::disassemble(base + doff + 2, len - 2);
                ctx.movie.initActions[spriteId] = std::move(sc);
                break;
            }
            case 14: {  // DefineSound
                Sound snd;
                snd.id = static_cast<std::uint16_t>(base[doff] | (base[doff + 1] << 8));
                std::uint8_t flags = base[doff + 2];
                snd.format = (flags >> 4) & 0x0F;
                snd.rate = (flags >> 2) & 0x03;
                snd.bits = (flags >> 1) & 0x01;
                snd.channels = flags & 0x01;
                snd.sampleCount = static_cast<std::uint32_t>(base[doff + 3]) | (static_cast<std::uint32_t>(base[doff + 4]) << 8)
                                | (static_cast<std::uint32_t>(base[doff + 5]) << 16) | (static_cast<std::uint32_t>(base[doff + 6]) << 24);
                int dataOff = (int)doff + 7;
                int dataLen = (int)len - 7;
                if (dataLen > 0) snd.data.assign(base + dataOff, base + dataOff + dataLen);
                ctx.movie.sounds[snd.id] = std::move(snd);
                break;
            }
            case 15: case 89: {  // StartSound / StartSound2：记录到本帧声音事件队列
                SoundEvent ev;
                if (code == 15) {
                    ev.soundId = bv.readUI16();
                } else {
                    // StartSound2：SoundId 为导出名（字符串），此处仅消费
                    readStringZ(bv, doff + len);
                }
                bool stop = false;
                ev.loop = readSoundInfoSkip(bv, doff + len, &stop);
                ev.stop = stop;
                if (ev.soundId != 0 || code == 15) ctx.pendingSoundEvents.push_back(ev);
                break;
            }
            case 18: case 45: {  // SoundStreamHead / SoundStreamHead2：记录流式声音参数
                if (len >= 2) {
                    std::uint8_t b0 = base[doff], b1 = base[doff + 1];
                    auto& sa = ctx.movie.streamAudio;
                    sa.have = true;
                    sa.rate = (b1 >> 6) & 0x03;
                    sa.bits = (b1 >> 5) & 0x01;
                    sa.channels = (b1 >> 4) & 0x01;
                    // SoundStreamHead2(45) 含 8-bit 编码字节；SoundStreamHead(18) 隐含 PCM/ADPCM
                    sa.format = (code == 45) ? (b1 & 0x0F) : 0;
                }
                break;
            }
            case 19: {  // SoundStreamBlock：累积每帧压缩音频 + 记录流事件
                if (len > 0) {
                    SoundEvent ev;
                    ev.soundId = 0;
                    ev.streamBlock = ctx.streamBlockCount++;
                    ctx.pendingSoundEvents.push_back(ev);
                    ctx.movie.streamAudio.blocks.emplace_back(base + doff, base + doff + len);
                }
                break;
            }
            case 60: {  // DefineVideoStream
                auto vs = std::make_shared<VideoStream>();
                vs->id = static_cast<std::uint16_t>(base[doff] | (base[doff + 1] << 8));
                vs->numFrames = static_cast<std::uint16_t>(base[doff + 2] | (base[doff + 3] << 8));
                vs->width = static_cast<std::uint16_t>(base[doff + 4] | (base[doff + 5] << 8));
                vs->height = static_cast<std::uint16_t>(base[doff + 6] | (base[doff + 7] << 8));
                std::uint8_t vf = base[doff + 8];
                // SWF VideoFlags（1 字节）：低 4 位 = VideoCodecID
                //   2=H.263/Sorenson 3=ScreenVideo 4=VP6 5=VP6+alpha 6=ScreenVideo2 7=AVC
                // 之上两位分别是 Smoothing(bit4) 与 Deblocking(bit5)，高位保留。
                vs->codecId = vf & 0x0F;
                vs->smoothing = (vf & 0x10) != 0;
                vs->deblocking = (vf & 0x20) != 0;
                ctx.movie.videos[vs->id] = vs;
                // 同时注册为角色，便于 PlaceObject2 放置并由渲染器绘制当前帧
                auto vch = std::make_shared<Character>();
                vch->kind = Character::Kind::Video;
                vch->video = vs;
                ctx.characters[vs->id] = vch;
                break;
            }
            case 61: {  // VideoFrame
                std::uint16_t vid = static_cast<std::uint16_t>(base[doff] | (base[doff + 1] << 8));
                std::uint16_t fnum = static_cast<std::uint16_t>(base[doff + 2] | (base[doff + 3] << 8));
                auto vit = ctx.movie.videos.find(vid);
                if (vit != ctx.movie.videos.end()) {
                    int dataOff = (int)doff + 4;
                    int dataLen = (int)len - 4;
                    if (dataLen > 0) {
                        auto& vs = *vit->second;
                        vs.frames[fnum].assign(base + dataOff, base + dataOff + dataLen);
                        std::uint8_t vcid = vs.codecId;
                        if (vcid == 3) {  // ScreenVideo：纯 C++ 解码（增量）
                            const Bitmap* prev = nullptr;
                            auto pit = vs.decoded.lower_bound(fnum);
                            if (pit != vs.decoded.begin()) { --pit; prev = pit->second.get(); }
                            vs.decoded[fnum] = decodeScreenVideo(
                                vs.frames[fnum], vs.width, vs.height, prev);
                        } else if (vcid == 2 || vcid == 4 || vcid == 5 || vcid == 7) {
                            // H.263(FLV1) / VP6 / VP6+alpha / AVC(H.264)：经 FFmpeg 解码
                            auto& dec = ctx.videoDecoders[vs.id];
                            if (!dec) {
                                dec = std::make_unique<FFmpegVideoDecoder>();
                                // AVC：本帧即 AVCDecoderConfigurationRecord（作为 extradata），不输出画面
                                const std::vector<std::uint8_t>* extra =
                                    (vcid == 7) ? &vs.frames[fnum] : nullptr;
                                std::string e;
                                if (!dec->open(vcid, vs.width, vs.height,
                                               extra ? *extra : std::vector<std::uint8_t>{}, e)) {
                                    ctx.videoDecodeError[vs.id] = e;
                                    dec.reset();
                                } else if (vcid != 7) {
                                    decodeVideoFrameInto(vs, fnum, *dec);
                                }
                            } else if (dec) {
                                decodeVideoFrameInto(vs, fnum, *dec);
                            }
                        }
                    }
                }
                break;
            }
            case 86: {  // DefineSceneAndFrameLabelData
                ByteView fb(base, avail); fb.seek(doff);
                auto readStr = [&]() {
                    std::string s; int c;
                    while ((c = fb.readUI8()) != 0 && c != -1) s.push_back((char)c);
                    return s;
                };
                std::uint32_t sceneCount = fb.readUI32();
                for (std::uint32_t i = 0; i < sceneCount; ++i) { fb.readUI32(); readStr(); }
                std::uint32_t labelCount = fb.readUI32();
                std::uint8_t ver = base[3];
                for (std::uint32_t i = 0; i < labelCount; ++i) {
                    std::uint16_t fn = fb.readUI16();
                    std::string name = readStr();
                    if (ver >= 8 && fb.pos() < doff + len) fb.readUI8();  // UTF-8 标志
                    ctx.movie.frameLabels[name] = (int)fn + 1;
                }
                break;
            }
            case 88: {  // DefineFontName：为已有 FontId 补上字体族名
                ByteView fb(base, avail); fb.seek(doff);
                std::uint16_t fontId = fb.readUI16();
                std::string name; int c;
                while ((c = fb.readUI8()) != 0 && c != -1) name.push_back((char)c);
                if (!name.empty()) {
                    ctx.movie.fontNames[fontId] = name;
                    auto fit = ctx.characters.find(fontId);
                    if (fit != ctx.characters.end() && fit->second && fit->second->font &&
                        fit->second->font->name.empty())
                        fit->second->font->name = std::move(name);
                }
                break;
            }
        }
        pos = cursor + len;
    }
done:
    if (ctx.movie.frames.empty()) ctx.snapshotFrame();
    ctx.movie.characters = std::move(ctx.characters);
    { std::vector<const Character*> seen;
      resolveForwardRefs(ctx.movie.frames, ctx.movie.characters, seen, 0); }
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
