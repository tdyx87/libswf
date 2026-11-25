// GpuRenderSink.cpp
// 显示列表 sink -> swfr 渲染命令的适配器骨架。
//
// 关键换算（与软件光栅器 Render.cpp 的语义对齐）：
//   * 几何：SWF 形状是 twips（1px = 20 twips），这里在**建立网格时**除以 20，
//     得到“舞台像素”空间；绘制时用 world * S(20) 把像素空间送回 twips 再变换。
//     这样三角化精度与像素一致，不会因为 twips 数值放大 20 倍而多出 20 倍三角形。
//   * 渐变：软件光栅器里渐变空间半程 = 16384 twips（线性 t=(gx+16384)/32768，
//     径向 t=|g|/16384）。swfr 的填充空间是单位方框（径向 length(uv-0.5)*2），
//     所以 uv = N ∘ inv(gradientMatrix) ∘ S20，其中 N 把渐变空间映射到 uv（缩放
//     1/32768，平移 0.5），fillMatrix 取其逆。
//     注意 gradientMatrix 是「渐变/位图空间 -> 形状空间」（与软件光栅器里
//     Matrix::invert 后再采样一致），所以这里必须先求逆，直接用会把渐变缩放平方。
//   * 位图：swfr 的 uv 是“形状 -> [0,1] 贴图”，故 uv = S(1/w,1/h) ∘ inv(gradientMatrix) ∘ S20。
//   * 填充几何：优先用 Shape::fillEdges（有向边 + 侧别）重建闭环，再按非零环绕填充，
//     与软件光栅器的 winding 判定逐点一致；缺边时退回 fillPaths + 非零环绕。
#include "swf/GpuRenderSink.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

#include "render/Renderer.h"
#include "swf/Render.hpp"

