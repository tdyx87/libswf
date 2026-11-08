// 软件光栅器实现：扫描线填充（even-odd）、纯色/渐变/位图填充、线描边、over 合成。
#include "swf/Render.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "swf/Decoder.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"


namespace swf {

namespace {

inline std::uint8_t clampU8(int v) {
    return static_cast<std::uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v);
}
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }


struct Paint {
    FillKind kind = FillKind::Solid;
    Color solid;
    bool tiled = true;
    std::vector<GradientStop> grad;
    Matrix gmat;                // 形状局部 -> 渐变空间（gradientMatrix 的逆）
    float focalX = 0.0f, focalY = 0.0f;  // 焦点渐变焦点（相对渐变中心）
    std::uint8_t spread = 0;    // GRADIENT SpreadMode：0=Pad 1=Reflect 2=Repeat
    std::uint8_t interp = 0;    // GRADIENT InterpolationMode：0=sRGB 1=Linear RGB
    const Bitmap* bmp = nullptr;
    Matrix bmat;                // 形状局部 -> 位图像素
    ColorTransform cx;
    // DefineShape4 的 UsesFillWindingRule：为 true 时该形状按非零环绕填充
    bool winding = false;
};

// sRGB <-> 线性光查表。InterpolationMode=LinearRGB 时 Flash 在线性光空间
// 插值色停、再转回 sRGB；直接用字节域 lerp 会让中间色明显偏暗/偏色
// （门户页气泡标签的橙→黄→橙渐变即是这种情况）。
static const std::array<float, 256>& srgbToLinearLut() {
    static const std::array<float, 256> lut = [] {
        std::array<float, 256> a{};
        for (int i = 0; i < 256; ++i) {
            float c = i / 255.0f;
            a[static_cast<std::size_t>(i)] =
                c <= 0.04045f ? c / 12.92f
                              : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        return a;
    }();
    return lut;
}
static const std::array<std::uint8_t, 4097>& linearToSrgbLut() {
    static const std::array<std::uint8_t, 4097> lut = [] {
        std::array<std::uint8_t, 4097> a{};
        for (int i = 0; i <= 4096; ++i) {
            float l = i / 4096.0f;
            float c = l <= 0.0031308f ? l * 12.92f
                                      : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
            a[static_cast<std::size_t>(i)] =
                static_cast<std::uint8_t>(std::max(0.0f, std::min(255.0f, c * 255.0f + 0.5f)));
        }
        return a;
    }();
    return lut;
}

// GRADIENT SpreadMode 作用于归一化参数 t（规范：Pad / Reflect / Repeat）。
inline float applySpread(float t, std::uint8_t mode) {
    if (mode == 2) return t - std::floor(t);               // Repeat
    if (mode == 1) {                                        // Reflect
        float u = t - 2.0f * std::floor(t * 0.5f);          // [0,2)
        return u > 1.0f ? 2.0f - u : u;
    }
    return std::max(0.0f, std::min(1.0f, t));               // Pad（含保留值 3）
}

Color sampleGrad(const std::vector<GradientStop>& g, float t, bool linearRGB = false) {
    if (g.empty()) return Color(0, 0, 0, 255);
    if (t <= g[0].ratio / 255.0f) return g[0].color;
    if (t >= g.back().ratio / 255.0f) return g.back().color;
    for (std::size_t i = 1; i < g.size(); ++i) {
        if (t <= g[i].ratio / 255.0f) {
            float r0 = g[i - 1].ratio / 255.0f, r1 = g[i].ratio / 255.0f;
            float f = (r1 - r0) > 1e-6f ? (t - r0) / (r1 - r0) : 0.0f;
            const Color& a = g[i - 1].color; const Color& b = g[i].color;
            if (linearRGB) {
                const auto& s2l = srgbToLinearLut();
                const auto& l2s = linearToSrgbLut();
                auto ch = [&](std::uint8_t x, std::uint8_t y) {
                    float v = lerp(s2l[x], s2l[y], f);
                    int k = static_cast<int>(v * 4096.0f + 0.5f);
                    if (k < 0) k = 0; if (k > 4096) k = 4096;
                    return l2s[static_cast<std::size_t>(k)];
                };
                return Color(ch(a.r, b.r), ch(a.g, b.g), ch(a.b, b.b),
                             clampU8(static_cast<int>(lerp(a.a, b.a, f))));
            }
            return Color(clampU8(static_cast<int>(lerp(a.r, b.r, f))),
                         clampU8(static_cast<int>(lerp(a.g, b.g, f))),
                         clampU8(static_cast<int>(lerp(a.b, b.b, f))),
                         clampU8(static_cast<int>(lerp(a.a, b.a, f))));
        }
    }
    return g.back().color;
}

// 平铺取模的快速版：fmodf 是 libm 调用、每像素两次时开销显著。
// 位图 uv 在正常变换下远小于 2^31 个平铺周期，整数商法足够。
inline float fastWrap(float v, float period) {
    float k = std::floor(v / period);
    return v - k * period;
}

// 在"填充空间"坐标（渐变空间 / 位图像素空间）直接取色，省一次仿射变换。
inline Color paintColorInPaintSpace(const Paint& p, float gx, float gy) {
    // SWF 渐变坐标空间的单位长度是 16384 twips（规范 GradRecord/渐变矩阵语义），
    // 不是可调参数。
    const float G = 16384.0f;
    Color c = p.solid;
    const bool linearRGB = (p.interp == 1);
    if (p.kind == FillKind::LinearGradient) {
        c = sampleGrad(p.grad, applySpread((gx + G) / (2.0f * G), p.spread), linearRGB);
    } else if (p.kind == FillKind::RadialGradient) {
        float r = std::sqrt(gx * gx + gy * gy) / G;
        c = sampleGrad(p.grad, applySpread(r, p.spread), linearRGB);
    } else if (p.kind == FillKind::FocalGradient) {
        gx /= G; gy /= G;                       // 归一化到半径 1
        const float fx = p.focalX, fy = p.focalY;
        const float fm = std::sqrt(fx * fx + fy * fy);
        float t;
        if (fm < 1e-6f) {
            t = std::sqrt(gx * gx + gy * gy);   // 无焦点偏移 == 径向
        } else {
            float vx = gx - fx, vy = gy - fy;
            float d = std::sqrt(vx * vx + vy * vy);
            if (d < 1e-6f) {
                t = 0.0f;                       // 正处焦点
            } else {
                float dx = vx / d, dy = vy / d; // 方向
                float fd = fx * dx + fy * dy;   // dot(f, dir)
                float disc = fd * fd + 1.0f - fm * fm;
                float s = -fd + std::sqrt(disc > 0.0f ? disc : 0.0f);
                t = (s > 1e-6f) ? (d / s) : d;
            }
        }
        c = sampleGrad(p.grad, applySpread(t, p.spread), linearRGB);
    } else if (p.kind == FillKind::Bitmap && p.bmp && p.bmp->width) {
        int w = p.bmp->width, h = p.bmp->height;
        float u = gx, v = gy;
        if (p.tiled) {
            u = fastWrap(u, static_cast<float>(w));
            v = fastWrap(v, static_cast<float>(h));
        } else if (u < 0 || v < 0 || u >= w || v >= h) {
            return Color(0, 0, 0, 0);
        }
        int ix = static_cast<int>(u), iy = static_cast<int>(v);
        if (ix < 0) ix = 0; if (ix >= w) ix = w - 1;
        if (iy < 0) iy = 0; if (iy >= h) iy = h - 1;
        const std::uint8_t* p4 = &p.bmp->rgba[static_cast<std::size_t>(iy) * w * 4 + ix * 4];
        c = Color(p4[0], p4[1], p4[2], p4[3]);
    }
    return p.cx.apply(c);
}

Color paintColor(const Paint& p, float lx, float ly) {
    // SWF 渐变空间：以渐变中心为原点、半径/半程 = 16384 twips（819.2px）的正方形。
    // 线性渐变沿 x 轴从 -16384 到 +16384；径向渐变为半径 16384 的圆。
    if (p.kind == FillKind::Solid) return p.cx.apply(p.solid);
    float gx, gy;
    if (p.kind == FillKind::Bitmap) p.bmat.apply(lx, ly, gx, gy);
    else p.gmat.apply(lx, ly, gx, gy);
    return paintColorInPaintSpace(p, gx, gy);
}

// 逐像素上色的设备空间预计算：把"设备->局部->填充空间"两次仿射合并为一次，
// 纯色样式完全免逐像素取色；不透明纯色 + Normal 混合可直写缓冲。
struct DevPaint {
    const Paint* pp = nullptr;
    Matrix Mu;                 // 设备 -> 填充空间（bmat/gmat ∘ Minv）
    bool solid = false;
    Color solidC;              // 已含 cxform 与 alphaMul
    bool opaqueDirect = false; // solid 且不透明且 Normal：可绕过 plot 直写
    void setup(const Paint& paint, const Matrix& Minv, float alphaMul, BlendMode bm) {
        pp = &paint;
        solid = (paint.kind == FillKind::Solid);
        if (solid) {
            solidC = paint.cx.apply(paint.solid);
            solidC.a = clampU8(static_cast<int>(solidC.a * alphaMul + 0.5f));
            opaqueDirect = (solidC.a == 255) && (bm == BlendMode::Normal);
        } else if (paint.kind == FillKind::Bitmap) {
            Mu = paint.bmat.post(Minv);
        } else {
            Mu = paint.gmat.post(Minv);
        }
    }
    inline Color color(float dx, float dy) const {
        if (solid) return solidC;
        float gx, gy; Mu.apply(dx, dy, gx, gy);
        return paintColorInPaintSpace(*pp, gx, gy);
    }
    // 行游标：填充空间坐标逐像素增量推进（2 次加法），替代每像素一次仿射变换。
    struct Cursor { float gx = 0, gy = 0, sx = 0, ry = 0; };
    inline Cursor cursor(float dx, float dy) const {
        Cursor c; Mu.apply(dx, dy, c.gx, c.gy); c.sx = Mu.sx; c.ry = Mu.r1; return c;
    }
    inline Color at(const Cursor& c) const { return paintColorInPaintSpace(*pp, c.gx, c.gy); }
};

// 行带并行：扫描线填充/遮罩合成各行互相独立（只写自己行的像素），按行带切给
// 多线程。逐像素着色是软件光栅器的帧率瓶颈，这是吞吐扩展的主要手段。
// 常驻线程池：每帧有成百上千个可并行行带（中尺寸形状居多），若每次提交都创建
// 线程，开销会吞掉收益；池化后单次提交仅一次条件变量唤醒。
// 约束：仅从单一渲染线程提交（run 阻塞至全部完成），工作线程不会嵌套提交。
struct BandPool {
    std::mutex mu;
    std::condition_variable cvWork, cvDone;
    std::deque<std::pair<int, int>> pending;
    std::function<void(const std::pair<int, int>&)>* job = nullptr;
    int remaining = 0;
    bool stop = false;
    std::vector<std::thread> workers;

    BandPool() {
        unsigned hw = std::thread::hardware_concurrency();
        int T = hw ? static_cast<int>(hw) : 2;
        if (T > 16) T = 16;   // 行带之间互不重叠，线程数上限只受硬件与内存带宽约束
        if (T < 2) T = 2;
        for (int i = 0; i < T - 1; ++i)
            workers.emplace_back([this] { loop(); });
    }
    ~BandPool() {
        { std::lock_guard<std::mutex> g(mu); stop = true; }
        cvWork.notify_all();
        for (auto& t : workers) t.join();
    }
    void loop() {
        for (;;) {
            std::pair<int, int> band;
            std::function<void(const std::pair<int, int>&)>* j;
            {
                std::unique_lock<std::mutex> g(mu);
                cvWork.wait(g, [this] { return stop || !pending.empty(); });
                if (stop && pending.empty()) return;
                band = pending.front();
                pending.pop_front();
                j = job;
            }
            (*j)(band);
            {
                std::lock_guard<std::mutex> g(mu);
                if (--remaining == 0) cvDone.notify_all();
            }
        }
    }
    template <class Fn>
    void run(int y0, int y1, int minRows, const Fn& fn) {
        const int rows = y1 - y0 + 1;
        const int T = static_cast<int>(workers.size()) + 1;
        if (rows < minRows || T < 2) { fn(y0, y1); return; }
        std::function<void(const std::pair<int, int>&)> wrapper =
            [&fn](const std::pair<int, int>& ab) { fn(ab.first, ab.second); };
        {
            std::lock_guard<std::mutex> g(mu);
            job = &wrapper;
            const int band = (rows + T - 1) / T;
            remaining = 0;
            for (int t = 0; t < T; ++t) {
                int a = y0 + t * band, b = std::min(y1, a + band - 1);
                if (a > b) break;
                pending.push_back({a, b});
                ++remaining;
            }
        }
        cvWork.notify_all();
        for (;;) {   // 提交线程也参与消费行带
            std::pair<int, int> ab;
            {
                std::lock_guard<std::mutex> g(mu);
                if (pending.empty()) break;
                ab = pending.front();
                pending.pop_front();
            }
            wrapper(ab);
            {
                std::lock_guard<std::mutex> g(mu);
                --remaining;
            }
        }
        std::unique_lock<std::mutex> g(mu);
        cvDone.wait(g, [this] { return remaining == 0; });
    }
};

template <class Fn>
void parallelBands(int y0, int y1, const Fn& fn) {
    static BandPool pool;
    pool.run(y0, y1, 32, fn);
}

// 把 (lx,ly) 设备像素反算为形状局部 twips，再上色并合成到 buf

void plot(std::uint8_t* buf, int W, int H, int px, int py, const Color& c, BlendMode bm = BlendMode::Normal) {
    if (px < 0 || py < 0 || px >= W || py >= H) return;

    float sa = c.a / 255.0f;
    std::uint8_t* d = buf + (static_cast<std::size_t>(py) * W + px) * 4;
    float da = d[3] / 255.0f;
    // 仅调制/削减 dst alpha 的混合模式
    if (bm == BlendMode::Erase) {
        float outA = da * (1.0f - sa);
        d[3] = clampU8(static_cast<int>(outA * 255.0f));
        return;
    }
    if (bm == BlendMode::Alpha) {
        float outA = da * sa;
        d[3] = clampU8(static_cast<int>(outA * 255.0f));
        return;
    }
    if (sa <= 0.0f) return;
    // 不透明源 + Normal：over 的结果就是源本身，直写可省掉每像素的浮点混合与除法。
    if (sa >= 1.0f && bm == BlendMode::Normal) {
        d[0] = c.r; d[1] = c.g; d[2] = c.b; d[3] = 255;
        return;
    }
    // 计算混合后的源 RGB（src 与 dst 按模式混合）
    float sr = c.r, sg = c.g, sb = c.b;
    float dr = d[0], dg = d[1], db = d[2];
    float br = sr, bg = sg, bb = sb;
    switch (bm) {
        case BlendMode::Add:      br = sr + dr; bg = sg + dg; bb = sb + db; break;
        case BlendMode::Multiply: br = sr * dr / 255.0f; bg = sg * dg / 255.0f; bb = sb * db / 255.0f; break;
        case BlendMode::Screen:   br = 255.0f - (255.0f - sr) * (255.0f - dr) / 255.0f;
                                  bg = 255.0f - (255.0f - sg) * (255.0f - dg) / 255.0f;
                                  bb = 255.0f - (255.0f - sb) * (255.0f - db) / 255.0f; break;
        case BlendMode::Lighten:  br = std::max(sr, dr); bg = std::max(sg, dg); bb = std::max(sb, db); break;
        case BlendMode::Darken:   br = std::min(sr, dr); bg = std::min(sg, dg); bb = std::min(sb, db); break;
        case BlendMode::Difference: br = std::fabs(sr - dr); bg = std::fabs(sg - dg); bb = std::fabs(sb - db); break;
        case BlendMode::Subtract: br = dr - sr; bg = dg - sg; bb = db - sb; break;
        case BlendMode::Invert:   br = 255.0f - dr; bg = 255.0f - dg; bb = 255.0f - db; break;
        default: break;   // Normal / Layer / Overlay / HardLight：标准 over
    }
    br = clampU8(static_cast<int>(br + 0.5f));
    bg = clampU8(static_cast<int>(bg + 0.5f));
    bb = clampU8(static_cast<int>(bb + 0.5f));
    if (sa >= 1.0f) {
        d[0] = static_cast<std::uint8_t>(br); d[1] = static_cast<std::uint8_t>(bg);
        d[2] = static_cast<std::uint8_t>(bb); d[3] = 255;
        return;
    }
    float outA = sa + da * (1.0f - sa);
    if (outA <= 0.0f) return;
    d[0] = static_cast<std::uint8_t>((br * sa + d[0] * da * (1.0f - sa)) / outA);
    d[1] = static_cast<std::uint8_t>((bg * sa + d[1] * da * (1.0f - sa)) / outA);
    d[2] = static_cast<std::uint8_t>((bb * sa + d[2] * da * (1.0f - sa)) / outA);
    d[3] = clampU8(static_cast<int>(outA * 255.0f));
}

// even-odd 扫描线填充一组轮廓（设备坐标），按 Paint 上色
// alphaMul：对象级透明度（运行时 DisplayObject.alpha 等）乘到源色 alpha 上。
void fillContours(const std::vector<Contour>& contours, const Matrix& M, const Matrix& Minv,
                  const Paint& paint, std::uint8_t* buf, int W, int H, float alphaMul = 1.0f,
                  BlendMode bm = BlendMode::Normal) {
    struct Edge { float x0, y0, x1, y1; };
    // 每个轮廓单独成组（保留下面的"嵌套 = 洞、部分重叠 = 并集"判定）。
    struct Poly {
        std::vector<Edge> edges;
        float xmin = 1e9f, ymin = 1e9f, xmax = -1e9f, ymax = -1e9f;
        int depth = 0;      // 嵌套层级：偶数=实体，奇数=洞
    };
    std::vector<Poly> polys;
    float ymin = 1e9f, ymax = -1e9f, xmin = 1e9f, xmax = -1e9f;
    for (const auto& ct : contours) {
        int n = static_cast<int>(ct.xs.size());
        if (n < 2) continue;
        std::vector<float> dx(n), dy(n);
        Poly p;
        for (int i = 0; i < n; ++i) {
            float X, Y; M.apply(ct.xs[i], ct.ys[i], X, Y);
            dx[i] = X; dy[i] = Y;
            p.xmin = std::min(p.xmin, X); p.xmax = std::max(p.xmax, X);
            p.ymin = std::min(p.ymin, Y); p.ymax = std::max(p.ymax, Y);
        }
        p.edges.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            int j = (i + 1) % n;
            p.edges.push_back({dx[i], dy[i], dx[j], dy[j]});
        }
        ymin = std::min(ymin, p.ymin);
        ymax = std::max(ymax, p.ymax);
        xmin = std::min(xmin, p.xmin);
        xmax = std::max(xmax, p.xmax);
        polys.push_back(std::move(p));
    }
    if (polys.empty()) return;

    // 嵌套层级：统计有多少个"面积更大且外包住自己"的轮廓。
    // SWF 用**嵌套轮廓**表达洞（例如 swf.swf 里一个外框 + 恰好画布大小的内框的
    // 黑色填充，两条轮廓绕行方向相同，只有靠嵌套关系才能判成"环"），
    // 而角色的图形由大量**部分重叠**的笔触构成，那些必须取并集，否则 even-odd
    // 会把重叠处互相抵消、画面出现成片竖条纹。
    // 因此规则：嵌套（被更大轮廓完全包含）= 洞，从覆盖中扣除；
    //          部分重叠 = 并集，取最大覆盖。
    for (std::size_t i = 0; i < polys.size(); ++i) {
        int d = 0;
        for (std::size_t j = 0; j < polys.size(); ++j) {
            if (i == j) continue;
            const Poly& a = polys[j];
            const Poly& b = polys[i];
            float aArea = (a.xmax - a.xmin) * (a.ymax - a.ymin);
            float bArea = (b.xmax - b.xmin) * (b.ymax - b.ymin);
            if (aArea <= bArea) continue;                       // 必须面积更大
            // 包围盒外包只是**必要条件**，不能作为判据：斜置或凹形轮廓的
            // 包围盒可以落在另一个轮廓内，而轮廓本身大部分在外。只看包围盒
            // 会把这类轮廓误判成"洞"而整块扣除，地面因此出现成片楔形空洞。
            // 真正判定：统计 b 的顶点有多少落在 a 的内部。
            if (!(a.xmin <= b.xmin && a.ymin <= b.ymin &&
                  a.xmax >= b.xmax && a.ymax >= b.ymax)) continue;
            int inside = 0, total = 0;
            for (const auto& ed : b.edges) {
                ++total;
                float x = ed.x0, y = ed.y0;
                bool in = false;                 // 射线法（向 +x 方向）
                for (const auto& ea : a.edges) {
                    if ((ea.y0 > y) != (ea.y1 > y)) {
                        float xi = ea.x0 + (y - ea.y0) * (ea.x1 - ea.x0) / (ea.y1 - ea.y0);
                        if (x < xi) in = !in;
                    }
                }
                if (in) ++inside;
            }
            if (total > 0 && inside * 2 > total) ++d;           // 过半顶点在内才算包含
        }
        // DefineShape4 明确要求非零环绕时，所有轮廓按并集处理（不做嵌套挖洞）
        polys[i].depth = paint.winding ? 0 : d;
    }

    int y0 = std::max(0, static_cast<int>(std::floor(ymin)));
    int y1 = std::min(H - 1, static_cast<int>(std::ceil(ymax)));
    int x0 = std::max(0, static_cast<int>(std::floor(xmin)));
    int x1 = std::min(W - 1, static_cast<int>(std::ceil(xmax)));
    if (x0 > x1) return;
    DevPaint dp; dp.setup(paint, Minv, alphaMul, bm);
    parallelBands(y0, y1, [&](int ya, int yb) {
        // 行缓冲按线程私有分配（行带内复用），避免跨线程共享写。
        std::vector<float> xs, solidCov(static_cast<std::size_t>(W), 0.0f),
            holeCov(static_cast<std::size_t>(W), 0.0f), cov(static_cast<std::size_t>(W), 0.0f);
        for (int y = ya; y <= yb; ++y) {
            std::fill(solidCov.begin() + x0, solidCov.begin() + x1 + 1, 0.0f);
            std::fill(holeCov.begin() + x0, holeCov.begin() + x1 + 1, 0.0f);
            for (const auto& p : polys) {
                // 该行两条子扫描线都不与轮廓 y 范围相交则跳过
                if (p.ymax <= y + 0.25f || p.ymin >= y + 0.75f) continue;
                std::fill(cov.begin() + x0, cov.begin() + x1 + 1, 0.0f);
                // 2x2 超采样：每像素行取 2 条子扫描线，各贡献 0.5 覆盖率（水平抗锯齿）
                for (int sub = 0; sub < 2; ++sub) {
                    float yy = y + (sub == 0 ? 0.25f : 0.75f);
                    xs.clear();
                    for (const auto& e : p.edges) {
                        if ((e.y0 <= yy && e.y1 > yy) || (e.y1 <= yy && e.y0 > yy)) {
                            float t = (yy - e.y0) / (e.y1 - e.y0);
                            xs.push_back(e.x0 + t * (e.x1 - e.x0));
                        }
                    }
                    if (xs.size() < 2) continue;
                    std::sort(xs.begin(), xs.end());
                    for (std::size_t i = 0; i + 1 < xs.size(); i += 2) {
                        // 水平方向按**小数覆盖率**累加，而不是"整像素开关"：
                        // 按跨度与像素区间 [x-0.5, x+0.5) 的重叠长度计权，
                        // 细笔触会平滑成半透明过渡，避免 0/0.5/1.0 的硬跳变。
                        float A = xs[i] - 0.5f, B = xs[i + 1] - 0.5f;
                        if (B <= A) continue;
                        int xa = std::max(x0, static_cast<int>(std::floor(A - 0.5f)));
                        int xb = std::min(x1, static_cast<int>(std::ceil(B + 0.5f)));
                        for (int x = xa; x <= xb; ++x) {
                            float l = std::max(A, static_cast<float>(x) - 0.5f);
                            float r = std::min(B, static_cast<float>(x) + 0.5f);
                            if (r > l) cov[x] += 0.5f * (r - l);
                        }
                    }
                }
                std::vector<float>& dst = (p.depth & 1) ? holeCov : solidCov;
                for (int x = x0; x <= x1; ++x)
                    if (cov[x] > dst[x]) dst[x] = cov[x];
            }
            // 用像素中心采样上色，按覆盖率调制 alpha 后合成
            DevPaint::Cursor cur{};
            if (!dp.solid) cur = dp.cursor(x0 + 0.5f, y + 0.5f);
            for (int x = x0; x <= x1; ++x, cur.gx += cur.sx, cur.gy += cur.ry) {
                float cover = solidCov[x] - holeCov[x];
                if (cover <= 0.0f) continue;
                if (cover > 1.0f) cover = 1.0f;
                if (dp.opaqueDirect && cover >= 1.0f) {   // 不透明纯色内部像素：直写
                    std::uint8_t* d = buf + (static_cast<std::size_t>(y) * W + x) * 4;

                    d[0] = dp.solidC.r; d[1] = dp.solidC.g; d[2] = dp.solidC.b; d[3] = 255;
                    continue;
                }
                Color c = dp.solid ? dp.solidC : dp.at(cur);
                if (!dp.solid)
                    c.a = clampU8(static_cast<int>(c.a * alphaMul + 0.5f));   // 对象级透明度
                if (cover < 1.0f) {
                    float a = (c.a / 255.0f) * cover;
                    c.a = clampU8(static_cast<int>(a * 255.0f + 0.5f));
                    if (c.a == 0) continue;
                } else if (c.a == 255 && bm == BlendMode::Normal) {
                    // 采样结果不透明（位图/渐变内部像素常见）：免浮点混合直写
                    std::uint8_t* d = buf + (static_cast<std::size_t>(y) * W + x) * 4;

                    d[0] = c.r; d[1] = c.g; d[2] = c.b; d[3] = 255;
                    continue;
                }
                plot(buf, W, H, x, y, c, bm);
            }
        }
    });
}

