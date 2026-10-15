// 渲染层 NanoVG 后端桩。
// 仅打通"编译 vendored nanovg + 持有 NVGcontext 句柄"链路以验证依赖与链接；
// 真正的 SWF 矢量绘制（DefineShape 填充/线条、位图、显示列表合成）在解码层完成后填充。
//
// 注：nvgCreateGL3() 需要宿主提供 OpenGL/OpenGL ES 上下文，故留待真正渲染阶段
// （带窗口/EGL 上下文）再调用，这里只占位。
#include "swf/renderer.h"
#include "nanovg.h"   // vendored

namespace swf {

class NanoVGRenderer : public Renderer {
public:
    NanoVGRenderer(int w, int h) : m_w(w), m_h(h), m_vg(nullptr) {}
    ~NanoVGRenderer() override {
        // NVGcontext 由 nvgCreateGL3 创建、应由 nvgDeleteGL3 销毁；
        // 真正渲染阶段再配对。此桩未创建上下文，故不销毁。
    }

    bool beginFrame(int w, int h) override {
        m_w = w; m_h = h;
        // 真正绘制：nvgBeginFrame(m_vg, w, h, dpr); ... 待 GL 上下文就绪
        return true;
    }

    void endFrame() override {
        // 真正绘制：nvgEndFrame(m_vg); 待 GL 上下文就绪
    }

    bool savePNG(const std::string&) override {
        // 占位：需帧缓冲（FBO/截图）就绪后实现
        return false;
    }

    RenderBackend backend() const override { return RenderBackend::NanoVG; }

private:
    int        m_w = 0, m_h = 0;
    NVGcontext* m_vg = nullptr;   // 由 nvgCreateGL3 创建（渲染阶段）
};

Renderer* createRenderer(RenderBackend backend, int w, int h) {
    switch (backend) {
        case RenderBackend::NanoVG:
            return new NanoVGRenderer(w, h);
        default:
            return nullptr; // Skia / OpenGLES 待实现
    }
}

} // namespace swf
