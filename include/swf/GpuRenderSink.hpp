// GpuRenderSink.hpp
// 把显示列表遍历（swf::renderDisplayList）翻译成 swfr 渲染后端命令的适配器骨架。
//
// 覆盖静态帧的 **填充 / 描边 / 位图 / 字形轮廓**：
//   drawFill    -> swfr::Renderer::DrawShape（实色 / 线性 / 径向 / 位图填充）
//   drawStroke  -> swfr::Renderer::DrawStroke（线宽按 twips 换算，圆角连接）
//   drawBitmap  -> swfr::Renderer::DrawTexture
//   drawGlyph   -> 白色字形网格 + 颜色变换着色
//   pushMask    -> swfr::Renderer::PushMask + AddMaskShape（组内多条形状取并集）
//
//   beginFilteredLayer -> swfr::Renderer::BeginLayer（离屏层）
//   endFilteredLayer   -> BlurLayer / DrawLayerShadow / EndLayer
//
// 尚未适配（骨架阶段，见 Stats 计数；内容按“无该效果”绘制或直接跳过）：
//   - drawEditText：字符按字体嵌入轮廓绘制；仅“无嵌入字形”的设备字体回退字符不画
//   - 斜角 / 渐变辉光 / 卷积 / 颜色矩阵滤镜（只合成内容本体）
//   - AVM2 运行时舞台（Player::renderFrame 对 runtimeStage 非空直接返回 false）
//
// 本头文件不包含 D3D11 / swfr 头文件（只前向声明 swfr::Renderer），因此
// 不编译 GPU 后端的构建也能安全包含它；只有 GpuRenderSink.cpp 需要后端。
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

#include "swf/Decoder.hpp"
#include "swf/DisplayListSink.hpp"

namespace swfr {
class Renderer;
}

namespace swf {

class GpuRenderSink final : public IDisplayListSink {
public:
    // 诊断计数：确认一条帧实际走到了哪些命令，以及哪些效果被跳过。
    struct Stats {
        std::uint64_t fills = 0;
        std::uint64_t strokes = 0;
        std::uint64_t glyphs = 0;
        std::uint64_t bitmaps = 0;
        std::uint64_t editTexts = 0;      // 已排版的文本框（drawEditText 调用）
        std::uint64_t editTextGlyphs = 0;         // 文本框里已画出的嵌入字形
        std::uint64_t editTextDeviceGlyphs = 0;   // 无嵌入轮廓、需设备字体的字符（GPU 侧无法画）
        std::uint64_t masks = 0;
        std::uint64_t masksDropped = 0;   // 无法建网格/为空、没提交给后端的遮罩形状
        std::uint64_t maskGroups = 0;
        std::uint64_t filterLayers = 0;   // beginFilteredLayer 调用
        std::uint64_t blurs = 0;          // 已应用 Blur 滤镜
        std::uint64_t shadows = 0;        // 已应用 DropShadow / Glow
        std::uint64_t filtersDropped = 0; // 未适配的滤镜种类（斜角/卷积/颜色矩阵）
        std::uint64_t blendFallback = 0;  // 后端不支持的混合模式 -> Normal
        std::uint64_t shapesCreated = 0;
        std::uint64_t texturesCreated = 0;
    };

    explicit GpuRenderSink(swfr::Renderer& renderer);
    ~GpuRenderSink() override;

    GpuRenderSink(const GpuRenderSink&) = delete;
    GpuRenderSink& operator=(const GpuRenderSink&) = delete;

    // IDisplayListSink
    void beginFrame(int width, int height,
                    const std::map<std::uint16_t, std::shared_ptr<Character>>& dict) override;
    void endFrame() override;
    void drawFill(const Shape& shape, std::size_t styleIndex, const Matrix& world,
                  const ColorTransform& cx, BlendMode blend, float alpha) override;
    void drawStroke(const Shape& shape, std::size_t styleIndex, const Matrix& world,
                    const ColorTransform& cx, BlendMode blend, float alpha) override;
    void drawGlyph(const Shape& glyph, const Matrix& world, const Color& color,
                   const ColorTransform& cx, BlendMode blend, float alpha) override;
    void drawEditText(const EditText& text, const Matrix& world, float alpha) override;
    void drawBitmap(const Bitmap& bitmap, const Matrix& world,
                    const ColorTransform& cx, BlendMode blend, float alpha) override;
    void pushMask(const std::vector<MaskShape>& masks) override;
    void popMask() override;
    void beginFilteredLayer(const std::vector<Filter>& filters) override;
    void endFilteredLayer() override;