// 非零环绕（non-zero winding）扫描线填充：每条边带符号（+1 = 该样式在边右侧，
// -1 = 在边左侧）。扫描线从左到右累加环绕数，环绕数 != 0 的区间即为该样式的区域。
// 与"嵌套=洞 / 部分重叠=并集"启发式相比，此规则天然同时正确处理两种情况：
//   同向重叠轮廓 -> 环绕数累加（并集）；反向嵌套轮廓 -> 环绕数抵消（洞）。
struct DEdge { float x0, y0, x1, y1; int sign; };

// 对一组设备坐标边，计算扫描线行 y 的覆盖率（2 条子扫描线，水平抗锯齿）。
// rule: 0 = 带符号非零环绕（默认）；1 = 偶奇；2 = 无符号并集（忽略侧号）。
static void edgesCoverageRow(const std::vector<DEdge>& es, const std::vector<int>& act, int y,
                             int x0, int x1, float* cov, int rule,
                             std::vector<std::pair<float, int>>& xs) {
    std::fill(cov + x0, cov + x1 + 1, 0.0f);
    for (int sub = 0; sub < 2; ++sub) {          // 2x2 超采样（水平抗锯齿）
        float yy = y + (sub == 0 ? 0.25f : 0.75f);
        xs.clear();
        for (int ei : act) {
            const DEdge& e = es[ei];
            if ((e.y0 <= yy && e.y1 > yy) || (e.y1 <= yy && e.y0 > yy)) {
                float t = (yy - e.y0) / (e.y1 - e.y0);
                float x = e.x0 + t * (e.x1 - e.x0);
                int s = (rule == 2) ? ((e.y1 > e.y0) ? 1 : -1)      // 无符号并集
                                    : ((e.y1 > e.y0) ? e.sign : -e.sign);
                xs.push_back({x, s});
            }
        }
        if (xs.size() < 2) continue;
        std::sort(xs.begin(), xs.end(),
                  [](const std::pair<float, int>& a, const std::pair<float, int>& b) {
                      return a.first < b.first;
                  });
        int w = 0;
        bool inEO = false;
        for (std::size_t k = 0; k + 1 < xs.size(); ++k) {
            if (rule == 1) { inEO = !inEO; if (!inEO) continue; }   // 偶奇
            else { w += xs[k].second; if (w == 0) continue; }       // 环绕数归零 = 区间外
            float A = xs[k].first - 0.5f, B = xs[k + 1].first - 0.5f;
            if (B <= A) continue;
            int xa = std::max(x0, static_cast<int>(std::floor(A - 0.5f)));
            int xb = std::min(x1, static_cast<int>(std::ceil(B + 0.5f)));
            for (int x = xa; x <= xb; ++x) {
                float l = std::max(A, static_cast<float>(x) - 0.5f);
                float r = std::min(B, static_cast<float>(x) + 0.5f);
                if (r > l) cov[x] += 0.5f * (r - l);
            }
        }
    }
}

void fillEdgesWinding(const std::vector<FillEdge>& edges, const Matrix& M, const Matrix& Minv,
                      const Paint& paint, std::uint8_t* buf, int W, int H,
                      float alphaMul = 1.0f, BlendMode bm = BlendMode::Normal) {
    if (edges.empty()) return;
    typedef DEdge E;
    // 边表按整次填充一次性构建：变换后的边与包围盒只算一遍，行带内复用。
    std::vector<E> es;
    es.clear();
    es.reserve(edges.size());
    float ymin = 1e9f, ymax = -1e9f, xmin = 1e9f, xmax = -1e9f;
    for (const auto& e : edges) {
        // 只有在**变换后**（设备空间）仍水平、不参与扫描线相交的边才可跳过。
        // 形状局部为水平、但经旋转/斜切后变为斜边的情况必须保留，否则会漏交点。
        float X0, Y0, X1, Y1;
        M.apply(e.x0, e.y0, X0, Y0);
        M.apply(e.x1, e.y1, X1, Y1);
        if (Y0 == Y1) continue;
        es.push_back({X0, Y0, X1, Y1, e.sign});
        ymin = std::min(ymin, std::min(Y0, Y1));
        ymax = std::max(ymax, std::max(Y0, Y1));
        xmin = std::min(xmin, std::min(X0, X1));
        xmax = std::max(xmax, std::max(X0, X1));
    }
    if (es.empty()) return;

    int yy0 = std::max(0, static_cast<int>(std::floor(ymin)));
    int yy1 = std::min(H - 1, static_cast<int>(std::ceil(ymax)));
    // 水平包围盒裁剪：复杂形状（数百个填充样式）里每个样式只覆盖局部区域，
    // 若仍按整幅宽度清覆盖率并逐像素上色，绝大部分时间花在空白像素上。
    int xx0 = std::max(0, static_cast<int>(std::floor(xmin)));
    int xx1 = std::min(W - 1, static_cast<int>(std::ceil(xmax)));
    if (xx0 > xx1) return;
    DevPaint dp; dp.setup(paint, Minv, alphaMul, bm);
    // 默认按非零环绕规则（规范 UsesFillWindingRule 决定，形状级已在 Paint::winding 中处理）。
    const int rule = 0;

    // 活动边表：按边起始扫描线排序。行带内逐行"加入新边、移除已结束边"，
    // 单行只遍历当前与该行相交的边。原实现每行遍历形状全部边，复杂形状
    // （边数可达上千）时每帧耗时随行数×边数增长，是填充的主要瓶颈。
    std::vector<int> yStart, yEnd, order;
    yStart.resize(es.size()); yEnd.resize(es.size()); order.resize(es.size());
    for (std::size_t i = 0; i < es.size(); ++i) {
        yStart[i] = static_cast<int>(std::floor(std::min(es[i].y0, es[i].y1)));
        yEnd[i]   = static_cast<int>(std::ceil(std::max(es[i].y0, es[i].y1)));
        order[i]  = static_cast<int>(i);
    }
    std::sort(order.begin(), order.end(),
              [&](int a, int b) { return yStart[a] < yStart[b] || (yStart[a] == yStart[b] && a < b); });

    parallelBands(yy0, yy1, [&](int a, int b) {
        // 行带任务运行在工作线程上，故这里声明的缓冲天然是每线程一份、跨任务复用。
        static thread_local std::vector<std::pair<float, int>> xs;
        static thread_local std::vector<float> cov;
        static thread_local std::vector<int> act;
        if (cov.size() < static_cast<std::size_t>(W)) cov.resize(static_cast<std::size_t>(W));
        act.clear();
        xs.clear();
        std::size_t next = 0;
        while (next < order.size() && yStart[order[next]] <= a) act.push_back(order[next++]);
        for (int y = a; y <= b; ++y) {
            while (next < order.size() && yStart[order[next]] <= y) act.push_back(order[next++]);
            if (!act.empty())
                act.erase(std::remove_if(act.begin(), act.end(),
                                         [&](int i) { return yEnd[i] <= y; }), act.end());
            edgesCoverageRow(es, act, y, xx0, xx1, cov.data(), rule, xs);
            DevPaint::Cursor cur{};
            if (!dp.solid) cur = dp.cursor(xx0 + 0.5f, y + 0.5f);
            for (int x = xx0; x <= xx1; ++x, cur.gx += cur.sx, cur.gy += cur.ry) {
                float cover = cov[x];
                if (cover <= 0.0f) continue;
                if (cover > 1.0f) cover = 1.0f;
                if (dp.opaqueDirect && cover >= 1.0f) {   // 不透明纯色内部像素：直写
                    std::uint8_t* d = buf + (static_cast<std::size_t>(y) * W + x) * 4;

                    d[0] = dp.solidC.r; d[1] = dp.solidC.g; d[2] = dp.solidC.b; d[3] = 255;
                    continue;
                }
                Color c = dp.solid ? dp.solidC : dp.at(cur);
                if (!dp.solid)
                    c.a = clampU8(static_cast<int>(c.a * alphaMul + 0.5f));
                if (cover < 1.0f) {
                    float a2 = (c.a / 255.0f) * cover;
                    c.a = clampU8(static_cast<int>(a2 * 255.0f + 0.5f));
                    if (c.a == 0) continue;
                } else if (c.a == 255 && bm == BlendMode::Normal) {
                    std::uint8_t* d = buf + (static_cast<std::size_t>(y) * W + x) * 4;

                    d[0] = c.r; d[1] = c.g; d[2] = c.b; d[3] = 255;
                    continue;
                }
                plot(buf, W, H, x, y, c, bm);
            }
        }
    });
}

// 光栅化形状的第 i 个填充样式：优先使用有向边 + 非零环绕（权威几何）；
// 缺边时回退到轮廓 + 启发式（用于变形插值等没有边数据的场合）。
void fillStyleOf(const Shape& shape, std::size_t i, const Matrix& M, const Matrix& Minv,
                 const Paint& p, std::uint8_t* buf, int W, int H, float alphaMul, BlendMode bm) {
    if (i < shape.fillEdges.size() && !shape.fillEdges[i].empty()) {
        fillEdgesWinding(shape.fillEdges[i], M, Minv, p, buf, W, H, alphaMul, bm);
    } else if (i < shape.fillPaths.size() && !shape.fillPaths[i].empty()) {
        fillContours(shape.fillPaths[i], M, Minv, p, buf, W, H, alphaMul, bm);
    }
}

// 直接光栅化一段粗线：对线段外接矩形内的像素求"到线段的距离"，
// 用距离与半宽的比值做覆盖率。相比"每段拼一个四边形再走通用填充"，
// 这条路是有界的、不会因为退化/自交四边形而糊出大片色块。
static void drawThickSegment(float axp, float ayp, float bxp, float byp, float hw,
                             const Color& color, std::uint8_t* buf, int W, int H,
                             float alphaMul, BlendMode bm, int bandLo, int bandHi) {
    float dx = bxp - axp, dy = byp - ayp;
    float len2 = dx * dx + dy * dy;
    int x0 = std::max(0, static_cast<int>(std::floor(std::min(axp, bxp) - hw - 1)));
    int x1 = std::min(W - 1, static_cast<int>(std::ceil(std::max(axp, bxp) + hw + 1)));
    int y0 = std::max(std::max(0, bandLo), static_cast<int>(std::floor(std::min(ayp, byp) - hw - 1)));
    int y1 = std::min(std::min(H - 1, bandHi), static_cast<int>(std::ceil(std::max(ayp, byp) + hw + 1)));
    if (x0 > x1 || y0 > y1) return;
    // 覆盖率 > 0 的像素恰为「到线段距离 < r」的胶囊：r = 半宽 + 半像素。
    Color base = color;
    base.a = clampU8(static_cast<int>(base.a * alphaMul + 0.5f));
    if (base.a == 0) return;
    const float r = hw + 0.5f;
    const float rInner = hw - 0.5f;               // 该半径内覆盖率恒为 1（可整片实心填充）
    const float len = std::sqrt(len2);
    // 胶囊 = 以线段为轴的矩形 ∪ 两端圆盘。逐行求它们的并集 x 区间：
    // 胶囊是凸的，故该行并集就是一个区间，取各分量区间的 min/max 即完整覆盖范围，
    // 斜线段因此不必再扫过轴对齐包围盒里的空白角落。
    float nx = 0.0f, ny = 0.0f;
    if (len > 1e-6f) { nx = -dy / len; ny = dx / len; }
    // 预生成外缘/内缘两条胶囊的矩形四角；圆盘半径在逐行时使用。
    float qxO[4], qyO[4], qxI[4], qyI[4];
    auto buildQuad = [&](float R, float* qx, float* qy) {
        qx[0] = axp + nx * R; qy[0] = ayp + ny * R;
        qx[1] = bxp + nx * R; qy[1] = byp + ny * R;
        qx[2] = bxp - nx * R; qy[2] = byp - ny * R;
        qx[3] = axp - nx * R; qy[3] = ayp - ny * R;
    };
    buildQuad(r, qxO, qyO);
    const bool hasInner = rInner > 0.0f;
    if (hasInner) buildQuad(rInner, qxI, qyI);
    auto rowSpan = [&](float py, float R, const float* qx, const float* qy, float& lo, float& hi) {
        lo = 1e9f; hi = -1e9f;
        for (int k = 0; k < 4; ++k) {
            int k2 = (k + 1) & 3;
            float ea = qy[k], eb = qy[k2];
            if ((ea <= py && eb > py) || (eb <= py && ea > py)) {
                float t = (py - ea) / (eb - ea);
                float xx = qx[k] + t * (qx[k2] - qx[k]);
                lo = std::min(lo, xx);
                hi = std::max(hi, xx);
            }
        }
        const float R2 = R * R;
        float d0 = py - ayp;
        if (d0 > -R && d0 < R) {
            float s = std::sqrt(R2 - d0 * d0);
            lo = std::min(lo, axp - s); hi = std::max(hi, axp + s);
        }
        float d1 = py - byp;
        if (d1 > -R && d1 < R) {
            float s = std::sqrt(R2 - d1 * d1);
            lo = std::min(lo, bxp - s); hi = std::max(hi, bxp + s);
        }
    };

    auto plotDist = [&](int x, int y, float py) {
        float px = x + 0.5f;
        float t = 0.0f;
        if (len2 > 1e-9f) {
            t = ((px - axp) * dx + (py - ayp) * dy) / len2;
            if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
        }
        float cx = axp + t * dx, cy = ayp + t * dy;
        float ddx = px - cx, ddy = py - cy;
        float d = std::sqrt(ddx * ddx + ddy * ddy);
        float cover = hw + 0.5f - d;      // 半宽 + 半像素做简单抗锯齿
        if (cover <= 0.0f) return;
        if (cover > 1.0f) cover = 1.0f;
        Color c = base;
        float a = (c.a / 255.0f) * cover;
        c.a = clampU8(static_cast<int>(a * 255.0f + 0.5f));
        if (c.a == 0) return;
        plot(buf, W, H, x, y, c, bm);
    };
    for (int y = y0; y <= y1; ++y) {
        float py = y + 0.5f;
        float lo, hi;
        rowSpan(py, r, qxO, qyO, lo, hi);

        if (lo > hi) continue;
        int xa = std::max(x0, static_cast<int>(std::floor(lo)) - 1);
        int xb = std::min(x1, static_cast<int>(std::ceil(hi)) + 1);
        if (xa > xb) continue;   // 该行胶囊区间完全落在屏幕外，无可写像素
        // 内部满覆盖区：裁掉 1px 余量保证仍落在 cover>=1 的真实范围内，
        // 其余像素（仅两侧各约 1px 宽的抗锯齿边）才走距离场。
        int ia = xb + 1, ib = xa - 1;
        if (hasInner) {
            float loI, hiI;
            rowSpan(py, rInner, qxI, qyI, loI, hiI);
            if (loI <= hiI) {
                ia = std::max(xa, static_cast<int>(std::ceil(loI - 0.5f)) + 1);
                ib = std::min(xb, static_cast<int>(std::floor(hiI - 0.5f)) - 1);
            }
        }
        // 纵深防御：直写不经过 plot() 的边界检查，必须把区间夹回本行合法范围。
        // 否则 ia/ib 一旦因异常浮点值算出界，x >= W 会回绕写到下一行开头。
        ia = std::max(ia, xa);
        ib = std::min(ib, xb);
        if (ib >= ia) {
            for (int x = xa; x < ia; ++x) plotDist(x, y, py);
            if (base.a == 255 && alphaMul >= 1.0f && bm == BlendMode::Normal) {
                // 不透明描边内部：整行直写，避免每像素调用 plot 做浮点混合
                std::uint8_t* row = buf + (static_cast<std::size_t>(y) * W) * 4;
                for (int x = ia; x <= ib; ++x) {
                    std::uint8_t* d = row + x * 4;
                    d[0] = base.r; d[1] = base.g; d[2] = base.b; d[3] = 255;
                }
            } else {
                for (int x = ia; x <= ib; ++x) plot(buf, W, H, x, y, base, bm);
            }
            for (int x = ib + 1; x <= xb; ++x) plotDist(x, y, py);
        } else {
            for (int x = xa; x <= xb; ++x) plotDist(x, y, py);
        }

    }

}