namespace swf {
namespace {

constexpr float kTwipsPerPx = 20.0f;
constexpr float kPxPerTwip = 1.0f / kTwipsPerPx;
// SWF 渐变空间的半程（twips），见 Render.cpp paintColorInPaintSpace。
constexpr float kGradientHalf = 16384.0f;

static_assert(sizeof(swfr::ShapeHandle) == sizeof(std::uint32_t), "handle size changed");

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

swfr::Mat2D ToMat2D(const Matrix& m) {
    swfr::Mat2D out;
    out.a = m.sx;
    out.b = m.r1;
    out.c = m.r0;
    out.d = m.sy;
    out.tx = m.tx;
    out.ty = m.ty;
    return out;
}

swfr::Mat2D Invert(const swfr::Mat2D& m) {
    const float det = m.a * m.d - m.b * m.c;
    if (std::fabs(det) < 1e-9f) return swfr::Mat2D::Identity();
    const float id = 1.0f / det;
    swfr::Mat2D inv;
    inv.a = m.d * id;
    inv.b = -m.b * id;
    inv.c = -m.c * id;
    inv.d = m.a * id;
    inv.tx = -(inv.a * m.tx + inv.c * m.ty);
    inv.ty = -(inv.b * m.tx + inv.d * m.ty);
    return inv;
}

// 几何已经除以 20，绘制前再乘回去：device = world(twips) ∘ S20。
swfr::Mat2D ToDevice(const Matrix& world) {
    return swfr::Mat2D::Multiply(ToMat2D(world), swfr::Mat2D::Scaling(kTwipsPerPx, kTwipsPerPx));
}

swfr::Color ToColor(const Color& c) {
    swfr::Color out;
    out.r = c.r / 255.0f;
    out.g = c.g / 255.0f;
    out.b = c.b / 255.0f;
    out.a = c.a / 255.0f;
    return out;
}

swfr::ColorTransform ToColorTransform(const ColorTransform& cx, float alpha) {
    swfr::ColorTransform out;
    out.mul[0] = cx.mr;
    out.mul[1] = cx.mg;
    out.mul[2] = cx.mb;
    out.mul[3] = cx.ma * alpha;   // 对象级透明度折进 alpha 乘项
    // ColorTransform 的加项是 0..255 的整数偏移（见 Decoder 的 readCXForm），
    // 软件端直接加到 0..255 的分量上；着色器工作在 0..1，必须除以 255。
    // 漏掉这一步会把整块调色对象染错（金边变成亮蓝），也是对比度的主要来源。
    out.add[0] = cx.ar / 255.0f;
    out.add[1] = cx.ag / 255.0f;
    out.add[2] = cx.ab / 255.0f;
    out.add[3] = cx.aa / 255.0f;
    return out;
}

swfr::BlendMode ToBlendMode(BlendMode mode, GpuRenderSink::Stats* stats) {
    switch (mode) {
        case BlendMode::Normal: return swfr::BlendMode::Normal;
        case BlendMode::Add: return swfr::BlendMode::Add;
        case BlendMode::Multiply: return swfr::BlendMode::Multiply;
        case BlendMode::Screen: return swfr::BlendMode::Screen;
        case BlendMode::Lighten: return swfr::BlendMode::Lighten;
        case BlendMode::Darken: return swfr::BlendMode::Darken;
        default:
            // Layer / Difference / Subtract / Invert / Alpha / Erase / Overlay / HardLight:
            // 后端还没有对应的 blend state 或离屏合成。
            ++stats->blendFallback;
            return swfr::BlendMode::Normal;
    }
}

std::int64_t Quantize(float x, float y) {
    // 1/16 twip 的网格，足以把“同一条轮廓的相邻边”配对上，又不会误合并邻近顶点。
    const auto ix = static_cast<std::int64_t>(std::llround(x * 16.0f));
    const auto iy = static_cast<std::int64_t>(std::llround(y * 16.0f));
    return (ix << 32) ^ (iy & 0xffffffff);
}

void AppendScaled(std::vector<swfr::PathCommand>& path, const Vec2& p, bool move) {
    swfr::PathCommand command;
    command.verb = move ? swfr::PathVerb::MoveTo : swfr::PathVerb::LineTo;
    command.x = p.x * kPxPerTwip;
    command.y = p.y * kPxPerTwip;
    path.push_back(command);
}

void AppendClose(std::vector<swfr::PathCommand>& path) {
    swfr::PathCommand command;
    command.verb = swfr::PathVerb::Close;
    path.push_back(command);
}

// 填充边 -> 闭环。
//
// 解码器里每条填充边带 sign：+1 表示该样式在边右侧，-1 表示在左侧。软件光栅器的
// 环绕数贡献是 sign * sign(y1-y0)。要把它变成几何上等价的有向环，必须先按 sign 把
// 边归一成“样式始终在右侧”的方向（sign=-1 反向），再**沿存储方向**串环。
//
// 这样每个闭环的几何环绕数就与软件光栅器的环绕数一致：外环 +1、内侧孔洞环 -1，
// 非零环绕下孔洞自然抵消为空。若按未归一的方向串环或事后统一翻成正向，孔洞会被
// 填实（字形「始」里两笔画之间的缝隙、内耳廓等）。
void ChainLoops(const std::vector<FillEdge>& edges, std::vector<std::vector<Vec2>>* loops) {
    struct Segment {
        Vec2 a;
        Vec2 b;
    };

    std::vector<Segment> segments;
    segments.reserve(edges.size());
    for (const FillEdge& edge : edges) {
        if (edge.x0 == edge.x1 && edge.y0 == edge.y1) continue;
        Segment segment;
        // 统一成“样式在边右侧”的朝向。
        if (edge.sign >= 0) {
            segment.a = { edge.x0, edge.y0 };
            segment.b = { edge.x1, edge.y1 };
        } else {
            segment.a = { edge.x1, edge.y1 };
            segment.b = { edge.x0, edge.y0 };
        }
        segments.push_back(segment);
    }
    if (segments.empty()) return;

    // 起点 -> 从该点出发、尚未使用的边
    std::unordered_multimap<std::int64_t, std::size_t> outgoing;
    outgoing.reserve(segments.size());
    for (std::size_t i = 0; i < segments.size(); ++i)
        outgoing.emplace(Quantize(segments[i].a.x, segments[i].a.y), i);

    std::vector<bool> used(segments.size(), false);
    for (std::size_t i = 0; i < segments.size(); ++i) {
        if (used[i]) continue;

        std::vector<Vec2> loop;
        const std::int64_t startKey = Quantize(segments[i].a.x, segments[i].a.y);
        loop.push_back(segments[i].a);
        used[i] = true;

        Vec2 current = segments[i].b;
        std::int64_t currentKey = Quantize(current.x, current.y);
        std::size_t guard = segments.size() + 1;
        while (guard-- > 0 && currentKey != startKey) {
            const auto range = outgoing.equal_range(currentKey);
            std::size_t next = static_cast<std::size_t>(-1);
            for (auto it = range.first; it != range.second; ++it) {
                if (!used[it->second]) {
                    next = it->second;
                    break;
                }
            }
            if (next == static_cast<std::size_t>(-1)) break;   // 断链：环不闭合，丢弃

            used[next] = true;
            loop.push_back(segments[next].a);
            current = segments[next].b;
            currentKey = Quantize(current.x, current.y);
        }

        if (currentKey != startKey || loop.size() < 3) continue;
        loops->push_back(std::move(loop));
    }
}

std::vector<swfr::PathCommand> BuildFillPath(const Shape& shape, std::size_t style) {
    std::vector<swfr::PathCommand> path;

    std::vector<std::vector<Vec2>> loops;
    if (std::getenv("SWF_GPU_PATHS") && style < shape.fillPaths.size()) {
        for (const Contour& contour : shape.fillPaths[style]) {
            if (contour.xs.size() < 3) continue;
            std::vector<Vec2> loop;
            loop.reserve(contour.xs.size());
            for (std::size_t i = 0; i < contour.xs.size(); ++i)
                loop.push_back({ contour.xs[i], contour.ys[i] });
            loops.push_back(std::move(loop));
        }
    } else if (style < shape.fillEdges.size() && !shape.fillEdges[style].empty())
        ChainLoops(shape.fillEdges[style], &loops);

    if (!loops.empty()) {
        for (const auto& loop : loops) {
            for (std::size_t i = 0; i < loop.size(); ++i) AppendScaled(path, loop[i], i == 0);
            AppendClose(path);
        }
        return path;
    }

    if (style >= shape.fillPaths.size()) return path;
    for (const Contour& contour : shape.fillPaths[style]) {
        if (contour.xs.size() < 3) continue;
        for (std::size_t i = 0; i < contour.xs.size(); ++i)
            AppendScaled(path, { contour.xs[i], contour.ys[i] }, i == 0);
        AppendClose(path);
    }
    return path;
}

// 字形 / 遮罩：把该形状所有样式的边合并成一个轮廓集（同一形状的填充样式
// 是同一个字形/遮罩区域的拆分，并集即轮廓）。
std::vector<swfr::PathCommand> BuildMergedPath(const Shape& shape) {
    std::vector<FillEdge> merged;
    for (const auto& group : shape.fillEdges) merged.insert(merged.end(), group.begin(), group.end());

    std::vector<std::vector<Vec2>> loops;
    if (!merged.empty()) ChainLoops(merged, &loops);

    std::vector<swfr::PathCommand> path;
    if (!loops.empty()) {
        for (const auto& loop : loops) {
            for (std::size_t i = 0; i < loop.size(); ++i) AppendScaled(path, loop[i], i == 0);
            AppendClose(path);
        }
        return path;
    }

    for (const auto& group : shape.fillPaths) {
        for (const Contour& contour : group) {
            if (contour.xs.size() < 3) continue;
            for (std::size_t i = 0; i < contour.xs.size(); ++i)
                AppendScaled(path, { contour.xs[i], contour.ys[i] }, i == 0);
            AppendClose(path);
        }
    }
    return path;
}

// 线样式的轮廓同样是“弧段”：同一样式的多条轮廓首尾相接才是完整的线环。
// 这里保留开放子路径（不 Close），端点用圆头，配合圆角连接——与软件光栅器
// 按线段胶囊逐段绘制的形状一致；强行闭合会在环内部凭空切出一条线。
std::vector<swfr::PathCommand> BuildStrokePath(const std::vector<Contour>& contours) {
    std::vector<swfr::PathCommand> path;
    for (const Contour& contour : contours) {
        if (contour.xs.size() < 2) continue;
        for (std::size_t i = 0; i < contour.xs.size(); ++i)
            AppendScaled(path, { contour.xs[i], contour.ys[i] }, i == 0);
    }
    return path;
}

}  // namespace

GpuRenderSink::GpuRenderSink(swfr::Renderer& renderer) : m_renderer(&renderer) {}

GpuRenderSink::~GpuRenderSink() = default;

void GpuRenderSink::beginFrame(int width, int height,
                               const std::map<std::uint16_t, std::shared_ptr<Character>>& dict) {
    m_width = width;
    m_height = height;
    m_dict = &dict;
    m_maskPushed.clear();
}

void GpuRenderSink::endFrame() {
    m_dict = nullptr;
}

// ---------------------------------------------------------------- 资源缓存

// 缓存指纹（FNV-1a）。同一地址的 Shape 内容变了（Morph 每帧插值、分配器复用
// 地址）就必须重建网格，否则会被当成命中、永远画旧几何。
namespace {

struct ContentHasher {
    std::uint64_t h = 1469598103934665603ull;
    void u64(std::uint64_t v) { h ^= v; h *= 1099511628211ull; }
    void f(float v) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        u64(bits);
    }
    void color(const Color& c) { u64(c.r); u64(c.g); u64(c.b); u64(c.a); }
    void matrix(const Matrix& m) { f(m.sx); f(m.r0); f(m.r1); f(m.sy); f(m.tx); f(m.ty); }
    void edges(const std::vector<std::vector<FillEdge>>& groups) {
        u64(groups.size());
        for (const auto& g : groups) {
            u64(g.size());
            for (const auto& e : g) { f(e.x0); f(e.y0); f(e.x1); f(e.y1); }
        }
    }
    void contours(const std::vector<std::vector<Contour>>& groups) {
        u64(groups.size());
        for (const auto& g : groups) {
            u64(g.size());
            for (const auto& c : g) {
                u64(c.xs.size());
                for (std::size_t i = 0; i < c.xs.size(); ++i) { f(c.xs[i]); f(c.ys[i]); }
            }
        }
    }
};

}  // namespace

