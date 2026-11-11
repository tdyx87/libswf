// SWF 解码器：把字节流还原成"字典 + 时间轴帧"，供软件光栅器渲染。
// 覆盖：DefineShape(1-4) / DefineBits(JPEG) / DefineBitsLossless(2) / PlaceObject(1/2/3)
//       / RemoveObject(2) / ShowFrame / SetBackgroundColor / JPEGTables。
#include "swf/Decoder.hpp"

#include <algorithm>
#include <climits>
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
#include "swf/TagReader.hpp"

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
void Matrix::decompose(double& outScaleX, double& outScaleY, double& outRotation) const {
    const double a = sx, d = sy, b = r0, c = r1;
    double sX = std::sqrt(a * a + b * b);          // hypot(ScaleX, RotateSkew1)
    if (sX == 0) sX = 1;
    double sY = (a * d - b * c) / sX;              // det/scaleX：保留镜像符号
    if (sY == 0) sY = 1;
    // 旋转角取自 RotateSkew1(r0)：RotateSkew0(r1) 可能含错切分量。
    const double rot = std::atan2(-b, a) * 180.0 / 3.14159265358979323846;
    outScaleX = sX; outScaleY = sY; outRotation = rot;
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
        float skew0 = static_cast<float>(br.readSB(nr));   // 文件第一个 = RotateSkew0
        float skew1 = static_cast<float>(br.readSB(nr));   // 文件第二个 = RotateSkew1
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
    // MorphShape 解码时开启：额外记录**未拍平**的原始形状记录流，供逐记录插值。
    bool recordRaw = false;

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
    // 原始记录（仅 recordRaw）：与 fillPaths 的拍平结果并行维护。
    void rawRec(unsigned char kind, float x, float y, float cx, float cy) {
        if (!recordRaw || !shape) return;
        Shape::RawRec r; r.kind = kind; r.x = x; r.y = y; r.cx = cx; r.cy = cy;
        shape->rawRecs.push_back(r);
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
        bool changed = false;
        if (has0 && nf0 != actFill0) {
            if (actFill0) pushFill(keyOf(actFill0, 0), openFill[keyOf(actFill0, 0)]);
            actFill0 = nf0;
            if (actFill0) ensure(openFill[keyOf(actFill0, 0)], penX, penY);
            changed = true;
        }
        if (has1 && nf1 != actFill1) {
            if (actFill1) pushFill(keyOf(actFill1, 1), openFill[keyOf(actFill1, 1)]);
            actFill1 = nf1;
            if (actFill1) ensure(openFill[keyOf(actFill1, 1)], penX, penY);
            changed = true;
        }
        if (hasL && nl != actLine) {
            if (actLine > 0) { pushLine(actLine, openLine[actLine]); openLine.erase(actLine); }
            actLine = nl;
            if (actLine > 0) ensure(openLine[actLine], penX, penY);
            changed = true;
        }
        if (recordRaw && shape && changed) {
            // 样式变更也占一条记录，保证起止两段的记录流逐条对应
            Shape::RawRec r;
            r.kind = 3; r.f0 = nf0; r.f1 = nf1; r.ln = nl;
            r.has = static_cast<unsigned char>((has0 ? 1 : 0) | (has1 ? 2 : 0) | (hasL ? 4 : 0));
            shape->rawRecs.push_back(r);
        }
    }
    void moveTo(float x, float y) {
        rawRec(0, x, y, 0, 0);
        finalizeAll();
        penX = x; penY = y;
        // 新的子路径从笔位置起头；只给当前激活的样式开路径
        if (actFill0) ensure(openFill[keyOf(actFill0, 0)], x, y);
        if (actFill1) ensure(openFill[keyOf(actFill1, 1)], x, y);
        if (actLine)  ensure(openLine[actLine], x, y);
    }
    void lineTo(float x, float y) {
        rawRec(1, x, y, 0, 0);
        addLineTo(x, y);
        penX = x; penY = y;
    }
    void curveTo(float cx, float cy, float ax, float ay) {
        // 原始记录先落（拍平循环走 addLineTo，不产生额外记录）：控制点与锚点各自
        // 独立插值，才是 Flash 的 morph 语义。
        rawRec(2, ax, ay, cx, cy);
        // 二次贝塞尔拍平：段数由控制点到弦的偏离（矢高 sag，twips）决定。
        // 几何推导：二次曲线总弦的矢高 h，整段 N 等分后每小段矢高约 h/N²
        //（控制点结构按 1/N² 缩放）；弦高差（折线最大偏差）≈ 矢高/2 = h/(2N²)。
        // 令 h/(2N²) ≤ 0.5 twip（SWF 坐标 1/20 像素）得 N ≈ sqrt(h)，即 sqrt(2·sag)。
        // 纯几何依据，与具体播放器行为无关。
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
    // GRADIENT 头：bits7-6=SpreadMode, bits5-4=InterpolationMode, bits3-0=NumGradients
    fs.spreadMode = static_cast<std::uint8_t>((hdr >> 6) & 0x03);
    fs.interpMode = static_cast<std::uint8_t>((hdr >> 4) & 0x03);
    int n = hdr & 0x0F;
    for (int i = 0; i < n; ++i) {
        GradientStop s;
        s.ratio = bv.readUI8();
        readColor(bv, alpha, s.color);
        fs.gradient.push_back(s);
    }
    // 焦点渐变（0x13）在渐变记录之后还有一个 FocalPoint（FIXED8，2 字节，范围 -1..1，
    // 沿渐变矩阵 x 轴）。此处必须按 2 字节读，否则每次焦点渐变都会多吃 2 字节，
    // 后续填充/线样式表与形状记录全部错位（曾把 id=670 的 4056 字节形状解析出 1482 条线）。
    if (focal) {
        fs.focalX = static_cast<std::int16_t>(bv.readUI16()) / 256.0f;
        fs.focalY = 0.0f;
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
        // 0x40 平滑重复 / 0x41 平滑裁剪 / 0x42 非平滑重复 / 0x43 非平滑裁剪
        case 0x40: case 0x42: fs.kind = FillKind::Bitmap; fs.tiled = true;  readBitmapFill(bv, fs); break;
        case 0x41: case 0x43: fs.kind = FillKind::Bitmap; fs.tiled = false; readBitmapFill(bv, fs); break;
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
    // LINESTYLE2 的位字段到此结束（共 16 位）；之后直接是 Color / FILLSTYLE。
    // 若在此多读一位“模糊标志”，后续颜色与整个形状记录流都会错开一字节，
    // 记录流会读成垃圾（曾把 id=414 的 119 字节形状解析出 948 个填充样式）。
    br.finish();  // 统一对齐到字节边界，再按字节读颜色/填充
    if (hasFill) {
        readFillStyle(bv, true);  // 消费字节（线填充）
    } else {
        readColor(bv, true, ls.color);
    }
    return ls;
}

// ---------- MORPH 样式（DefineMorphShape 专用，与 FILLSTYLE/LINESTYLE 不同） ----------
// 规范（对照 Ruffle read_morph_fill_style / read_morph_line_style）：每条记录同时含
// 起始与终止两套值，**共享**类型字节/CharacterId/GradientFlags，渐变 stop 为交错排列。
// 用两条普通 readFillStyle 读取会整体错位（床单圆点黑点根因，cid=1287）。
static void readMorphFillStyle(ByteView& bv, FillStyle& s, FillStyle& e) {
    std::uint8_t t = bv.readUI8();
    switch (t) {
        case 0x00:
            s.kind = e.kind = FillKind::Solid;
            readColor(bv, true, s.color);
            readColor(bv, true, e.color);
            break;
        case 0x10: case 0x12: case 0x13: {
            s.kind = (t == 0x10) ? FillKind::LinearGradient : FillKind::RadialGradient;
            e.kind = s.kind;
            s.gradientMatrix = readMatrix(bv);
            e.gradientMatrix = readMatrix(bv);
            std::uint8_t hdr = bv.readUI8();          // 共享 Spread/Interp/NumControlPoints
            s.spreadMode = e.spreadMode = static_cast<std::uint8_t>((hdr >> 6) & 0x03);
            s.interpMode = e.interpMode = static_cast<std::uint8_t>((hdr >> 4) & 0x03);
            int n = hdr & 0x0F;
            for (int i = 0; i < n; ++i) {             // 交错：start ratio+color, end ratio+color
                GradientStop gs, ge;
                gs.ratio = bv.readUI8(); readColor(bv, true, gs.color);
                ge.ratio = bv.readUI8(); readColor(bv, true, ge.color);
                s.gradient.push_back(gs); e.gradient.push_back(ge);
            }
            if (t == 0x13) {                          // 焦点渐变：Start/EndFocalPoint 各 FIXED8
                s.kind = e.kind = FillKind::FocalGradient;
                s.focalX = static_cast<std::int16_t>(bv.readUI16()) / 256.0f;
                e.focalX = static_cast<std::int16_t>(bv.readUI16()) / 256.0f;
            }
            break;
        }
        case 0x40: case 0x41: case 0x42: case 0x43:
            s.kind = e.kind = FillKind::Bitmap;
            s.tiled = e.tiled = (t == 0x40 || t == 0x42);
            s.bitmapId = e.bitmapId = bv.readUI16();  // 共享 CharacterId
            s.gradientMatrix = readMatrix(bv);        // StartMatrix
            e.gradientMatrix = readMatrix(bv);        // EndMatrix
            break;
        default:
            s.kind = e.kind = FillKind::Solid;
            break;
    }
}

static void readMorphLineStyle(ByteView& bv, LineStyle& s, LineStyle& e, int shapeVer) {
    s.width = static_cast<float>(bv.readUI16());     // StartWidth
    e.width = static_cast<float>(bv.readUI16());     // EndWidth
    if (shapeVer < 2) {
        readColor(bv, true, s.color);
        readColor(bv, true, e.color);
        return;
    }
    // DefineMorphShape2：共享 LINESTYLE2 位字段（16 位）
    BitReader br(bv);
    br.readUB(2);                                     // StartCapStyle
    int join = static_cast<int>(br.readUB(2));
    int hasFill = static_cast<int>(br.readUB(1));
    br.readUB(1); br.readUB(1); br.readUB(1);         // NoVScale/NoHScale/PixelHinting
    br.readUB(5);                                     // reserved
    br.readUB(1);                                     // NoClose
    br.readUB(2);                                     // EndCapStyle
    if (join == 2) br.readUB(16);                     // 共享 MiterLimitFactor
    br.finish();
    if (hasFill) {
        FillStyle fs, fe;                             // 线填充也是 morph 对
        readMorphFillStyle(bv, fs, fe);
    } else {
        readColor(bv, true, s.color);
        readColor(bv, true, e.color);
    }
}

static std::shared_ptr<Shape> decodeShape(ByteView& bv, int shapeVer, std::uint16_t shapeId = 0) {
    auto shape = std::make_shared<Shape>();
    bool alpha = (shapeVer >= 3);

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
                // SWF 曲线边：AnchorDelta 相对**控制点**（而非笔点），
                // anchor = pen + ctrlDelta + anchorDelta
                sb.curveTo(sb.penX + cx, sb.penY + cy,
                           sb.penX + cx + ax, sb.penY + cy + ay);
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

// 解析变形形状的单段（起始段在前；终止段紧跟哑 UI8 之后，顺序两段）。
// 权威依据（Ruffle read_shape_record）：morph 记录标志**不含 MorphFlag 位**——
//   StyleChange = tf(1)+sn(1)+sl(1)+f1(1)+f0(1)+mv(1)；Edge = tf(1)+st(1)+nb(4)。
//   5 标志全 0 = EndShapeRecord；StateNewStyles = 普通样式数组 + 1 打包 UI8 位宽（非成对）。
static void parseMorphSection(ByteView& bv, int shapeVer, ShapeBuilder& sb,
                              int& numFillBits, int& numLineBits) {
    BitReader br(bv);
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
            if (!(stateNew || stateLine || stateFill1 || stateFill0 || stateMove)) {
                sb.finalizeAll();
                break;   // EndShapeRecord
            }
            if (stateMove) {
                int mb = static_cast<int>(br.readUB(5));
                float x = static_cast<float>(br.readSB(mb));
                float y = static_cast<float>(br.readSB(mb));
                sb.moveTo(x, y);
            }
            // 索引语义与 decodeShape 一致：1 基 + 批内基址（fillBase/lineBase）
            int raw0 = stateFill0 ? static_cast<int>(br.readUB(numFillBits)) : 0;
            int raw1 = stateFill1 ? static_cast<int>(br.readUB(numFillBits)) : 0;
            int rawL = stateLine ? static_cast<int>(br.readUB(numLineBits)) : 0;
            int nf0 = stateFill0 ? (raw0 ? sb.fillBase + raw0 : 0) : sb.actFill0;
            int nf1 = stateFill1 ? (raw1 ? sb.fillBase + raw1 : 0) : sb.actFill1;
            int nl = stateLine ? (rawL ? sb.lineBase + rawL : 0) : sb.actLine;
            sb.styleChange(nf0, nf1, nl, stateFill0 != 0, stateFill1 != 0, stateLine != 0);
            if (stateNew) {
                // 与 decodeShape 一致：索引位读完后先 byte_align（BitReader 已预读 4 字节，
                // 必须先对齐把 bv 落回真实位位置），再用字节模式读样式数组 + nb 字节。
                br.alignToByte();
                int nfc = bv.readUI8(); if (nfc == 0xFF && shapeVer >= 2) nfc = bv.readUI16();
                if (nfc < 0 || nfc > 1024) break;
                {
                    int old = static_cast<int>(sb.shape->fillStyles.size());
                    sb.fillBase = old;
                    sb.shape->fillStyles.resize(old + nfc); sb.shape->fillPaths.resize(old + nfc);
                    sb.shape->fillEdges.resize(old + nfc);
                    for (int i = 0; i < nfc; ++i) sb.shape->fillStyles[old + i] = readFillStyle(bv, false);
                    int nlc = bv.readUI8(); if (nlc == 0xFF && shapeVer >= 2) nlc = bv.readUI16();
                    if (nlc < 0 || nlc > 1024) break;
                    int oldl = static_cast<int>(sb.shape->lineStyles.size());
                    sb.lineBase = oldl;
                    sb.shape->lineStyles.resize(oldl + nlc); sb.shape->strokePaths.resize(oldl + nlc);
                    for (int i = 0; i < nlc; ++i) sb.shape->lineStyles[oldl + i] = readLineStyle(bv, shapeVer);
                    int nb2 = bv.readUI8();
                    numFillBits = nb2 >> 4;
                    numLineBits = nb2 & 0x0F;
                }
            }
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
                float cx = static_cast<float>(br.readSB(nbits)), cy = static_cast<float>(br.readSB(nbits)), ax = static_cast<float>(br.readSB(nbits)), ay = static_cast<float>(br.readSB(nbits));
                // 与 decodeShape 一致：AnchorDelta 相对控制点（规范 SHAPERECORD）
                sb.curveTo(sb.penX + cx, sb.penY + cy, sb.penX + cx + ax, sb.penY + cy + ay);
            }
        }
    }
    br.finish();
}

