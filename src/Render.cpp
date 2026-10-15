// 软件光栅器实现：扫描线填充（even-odd）、纯色/渐变/位图填充、线描边、over 合成。
#include "swf/Render.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "swf/Decoder.hpp"

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
    Matrix gmat;                // 形状局部 -> 渐变空间
    float focalX = 0.0f, focalY = 0.0f;  // 焦点渐变焦点（相对渐变中心）
    const Bitmap* bmp = nullptr;
    Matrix bmat;                // 形状局部 -> 位图像素
    ColorTransform cx;
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
    Color c = p.solid;
    if (p.kind == FillKind::LinearGradient) {
        float gx, gy; p.gmat.apply(lx, ly, gx, gy);
        c = sampleGrad(p.grad, std::max(0.0f, std::min(1.0f, gx)));
    } else if (p.kind == FillKind::RadialGradient) {
        float gx, gy; p.gmat.apply(lx, ly, gx, gy);
        float r = std::sqrt(gx * gx + gy * gy);
        c = sampleGrad(p.grad, std::max(0.0f, std::min(1.0f, r)));
    } else if (p.kind == FillKind::FocalGradient) {
        // 焦点渐变：渐变自焦点起、至渐变圆周止。归一化到半径 R=1 的圆内。
        float gx, gy; p.gmat.apply(lx, ly, gx, gy);
        // 归一化坐标（半径 1）
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
void plot(std::uint8_t* buf, int W, int H, int px, int py, const Color& c) {
    if (px < 0 || py < 0 || px >= W || py >= H) return;
    float sa = c.a / 255.0f;
    if (sa <= 0.0f) return;
    std::uint8_t* d = buf + (static_cast<std::size_t>(py) * W + px) * 4;
    if (sa >= 1.0f) {
        d[0] = c.r; d[1] = c.g; d[2] = c.b; d[3] = 255;
        return;
    }
    float da = d[3] / 255.0f;
    float outA = sa + da * (1.0f - sa);
    if (outA <= 0.0f) return;
    d[0] = static_cast<std::uint8_t>((c.r * sa + d[0] * da * (1.0f - sa)) / outA);
    d[1] = static_cast<std::uint8_t>((c.g * sa + d[1] * da * (1.0f - sa)) / outA);
    d[2] = static_cast<std::uint8_t>((c.b * sa + d[2] * da * (1.0f - sa)) / outA);
    d[3] = static_cast<std::uint8_t>(outA * 255.0f);
}

// even-odd 扫描线填充一组轮廓（设备坐标），按 Paint 上色
void fillContours(const std::vector<Contour>& contours, const Matrix& M, const Matrix& Minv,
                  const Paint& paint, std::uint8_t* buf, int W, int H) {
    struct Edge { float x0, y0, x1, y1; };
    std::vector<Edge> edges;
    float ymin = 1e9f, ymax = -1e9f;
    for (const auto& ct : contours) {
        int n = static_cast<int>(ct.xs.size());
        if (n < 2) continue;
        std::vector<float> dx(n), dy(n);
        for (int i = 0; i < n; ++i) {
            float X, Y; M.apply(ct.xs[i], ct.ys[i], X, Y);
            dx[i] = X; dy[i] = Y;
        }
        for (int i = 0; i < n; ++i) {
            int j = (i + 1) % n;
            edges.push_back({dx[i], dy[i], dx[j], dy[j]});
            ymin = std::min(ymin, std::min(dy[i], dy[j]));
            ymax = std::max(ymax, std::max(dy[i], dy[j]));
        }
    }
    if (edges.empty()) return;
    int y0 = std::max(0, static_cast<int>(std::floor(ymin)));
    int y1 = std::min(H - 1, static_cast<int>(std::ceil(ymax)));
    std::vector<float> xs;
    std::vector<float> cov(static_cast<std::size_t>(W), 0.0f);  // 每像素水平覆盖率
    for (int y = y0; y <= y1; ++y) {
        std::fill(cov.begin(), cov.end(), 0.0f);
        // 2x2 超采样：每像素行取 2 条子扫描线，各贡献 0.5 覆盖率（水平抗锯齿）
        for (int sub = 0; sub < 2; ++sub) {
            float yy = y + (sub == 0 ? 0.25f : 0.75f);
            xs.clear();
            for (const auto& e : edges) {
                if ((e.y0 <= yy && e.y1 > yy) || (e.y1 <= yy && e.y0 > yy)) {
                    float t = (yy - e.y0) / (e.y1 - e.y0);
                    xs.push_back(e.x0 + t * (e.x1 - e.x0));
                }
            }
            if (xs.size() < 2) continue;
            std::sort(xs.begin(), xs.end());
            for (std::size_t i = 0; i + 1 < xs.size(); i += 2) {
                int xa = std::max(0, static_cast<int>(std::ceil(xs[i] - 0.5)));
                int xb = std::min(W - 1, static_cast<int>(std::floor(xs[i + 1] - 0.5)));
                for (int x = xa; x <= xb; ++x) cov[x] += 0.5f;
            }
        }
        // 用像素中心采样上色，按覆盖率调制 alpha 后合成
        for (int x = 0; x < W; ++x) {
            float cover = cov[x];
            if (cover <= 0.0f) continue;
            float lx, ly; Minv.apply(x + 0.5f, y + 0.5f, lx, ly);
            Color c = paintColor(paint, lx, ly);
            if (cover < 1.0f) {
                float a = (c.a / 255.0f) * cover;
                c.a = clampU8(static_cast<int>(a * 255.0f + 0.5f));
                if (c.a == 0) continue;
            }
            plot(buf, W, H, x, y, c);
        }
    }
}

void strokeContours(const std::vector<Contour>& contours, const Matrix& M, const Color& color,
                    float widthTwips, std::uint8_t* buf, int W, int H) {
    float hw = (widthTwips / 2.0f) / 20.0f;  // twips -> 设备半宽(px)
    if (hw < 0.25f) hw = 0.25f;
    Paint p; p.kind = FillKind::Solid; p.solid = color;
    for (const auto& ct : contours) {
        int n = static_cast<int>(ct.xs.size());
        for (int i = 0; i < n - 1; ++i) {
            float axp, ayp, bxp, byp;
            M.apply(ct.xs[i], ct.ys[i], axp, ayp);
            M.apply(ct.xs[i + 1], ct.ys[i + 1], bxp, byp);
            float dx = bxp - axp, dy = byp - ayp;
            float len = std::sqrt(dx * dx + dy * dy);
            if (len < 1e-6f) continue;
            float px = -dy / len * hw, py = dx / len * hw;
            std::vector<Contour> quad(1);
            quad[0].xs = {axp + px, bxp + px, bxp - px, axp - px};
            quad[0].ys = {ayp + py, byp + py, byp - py, axp - py};
            fillContours(quad, Matrix{}, Matrix{}, p, buf, W, H);  // 已是设备坐标
        }
    }
}

void renderShape(const Shape& shape, const Matrix& world, const ColorTransform& cx,
                 const std::map<std::uint16_t, std::shared_ptr<Character>>& dict,
                 std::uint8_t* buf, int W, int H) {
    Matrix Minv; world.invert(Minv);
    // 填充
    for (std::size_t i = 0; i < shape.fillStyles.size(); ++i) {
        if (shape.fillPaths[i].empty()) continue;
        const FillStyle& fs = shape.fillStyles[i];
        Paint p;
        p.kind = fs.kind; p.solid = fs.color; p.cx = cx; p.tiled = fs.tiled;
        if (fs.kind == FillKind::LinearGradient || fs.kind == FillKind::RadialGradient ||
            fs.kind == FillKind::FocalGradient) {
            p.grad = fs.gradient;
            p.gmat = fs.gradientMatrix;
            p.focalX = fs.focalX; p.focalY = fs.focalY;
        } else if (fs.kind == FillKind::Bitmap) {
            auto it = dict.find(fs.bitmapId);
            if (it != dict.end() && it->second->bitmap) {
                p.bmp = it->second->bitmap.get();
                p.bmat = fs.gradientMatrix;   // 位图矩阵（形状局部 -> 位图像素）
            }
        }
        fillContours(shape.fillPaths[i], world, Minv, p, buf, W, H);
    }
    // 描边
    for (std::size_t i = 0; i < shape.lineStyles.size(); ++i) {
        if (shape.strokePaths[i].empty()) continue;
        const LineStyle& ls = shape.lineStyles[i];
        strokeContours(shape.strokePaths[i], world, ls.color, ls.width, buf, W, H);
    }
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

// 把单个显示列表项（shape / bitmap / sprite）渲染进 buf。
void renderItemInto(const DisplayItem& item, const Matrix& root,
                    const std::map<std::uint16_t, std::shared_ptr<Character>>& dict,
                    std::uint8_t* buf, int W, int H, int frameIndex) {
    Matrix world = root.post(item.matrix);
    if (item.character->kind == Character::Kind::Shape && item.character->shape) {
        renderShape(*item.character->shape, world, item.cxform, dict, buf, W, H);
    } else if (item.character->kind == Character::Kind::Sprite) {
        // 精灵：按放置矩阵作为新的根，递归渲染其内部时间轴对应帧。
        // 内部帧号 = 当前时间轴帧号 - 该精灵的放置帧（新放置的精灵从内部第 0 帧开始）。
        const auto& sf = item.character->spriteFrames;
        if (sf.empty()) return;
        int idx = frameIndex - item.birthFrame;
        if (idx < 0) idx = 0;
        if (idx >= static_cast<int>(sf.size())) idx = static_cast<int>(sf.size()) - 1;
        for (const auto& child : sf[idx].items) {
            if (!child.character) continue;
            renderItemInto(child, world, dict, buf, W, H, idx);
        }
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
            Matrix gm = tm; gm.tx = gx; gm.ty = gy;
            Matrix Minv; gm.invert(Minv);
            Paint p; p.kind = FillKind::Solid; p.solid = tg.color;
            for (std::size_t i = 0; i < gs->fillStyles.size(); ++i) {
                if (i >= gs->fillPaths.size() || gs->fillPaths[i].empty()) continue;
                fillContours(gs->fillPaths[i], gm, Minv, p, buf, W, H);
            }
        }
    } else if (item.character->kind == Character::Kind::Bitmap && item.character->bitmap) {
        const Bitmap& bmp = *item.character->bitmap;
        for (int j = 0; j < bmp.height; ++j) {
            for (int i = 0; i < bmp.width; ++i) {
                float lx = static_cast<float>(i * 20), ly = static_cast<float>(j * 20);
                float dx, dy; world.apply(lx, ly, dx, dy);
                int px = static_cast<int>(std::round(dx)), py = static_cast<int>(std::round(dy));
                const std::uint8_t* p4 = &bmp.rgba[static_cast<std::size_t>(j) * bmp.width * 4 + i * 4];
                plot(buf, W, H, px, py, item.cxform.apply(Color(p4[0], p4[1], p4[2], p4[3])));
            }
        }
    }
}

// AVM2 运行时显示列表（AS3 构建的场景）的渲染。
// 变换约定：parent 把"角色局部 twips"映射为设备像素；DisplayObject 的 x/y 单位是
// 父级局部空间的像素（即 20 twips），scale 为缩放系数。
struct DevT { double sx = 1.0 / 20.0, sy = 1.0 / 20.0, tx = 0, ty = 0; };

void renderDisplayObject(const std::shared_ptr<fvm::DisplayObject>& d, const DevT& parent,
                         const Movie& movie, std::uint8_t* buf, int W, int H, int frameIndex) {
    if (!d || !d->visible) return;
    DevT cur;
    cur.sx = parent.sx * (d->scaleX != 0 ? d->scaleX : 1.0);
    cur.sy = parent.sy * (d->scaleY != 0 ? d->scaleY : 1.0);
    cur.tx = parent.tx + parent.sx * (d->x * 20.0);
    cur.ty = parent.ty + parent.sy * (d->y * 20.0);

    if (d->characterId) {
        auto it = movie.characters.find(d->characterId);
        if (it != movie.characters.end() && it->second) {
            Matrix w;
            w.sx = static_cast<float>(cur.sx);
            w.sy = static_cast<float>(cur.sy);
            w.tx = static_cast<float>(cur.tx);
            w.ty = static_cast<float>(cur.ty);
            DisplayItem di;
            di.depth = 0;
            di.character = it->second;
            di.matrix = Matrix{};              // 单位矩阵：world = w
            renderItemInto(di, w, movie.characters, buf, W, H, frameIndex);
        }
    }
    for (const auto& c : d->children)
        renderDisplayObject(c, cur, movie, buf, W, H, frameIndex);
}

}  // namespace