// 遮罩网格只由轮廓几何决定（BuildMergedPath 优先用 fillEdges，其次 fillPaths）。
static std::uint64_t maskGeometryHash(const Shape& shape) {
    ContentHasher hh;
    hh.edges(shape.fillEdges);
    hh.contours(shape.fillPaths);
    return hh.h;
}

// 填充网格还受填充样式影响（morph 会插值颜色/渐变/焦点）。
static std::uint64_t fillContentHash(const Shape& shape) {
    ContentHasher hh;
    hh.edges(shape.fillEdges);
    hh.contours(shape.fillPaths);
    hh.u64(shape.fillStyles.size());
    for (const auto& s : shape.fillStyles) {
        hh.u64(static_cast<std::uint64_t>(s.kind));
        hh.color(s.color);
        hh.matrix(s.gradientMatrix);
        hh.u64(s.gradient.size());
        for (const auto& stop : s.gradient) { hh.u64(stop.ratio); hh.color(stop.color); }
        hh.f(s.focalX); hh.f(s.focalY);
        hh.u64(s.spreadMode); hh.u64(s.interpMode); hh.u64(s.bitmapId); hh.u64(s.tiled ? 1 : 0);
    }
    return hh.h;
}

// 描边网格取决于路径与线样式（宽度/颜色）。
static std::uint64_t strokeContentHash(const Shape& shape) {
    ContentHasher hh;
    hh.contours(shape.strokePaths);
    hh.u64(shape.lineStyles.size());
    for (const auto& s : shape.lineStyles) { hh.f(s.width); hh.color(s.color); }
    return hh.h;
}