// DefineMorphShape(46) / DefineMorphShape2(84)：变形形状。
// 记录流含起始(段)与终止(Offset 处)两套几何，样式索引对齐。产出 t=0/t=1 两形状供插值。
static std::shared_ptr<MorphShape> decodeMorphShape(ByteView& bv, int shapeVer, std::uint16_t shapeId = 0) {
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
    for (int i = 0; i < fc; ++i)
        readMorphFillStyle(bv, ms->start->fillStyles[i], ms->end->fillStyles[i]);
    int lc = bv.readUI8(); if (lc == 0xFF) lc = bv.readUI16();
    if (lc < 0 || lc > 4096 || bv.overflow()) return ms;
    ms->start->lineStyles.resize(lc); ms->start->strokePaths.resize(lc);
    ms->end->lineStyles.resize(lc);   ms->end->strokePaths.resize(lc);
    for (int i = 0; i < lc; ++i)
        readMorphLineStyle(bv, ms->start->lineStyles[i], ms->end->lineStyles[i], shapeVer);
    // SHAPEMORPHSHAPEWITHSTYLE：线样式表之后还有 1 个字节 = NumFillBits(高4位)|NumLineBits(低4位)，
    // 必须从流里读出来（与 decodeShape 一致），不能按 neededBits 估算。此前漏读这 1 字节，
    // 使 MORPHSHAPERECORD 位流自起点整体错位 1 字节，StateNewStyles 追加的位图填充
    // 全部解码成垃圾 bitmapId（床单圆点黑点根因，cid=1287）。
    int nb = bv.readUI8();
    int numFillBits = nb >> 4;
    int numLineBits = nb & 0x0F;

    // 两段都额外记录**未拍平**的原始形状记录流，供 interpolateMorph 逐记录插值。
    sbS.recordRaw = true;
    sbE.recordRaw = true;
    // 每段开头补一条"初始样式状态"记录（kind=4）：终止段的样式索引是从起始段
    // 末尾继承来的、并不写在自己的记录流里，回放时必须显式给出，否则第一段
    // MoveTo 之后所有边都会落到"无填充"。两段各一条，保持记录流逐条对应。
    {
        Shape::RawRec r; r.kind = 4; r.f0 = 0; r.f1 = 0; r.ln = 0;
        ms->start->rawRecs.push_back(r);
    }
    parseMorphSection(bv, shapeVer, sbS, numFillBits, numLineBits);   // 起始段
    // 终止段**不 seek 到 Offset**（对照 Ruffle read_define_morph_shape）：起始段的
    // EndShapeRecord 之后紧跟 1 个打包字节（本片写 0，即终止段的 NumFillBits|NumLineBits），
    // 终止段记录从当前位流位置继续。MORPH 两段共用同一张样式表，样式索引位宽沿用起始段。
    // 关键修复：终止段是独立的 ShapeBuilder，若其第一段不含 StateFill0（本片的第一个
    // 记录就是 MoveTo），sbE.actFill0 会保持初值 0，所有边落到"无填充"，m.end 几何全空
    // -> Morph 中间帧无法插值、进度条只能固定在末帧再手工缩放。这里把起始段建立的当前
    // 填充/线样式继承给终止段，等价于 Flash 的"两段样式状态连续"语义。
    bv.readUI8();
    sbE.actFill0 = sbS.actFill0;
    sbE.actFill1 = sbS.actFill1;
    sbE.actLine = sbS.actLine;
    {
        Shape::RawRec r; r.kind = 4; r.f0 = sbS.actFill0; r.f1 = sbS.actFill1; r.ln = sbS.actLine;
        ms->end->rawRecs.push_back(r);
    }
    parseMorphSection(bv, shapeVer, sbE, numFillBits, numLineBits);
    (void)endOffset; (void)recStart; (void)shapeId;
    return ms;
}