void strokeContours(const std::vector<Contour>& contours, const Matrix& M, const Color& color,
                    float widthTwips, std::uint8_t* buf, int W, int H, float alphaMul = 1.0f,
                    BlendMode bm = BlendMode::Normal) {
    // 描边宽度随对象变换缩放（SWF 的普通描边会随放置矩阵等比缩放；只有
    // DefineShape4 的 UsesNonScalingStrokes 例外，这里按等比缩放近似）。
    // 用 |det(M)| 的平方根作为等效缩放系数（各向异性时取几何平均）。
    // M 把 twips 直接映射到设备像素，故 sqrt(|det M|) = 每 twip 对应的设备像素数。
    // 线宽(device px) = widthTwips * sqrt(|det M|)，半宽取一半。
    float scale = std::sqrt(std::fabs(M.sx * M.sy - M.r0 * M.r1));
    if (!(scale > 1e-9f)) scale = 0.05f;             // 退化矩阵：按 1px/20twip 兜底
    float hw = widthTwips * scale * 0.5f;
    if (hw < 0.25f) hw = 0.25f;
    // 线段先统一变换到设备空间并记录各自行范围；描边按行带并行（行之间互不重叠，
    // 同一行的像素只由同一条带的工作线程写入）。逐段串行在粗描边/长轮廓时是单线程
    // 瓶颈，按行分带后可直接用满线程池。
    struct Seg { float ax, ay, bx, by; int y0, y1; };
    std::vector<Seg> segs;
    float gy0 = 1e9f, gy1 = -1e9f;
    for (const auto& ct : contours) {
        int n = static_cast<int>(ct.xs.size());
        for (int i = 0; i < n - 1; ++i) {
            float axp, ayp, bxp, byp;
            M.apply(ct.xs[i], ct.ys[i], axp, ayp);
            M.apply(ct.xs[i + 1], ct.ys[i + 1], bxp, byp);
            int y0 = static_cast<int>(std::floor(std::min(ayp, byp) - hw - 1));
            int y1 = static_cast<int>(std::ceil(std::max(ayp, byp) + hw + 1));
            segs.push_back({axp, ayp, bxp, byp, y0, y1});
            gy0 = std::min(gy0, static_cast<float>(std::min(ayp, byp)));
            gy1 = std::max(gy1, static_cast<float>(std::max(ayp, byp)));
        }
    }
    if (segs.empty()) return;

    int by0 = std::max(0, static_cast<int>(std::floor(gy0 - hw - 1)));
    int by1 = std::min(H - 1, static_cast<int>(std::ceil(gy1 + hw + 1)));
    if (by0 > by1) return;
    parallelBands(by0, by1, [&](int ya, int yb) {
        for (const auto& s : segs) {
            if (s.y1 < ya || s.y0 > yb) continue;
            drawThickSegment(s.ax, s.ay, s.bx, s.by, hw, color, buf, W, H, alphaMul, bm, ya, yb);
        }
    });
}

void renderShape(const Shape& shape, const Matrix& world, const ColorTransform& cx,
                 const std::map<std::uint16_t, std::shared_ptr<Character>>& dict,
                 std::uint8_t* buf, int W, int H, float alphaMul = 1.0f,
                 BlendMode bm = BlendMode::Normal) {
    Matrix Minv; world.invert(Minv);

    // 填充
    auto drawFill = [&](std::size_t i) {
        if (i >= shape.fillStyles.size()) return;
        if (i >= shape.fillEdges.size() && i >= shape.fillPaths.size()) return;
        const FillStyle& fs = shape.fillStyles[i];
        Paint p;
        p.kind = fs.kind; p.solid = fs.color; p.cx = cx; p.tiled = fs.tiled;
        p.spread = fs.spreadMode; p.interp = fs.interpMode;
        p.winding = shape.usesWinding;
        if (fs.kind == FillKind::LinearGradient || fs.kind == FillKind::RadialGradient ||
            fs.kind == FillKind::FocalGradient) {
            p.grad = fs.gradient;
            // SWF 的 gradientMatrix 是「渐变空间 -> 形状空间」，采样需用其逆
            // 把形状局部坐标映射回渐变空间。旋转分量的存储约定已在 readMatrix
            // 中按矩阵布局归一（r0 = RotateSkew1、r1 = RotateSkew0），这里直接求逆。
            Matrix g = fs.gradientMatrix;
            if (!g.invert(p.gmat)) p.gmat = fs.gradientMatrix;
            p.focalX = fs.focalX; p.focalY = fs.focalY;
        } else if (fs.kind == FillKind::Bitmap) {
            auto it = dict.find(fs.bitmapId);
            if (it != dict.end() && it->second->bitmap) {
                p.bmp = it->second->bitmap.get();
                // 与渐变矩阵同约定：bitmapMatrix 是「位图空间 -> 形状空间」，
                // 采样需用其逆把形状局部（twips）映射回位图像素坐标。
                Matrix b = fs.gradientMatrix;
                if (!b.invert(p.bmat)) p.bmat = fs.gradientMatrix;
            }
        }
        fillStyleOf(shape, i, world, Minv, p, buf, W, H, alphaMul, bm);
    };
    auto drawStroke = [&](std::size_t i) {
        if (i >= shape.lineStyles.size() || shape.strokePaths[i].empty()) return;
        const LineStyle& ls = shape.lineStyles[i];
        strokeContours(shape.strokePaths[i], world, ls.color, ls.width, buf, W, H, alphaMul, bm);
    };
    // 按形状记录流中样式路径被刷新的先后绘制（Flash 语义）：后出现的填充会盖住
    // 先出现的描边。形状 27（"开始"）的 StateNewStyles 追加渐变填充，Flash 中
    // 该渐变必须画在前一批的 45twips 描边之上，否则描边会压在渐变文字上使文字发暗。
    if (!shape.drawOrder.empty()) {
        for (const auto& op : shape.drawOrder) {
            if (op.style < 0) continue;
            if (op.isLine) drawStroke(static_cast<std::size_t>(op.style));
            else drawFill(static_cast<std::size_t>(op.style));
        }
        return;
    }
    // 无绘制顺序数据（例如手工构造的形状）：回退到"先填充后描边"。
    for (std::size_t i = 0; i < shape.fillStyles.size(); ++i) drawFill(i);
    for (std::size_t i = 0; i < shape.lineStyles.size(); ++i) drawStroke(i);
}

// 把一组填充路径光栅化为遮罩（255=内部，多路径 even-odd 合并），设备坐标。
void fillMask(const std::vector<std::vector<Contour>>& paths, const Matrix& M,
              std::uint8_t* mask, int W, int H) {
    struct Edge { float x0, y0, x1, y1; };
    std::vector<Edge> edges;
    float ymin = 1e9f, ymax = -1e9f;
    for (const auto& contours : paths) {
        for (const auto& ct : contours) {
            int n = static_cast<int>(ct.xs.size());
            if (n < 2) continue;
            std::vector<float> dx(n), dy(n);
            for (int k = 0; k < n; ++k) {
                float X, Y; M.apply(ct.xs[k], ct.ys[k], X, Y); dx[k] = X; dy[k] = Y;
            }
            for (int k = 0; k < n; ++k) {
                int m = (k + 1) % n;
                edges.push_back({dx[k], dy[k], dx[m], dy[m]});
                ymin = std::min(ymin, std::min(dy[k], dy[m]));
                ymax = std::max(ymax, std::max(dy[k], dy[m]));
            }
        }
    }
    if (edges.empty()) return;
    int y0 = std::max(0, static_cast<int>(std::floor(ymin)));
    int y1 = std::min(H - 1, static_cast<int>(std::ceil(ymax)));
    std::vector<float> xs;
    std::vector<float> cov(static_cast<std::size_t>(W), 0.0f);
    for (int y = y0; y <= y1; ++y) {
        std::fill(cov.begin(), cov.end(), 0.0f);
        // 与填充相同的 2x2 超采样，得到 0..1 覆盖率写入遮罩（裁剪边缘平滑）
        for (int sub = 0; sub < 2; ++sub) {
            float yy = y + (sub == 0 ? 0.25f : 0.75f); xs.clear();
            for (const auto& e : edges) {
                if ((e.y0 <= yy && e.y1 > yy) || (e.y1 <= yy && e.y0 > yy)) {
                    float t = (yy - e.y0) / (e.y1 - e.y0);
                    xs.push_back(e.x0 + t * (e.x1 - e.x0));
                }
            }
            if (xs.size() < 2) continue;
            std::sort(xs.begin(), xs.end());
            for (std::size_t k = 0; k + 1 < xs.size(); k += 2) {
                int xa = std::max(0, static_cast<int>(std::ceil(xs[k] - 0.5f)));
                int xb = std::min(W - 1, static_cast<int>(std::floor(xs[k + 1] - 0.5f)));
                for (int x = xa; x <= xb; ++x) cov[x] += 0.5f;
            }
        }
        for (int x = 0; x < W; ++x) {
            if (cov[x] > 0.0f) mask[y * W + x] = clampU8(static_cast<int>(cov[x] * 255.0f + 0.5f));
        }
    }
}

// 由起始/终止形状按 ratio(0..1) 插值出中间形状（样式索引对齐；逐点线性插值）。
static Shape interpolateMorph(const MorphShape& m, float t) {
    Shape s;
    t = (t < 0.0f) ? 0.0f : (t > 1.0f ? 1.0f : t);
    if (!m.start) { if (m.end) return *m.end; return s; }
    if (!m.end) return *m.start;
    // 终止段几何尚未解码时退化为起始形状：Flash 中 Morph 总是可见的，
    // 让中间帧整体消失比停在起始形状离标准更远。
    if (m.end->fillEdges.empty() && m.end->fillPaths.empty()) return *m.start;
    // SWF 规范保证 Morph 两段的边记录一一对应；若终止段解码不完整（边/轮廓/
    // 点数不一致），截断插值会产生自相交的退化多边形（门户页整块黑斑即由此
    // 而来），此时整体退化为起始形状，等价于旧行为且无回归。
    auto contoursMatch = [](const std::vector<std::vector<Contour>>& a,
                            const std::vector<std::vector<Contour>>& b) {
        if (a.size() != b.size()) return false;
        for (std::size_t fi = 0; fi < a.size(); ++fi) {
            if (a[fi].size() != b[fi].size()) return false;
            for (std::size_t ci = 0; ci < a[fi].size(); ++ci)
                if (a[fi][ci].xs.size() != b[fi][ci].xs.size() ||
                    a[fi][ci].ys.size() != b[fi][ci].ys.size()) return false;
        }
        return true;
    };
    bool edgesMatch = m.start->fillEdges.size() == m.end->fillEdges.size();
    for (std::size_t fi = 0; edgesMatch && fi < m.start->fillEdges.size(); ++fi)
        if (m.start->fillEdges[fi].size() != m.end->fillEdges[fi].size()) edgesMatch = false;
    if (!edgesMatch ||
        !contoursMatch(m.start->fillPaths, m.end->fillPaths) ||
        !contoursMatch(m.start->strokePaths, m.end->strokePaths))
        return *m.start;
    s.bounds = m.start->bounds;
    s.usesWinding = m.start->usesWinding;
    s.drawOrder = m.start->drawOrder;
    // MORPHFILLSTYLE / MORPHLINESTYLE 的样式本身也要插值：颜色、渐变矩阵、
    // 渐变色停、焦点与线宽。只插值轮廓而沿用起始样式会让中间帧退化成起始
    // 形状（进度条 Bar 就是因此被迫固定在末帧再手工缩放填充层）。
    auto mixColor = [&](const Color& a, const Color& b) {
        return Color(clampU8(static_cast<int>(lerp(a.r, b.r, t))),
                     clampU8(static_cast<int>(lerp(a.g, b.g, t))),
                     clampU8(static_cast<int>(lerp(a.b, b.b, t))),
                     clampU8(static_cast<int>(lerp(a.a, b.a, t))));
    };
    auto mixMatrix = [&](const Matrix& a, const Matrix& b) {
        Matrix r;
        r.sx = lerp(a.sx, b.sx, t); r.r0 = lerp(a.r0, b.r0, t);
        r.r1 = lerp(a.r1, b.r1, t); r.sy = lerp(a.sy, b.sy, t);
        r.tx = lerp(a.tx, b.tx, t); r.ty = lerp(a.ty, b.ty, t);
        return r;
    };
    s.fillStyles = m.start->fillStyles;
    for (std::size_t i = 0; i < s.fillStyles.size(); ++i) {
        if (i >= m.end->fillStyles.size()) break;
        FillStyle& fs = s.fillStyles[i];
        const FillStyle& fe = m.end->fillStyles[i];
        if (fs.kind != fe.kind) continue;
        if (fs.kind == FillKind::Solid) {
            fs.color = mixColor(fs.color, fe.color);
        } else if (fs.kind == FillKind::Bitmap) {
            fs.gradientMatrix = mixMatrix(fs.gradientMatrix, fe.gradientMatrix);
        } else {
            fs.gradientMatrix = mixMatrix(fs.gradientMatrix, fe.gradientMatrix);
            fs.focalX = lerp(fs.focalX, fe.focalX, t);
            fs.focalY = lerp(fs.focalY, fe.focalY, t);
            for (std::size_t k = 0; k < fs.gradient.size() && k < fe.gradient.size(); ++k) {
                fs.gradient[k].ratio = static_cast<std::uint8_t>(
                    clampU8(static_cast<int>(lerp(fs.gradient[k].ratio, fe.gradient[k].ratio, t))));
                fs.gradient[k].color = mixColor(fs.gradient[k].color, fe.gradient[k].color);
            }
        }
    }
    s.lineStyles = m.start->lineStyles;
    for (std::size_t i = 0; i < s.lineStyles.size() && i < m.end->lineStyles.size(); ++i) {
        s.lineStyles[i].width = lerp(s.lineStyles[i].width, m.end->lineStyles[i].width, t);
        s.lineStyles[i].color = mixColor(s.lineStyles[i].color, m.end->lineStyles[i].color);
    }
    // 有向边同步插值，否则 fillEdges 为空会退回轮廓启发式，丢失洞/并集的
    // 非零环绕语义（MORPHSHAPE 的边记录是逐边一一对应的）。
    s.fillEdges.resize(m.start->fillEdges.size());
    for (std::size_t fi = 0; fi < s.fillEdges.size(); ++fi) {
        const auto& a = m.start->fillEdges[fi];
        const auto& b = (fi < m.end->fillEdges.size()) ? m.end->fillEdges[fi] : a;
        s.fillEdges[fi].resize(std::min(a.size(), b.size()));
        for (std::size_t k = 0; k < s.fillEdges[fi].size(); ++k) {
            FillEdge e = a[k];
            e.x0 = lerp(a[k].x0, b[k].x0, t); e.y0 = lerp(a[k].y0, b[k].y0, t);
            e.x1 = lerp(a[k].x1, b[k].x1, t); e.y1 = lerp(a[k].y1, b[k].y1, t);
            s.fillEdges[fi][k] = e;
        }
    }
    s.fillPaths.resize(m.start->fillPaths.size());
    for (std::size_t fi = 0; fi < s.fillPaths.size(); ++fi) {
        const auto& a = m.start->fillPaths[fi];
        const auto& b = (fi < m.end->fillPaths.size()) ? m.end->fillPaths[fi] : a;
        s.fillPaths[fi].resize(std::min(a.size(), b.size()));
        for (std::size_t ci = 0; ci < s.fillPaths[fi].size(); ++ci) {
            const Contour& ca = a[ci]; const Contour& cb = b[ci];
            Contour c; std::size_t n = std::min(ca.xs.size(), cb.xs.size());
            c.xs.resize(n); c.ys.resize(n);
            for (std::size_t k = 0; k < n; ++k) { c.xs[k] = lerp(ca.xs[k], cb.xs[k], t); c.ys[k] = lerp(ca.ys[k], cb.ys[k], t); }
            s.fillPaths[fi][ci] = c;
        }
    }
    s.strokePaths.resize(m.start->strokePaths.size());
    for (std::size_t li = 0; li < s.strokePaths.size(); ++li) {
        const auto& a = m.start->strokePaths[li];
        const auto& b = (li < m.end->strokePaths.size()) ? m.end->strokePaths[li] : a;
        s.strokePaths[li].resize(std::min(a.size(), b.size()));
        for (std::size_t ci = 0; ci < s.strokePaths[li].size(); ++ci) {
            const Contour& ca = a[ci]; const Contour& cb = b[ci];
            Contour c; std::size_t n = std::min(ca.xs.size(), cb.xs.size());
            c.xs.resize(n); c.ys.resize(n);
            for (std::size_t k = 0; k < n; ++k) { c.xs[k] = lerp(ca.xs[k], cb.xs[k], t); c.ys[k] = lerp(ca.ys[k], cb.ys[k], t); }
            s.strokePaths[li][ci] = c;
        }
    }
    return s;
}