std::uint32_t GpuRenderSink::fillShape(const Shape& shape, std::size_t styleIndex) {
    const std::uint64_t hash = fillContentHash(shape);
    ShapeMeshEntry& entry = m_fills[&shape];
    if (entry.hash != hash) {
        // 同一地址换了内容（morph 每帧插值）：整组样式句柄作废并释放。
        for (std::uint32_t h : entry.handles)
            if (h) m_renderer->ReleaseShape(h);
        entry.handles.assign(shape.fillStyles.size(), 0);
        entry.hash = hash;
    }
    std::vector<std::uint32_t>& cache = entry.handles;
    if (cache.size() < shape.fillStyles.size()) cache.resize(shape.fillStyles.size(), 0);
    if (styleIndex < cache.size() && cache[styleIndex]) return cache[styleIndex];

    const FillStyle& style = shape.fillStyles[styleIndex];
    swfr::ShapeDesc desc;
    desc.path = BuildFillPath(shape, styleIndex);
    if (desc.path.empty()) return 0;
    desc.fillRule = swfr::FillRule::NonZero;
    desc.fill.color = ToColor(style.color);

    const swfr::Mat2D gradientToShape = ToMat2D(style.gradientMatrix);
    const swfr::Mat2D shapeToGradient = Invert(gradientToShape);   // 形状(twips) -> 渐变/位图空间
    const swfr::Mat2D twipsScale = swfr::Mat2D::Scaling(kTwipsPerPx, kTwipsPerPx);

    switch (style.kind) {
        case FillKind::Solid:
            desc.fill.kind = swfr::FillKind::Solid;
            break;

        case FillKind::LinearGradient:
        case FillKind::RadialGradient:
        case FillKind::FocalGradient: {
            // FocalGradient 暂时按 RadialGradient 画（后端没有焦点渐变）。
            desc.fill.kind = (style.kind == FillKind::LinearGradient)
                                 ? swfr::FillKind::LinearGradient
                                 : swfr::FillKind::RadialGradient;

            swfr::Mat2D normalise;   // 渐变空间 -> uv=[0,1]
            normalise.a = normalise.d = 1.0f / (2.0f * kGradientHalf);
            normalise.tx = normalise.ty = 0.5f;
            const swfr::Mat2D toUv = swfr::Mat2D::Multiply(
                normalise, swfr::Mat2D::Multiply(shapeToGradient, twipsScale));
            desc.fill.fillMatrix = Invert(toUv);

            desc.fill.stops.reserve(style.gradient.size());
            for (const GradientStop& stop : style.gradient)
                desc.fill.stops.push_back({ stop.ratio / 255.0f, ToColor(stop.color) });
            break;
        }

        case FillKind::Bitmap: {
            desc.fill.kind = swfr::FillKind::Bitmap;

            const Bitmap* bitmap = nullptr;
            if (m_dict) {
                const auto it = m_dict->find(style.bitmapId);
                if (it != m_dict->end() && it->second) bitmap = it->second->bitmap.get();
            }
            if (!bitmap || !bitmap->width || !bitmap->height) {   // 位图缺失：退化为实色
                desc.fill.kind = swfr::FillKind::Solid;
                break;
            }

            desc.fill.texture = bitmapTexture(*bitmap);
            if (!desc.fill.texture) {
                desc.fill.kind = swfr::FillKind::Solid;
                break;
            }

            // inv(gradientMatrix) 是「形状(twips) -> 位图像素」，uv 是「形状 -> [0,1] 贴图」。
            const swfr::Mat2D toUv = swfr::Mat2D::Multiply(
                swfr::Mat2D::Scaling(1.0f / static_cast<float>(bitmap->width),
                                     1.0f / static_cast<float>(bitmap->height)),
                swfr::Mat2D::Multiply(shapeToGradient, twipsScale));
            desc.fill.fillMatrix = Invert(toUv);
            desc.fill.wrap = style.tiled;
            break;
        }
    }

    if (std::getenv("SWF_GPU_DBG") && desc.fill.kind != swfr::FillKind::Solid) {
        float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
        for (const auto& c : desc.path) {
            if (c.verb == swfr::PathVerb::Close) continue;
            minx = std::min(minx, c.x); maxx = std::max(maxx, c.x);
            miny = std::min(miny, c.y); maxy = std::max(maxy, c.y);
        }
        const swfr::Mat2D inv = Invert(desc.fill.fillMatrix);
        float u0 = 1e9f, v0 = 1e9f, u1 = -1e9f, v1 = -1e9f;
        const float cx[4] = { minx, maxx, minx, maxx };
        const float cy[4] = { miny, miny, maxy, maxy };
        for (int k = 0; k < 4; ++k) {
            float u, v;
            inv.TransformPoint(cx[k], cy[k], &u, &v);
            u0 = std::min(u0, u); u1 = std::max(u1, u);
            v0 = std::min(v0, v); v1 = std::max(v1, v);
        }
        std::fprintf(stderr, "[fill] kind=%d wrap=%d stops=%zu bbox=(%.1f,%.1f)-(%.1f,%.1f) uv=(%.3f,%.3f)-(%.3f,%.3f)\n",
                     (int)desc.fill.kind, desc.fill.wrap ? 1 : 0, desc.fill.stops.size(), minx, miny, maxx, maxy, u0, v0, u1, v1);
        for (const auto& st : desc.fill.stops)
            std::fprintf(stderr, "   stop %.3f rgba=%.2f,%.2f,%.2f,%.2f\n", st.offset,
                         st.color.r, st.color.g, st.color.b, st.color.a);
    }
    const swfr::ShapeHandle handle = m_renderer->CreateShape(desc);
    if (!handle) return 0;
    ++m_stats.shapesCreated;
    cache[styleIndex] = handle;
    return handle;
}