    // 绘制临时 Shape（不缓存、不按指针复用），用于运行时 graphics 等非字典形状。
    void drawTransientFill(const Shape& shape, std::size_t styleIndex, const Matrix& world,
                           const ColorTransform& cx, BlendMode blend, float alpha) override;
    void drawTransientStroke(const Shape& shape, std::size_t styleIndex, const Matrix& world,
                             const ColorTransform& cx, BlendMode blend, float alpha) override;

    const Stats& stats() const { return m_stats; }
    void resetStats() { m_stats = {}; }

    // 缓存里已创建的 GPU 资源数量（形状网格 / 位图纹理），供退出时释放。
    std::size_t cachedShapeCount() const;
    std::size_t cachedTextureCount() const;
    void releaseGpuResources();

private:
    std::uint32_t fillShape(const Shape& shape, std::size_t styleIndex);
    std::uint32_t strokeShape(const Shape& shape, std::size_t styleIndex);
    std::uint32_t glyphShape(const Shape& glyph);
    // 非嵌入（设备字体）字形：轮廓由 GDI 取出后当白色网格用，按（字体, 码点）缓存。
    std::uint32_t deviceGlyphShape(const FontGlyphs& font, std::uint32_t code);
    // 白色实色字形网格（字形轮廓 + 颜色变换着色），供上面两者共用。
    std::uint32_t whiteShapeMesh(const Shape& shape);
    std::uint32_t maskShape(const Shape& shape);
    std::uint32_t bitmapTexture(const Bitmap& bitmap);

    swfr::Renderer* m_renderer = nullptr;
    const std::map<std::uint16_t, std::shared_ptr<Character>>* m_dict = nullptr;
    int m_width = 0;
    int m_height = 0;

    // 网格缓存键 = Shape 地址 + 内容哈希。Morph 插值每帧新建 Shape，分配器会复用
    // 同一地址；只看地址会命中上一帧的网格，导致形状"冻住"（启动进度条 c77 的
    // morph 填充即如此：数字在走，浅紫色条一直是空的）。
    struct ShapeMeshEntry {
        std::uint64_t hash = 0;
        std::vector<std::uint32_t> handles;   // style -> handle
    };
    std::unordered_map<const Shape*, ShapeMeshEntry> m_fills;
    std::unordered_map<const Shape*, ShapeMeshEntry> m_strokes;
    std::unordered_map<const Shape*, std::uint32_t> m_glyphs;                // 白色字形网格
    std::map<std::pair<const FontGlyphs*, std::uint32_t>, std::uint32_t> m_deviceGlyphs;
    // 实色遮罩网格。缓存键是 Shape 地址 + 几何哈希：遮罩形状可能是每帧新建的
    // 插值 Morph（地址被分配器复用），只按地址命中会拿到旧帧的网格——加载进度条
    // （cid721 遮罩）在窗口播放器里因此永远停在首帧的空遮罩、进度条填不满。
    struct MaskEntry { std::uint64_t hash = 0; std::uint32_t handle = 0; };
    std::unordered_map<const Shape*, MaskEntry> m_masks;
    std::unordered_map<const Bitmap*, std::uint32_t> m_textures;
    std::vector<bool> m_maskPushed;   // 与 pushMask/popMask 调用配对（空遮罩组不压栈）
    // 与 begin/endFilteredLayer 配对；nullptr 表示离屏层创建失败（内容不带滤镜）。
    std::vector<const std::vector<Filter>*> m_filterStack;

    Stats m_stats;
};

}  // namespace swf