// UTF-8 解码：SWF 6+ 的文本（EditText 内容、AVM1 字符串）以 UTF-8 存储，中文占 3 字节。
// 文本排版必须按**码点**推进，而不是按字节——按字节会把一个汉字拆成 3 个非法字符，
// 设备字体回退时每个字节都会拿到一个 '?' 字形（结算页/商品说明的乱码来源）。
static std::vector<std::uint32_t> utf8Codes(const std::string& s) {
    std::vector<std::uint32_t> out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { out.push_back(c); ++i; continue; }
        int extra;
        std::uint32_t cp;
        if ((c & 0xE0) == 0xC0)      { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
        else { out.push_back(c); ++i; continue; }   // 非法首字节：按 Latin-1 保留
        ++i;
        bool ok = true;
        for (int k = 0; k < extra; ++k) {
            if (i >= s.size() || (static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
            ++i;
        }
        out.push_back(ok ? cp : c);
    }
    return out;
}

#ifdef _WIN32
// UTF-8 -> UTF-16（字体名同样以 UTF-8 存储，如"黑体"）。
static std::wstring utf8ToWide(const std::string& s) {
    std::wstring w;
    if (s.empty()) return w;
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    UINT cp = CP_UTF8;
    if (n <= 0) { cp = CP_ACP; n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0); }
    if (n <= 0) return w;
    w.resize(static_cast<std::size_t>(n));
    MultiByteToWideChar(cp, 0, s.c_str(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

// Flash 合成斜体的固定斜切系数 tan(12°)（规范常量，不是经验近似值）。
constexpr float kFlashSyntheticItalicShear = 0.207f;

// —— 设备字体（系统字体）回退 ——
// 嵌入字体（DefineFont2）只包含实际用到的那几个字形；Flash 遇到缺字形时会用
// **系统同名字体**（字体替换 / 设备字体）绘制该字符。我们用 GDI
// GetGlyphOutline(GGO_NATIVE) 取系统字体的 TrueType 二次样条轮廓，拍平成多边形，
// 再交给与嵌入字形完全相同的填充路径。坐标统一到 em = 1024（与嵌入字形一致）。
struct DeviceGlyph {
    std::vector<Contour> contours;
    float advanceEm = 0.0f;   // 前进宽度（em=1024）
    bool found = false;
};

// ---- M7：GDI 临时对象 RAII ----
// 只封装"临时创建"与"临时选入"的句柄：异常或提前返回时自动恢复 DC 状态并释放，
// 避免 GetGlyphOutline 轮廓解析路径（含 vector 分配）抛异常时 DC 停留在脏状态。
struct ScopedGdiObject {
    HGDIOBJ h = nullptr;
    explicit ScopedGdiObject(HGDIOBJ v) : h(v) {}
    ~ScopedGdiObject() { if (h) DeleteObject(h); }
    ScopedGdiObject(const ScopedGdiObject&) = delete;
    ScopedGdiObject& operator=(const ScopedGdiObject&) = delete;
};
struct ScopedGdiSelect {
    HDC dc = nullptr;
    HGDIOBJ old = nullptr;
    ScopedGdiSelect(HDC d, HGDIOBJ obj) : dc(d), old(obj ? SelectObject(d, obj) : nullptr) {}
    ~ScopedGdiSelect() { if (old) SelectObject(dc, old); }
    ScopedGdiSelect(const ScopedGdiSelect&) = delete;
    ScopedGdiSelect& operator=(const ScopedGdiSelect&) = delete;
};

// 把二次贝塞尔 (p0, c, p1) 拍平为折线点（不含起点）追加到 ct。
static void flattenQuadTo(Contour& ct, float x0, float y0, float cx, float cy,
                          float x1, float y1) {
    float len = std::fabs(cx - x0) + std::fabs(cy - y0) + std::fabs(x1 - cx) + std::fabs(y1 - cy);
    int steps = static_cast<int>(len / 16.0f) + 2;
    if (steps < 2) steps = 2;
    if (steps > 24) steps = 24;
    for (int i = 1; i <= steps; ++i) {
        float t = static_cast<float>(i) / steps, u = 1.0f - t;
        ct.xs.push_back(u * u * x0 + 2.0f * u * t * cx + t * t * x1);
        ct.ys.push_back(u * u * y0 + 2.0f * u * t * cy + t * t * y1);
    }
}

// 取得（并缓存）系统字体某个 Unicode 码点的轮廓。family 为空或不存在的字体返回 found=false。
// italic / bold 来自 SWF 的 FontFlags：粗体通过 GDI 的 FW_BOLD 选择/合成字重；
// 斜体不依赖系统是否存在对应斜体字面（SimHei 等无斜体），统一对轮廓做合成斜切，
// 这样设备字体与嵌入字形的倾斜方向/角度一致。
static const DeviceGlyph& deviceGlyph(const std::string& family, std::uint32_t code,
                                      bool italic = false, bool bold = false) {
    static DeviceGlyph none;
    if (family.empty()) return none;
    static std::map<std::string, DeviceGlyph> cache;
    char key[340];
    std::snprintf(key, sizeof key, "%s#%u#%d%d", family.c_str(), code, italic ? 1 : 0, bold ? 1 : 0);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;

    DeviceGlyph g;
    // M7：sdc / fonts 为进程级缓存，故意长期持有（由 OS 在进程退出时回收）；
    // 套 RAII 会引入静态析构顺序问题（缓存先于 DC 析构等），故不封装，仅封装临时句柄。
    static HDC sdc = CreateCompatibleDC(nullptr);
    static std::map<std::string, HFONT> fonts;
    // em（设计单位）是整数，过去被 reinterpret_cast 成 HFONT 塞进同一个 map；
    // 任何遍历 DeleteObject 的清理逻辑都会把它当句柄释放而崩溃。分开存放。
    static std::map<std::string, int> emCache;
    if (sdc) {
        HFONT hf = nullptr;
        const std::wstring wfam = utf8ToWide(family);
        const std::string styleKey = family + (bold ? "#b" : "#n");
        auto fit = fonts.find(styleKey);
        if (fit == fonts.end()) {
            // 先按任意高度建一次，读出该字体的 em（设计单位），再按 em 高度重建，
            // 这样 GGO_NATIVE 返回的坐标就是**设计单位**（scale = 1）。
            auto makeFont = [](const std::wstring& fam, int h, bool bold) {
                LOGFONTW lf = {};
                lf.lfHeight = -h;
                lf.lfWeight = bold ? FW_BOLD : FW_NORMAL;
                lf.lfCharSet = DEFAULT_CHARSET;
                lf.lfOutPrecision = OUT_TT_ONLY_PRECIS;   // GGO_NATIVE 需要 TrueType
                lf.lfQuality = ANTIALIASED_QUALITY;
                std::copy(fam.begin(), fam.begin() + std::min<std::size_t>(fam.size(), LF_FACESIZE - 1),
                          lf.lfFaceName);
                return CreateFontIndirectW(&lf);
            };
            // GetOutlineTextMetrics 不可用时按规范的 EM square 假设；正常情况下
            // 紧接的 OTM 查询会用字体真实设计单位覆盖它。
            int em = kFontEmSquare;
            HFONT tmp = makeFont(wfam, kFontEmSquare, bold);
            if (tmp) {
                ScopedGdiObject tmpGuard(tmp);   // 声明在前 → 析构在后（恢复选入后才删字体）
                ScopedGdiSelect sel(sdc, tmp);   // 析构在前 → 先恢复 DC 原选入对象
                OUTLINETEXTMETRICW otm = {};
                if (GetOutlineTextMetricsW(sdc, sizeof(otm), &otm) &&
                    otm.otmEMSquare > 0 && otm.otmEMSquare <= 16384)
                    em = static_cast<int>(otm.otmEMSquare);
            }
            hf = makeFont(wfam, em, bold);
            fonts[styleKey] = hf;
            emCache[styleKey] = em;
        } else {
            hf = fit->second;
        }
        int em = kFontEmSquare;   // 缓存缺失时的规范默认，正常路径由 emCache 给出真实设计单位
        auto eit = emCache.find(styleKey);
        if (eit != emCache.end()) em = eit->second;
        const float toEm = static_cast<float>(kFontEmSquare) / static_cast<float>(em);

        if (hf) {
            ScopedGdiSelect sel(sdc, hf);   // 自动恢复原选入对象（含向量分配异常路径）
            MAT2 mat = {};
            mat.eM11.value = 1; mat.eM22.value = 1;   // 单位矩阵（FIXED 16.16）
            GLYPHMETRICS gm = {};
            DWORD need = GetGlyphOutlineW(sdc, static_cast<UINT>(code), GGO_NATIVE, &gm, 0, nullptr, &mat);
            if (need != GDI_ERROR && need > 0) {
                std::vector<std::uint8_t> buf(need);
                if (GetGlyphOutlineW(sdc, static_cast<UINT>(code), GGO_NATIVE, &gm, need,
                                     buf.data(), &mat) != GDI_ERROR) {
                    auto fx = [toEm](const FIXED& f) {
                        return (static_cast<float>(f.value) + f.fract / 65536.0f) * toEm;
                    };
                    std::size_t p = 0;
                    while (p + sizeof(TTPOLYGONHEADER) <= buf.size()) {
                        const TTPOLYGONHEADER* ph =
                            reinterpret_cast<const TTPOLYGONHEADER*>(buf.data() + p);
                        if (ph->cb == 0) break;
                        Contour ct;
                        float px = fx(ph->pfxStart.x), py = -fx(ph->pfxStart.y);  // 系统字体 y 向上 -> 取反
                        ct.xs.push_back(px); ct.ys.push_back(py);
                        const std::uint8_t* q = buf.data() + p + sizeof(TTPOLYGONHEADER);
                        const std::uint8_t* end = buf.data() + p + ph->cb;
                        while (q + offsetof(TTPOLYCURVE, apfx) <= end) {
                            const TTPOLYCURVE* cu = reinterpret_cast<const TTPOLYCURVE*>(q);
                            if (cu->wType == TT_PRIM_LINE) {
                                for (int i = 0; i < cu->cpfx; ++i) {
                                    float x = fx(cu->apfx[i].x), y = -fx(cu->apfx[i].y);
                                    ct.xs.push_back(x); ct.ys.push_back(y); px = x; py = y;
                                }
                            } else if (cu->wType == TT_PRIM_QSPLINE) {
                                for (int i = 0; i + 1 < cu->cpfx; ++i) {
                                    float cx = fx(cu->apfx[i].x), cy = -fx(cu->apfx[i].y);
                                    float ex, ey;
                                    if (i + 1 < cu->cpfx - 1) {
                                        ex = 0.5f * (fx(cu->apfx[i].x) + fx(cu->apfx[i + 1].x));
                                        ey = -0.5f * (fx(cu->apfx[i].y) + fx(cu->apfx[i + 1].y));
                                    } else {
                                        ex = fx(cu->apfx[i + 1].x); ey = -fx(cu->apfx[i + 1].y);
                                    }
                                    flattenQuadTo(ct, px, py, cx, cy, ex, ey);
                                    px = ex; py = ey;
                                }
                            }
                            q += offsetof(TTPOLYCURVE, apfx) +
                                 static_cast<std::size_t>(cu->cpfx) * sizeof(POINTFX);
                        }
                        if (ct.xs.size() >= 3) g.contours.push_back(std::move(ct));
                        p += ph->cb;
                    }
                    g.advanceEm = static_cast<float>(gm.gmCellIncX) * toEm;
                    if (g.advanceEm <= 0.0f) {
                        float mx = 0;
                        for (const auto& ct : g.contours) for (float x : ct.xs) mx = std::max(mx, x);
                        g.advanceEm = mx;
                    }
                    g.found = !g.contours.empty();
                }
            }
        }
    }
    // 合成斜体：轮廓坐标为 em=1024、y 已向下取反（顶部为负 y），
    // 斜体要求顶部右移，故 x' = x - k*y（k = tan(12°)，与 Flash 的合成斜体角度一致）。
    if (italic) {
        for (auto& ct : g.contours)
            for (std::size_t i = 0; i < ct.xs.size(); ++i)
                ct.xs[i] -= kFlashSyntheticItalicShear * ct.ys[i];
    }
    it = cache.emplace(key, std::move(g)).first;
    return it->second;
}
#endif  // _WIN32

// ---------- 文本排版度量 ----------
// 系统（设备）字体的真实垂直度量，归一到字体的 EM square 设计单位。
// 仅当 SWF 的 FontFlagsHasLayout 未提供度量时，作为「字体自身度量」使用。
struct DeviceFontMetrics {
    float ascent = 0.0f, descent = 0.0f, lineGap = 0.0f;
    bool ok = false;
};

static const DeviceFontMetrics& deviceFontMetrics(const std::string& family, bool bold) {
    static std::map<std::string, DeviceFontMetrics> cache;
    std::string key = family + (bold ? "#b" : "#n");
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    DeviceFontMetrics m;
#ifdef _WIN32
    static HDC dc = CreateCompatibleDC(nullptr);
    if (dc) {
        const std::wstring wfam = utf8ToWide(family.empty() ? "Arial" : family);
        LOGFONTW lf = {};
        lf.lfHeight = -kFontEmSquare;     // 令逻辑单位高 = EM square，度量直接以设计单位表示
        lf.lfWeight = bold ? FW_BOLD : FW_NORMAL;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfOutPrecision = OUT_TT_ONLY_PRECIS;
        std::copy(wfam.begin(),
                  wfam.begin() + std::min<std::size_t>(wfam.size(), LF_FACESIZE - 1),
                  lf.lfFaceName);
        HFONT hf = CreateFontIndirectW(&lf);
        if (hf) {
            ScopedGdiObject keep(hf);     // 后析构：恢复选入后删除字体
            ScopedGdiSelect sel(dc, hf);  // 先析构：恢复 DC
            TEXTMETRICW tm = {};
            if (GetTextMetricsW(dc, &tm) && tm.tmHeight > 0) {
                const float k = static_cast<float>(kFontEmSquare) /
                                static_cast<float>(tm.tmHeight);
                m.ascent  = static_cast<float>(tm.tmAscent) * k;
                m.descent = static_cast<float>(tm.tmDescent) * k;
                m.lineGap = static_cast<float>(tm.tmExternalLeading) * k;
                m.ok = true;
            }
        }
    }
#endif
    return cache.emplace(std::move(key), m).first->second;
}

struct GlyphBox {
    float xMin = 0, xMax = 0, yMin = 0, yMax = 0;
    bool empty = true;
};

static GlyphBox glyphBox(const Shape& sh) {
    GlyphBox b;
    for (const auto& ct : sh.fillPaths)
        for (const auto& c : ct)
            for (std::size_t i = 0; i < c.xs.size(); ++i) {
                const float x = c.xs[i], y = c.ys[i];
                if (b.empty) { b.xMin = b.xMax = x; b.yMin = b.yMax = y; b.empty = false; }
                else {
                    b.xMin = std::min(b.xMin, x); b.xMax = std::max(b.xMax, x);
                    b.yMin = std::min(b.yMin, y); b.yMax = std::max(b.yMax, y);
                }
            }
    return b;
}

// 排版度量（单位 twips）：优先 SWF 的 HasLayout 表；没有该表时按字体自身数据推导——
// 升降部取所有嵌入字形的包围盒，行距取系统同名字体的真实 ExternalLeading。
struct TextMetrics {
    float scale = 1.0f;         // 嵌入字形坐标单位 -> twips
    float deviceScale = 1.0f;   // 设备字体轮廓（恒为 EM square 单位）-> twips
    float ascent = 0, descent = 0, leading = 0, lineH = 0;
};

static TextMetrics textMetrics(const FontGlyphs& fg, float fontHeight) {
    TextMetrics m;
    m.scale = fg.glyphsInTwips ? 1.0f
            : fontHeight / static_cast<float>(fg.unitsPerEm > 0 ? fg.unitsPerEm : kFontEmSquare);
    m.deviceScale = fontHeight / static_cast<float>(kFontEmSquare);
    const DeviceFontMetrics& dm = deviceFontMetrics(fg.name, fg.bold);
    if (fg.hasLayout) {
        m.ascent  = static_cast<float>(fg.ascent)  * m.scale;
        m.descent = static_cast<float>(fg.descent) * m.scale;
        m.leading = static_cast<float>(fg.leading) * m.scale;
    } else {
        float yMin = 0.0f, yMax = 0.0f;
        bool any = false;
        for (const auto& sh : fg.shapes) {
            if (!sh) continue;
            const GlyphBox b = glyphBox(*sh);
            if (b.empty) continue;
            yMin = std::min(yMin, b.yMin);
            yMax = std::max(yMax, b.yMax);
            any = true;
        }
        if (any) { m.ascent = -yMin * m.scale; m.descent = yMax * m.scale; }
        else if (dm.ok) {
            m.ascent  = dm.ascent  * m.scale;
            m.descent = dm.descent * m.scale;
        }
        m.leading = dm.ok ? dm.lineGap * m.scale : 0.0f;
    }
    m.lineH = m.ascent + m.descent + m.leading;
    if (m.lineH <= 0.0f) m.lineH = fontHeight;   // 完全无度量时以字号自身为行高
    return m;
}

// 前进宽度（twips）：SWF FontAdvanceTable -> 字形边界框 -> 设备字体 gmCellIncX。
static float glyphAdvanceTwips(const FontGlyphs& fg, int gi, const Shape* gs,
                               float deviceAdvEm, const TextMetrics& tm) {
    if (fg.hasLayout && gi >= 0 && gi < static_cast<int>(fg.advances.size()) &&
        fg.advances[static_cast<std::size_t>(gi)] != 0)
        return static_cast<float>(fg.advances[static_cast<std::size_t>(gi)]) * tm.scale;
    if (gs) {
        const GlyphBox b = glyphBox(*gs);
        if (!b.empty) return b.xMax * tm.scale;   // 无布局表时 Flash 以字形边界框推导
    }
    return deviceAdvEm * tm.deviceScale;          // 空格 / 缺字形：系统字体真实前进宽度
}

static float kerningUnits(const FontGlyphs& fg, std::uint32_t prev, std::uint32_t cur) {
    if (!fg.hasLayout || !prev) return 0.0f;
    const std::uint32_t key = ((prev & 0xFFFFu) << 16) | (cur & 0xFFFFu);
    auto it = fg.kerning.find(key);
    return it == fg.kerning.end() ? 0.0f : static_cast<float>(it->second);
}

// 渲染时间轴文本框（DefineEditText）：字体/颜色/边界/对齐/边距/缩进/行距
// 全部取自 SWF 字段，字形缩放按字体的 EM square，度量由上面的算法给出。
static void renderEditText(const EditText& et, const Matrix& world,
                           const std::map<std::uint16_t, std::shared_ptr<Character>>& dict,
                           std::uint8_t* buf, int W, int H, float alpha) {
    // FontID 是 SWF 明确指定的字体角色；找不到就不排版（不再随便挑字典里某个字体）。
    auto fit = dict.find(et.fontId);
    if (fit == dict.end() || !fit->second || !fit->second->font ||
        et.text.empty() || et.fontHeight <= 0) return;
    const FontGlyphs& fg = *fit->second->font;

    const TextMetrics tm = textMetrics(fg, static_cast<float>(et.fontHeight));
    const float lineH  = tm.lineH + static_cast<float>(et.leading);
    // Flash TextField 的文本区域相对 Bounds 内缩 2px（40 twips）；右对齐时缺失
    // 这个内边距会让动态数字比同排的静态 DefineText（如 "%"）右移 2px、贴得过近。
    const float kPad   = 40.0f;
    const float left   = static_cast<float>(et.bounds.x0) + kPad
                         + static_cast<float>(et.leftMargin);
    const float availW = static_cast<float>(et.bounds.x1 - et.bounds.x0)
                         - 2.0f * kPad - static_cast<float>(et.leftMargin)
                         - static_cast<float>(et.rightMargin);
    const bool wrap      = et.wordWrap && availW > 0.0f;
    const bool multiline = et.multiline || et.wordWrap;
    Paint p; p.kind = FillKind::Solid; p.solid = et.color;

    struct Cell {
        std::uint32_t code = 0;
        const Shape* shape = nullptr;
        float adv = 0.0f;        // twips
        bool space = false;
    };
    std::vector<Cell> cells;
    std::uint32_t prev = 0;
    for (std::uint32_t ch : utf8Codes(et.text)) {
        if (ch == '\r') continue;
        if (ch == '\n') { if (multiline) cells.push_back({ch, nullptr, 0.0f, false}); prev = 0; continue; }
        const std::uint32_t draw = et.password ? static_cast<std::uint32_t>('*') : ch;
        int gi = -1;
        const Shape* gs = nullptr;
        auto it = fg.codeToGlyph.find(static_cast<int>(draw));
        if (it != fg.codeToGlyph.end() && it->second >= 0 &&
            it->second < static_cast<int>(fg.shapes.size())) {
            gi = it->second;
            gs = fg.shapes[static_cast<std::size_t>(gi)].get();
        }
        float deviceAdv = 0.0f;
#ifdef _WIN32
        if (!gs) deviceAdv = deviceGlyph(fg.name, draw, fg.italic, fg.bold).advanceEm;
#endif
        const float adv = glyphAdvanceTwips(fg, gi, gs, deviceAdv, tm) +
                          kerningUnits(fg, prev, draw) * tm.scale;
        cells.push_back({ch, gs, adv, ch == ' ' || ch == '\t'});
        prev = draw;
    }

    // 分行：显式换行 + WordWrap 按词贪心折行（不把词从中间断开）。
    std::vector<std::vector<Cell>> lines;
    std::vector<Cell> cur;
    float curW = 0.0f;
    auto flush = [&]() { lines.push_back(std::move(cur)); cur.clear(); curW = 0.0f; };
    for (const auto& c : cells) {
        if (c.code == '\n') { flush(); continue; }
        if (wrap && !cur.empty() && curW + c.adv > availW) {
            std::size_t brk = cur.size();
            while (brk > 0 && !cur[brk - 1].space) --brk;
            if (brk > 0 && brk < cur.size()) {
                std::vector<Cell> tail(cur.begin() + static_cast<std::ptrdiff_t>(brk), cur.end());
                cur.resize(brk);
                flush();
                for (const auto& t : tail) { cur.push_back(t); curW += t.adv; }
            } else flush();
        }
        cur.push_back(c);
        curW += c.adv;
    }
    if (!cur.empty() || lines.empty()) flush();

    const std::uint8_t align = et.align > 3 ? 0 : et.align;

    for (std::size_t li = 0; li < lines.size(); ++li) {
        auto& line = lines[li];
        while (!line.empty() && line.back().space) line.pop_back();   // 行尾空白不参与对齐
        float width = 0.0f;
        int spaces = 0;
        for (const auto& c : line) { width += c.adv; if (c.space) ++spaces; }
        float extra = 0.0f;
        float x = left + (li == 0 ? static_cast<float>(et.indent) : 0.0f);
        if (align == 1) x += availW - width;                         // Right
        else if (align == 2) x += (availW - width) * 0.5f;            // Center
        else if (align == 3 && li + 1 < lines.size() && spaces > 0)   // Justify
            extra = (availW - width) / static_cast<float>(spaces);
        // 首行基线按 SWF 的 FontHeight 落位，后续行以 lineH 递增。用字体 ascent
        // 会把首行抬高（本例 fontHeight=200 / ascent=148，约 2.6px），与同排静态
        // DefineText 的百分号基线错开。
        const float y = static_cast<float>(et.bounds.y0) + kPad
                        + static_cast<float>(li) * lineH
                        + static_cast<float>(et.fontHeight);
        for (const auto& c : line) {
            if (c.shape) {
                Matrix st; st.sx = tm.scale; st.sy = tm.scale; st.tx = x; st.ty = y;
                if (fg.italic) st.r0 = -kFlashSyntheticItalicShear * tm.scale;
                Matrix gm = world.post(st);
                Matrix Minv; gm.invert(Minv);
                for (std::size_t i = 0; i < c.shape->fillStyles.size(); ++i)
                    fillStyleOf(*c.shape, i, gm, Minv, p, buf, W, H, alpha, BlendMode::Normal);
#ifdef _WIN32
            } else if (!c.space) {
                const auto& dg = deviceGlyph(fg.name, c.code, fg.italic, fg.bold);
                if (dg.found) {
                    Matrix st; st.sx = tm.deviceScale; st.sy = tm.deviceScale;
                    st.tx = x; st.ty = y;
                    Matrix gm = world.post(st);
                    Matrix Minv; gm.invert(Minv);
                    fillContours(dg.contours, gm, Minv, p, buf, W, H, alpha, BlendMode::Normal);
                }
#endif
            }
            x += c.adv + (c.space ? extra : 0.0f);
        }
    }
}

// 渲染一组显示列表项并处理 clipDepth 裁剪组（主时间轴与精灵内部时间轴通用）。
// inheritedCx：祖先累积的颜色变换（Flash 中 ColorTransform 作用于整个子树），
// 逐层与每个对象自身 cxform 组合。
void renderItemsWithClip(const std::vector<DisplayItem>& items, const Matrix& root,
                         const std::map<std::uint16_t, std::shared_ptr<Character>>& dict,
                         std::uint8_t* buf, int W, int H, int frameIndex,
                         std::size_t npix, float alpha,
                         const ColorTransform& inheritedCx = ColorTransform{});

// 把单个显示列表项（shape / bitmap / sprite / morph / text / edittext / button）渲染进 buf。
// alpha：累积的对象级透明度；bm：混合模式（运行时 DisplayObject.blend / PlaceObject3 blend）。
// 运行时覆盖（AVM1 SetProperty / EditText 输入）：item.hasRt 时以 rt* 字段合成矩阵/透明度。
void renderItemInto(const DisplayItem& item, const Matrix& root,
                    const std::map<std::uint16_t, std::shared_ptr<Character>>& dict,
                    std::uint8_t* buf, int W, int H, int frameIndex, float alpha = 1.0f,
                    BlendMode bm = BlendMode::Normal,
                    const ColorTransform& inheritedCx = ColorTransform{}) {
    if (item.hasRt && !item.rtVisible) return;      // AVM1 _visible = false
    // 世界矩阵：静态 matrix 或运行时覆盖合成（T(x,y) * R(rot) * S(sx,sy)，单位像素->twips）
    Matrix world;
    if (item.hasRt) {
        double rad = item.rtRotation * 3.141592653589793 / 180.0;
        double ca = std::cos(rad), sa = std::sin(rad);
        Matrix rt;
        rt.sx  = static_cast<float>(item.rtScaleX * ca);
        rt.r0  = static_cast<float>(-item.rtScaleX * sa);
        rt.r1  = static_cast<float>(item.rtScaleY * sa);
        rt.sy  = static_cast<float>(item.rtScaleY * ca);
        rt.tx  = static_cast<float>(item.rtX * 20.0);
        rt.ty  = static_cast<float>(item.rtY * 20.0);
        world = root.post(rt);
        alpha *= static_cast<float>(item.rtAlpha);
    } else {
        world = root.post(item.matrix);
    }
    if (!item.character) return;
    // Flash 语义：对象的 ColorTransform 作用于自身及其整个子树。
    // 本项最终变换 = 祖先累积变换 ∘ 自身变换；子项渲染时以本变换为祖先继续下传。
    const ColorTransform cx = inheritedCx.post(item.cxform);



    if (item.character->kind == Character::Kind::Shape && item.character->shape) {
        renderShape(*item.character->shape, world, cx, dict, buf, W, H, alpha, bm);
    } else if (item.character->kind == Character::Kind::Morph && item.character->morph) {
        // 变形形状：按当前 ratio 插值后渲染
        Shape interp = interpolateMorph(*item.character->morph, item.ratio);
        renderShape(interp, world, cx, dict, buf, W, H, alpha, bm);
    } else if (item.character->kind == Character::Kind::Button && item.character->button) {
        // 按钮：按交互态（0=Up 1=Over 2=Down 3=Hit）选桶；Hit 态不渲染（仅命中区域）
        const auto& bd = *item.character->button;
        const std::vector<DisplayItem>* bucket = &bd.up;
        switch (item.buttonState) {
            case 1: if (!bd.over.empty()) bucket = &bd.over; break;
            case 2: if (!bd.down.empty()) bucket = &bd.down; break;
            case 3: return;                            // Hit 态不绘制
            default: break;
        }
        // 按钮各状态桶的记录在文件中**不保证**按 depth 升序（实测退出本关按钮的 Over 桶
        // 顺序为 1,3,2），若直接按定义顺序绘制，depth 小的图形会覆盖 depth 大的文字，
        // 导致悬停时按钮文字消失。此处与时间轴一致，按 depth 升序渲染。
        std::vector<const DisplayItem*> order;
        order.reserve(bucket->size());
        for (const auto& sub : *bucket) order.push_back(&sub);
        std::stable_sort(order.begin(), order.end(),
                         [](const DisplayItem* a, const DisplayItem* b) { return a->depth < b->depth; });
        for (const auto* sub : order) renderItemInto(*sub, world, dict, buf, W, H, frameIndex, alpha, bm, cx);
    } else if (item.character->kind == Character::Kind::Sprite) {
        // 精灵：递归渲染其运行态内部帧子树（children 由 Player 每帧同步；支持嵌套精灵、
        // 嵌套按钮与各自的独立播放头/按钮态）。
        const auto& sf = item.character->spriteFrames;
        std::size_t npix = static_cast<std::size_t>(W) * H;
        if (!item.children.empty()) {
            // 用带 clipDepth 处理的渲染器渲染精灵内部帧（支持精灵内遮罩）。
            renderItemsWithClip(item.children, world, dict, buf, W, H, frameIndex, npix, alpha, cx);
            return;
        }
        // 回退（children 尚未同步，例如按钮内部的静态子项）：按放置帧对齐即时取帧。
        if (sf.empty()) return;
        int idx;
        if (item.rtFrame >= 0) {
            idx = item.rtFrame % static_cast<int>(sf.size());
        } else if (!item.spInit) {
            idx = 0;   // 尚未初始化的新精灵：按首帧渲染（下一 tick 由 initTree 接管）
        } else if (item.spStopped) {
            idx = 0;   // 首帧脚本 stop()：停在内部第 0 帧
        } else {
            idx = frameIndex - item.birthFrame;
            if (idx < 0) idx = 0;
            idx %= static_cast<int>(sf.size());
        }
        renderItemsWithClip(sf[static_cast<std::size_t>(idx)].items, world, dict, buf, W, H, idx, npix, alpha, cx);
    } else if (item.character->kind == Character::Kind::Text && item.character->text) {
        // 文本：逐字形按文本矩阵+笔位放置，用字形颜色实心填充字形轮廓
        const TextDef& td = *item.character->text;
        Matrix tm = world.post(td.textMatrix);
        for (const auto& tg : td.glyphs) {
            auto fit = dict.find(tg.fontId);
            if (fit == dict.end() || !fit->second || !fit->second->font) continue;
            const FontGlyphs& fg = *fit->second->font;
            if (tg.glyphIndex >= fg.shapes.size()) continue;
            const auto& gs = fg.shapes[tg.glyphIndex];
            if (!gs) continue;
            float gx, gy; tm.apply(static_cast<float>(tg.x), static_cast<float>(tg.y), gx, gy);

            // 字形坐标单位由字体定义：DefineFont(10) 是 twips（无需缩放），
            // DefineFont2/3 是 EM square（字号 twips / unitsPerEm）。TEXTRECORD 的
            // FontHeight 是本 run 的字号；缺失时没有可依据的缩放，跳过该字形。
            const float gs_ = fg.glyphsInTwips ? 1.0f
                : static_cast<float>(tg.fontHeight) /
                  static_cast<float>(fg.unitsPerEm > 0 ? fg.unitsPerEm : kFontEmSquare);
            if (gs_ <= 0.0f) continue;
            // 字形缩放作用于**整个线性部分**（含旋转/斜切分量 r0/r1），平移则取笔位经
            // 文本矩阵映射后的设备坐标，等价于 tm ∘ T(x,y) ∘ Scale(gs_)。
            Matrix gm = tm; gm.sx *= gs_; gm.sy *= gs_; gm.r0 *= gs_; gm.r1 *= gs_;
            // 字体带 Italic 标志：在字形空间叠加合成斜切（x' = x - k*y，再经文本矩阵）。
            if (fg.italic) gm.r0 = (tm.r0 - kFlashSyntheticItalicShear * tm.sx) * gs_;
            gm.tx = gx; gm.ty = gy;
            Matrix Minv; gm.invert(Minv);
            Paint p; p.kind = FillKind::Solid; p.solid = tg.color; p.cx = cx;
            for (std::size_t i = 0; i < gs->fillStyles.size(); ++i) {
                fillStyleOf(*gs, i, gm, Minv, p, buf, W, H, alpha, bm);
            }
        }
    } else if (item.character->kind == Character::Kind::EditText && item.character->editText) {
        // 文本框：运行时输入覆盖 InitialText
        if (item.hasTextOverride) {
            EditText et = *item.character->editText;   // 拷贝并替换文本
            et.text = item.textOverride;
            renderEditText(et, world, dict, buf, W, H, alpha);
        } else {
            renderEditText(*item.character->editText, world, dict, buf, W, H, alpha);
        }
    } else if (item.character->kind == Character::Kind::Bitmap && item.character->bitmap) {
        const Bitmap& bmp = *item.character->bitmap;
        for (int j = 0; j < bmp.height; ++j) {
            for (int i = 0; i < bmp.width; ++i) {
                float lx = static_cast<float>(i * 20), ly = static_cast<float>(j * 20);
                float dx, dy; world.apply(lx, ly, dx, dy);
                int px = static_cast<int>(std::round(dx)), py = static_cast<int>(std::round(dy));
                const std::uint8_t* p4 = &bmp.rgba[static_cast<std::size_t>(j) * bmp.width * 4 + i * 4];
                Color col = cx.apply(Color(p4[0], p4[1], p4[2], p4[3]));
                col.a = clampU8(static_cast<int>(col.a * alpha + 0.5f));   // 对象级透明度
                plot(buf, W, H, px, py, col, bm);
            }
        }
    } else if (item.character->kind == Character::Kind::Video && item.character->video) {
        // 视频流：按当前时间轴帧选择已解码帧（frameNum <= frameIndex+1 的最大者；无则首帧）
        const auto& vs = *item.character->video;
        std::shared_ptr<Bitmap> frame;
        auto pit = vs.decoded.upper_bound(static_cast<std::uint16_t>(frameIndex + 1));
        if (pit != vs.decoded.begin()) { --pit; frame = pit->second; }
        else if (!vs.decoded.empty()) frame = vs.decoded.begin()->second;
        if (frame) {
            for (int j = 0; j < frame->height; ++j) {
                for (int i = 0; i < frame->width; ++i) {
                    float lx = static_cast<float>(i * 20), ly = static_cast<float>(j * 20);
                    float dx, dy; world.apply(lx, ly, dx, dy);
                    int px = static_cast<int>(std::round(dx)), py = static_cast<int>(std::round(dy));
                    const std::uint8_t* p4 = &frame->rgba[static_cast<std::size_t>(j) * frame->width * 4 + i * 4];
                    Color col = cx.apply(Color(p4[0], p4[1], p4[2], p4[3]));
                    col.a = clampU8(static_cast<int>(col.a * alpha + 0.5f));
                    plot(buf, W, H, px, py, col, bm);
                }
            }
        }
    }
}

// ---- 位图滤镜（PlaceObject3 FilterList）----
// over 合成：src 覆盖到 dst
static void compositeOver(const std::uint8_t* src, std::uint8_t* dst, std::size_t npix) {
    for (std::size_t p = 0; p < npix; ++p) {
        const std::uint8_t* s = &src[p * 4]; std::uint8_t* d = &dst[p * 4];
        float sa = s[3] / 255.0f; if (sa <= 0.0f) continue;
        float da = d[3] / 255.0f;
        float outA = sa + da * (1.0f - sa); if (outA <= 0.0f) continue;
        d[0] = static_cast<std::uint8_t>((s[0] * sa + d[0] * da * (1.0f - sa)) / outA);
        d[1] = static_cast<std::uint8_t>((s[1] * sa + d[1] * da * (1.0f - sa)) / outA);
        d[2] = static_cast<std::uint8_t>((s[2] * sa + d[2] * da * (1.0f - sa)) / outA);
        d[3] = static_cast<std::uint8_t>(outA * 255.0f);
    }
}
// 可分离盒式模糊（RGBA）。
// 用前缀和滑窗代替逐像素内层循环：每个方向 O(W*H)，与原实现逐点取
// [x-r, x+r] 窗口均值（边界按有效样本数取平均）结果完全一致。
static void blurBuffer(std::uint8_t* buf, int W, int H, int rx, int ry, int passes) {
    if (rx < 1 && ry < 1) return;
    std::vector<int> ps(static_cast<std::size_t>(std::max(W, H) + 1) * 4);
    for (int p = 0; p < passes; ++p) {
        if (rx >= 1) {
            for (int y = 0; y < H; ++y) {
                const std::uint8_t* row = &buf[static_cast<std::size_t>(y) * W * 4];
                for (int c = 0; c < 4; ++c) ps[c] = 0;
                for (int x = 0; x < W; ++x) {
                    for (int c = 0; c < 4; ++c)
                        ps[static_cast<std::size_t>(x + 1) * 4 + c] =
                            ps[static_cast<std::size_t>(x) * 4 + c] + row[x * 4 + c];
                }
                // 前缀和已整行就绪，可直接写回 buf（逐像素只依赖该行前缀）
                std::uint8_t* out = &buf[static_cast<std::size_t>(y) * W * 4];
                for (int x = 0; x < W; ++x) {
                    int lo = std::max(0, x - rx), hi = std::min(W - 1, x + rx);
                    int n = hi - lo + 1;
                    for (int c = 0; c < 4; ++c)
                        out[x * 4 + c] = static_cast<std::uint8_t>(
                            (ps[static_cast<std::size_t>(hi + 1) * 4 + c] -
                             ps[static_cast<std::size_t>(lo) * 4 + c]) / n);
                }
            }
        }
        if (ry >= 1) {
            // 竖直方向按 16 列一块处理：若逐列整幅扫描，每次只取 4 字节却要跨
            // W*4 字节的步长，缓存几乎全部失效（实测占模糊耗时的大头）。
            // 分块后同一行 16 个像素处在同一两条缓存行内，块内前缀和也能留在 L2。
            constexpr int kTile = 16;
            std::vector<int> psTile(static_cast<std::size_t>(H) * kTile * 4);
            for (int x0 = 0; x0 < W; x0 += kTile) {
                const int tw = std::min(kTile, W - x0);
                const int rowInts = tw * 4;
                for (int y = 0; y < H; ++y) {
                    const std::uint8_t* row = &buf[(static_cast<std::size_t>(y) * W + x0) * 4];
                    int* cur = &psTile[static_cast<std::size_t>(y) * rowInts];
                    const int* prev = y ? &psTile[static_cast<std::size_t>(y - 1) * rowInts] : nullptr;
                    if (prev) {
                        for (int c = 0; c < rowInts; ++c) cur[c] = prev[c] + row[c];
                    } else {
                        for (int c = 0; c < rowInts; ++c) cur[c] = row[c];
                    }
                }
                for (int y = 0; y < H; ++y) {
                    int lo = std::max(0, y - ry), hi = std::min(H - 1, y + ry);
                    int n = hi - lo + 1;
                    std::uint8_t* d = &buf[(static_cast<std::size_t>(y) * W + x0) * 4];
                    const int* pHi = &psTile[static_cast<std::size_t>(hi) * rowInts];
                    const int* pLo = lo ? &psTile[static_cast<std::size_t>(lo - 1) * rowInts] : nullptr;
                    if (pLo) {
                        for (int c = 0; c < rowInts; ++c)
                            d[c] = static_cast<std::uint8_t>((pHi[c] - pLo[c]) / n);
                    } else {
                        for (int c = 0; c < rowInts; ++c)
                            d[c] = static_cast<std::uint8_t>(pHi[c] / n);
                    }
                }
            }
        }
    }
}
// 由 layer 的 alpha 生成投影/辉光（偏移 offx/offy，着色为 c，alpha 乘 strength）
static void buildShadow(const std::uint8_t* layer, std::uint8_t* out, int W, int H,
                        const Color& c, int offx, int offy, float strength) {
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        const std::uint8_t* s = &layer[(static_cast<std::size_t>(y) * W + x) * 4];
        if (s[3] == 0) continue;
        int dx = x + offx, dy = y + offy;
        if (dx < 0 || dx >= W || dy < 0 || dy >= H) continue;
        std::uint8_t* d = &out[(static_cast<std::size_t>(dy) * W + dx) * 4];
        std::uint8_t na = clampU8(static_cast<int>(s[3] * strength));
        if (na > d[3]) { d[0] = c.r; d[1] = c.g; d[2] = c.b; d[3] = na; }
    }
}
// 颜色矩阵滤镜（4x5）：对 layer 每像素应用矩阵（偏移项以 0..255 计），原位修改。
static void applyColorMatrix(std::vector<std::uint8_t>& layer, const float* m) {
    std::size_t n = layer.size() / 4;
    for (std::size_t p = 0; p < n; ++p) {
        std::uint8_t* d = &layer[p * 4];
        float r = d[0], g = d[1], b = d[2], a = d[3];
        float nr = m[0]*r + m[1]*g + m[2]*b + m[3]*a + m[4]*255.0f;
        float ng = m[5]*r + m[6]*g + m[7]*b + m[8]*a + m[9]*255.0f;
        float nb = m[10]*r+ m[11]*g+ m[12]*b+ m[13]*a+ m[14]*255.0f;
        float na = m[15]*r+ m[16]*g+ m[17]*b+ m[18]*a+ m[19]*255.0f;
        d[0] = clampU8(static_cast<int>(nr)); d[1] = clampU8(static_cast<int>(ng));
        d[2] = clampU8(static_cast<int>(nb)); d[3] = clampU8(static_cast<int>(na));
    }
}
// 卷积滤镜：对 layer 应用 mx×my 核（按除数缩放 + bias），越界按 clamp 或默认色处理。
static void applyConvolution(std::vector<std::uint8_t>& layer, int W, int H, const Filter& f) {
    std::vector<std::uint8_t> src = layer;     // 快照，避免就地累加污染
    int mx = f.convW > 0 ? f.convW : 1, my = f.convH > 0 ? f.convH : 1;
    float div = (f.convDivisor != 0.0f) ? f.convDivisor : 1.0f;
    int cx = mx / 2, cy = my / 2;
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        float sr = 0, sg = 0, sb = 0, sa = 0;
        for (int ky = 0; ky < my; ++ky) for (int kx = 0; kx < mx; ++kx) {
            int sx = x + kx - cx, sy = y + ky - cy;
            float w = f.convMatrix[static_cast<std::size_t>(ky) * mx + kx];
            if (sx < 0 || sx >= W || sy < 0 || sy >= H) {
                if (f.convClamp) { sx = std::max(0, std::min(W - 1, sx)); sy = std::max(0, std::min(H - 1, sy)); }
                else { sr += f.convColor.r * w; sg += f.convColor.g * w; sb += f.convColor.b * w; sa += f.convColor.a * w; continue; }
            }
            const std::uint8_t* q = &src[(static_cast<std::size_t>(sy) * W + sx) * 4];
            sr += q[0] * w; sg += q[1] * w; sb += q[2] * w; sa += q[3] * w;
        }
        std::uint8_t* d = &layer[(static_cast<std::size_t>(y) * W + x) * 4];
        d[0] = clampU8(static_cast<int>(sr / div + f.convBias * 255.0f));
        d[1] = clampU8(static_cast<int>(sg / div + f.convBias * 255.0f));
        d[2] = clampU8(static_cast<int>(sb / div + f.convBias * 255.0f));
        if (f.convPreserveAlpha) d[3] = src[(static_cast<std::size_t>(y) * W + x) * 4 + 3];
        else d[3] = clampU8(static_cast<int>(sa / div + f.convBias * 255.0f));
    }
}
// 按 alpha 强度(0..255) 在渐变色停间插值（ratio 升序：越大越靠内）。近似 Flash 渐变辉光着色。
static Color gradientColorAt(const Filter& f, std::uint8_t intensity) {
    int n = static_cast<int>(f.gradientColors.size());
    if (n == 0) return f.color;
    if (n == 1) return f.gradientColors[0];
    int t = intensity, lo = 0;
    while (lo < n - 1 && f.gradientRatios[lo + 1] <= t) ++lo;
    int hi = std::min(n - 1, lo + 1);
    int r0 = f.gradientRatios[lo], r1 = f.gradientRatios[hi];
    float u = (r1 > r0) ? static_cast<float>(t - r0) / (r1 - r0) : 0.0f;
    const Color& a = f.gradientColors[lo]; const Color& b = f.gradientColors[hi];
    return Color(static_cast<std::uint8_t>(a.r + (b.r - a.r) * u),
                 static_cast<std::uint8_t>(a.g + (b.g - a.g) * u),
                 static_cast<std::uint8_t>(a.b + (b.b - a.b) * u),
                 static_cast<std::uint8_t>(a.a + (b.a - a.a) * u));
}
// 渐变辉光：偏移+着色 layer alpha（按强度查渐变色），输出到 out。
static void buildGradientGlow(const std::uint8_t* layer, std::uint8_t* out, int W, int H,
                              const Filter& f, int offx, int offy) {
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        const std::uint8_t* s = &layer[(static_cast<std::size_t>(y) * W + x) * 4];
        if (s[3] == 0) continue;
        int dx = x + offx, dy = y + offy;
        if (dx < 0 || dx >= W || dy < 0 || dy >= H) continue;
        std::uint8_t* d = &out[(static_cast<std::size_t>(dy) * W + dx) * 4];
        Color c = gradientColorAt(f, s[3]);
        std::uint8_t na = clampU8(static_cast<int>(s[3] * f.strength));
        if (na > d[3]) { d[0] = c.r; d[1] = c.g; d[2] = c.b; d[3] = na; }
    }
}
// 对离屏 layer 应用滤镜并合成回 buf：Blur 模糊本体；DropShadow/Glow 生成着色模糊阴影衬底。
static void applyFilters(const std::vector<Filter>& filters, std::vector<std::uint8_t>& layer,
                         std::uint8_t* buf, int W, int H, std::size_t npix) {

    for (const auto& f : filters) {
        if (f.kind == FilterKind::Blur) {
            int rx = static_cast<int>(f.blurX / 20.0f), ry = static_cast<int>(f.blurY / 20.0f);
            blurBuffer(layer.data(), W, H, rx, ry, std::max(1, f.passes));
        } else if (f.kind == FilterKind::DropShadow || f.kind == FilterKind::Glow) {
            int offx = 0, offy = 0;
            if (f.kind == FilterKind::DropShadow) {
                double rad = f.angle * 3.141592653589793 / 180.0;
                offx = static_cast<int>(f.distance / 20.0 * std::cos(rad));
                offy = static_cast<int>(f.distance / 20.0 * std::sin(rad));
            }
            float str = std::min(1.0f, std::max(0.0f, f.strength));
            std::vector<std::uint8_t> glow(npix * 4, 0);
            buildShadow(layer.data(), glow.data(), W, H, f.color, offx, offy, str);
            int rx = static_cast<int>(f.blurX / 20.0f), ry = static_cast<int>(f.blurY / 20.0f);
            if (rx >= 1 || ry >= 1) blurBuffer(glow.data(), W, H, rx, ry, std::max(1, f.passes));
            compositeOver(glow.data(), buf, npix);   // 阴影衬底先合成（在对象之后）
        } else if (f.kind == FilterKind::ColorMatrix) {
            applyColorMatrix(layer, f.colorMatrix);   // 修改对象本体颜色，末尾由 layer 合成
        } else if (f.kind == FilterKind::Convolution) {
            applyConvolution(layer, W, H, f);         // 同上，原地作用于 layer
        } else if (f.kind == FilterKind::Bevel) {
            double rad = f.angle * 3.141592653589793 / 180.0;
            int offx = static_cast<int>(f.distance / 20.0 * std::cos(rad));
            int offy = static_cast<int>(f.distance / 20.0 * std::sin(rad));
            float str = std::min(1.0f, std::max(0.0f, f.strength));
            int rx = static_cast<int>(f.blurX / 20.0f), ry = static_cast<int>(f.blurY / 20.0f);
            std::vector<std::uint8_t> sh(npix * 4, 0), hi(npix * 4, 0);
            buildShadow(layer.data(), sh.data(), W, H, f.color, offx, offy, str);       // 暗侧
            buildShadow(layer.data(), hi.data(), W, H, f.highlight, -offx, -offy, str); // 亮侧
            if (rx >= 1 || ry >= 1) {
                blurBuffer(sh.data(), W, H, rx, ry, std::max(1, f.passes));
                blurBuffer(hi.data(), W, H, rx, ry, std::max(1, f.passes));
            }
            compositeOver(sh.data(), buf, npix);
            compositeOver(hi.data(), buf, npix);
        } else if (f.kind == FilterKind::GradientGlow || f.kind == FilterKind::GradientBevel) {
            double rad = f.angle * 3.141592653589793 / 180.0;
            int offx = static_cast<int>(f.distance / 20.0 * std::cos(rad));
            int offy = static_cast<int>(f.distance / 20.0 * std::sin(rad));
            float str = std::min(1.0f, std::max(0.0f, f.strength));
            int rx = static_cast<int>(f.blurX / 20.0f), ry = static_cast<int>(f.blurY / 20.0f);
            std::vector<std::uint8_t> glow(npix * 4, 0);
            buildGradientGlow(layer.data(), glow.data(), W, H, f, offx, offy);
            if (f.kind == FilterKind::GradientBevel) {  // 反向再生成一侧，模拟斜角
                std::vector<std::uint8_t> glow2(npix * 4, 0);
                buildGradientGlow(layer.data(), glow2.data(), W, H, f, -offx, -offy);
                if (rx >= 1 || ry >= 1) blurBuffer(glow2.data(), W, H, rx, ry, std::max(1, f.passes));
                compositeOver(glow2.data(), buf, npix);
            }
            if (rx >= 1 || ry >= 1) blurBuffer(glow.data(), W, H, rx, ry, std::max(1, f.passes));
            compositeOver(glow.data(), buf, npix);
        }
    }
    compositeOver(layer.data(), buf, npix);          // 对象本体最后合成
}

// 渲染一组显示列表项；遇到 clipDepth 图层时，把其 depth+1..clipDepth 范围内的子项
// 渲染到离屏缓冲，并用该图层形状的填充区域作遮罩合成回主缓冲（实现 SWF 裁剪）。
// 主时间轴与精灵内部时间轴通用（精灵内遮罩由此获得支持）。
void renderItemsWithClip(const std::vector<DisplayItem>& items, const Matrix& root,
                         const std::map<std::uint16_t, std::shared_ptr<Character>>& dict,
                         std::uint8_t* buf, int W, int H, int frameIndex,
                         std::size_t npix, float alpha,
                         const ColorTransform& inheritedCx) {
    int n = static_cast<int>(items.size());
    int i = 0;
    while (i < n) {
        const DisplayItem& it = items[i];
        if (it.clipDepth > 0) {
            // SWF 语义：clipDepth 对象本身仍正常绘制（无填充的"隐形遮罩"自然不画任何像素），
            // 同时它定义了后续 depth+1..clipDepth 层的裁剪区域。
            if (it.character) renderItemInto(it, root, dict, buf, W, H, frameIndex, alpha, it.blend, inheritedCx);
            const Shape* ms = (it.character && it.character->kind == Character::Kind::Shape &&
                               it.character->shape) ? it.character->shape.get() : nullptr;
            int j = i + 1;
            while (j < n && items[j].depth <= it.clipDepth) ++j;
            if (ms) {
                // 遮罩设备包围盒：缓冲清零与合成只在盒内进行。
                // 全屏遮罩组若按整幅分配/清零/合成，是慢场景的主要固定开销。
                Matrix wm = root.post(it.matrix);
                float fbx0 = 1e9f, fby0 = 1e9f, fbx1 = -1e9f, fby1 = -1e9f;
                for (const auto& contours : ms->fillPaths)
                    for (const auto& ct : contours)
                        for (int k = 0; k < static_cast<int>(ct.xs.size()); ++k) {
                            float X, Y; wm.apply(ct.xs[k], ct.ys[k], X, Y);
                            fbx0 = std::min(fbx0, X); fby0 = std::min(fby0, Y);
                            fbx1 = std::max(fbx1, X); fby1 = std::max(fby1, Y);
                        }
                int bx0 = std::max(0, static_cast<int>(std::floor(fbx0)));
                int by0 = std::max(0, static_cast<int>(std::floor(fby0)));
                int bx1 = std::min(W - 1, static_cast<int>(std::ceil(fbx1)));
                int by1 = std::min(H - 1, static_cast<int>(std::ceil(fby1)));
                if (bx0 <= bx1 && by0 <= by1) {
                    static std::vector<std::uint8_t> maskBuf, layerBuf;
                    if (maskBuf.size() < npix) maskBuf.assign(npix, 0);
                    if (layerBuf.size() < npix * 4) layerBuf.assign(npix * 4, 0);
                    std::uint8_t* mask = maskBuf.data();
                    std::uint8_t* layer = layerBuf.data();
                    int bw = bx1 - bx0 + 1;
                    for (int y = by0; y <= by1; ++y) {
                        std::memset(mask + static_cast<std::size_t>(y) * W + bx0, 0, static_cast<std::size_t>(bw));
                        std::memset(layer + (static_cast<std::size_t>(y) * W + bx0) * 4, 0,
                                    static_cast<std::size_t>(bw) * 4);
                    }
                    fillMask(ms->fillPaths, wm, mask, W, H);
                    for (int q = i + 1; q < j; ++q)
                        if (items[q].clipDepth == 0 && items[q].character)
                            renderItemInto(items[q], root, dict, layer, W, H, frameIndex, alpha, items[q].blend, inheritedCx);
                    parallelBands(by0, by1, [&](int ya, int yb) {
                        for (int y = ya; y <= yb; ++y) {
                            std::size_t p = static_cast<std::size_t>(y) * W + bx0;
                            for (int x = 0; x < bw; ++x, ++p) {
                                std::uint8_t mv = mask[p];
                                if (!mv) continue;
                                const std::uint8_t* s = &layer[p * 4];
                                std::uint8_t* d = &buf[p * 4];
                                float sa = (s[3] / 255.0f) * (mv / 255.0f);
                                if (sa <= 0.0f) continue;
                                float da = d[3] / 255.0f;
                                float outA = sa + da * (1.0f - sa);
                                if (outA <= 0.0f) continue;
                                d[0] = static_cast<std::uint8_t>((s[0] * sa + d[0] * da * (1.0f - sa)) / outA);
                                d[1] = static_cast<std::uint8_t>((s[1] * sa + d[1] * da * (1.0f - sa)) / outA);
                                d[2] = static_cast<std::uint8_t>((s[2] * sa + d[2] * da * (1.0f - sa)) / outA);
                                d[3] = static_cast<std::uint8_t>(outA * 255.0f);
                            }
                        }
                    });
                }
            }
            i = j;  // 跳过整个裁剪组
        } else {
            if (it.character) {
                if (!it.filters.empty()) {
                    // 带滤镜：先渲染到离屏层，应用滤镜（投影/模糊/辉光），再合成回主缓冲
                    std::vector<std::uint8_t> layer(npix * 4, 0);
                    renderItemInto(it, root, dict, layer.data(), W, H, frameIndex, alpha, it.blend, inheritedCx);
                    applyFilters(it.filters, layer, buf, W, H, npix);
                } else {
                    renderItemInto(it, root, dict, buf, W, H, frameIndex, alpha, it.blend, inheritedCx);
                }
            }
            ++i;
        }
    }
}

// 栅格化运行时 TextField（AS3 设置的 .text）。复用已解码的字体字形轮廓，
// 按字符码映射到字形，并以字号缩放（字形设计单位 em=1024）。
static void renderDynamicText(const fvm::DisplayObject& d, const Matrix& world,
                              const Movie& movie, std::uint8_t* buf, int W, int H, float alpha) {
    // AS3 TextField 的字体由 defaultTextFormat.font 指定：按名字找嵌入字体，
    // 找不到就用同名的系统（设备）字体，不再随便取字典里的第一个字体。
    const FontGlyphs* fg = nullptr;
    if (!d.textFont.empty())
        for (const auto& kv : movie.characters)
            if (kv.second && kv.second->font && kv.second->font->name == d.textFont) {
                fg = kv.second->font.get(); break;
            }
    FontGlyphs deviceFont;
    if (!fg) { deviceFont.name = d.textFont; fg = &deviceFont; }

    const float fontHeight = static_cast<float>(d.textSize) * 20.0f;   // px -> twips
    if (fontHeight <= 0.0f || d.textData.empty()) return;

    const TextMetrics tm = textMetrics(*fg, fontHeight);
    Color col(static_cast<std::uint8_t>((d.textColor >> 16) & 0xFF),
              static_cast<std::uint8_t>((d.textColor >> 8) & 0xFF),
              static_cast<std::uint8_t>(d.textColor & 0xFF),
              static_cast<std::uint8_t>((d.textColor >> 24) & 0xFF));
    Paint p; p.kind = FillKind::Solid; p.solid = col;
    float penX = 0.0f, penY = 0.0f;
    std::uint32_t prev = 0;
    for (std::uint32_t ch : utf8Codes(d.textData)) {
        if (ch == '\r') continue;
        if (ch == '\n') { penY += tm.lineH; penX = 0.0f; prev = 0; continue; }
        int gi = -1;
        const Shape* gs = nullptr;
        auto it = fg->codeToGlyph.find(static_cast<int>(ch));
        if (it != fg->codeToGlyph.end() && it->second >= 0 &&
            it->second < static_cast<int>(fg->shapes.size())) {
            gi = it->second;
            gs = fg->shapes[static_cast<std::size_t>(gi)].get();
        }
        float deviceAdv = 0.0f;
#ifdef _WIN32
        if (!gs) deviceAdv = deviceGlyph(fg->name, ch, fg->italic, fg->bold).advanceEm;
#endif
        const float adv = glyphAdvanceTwips(*fg, gi, gs, deviceAdv, tm) +
                          kerningUnits(*fg, prev, ch) * tm.scale;
        const float gsScale = gs ? tm.scale : tm.deviceScale;
        Matrix ms; ms.sx = gsScale; ms.sy = gsScale;
        Matrix mt; mt.tx = penX; mt.ty = penY + tm.ascent;
        Matrix gm = world.post(ms).post(mt);
        Matrix Minv; gm.invert(Minv);
        if (gs) {
            for (std::size_t i = 0; i < gs->fillStyles.size(); ++i)
                fillStyleOf(*gs, i, gm, Minv, p, buf, W, H, alpha, BlendMode::Normal);
#ifdef _WIN32
        } else {
            const auto& dg = deviceGlyph(fg->name, ch, fg->italic, fg->bold);
            if (dg.found) fillContours(dg.contours, gm, Minv, p, buf, W, H, alpha, BlendMode::Normal);
#endif
        }
        penX += adv;
        prev = ch;
    }
}

// 对象局部变换（parent twips -> parent twips）：显示树渲染与裁剪组共用。
static Matrix displayLocalMatrix(const fvm::DisplayObject& d) {
    Matrix Lt;
    if (d.hasFull) {
        Lt.sx = static_cast<float>(d.fullA);
        Lt.r0 = static_cast<float>(d.fullB);
        Lt.r1 = static_cast<float>(d.fullC);
        Lt.sy = static_cast<float>(d.fullD);
    } else {
        double rad = d.rotation * 3.141592653589793 / 180.0;
        double ca = std::cos(rad), sa = std::sin(rad);
        Lt.sx  = static_cast<float>(d.scaleX * ca);
        Lt.r0  = static_cast<float>(-d.scaleX * sa);
        Lt.r1  = static_cast<float>(d.scaleY * sa);
        Lt.sy  = static_cast<float>(d.scaleY * ca);
    }
    Lt.tx = static_cast<float>(d.x * 20.0);
    Lt.ty = static_cast<float>(d.y * 20.0);
    return Lt;
}

void renderDisplayObject(const std::shared_ptr<fvm::DisplayObject>& d, const Matrix& parent,
                         const Movie& movie, std::uint8_t* buf, int W, int H,
                         int frameIndex, float alpha, const ColorTransform& inheritedCx);

// 取出 clipDepth 层用作裁剪区域的形状（通常是文字轮廓/实心遮罩）。
static const Shape* maskShapeOf(const fvm::DisplayObject& d, const Movie& movie) {
    if (!d.characterId) return nullptr;
    auto it = movie.characters.find(d.characterId);
    if (it == movie.characters.end() || !it->second ||
        it->second->kind != Character::Kind::Shape || !it->second->shape) return nullptr;
    return it->second->shape.get();
}

// AVM2 运行时子显示列表：按 depth 顺序绘制，并对 clipDepth 组做与静态
// renderItemsWithClip 相同的遮罩合成。品牌门户页的标语渐变色带就是靠
// 白色文字轮廓遮罩裁成彩色字形，缺了这步会溢出成一整条矩形色块。
static void renderRuntimeChildren(const std::vector<std::shared_ptr<fvm::DisplayObject>>& children,
                                  const Matrix& parent, const Movie& movie,
                                  std::uint8_t* buf, int W, int H, int frameIndex,
                                  float alpha, const ColorTransform& cx) {
    const int n = static_cast<int>(children.size());
    const std::size_t npix = static_cast<std::size_t>(W) * H;
    int i = 0;
    while (i < n) {
        const auto& c = children[static_cast<std::size_t>(i)];
        if (c && c->clipDepth > 0) {
            int j = i + 1;
            while (j < n && children[static_cast<std::size_t>(j)] &&
                   children[static_cast<std::size_t>(j)]->depth >= 0 &&
                   children[static_cast<std::size_t>(j)]->depth <= c->clipDepth) ++j;
            const Shape* ms = maskShapeOf(*c, movie);
            if (!ms || ms->fillPaths.empty() || j == i + 1) {
                // 遮罩不可用：保持旧行为（遮罩层本身不绘制，组内对象直接画）
                for (int q = i + 1; q < j; ++q)
                    renderDisplayObject(children[static_cast<std::size_t>(q)], parent, movie,
                                        buf, W, H, frameIndex, alpha, cx);
                i = j;
                continue;
            }
            Matrix wm = parent.post(displayLocalMatrix(*c));
            float fbx0 = 1e9f, fby0 = 1e9f, fbx1 = -1e9f, fby1 = -1e9f;
            for (const auto& contours : ms->fillPaths)
                for (const auto& ct : contours)
                    for (int k = 0; k < static_cast<int>(ct.xs.size()); ++k) {
                        float X, Y; wm.apply(ct.xs[k], ct.ys[k], X, Y);
                        fbx0 = std::min(fbx0, X); fby0 = std::min(fby0, Y);
                        fbx1 = std::max(fbx1, X); fby1 = std::max(fby1, Y);
                    }
            int bx0 = std::max(0, static_cast<int>(std::floor(fbx0)));
            int by0 = std::max(0, static_cast<int>(std::floor(fby0)));
            int bx1 = std::min(W - 1, static_cast<int>(std::ceil(fbx1)));
            int by1 = std::min(H - 1, static_cast<int>(std::ceil(fby1)));
            if (bx0 <= bx1 && by0 <= by1) {
                // 与静态遮罩路径一致：离屏层只清包围盒，合成按行带并行。
                // 整幅 4MB 零初始化 + 串行逐像素合成是运行时遮罩的主要开销。
                std::vector<std::uint8_t> mask(npix, 0);
                std::unique_ptr<std::uint8_t[]> layerBuf(new std::uint8_t[npix * 4]);
                std::uint8_t* layer = layerBuf.get();
                const int bw = bx1 - bx0 + 1;
                for (int y = by0; y <= by1; ++y)
                    std::memset(layer + (static_cast<std::size_t>(y) * W + bx0) * 4, 0,
                                static_cast<std::size_t>(bw) * 4);
                fillMask(ms->fillPaths, wm, mask.data(), W, H);
                for (int q = i + 1; q < j; ++q) {
                    const auto& g = children[static_cast<std::size_t>(q)];
                    if (!g || g->clipDepth > 0) continue;
                    renderDisplayObject(g, parent, movie, layer, W, H,
                                        frameIndex, alpha, cx);
                }
                parallelBands(by0, by1, [&](int ya, int yb) {
                    for (int y = ya; y <= yb; ++y) {
                        std::size_t p = static_cast<std::size_t>(y) * W + bx0;
                        for (int x = 0; x < bw; ++x, ++p) {
                            std::uint8_t mv = mask[p];
                            if (!mv) continue;
                            const std::uint8_t* s = &layer[p * 4];
                            std::uint8_t* d = &buf[p * 4];
                            float sa = (s[3] / 255.0f) * (mv / 255.0f);
                            if (sa <= 0.0f) continue;
                            float da = d[3] / 255.0f;
                            float outA = sa + da * (1.0f - sa);
                            if (outA <= 0.0f) continue;
                            d[0] = static_cast<std::uint8_t>((s[0] * sa + d[0] * da * (1.0f - sa)) / outA);
                            d[1] = static_cast<std::uint8_t>((s[1] * sa + d[1] * da * (1.0f - sa)) / outA);
                            d[2] = static_cast<std::uint8_t>((s[2] * sa + d[2] * da * (1.0f - sa)) / outA);
                            d[3] = static_cast<std::uint8_t>(outA * 255.0f);
                        }
                    }
                });
            }
            i = j;
        } else {
            renderDisplayObject(c, parent, movie, buf, W, H, frameIndex, alpha, cx);
            ++i;
        }
    }
}

// 纯黑 alpha 渐变（色停 RGB 全为 0、仅靠 alpha 交替）是 SWF 常见的遮罩/闪光层：
// Flash 用它定义透明度，而不是可见的黑色。把它当普通渐变绘制会铺出实心黑条
// （品牌门户页标语处的黑块即由此产生），按遮罩语义跳过该对象。
static bool isAlphaMaskShape(const fvm::DisplayObject& d, const Movie& movie) {
    if (!d.characterId) return false;
    auto it = movie.characters.find(d.characterId);
    if (it == movie.characters.end() || !it->second) return false;
    // 普通形状与变形形状都可能是纯黑 alpha 遮罩层；Morph 用起始段样式判定
    // （门户页的闪光扫过层就是 DefineMorphShape 版本，终止段解码后才会画出）。
    const Shape* shp = nullptr;
    if (it->second->kind == Character::Kind::Shape && it->second->shape)
        shp = it->second->shape.get();
    else if (it->second->kind == Character::Kind::Morph && it->second->morph &&
             it->second->morph->start)
        shp = it->second->morph->start.get();
    if (!shp) return false;
    const auto& styles = shp->fillStyles;
    if (styles.empty()) return false;
    bool allBlack = true, hasTransparent = false;
    for (const auto& fs : styles) {
        if (fs.kind != FillKind::LinearGradient && fs.kind != FillKind::RadialGradient &&
            fs.kind != FillKind::FocalGradient) return false;
        if (fs.gradient.empty()) return false;
        for (const auto& gs : fs.gradient) {
            if (gs.color.r || gs.color.g || gs.color.b) allBlack = false;
            if (gs.color.a < 255) hasTransparent = true;
        }
    }
    return allBlack && hasTransparent;
}

// ---- AS3 Graphics 绘制命令回放（此前 VM 端整组方法为 no-op，动态绘制画不出来）----
// 命令坐标是对象局部像素；world 把"局部 twips"映射到设备像素（与角色渲染一致），
// 故局部像素先乘 20 转 twips 再进矩阵。填充用非零环绕（与 Flash UsesFillWindingRule
// 语义一致，子路径方向自然表达并集/洞）；描边按段生成垂直四边形（无 join/cap，最小可用）。
static void renderGraphicsInto(const fvm::DisplayObject& d, const Matrix& world,
                               std::uint8_t* buf, int W, int H,
                               float alphaMul, BlendMode bm) {
    if (!d.graphics || d.graphics->empty()) return;
    Matrix Minv;
    world.invert(Minv);
    auto toTw = [](double v) { return static_cast<float>(v * 20.0); };

    std::vector<FillEdge> group;                 // 当前填充组的全部有向边（多子路径共存）
    std::vector<std::pair<float, float>> cur;    // 进行中的子路径（局部 twips）
    bool fillOn = false;
    float fillAlpha = 1.0f;
    Paint fillPaint; fillPaint.kind = FillKind::Solid; fillPaint.winding = true;
    float strokeW = 0.0f;                        // 局部 twips；<=0/NaN = 无描边
    float strokeAlpha = 1.0f;
    Paint strokePaint; strokePaint.kind = FillKind::Solid; strokePaint.winding = true;

    auto setPaint = [](Paint& p, std::uint32_t argb) {
        p.solid = Color(static_cast<std::uint8_t>((argb >> 16) & 0xFF),
                        static_cast<std::uint8_t>((argb >> 8) & 0xFF),
                        static_cast<std::uint8_t>(argb & 0xFF),
                        static_cast<std::uint8_t>(argb >> 24));
    };
    auto closeSubpath = [&]() {
        if (cur.size() >= 3) {
            const std::size_t n = cur.size();
            for (std::size_t i = 0; i < n; ++i) {
                const auto& a = cur[i];
                const auto& b = cur[(i + 1) % n];
                group.push_back({a.first, a.second, b.first, b.second, 1});
            }
        }
        cur.clear();
    };
    auto flushFill = [&]() {
        closeSubpath();
        if (fillOn && !group.empty())
            fillEdgesWinding(group, world, Minv, fillPaint, buf, W, H,
                             alphaMul * fillAlpha, bm);
        group.clear();
        fillOn = false;
    };
    auto strokeSeg = [&](float x0, float y0, float x1, float y1) {
        if (!(strokeW > 0.0f)) return;
        const float dx = x1 - x0, dy = y1 - y0;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len <= 1e-6f) return;
        const float nx = -dy / len * strokeW * 0.5f, ny = dx / len * strokeW * 0.5f;
        const float px[4] = {x0 + nx, y0 + ny, x1 + nx, y1 + ny};
        const float py[4] = {y0 + ny, y1 + ny, y1 - ny, y0 - ny};
        std::vector<FillEdge> quad;
        quad.reserve(4);
        for (int i = 0; i < 4; ++i) {
            const int j = (i + 1) & 3;
            quad.push_back({px[i], py[i], px[j], py[j], 1});
        }
        fillEdgesWinding(quad, world, Minv, strokePaint, buf, W, H,
                         alphaMul * strokeAlpha, bm);
    };
    auto lineToPt = [&](float x, float y) {
        if (cur.empty()) { cur.emplace_back(x, y); return; }
        const auto p = cur.back();
        if (strokeW > 0.0f) strokeSeg(p.first, p.second, x, y);
        cur.emplace_back(x, y);
    };
    auto addLoop = [&](const std::vector<std::pair<float, float>>& pts) {
        if (pts.size() < 3) return;
        for (std::size_t i = 0; i < pts.size(); ++i) {
            const auto& a = pts[i];
            const auto& b = pts[(i + 1) % pts.size()];
            group.push_back({a.first, a.second, b.first, b.second, 1});
        }
    };
    auto ellipseLoop = [&](float cx, float cy, float rx, float ry) {
        if (rx <= 0 || ry <= 0) return;
        std::vector<std::pair<float, float>> pts;
        pts.reserve(40);
        const int N = 40;
        for (int i = 0; i < N; ++i) {
            const float ang = static_cast<float>(i) * 6.28318530718f / N;
            pts.emplace_back(cx + std::cos(ang) * rx, cy + std::sin(ang) * ry);
        }
        addLoop(pts);
    };

    for (const auto& cmd : *d.graphics) {
        switch (cmd.op) {
            case fvm::GraphicsCmd::Op::BeginFill:
                closeSubpath();
                group.clear();
                fillOn = true;
                setPaint(fillPaint, cmd.color);
                fillAlpha = static_cast<float>(cmd.color >> 24) / 255.0f;
                break;
            case fvm::GraphicsCmd::Op::EndFill:
                flushFill();
                break;
            case fvm::GraphicsCmd::Op::LineStyle:
                strokeW = (cmd.a == cmd.a && cmd.a > 0.0) ? toTw(cmd.a) : 0.0f;
                setPaint(strokePaint, cmd.color);
                strokeAlpha = static_cast<float>(cmd.color >> 24) / 255.0f;
                break;
            case fvm::GraphicsCmd::Op::MoveTo:
                closeSubpath();
                cur.emplace_back(toTw(cmd.x), toTw(cmd.y));
                break;
            case fvm::GraphicsCmd::Op::LineTo:
                lineToPt(toTw(cmd.x), toTw(cmd.y));
                break;
            case fvm::GraphicsCmd::Op::CurveTo: {
                float x0 = 0, y0 = 0;
                if (!cur.empty()) { x0 = cur.back().first; y0 = cur.back().second; }
                const int N = 12;
                for (int i = 1; i <= N; ++i) {
                    const float t = static_cast<float>(i) / N;
                    const float u = 1.0f - t;
                    lineToPt(u * u * x0 + 2 * u * t * toTw(cmd.cx) + t * t * toTw(cmd.x),
                             u * u * y0 + 2 * u * t * toTw(cmd.cy) + t * t * toTw(cmd.y));
                }
                break;
            }
            case fvm::GraphicsCmd::Op::Rect: {
                closeSubpath();
                const float x = toTw(cmd.x), y = toTw(cmd.y);
                const float x1 = toTw(cmd.x + cmd.w), y1 = toTw(cmd.y + cmd.h);
                addLoop({{x, y}, {x1, y}, {x1, y1}, {x, y1}});
                break;
            }
            case fvm::GraphicsCmd::Op::RoundRect: {
                closeSubpath();
                const float x = toTw(cmd.x), y = toTw(cmd.y);
                const float x1 = toTw(cmd.x + cmd.w), y1 = toTw(cmd.y + cmd.h);
                const float r = toTw(cmd.r);
                const float rx = std::min(r, (x1 - x) * 0.5f), ry = std::min(r, (y1 - y) * 0.5f);
                if (rx <= 0 || ry <= 0) {
                    addLoop({{x, y}, {x1, y}, {x1, y1}, {x, y1}});
                    break;
                }
                std::vector<std::pair<float, float>> pts;
                auto arc = [&](float acx, float acy, float a0, float a1) {
                    const int N = 8;
                    for (int i = 0; i <= N; ++i) {
                        const float ang = a0 + (a1 - a0) * i / N;
                        pts.emplace_back(acx + std::cos(ang) * rx, acy + std::sin(ang) * ry);
                    }
                };
                arc(x1 - rx, y + ry, -1.57079632679f, 0.0f);
                arc(x1 - rx, y1 - ry, 0.0f, 1.57079632679f);
                arc(x + rx, y1 - ry, 1.57079632679f, 3.14159265359f);
                arc(x + rx, y + ry, 3.14159265359f, 4.71238898038f);
                addLoop(pts);
                break;
            }
            case fvm::GraphicsCmd::Op::Circle:
                closeSubpath();
                ellipseLoop(toTw(cmd.x), toTw(cmd.y), toTw(cmd.r), toTw(cmd.r));
                break;
            case fvm::GraphicsCmd::Op::Ellipse:
                closeSubpath();
                ellipseLoop(toTw(cmd.x + cmd.w * 0.5), toTw(cmd.y + cmd.h * 0.5),
                            toTw(cmd.w * 0.5), toTw(cmd.h * 0.5));
                break;
        }
    }
    flushFill();   // 命令流结束仍在填充中：Flash 语义会隐式提交
}