std::uint32_t GpuRenderSink::strokeShape(const Shape& shape, std::size_t styleIndex) {
    const std::uint64_t hash = strokeContentHash(shape);
    ShapeMeshEntry& entry = m_strokes[&shape];
    if (entry.hash != hash) {
        for (std::uint32_t h : entry.handles)
            if (h) m_renderer->ReleaseShape(h);
        entry.handles.assign(shape.lineStyles.size(), 0);
        entry.hash = hash;
    }
    std::vector<std::uint32_t>& cache = entry.handles;
    if (cache.size() < shape.lineStyles.size()) cache.resize(shape.lineStyles.size(), 0);
    if (styleIndex < cache.size() && cache[styleIndex]) return cache[styleIndex];
    if (styleIndex >= shape.strokePaths.size()) return 0;

    const LineStyle& style = shape.lineStyles[styleIndex];
    swfr::StrokeDesc desc;
    desc.path = BuildStrokePath(shape.strokePaths[styleIndex]);
    if (desc.path.empty()) return 0;
    desc.width = style.width * kPxPerTwip;
    // SWF LineStyle1 是圆角连接；LineStyle2 的 JoinStyle 尚未解码，先按圆角画。
    // 轮廓是开放弧段，圆头端点与软件端“线段胶囊”的端点形状一致。
    desc.join = swfr::LineJoin::Round;
    desc.cap = swfr::LineCap::Round;
    desc.color = ToColor(style.color);

    const swfr::ShapeHandle handle = m_renderer->CreateStroke(desc);
    if (!handle) return 0;
    ++m_stats.shapesCreated;
    cache[styleIndex] = handle;
    return handle;
}

std::uint32_t GpuRenderSink::whiteShapeMesh(const Shape& shape) {
    swfr::ShapeDesc desc;
    desc.path = BuildMergedPath(shape);
    if (desc.path.empty()) return 0;
    desc.fillRule = swfr::FillRule::NonZero;
    desc.fill.kind = swfr::FillKind::Solid;
    desc.fill.color = { 1.0f, 1.0f, 1.0f, 1.0f };   // 由颜色变换着色

    const swfr::ShapeHandle handle = m_renderer->CreateShape(desc);
    if (!handle) return 0;
    ++m_stats.shapesCreated;
    return handle;
}

std::uint32_t GpuRenderSink::glyphShape(const Shape& glyph) {
    const auto it = m_glyphs.find(&glyph);
    if (it != m_glyphs.end()) return it->second;

    const swfr::ShapeHandle handle = whiteShapeMesh(glyph);
    if (!handle) return 0;
    m_glyphs.emplace(&glyph, handle);
    return handle;
}

std::uint32_t GpuRenderSink::deviceGlyphShape(const FontGlyphs& font, std::uint32_t code) {
    const auto key = std::make_pair(&font, code);
    const auto it = m_deviceGlyphs.find(key);
    if (it != m_deviceGlyphs.end()) return it->second;

    Shape outline;
    const swfr::ShapeHandle handle = deviceGlyphOutline(font, code, outline)
                                         ? whiteShapeMesh(outline) : 0;
    m_deviceGlyphs.emplace(key, handle);
    return handle;
}

std::uint32_t GpuRenderSink::maskShape(const Shape& shape) {
    const std::uint64_t hash = maskGeometryHash(shape);
    auto it = m_masks.find(&shape);
    if (it != m_masks.end() && it->second.hash == hash) return it->second.handle;

    swfr::ShapeDesc desc;
    desc.path = BuildMergedPath(shape);
    if (desc.path.empty()) return 0;
    desc.fillRule = swfr::FillRule::NonZero;
    desc.fill.kind = swfr::FillKind::Solid;
    desc.fill.color = { 1.0f, 1.0f, 1.0f, 1.0f };

    const swfr::ShapeHandle handle = m_renderer->CreateShape(desc);
    if (!handle) return 0;
    ++m_stats.shapesCreated;

    if (it != m_masks.end()) {
        // 同一地址换了内容：释放旧网格，就地更新（不会让缓存随帧数膨胀）。
        if (it->second.handle) m_renderer->ReleaseShape(it->second.handle);
        it->second = MaskEntry{ hash, handle };
    } else {
        m_masks.emplace(&shape, MaskEntry{ hash, handle });
    }
    return handle;
}

