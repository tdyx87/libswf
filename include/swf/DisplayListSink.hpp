// DisplayListSink.hpp
// 显示列表遍历的输出接口（sink）。
//
// Render.cpp 的主时间轴遍历（Sprite/Button/clipDepth/滤镜/文本/位图/视频）
// 只决定“画什么、按什么顺序画”，具体像素或 GPU 命令由 sink 完成：
//   * SoftwareSink（Render.cpp 内部）：落到 CPU RGBA 缓冲，与拆分前逐像素一致；
//   * GPU 后端（例如 D3D11 Renderer）：转成网格/纹理/状态命令。
//
// 坐标约定：Shape 局部坐标与 Matrix 都是 SWF twips；Matrix 直接映射到设备像素。

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "swf/Decoder.hpp"

namespace swf {

// 遮罩几何：形状 + 形状局部（twips）到设备像素的矩阵。
struct MaskShape {
    const Shape* shape = nullptr;
    Matrix matrix;
};

class IDisplayListSink {
public:
    virtual ~IDisplayListSink() = default;

    // 每次遍历开始/结束。dict 用于解析位图填充（FillStyle::bitmapId）与 EditText 字体。
    virtual void beginFrame(int width, int height,
                            const std::map<std::uint16_t, std::shared_ptr<Character>>& dict) = 0;
    virtual void endFrame() = 0;

    // 形状的单个填充/描边样式。alpha 已含对象级透明度，cx 已含祖先累积颜色变换。
    virtual void drawFill(const Shape& shape, std::size_t styleIndex, const Matrix& world,
                          const ColorTransform& cx, BlendMode blend, float alpha) = 0;
    virtual void drawStroke(const Shape& shape, std::size_t styleIndex, const Matrix& world,
                            const ColorTransform& cx, BlendMode blend, float alpha) = 0;

    // 一个形状的全部填充/描边（按 drawOrder 排序；无 drawOrder 时先填充后描边）。
    // 软件后端重写此方法，把同一形状的多个填充按覆盖率累积后一次合成，
    // 消除相邻填充之间漏背景造成的 1px 接缝亮线；默认实现即逐个样式绘制。
    virtual void drawShape(const Shape& shape, const Matrix& world, const ColorTransform& cx,
                           BlendMode blend, float alpha) {
        if (!shape.drawOrder.empty()) {
            for (const auto& op : shape.drawOrder) {
                if (op.style < 0) continue;
                if (op.isLine)
                    drawStroke(shape, static_cast<std::size_t>(op.style), world, cx, blend, alpha);
                else
                    drawFill(shape, static_cast<std::size_t>(op.style), world, cx, blend, alpha);
            }
            return;
        }
        for (std::size_t i = 0; i < shape.fillStyles.size(); ++i)
            drawFill(shape, i, world, cx, blend, alpha);
        for (std::size_t i = 0; i < shape.lineStyles.size(); ++i)
            drawStroke(shape, i, world, cx, blend, alpha);
    }

    // 运行时 graphics 产生的临时形状（不属于影片字典，形状对象每帧都可能重建）。
    // 后端若按形状指针缓存网格（GPU）必须重写这两个方法，不能直接走 drawFill 的缓存。
    virtual void drawTransientFill(const Shape& shape, std::size_t styleIndex, const Matrix& world,
                                   const ColorTransform& cx, BlendMode blend, float alpha) {
        drawFill(shape, styleIndex, world, cx, blend, alpha);
    }
    virtual void drawTransientStroke(const Shape& shape, std::size_t styleIndex, const Matrix& world,
                                     const ColorTransform& cx, BlendMode blend, float alpha) {
        drawStroke(shape, styleIndex, world, cx, blend, alpha);
    }

    // DefineText：字形轮廓以纯色填充；颜色取自 TEXTRECORD（不经 cx）。
    virtual void drawGlyph(const Shape& glyph, const Matrix& world, const Color& color,
                           const ColorTransform& cx, BlendMode blend, float alpha) = 0;

    // DefineEditText：文本排版与字体回退由实现负责（软件端复用 renderEditText）。
    virtual void drawEditText(const EditText& text, const Matrix& world, float alpha) = 0;

    // 位图 / 视频帧（视频帧按位图绘制）。
    virtual void drawBitmap(const Bitmap& bitmap, const Matrix& world,
                            const ColorTransform& cx, BlendMode blend, float alpha) = 0;

    // clipDepth 裁剪组：期间的绘制被 masks 的并集裁剪。
    virtual void pushMask(const std::vector<MaskShape>& masks) = 0;
    virtual void popMask() = 0;

    // PlaceObject3 FilterList：期间的绘制先进入离屏层，结束时应用滤镜并合成。
    virtual void beginFilteredLayer(const std::vector<Filter>& filters) = 0;
    virtual void endFilteredLayer() = 0;
};

}  // namespace swf
