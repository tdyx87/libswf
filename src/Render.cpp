// 软件光栅器实现：扫描线填充（even-odd）、纯色/渐变/位图填充、线描边、over 合成。
#include "swf/Render.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
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
    const Bitmap* bmp = nullptr;
    Matrix bmat;                // 形状局部 -> 位图像素
    ColorTransform cx;
    // DefineShape4 的 UsesFillWindingRule：为 true 时该形状按非零环绕填充
    bool winding = false;
};

Color sampleGrad(const std::vector<GradientStop>& g, float t) {
    if (g.empty()) return Color(0, 0, 0, 255);
    if (t <= g[0].ratio / 255.0f) return g[0].color;
    if (t >= g.back().ratio / 255.0f) return g.back().color;
    for (std::size_t i = 1; i < g.size(); ++i) {
        if (t <= g[i].ratio / 255.0f) {
            float r0 = g[i - 1].ratio / 255.0f, r1 = g[i].ratio / 255.0f;
            float f = (r1 - r0) > 1e-6f ? (t - r0) / (r1 - r0) : 0.0f;
            const Color& a = g[i - 1].color; const Color& b = g[i].color;
            return Color(clampU8(static_cast<int>(lerp(a.r, b.r, f))),
                         clampU8(static_cast<int>(lerp(a.g, b.g, f))),
                         clampU8(static_cast<int>(lerp(a.b, b.b, f))),
                         clampU8(static_cast<int>(lerp(a.a, b.a, f))));
        }
    }
    return g.back().color;
}

Color paintColor(const Paint& p, float lx, float ly) {
    // SWF 渐变空间：以渐变中心为原点、半径/半程 = 16384 twips（819.2px）的正方形。
    // 线性渐变沿 x 轴从 -16384 到 +16384；径向渐变为半径 16384 的圆。
    static const float GRAD_UNIT = [] {
        const char* e = std::getenv("SWF_GRAD_UNIT");
        return e ? static_cast<float>(std::atof(e)) : 16384.0f;
    }();
    const float G = GRAD_UNIT;
    Color c = p.solid;
    if (p.kind == FillKind::LinearGradient) {
        float gx, gy; p.gmat.apply(lx, ly, gx, gy);
        c = sampleGrad(p.grad, std::max(0.0f, std::min(1.0f, (gx + G) / (2.0f * G))));
    } else if (p.kind == FillKind::RadialGradient) {
        float gx, gy; p.gmat.apply(lx, ly, gx, gy);
        float r = std::sqrt(gx * gx + gy * gy) / G;
        c = sampleGrad(p.grad, std::max(0.0f, std::min(1.0f, r)));
    } else if (p.kind == FillKind::FocalGradient) {
        // 焦点渐变：渐变自焦点起、至渐变圆周止。归一化到半径 R=1 的圆内。
        float gx, gy; p.gmat.apply(lx, ly, gx, gy);
        gx /= G; gy /= G;                       // 归一化到半径 1
        const float fx = p.focalX, fy = p.focalY;
        const float fm = std::sqrt(fx * fx + fy * fy);
        float t;
        if (fm < 1e-6f) {
            t = std::sqrt(gx * gx + gy * gy);   // 无焦点偏移 == 径向
        } else {
            // 从焦点 f 沿方向 dir 到单位圆周的距离
            float vx = gx - fx, vy = gy - fy;
            float d = std::sqrt(vx * vx + vy * vy);
            if (d < 1e-6f) {
                t = 0.0f;                       // 正处焦点
            } else {
                float dx = vx / d, dy = vy / d; // 方向
                float fd = fx * dx + fy * dy;   // dot(f, dir)
                // 解 |f + s*dir| = 1 取正根 s = -fd + sqrt(fd^2 + 1 - |f|^2)
                float disc = fd * fd + 1.0f - fm * fm;
                float s = -fd + std::sqrt(disc > 0.0f ? disc : 0.0f);
                t = (s > 1e-6f) ? (d / s) : d;
            }
        }
        c = sampleGrad(p.grad, std::max(0.0f, std::min(1.0f, t)));
    } else if (p.kind == FillKind::Bitmap && p.bmp && p.bmp->width) {
        float u, v; p.bmat.apply(lx, ly, u, v);
        int w = p.bmp->width, h = p.bmp->height;
        if (p.tiled) {
            u = std::fmod(u, static_cast<float>(w));
            v = std::fmod(v, static_cast<float>(h));
            if (u < 0) u += w; if (v < 0) v += h;
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
    float ymin = 1e9f, ymax = -1e9f;
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
    std::vector<float> xs;
    std::vector<float> solidCov(static_cast<std::size_t>(W), 0.0f);  // 实体轮廓并集
    std::vector<float> holeCov(static_cast<std::size_t>(W), 0.0f);   // 洞轮廓并集
    std::vector<float> cov(static_cast<std::size_t>(W), 0.0f);       // 单个轮廓
    for (int y = y0; y <= y1; ++y) {
        std::fill(solidCov.begin(), solidCov.end(), 0.0f);
        std::fill(holeCov.begin(), holeCov.end(), 0.0f);
        for (const auto& p : polys) {
            // 该行两条子扫描线都不与轮廓 y 范围相交则跳过
            if (p.ymax <= y + 0.25f || p.ymin >= y + 0.75f) continue;
            std::fill(cov.begin(), cov.end(), 0.0f);
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
                    int xa = std::max(0, static_cast<int>(std::floor(A - 0.5f)));
                    int xb = std::min(W - 1, static_cast<int>(std::ceil(B + 0.5f)));
                    for (int x = xa; x <= xb; ++x) {
                        float l = std::max(A, static_cast<float>(x) - 0.5f);
                        float r = std::min(B, static_cast<float>(x) + 0.5f);
                        if (r > l) cov[x] += 0.5f * (r - l);
                    }
                }
            }
            std::vector<float>& dst = (p.depth & 1) ? holeCov : solidCov;
            for (int x = 0; x < W; ++x)
                if (cov[x] > dst[x]) dst[x] = cov[x];
        }
        // 用像素中心采样上色，按覆盖率调制 alpha 后合成
        for (int x = 0; x < W; ++x) {
            float cover = solidCov[x] - holeCov[x];
            if (cover <= 0.0f) continue;
            if (cover > 1.0f) cover = 1.0f;
            float lx, ly; Minv.apply(x + 0.5f, y + 0.5f, lx, ly);
            Color c = paintColor(paint, lx, ly);
            c.a = clampU8(static_cast<int>(c.a * alphaMul + 0.5f));   // 对象级透明度
            if (cover < 1.0f) {
                float a = (c.a / 255.0f) * cover;
                c.a = clampU8(static_cast<int>(a * 255.0f + 0.5f));
                if (c.a == 0) continue;
            }
            plot(buf, W, H, x, y, c, bm);
        }
    }
}