std::uint32_t GpuRenderSink::bitmapTexture(const Bitmap& bitmap) {
    const auto it = m_textures.find(&bitmap);
    if (it != m_textures.end()) return it->second;
    if (bitmap.width <= 0 || bitmap.height <= 0 || bitmap.rgba.empty()) return 0;

    swfr::TextureDesc desc;
    desc.width = static_cast<std::uint32_t>(bitmap.width);
    desc.height = static_cast<std::uint32_t>(bitmap.height);
    desc.pixels = bitmap.rgba.data();
    desc.stride = static_cast<std::uint32_t>(bitmap.width) * 4;

    const swfr::TextureHandle handle = m_renderer->CreateTexture(desc);
    if (!handle) return 0;
    ++m_stats.texturesCreated;
    m_textures.emplace(&bitmap, handle);
    return handle;
}

std::size_t GpuRenderSink::cachedShapeCount() const {
    std::size_t count = m_glyphs.size() + m_masks.size();
    for (const auto& entry : m_fills) count += entry.second.handles.size();
    for (const auto& entry : m_strokes) count += entry.second.handles.size();
    return count;
}

std::size_t GpuRenderSink::cachedTextureCount() const { return m_textures.size(); }

void GpuRenderSink::releaseGpuResources() {
    if (m_renderer) {
        for (auto& entry : m_fills)
            for (std::uint32_t handle : entry.second.handles)
                if (handle) m_renderer->ReleaseShape(handle);
        for (auto& entry : m_strokes)
            for (std::uint32_t handle : entry.second.handles)
                if (handle) m_renderer->ReleaseShape(handle);
        for (const auto& entry : m_glyphs) m_renderer->ReleaseShape(entry.second);
        for (const auto& entry : m_masks)
            if (entry.second.handle) m_renderer->ReleaseShape(entry.second.handle);
        for (const auto& entry : m_deviceGlyphs)
            if (entry.second) m_renderer->ReleaseShape(entry.second);
        for (const auto& entry : m_textures) m_renderer->ReleaseTexture(entry.second);
    }
    m_fills.clear();
    m_strokes.clear();
    m_glyphs.clear();
    m_masks.clear();
    m_deviceGlyphs.clear();
    m_textures.clear();
    m_maskPushed.clear();
}

// ---------------------------------------------------------------- 绘制命令

void GpuRenderSink::drawFill(const Shape& shape, std::size_t styleIndex, const Matrix& world,
                             const ColorTransform& cx, BlendMode blend, float alpha) {
    if (styleIndex >= shape.fillStyles.size()) return;
    const std::uint32_t handle = fillShape(shape, styleIndex);
    if (!handle) return;

    m_renderer->DrawShape(handle, ToDevice(world), ToColorTransform(cx, alpha),
                          ToBlendMode(blend, &m_stats));
    ++m_stats.fills;
}

void GpuRenderSink::drawStroke(const Shape& shape, std::size_t styleIndex, const Matrix& world,
                               const ColorTransform& cx, BlendMode blend, float alpha) {
    if (styleIndex >= shape.lineStyles.size()) return;
    const std::uint32_t handle = strokeShape(shape, styleIndex);
    if (!handle) return;

    m_renderer->DrawStroke(handle, ToDevice(world), ToColorTransform(cx, alpha),
                           ToBlendMode(blend, &m_stats));
    ++m_stats.strokes;
}

void GpuRenderSink::drawTransientFill(const Shape& shape, std::size_t styleIndex, const Matrix& world,
                                      const ColorTransform& cx, BlendMode blend, float alpha) {
    if (styleIndex >= shape.fillStyles.size()) return;
    const FillStyle& style = shape.fillStyles[styleIndex];
    swfr::ShapeDesc desc;
    desc.path = BuildFillPath(shape, styleIndex);
    if (desc.path.empty()) return;
    desc.fillRule = swfr::FillRule::NonZero;
    desc.fill.color = ToColor(style.color);
    const swfr::Mat2D gradientToShape = ToMat2D(style.gradientMatrix);
    const swfr::Mat2D shapeToGradient = Invert(gradientToShape);
    const swfr::Mat2D twipsScale = swfr::Mat2D::Scaling(kTwipsPerPx, kTwipsPerPx);
    switch (style.kind) {
        case FillKind::Solid: desc.fill.kind = swfr::FillKind::Solid; break;
        case FillKind::LinearGradient:
        case FillKind::RadialGradient:
        case FillKind::FocalGradient: {
            desc.fill.kind = (style.kind == FillKind::LinearGradient) ? swfr::FillKind::LinearGradient : swfr::FillKind::RadialGradient;
            swfr::Mat2D normalise; normalise.a = normalise.d = 1.0f / (2.0f * kGradientHalf); normalise.tx = normalise.ty = 0.5f;
            desc.fill.fillMatrix = Invert(swfr::Mat2D::Multiply(normalise, swfr::Mat2D::Multiply(shapeToGradient, twipsScale)));
            for (const GradientStop& stop : style.gradient) desc.fill.stops.push_back({ stop.ratio / 255.0f, ToColor(stop.color) });
            break;
        }
        case FillKind::Bitmap: {
            desc.fill.kind = swfr::FillKind::Bitmap;
            const Bitmap* bitmap = nullptr;
            if (m_dict) { auto it = m_dict->find(style.bitmapId); if (it != m_dict->end() && it->second) bitmap = it->second->bitmap.get(); }
            if (!bitmap || !bitmap->width || !bitmap->height) { desc.fill.kind = swfr::FillKind::Solid; break; }
            desc.fill.texture = bitmapTexture(*bitmap);
            if (!desc.fill.texture) { desc.fill.kind = swfr::FillKind::Solid; break; }
            const swfr::Mat2D toUv = swfr::Mat2D::Multiply(
                swfr::Mat2D::Scaling(1.0f / (float)bitmap->width, 1.0f / (float)bitmap->height),
                swfr::Mat2D::Multiply(shapeToGradient, twipsScale));
            desc.fill.fillMatrix = Invert(toUv);
            desc.fill.wrap = style.tiled;
            break;
        }
    }
    const swfr::ShapeHandle handle = m_renderer->CreateShape(desc);
    if (!handle) return;
    m_renderer->DrawShape(handle, ToDevice(world), ToColorTransform(cx, alpha), ToBlendMode(blend, &m_stats));
    m_renderer->ReleaseShape(handle);
    ++m_stats.fills;
}