// 临时诊断：当前 renderFrameWithItems 调用序号（按输出帧计）。
static int g_dbgRenderCall = 0;

// AVM2 运行时显示列表（AS3 构建的场景）的渲染。
// parent：父级把"角色局部 twips"映射为设备像素的仿射矩阵；
// DisplayObject 的 x/y 单位 = 父级局部空间的像素（20 twips），scale/rotation 分解自其变换矩阵。
static void renderDisplayObjectImpl(const std::shared_ptr<fvm::DisplayObject>& d, const Matrix& parent,
                                    const Movie& movie, std::uint8_t* buf, int W, int H,
                                    int frameIndex, float alpha,
                                    const ColorTransform& inheritedCx) {
    if (d && g_dbgRenderCall == 180 && std::getenv("SWF_RENDER_DBG") &&
        (d->characterId == 370 || d->characterId == 372)) {
        static int _ln = 0;
        if (_ln < 12) {
            std::fprintf(stderr,
                         "[leaf] cid=%d depth=%d vis=%d clip=%u a=%.2f built=%d mask=%d filt=%d "
                         "x=%.1f y=%.1f sx=%.3f sy=%.3f kids=%zu\n",
                         d->characterId, d->depth, (int)d->visible, d->clipDepth, d->alpha,
                         (int)d->timelineBuilt, (int)isAlphaMaskShape(*d, movie),
                         (int)(d->filters && !d->filters->empty()), d->x, d->y,
                         d->scaleX, d->scaleY, d->children.size());
            ++_ln;
        }
    }
    if (!d || !d->visible) return;
    // 带 clipDepth 的层是遮罩：Flash 只把它当作裁剪区域，自身不绘制。
    // 运行时显示树此前丢了该字段，preloader 头像的黑色渐变遮罩会被画成实心黑块。
    if (d->clipDepth > 0) return;
    if (isAlphaMaskShape(*d, movie)) return;
    // 对象局部变换（parent twips -> parent twips）。
    // 时间轴放置的对象直接沿用 PlaceObject 的完整 2x2（hasFull），保留错切/旋转的
    // 非正交分量；仅 AS3 动态创建/改写 scaleX/scaleY/rotation 的对象才走分解模型。
    Matrix world = parent.post(displayLocalMatrix(*d));   // 角色局部 twips -> 设备像素
    float a = alpha * static_cast<float>(d->alpha);
    if (g_dbgRenderCall == 180 && std::getenv("SWF_RENDER_DBG") &&
        (d->characterId == 366 || d->characterId == 369 || d->characterId == 378 ||
         d->characterId == 381 || d->characterId == 385 || d->characterId == 386)) {
        static int _sn = 0;
        if (_sn < 30) {
            std::fprintf(stderr, "[spr] cid=%d depth=%d pos=(%.1f,%.1f) scale=%.4f a=%.2f cf=%d built=%d fi=%d\n",
                         d->characterId, d->depth, world.tx, world.ty,
                         std::sqrt(static_cast<double>(world.sx) * world.sx +
                                   static_cast<double>(world.r1) * world.r1), a,
                         d->currentFrame, (int)d->timelineBuilt, frameIndex);
            ++_sn;
        }
    }
    // 本对象最终 ColorTransform = 祖先累积 ∘ 自身（ma 已并入 alpha，避免重复应用）
    ColorTransform ownCx;
    ownCx.hasMult = true; ownCx.hasAdd = true;
    ownCx.mr = static_cast<float>(d->cxMr); ownCx.mg = static_cast<float>(d->cxMg);
    ownCx.mb = static_cast<float>(d->cxMb); ownCx.ma = 1.0f;
    ownCx.ar = static_cast<float>(d->cxAr); ownCx.ag = static_cast<float>(d->cxAg);
    ownCx.ab = static_cast<float>(d->cxAb); ownCx.aa = 0.0f;
    const ColorTransform cx = inheritedCx.post(ownCx);
    bool staticTextReplaced = false;


    // 时间轴帧内容已由 AVM2 重建为子显示对象（timelineBuilt）时，不再重复绘制整段精灵
    if (d->characterId && !d->timelineBuilt) {
        auto it = movie.characters.find(d->characterId);
        if (it != movie.characters.end() && it->second) {
            DisplayItem di;
            di.depth = static_cast<std::uint16_t>(d->depth < 0 ? 0xFFFF : d->depth);
            di.characterId = d->characterId;
            di.character = it->second;
            di.buttonState = d->buttonState;   // 按钮悬停/按下态
            di.ratio = d->ratio;              // Morph 形状补间比例
            di.matrix = Matrix{};            // 单位矩阵：world 已含对象变换
            di.cxform = cx;                  // 祖先累积 + 自身调色
            if (d->hasTextOverride) {        // 运行时数值覆盖时间轴 EditText 的初始文本
                di.textOverride = d->textOverride;
                di.hasTextOverride = true;
            } else if (d->hasText && !d->textData.empty() &&
                       it->second->kind == Character::Kind::EditText) {
                // AS3 的 tf.text 必须替换 DefineEditText 的 InitialText；此前只额外叠加
                // 一层动态文本，静态初始文本（如进度条 "0%"）仍留在画面上，百分比不更新。
                di.textOverride = d->textData;
                di.hasTextOverride = true;
                staticTextReplaced = true;
            }
            // MovieClip 用自己的 currentFrame（动画），其余用外层帧号
            int fIdx = (d->kind == fvm::ObjKind::MovieClip && d->currentFrame > 0)
                           ? d->currentFrame : frameIndex;
            if (g_dbgRenderCall == 180 && std::getenv("SWF_RENDER_DBG") &&
                (d->characterId == 370 || d->characterId == 372)) {
                static int _gn = 0;
                if (_gn < 8) {
                    const Shape* shp = nullptr;
                    if (it->second->kind == Character::Kind::Shape && it->second->shape)
                        shp = it->second->shape.get();
                    else if (it->second->kind == Character::Kind::Morph && it->second->morph)
                        shp = it->second->morph->start.get();
                    float px = 0, py = 0;
                    world.apply(0, 0, px, py);
                    std::fprintf(stderr,
                                 "[geo] cid=%d ratio=%.2f a=%.2f world=(%.4f,%.4f,%.4f,%.4f,%.1f,%.1f) "
                                 "screen=(%.1f,%.1f) shp=%d fills=%zu paths=%zu edges=%zu strokes=%zu "
                                 "bounds=(%.0f,%.0f)-(%.0f,%.0f)\n",
                                 d->characterId, d->ratio, a, world.sx, world.r0, world.r1,
                                 world.sy, world.tx, world.ty, px, py, (int)(shp != nullptr),
                                 shp ? shp->fillStyles.size() : 0,
                                 shp ? shp->fillPaths.size() : 0,
                                 shp ? shp->fillEdges.size() : 0,
                                 shp ? shp->strokePaths.size() : 0,
                                 shp ? static_cast<double>(shp->bounds.x0) : 0.0,
                                 shp ? static_cast<double>(shp->bounds.y0) : 0.0,
                                 shp ? static_cast<double>(shp->bounds.x1) : 0.0,
                                 shp ? static_cast<double>(shp->bounds.y1) : 0.0);
                    ++_gn;
                }
            }
            const bool dbgPix = g_dbgRenderCall == 180 && std::getenv("SWF_RENDER_DBG") &&
                                (d->characterId == 370 || d->characterId == 372);
            long long s0 = 0;
            if (dbgPix) {
                for (std::size_t k = 0; k < static_cast<std::size_t>(W) * H * 4; k += 97) s0 += buf[k];
            }
            renderItemInto(di, world, movie.characters, buf, W, H, fIdx, a, d->blend);
            if (dbgPix) {
                long long s1 = 0;
                for (std::size_t k = 0; k < static_cast<std::size_t>(W) * H * 4; k += 97) s1 += buf[k];
                static int _pn = 0;
                if (_pn < 10) { std::fprintf(stderr, "[pix] cid=%d delta=%lld\n", d->characterId, s1 - s0); ++_pn; }
            }
        }
    }
    // AS3 Graphics 绘制命令回放（对象自身内容，位于运行时文本与子对象之下）
    if (d->graphics && !d->graphics->empty())
        renderGraphicsInto(*d, world, buf, W, H, a, d->blend);
    if (d->hasText && !d->textData.empty() && !staticTextReplaced)
        renderDynamicText(*d, world, movie, buf, W, H, a);
    renderRuntimeChildren(d->children, world, movie, buf, W, H, frameIndex, a, cx);
}