// 非零环绕（non-zero winding）扫描线填充：每条边带符号（+1 = 该样式在边右侧，
// -1 = 在边左侧）。扫描线从左到右累加环绕数，环绕数 != 0 的区间即为该样式的区域。
// 与"嵌套=洞 / 部分重叠=并集"启发式相比，此规则天然同时正确处理两种情况：
//   同向重叠轮廓 -> 环绕数累加（并集）；反向嵌套轮廓 -> 环绕数抵消（洞）。
struct DEdge { float x0, y0, x1, y1; int sign; };

// 对一组设备坐标边，计算扫描线行 y 的覆盖率（2 条子扫描线，水平抗锯齿）。
// rule: 0 = 带符号非零环绕（默认）；1 = 偶奇；2 = 无符号并集（忽略侧号）。
static void edgesCoverageRow(const std::vector<DEdge>& es, int y, int W, float* cov, int rule,
                             std::vector<std::pair<float, int>>& xs) {
    std::fill(cov, cov + W, 0.0f);
    for (int sub = 0; sub < 2; ++sub) {          // 2x2 超采样（水平抗锯齿）
        float yy = y + (sub == 0 ? 0.25f : 0.75f);
        xs.clear();
        for (const auto& e : es) {
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
            int xa = std::max(0, static_cast<int>(std::floor(A - 0.5f)));
            int xb = std::min(W - 1, static_cast<int>(std::ceil(B + 0.5f)));
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
    std::vector<E> es;
    es.reserve(edges.size());
    float ymin = 1e9f, ymax = -1e9f;
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
    }
    if (es.empty()) return;

    if (std::getenv("SWF_PRINT_ITEMS")) {
        float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
        const E* emin = nullptr; float eminx = 1e9f;
        for (const auto& e : es) {
            bx0 = std::min(bx0, std::min(e.x0, e.x1)); bx1 = std::max(bx1, std::max(e.x0, e.x1));
            by0 = std::min(by0, std::min(e.y0, e.y1)); by1 = std::max(by1, std::max(e.y0, e.y1));
            if (std::min(e.x0, e.x1) < eminx) { eminx = std::min(e.x0, e.x1); emin = &e; }
        }
        std::printf("fillEdgesWinding n=%zu dev bbox x %.1f..%.1f y %.1f..%.1f  e0=(%.1f,%.1f)->(%.1f,%.1f) minx e=(%.2f,%.2f)->(%.2f,%.2f)\n",
                    es.size(), bx0, bx1, by0, by1, es[0].x0, es[0].y0, es[0].x1, es[0].y1,
                    emin->x0, emin->y0, emin->x1, emin->y1);
    }

    int yy0 = std::max(0, static_cast<int>(std::floor(ymin)));
    int yy1 = std::min(H - 1, static_cast<int>(std::ceil(ymax)));
    std::vector<std::pair<float, int>> xs;
    std::vector<float> cov(static_cast<std::size_t>(W), 0.0f);
    static const bool evenOddB = std::getenv("SWF_EVENODD") != nullptr;
    static const bool noSignB  = std::getenv("SWF_NO_SIGN") != nullptr;
    const int rule = noSignB ? 2 : (evenOddB ? 1 : 0);

    for (int y = yy0; y <= yy1; ++y) {
        edgesCoverageRow(es, y, W, cov.data(), rule, xs);
        for (int x = 0; x < W; ++x) {
            float cover = cov[x];
            if (cover <= 0.0f) continue;
            if (cover > 1.0f) cover = 1.0f;
            float lx, ly; Minv.apply(x + 0.5f, y + 0.5f, lx, ly);
            Color c = paintColor(paint, lx, ly);
            c.a = clampU8(static_cast<int>(c.a * alphaMul + 0.5f));
            if (cover < 1.0f) {
                float a = (c.a / 255.0f) * cover;
                c.a = clampU8(static_cast<int>(a * 255.0f + 0.5f));
                if (c.a == 0) continue;
            }
            plot(buf, W, H, x, y, c, bm);
        }
    }
}

// 光栅化形状的第 i 个填充样式：优先使用有向边 + 非零环绕（权威几何）；
// 缺边时回退到轮廓 + 启发式（用于变形插值等没有边数据的场合）。
void fillStyleOf(const Shape& shape, std::size_t i, const Matrix& M, const Matrix& Minv,
                 const Paint& p, std::uint8_t* buf, int W, int H, float alphaMul, BlendMode bm) {
    static const bool noEdges = std::getenv("SWF_NO_EDGES") != nullptr;
    if (!noEdges && i < shape.fillEdges.size() && !shape.fillEdges[i].empty()) {
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
                             float alphaMul, BlendMode bm) {
    float dx = bxp - axp, dy = byp - ayp;
    float len2 = dx * dx + dy * dy;
    int x0 = std::max(0, static_cast<int>(std::floor(std::min(axp, bxp) - hw - 1)));
    int x1 = std::min(W - 1, static_cast<int>(std::ceil(std::max(axp, bxp) + hw + 1)));
    int y0 = std::max(0, static_cast<int>(std::floor(std::min(ayp, byp) - hw - 1)));
    int y1 = std::min(H - 1, static_cast<int>(std::ceil(std::max(ayp, byp) + hw + 1)));
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            float px = x + 0.5f, py = y + 0.5f;
            float t = 0.0f;
            if (len2 > 1e-9f) {
                t = ((px - axp) * dx + (py - ayp) * dy) / len2;
                if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
            }
            float cx = axp + t * dx, cy = ayp + t * dy;
            float ddx = px - cx, ddy = py - cy;
            float d = std::sqrt(ddx * ddx + ddy * ddy);
            float cover = hw + 0.5f - d;      // 半宽 + 半像素做简单抗锯齿
            if (cover <= 0.0f) continue;
            if (cover > 1.0f) cover = 1.0f;
            Color c = color;
            c.a = clampU8(static_cast<int>(c.a * alphaMul + 0.5f));
            float a = (c.a / 255.0f) * cover;
            c.a = clampU8(static_cast<int>(a * 255.0f + 0.5f));
            if (c.a == 0) continue;
            plot(buf, W, H, x, y, c, bm);
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
    for (const auto& ct : contours) {
        int n = static_cast<int>(ct.xs.size());
        for (int i = 0; i < n - 1; ++i) {
            float axp, ayp, bxp, byp;
            M.apply(ct.xs[i], ct.ys[i], axp, ayp);
            M.apply(ct.xs[i + 1], ct.ys[i + 1], bxp, byp);
            drawThickSegment(axp, ayp, bxp, byp, hw, color, buf, W, H, alphaMul, bm);
        }
    }
}

void renderShape(const Shape& shape, const Matrix& world, const ColorTransform& cx,
                 const std::map<std::uint16_t, std::shared_ptr<Character>>& dict,
                 std::uint8_t* buf, int W, int H, float alphaMul = 1.0f,
                 BlendMode bm = BlendMode::Normal) {
    Matrix Minv; world.invert(Minv);
    if (false && std::getenv("SWF_PRINT_ITEMS") && !shape.fillEdges.empty() && !shape.fillEdges[0].empty()) {
        const auto& fe = shape.fillEdges[0];
        std::printf("renderShape fillEdges[0] n=%zu e0=(%.2f,%.2f)->(%.2f,%.2f s%d) e1=(%.2f,%.2f)->(%.2f,%.2f s%d)\n",
                    fe.size(), fe[0].x0, fe[0].y0, fe[0].x1, fe[0].y1, fe[0].sign,
                    fe[1].x0, fe[1].y0, fe[1].x1, fe[1].y1, fe[1].sign);
    }
    // 填充
    const char* onlyFillEnv = std::getenv("SWF_ONLY_FILL");
    int onlyFill = onlyFillEnv ? std::atoi(onlyFillEnv) : -1;
    const char* maxFillEnv = std::getenv("SWF_MAX_FILL");
    int maxFill = maxFillEnv ? std::atoi(maxFillEnv) : 0x7fffffff;
    const char* skipFillEnv = std::getenv("SWF_SKIP_FILL");
    int skipFill = skipFillEnv ? std::atoi(skipFillEnv) : -1;
    auto drawFill = [&](std::size_t i) {
        if (onlyFill == -2) return;   // -2 = 只画描边（调试）
        if (static_cast<int>(i) > maxFill) return;   // 累积渲染 0..N（调试 z 序）
        if (static_cast<int>(i) == skipFill) return;   // 跳过指定索引（调试）
        if (onlyFill >= 0 && static_cast<int>(i) != onlyFill) return;
        if (i >= shape.fillStyles.size()) return;
        if (i >= shape.fillEdges.size() && i >= shape.fillPaths.size()) return;
        const FillStyle& fs = shape.fillStyles[i];
        Paint p;
        p.kind = fs.kind; p.solid = fs.color; p.cx = cx; p.tiled = fs.tiled;
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
        if (onlyFill >= 0 || onlyFill == -3) return;   // >=0 = 只画单个 fill；-3 = 只画填充
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
    s.bounds = m.start->bounds;
    s.fillStyles = m.start->fillStyles;
    s.lineStyles = m.start->lineStyles;
    s.drawOrder = m.start->drawOrder;
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
    static HDC sdc = CreateCompatibleDC(nullptr);
    static std::map<std::string, HFONT> fonts;
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
            int em = 2048;
            HFONT tmp = makeFont(wfam, 1024, bold);
            if (tmp) {
                HGDIOBJ old = SelectObject(sdc, tmp);
                OUTLINETEXTMETRICW otm = {};
                if (GetOutlineTextMetricsW(sdc, sizeof(otm), &otm) &&
                    otm.otmEMSquare > 0 && otm.otmEMSquare <= 16384)
                    em = static_cast<int>(otm.otmEMSquare);
                SelectObject(sdc, old);
                DeleteObject(tmp);
            }
            hf = makeFont(wfam, em, bold);
            fonts[styleKey] = hf;
            fonts[styleKey + "#em"] = reinterpret_cast<HFONT>(static_cast<std::intptr_t>(em));
        } else {
            hf = fit->second;
        }
        int em = 2048;
        auto eit = fonts.find(styleKey + "#em");
        if (eit != fonts.end()) em = static_cast<int>(reinterpret_cast<std::intptr_t>(eit->second));
        const float toEm = 1024.0f / static_cast<float>(em);

        if (hf) {
            HGDIOBJ old = SelectObject(sdc, hf);
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
            SelectObject(sdc, old);
        }
    }
    // 合成斜体：轮廓坐标为 em=1024、y 已向下取反（顶部为负 y），
    // 斜体要求顶部右移，故 x' = x - k*y（k = tan(12°)，与 Flash 的合成斜体角度一致）。
    if (italic) {
        constexpr float kShear = 0.207f;
        for (auto& ct : g.contours)
            for (std::size_t i = 0; i < ct.xs.size(); ++i)
                ct.xs[i] -= kShear * ct.ys[i];
    }
    it = cache.emplace(key, std::move(g)).first;
    return it->second;
}
#endif  // _WIN32

// 渲染时间轴文本框（DefineEditText）：用字段自带字体/颜色/边界排版 InitialText。
// 字形为 em(1024) 设计单位，缩放到 fontHeight(twips)；基线置于边界顶部 + ascent。
static void renderEditText(const EditText& et, const Matrix& world,
                           const std::map<std::uint16_t, std::shared_ptr<Character>>& dict,
                           std::uint8_t* buf, int W, int H, float alpha) {
    const FontGlyphs* fg = nullptr;
    auto fit = dict.find(et.fontId);
    if (fit != dict.end() && fit->second && fit->second->font) fg = fit->second->font.get();
    if (!fg) {
        for (const auto& kv : dict)
            if (kv.second && kv.second->kind == Character::Kind::Font && kv.second->font) { fg = kv.second->font.get(); break; }
    }
    if (!fg || et.text.empty()) return;
    float s = static_cast<float>(et.fontHeight) / 1024.0f;   // em -> twips
    if (s <= 0.0f) s = 240.0f / 1024.0f;
    Paint p; p.kind = FillKind::Solid; p.solid = et.color;
    float lineH = static_cast<float>(et.fontHeight) * 1.2f;
    float ascent = static_cast<float>(et.fontHeight) * 0.75f;
    float maxW = static_cast<float>(et.bounds.x1 - et.bounds.x0);
    auto advEm = [&](const Shape& sh) {
        float w = 0; for (const auto& ct : sh.fillPaths) for (const auto& c : ct) for (float x : c.xs) w = std::max(w, x); return w;
    };
    auto glyphFor = [&](std::uint32_t ch) -> const Shape* {
        auto it = fg->codeToGlyph.find(static_cast<int>(ch));
        if (it == fg->codeToGlyph.end()) return nullptr;
        int gi = it->second;
        if (gi < 0 || gi >= static_cast<int>(fg->shapes.size())) return nullptr;
        return fg->shapes[gi].get();
    };
    float penX = 0.0f, penY = 0.0f;   // 相对边界原点（twips）
    for (std::uint32_t ch : utf8Codes(et.text)) {
        if (ch == '\r') continue;
        if (ch == '\n') { penY += lineH; penX = 0.0f; continue; }
        std::uint32_t draw = et.password ? '*' : ch;
        const Shape* gs = glyphFor(draw);
#ifdef _WIN32
        const DeviceGlyph* dg = nullptr;
#endif
        float wEm;
        if (gs) {
            wEm = advEm(*gs);
        } else {
#ifdef _WIN32
            // 嵌入字体缺少该字形 -> 回退到系统字体（Flash 的字体替换 / 设备字体）
            dg = &deviceGlyph(fg->name, draw, fg->italic, fg->bold);
            wEm = dg->advanceEm;
#else
            wEm = 0.0f;
#endif
        }
        float adv = (wEm > 0.0f ? wEm : 512.0f) * s;
        if ((et.wordWrap || et.multiline) && maxW > 0.0f && penX > 0.0f && penX + adv > maxW) {
            penY += lineH; penX = 0.0f;
        }
        float lx = static_cast<float>(et.bounds.x0) + penX;
        float ly = static_cast<float>(et.bounds.y0) + penY + ascent;
        if (gs) {
            // 嵌入字形：字体带 Italic 标志时对字形做同样的合成斜切（x' = x - k*y）。
            Matrix st; st.sx = s; st.sy = s; st.tx = lx; st.ty = ly;
            if (fg->italic) st.r0 = -0.207f * s;
            Matrix gm = world.post(st);
            Matrix Minv; gm.invert(Minv);
            for (std::size_t i = 0; i < gs->fillStyles.size(); ++i) {
                fillStyleOf(*gs, i, gm, Minv, p, buf, W, H, alpha, BlendMode::Normal);
            }
#ifdef _WIN32
        } else if (dg && dg->found) {
            // 系统字体轮廓的 y 轴已在 deviceGlyph 内取反（成 y 向下），故 sy 取正。
            Matrix st; st.sx = s; st.sy = s; st.tx = lx; st.ty = ly;
            Matrix gm = world.post(st);
            Matrix Minv; gm.invert(Minv);
            fillContours(dg->contours, gm, Minv, p, buf, W, H, alpha, BlendMode::Normal);
#endif
        }
        penX += adv;
    }
}

// 渲染一组显示列表项并处理 clipDepth 裁剪组（主时间轴与精灵内部时间轴通用）。
void renderItemsWithClip(const std::vector<DisplayItem>& items, const Matrix& root,
                         const std::map<std::uint16_t, std::shared_ptr<Character>>& dict,
                         std::uint8_t* buf, int W, int H, int frameIndex,
                         std::size_t npix, float alpha);

// 把单个显示列表项（shape / bitmap / sprite / morph / text / edittext / button）渲染进 buf。
// alpha：累积的对象级透明度；bm：混合模式（运行时 DisplayObject.blend / PlaceObject3 blend）。
// 运行时覆盖（AVM1 SetProperty / EditText 输入）：item.hasRt 时以 rt* 字段合成矩阵/透明度。
static int g_dbgDepth = 0;   // 调试缩进
struct DbgDepthGuard { DbgDepthGuard() { ++g_dbgDepth; } ~DbgDepthGuard() { --g_dbgDepth; } };
void renderItemInto(const DisplayItem& item, const Matrix& root,
                    const std::map<std::uint16_t, std::shared_ptr<Character>>& dict,
                    std::uint8_t* buf, int W, int H, int frameIndex, float alpha = 1.0f,
                    BlendMode bm = BlendMode::Normal) {
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
    if (const char* hc = std::getenv("SWF_HIDE_CID")) {   // 调试：隐藏指定 cid 以定位来源
        if (static_cast<int>(item.characterId) == std::atoi(hc)) {
            const char* hd = std::getenv("SWF_HIDE_DEPTH");
            if (!hd || static_cast<int>(item.depth) == std::atoi(hd)) return;
        }
    }
    if (std::getenv("SWF_PRINT_ITEMS"))   // 调试：递归打印每一项（含精灵内部）
        std::printf("%*sitem cid=%u depth=%u kind=%d frame=%d name=%s children=%zu rtFrame=%d rtPlaying=%d spInit=%d stopped=%d clipDepth=%u mask=%d alpha=%.3f cx_a=%.3f pos=(%.1f,%.1f) sc=(%.3f,%.3f)\n",
                    g_dbgDepth * 2, "", item.characterId,
                    item.depth, static_cast<int>(item.character->kind), frameIndex, item.name.c_str(),
                    item.children.size(), item.rtFrame, (int)item.rtPlaying, (int)item.spInit, (int)item.spStopped,
                    item.clipDepth, (int)item.hasMask, item.hasRt ? item.rtAlpha : 1.0,
                    item.cxform.hasMult ? item.cxform.ma : 1.0,
                    world.tx / 20.0f, world.ty / 20.0f, world.sx, world.sy);
    DbgDepthGuard dbgGuard;
    if (item.character->kind == Character::Kind::Shape && item.character->shape) {
        renderShape(*item.character->shape, world, item.cxform, dict, buf, W, H, alpha, bm);
    } else if (item.character->kind == Character::Kind::Morph && item.character->morph) {
        // 变形形状：按当前 ratio 插值后渲染
        Shape interp = interpolateMorph(*item.character->morph, item.ratio);
        renderShape(interp, world, item.cxform, dict, buf, W, H, alpha, bm);
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
        for (const auto* sub : order) renderItemInto(*sub, world, dict, buf, W, H, frameIndex, alpha, bm);
    } else if (item.character->kind == Character::Kind::Sprite) {
        // 精灵：递归渲染其运行态内部帧子树（children 由 Player 每帧同步；支持嵌套精灵、
        // 嵌套按钮与各自的独立播放头/按钮态）。
        const auto& sf = item.character->spriteFrames;
        std::size_t npix = static_cast<std::size_t>(W) * H;
        if (!item.children.empty()) {
            // 用带 clipDepth 处理的渲染器渲染精灵内部帧（支持精灵内遮罩）。
            renderItemsWithClip(item.children, world, dict, buf, W, H, frameIndex, npix, alpha);
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
        if (const char* ff = std::getenv("SWF_SPRITE_FRAME")) {   // 调试：强制所有精灵的内部帧
            idx = std::atoi(ff) % static_cast<int>(sf.size());
            if (idx < 0) idx += static_cast<int>(sf.size());
        }
        renderItemsWithClip(sf[static_cast<std::size_t>(idx)].items, world, dict, buf, W, H, idx, npix, alpha);
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
            // 字形轮廓是字体设计单位（em = 1024）；本 run 的字号（twips）由 TEXTRECORD
            // 的 FontHeight 给出，先缩放到 twips（fontHeight/1024）再经文本矩阵映射到设备。
            float gs_ = (tg.fontHeight > 0) ? static_cast<float>(tg.fontHeight) / 1024.0f : 1.0f;
            // 字形缩放作用于**整个线性部分**（含旋转/斜切分量 r0/r1），平移则取笔位经
            // 文本矩阵映射后的设备坐标，等价于 tm ∘ T(x,y) ∘ Scale(gs_)。
            Matrix gm = tm; gm.sx *= gs_; gm.sy *= gs_; gm.r0 *= gs_; gm.r1 *= gs_;
            // 字体带 Italic 标志：在字形空间叠加合成斜切（x' = x - k*y，再经文本矩阵）。
            if (fg.italic) gm.r0 = (tm.r0 - 0.207f * tm.sx) * gs_;
            gm.tx = gx; gm.ty = gy;
            Matrix Minv; gm.invert(Minv);
            Paint p; p.kind = FillKind::Solid; p.solid = tg.color;
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
                Color col = item.cxform.apply(Color(p4[0], p4[1], p4[2], p4[3]));
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
                    Color col = item.cxform.apply(Color(p4[0], p4[1], p4[2], p4[3]));
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
// 可分离盒式模糊（RGBA）
static void blurBuffer(std::uint8_t* buf, int W, int H, int rx, int ry, int passes) {
    if (rx < 1 && ry < 1) return;
    std::vector<std::uint8_t> tmp(static_cast<std::size_t>(W) * H * 4);
    for (int p = 0; p < passes; ++p) {
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
            int r=0,g=0,b=0,a=0,n=0;
            for (int k=-rx;k<=rx;++k){int xx=x+k; if(xx<0||xx>=W)continue; const std::uint8_t* q=&buf[(static_cast<std::size_t>(y)*W+xx)*4]; r+=q[0];g+=q[1];b+=q[2];a+=q[3];++n;}
            std::uint8_t* t=&tmp[(static_cast<std::size_t>(y)*W+x)*4]; if(n){t[0]=r/n;t[1]=g/n;t[2]=b/n;t[3]=a/n;}
        }
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
            int r=0,g=0,b=0,a=0,n=0;
            for (int k=-ry;k<=ry;++k){int yy=y+k; if(yy<0||yy>=H)continue; const std::uint8_t* q=&tmp[(static_cast<std::size_t>(yy)*W+x)*4]; r+=q[0];g+=q[1];b+=q[2];a+=q[3];++n;}
            std::uint8_t* d=&buf[(static_cast<std::size_t>(y)*W+x)*4]; if(n){d[0]=r/n;d[1]=g/n;d[2]=b/n;d[3]=a/n;}
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
                         std::size_t npix, float alpha) {
    int n = static_cast<int>(items.size());
    int i = 0;
    while (i < n) {
        const DisplayItem& it = items[i];
        if (it.clipDepth > 0) {
            // SWF 语义：clipDepth 对象本身仍正常绘制（无填充的“隐形遮罩”自然不画任何像素），
            // 同时它定义了后续 depth+1..clipDepth 层的裁剪区域。
            if (it.character) renderItemInto(it, root, dict, buf, W, H, frameIndex, alpha, it.blend);
            std::vector<std::uint8_t> mask(npix, 0);
            if (it.character && it.character->kind == Character::Kind::Shape && it.character->shape) {
                Matrix wm = root.post(it.matrix);
                fillMask(it.character->shape->fillPaths, wm, mask.data(), W, H);
            }
            std::vector<std::uint8_t> layer(npix * 4, 0);
            int j = i + 1;
            while (j < n && items[j].depth <= it.clipDepth) {
                if (items[j].clipDepth == 0 && items[j].character)
                    renderItemInto(items[j], root, dict, layer.data(), W, H, frameIndex, alpha, items[j].blend);
                ++j;
            }
            for (std::size_t p = 0; p < npix; ++p) {
                if (!mask[p]) continue;
                const std::uint8_t* s = &layer[p * 4];
                std::uint8_t* d = &buf[p * 4];
                float sa = (s[3] / 255.0f) * (mask[p] / 255.0f);   // 遮罩覆盖率调制源 alpha
                if (sa <= 0.0f) continue;
                float da = d[3] / 255.0f;
                float outA = sa + da * (1.0f - sa);
                if (outA <= 0.0f) continue;
                d[0] = static_cast<std::uint8_t>((s[0] * sa + d[0] * da * (1.0f - sa)) / outA);
                d[1] = static_cast<std::uint8_t>((s[1] * sa + d[1] * da * (1.0f - sa)) / outA);
                d[2] = static_cast<std::uint8_t>((s[2] * sa + d[2] * da * (1.0f - sa)) / outA);
                d[3] = static_cast<std::uint8_t>(outA * 255.0f);
            }
            i = j;  // 跳过整个裁剪组
        } else {
            if (it.character) {
                if (!it.filters.empty()) {
                    // 带滤镜：先渲染到离屏层，应用滤镜（投影/模糊/辉光），再合成回主缓冲
                    std::vector<std::uint8_t> layer(npix * 4, 0);
                    renderItemInto(it, root, dict, layer.data(), W, H, frameIndex, alpha, it.blend);
                    applyFilters(it.filters, layer, buf, W, H, npix);
                } else {
                    renderItemInto(it, root, dict, buf, W, H, frameIndex, alpha, it.blend);
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
    const FontGlyphs* fg = nullptr;
    for (const auto& kv : movie.characters) {
        if (kv.second && kv.second->kind == Character::Kind::Font && kv.second->font) {
            fg = kv.second->font.get();
            break;
        }
    }
    if (!fg) return;
    float s = (static_cast<float>(d.textSize) * 20.0f) / 1024.0f;   // 字形设计单位(em=1024) -> twips
    Color col(static_cast<std::uint8_t>((d.textColor >> 16) & 0xFF),
              static_cast<std::uint8_t>((d.textColor >> 8) & 0xFF),
              static_cast<std::uint8_t>(d.textColor & 0xFF),
              static_cast<std::uint8_t>((d.textColor >> 24) & 0xFF));
    Paint p; p.kind = FillKind::Solid; p.solid = col;
    float penX = 0, penY = 0;
    float lineH = static_cast<float>(d.textSize) * 20.0f * 1.2f;   // 行高（twips）
    auto glyphAdvance = [&](const Shape& shape) -> float {
        float w = 0;
        for (const auto& ct : shape.fillPaths)
            for (const auto& c : ct)
                for (float x : c.xs) w = std::max(w, x);
        return w;
    };
    for (char ch : d.textData) {
        if (ch == '\n') { penY += lineH; penX = 0; continue; }
        auto it = fg->codeToGlyph.find(static_cast<unsigned char>(ch));
        int gi = (it != fg->codeToGlyph.end()) ? it->second : -1;
        if (gi < 0 || gi >= static_cast<int>(fg->shapes.size()) || !fg->shapes[gi]) {
            penX += s * 512.0f; continue;   // 未知字符占位
        }
        const auto& shape = *fg->shapes[gi];
        Matrix ms; ms.sx = s; ms.sy = s;                 // scale（em -> twips）
        Matrix mt; mt.tx = penX; mt.ty = penY;          // 笔位（twips）
        Matrix gm = world.post(ms).post(mt);
        Matrix Minv; gm.invert(Minv);
        for (std::size_t i = 0; i < shape.fillStyles.size(); ++i) {
            fillStyleOf(shape, i, gm, Minv, p, buf, W, H, alpha, BlendMode::Normal);
        }
        float wEm = glyphAdvance(shape);
        penX += (wEm > 0 ? wEm : 512.0f) * s + s * 40.0f;   // 前进 + 字间距
    }
}

// AVM2 运行时显示列表（AS3 构建的场景）的渲染。
// parent：父级把"角色局部 twips"映射为设备像素的仿射矩阵；
// DisplayObject 的 x/y 单位 = 父级局部空间的像素（20 twips），scale/rotation 分解自其变换矩阵。
void renderDisplayObject(const std::shared_ptr<fvm::DisplayObject>& d, const Matrix& parent,
                         const Movie& movie, std::uint8_t* buf, int W, int H,
                         int frameIndex, float alpha = 1.0f) {
    if (!d || !d->visible) return;
    // 对象局部变换（parent twips -> parent twips）：Translate(x*20,y*20) * Rotate * Scale
    Matrix Lt;
    double rad = d->rotation * 3.141592653589793 / 180.0;
    double ca = std::cos(rad), sa = std::sin(rad);
    Lt.sx  = static_cast<float>(d->scaleX * ca);
    Lt.r0  = static_cast<float>(-d->scaleX * sa);
    Lt.r1  = static_cast<float>(d->scaleY * sa);
    Lt.sy  = static_cast<float>(d->scaleY * ca);
    Lt.tx  = static_cast<float>(d->x * 20.0);
    Lt.ty  = static_cast<float>(d->y * 20.0);
    Matrix world = parent.post(Lt);          // 角色局部 twips -> 设备像素
    float a = alpha * static_cast<float>(d->alpha);

    if (d->characterId) {
        auto it = movie.characters.find(d->characterId);
        if (it != movie.characters.end() && it->second) {
            DisplayItem di;
            di.depth = 0;
            di.character = it->second;
            di.matrix = Matrix{};            // 单位矩阵：world 已含对象变换
            // MovieClip 用自己的 currentFrame（动画），其余用外层帧号
            int fIdx = (d->kind == fvm::ObjKind::MovieClip && d->currentFrame > 0)
                           ? d->currentFrame : frameIndex;
            renderItemInto(di, world, movie.characters, buf, W, H, fIdx, a, d->blend);
        }
    }
    if (d->hasText && !d->textData.empty())
        renderDynamicText(*d, world, movie, buf, W, H, a);
    for (const auto& c : d->children)
        renderDisplayObject(c, world, movie, buf, W, H, frameIndex, a);
}

}  // namespace

bool renderMovieToBuffer(const Movie& movie, int frameIndex,
                          std::vector<std::uint8_t>& out, int& W, int& H) {
    if (frameIndex < 0 || frameIndex >= static_cast<int>(movie.frames.size())) return false;
    const Frame& frame = movie.frames[frameIndex];

    W = static_cast<int>((movie.frameSize.x1 - movie.frameSize.x0) / 20.0f);
    H = static_cast<int>((movie.frameSize.y1 - movie.frameSize.y0) / 20.0f);
    if (W <= 0) W = 1; if (H <= 0) H = 1;
    std::size_t npix = static_cast<std::size_t>(W) * H;

    out.assign(npix * 4, 0);

    // 背景
    Color bg = movie.backgroundColor;
    for (int i = 0; i < static_cast<int>(npix); ++i) {
        out[i * 4 + 0] = bg.r; out[i * 4 + 1] = bg.g; out[i * 4 + 2] = bg.b; out[i * 4 + 3] = bg.a;
    }

    // 根变换：局部 twips -> 设备 px（含帧原点偏移）
    Matrix root;
    root.sx = 1.0f / 20.0f; root.sy = 1.0f / 20.0f;
    root.tx = -movie.frameSize.x0 / 20.0f;
    root.ty = -movie.frameSize.y0 / 20.0f;

    // 按深度顺序绘制（含 clipDepth 裁剪组处理，与精灵内部通用）
    // SWF_ONLY_DEPTH=<d>：只渲染该顶层显示项，用于逐项定位是哪一个画错了。
    if (std::getenv("SWF_PRINT_ITEMS"))
        for (const auto& it : frame.items)
            std::printf("item d%u cid=%u mat sx=%.4f sy=%.4f r0=%.4f r1=%.4f tx=%.1f ty=%.1f\n",
                        it.depth, it.characterId, it.matrix.sx, it.matrix.sy, it.matrix.r0, it.matrix.r1,
                        it.matrix.tx, it.matrix.ty);
    if (const char* onlyDepth = std::getenv("SWF_ONLY_DEPTH")) {
        int want = std::atoi(onlyDepth);
        std::vector<DisplayItem> one;
        for (const auto& it : frame.items) {
            if (static_cast<int>(it.depth) == want) {
                one.push_back(it);
                if (std::getenv("SWF_PRINT_ITEMS"))
                    std::printf("item d%u mat sx=%.4f sy=%.4f r0=%.4f r1=%.4f tx=%.1f ty=%.1f\n",
                                it.depth,
                                it.matrix.sx, it.matrix.sy, it.matrix.r0, it.matrix.r1, it.matrix.tx, it.matrix.ty);
            }
        }
        renderItemsWithClip(one, root, movie.characters, out.data(), W, H, frameIndex, npix, 1.0f);
    } else if (const char* maxDepthEnv = std::getenv("SWF_MAX_DEPTH")) {
        // 渲染 depth <= N 的前缀（二分定位“哪一层画错了”用）
        int maxDepth = std::atoi(maxDepthEnv);
        std::vector<DisplayItem> one;
        for (const auto& it : frame.items)
            if (static_cast<int>(it.depth) <= maxDepth) one.push_back(it);
        renderItemsWithClip(one, root, movie.characters, out.data(), W, H, frameIndex, npix, 1.0f);
    } else {
        renderItemsWithClip(frame.items, root, movie.characters, out.data(), W, H, frameIndex, npix, 1.0f);
    }

    // 叠加 AVM2 运行时（ActionScript 构建）的显示列表
    if (!movie.runtimeStage.empty()) {
        Matrix rootM;                     // 角色局部 twips -> 设备像素
        rootM.sx = root.sx; rootM.sy = root.sy;
        rootM.tx = root.tx; rootM.ty = root.ty;
        for (const auto& d : movie.runtimeStage)
            renderDisplayObject(d, rootM, movie, out.data(), W, H, frameIndex);
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
                          std::vector<std::uint8_t>& out, int& W, int& H) {
    W = static_cast<int>((movie.frameSize.x1 - movie.frameSize.x0) / 20.0f);
    H = static_cast<int>((movie.frameSize.y1 - movie.frameSize.y0) / 20.0f);
    if (W <= 0) W = 1; if (H <= 0) H = 1;
    std::size_t npix = static_cast<std::size_t>(W) * H;
    out.assign(npix * 4, 0);
    Color bg = movie.backgroundColor;
    for (int i = 0; i < static_cast<int>(npix); ++i) {
        out[i * 4 + 0] = bg.r; out[i * 4 + 1] = bg.g; out[i * 4 + 2] = bg.b; out[i * 4 + 3] = bg.a;
    }
    Matrix root;
    root.sx = 1.0f / 20.0f; root.sy = 1.0f / 20.0f;
    root.tx = -movie.frameSize.x0 / 20.0f;
    root.ty = -movie.frameSize.y0 / 20.0f;
    renderItemsWithClip(items, root, movie.characters, out.data(), W, H, frameIndex, npix, 1.0f);
    if (!movie.runtimeStage.empty()) {
        Matrix rootM; rootM.sx = root.sx; rootM.sy = root.sy;
        rootM.tx = root.tx; rootM.ty = root.ty;
        for (const auto& d : movie.runtimeStage)
            renderDisplayObject(d, rootM, movie, out.data(), W, H, frameIndex);
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

}  // namespace swf