void GpuRenderSink::drawTransientStroke(const Shape& shape, std::size_t styleIndex, const Matrix& world,
                                        const ColorTransform& cx, BlendMode blend, float alpha) {
    if (styleIndex >= shape.lineStyles.size()) return;
    if (styleIndex >= shape.strokePaths.size()) return;
    const LineStyle& style = shape.lineStyles[styleIndex];
    swfr::StrokeDesc desc;
    desc.path = BuildStrokePath(shape.strokePaths[styleIndex]);
    if (desc.path.empty()) return;
    desc.width = style.width * kPxPerTwip;
    desc.join = swfr::LineJoin::Round;
    desc.cap = swfr::LineCap::Round;
    desc.color = ToColor(style.color);
    const swfr::ShapeHandle handle = m_renderer->CreateStroke(desc);
    if (!handle) return;
    m_renderer->DrawStroke(handle, ToDevice(world), ToColorTransform(cx, alpha), ToBlendMode(blend, &m_stats));
    m_renderer->ReleaseShape(handle);
    ++m_stats.strokes;
}

void GpuRenderSink::drawGlyph(const Shape& glyph, const Matrix& world, const Color& color,
                              const ColorTransform& cx, BlendMode blend, float alpha) {
    const std::uint32_t handle = glyphShape(glyph);
    if (!handle) return;

    // 白色网格 + 颜色变换：字形颜色（已乘以祖先颜色变换）直接进乘项。
    swfr::ColorTransform tint = ToColorTransform(cx, alpha);
    tint.mul[0] *= color.r / 255.0f;
    tint.mul[1] *= color.g / 255.0f;
    tint.mul[2] *= color.b / 255.0f;
    tint.mul[3] *= color.a / 255.0f;

    m_renderer->DrawShape(handle, ToDevice(world), tint, ToBlendMode(blend, &m_stats));
    ++m_stats.glyphs;
}

void GpuRenderSink::drawEditText(const EditText& text, const Matrix& world, float alpha) {
    // 排版与软件光栅器共用 layoutEditText，再复用字形网格路径：字形轮廓仍作为
    // 白色网格 + 颜色乘项绘制，所以字号/字距/对齐与 CPU 完全一致。
    if (!m_dict) return;
    std::vector<EditTextGlyph> glyphs;
    layoutEditText(text, world, *m_dict, glyphs);
    if (std::getenv("SWF_ETEXT_DBG"))
        std::fprintf(stderr, "[etext] font=%u h=%u text=\"%s\" glyphs=%zu shapes=%zu\n",
                     (unsigned)text.fontId, (unsigned)text.fontHeight, text.text.c_str(),
                     glyphs.size(),
                     (std::size_t)std::count_if(glyphs.begin(), glyphs.end(),
                                                [](const EditTextGlyph& g) { return g.shape != nullptr; }));
    ++m_stats.editTexts;
    for (const auto& g : glyphs) {
        const std::uint32_t handle = g.shape ? glyphShape(*g.shape)
                                             : deviceGlyphShape(*g.font, g.code);
        if (!handle) {
            if (!g.shape) ++m_stats.editTextDeviceGlyphs;
            continue;
        }
        // 与 drawGlyph 一致：白网格 × 文本颜色 × 祖先透明度。
        swfr::ColorTransform tint = ToColorTransform(ColorTransform{}, alpha);
        tint.mul[0] *= text.color.r / 255.0f;
        tint.mul[1] *= text.color.g / 255.0f;
        tint.mul[2] *= text.color.b / 255.0f;
        tint.mul[3] *= text.color.a / 255.0f;
        m_renderer->DrawShape(handle, ToDevice(g.shape ? g.matrix : g.device), tint,
                              swfr::BlendMode::Normal);
        ++m_stats.editTextGlyphs;
    }
}