bool renderMovieToPNG(const Movie& movie, int frameIndex, const char* path) {
    if (frameIndex < 0 || frameIndex >= static_cast<int>(movie.frames.size())) return false;
    const Frame& frame = movie.frames[frameIndex];

    int W = static_cast<int>((movie.frameSize.x1 - movie.frameSize.x0) / 20.0f);
    int H = static_cast<int>((movie.frameSize.y1 - movie.frameSize.y0) / 20.0f);
    if (W <= 0) W = 1; if (H <= 0) H = 1;
    std::size_t npix = static_cast<std::size_t>(W) * H;

    std::vector<std::uint8_t> buf(npix * 4, 0);

    // 背景
    Color bg = movie.backgroundColor;
    for (int i = 0; i < static_cast<int>(npix); ++i) {
        buf[i * 4 + 0] = bg.r; buf[i * 4 + 1] = bg.g; buf[i * 4 + 2] = bg.b; buf[i * 4 + 3] = bg.a;
    }

    // 根变换：局部 twips -> 设备 px（含帧原点偏移）
    Matrix root;
    root.sx = 1.0f / 20.0f; root.sy = 1.0f / 20.0f;
    root.tx = -movie.frameSize.x0 / 20.0f;
    root.ty = -movie.frameSize.y0 / 20.0f;

    // 按深度顺序绘制；遇到 clipDepth 图层时，把其 depth+1..clipDepth 范围内的子项
    // 渲染到离屏缓冲，并用该图层形状的填充区域作为遮罩合成回主缓冲（实现 SWF 裁剪）。
    const auto& items = frame.items;
    int n = static_cast<int>(items.size());
    int i = 0;
    while (i < n) {
        const DisplayItem& it = items[i];
        if (it.clipDepth > 0) {
            std::vector<std::uint8_t> mask(npix, 0);
            if (it.character && it.character->kind == Character::Kind::Shape && it.character->shape) {
                Matrix wm = root.post(it.matrix);
                fillMask(it.character->shape->fillPaths, wm, mask.data(), W, H);
            }
            std::vector<std::uint8_t> layer(npix * 4, 0);
            int j = i + 1;
            while (j < n && items[j].depth <= it.clipDepth) {
                if (items[j].clipDepth == 0 && items[j].character)
                    renderItemInto(items[j], root, movie.characters, layer.data(), W, H, frameIndex);
                ++j;
            }
            for (int p = 0; p < static_cast<int>(npix); ++p) {
                if (!mask[p]) continue;
                const std::uint8_t* s = &layer[p * 4];
                std::uint8_t* d = &buf[p * 4];
                // 遮罩覆盖率调制源 alpha（裁剪边缘抗锯齿）
                float sa = (s[3] / 255.0f) * (mask[p] / 255.0f);
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
            if (it.character) renderItemInto(it, root, movie.characters, buf.data(), W, H, frameIndex);
            ++i;
        }
    }

    // 叠加 AVM2 运行时（ActionScript 构建）的显示列表
    if (!movie.runtimeStage.empty()) {
        DevT stage;
        stage.sx = root.sx; stage.sy = root.sy;
        stage.tx = root.tx; stage.ty = root.ty;
        for (const auto& d : movie.runtimeStage)
            renderDisplayObject(d, stage, movie, buf.data(), W, H, frameIndex);
    }

    return stbi_write_png(path, W, H, 4, buf.data(), W * 4) != 0;
}

}  // namespace swf
