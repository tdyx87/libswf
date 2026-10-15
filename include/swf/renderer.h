#pragma once
// 渲染层抽象接口（移动端 GPU 友好：vendored nanovg / OpenGL ES）。
// 当前仅 NanoVG 后端可用；后续"解码层"（DefineShape / 位图 / 显示列表）完成后，
// 在 NanoVGRenderer 中填充真正的矢量绘制逻辑。Skia / OpenGLES 后端可经工厂扩展。
#include <cstdint>
#include <string>

namespace swf {

// 渲染后端枚举
enum class RenderBackend {
    NanoVG,    // vendored，OpenGL/OpenGL ES（移动端 GPU），当前可用
    Skia,      // 移动 GPU 加速（后续扩展，能力更优）
    OpenGLES   // 移动原生 GPU（后续扩展）
};

// 抽象渲染接口：占位骨架，具体绘制待解码层就绪后实现
class Renderer {
public:
    virtual ~Renderer() = default;

    // 准备一帧画布（width/height 单位：像素）
    virtual bool beginFrame(int width, int height) = 0;
    // 结束当前帧（flush）
    virtual void endFrame() = 0;
    // 导出为 PNG（无窗口环境/移动端验证用；需帧缓冲就绪）
    virtual bool savePNG(const std::string& path) = 0;

    virtual RenderBackend backend() const = 0;
};

// 工厂：依据后端创建渲染器（目前仅 NanoVG 可用，其余返回 nullptr）
Renderer* createRenderer(RenderBackend backend, int width, int height);

} // namespace swf