// PlaceObject3 FilterList 在运行时显示树同样要生效：把对象（含整棵子树）画进
// 离屏层，应用滤镜后再合成。静态 renderItemsWithClip 早已如此处理，此前只有
// 运行时路径丢失，导致 AVM2 场景里的发光/投影/模糊全部消失。
void renderDisplayObject(const std::shared_ptr<fvm::DisplayObject>& d, const Matrix& parent,
                         const Movie& movie, std::uint8_t* buf, int W, int H,
                         int frameIndex, float alpha = 1.0f,
                         const ColorTransform& inheritedCx = ColorTransform{}) {
    if (!d) return;
    if (d->filters && !d->filters->empty()) {
        const std::size_t npix = static_cast<std::size_t>(W) * H;
        std::vector<std::uint8_t> layer(npix * 4, 0);
        renderDisplayObjectImpl(d, parent, movie, layer.data(), W, H,
                                frameIndex, alpha, inheritedCx);
        applyFilters(*d->filters, layer, buf, W, H, npix);
        return;
    }
    renderDisplayObjectImpl(d, parent, movie, buf, W, H, frameIndex, alpha, inheritedCx);
}

}  // namespace

// 渲染单个 AVM2 运行时显示对象（含其子树）到 RGBA 缓冲。公开入口（测试/内嵌使用）：
// Graphics 命令回放、运行时文本与子树递归均在匿名命名空间的 renderDisplayObject 中完成。
void renderRuntimeObject(const std::shared_ptr<fvm::DisplayObject>& d, const Matrix& parent,
                         const Movie& movie, std::uint8_t* buf, int W, int H,
                         int frameIndex, float alpha) {
    renderDisplayObject(d, parent, movie, buf, W, H, frameIndex, alpha);
}