// MorphShape 的逐记录插值（首选路径）。
// SWF 规范保证 morph 起止两段的形状记录一一对应；把每条记录的锚点/控制点线性
// 插值后再按插值结果拍平，才是 Flash 的 shape tween 语义。
// 不能反过来对"拍平后的点"插值：拍平段数 N 由矢高决定，起止形态弯曲程度不同
// 就会段数不同，逐点对齐既错位又会把合法 morph 误判为不可插值
// （loading_mc c1269 的扇形遮罩 c1252/1253/1258 因此整段冻结）。
// 记录数量或类型不对应、或没有原始记录时返回 false，调用方退回逐点插值。
bool interpolateMorphRecords(const MorphShape& m, float t, Shape& out) {
    if (!m.start || !m.end) return false;
    // 配对策略：只按**几何记录**（Move/Line/Curve）逐条对应，样式状态作为每条记录
    // 的附属信息带过去，样式变更记录本身不参与配对。
    // 原因：终止段沿用起始段末尾的样式状态（由 kind=4 承载），于是起始段里那些
    // "把索引从 A 改成 B"的记录在终止段根本不出现（索引已经是 B，无变化），两段
    // 的记录条数天然不等。而 SWF 规范保证的是**几何记录**逐条对应。
    struct Item { unsigned char kind; float x, y, cx, cy; int f0, f1, ln; };
    auto flatten = [](const std::vector<Shape::RawRec>& in) {
        std::vector<Item> r;
        r.reserve(in.size());
        int f0 = 0, f1 = 0, ln = 0;
        for (const auto& rec : in) {
            switch (rec.kind) {
                case 4: f0 = rec.f0; f1 = rec.f1; ln = rec.ln; break;
                case 3:
                    if (rec.has & 1) f0 = rec.f0;
                    if (rec.has & 2) f1 = rec.f1;
                    if (rec.has & 4) ln = rec.ln;
                    break;
                default: r.push_back(Item{rec.kind, rec.x, rec.y, rec.cx, rec.cy, f0, f1, ln}); break;
            }
        }
        return r;
    };
    const std::vector<Item> a = flatten(m.start->rawRecs);
    const std::vector<Item> b = flatten(m.end->rawRecs);
    if (a.empty() || a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const unsigned char ka = a[i].kind, kb = b[i].kind;
        // 允许同种记录，以及直边↔曲边互变；MoveTo 与边记录互不对应属结构不符。
        const bool edgePair = (ka >= 1 && ka <= 2) && (kb >= 1 && kb <= 2);
        if (ka != kb && !edgePair) return false;
    }
    auto lerpF = [](float x, float y, float tt) { return x + (y - x) * tt; };
    auto sh = std::make_shared<Shape>();
    sh->bounds.x0 = static_cast<std::int32_t>(lerpF(static_cast<float>(m.start->bounds.x0), static_cast<float>(m.end->bounds.x0), t));
    sh->bounds.y0 = static_cast<std::int32_t>(lerpF(static_cast<float>(m.start->bounds.y0), static_cast<float>(m.end->bounds.y0), t));
    sh->bounds.x1 = static_cast<std::int32_t>(lerpF(static_cast<float>(m.start->bounds.x1), static_cast<float>(m.end->bounds.x1), t));
    sh->bounds.y1 = static_cast<std::int32_t>(lerpF(static_cast<float>(m.start->bounds.y1), static_cast<float>(m.end->bounds.y1), t));
    sh->usesWinding = m.start->usesWinding;
    // 样式表沿用起始段（颜色/渐变矩阵由调用方按同一 t 插值后覆盖），
    // 几何容器按样式数量预置，ShapeBuilder 的索引边界检查依赖其大小。
    sh->fillStyles = m.start->fillStyles;
    sh->lineStyles = m.start->lineStyles;
    sh->fillPaths.resize(sh->fillStyles.size());
    sh->fillEdges.resize(sh->fillStyles.size());
    sh->strokePaths.resize(sh->lineStyles.size());
    ShapeBuilder sb;
    sb.fills = &sh->fillStyles;
    sb.lines = &sh->lineStyles;
    sb.shape = sh;
    // 直线↔曲线互变：Flash 的 shape tween 允许一条直边渐变为曲边（loading 扇形
    // 遮罩 c1252/1253/1258 就是这样：两个底点不动，闭合边由直变鼓）。把直边视作
    // "控制点落在弦中点"的退化二次曲线 —— 中点控制点使 B(t) 恰为线性插值，因此
    // t=0 精确还原起始直边、t=1 精确还原终止曲边，中间帧连续过渡。
    float penX = 0, penY = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const Item& ra = a[i];
        const Item& rb = b[i];
        // 样式状态随几何记录一起给出（styleChange 内部只在索引真的变化时才闭合/
        // 重开路径，值相同即无副作用），保证中间帧的样式归属与起止两端一致。
        sb.styleChange(ra.f0, ra.f1, ra.ln, true, true, true);
        const float nx = lerpF(ra.x, rb.x, t), ny = lerpF(ra.y, rb.y, t);
        if (ra.kind == 0) {
            sb.moveTo(nx, ny);
            penX = nx; penY = ny;
            continue;
        }
        if (ra.kind == 1 && rb.kind == 1) {
            sb.lineTo(nx, ny);
        } else if (ra.kind == 2 && rb.kind == 2) {
            sb.curveTo(lerpF(ra.cx, rb.cx, t), lerpF(ra.cy, rb.cy, t), nx, ny);
        } else if (ra.kind == 1 && rb.kind == 2) {
            sb.curveTo(lerpF((penX + nx) * 0.5f, rb.cx, t),
                       lerpF((penY + ny) * 0.5f, rb.cy, t), nx, ny);
        } else if (ra.kind == 2 && rb.kind == 1) {
            sb.curveTo(lerpF(ra.cx, (penX + nx) * 0.5f, t),
                       lerpF(ra.cy, (penY + ny) * 0.5f, t), nx, ny);
        } else {
            return false;
        }
        penX = nx; penY = ny;
    }
    sb.finalizeAll();
    out = std::move(*sh);
    return true;
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
static std::shared_ptr<FontGlyphs> decodeFont(ByteView& bv, int swfVersion, bool font3) {
    auto fg = std::make_shared<FontGlyphs>();
    // DefineFont3 把字形坐标（以及 HasLayout 度量表）放大到 DefineFont2 的 20 倍，
    // 即 EM square = 1024 * 20，与 twips 对齐；DefineFont2 仍是规范默认的 1024。
    // 两者都用 fontHeight/unitsPerEm 把字形坐标换算成 twips，单位弄错会让 DefineText
    // 与 EditText 的嵌入字形整体放大 20 倍（swf2.swf 首帧的 "LOADING"/"0%" 即如此）。
    fg->unitsPerEm = font3 ? kFontEmSquare * 20 : kFontEmSquare;
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
        // FontFlagsHasLayout 的布局表紧跟 CodeTable（SWF 规范 DefineFont2/3）：
        //   SI16 Ascent, SI16 Descent, SI16 Leading,
        //   SI16 FontAdvanceTable[NumGlyphs], RECT FontBoundsTable[NumGlyphs],
        //   UI16 KerningCount, KERNINGRECORD[Count]（码宽由 WideCodes 决定, SI16 调整量）
        // 这些值与字形轮廓同处 EM square 坐标系，渲染时统一乘 fontHeight/unitsPerEm。
        if (hasLayout && codeTablePos + need + 6 <= bv.size()) {
            ByteView lv(bv.base(), bv.size());
            lv.seek(codeTablePos + need);
            fg->hasLayout = true;
            fg->ascent  = static_cast<std::int16_t>(lv.readUI16());
            fg->descent = static_cast<std::int16_t>(lv.readUI16());
            fg->leading = static_cast<std::int16_t>(lv.readUI16());
            for (int i = 0; i < numGlyphs && !lv.overflow(); ++i)
                fg->advances[static_cast<std::size_t>(i)] = static_cast<std::int16_t>(lv.readUI16());
            // FontBoundsTable：每个 RECT 按字节对齐，用 readRect 精确推进（值排版不需要）
            for (int i = 0; i < numGlyphs && !lv.overflow(); ++i) (void)readRect(lv);
            if (!lv.overflow()) {
                int kcount = static_cast<int>(lv.readUI16());
                for (int i = 0; i < kcount && !lv.overflow(); ++i) {
                    int c1 = wideCodes ? static_cast<int>(lv.readUI16()) : static_cast<int>(lv.readUI8());
                    int c2 = wideCodes ? static_cast<int>(lv.readUI16()) : static_cast<int>(lv.readUI8());
                    int adj = static_cast<std::int16_t>(lv.readUI16());
                    fg->kerning[(static_cast<std::uint32_t>(c1) << 16) |
                                static_cast<std::uint16_t>(c2)] = adj;
                }
            }
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
    if (hasFontClass) et->fontClass = readStr();   // 设备字体类名（字体替换时用）
    if (hasFont || hasFontClass) et->fontHeight = bv.readUI16();
    if (hasTextColor) { auto c = bv.readRGBA(true); et->color = Color(c.r, c.g, c.b, c.a); }
    if (hasMaxLength) et->maxLength = bv.readUI16();
    if (hasLayout) {
        et->hasLayout  = true;
        et->align      = bv.readUI8();                                          // UI8: Left/Right/Center/Justify
        et->leftMargin = bv.readUI16();
        et->rightMargin = bv.readUI16();
        et->indent     = static_cast<std::int16_t>(bv.readUI16());             // SI16
        et->leading    = static_cast<std::int16_t>(bv.readUI16());             // SI16（twips）
    }
    // VariableName 在 InitialText 之前（脚本写同名变量时文本随之更新）。
    et->variableName = readStr();
    if (hasText) et->text = readStr();
    return et;
}

// 解析 FILTERLIST（SWF 规范 FILTER / FILTERLIST）。
// 注意字段类型：BlurX/BlurY/Angle/Distance 为 FIXED(16.16)，Strength 为 FIXED8(SI16/256)，
// Convolution 的 Divisor/Bias/Matrix 与 ColorMatrix 的 20 个值都是 32 位 FLOAT。
static void parseFilterList(ByteView& bv, std::vector<Filter>& out) {
    std::uint8_t n = bv.readUI8();
    // SWF 规范里 FILTER 的 BlurX/BlurY/Distance 是 **像素**（16.16），Angle 是 **弧度**。
    // 内部渲染约定为 twips / 角度（见 applyFilters：/20 取像素、角度化弧度），在此统一换算。
    // 若不做换算，真实文件的 4px 辉光会被当成 0.2px 取整为 0：辉光失去扩散，
    // 被本体图形完全盖住（swf2.swf play 按钮 150 Over 态的黄色 GlowFilter"黄边"消失）。
    auto fx = [&]() { return bv.readSI32() / 65536.0f * 20.0f; };            // px -> twips
    auto fang = [&]() { return bv.readSI32() / 65536.0f * 57.29577951308232f; }; // rad -> deg
    auto fx8 = [&]() { return static_cast<std::int16_t>(bv.readUI16()) / 256.0f; };  // FIXED8
    auto rf = [&]() {                                                     // FLOAT32
        std::uint32_t bits = bv.readUI32();
        float v; std::memcpy(&v, &bits, sizeof(v));
        return v;
    };
    for (int i = 0; i < n && !bv.overflow(); ++i) {
        std::uint8_t id = bv.readUI8();
        Filter f;
        switch (id) {
            case 0: {  // DropShadow
                auto c = bv.readRGBA(true); f.kind = FilterKind::DropShadow; f.color = Color(c.r, c.g, c.b, c.a);
                f.blurX = fx(); f.blurY = fx(); f.angle = fang(); f.distance = fx(); f.strength = fx8();
                // 位：InnerShadow(7) Knockout(6) CompositeSource(5) Passes(4..0)
                std::uint8_t fl = bv.readUI8(); f.inner = (fl & 0x80) != 0; f.knockout = (fl & 0x40) != 0; f.passes = (fl & 0x1F);
                out.push_back(f); } break;
            case 1: {  // Blur
                f.kind = FilterKind::Blur; f.blurX = fx(); f.blurY = fx();
                std::uint8_t fl = bv.readUI8(); f.passes = (fl & 0x1F);
                out.push_back(f); } break;
            case 2: {  // Glow
                auto c = bv.readRGBA(true); f.kind = FilterKind::Glow; f.color = Color(c.r, c.g, c.b, c.a);
                f.blurX = fx(); f.blurY = fx(); f.strength = fx8();
                std::uint8_t fl = bv.readUI8(); f.inner = (fl & 0x80) != 0; f.knockout = (fl & 0x40) != 0; f.passes = (fl & 0x1F);
                out.push_back(f); } break;
            case 3: {  // Bevel
                Filter f; f.kind = FilterKind::Bevel;
                auto sh = bv.readRGBA(true); f.color = Color(sh.r, sh.g, sh.b, sh.a);     // 阴影像素
                auto hi = bv.readRGBA(true); f.highlight = Color(hi.r, hi.g, hi.b, hi.a); // 高光像素
                f.blurX = fx(); f.blurY = fx(); f.angle = fang(); f.distance = fx(); f.strength = fx8();
                // 位：InnerShadow(7) Knockout(6) CompositeSource(5) OnTop(4) Passes(3..0)
                std::uint8_t fl = bv.readUI8();
                f.inner = (fl & 0x80) != 0; f.knockout = (fl & 0x40) != 0; f.onTop = (fl & 0x10) != 0;
                f.passes = (fl & 0x0F);
                out.push_back(f); } break;
            case 4: case 7: {  // GradientGlow / GradientBevel
                Filter f; f.kind = (id == 4) ? FilterKind::GradientGlow : FilterKind::GradientBevel;
                std::uint8_t nc = bv.readUI8();
                for (int k = 0; k < nc && !bv.overflow(); ++k) {
                    auto c = bv.readRGBA(true); f.gradientColors.emplace_back(c.r, c.g, c.b, c.a);
                }
                for (int k = 0; k < nc && !bv.overflow(); ++k) f.gradientRatios.push_back(bv.readUI8());
                f.blurX = fx(); f.blurY = fx(); f.angle = fang(); f.distance = fx(); f.strength = fx8();
                // 位：InnerShadow(7) Knockout(6) CompositeSource(5) OnTop(4) Passes(3..0)
                std::uint8_t fl = bv.readUI8();
                f.inner = (fl & 0x80) != 0; f.knockout = (fl & 0x40) != 0; f.onTop = (fl & 0x10) != 0;
                f.passes = (fl & 0x0F);
                out.push_back(f); } break;
            case 5: {  // Convolution
                Filter f; f.kind = FilterKind::Convolution;
                std::uint8_t mx = bv.readUI8(), my = bv.readUI8();
                f.convW = mx; f.convH = my;
                f.convDivisor = rf(); f.convBias = rf();
                int cnt = (int)mx * (int)my; f.convMatrix.resize(cnt);
                for (int k = 0; k < cnt && !bv.overflow(); ++k) f.convMatrix[k] = rf();
                auto def = bv.readRGBA(true); f.convColor = Color(def.r, def.g, def.b, def.a);
                if (!bv.overflow()) {
                    std::uint8_t fl = bv.readUI8();
                    f.convClamp = (fl & 0x02) != 0;          // Clamp（Reserved UB[6] + Clamp + PreserveAlpha）
                    f.convPreserveAlpha = (fl & 0x01) != 0;
                }
                out.push_back(f); } break;
            case 6: {  // ColorMatrix（4x5 = 20 个 FLOAT32）
                Filter f; f.kind = FilterKind::ColorMatrix;
                for (int k = 0; k < 20; ++k) f.colorMatrix[k] = rf();
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
        // 记录标志高 4 位：0x20 HasBlendMode、0x10 HasFilterList（必须在读完 CXFORM 后消费，
        // 否则滤镜/混合模式的字节会被当成下一条 BUTTONRECORD 解析出垃圾状态项）
        if (recFlags & 0x20) it.blend = static_cast<BlendMode>(bv.readUI8());
        if (recFlags & 0x10) parseFilterList(bv, it.filters);
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
struct DecodeContext {
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

// ---- Strategy：位图类 tag 集中处理 ----
// 这些 tag 过去直接用 base + doff + n 做裸指针算术，长度字段一旦被篡改就越界。
// 抽成 handler 后只能看到 TagReader（tag 边界视图），边界由 TagPipeline 统一保证。
static void installBitmapHandlers(TagRegistry<DecodeContext>& reg) {
    reg.add({6, 8, 21, 35, 20, 36},
        [](TagReader& r, DecodeContext& ctx, std::uint16_t code) -> TagStatus {
            if (code == 8) {  // JPEGTables：整段即表数据
                if (r.size() == 0) return TagStatus::Ok;
                ctx.jpegTables.assign(r.base(), r.base() + r.size());
                return TagStatus::Ok;
            }

            const std::uint16_t id = r.readUI16();
            if (r.overflow()) return TagStatus::Fail;

            auto ch = std::make_shared<Character>();
            ch->kind = Character::Kind::Bitmap;
            ch->bitmap = std::make_shared<Bitmap>();

            switch (code) {
                case 6: {  // DefineBits：JPEGTables + 本 tag 剩余数据
                    std::vector<std::uint8_t> blob;
                    blob.insert(blob.end(), ctx.jpegTables.begin(), ctx.jpegTables.end());
                    const std::size_t rest = r.remaining();
                    if (rest > 0)
                        blob.insert(blob.end(), r.current(), r.current() + rest);
                    if (!blob.empty())
                        decodeJpeg(blob.data(), static_cast<int>(blob.size()), *ch->bitmap);
                    break;
                }
                case 21: {  // DefineBitsJPEG2：完整 JPEG
                    const std::size_t rest = r.remaining();
                    if (rest > 0 && rest <= static_cast<std::size_t>(INT_MAX))
                        decodeJpeg(r.current(), static_cast<int>(rest), *ch->bitmap);
                    break;
                }
                case 35: {  // DefineBitsJPEG3：UI32 jpegSize + JPEG + zlib alpha
                    if (r.remaining() < 4) return TagStatus::Fail;
                    const std::uint32_t jpegSize = r.readUI32();
                    if (jpegSize > r.remaining()) return TagStatus::Fail;  // 杜绝下溢/越界
                    const std::uint8_t* jpeg = r.current();
                    r.skip(jpegSize);
                    if (jpegSize > 0 && jpegSize <= static_cast<std::uint32_t>(INT_MAX))
                        decodeJpeg(jpeg, static_cast<int>(jpegSize), *ch->bitmap);
                    std::vector<std::uint8_t> alpha;
                    if (r.remaining() > 0)
                        inflateZlib(r.current(), r.remaining(), alpha);
                    applyAlpha(*ch->bitmap, alpha);
                    break;
                }
                case 20: case 36: {  // DefineBitsLossless / Lossless2
                    const bool withAlpha = (code == 36);
                    if (r.remaining() < 5) return TagStatus::Fail;
                    const std::uint8_t fmt = r.readUI8();
                    const std::uint16_t w = r.readUI16();
                    const std::uint16_t h = r.readUI16();
                    // 先限像素总量，再按文件字段分配，避免 65535x65535 的 17GB 分配。
                    constexpr std::size_t kMaxPixels = 16ull * 1024ull * 1024ull;
                    if (w == 0 || h == 0 ||
                        static_cast<std::size_t>(w) * h > kMaxPixels) return TagStatus::Fail;
                    std::vector<std::uint8_t> raw;
                    if (!inflateZlib(r.current(), r.remaining(), raw))
                        return TagStatus::Ok;
                    ch->bitmap->width = w;
                    ch->bitmap->height = h;
                    ch->bitmap->rgba.assign(static_cast<std::size_t>(w) * h * 4, 0);
                    if (!raw.empty()) decodeLossless(fmt, withAlpha, w, h, raw, *ch->bitmap);
                    break;
                }
                default:
                    return TagStatus::Ignored;
            }
            ctx.characters[id] = std::move(ch);
            return TagStatus::Ok;
        });
}
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

// FIX-2026-023：Flash 的 shape tween 是"跨度"——MorphShape 只在首尾关键帧各 PlaceObject
// 一次（带 HasRatio），中间帧由播放器按播放头位置线性插值 ratio 生成。本引擎快照式解码只把
// 显示列表逐帧拷贝，导致 tween 跨度内所有中间帧的 morph 比值都停留在首关键帧 → 变形动画
// 看起来"卡住"。此函数在解码完成后补一遍跨度补间：对同一 depth 上连续帧内恒定的 Morph 比值，
// 按首尾关键帧比值线性插值中间帧，使播放时 morph 正常插值。（注：loading 转盘的旋转由 SWF
// 按加载进度在运行时设置 _rotation，进度一完成即停转，属另一类问题，此处不覆盖。）
static void expandShapeTweens(std::map<std::uint16_t, std::shared_ptr<Character>>& chars) {
    for (auto& kv : chars) {
        auto& ch = kv.second;
        if (!ch || ch->kind != Character::Kind::Sprite) continue;
        auto& frames = ch->spriteFrames;
        const int F = (int)frames.size();
        if (F < 2) continue;
        // depth -> 该 depth 上出现 Morph 的 (帧号, ratio) 序列
        std::map<std::uint16_t, std::vector<std::pair<int, float>>> morphByDepth;
        for (int f = 0; f < F; ++f)
            for (auto& it : frames[f].items)
                if (it.character && it.character->kind == Character::Kind::Morph)
                    morphByDepth[it.depth].push_back({f, it.ratio});
        for (auto& kv2 : morphByDepth) {
            const std::uint16_t depth = kv2.first;
            auto& seq = kv2.second;
            int i = 0;
            while (i < (int)seq.size()) {
                const float r0 = seq[i].second;
                int j = i;
                while (j + 1 < (int)seq.size() && seq[j + 1].second == r0) ++j;
                // 跨度 [i..j] 比值恒为 r0；若下一帧比值不同（r1），说明是 tween 跨度
                if (j + 1 < (int)seq.size()) {
                    const float r1 = seq[j + 1].second;
                    if (r1 != r0) {
                        const int startF = seq[i].first;
                        const int endF = seq[j + 1].first;   // 末关键帧
                        if (endF > startF + 1) {
                            for (int f = startF + 1; f < endF; ++f) {
                                const float t = (float)(f - startF) / (float)(endF - startF);
                                const float rr = r0 + (r1 - r0) * t;
                                for (auto& it : frames[f].items)
                                    if (it.depth == depth && it.character &&
                                        it.character->kind == Character::Kind::Morph)
                                        it.ratio = rr;
                            }
                        }
                    }
                }
                i = j + 1;
            }
        }
    }
}

bool decodeMovie(const std::uint8_t* data, std::size_t size, Movie& out, std::string& err) {
    if (size < 8) { err = "file too small"; return false; }
    // 与 Parser::parse 保持同一套头校验，避免任意以 'F' 开头的字节流进入完整解码。
    const char sig0 = static_cast<char>(data[0]);
    if (data[1] != 'W' || data[2] != 'S' ||
        (sig0 != 'F' && sig0 != 'C' && sig0 != 'Z')) {
        err = "bad signature"; return false;
    }
    if (data[3] == 0) { err = "invalid SWF version"; return false; }
    const std::uint32_t fileLength = static_cast<std::uint32_t>(data[4])
        | (static_cast<std::uint32_t>(data[5]) << 8)
        | (static_cast<std::uint32_t>(data[6]) << 16)
        | (static_cast<std::uint32_t>(data[7]) << 24);
    if (fileLength < 8) { err = "invalid FileLength"; return false; }

    // 解压成 FWS 缓冲
    std::vector<std::uint8_t> full;
    if (sig0 == 'C') {
        if (!inflateZlib(data + 8, size - 8, full)) { err = "zlib inflate failed"; return false; }
    } else if (sig0 == 'Z') {
        if (!decodeZws(data + 8, size - 8, full)) { err = "lzma failed"; return false; }
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

    DecodeContext ctx;
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

    // Template Method：标签流骨架统一走 TagPipeline；
    // 尚未迁移到 Registry 的 tag 由下面的 fallback（旧 switch）处理。
    // fallback 拿到的 ByteView 带 tag 软边界，故 bv.pos() 等绝对偏移语义保持不变。
    TagRegistry<DecodeContext> registry;
    installBitmapHandlers(registry);
    // M4 迁移（第一批，低风险 tag）：从 fallback switch 迁出为独立策略。
    registry.add(9, [](TagReader& r, DecodeContext& ctx, std::uint16_t) -> TagStatus {
        readColor(r.bytes(), false, ctx.movie.backgroundColor);
        return TagStatus::Ok;
    });
    registry.add(1, [](TagReader& r, DecodeContext& ctx, std::uint16_t) -> TagStatus {
        ctx.hasShowFrame = true;
        ctx.snapshotFrame();
        return TagStatus::Ok;
    });
    registry.add(43, [](TagReader& r, DecodeContext& ctx, std::uint16_t) -> TagStatus {
        std::string name;
        while (r.bytes().pos() < r.length() && !r.bytes().overflow()) {
            int c = r.bytes().readUI8();
            if (c == 0 || c == -1) break;
            name.push_back(static_cast<char>(c));
        }
        if (!name.empty()) ctx.movie.frameLabels[name] = ctx.curFrame + 1;
        return TagStatus::Ok;
    });
    registry.add(0, [](TagReader& r, DecodeContext& ctx, std::uint16_t) -> TagStatus {
        if (!ctx.hasShowFrame) ctx.snapshotFrame();
        return TagStatus::Stop;
    });

    // M4 第二批：定义类 tag 迁出 switch（handler 内用 absView() 重建原绝对坐标）。
    registry.add({2, 22, 32, 83}, [](TagReader& r, DecodeContext& ctx, std::uint16_t code) -> TagStatus {
        ByteView bv = r.absView();
        const std::uint8_t* base = r.fileBase();
        const std::size_t doff = r.dataOffset(), tEnd = doff + r.length();
        const std::size_t len = r.length();
        int ver = (code == 2) ? 1 : (code == 22) ? 2 : (code == 32) ? 3 : 4;
        std::uint16_t id = bv.readUI16();
        auto shp = decodeShape(bv, ver, id);
        auto ch = std::make_shared<Character>();
        ch->kind = Character::Kind::Shape;
        ch->shape = shp;
        ctx.characters[id] = ch;
        return TagStatus::Ok;
    });
    registry.add(56, [](TagReader& r, DecodeContext& ctx, std::uint16_t) -> TagStatus {
        ByteView bv = r.absView();
        const std::uint8_t* base = r.fileBase();
        const std::size_t doff = r.dataOffset(), tEnd = doff + r.length();
        const std::size_t len = r.length();
        std::uint16_t num = bv.readUI16();
        for (std::uint16_t k = 0; k < num; ++k) {
            std::uint16_t cid = bv.readUI16();
            std::string nm; int c;
            while ((c = bv.readUI8()) != 0 && c != -1) nm.push_back(static_cast<char>(c));
            if (!nm.empty()) ctx.movie.symbolClasses[nm] = cid;
        }
        return TagStatus::Ok;
    });
    registry.add({76, 77}, [](TagReader& r, DecodeContext& ctx, std::uint16_t) -> TagStatus {
        ByteView bv = r.absView();
        const std::uint8_t* base = r.fileBase();
        const std::size_t doff = r.dataOffset(), tEnd = doff + r.length();
        const std::size_t len = r.length();
        std::uint16_t num = bv.readUI16();
        for (std::uint16_t k = 0; k < num; ++k) {
            std::uint16_t cid = bv.readUI16();
            std::string nm; int c;
            while ((c = bv.readUI8()) != 0 && c != -1) nm.push_back(static_cast<char>(c));
            std::string dot = nm; size_t pos;
            while ((pos = dot.find("::")) != std::string::npos) dot.replace(pos, 2, ".");
            ctx.movie.symbolClasses[dot] = cid;
        }
        return TagStatus::Ok;
    });
    registry.add(82, [](TagReader& r, DecodeContext& ctx, std::uint16_t) -> TagStatus {
        ByteView bv = r.absView();
        const std::uint8_t* base = r.fileBase();
        const std::size_t doff = r.dataOffset(), tEnd = doff + r.length();
        const std::size_t len = r.length();
        bv.readUI32();
        while (bv.pos() < doff + len && bv.readUI8() != 0) {}
        std::size_t abcStart = bv.pos();
        if (abcStart < doff + len) {
            abc::AbcFile f; std::string aerr;
            if (abc::parseAbc(base + abcStart, (doff + len) - abcStart, f, aerr))
                ctx.movie.abcFiles.push_back(std::move(f));
        }
        return TagStatus::Ok;
    });
    registry.add(34, [](TagReader& r, DecodeContext& ctx, std::uint16_t) -> TagStatus {
        ByteView bv = r.absView();
        const std::uint8_t* base = r.fileBase();
        const std::size_t doff = r.dataOffset(), tEnd = doff + r.length();
        const std::size_t len = r.length();
        std::uint16_t id = bv.readUI16();
        auto btn = decodeButton2(bv, ctx.characters, ctx.swfVersion, doff + len);
        ctx.characters[id] = btn;
        return TagStatus::Ok;
    });
    registry.add(17, [](TagReader& r, DecodeContext& ctx, std::uint16_t) -> TagStatus {
        ByteView bv = r.absView();
        const std::uint8_t* base = r.fileBase();
        const std::size_t doff = r.dataOffset(), tEnd = doff + r.length();
        const std::size_t len = r.length();
        std::uint16_t id = bv.readUI16();
        auto itc = ctx.characters.find(id);
        if (itc != ctx.characters.end() && itc->second &&
            itc->second->kind == Character::Kind::Button && itc->second->button) {
            auto& btn = *itc->second->button;
            std::uint16_t* slots[4] = {&btn.soundUp, &btn.soundOver, &btn.soundDown, &btn.soundOut};
            for (int k = 0; k < 4 && bv.pos() + 2 <= doff + len; ++k) {
                std::uint16_t sid = bv.readUI16();
                readSoundInfoSkip(bv, doff + len, nullptr);
                if (sid) *slots[k] = sid;
            }
        }
        return TagStatus::Ok;
    });
    registry.add(10, [](TagReader& r, DecodeContext& ctx, std::uint16_t) -> TagStatus {
        ByteView bv = r.absView();
        const std::uint8_t* base = r.fileBase();
        const std::size_t doff = r.dataOffset(), tEnd = doff + r.length();
        const std::size_t len = r.length();
        std::uint16_t fid = bv.readUI16();
        std::size_t offStart = bv.pos();
        if (offStart + 2 > doff + len || bv.overflow()) return TagStatus::Ok;
        std::uint16_t o0 = bv.readUI16();
        int nGlyphs = static_cast<int>(o0) / 2;
        if (nGlyphs <= 0 || nGlyphs > 65535) return TagStatus::Ok;
        std::vector<std::uint32_t> offs(static_cast<std::size_t>(nGlyphs));
        offs[0] = o0;
        for (int i = 1; i < nGlyphs; ++i) offs[i] = bv.readUI16();
        auto fg = std::make_shared<FontGlyphs>();
        // DefineFont(10) 的字形坐标就是 twips（与 FontHeight 同尺度），
        // 没有 EM square / HasLayout 度量表。
        fg->glyphsInTwips = true;
        fg->unitsPerEm = 1;
        fg->shapes.resize(static_cast<std::size_t>(nGlyphs));
        fg->advances.assign(static_cast<std::size_t>(nGlyphs), 0);
        for (int i = 0; i < nGlyphs; ++i) {
            std::size_t gstart = offStart + offs[static_cast<std::size_t>(i)];
            std::size_t gend = (i + 1 < nGlyphs) ? offStart + offs[static_cast<std::size_t>(i + 1)] : doff + len;
            if (gstart >= gend || gend > bv.limit()) continue;
            ByteView gv(base, bv.size()); gv.seek(gstart); gv.setLimit(gend);
            auto shp = std::make_shared<Shape>();
            parseGlyphShape(gv, shp, 1);
            fg->shapes[static_cast<std::size_t>(i)] = shp;
        }
        auto ch = std::make_shared<Character>();
        ch->kind = Character::Kind::Font;
        ch->font = fg;
        ctx.characters[fid] = ch;
        return TagStatus::Ok;
    });
    registry.add({48, 75}, [](TagReader& r, DecodeContext& ctx, std::uint16_t code) -> TagStatus {
        ByteView bv = r.absView();
        const std::uint8_t* base = r.fileBase();
        const std::size_t doff = r.dataOffset(), tEnd = doff + r.length();
        const std::size_t len = r.length();
        ByteView fv(base + doff, len);
        std::uint16_t id = fv.readUI16();
        auto fg = decodeFont(fv, ctx.swfVersion, code == 75);
        auto ch = std::make_shared<Character>();
        ch->kind = Character::Kind::Font;
        ch->font = fg;
        ctx.characters[id] = ch;
        return TagStatus::Ok;
    });
    registry.add({13, 62}, [](TagReader& r, DecodeContext& ctx, std::uint16_t code) -> TagStatus {
        ByteView bv = r.absView();
        const std::uint8_t* base = r.fileBase();
        const std::size_t doff = r.dataOffset(), tEnd = doff + r.length();
        const std::size_t len = r.length();
        std::uint16_t fid = bv.readUI16();
        if (code == 62) bv.readUI8();
        int nameLen = bv.readUI8();
        std::string fname; std::size_t nameStart = bv.pos();
        for (int i = 0; i < nameLen; ++i) { int c = bv.readUI8(); if (c == 0) break; fname.push_back(static_cast<char>(c)); }
        bv.seek(nameStart + static_cast<std::size_t>(nameLen));
        std::uint8_t fflags = bv.readUI8();
        bool wide = (code == 62) || (fflags & 0x01) != 0;
        auto it = ctx.characters.find(fid);
        if (it != ctx.characters.end() && it->second && it->second->font) {
            FontGlyphs& fg = *it->second->font;
            if (fg.name.empty() && !fname.empty()) fg.name = fname;
            fg.italic = fg.italic || (fflags & 0x04) != 0;
            fg.bold = fg.bold || (fflags & 0x02) != 0;
            if (fg.codeToGlyph.empty()) {
                int n = static_cast<int>(fg.shapes.size());
                for (int i = 0; i < n && !bv.overflow(); ++i) {
                    int cc = wide ? static_cast<int>(bv.readUI16()) : static_cast<int>(bv.readUI8());
                    fg.codeToGlyph[cc] = i;
                }
            }
        }
        return TagStatus::Ok;
    });
    registry.add({11, 33}, [](TagReader& r, DecodeContext& ctx, std::uint16_t code) -> TagStatus {
        ByteView bv = r.absView();
        const std::uint8_t* base = r.fileBase();
        const std::size_t doff = r.dataOffset(), tEnd = doff + r.length();
        const std::size_t len = r.length();
        bool alpha = (code == 33);
        std::uint16_t id = bv.readUI16();
        auto td = decodeText(bv, alpha);
        auto ch = std::make_shared<Character>();
        ch->kind = Character::Kind::Text;
        ch->text = td;
        ctx.characters[id] = ch;
        return TagStatus::Ok;
    });
    registry.add(37, [](TagReader& r, DecodeContext& ctx, std::uint16_t) -> TagStatus {
        ByteView bv = r.absView();
        const std::uint8_t* base = r.fileBase();
        const std::size_t doff = r.dataOffset(), tEnd = doff + r.length();
        const std::size_t len = r.length();
        std::uint16_t id = bv.readUI16();
        auto et = decodeEditText(bv);
        auto ch = std::make_shared<Character>();
        ch->kind = Character::Kind::EditText;
        ch->editText = et;
        ctx.characters[id] = ch;
        return TagStatus::Ok;
    });
    registry.add({46, 84}, [](TagReader& r, DecodeContext& ctx, std::uint16_t code) -> TagStatus {
        ByteView bv = r.absView();
        const std::uint8_t* base = r.fileBase();
        const std::size_t doff = r.dataOffset(), tEnd = doff + r.length();
        const std::size_t len = r.length();
        int ver = (code == 46) ? 1 : 2;
        std::uint16_t id = bv.readUI16();
        auto ms = decodeMorphShape(bv, ver, id);
        auto ch = std::make_shared<Character>();
        ch->kind = Character::Kind::Morph;
        ch->morph = ms;
        ctx.characters[id] = ch;
        return TagStatus::Ok;
    });

    // M4 第三批：显示列表/脚本/声音/视频/标签类 tag 迁出 switch。
    registry.add({5, 28}, [](TagReader& r, DecodeContext& ctx, std::uint16_t code) -> TagStatus {
        ByteView bv = r.absView();
        const std::uint8_t* base = r.fileBase();
        const std::size_t doff = r.dataOffset();
        const std::size_t len = r.length();
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
        return TagStatus::Ok;
    });
    registry.add(12, [](TagReader& r, DecodeContext& ctx, std::uint16_t) -> TagStatus {
        const std::uint8_t* base = r.fileBase();
        const std::size_t doff = r.dataOffset();
        const std::size_t len = r.length();
        avm1::Script sc = avm1::disassemble(base + doff, len);
        ctx.pendingFrameActions.push_back(std::move(sc));
        return TagStatus::Ok;
    });
    registry.add(59, [](TagReader& r, DecodeContext& ctx, std::uint16_t) -> TagStatus {
        const std::uint8_t* base = r.fileBase();
        const std::size_t doff = r.dataOffset();
        const std::size_t len = r.length();
        if (len < 2) return TagStatus::Ok;  // 否则 len - 2 会无符号下溢成 0xFFFFFFFE
        std::uint16_t spriteId = static_cast<std::uint16_t>(base[doff] | (base[doff + 1] << 8));
        avm1::Script sc = avm1::disassemble(base + doff + 2, static_cast<std::size_t>(len - 2));
        ctx.movie.initActions[spriteId] = std::move(sc);
        return TagStatus::Ok;
    });
    registry.add(14, [](TagReader& r, DecodeContext& ctx, std::uint16_t) -> TagStatus {
        const std::uint8_t* base = r.fileBase();
        const std::size_t doff = r.dataOffset();
        const std::size_t len = r.length();
        if (len < 7) return TagStatus::Ok;  // 裸索引 base[doff..doff+6] 的前置条件
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
        return TagStatus::Ok;
    });
    registry.add({15, 89}, [](TagReader& r, DecodeContext& ctx, std::uint16_t code) -> TagStatus {
        ByteView bv = r.absView();
        const std::uint8_t* base = r.fileBase();
        const std::size_t doff = r.dataOffset();
        const std::size_t len = r.length();
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
        return TagStatus::Ok;
    });
    registry.add({18, 45}, [](TagReader& r, DecodeContext& ctx, std::uint16_t code) -> TagStatus {
        const std::uint8_t* base = r.fileBase();
        const std::size_t doff = r.dataOffset();
        const std::size_t len = r.length();
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
        return TagStatus::Ok;
    });
    registry.add(19, [](TagReader& r, DecodeContext& ctx, std::uint16_t) -> TagStatus {
        const std::uint8_t* base = r.fileBase();
        const std::size_t doff = r.dataOffset();
        const std::size_t len = r.length();
        if (len > 0) {
            SoundEvent ev;
            ev.soundId = 0;
            ev.streamBlock = ctx.streamBlockCount++;
            ctx.pendingSoundEvents.push_back(ev);
            ctx.movie.streamAudio.blocks.emplace_back(base + doff, base + doff + len);
        }
        return TagStatus::Ok;
    });
    registry.add(60, [](TagReader& r, DecodeContext& ctx, std::uint16_t) -> TagStatus {
        const std::uint8_t* base = r.fileBase();
        const std::size_t doff = r.dataOffset();
        const std::size_t len = r.length();
        if (len < 9) return TagStatus::Ok;
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
        return TagStatus::Ok;
    });
    registry.add(61, [](TagReader& r, DecodeContext& ctx, std::uint16_t) -> TagStatus {
        const std::uint8_t* base = r.fileBase();
        const std::size_t doff = r.dataOffset();
        const std::size_t len = r.length();
        if (len < 4) return TagStatus::Ok;
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
        return TagStatus::Ok;
    });
    registry.add(86, [](TagReader& r, DecodeContext& ctx, std::uint16_t) -> TagStatus {
        const std::uint8_t* base = r.fileBase();
        const std::size_t doff = r.dataOffset();
        const std::size_t len = r.length();
        // 结构：SceneCount(UI32) [FrameOffset(UI32) SceneName(STRING)]*
        //       FrameLabelCount(UI32) [FrameNum(UI32) FrameLabel(STRING)]*
        // 注意 FrameNum 是 UI32（不是 UI16），字符串已含长度语义（以 0 结尾）。
        // 计数一旦解析错位会变成天文数字，必须按标签长度夹紧，否则会空转极久。
        ByteView fb(base, doff + len); fb.seek(doff); fb.setLimit(doff + len);
        const std::size_t end = doff + len;
        auto readStr = [&]() {
            std::string s; int c;
            while (fb.pos() < end && (c = fb.readUI8()) != 0) s.push_back((char)c);
            return s;
        };
        // 每条记录最少 5 字节（UI32 + 1 字节结尾 0）
        const std::uint32_t maxEntries = static_cast<std::uint32_t>(len / 5) + 1;
        std::uint32_t sceneCount = fb.readUI32();
        if (sceneCount > maxEntries) sceneCount = maxEntries;
        for (std::uint32_t i = 0; i < sceneCount && fb.pos() < end; ++i) {
            fb.readUI32();
            readStr();
        }
        if (fb.pos() + 4 > end) return TagStatus::Ok;
        std::uint32_t labelCount = fb.readUI32();
        if (labelCount > maxEntries) labelCount = maxEntries;
        for (std::uint32_t i = 0; i < labelCount && fb.pos() < end; ++i) {
            std::uint32_t fn = fb.readUI32();
            std::string name = readStr();
            if (!name.empty()) ctx.movie.frameLabels[name] = static_cast<int>(fn) + 1;
        }
        return TagStatus::Ok;
    });
    registry.add(88, [](TagReader& r, DecodeContext& ctx, std::uint16_t) -> TagStatus {
        const std::uint8_t* base = r.fileBase();
        const std::size_t doff = r.dataOffset();
        const std::size_t len = r.length();
        ByteView fb(base, doff + len); fb.seek(doff); fb.setLimit(doff + len);
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
        return TagStatus::Ok;
    });

    // M4 第四批：PlaceObject 系列（显示列表）迁出 switch。
    registry.add(4, [](TagReader& r, DecodeContext& ctx, std::uint16_t) -> TagStatus {
        ByteView bv = r.absView();
        const std::uint8_t* base = r.fileBase();
        const std::size_t doff = r.dataOffset(), tEnd = doff + r.length();
        const std::size_t len = r.length();
        std::uint16_t cid = bv.readUI16();
        std::uint16_t depth = bv.readUI16();
        DisplayItem it;
        it.depth = depth;
        auto itc = ctx.characters.find(cid);
        it.character = (itc != ctx.characters.end()) ? itc->second : nullptr;
        it.characterId = cid;
        it.matrix = Matrix{};
        ctx.displayList[depth] = it;
        return TagStatus::Ok;
    });
    registry.add(26, [](TagReader& r, DecodeContext& ctx, std::uint16_t) -> TagStatus {
        ByteView bv = r.absView();
        const std::uint8_t* base = r.fileBase();
        const std::size_t doff = r.dataOffset(), tEnd = doff + r.length();
        const std::size_t len = r.length();
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
        return TagStatus::Ok;
    });
    registry.add(70, [](TagReader& r, DecodeContext& ctx, std::uint16_t) -> TagStatus {
        ByteView bv = r.absView();
        const std::uint8_t* base = r.fileBase();
        const std::size_t doff = r.dataOffset(), tEnd = doff + r.length();
        const std::size_t len = r.length();
        int f = bv.readUI8();
        int f2 = bv.readUI8();
        DisplayItem it;
        it.depth = bv.readUI16();
        if (f & 0x01) {   // Move：继承该 depth 上已有的对象
            auto e = ctx.displayList.find(it.depth);
            if (e != ctx.displayList.end()) it = e->second;
        }
        // PlaceFlag2（Reserved2）位定义（SWF 规范 6.2）：
        //   0x01 HasFilterList  0x02 HasBlendMode  0x04 HasCacheAsBitmap
        // 注意：PO3 **没有** ClassName 字段（ClassName 是 PlaceObject4 才引入的），
        // 字段序：Depth → [CharacterId] → [Matrix] → [CXForm] → [Ratio]
        //        → [Name] → [ClipDepth] → [FilterList] → [BlendMode] → [CacheAsBitmap] → [ClipActions]
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
        if (f2 & 0x01) parseFilterList(bv, it.filters);    // FilterList (HasFilterList=0x01)
        if (f2 & 0x02) it.blend = static_cast<BlendMode>(bv.readUI8());  // BlendMode (0x02)
        if (f2 & 0x04) bv.readUI8();                       // CacheAsBitmap (0x04)
        if (f & 0x80) readClipActions(bv, doff + len, ctx.swfVersion, it.clipEvents);
        ctx.displayList[it.depth] = it;
        return TagStatus::Ok;
    });
    registry.add(81, [](TagReader& r, DecodeContext& ctx, std::uint16_t) -> TagStatus {
        ByteView bv = r.absView();
        const std::uint8_t* base = r.fileBase();
        const std::size_t doff = r.dataOffset(), tEnd = doff + r.length();
        const std::size_t len = r.length();
        int f = bv.readUI8();    // PlaceFlag：0x01 Move 0x02 HasCharacter 0x04 HasMatrix
        int f2 = bv.readUI8();   // Reserved2：0x01 HasFilterList 0x02 HasBlendMode
                                //            0x04 HasCacheAsBitmap 0x08 HasImage 0x10 HasClassName
        DisplayItem it;
        it.depth = bv.readUI16();
        if (f & 0x01) {   // Move：继承该 depth 上已有的对象
            auto e = ctx.displayList.find(it.depth);
            if (e != ctx.displayList.end()) it = e->second;
        }
        // 字段序（规范）：Depth → [ClassName] → [CharacterId] → [Matrix] → [CXForm] → [Ratio]
        //                → [Name] → [ClipDepth] → [BitmapCache(仅 HasImage)]
        //                → [FilterList] → [BlendMode] → [CacheAsBitmap] → [ClipActions]
        if ((f2 & 0x10) || ((f2 & 0x08) != 0 && (f & 0x02) != 0))
            (void)readStringZ(bv, doff + len);            // ClassName
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
        if (f2 & 0x08) {                                   // BitmapCache（仅 HasImage 时存在）
            bv.readUI16();                                 // Reserved
            bv.readUI8();                                  // IsProtected
            bv.readUI8();                                  // ClipShadows
            std::uint16_t ci = bv.readUI16();              // ClassIndex（符号类注册表索引）
            (void)ci;
        }
        if (f2 & 0x01) parseFilterList(bv, it.filters);    // FilterList (HasFilterList=0x01)
        if (f2 & 0x02) it.blend = static_cast<BlendMode>(bv.readUI8());  // BlendMode (0x02)
        if (f2 & 0x04) bv.readUI8();                       // CacheAsBitmap (0x04)
        if (f & 0x80) readClipActions(bv, doff + len, ctx.swfVersion, it.clipEvents);
        ctx.displayList[it.depth] = it;
        return TagStatus::Ok;
    });

    // M4 收尾：DefineSprite（子时间轴，内部自解析子 tag 流）。
    registry.add(39, [](TagReader& r, DecodeContext& ctx, std::uint16_t) -> TagStatus {
        ByteView bv = r.absView();
        const std::size_t doff = r.dataOffset();
        const std::size_t len = r.length();
        std::uint16_t sid = bv.readUI16();
        bv.readUI16();  // 精灵帧数（渲染时按需取帧）
        auto sp = std::make_shared<Character>();
        sp->kind = Character::Kind::Sprite;
        std::map<std::uint16_t, DisplayItem> sub;
        std::map<std::uint16_t, std::shared_ptr<Character>> subPlaced;
        int subFrame = 0;
        // 与主时间轴一致：DoAction/StartSound 先累积，ShowFrame 时并入该帧。
        std::vector<avm1::Script> subPendingActions;
        std::vector<SoundEvent> subPendingSounds;
        std::size_t subEnd = doff + len;
        while (subEnd >= bv.pos() && subEnd - bv.pos() >= 2) {
            std::uint16_t tv = bv.readUI16();
            std::uint32_t tlen = tv & 0x3F;
            if (tlen == 63) {
                if (subEnd - bv.pos() < 4) break;  // 长头的 4 字节长度字段被截断
                tlen = bv.readUI32();
            }
            const std::size_t tStart = bv.pos();
            if (static_cast<std::size_t>(tlen) > subEnd - tStart) break;  // 防回绕/越界
            const std::size_t tEnd = tStart + tlen;
            std::uint16_t tc = static_cast<std::uint16_t>(tv >> 6);
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
                { auto prev = sub.find(it.depth); if (prev != sub.end()) it = prev->second; }
                if (f & 0x02) { std::uint16_t cid = bv.readUI16(); auto ic = ctx.characters.find(cid); it.character = (ic != ctx.characters.end()) ? ic->second : nullptr; it.characterId = cid; }
                if (f & 0x04) it.matrix = readMatrix(bv);
                if (f & 0x08) it.cxform = readCXForm(bv, true);
                if (f & 0x10) it.ratio = static_cast<float>(bv.readUI16()) / 65535.0f;
                if (f & 0x20) it.name = readStringZ(bv, tEnd);
                if (f & 0x40) it.clipDepth = bv.readUI16();
                if (f2 & 0x01) parseFilterList(bv, it.filters);
                if (f2 & 0x02) it.blend = static_cast<BlendMode>(bv.readUI8());
                if (f2 & 0x04) bv.readUI8();
                if (f & 0x80) readClipActions(bv, tEnd, ctx.swfVersion, it.clipEvents);
                sub[it.depth] = it;
            } else if (tc == 81) {  // PlaceObject4
                int f = bv.readUI8(); int f2 = bv.readUI8();
                DisplayItem it; it.depth = bv.readUI16();
                { auto prev = sub.find(it.depth); if (prev != sub.end()) it = prev->second; }
                if ((f2 & 0x10) || ((f2 & 0x08) != 0 && (f & 0x02) != 0)) (void)readStringZ(bv, tEnd);
                if (f & 0x02) { std::uint16_t cid = bv.readUI16(); auto ic = ctx.characters.find(cid); it.character = (ic != ctx.characters.end()) ? ic->second : nullptr; it.characterId = cid; }
                if (f & 0x04) it.matrix = readMatrix(bv);
                if (f & 0x08) it.cxform = readCXForm(bv, true);
                if (f & 0x10) it.ratio = static_cast<float>(bv.readUI16()) / 65535.0f;
                if (f & 0x20) it.name = readStringZ(bv, tEnd);
                if (f & 0x40) it.clipDepth = bv.readUI16();
                if (f2 & 0x08) { bv.readUI16(); bv.readUI8(); bv.readUI8(); (void)bv.readUI16(); }
                if (f2 & 0x01) parseFilterList(bv, it.filters);
                if (f2 & 0x02) it.blend = static_cast<BlendMode>(bv.readUI8());
                if (f2 & 0x04) bv.readUI8();
                if (f & 0x80) readClipActions(bv, tEnd, ctx.swfVersion, it.clipEvents);
                sub[it.depth] = it;
            } else if (tc == 46 || tc == 84) {  // DefineMorphShape / DefineMorphShape2
                int ver = (tc == 46) ? 1 : 2;
                std::uint16_t cid = bv.readUI16();
                auto ms = decodeMorphShape(bv, ver, cid);
                auto ch = std::make_shared<Character>();
                ch->kind = Character::Kind::Morph;
                ch->morph = ms;
                ctx.characters[cid] = ch;
            } else if (tc == 5) {
                bv.readUI16(); std::uint16_t d = bv.readUI16();
                sub.erase(d); subPlaced.erase(d);
            }
            else if (tc == 28) {
                std::uint16_t d = bv.readUI16();
                sub.erase(d); subPlaced.erase(d);
            }
            else if (tc == 1) {  // ShowFrame：快照精灵帧
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
                if (!subPendingActions.empty()) {
                    std::vector<std::uint8_t> bytes;
                    for (auto& a : subPendingActions)
                        bytes.insert(bytes.end(), a.bytes.begin(), a.bytes.end());
                    fr.doActions.push_back(avm1::disassemble(bytes.data(), bytes.size()));
                    subPendingActions.clear();
                }
                if (!subPendingSounds.empty()) {
                    fr.soundEvents = std::move(subPendingSounds);
                    subPendingSounds.clear();
                }
                sp->spriteFrames.push_back(std::move(fr));
                ++subFrame;
            }
            else if (tc == 12) {  // DoAction：累积到本帧，ShowFrame 时并入
                subPendingActions.push_back(avm1::disassemble(bv.base() + tStart, tEnd - tStart));
            }
            else if (tc == 15 || tc == 89) {  // StartSound / StartSound2
                SoundEvent ev;
                if (tc == 15) {
                    ev.soundId = bv.readUI16();
                } else {
                    (void)readStringZ(bv, tEnd);
                }
                bool stop = false;
                ev.loop = readSoundInfoSkip(bv, tEnd, &stop);
                ev.stop = stop;
                if (tc == 15) subPendingSounds.push_back(ev);
            }
            else if (tc == 43) {  // FrameLabel：给紧随其后的那一帧起名
                std::string nm = readStringZ(bv, tEnd);
                if (!nm.empty()) sp->frameLabels[nm] = subFrame + 1;
            }
            else if (tc == 59) {  // DoInitAction（精灵初始化脚本）
                bv.readUI16();
                avm1::Script sc = avm1::disassemble(bv.base() + bv.pos(), tEnd - bv.pos());
                ctx.movie.initActions[sid] = std::move(sc);
            }
            else if (tc == 0) { bv.seek(tEnd); break; }  // End of sprite
            bv.seek(tEnd);
        }
        ctx.characters[sid] = sp;
        return TagStatus::Ok;
    });

    TagPipeline<DecodeContext> pipeline(registry,
        [base, avail](ByteView& bv, DecodeContext& ctx, const TagHeader& th) -> TagStatus {
            const std::uint16_t code = th.code;
            const std::uint32_t len = th.length;
            const std::size_t doff = th.dataOffset;

        switch (code) {
            // case 6/8/21/35/20/36 已迁移到 installBitmapHandlers（Strategy），
            // 由 TagPipeline 在进入 switch 之前按 Registry 分派。

            default:
                break;  // 其它标签暂忽略
        }
        return TagStatus::Ok;
    });
    // End(Stop) / 数据不足(Incomplete) / 损坏(Fail) 都会让管道停下；
    // 与旧实现一致，此处保留已解析出的帧并正常收尾。
    pipeline.run(base, avail, pos, ctx);
    if (ctx.movie.frames.empty()) ctx.snapshotFrame();
    ctx.movie.characters = std::move(ctx.characters);
    { std::vector<const Character*> seen;
      resolveForwardRefs(ctx.movie.frames, ctx.movie.characters, seen, 0); }
    if (!std::getenv("SWF_NO_TWEEN")) expandShapeTweens(ctx.movie.characters);
    out = std::move(ctx.movie);
    return true;
}

// Lossless 位图：format 3(调色板) / 4(15/16bit) / 5(24/32bit)
static void decodeLossless(std::uint8_t fmt, bool withAlpha, std::uint16_t w, std::uint16_t h,
                          const std::vector<std::uint8_t>& raw, Bitmap& bmp) {
    const std::size_t n = static_cast<std::size_t>(w) * h;
    if (bmp.rgba.size() < n * 4) return;
    std::size_t p = 0;
    // 统一安全取值器：损坏/截断的解压数据越界时补 0，而不是 raw[p++] 越界读。
    auto byte = [&]() -> std::uint8_t { return p < raw.size() ? raw[p++] : 0; };
    auto u16 = [&]() -> std::uint16_t {
        const std::uint16_t lo = byte();
        return static_cast<std::uint16_t>(lo | (static_cast<std::uint16_t>(byte()) << 8));
    };
    if (fmt == 3) {
        const int entries = static_cast<int>(byte()) + 1;
        std::vector<Color> pal(static_cast<std::size_t>(entries));
        for (int i = 0; i < entries; ++i) {
            pal[static_cast<std::size_t>(i)].r = byte();
            pal[static_cast<std::size_t>(i)].g = byte();
            pal[static_cast<std::size_t>(i)].b = byte();
            pal[static_cast<std::size_t>(i)].a = withAlpha ? byte() : 0xFF;
        }
        for (std::size_t i = 0; i < n; ++i) {
            std::uint8_t idx = byte();
            if (idx >= entries) idx = 0;
            bmp.rgba[i*4+0] = pal[idx].r; bmp.rgba[i*4+1] = pal[idx].g;
            bmp.rgba[i*4+2] = pal[idx].b; bmp.rgba[i*4+3] = pal[idx].a;
        }
    } else if (fmt == 4) {
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint16_t v = u16();
            if (withAlpha) {
                bmp.rgba[i*4+0] = static_cast<std::uint8_t>((v & 0x0F00) >> 4);
                bmp.rgba[i*4+1] = static_cast<std::uint8_t>((v & 0x00F0) << 0);
                bmp.rgba[i*4+2] = static_cast<std::uint8_t>((v & 0x000F) << 4);
                bmp.rgba[i*4+3] = static_cast<std::uint8_t>((v & 0xF000) >> 8);
            } else {
                bmp.rgba[i*4+0] = static_cast<std::uint8_t>((v & 0x7C00) >> 7);
                bmp.rgba[i*4+1] = static_cast<std::uint8_t>((v & 0x03E0) >> 2);
                bmp.rgba[i*4+2] = static_cast<std::uint8_t>((v & 0x001F) << 3);
                bmp.rgba[i*4+3] = 0xFF;
            }
        }
    } else if (fmt == 5) {
        for (std::size_t i = 0; i < n; ++i) {
            bmp.rgba[i*4+0] = byte();
            bmp.rgba[i*4+1] = byte();
            bmp.rgba[i*4+2] = byte();
            bmp.rgba[i*4+3] = withAlpha ? byte() : 0xFF;
        }
    }
}

}  // namespace swf