void GpuRenderSink::drawBitmap(const Bitmap& bitmap, const Matrix& world,
                               const ColorTransform& cx, BlendMode blend, float alpha) {
    const std::uint32_t texture = bitmapTexture(bitmap);
    if (!texture) return;

    // DrawTexture 的局部空间是纹理像素：先乘 20 回到 twips，再走显示变换。
    const swfr::Mat2D transform = ToDevice(world);
    const swfr::RectF source{ 0.0f, 0.0f,
                              static_cast<float>(bitmap.width),
                              static_cast<float>(bitmap.height) };
    m_renderer->DrawTexture(texture, source, transform, ToColorTransform(cx, alpha),
                            ToBlendMode(blend, &m_stats));
    ++m_stats.bitmaps;
}

void GpuRenderSink::pushMask(const std::vector<MaskShape>& masks) {
    ++m_stats.maskGroups;

    // 同一裁剪组的多个遮罩形状在软件端是并集（逐个 fillMask 累加）。
    // 这里全部提交：第一条开层，其余并入同一个模板位。
    bool pushed = false;
    std::size_t submitted = 0;
    for (const MaskShape& mask : masks) {
        if (!mask.shape) continue;
        const std::uint32_t handle = maskShape(*mask.shape);
        if (!handle) continue;
        if (!pushed) {
            m_renderer->PushMask(handle, ToDevice(mask.matrix));
            pushed = true;
            ++m_stats.masks;
        } else {
            m_renderer->AddMaskShape(handle, ToDevice(mask.matrix));
        }
        ++submitted;
    }

    if (masks.size() > submitted) m_stats.masksDropped += masks.size() - submitted;

    // popMask 必须与每次 pushMask 调用配对：空遮罩组不压栈，靠这个标记跳过弹出。
    m_maskPushed.push_back(pushed);
}

void GpuRenderSink::popMask() {
    if (m_maskPushed.empty()) {
        m_renderer->PopMask();   // 防御：遍历保证配对，这里只兜底
        return;
    }
    const bool pushed = m_maskPushed.back();
    m_maskPushed.pop_back();
    if (pushed) m_renderer->PopMask();
}

namespace {

// 与软件端 blurShapeOf 一致：σ = blurPx / 2（规范 2.1 的 kScale），
// blurX/blurY 在解码后是 twips。
struct BlurShape {
    float sx = 0.0f;
    float sy = 0.0f;
};

BlurShape BlurShapeOf(const Filter& f) {
    BlurShape bs;
    bs.sx = std::max(0.0f, f.blurX * kPxPerTwip) * 0.5f;
    bs.sy = std::max(0.0f, f.blurY * kPxPerTwip) * 0.5f;
    return bs;
}

}  // namespace

void GpuRenderSink::beginFilteredLayer(const std::vector<Filter>& filters) {
    ++m_stats.filterLayers;
    if (m_renderer->BeginLayer()) {
        m_filterStack.push_back(&filters);
    } else {
        // 离屏层创建失败（显存等）：内容照常画到当前目标，只是没有滤镜。
        m_filterStack.push_back(nullptr);
        ++m_stats.filtersDropped;
    }
}

// 滤镜按声明顺序作用于层：Blur 直接改层（离屏模糊），投影/辉光把一份模糊后的
// 层 alpha 着色合成到父目标（在层本体之前，故最终位于对象之后方），最后合成层本体。
void GpuRenderSink::endFilteredLayer() {
    if (m_filterStack.empty()) return;
    const std::vector<Filter>* filters = m_filterStack.back();
    m_filterStack.pop_back();
    if (!filters) return;   // BeginLayer 失败：内容已经直接画好

    for (const Filter& f : *filters) {
        if (std::getenv("SWF_GPU_DBG"))
            std::fprintf(stderr, "[filter] kind=%d blur=%.2ftw passes=%d dist=%.2ftw str=%.2f\n",
                         (int)f.kind, f.blurX, f.passes, f.distance, f.strength);
        switch (f.kind) {
            case FilterKind::Blur: {
                const BlurShape bs = BlurShapeOf(f);
                m_renderer->BlurLayer(bs.sx, bs.sy);
                ++m_stats.blurs;
                break;
            }

            case FilterKind::DropShadow:
            case FilterKind::Glow: {
                float offx = 0.0f, offy = 0.0f;
                if (f.kind == FilterKind::DropShadow) {
                    const double rad = f.angle * 3.14159265358979323846 / 180.0;
                    offx = static_cast<float>(f.distance * kPxPerTwip * std::cos(rad));
                    offy = static_cast<float>(f.distance * kPxPerTwip * std::sin(rad));
                }
                const BlurShape bs = BlurShapeOf(f);
                m_renderer->DrawLayerShadow(offx, offy, bs.sx, bs.sy, ToColor(f.color),
                                            std::max(0.0f, f.strength));
                ++m_stats.shadows;
                break;
            }

            default:
                // 斜角/渐变辉光/卷积/颜色矩阵尚未适配：只合成内容本体。
                ++m_stats.filtersDropped;
                break;
        }
    }

    m_renderer->EndLayer();
}

}  // namespace swf