// 计算输出画布与根变换。targetW/H > 0 时按设备尺寸重新光栅化：根变换在
// twips->px 的 1/20 之上再乘显示缩放，所有矢量边缘/字形轮廓都按新分辨率扫描填充，
// 而不是先画小图再拉伸位图（后者放大必然像素化）。
static void setupSurface(const Movie& movie, int targetW, int targetH,
                         int& W, int& H, Matrix& root, std::size_t& npix) {
    const int stageW = static_cast<int>((movie.frameSize.x1 - movie.frameSize.x0) / 20.0f);
    const int stageH = static_cast<int>((movie.frameSize.y1 - movie.frameSize.y0) / 20.0f);
    const int baseW = stageW > 0 ? stageW : 1;
    const int baseH = stageH > 0 ? stageH : 1;
    W = targetW > 0 ? targetW : baseW;
    H = targetH > 0 ? targetH : baseH;
    npix = static_cast<std::size_t>(W) * H;
    const float sx = static_cast<float>(W) / static_cast<float>(baseW);
    const float sy = static_cast<float>(H) / static_cast<float>(baseH);
    root.sx = sx / 20.0f; root.sy = sy / 20.0f;
    root.tx = -movie.frameSize.x0 / 20.0f * sx;
    root.ty = -movie.frameSize.y0 / 20.0f * sy;
}

bool renderMovieToBuffer(const Movie& movie, int frameIndex,
                          std::vector<std::uint8_t>& out, int& W, int& H,
                          int targetW, int targetH) {
    if (frameIndex < 0 || frameIndex >= static_cast<int>(movie.frames.size())) return false;
    const Frame& frame = movie.frames[frameIndex];

    std::size_t npix = 0;
    Matrix root;
    setupSurface(movie, targetW, targetH, W, H, root, npix);
    // resize 而非 assign：紧随其后的背景填充会覆盖每个像素，省掉一次整幅清零。
    out.resize(npix * 4);

    // 背景
    Color bg = movie.backgroundColor;
    for (int i = 0; i < static_cast<int>(npix); ++i) {
        out[i * 4 + 0] = bg.r; out[i * 4 + 1] = bg.g; out[i * 4 + 2] = bg.b; out[i * 4 + 3] = bg.a;
    }

    // 按深度顺序绘制（含 clipDepth 裁剪组处理，与精灵内部通用）
    if (!movie.runtimeStage.empty()) {
        // AVM2 运行时树即最终显示列表（时间轴帧内容已实例化为子对象），
        // 不再叠加静态帧，否则会重复绘制且与 gotoAndStop 后的当前帧不一致。
    } else {
        renderItemsWithClip(frame.items, root, movie.characters, out.data(), W, H, frameIndex, npix, 1.0f);
    }

    // 叠加 AVM2 运行时（ActionScript 构建）的显示列表
    if (!movie.runtimeStage.empty()) {
        Matrix rootM;                     // 角色局部 twips -> 设备像素
        rootM.sx = root.sx; rootM.sy = root.sy;
        rootM.tx = root.tx; rootM.ty = root.ty;
        renderRuntimeChildren(movie.runtimeStage, rootM, movie, out.data(), W, H,
                              frameIndex, 1.0f, ColorTransform{});
    }
    return true;
}

bool renderMovieToPNG(const Movie& movie, int frameIndex, const char* path) {
    std::vector<std::uint8_t> buf;
    int W = 0, H = 0;
    if (!renderMovieToBuffer(movie, frameIndex, buf, W, H)) return false;
    return stbi_write_png(path, W, H, 4, buf.data(), W * 4) != 0;
}

bool renderFrameWithItems(const Movie& movie, const std::vector<DisplayItem>& items,
                          int frameIndex,
                          std::vector<std::uint8_t>& out, int& W, int& H,
                          int targetW, int targetH) {
    ++g_dbgRenderCall;
    std::size_t npix = 0;
    Matrix root;
    setupSurface(movie, targetW, targetH, W, H, root, npix);
    // resize 而非 assign：紧随其后的背景填充会覆盖每个像素，省掉一次整幅清零。
    out.resize(npix * 4);
    Color bg = movie.backgroundColor;
    for (int i = 0; i < static_cast<int>(npix); ++i) {
        out[i * 4 + 0] = bg.r; out[i * 4 + 1] = bg.g; out[i * 4 + 2] = bg.b; out[i * 4 + 3] = bg.a;
    }
    // 与 renderMovieToBuffer 对齐：AVM2 运行时树即最终显示列表（时间轴帧内容
    // 已实例化为子对象），叠加静态帧会重复绘制、双重 alpha 混合且与 gotoAndStop
    // 后的当前帧不一致，同时使每帧渲染耗时翻倍。
    if (movie.runtimeStage.empty())
        renderItemsWithClip(items, root, movie.characters, out.data(), W, H, frameIndex, npix, 1.0f);
    if (!movie.runtimeStage.empty()) {
        Matrix rootM; rootM.sx = root.sx; rootM.sy = root.sy;
        rootM.tx = root.tx; rootM.ty = root.ty;
        if (std::getenv("SWF_RENDER_DBG")) {
            if (g_dbgRenderCall == 30 || g_dbgRenderCall == 180) {
                int n386 = 0, n378 = 0, n370 = 0, n372 = 0, nodes = 0, negDepth = 0;
                std::map<int, int> hist;
                std::function<void(const fvm::DisplayObject&, int)> scan =
                    [&](const fvm::DisplayObject& o, int lv) {
                        ++nodes;
                        if (o.depth < 0) ++negDepth;
                        if (o.characterId >= 360) ++hist[o.characterId];
                        if (o.characterId == 386) ++n386;
                        if (o.characterId == 378) ++n378;
                        if (o.characterId == 370) ++n370;
                        if (o.characterId == 372) ++n372;
                        if (lv > 9) return;
                        for (const auto& c : o.children) if (c) scan(*c, lv + 1);
                    };
                for (const auto& r : movie.runtimeStage) if (r) scan(*r, 0);
                std::fprintf(stderr, "[rtree] call=%d roots=%zu nodes=%d negDepth=%d c386=%d c378=%d c370=%d c372=%d\n",
                             g_dbgRenderCall, movie.runtimeStage.size(), nodes, negDepth, n386, n378, n370, n372);
                for (const auto& kv : hist)
                    std::fprintf(stderr, "[hist] cid=%d n=%d\n", kv.first, kv.second);
            }
        }
        renderRuntimeChildren(movie.runtimeStage, rootM, movie, out.data(), W, H,
                              frameIndex, 1.0f, ColorTransform{});
    }
    return true;
}

// ---------- 命中测试 ----------
// 射线法（even-odd）：点是否在单条轮廓内（twips 局部坐标）。
static bool pointInContour(const Contour& c, double lx, double ly) {
    std::size_t n = c.xs.size();
    if (n < 3) {
        // 退化为线段：距离阈值 3px（60 twips）
        for (std::size_t i = 1; i < n; ++i) {
            double ax = c.xs[i - 1], ay = c.ys[i - 1], bx = c.xs[i], by = c.ys[i];
            double dx = bx - ax, dy = by - ay;
            double len2 = dx * dx + dy * dy;
            double t = len2 > 0 ? ((lx - ax) * dx + (ly - ay) * dy) / len2 : 0.0;
            t = t < 0 ? 0 : (t > 1 ? 1 : t);
            double px = ax + t * dx - lx, py = ay + t * dy - ly;
            if (px * px + py * py <= 60.0 * 60.0) return true;
        }
        return false;
    }
    bool inside = false;
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        double xi = c.xs[i], yi = c.ys[i], xj = c.xs[j], yj = c.ys[j];
        if (((yi > ly) != (yj > ly)) &&
            (lx < (xj - xi) * (ly - yi) / (yj - yi + 1e-12) + xi))
            inside = !inside;
    }
    return inside;
}

// 点击形状（局部 twips 坐标）：任一填充路径命中即可。
static bool hitShape(const Shape& sh, double lx, double ly) {
    for (const auto& group : sh.fillPaths)
        for (const auto& c : group)
            if (pointInContour(c, lx, ly)) return true;
    // 描边形状（无填充）：粗略用描边路径线段距离
    if (sh.fillPaths.empty()) {
        for (const auto& group : sh.strokePaths)
            for (const auto& c : group)
                if (pointInContour(c, lx, ly)) return true;
    }
    return false;
}

// 计算显示项 it 相对父变换 parent 的世界变换（与 hitItem 内部规则一致）。
static Matrix itemWorld(const Matrix& parent, const DisplayItem& it) {
    if (it.hasRt) {
        double rad = it.rtRotation * 3.141592653589793 / 180.0;
        double ca = std::cos(rad), sa = std::sin(rad);
        Matrix rt;
        rt.sx  = static_cast<float>(it.rtScaleX * ca);
        rt.r0  = static_cast<float>(-it.rtScaleX * sa);
        rt.r1  = static_cast<float>(it.rtScaleY * sa);
        rt.sy  = static_cast<float>(it.rtScaleY * ca);
        rt.tx  = static_cast<float>(it.rtX * 20.0);
        rt.ty  = static_cast<float>(it.rtY * 20.0);
        return parent.post(rt);
    }
    return parent.post(it.matrix);
}

// 单个显示项命中测试（root 为根变换；x,y 为设备像素）。
static bool hitItem(const Movie& movie, const DisplayItem& it,
                    const Matrix& root, double x, double y, int frameIndex, int depthGuard) {
    if (!it.character || depthGuard > 16) return false;
    if (it.hasRt && !it.rtVisible) return false;
    Matrix world;
    if (it.hasRt) {
        double rad = it.rtRotation * 3.141592653589793 / 180.0;
        double ca = std::cos(rad), sa = std::sin(rad);
        Matrix rt;
        rt.sx  = static_cast<float>(it.rtScaleX * ca);
        rt.r0  = static_cast<float>(-it.rtScaleX * sa);
        rt.r1  = static_cast<float>(it.rtScaleY * sa);
        rt.sy  = static_cast<float>(it.rtScaleY * ca);
        rt.tx  = static_cast<float>(it.rtX * 20.0);
        rt.ty  = static_cast<float>(it.rtY * 20.0);
        world = root.post(rt);
    } else {
        world = root.post(it.matrix);
    }
    Matrix inv;
    if (!world.invert(inv)) return false;
    float lxf, lyf;
    inv.apply(static_cast<float>(x), static_cast<float>(y), lxf, lyf);
    double lx = lxf, ly = lyf;   // 局部 twips

    const auto& ch = *it.character;
    switch (ch.kind) {
        case Character::Kind::Shape:
            return ch.shape && hitShape(*ch.shape, lx, ly);
        case Character::Kind::Morph:
            return ch.morph && ch.morph->start && hitShape(*ch.morph->start, lx, ly);
        case Character::Kind::Bitmap:
            return ch.bitmap && lx >= 0 && ly >= 0 &&
                   lx <= ch.bitmap->width * 20.0 && ly <= ch.bitmap->height * 20.0;
        case Character::Kind::Video:
            return ch.video && lx >= 0 && ly >= 0 &&
                   lx <= ch.video->width * 20.0 && ly <= ch.video->height * 20.0;
        case Character::Kind::EditText:
            return ch.editText && lx >= ch.editText->bounds.x0 && ly >= ch.editText->bounds.y0 &&
                   lx <= ch.editText->bounds.x1 && ly <= ch.editText->bounds.y1;
        case Character::Kind::Button: {
            if (!ch.button) return false;
            // 优先 Hit 态形状；无则用 Up 态
            const auto& bucket = !ch.button->hit.empty() ? ch.button->hit : ch.button->up;
            for (const auto& sub : bucket)
                if (hitItem(movie, sub, world, x, y, frameIndex, depthGuard + 1)) return true;
            return false;
        }
        case Character::Kind::Sprite: {
            if (!it.children.empty()) {   // 运行态子树（含嵌套精灵）
                for (const auto& sub : it.children)
                    if (hitItem(movie, sub, world, x, y, frameIndex, depthGuard + 1)) return true;
                return false;
            }
            const auto& sf = ch.spriteFrames;
            if (sf.empty()) return false;
            int idx;
            if (it.rtFrame >= 0) {
                idx = it.rtFrame % static_cast<int>(sf.size());
            } else {
                idx = (frameIndex - it.birthFrame) % static_cast<int>(sf.size());
                if (idx < 0) idx = 0;
            }
            for (const auto& sub : sf[static_cast<std::size_t>(idx)].items)
                if (hitItem(movie, sub, world, x, y, frameIndex, depthGuard + 1)) return true;
            return false;
        }
        default:
            return false;
    }
}

int hitTestItems(const Movie& movie, const std::vector<DisplayItem>& items,
                 double x, double y, int frameIndex) {
    Matrix root;
    root.sx = 1.0f / 20.0f; root.sy = 1.0f / 20.0f;
    root.tx = -movie.frameSize.x0 / 20.0f;
    root.ty = -movie.frameSize.y0 / 20.0f;
    // 从最高 depth（列表末尾）向低层扫描，返回最上层命中。
    // 注意：直接放在时间轴上的 Shape/Morph/Bitmap/Video 是"惰性"图形，Flash 不让
    // 它们拦截鼠标，否则覆盖整屏的背景形状会永远压住下方的按钮（swf.swf 主菜单的
    // "开始"按钮就是这样被 depth=187 的整屏形状挡住，导致点击无效）。
    // 只有 Button / MovieClip(Sprite) / EditText 参与鼠标命中。
    for (int i = static_cast<int>(items.size()) - 1; i >= 0; --i) {
        const auto& it = items[static_cast<std::size_t>(i)];
        if (it.clipDepth > 0) continue;   // 遮罩本身不接受点击
        if (!it.character) continue;
        const auto k = it.character->kind;
        if (k != Character::Kind::Button && k != Character::Kind::Sprite &&
            k != Character::Kind::EditText) continue;
        if (hitItem(movie, it, root, x, y, frameIndex, 0))
            return i;
    }
    return -1;
}

// 递归命中搜索：在 items 中（高 depth 优先）找到最上层可交互项，并把自顶向下的链条写入 chain。
static bool hitSearch(const Movie& movie, const std::vector<DisplayItem>& items,
                      const Matrix& parent, double x, double y, int frameIndex,
                      std::vector<const DisplayItem*>& chain, int guard) {
    if (guard > 16) return false;
    for (int i = static_cast<int>(items.size()) - 1; i >= 0; --i) {
        const auto& it = items[static_cast<std::size_t>(i)];
        if (it.clipDepth > 0 || !it.character) continue;
        const auto k = it.character->kind;
        if (k != Character::Kind::Button && k != Character::Kind::Sprite &&
            k != Character::Kind::EditText) continue;
        if (!hitItem(movie, it, parent, x, y, frameIndex, 0)) continue;
        chain.push_back(&it);
        if (k == Character::Kind::Button || k == Character::Kind::EditText) return true;
        // Sprite：深入其内部寻找更高层的可交互项（嵌套 Button 就挂在这里）。
        Matrix world = itemWorld(parent, it);
        const std::vector<DisplayItem>* kids = nullptr;
        if (!it.children.empty()) {
            kids = &it.children;
        } else {
            const auto& sf = it.character->spriteFrames;
            int n = static_cast<int>(sf.size());
            if (n > 0) {
                int idx;
                if (it.rtFrame >= 0) idx = it.rtFrame % n;
                else { idx = (frameIndex - it.birthFrame) % n; if (idx < 0) idx = 0; }
                kids = &sf[static_cast<std::size_t>(idx)].items;
            }
        }
        if (kids && hitSearch(movie, *kids, world, x, y, frameIndex, chain, guard + 1))
            return true;
        return true;   // 精灵本身命中，但其内部没有更上层的可交互项
    }
    return false;
}

bool hitTestItemsDeep(const Movie& movie, const std::vector<DisplayItem>& items,
                      double x, double y, int frameIndex,
                      std::vector<const DisplayItem*>& chain) {
    chain.clear();
    Matrix root;
    root.sx = 1.0f / 20.0f; root.sy = 1.0f / 20.0f;
    root.tx = -movie.frameSize.x0 / 20.0f;
    root.ty = -movie.frameSize.y0 / 20.0f;
    return hitSearch(movie, items, root, x, y, frameIndex, chain, 0);
}

// ---------- AVM2 运行时显示树命中测试 ----------
// 与 renderDisplayObject 完全一致的局部变换（父级 twips -> 设备像素）。
static Matrix runtimeLocalMatrix(const fvm::DisplayObject& d) {
    Matrix Lt;
    if (d.hasFull) {
        Lt.sx = static_cast<float>(d.fullA);
        Lt.r0 = static_cast<float>(d.fullB);
        Lt.r1 = static_cast<float>(d.fullC);
        Lt.sy = static_cast<float>(d.fullD);
    } else {
        double rad = d.rotation * 3.141592653589793 / 180.0;
        double ca = std::cos(rad), sa = std::sin(rad);
        Lt.sx = static_cast<float>(d.scaleX * ca);
        Lt.r0 = static_cast<float>(-d.scaleX * sa);
        Lt.r1 = static_cast<float>(d.scaleY * sa);
        Lt.sy = static_cast<float>(d.scaleY * ca);
    }
    Lt.tx = static_cast<float>(d.x * 20.0);
    Lt.ty = static_cast<float>(d.y * 20.0);
    return Lt;
}

// 对象自身几何是否命中（不递归 children）。world：对象局部 twips -> 设备像素。
static bool runtimeSelfHit(const Movie& movie, const fvm::DisplayObject& d,
                           const Matrix& world, double x, double y) {
    if (!d.characterId) return false;
    auto it = movie.characters.find(d.characterId);
    if (it == movie.characters.end() || !it->second) return false;
    const Character& ch = *it->second;
    Matrix inv;
    if (!world.invert(inv)) return false;
    float lxf = 0, lyf = 0;
    inv.apply(static_cast<float>(x), static_cast<float>(y), lxf, lyf);
    double lx = lxf, ly = lyf;   // 对象局部 twips
    switch (ch.kind) {
        case Character::Kind::Shape:
            return ch.shape && hitShape(*ch.shape, lx, ly);
        case Character::Kind::Morph:
            return ch.morph && ch.morph->start && hitShape(*ch.morph->start, lx, ly);
        case Character::Kind::Bitmap:
            return ch.bitmap && lx >= 0 && ly >= 0 &&
                   lx <= ch.bitmap->width * 20.0 && ly <= ch.bitmap->height * 20.0;
        case Character::Kind::Video:
            return ch.video && lx >= 0 && ly >= 0 &&
                   lx <= ch.video->width * 20.0 && ly <= ch.video->height * 20.0;
        case Character::Kind::EditText:
            return ch.editText && lx >= ch.editText->bounds.x0 && ly >= ch.editText->bounds.y0 &&
                   lx <= ch.editText->bounds.x1 && ly <= ch.editText->bounds.y1;
        case Character::Kind::Button: {
            if (!ch.button) return false;
            const auto& bucket = !ch.button->hit.empty() ? ch.button->hit : ch.button->up;
            for (const auto& sub : bucket)
                if (hitItem(movie, sub, world, x, y, 0, 0)) return true;
            return false;
        }
        case Character::Kind::Sprite: {
            // 时间轴已重建为运行时子对象时由 children 递归负责；否则退回静态帧
            if (d.timelineBuilt) return false;
            const auto& sf = ch.spriteFrames;
            if (sf.empty()) return false;
            int idx = d.currentFrame;
            if (idx < 0 || idx >= static_cast<int>(sf.size())) idx = 0;
            for (const auto& sub : sf[static_cast<std::size_t>(idx)].items)
                if (hitItem(movie, sub, world, x, y, 0, 0)) return true;
            return false;
        }
        default:
            return false;
    }
}

// 是否为可交互对象（InteractiveObject）。Shape/Bitmap/Video/静态文本不能成为鼠标事件目标，
// 也不拦截命中（鼠标会继续落到其下层的兄弟对象上）。
static bool runtimeInteractive(const Movie& movie, const fvm::DisplayObject& d) {
    if (!d.characterId) return true;   // 根时间轴（Stage/MainTimeline）
    auto it = movie.characters.find(d.characterId);
    if (it != movie.characters.end() && it->second) {
        switch (it->second->kind) {
            case Character::Kind::Sprite:
            case Character::Kind::Button:
            case Character::Kind::EditText:
                return true;
            case Character::Kind::Shape:
            case Character::Kind::Bitmap:
            case Character::Kind::Video:
            case Character::Kind::Morph:
            case Character::Kind::Text:
            case Character::Kind::Font:
                return false;
        }
    }
    switch (d.kind) {
        case fvm::ObjKind::Sprite:
        case fvm::ObjKind::MovieClip:
        case fvm::ObjKind::Stage:
        case fvm::ObjKind::TextField:
        case fvm::ObjKind::Loader:
        case fvm::ObjKind::InteractiveObject:
        case fvm::ObjKind::DisplayObjectContainer:
            return true;
        default:
            return false;
    }
}

// 对象自身或其后代几何是否覆盖该点（不考虑可交互性，world 为 d 局部 twips -> 设备像素）。
// Flash 语义：alpha 为 0 的对象仍然参与命中（只有 visible=false 才不参与）。
static bool runtimeGeometryHit(const Movie& movie, const std::shared_ptr<fvm::DisplayObject>& d,
                               const Matrix& world, double x, double y, int guard) {
    if (!d || !d->visible || guard > 32) return false;
    if (runtimeSelfHit(movie, *d, world, x, y)) return true;
    for (int i = static_cast<int>(d->children.size()) - 1; i >= 0; --i) {
        const auto& c = d->children[static_cast<std::size_t>(i)];
        if (!c) continue;
        Matrix cw = world.post(runtimeLocalMatrix(*c));
        if (runtimeGeometryHit(movie, c, cw, x, y, guard + 1)) return true;
    }
    return false;
}

// 递归查找鼠标事件目标：后加入的显示对象在上层，因此 children 逆序扫描。
// 只有可交互对象能成为目标；其命中区域包含全部（含非交互）子对象的几何。
static bool runtimeHitObject(const Movie& movie,
                             const std::shared_ptr<fvm::DisplayObject>& d,
                             const Matrix& parent, double x, double y,
                             std::vector<std::shared_ptr<fvm::DisplayObject>>& chain,
                             int guard) {
    if (!d || !d->visible || guard > 32) return false;
    Matrix world = parent.post(runtimeLocalMatrix(*d));
    // mouseChildren=false：不再把子对象作为独立目标（父对象整体作为一个目标）
    if (d->mouseChildren) {
        for (int i = static_cast<int>(d->children.size()) - 1; i >= 0; --i) {
            if (runtimeHitObject(movie, d->children[static_cast<std::size_t>(i)], world, x, y, chain, guard + 1)) {
                chain.insert(chain.begin(), d);
                return true;
            }
        }
    }
    // mouseEnabled=false：自身不接收鼠标事件（命中穿透到下层）
    if (d->mouseEnabled && runtimeInteractive(movie, *d) && runtimeGeometryHit(movie, d, world, x, y, guard)) {
        chain.push_back(d);
        return true;
    }
    return false;
}

bool hitTestRuntimeStage(const Movie& movie,
                         const std::vector<std::shared_ptr<fvm::DisplayObject>>& roots,
                         double x, double y,
                         std::vector<std::shared_ptr<fvm::DisplayObject>>* chain) {
    if (chain) chain->clear();
    Matrix root;
    root.sx = 1.0f / 20.0f; root.sy = 1.0f / 20.0f;
    root.tx = -movie.frameSize.x0 / 20.0f;
    root.ty = -movie.frameSize.y0 / 20.0f;
    std::vector<std::shared_ptr<fvm::DisplayObject>> tmp;
    for (int i = static_cast<int>(roots.size()) - 1; i >= 0; --i) {
        if (runtimeHitObject(movie, roots[static_cast<std::size_t>(i)], root, x, y, tmp, 0)) {
            if (chain) *chain = std::move(tmp);
            return true;
        }
        tmp.clear();
    }
    return false;
}

// 运行时对象的完整世界变换：对象局部 twips -> 舞台像素（与鼠标命中测试一致）。
static Matrix runtimeWorldMatrix(const std::shared_ptr<fvm::DisplayObject>& d, const Movie& movie) {
    std::vector<const fvm::DisplayObject*> chain;
    for (auto c = d; c; c = c->parent) chain.push_back(c.get());
    Matrix world;
    world.sx = 1.0f / 20.0f; world.sy = 1.0f / 20.0f;
    world.tx = -movie.frameSize.x0 / 20.0f;
    world.ty = -movie.frameSize.y0 / 20.0f;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        world = world.post(runtimeLocalMatrix(**it));
    return world;
}

// 对象（含后代）的几何包围盒，设备像素；用于 hitTestPoint(shapeFlag=false)。
static void runtimeBoundsAccum(const Movie& movie, const std::shared_ptr<fvm::DisplayObject>& d,
                               const Matrix& world, bool& any,
                               float& x0, float& y0, float& x1, float& y1) {
    auto expand = [&](const RectB& b) {
        const float xs[4] = {static_cast<float>(b.x0), static_cast<float>(b.x1),
                             static_cast<float>(b.x1), static_cast<float>(b.x0)};
        const float ys[4] = {static_cast<float>(b.y0), static_cast<float>(b.y0),
                             static_cast<float>(b.y1), static_cast<float>(b.y1)};
        for (int i = 0; i < 4; ++i) {
            float px = 0, py = 0;
            world.apply(xs[i], ys[i], px, py);
            if (!any) { x0 = x1 = px; y0 = y1 = py; any = true; }
            else {
                if (px < x0) x0 = px; if (px > x1) x1 = px;
                if (py < y0) y0 = py; if (py > y1) y1 = py;
            }
        }
    };
    if (d && d->characterId) {
        auto it = movie.characters.find(d->characterId);
        if (it != movie.characters.end() && it->second) {
            const Character& ch = *it->second;
            if (ch.kind == Character::Kind::Shape && ch.shape) {
                expand(ch.shape->bounds);
            } else if (ch.kind == Character::Kind::Bitmap && ch.bitmap) {
                RectB b; b.x1 = static_cast<int>(ch.bitmap->width * 20);
                b.y1 = static_cast<int>(ch.bitmap->height * 20);
                expand(b);
            } else if (ch.kind == Character::Kind::EditText && ch.editText) {
                expand(ch.editText->bounds);
            }
        }
    }
    if (d) {
        for (auto& c : d->children) {
            if (!c) continue;
            runtimeBoundsAccum(movie, c, world.post(runtimeLocalMatrix(*c)), any, x0, y0, x1, y1);
        }
    }
}

bool hitTestRuntimeObject(const Movie& movie, const std::shared_ptr<fvm::DisplayObject>& d,
                          double x, double y, bool shapeFlag) {
    if (!d) return false;
    Matrix world = runtimeWorldMatrix(d, movie);
    if (shapeFlag) return runtimeGeometryHit(movie, d, world, x, y, 0);
    bool any = false;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    runtimeBoundsAccum(movie, d, world, any, x0, y0, x1, y1);
    if (!any) return false;
    return x >= x0 && x <= x1 && y >= y0 && y <= y1;
}

}  // namespace swf
