// 软件光栅器：把解码后的 Movie 某一帧渲染成 RGBA 缓冲并写出 PNG。
// 不依赖任何 GPU/GL，纯 CPU，跨平台（含 CI / 移动端）可直接运行。
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "swf/Decoder.hpp"
#include "swf/DisplayListSink.hpp"

namespace swf {
// 显示列表遍历入口：按 depth 顺序处理 Sprite/Button/clipDepth 裁剪组/滤镜层，把
// 每次填充、描边、字形、文本框、位图按绘制顺序交给 sink。坐标与颜色变换语义与
// renderItemsWithClip（软件光栅）完全一致；GPU 后端只需实现 IDisplayListSink。
// root 为「显示列表局部 twips -> 设备像素」的根矩阵；alpha 为祖先透明度乘子。
void renderDisplayList(IDisplayListSink& sink, const std::vector<DisplayItem>& items,
                       const Matrix& root,
                       const std::map<std::uint16_t, std::shared_ptr<Character>>& dict,
                       int frameIndex, float alpha = 1.0f,
                       const ColorTransform& inheritedCx = ColorTransform{});

// 文本框排版结果：一个字符一次绘制。渲染器（软件光栅 / GPU sink）共用同一套
// 字体度量、字距、折行与对齐，避免两边各写一份导致字形位置/字号分叉。
struct EditTextGlyph {
    const Shape* shape = nullptr;     // 嵌入字形轮廓；nullptr 表示需要设备字体
    const FontGlyphs* font = nullptr; // 字体（设备字体回退时取名字/粗斜体）
    Matrix matrix;                    // 嵌入字形：局部 twips -> 设备像素
    Matrix device;                    // 设备字体轮廓矩阵（shape == nullptr 时有效）
    std::uint32_t code = 0;
};

// 设备字体（字体里没有嵌入字形）某字符的轮廓，归一化到 em=1024 的设计单位、
// y 向下，写进 out.fillPaths（实色白填充）。找不到时返回 false。
// 软件光栅器用 fillContours 直接填充，GPU sink 用它建网格，两边字形才能一致。
bool deviceGlyphOutline(const FontGlyphs& font, std::uint32_t code, Shape& out);

// 按 DefineEditText 的字体/边界/对齐/边距/行距/折行参数排版 et，输出每个可见字符的
// 绘制项（空格与换行只推进笔位，不输出）。world 为"文本框局部 twips -> 设备像素"。
void layoutEditText(const EditText& et, const Matrix& world,
                    const std::map<std::uint16_t, std::shared_ptr<Character>>& dict,
                    std::vector<EditTextGlyph>& out);

// 用任意 sink 渲染 AVM2 运行时舞台（movie.runtimeStage）。
void renderRuntimeStageToSink(IDisplayListSink& sink, const Movie& movie,
                              const Matrix& root, int frameIndex);
// 渲染 movie 的第 frameIndex 帧并写出 PNG。返回是否成功。
bool renderMovieToPNG(const Movie& movie, int frameIndex, const char* path);

// 与 renderMovieToPNG 相同的光栅化，但把 RGBA 写入 out（W*H*4），供像素级断言/测试。
// targetW/targetH > 0 时按该设备尺寸重新光栅化（矢量/字形在放大窗口下依然清晰）；
// 传 0 或不传则输出舞台原生尺寸。返回是否成功（frameIndex 越界返回 false）。
bool renderMovieToBuffer(const Movie& movie, int frameIndex,
                         std::vector<std::uint8_t>& out, int& W, int& H,
                         int targetW = 0, int targetH = 0);

// 用**指定的显示列表**（Player 已应用运行时覆盖/按钮状态的可变副本）渲染一帧。
// items 的 character 前向引用已解析；frameIndex 用于精灵内部帧对齐。
// 背景/尺寸与 renderMovieToBuffer 一致；AVM2 runtimeStage 照常叠加。
bool renderFrameWithItems(const Movie& movie, const std::vector<DisplayItem>& items,
                          int frameIndex,
                          std::vector<std::uint8_t>& out, int& W, int& H,
                          int targetW = 0, int targetH = 0);

// 命中测试：在给定显示列表上检测舞台像素点 (x, y)（左上原点）命中的最上层项。
// 返回 items 下标；未命中返回 -1。递归进入 Sprite 内部帧与 Button 的 Hit 态。
int hitTestItems(const Movie& movie, const std::vector<DisplayItem>& items,
                 double x, double y, int frameIndex);

// 命中测试（递归解析到最上层可交互项）：chain 为自顶层到命中项的显示项链条
// （顶层 -> … -> 命中的 Button/EditText/精灵）。命中返回 true。
// 与 hitTestItems 不同，本函数会深入 Sprite 内部，从而定位到嵌套的 Button
// （swf.swf 的商店/结算界面按钮都嵌在精灵中，顶层扫描无法命中）。
bool hitTestItemsDeep(const Movie& movie, const std::vector<DisplayItem>& items,
                      double x, double y, int frameIndex,
                      std::vector<const DisplayItem*>& chain);

// AVM2 运行时显示树（fvm::DisplayObject）命中测试：x,y 为舞台像素（左上原点）。
// chain 输出自根到命中最深对象的链条；未命中返回 false。
bool hitTestRuntimeStage(const Movie& movie,
                         const std::vector<std::shared_ptr<fvm::DisplayObject>>& roots,
                         double x, double y,
                         std::vector<std::shared_ptr<fvm::DisplayObject>>* chain);

// 单个运行时显示对象（含其后代）的命中测试，对应 AS3 DisplayObject.hitTestPoint。
// x,y 为舞台像素；shapeFlag=false 用包围盒，true 用实际几何（同级命中测试一致）。
bool hitTestRuntimeObject(const Movie& movie, const std::shared_ptr<fvm::DisplayObject>& d,
                          double x, double y, bool shapeFlag);

// 舞台根矩阵：显示列表局部 twips -> 设备像素（targetW/targetH 为 0 时输出舞台
// 原生像素尺寸）。renderFrameWithItems 与 GPU 适配器共用同一套换算。
Matrix stageRootMatrix(const Movie& movie, int& W, int& H, int targetW = 0, int targetH = 0);

// 渲染单个 AVM2 运行时显示对象（含其子树）到 RGBA 缓冲（W*H*4，左上原点）。
// 公开入口：单测/内嵌渲染使用；包含 AS3 Graphics 绘制命令回放、运行时文本与子树递归。
// parent 为"局部 twips -> 设备像素"的根矩阵（纯缩放场景用 sx=sy=1/20）。
void renderRuntimeObject(const std::shared_ptr<fvm::DisplayObject>& d, const Matrix& parent,
                         const Movie& movie, std::uint8_t* buf, int W, int H,
                         int frameIndex, float alpha = 1.0f);

// 静态帧跳过：计算"渲染输入集"的指纹。renderFrameWithItems 的输出是该输入集的
// 确定性函数（同输入必同像素），因此 Player::render 在指纹与上一帧相同时可安全
// 复用旧缓冲，省掉整帧软件光栅化。
// 覆盖：backgroundColor、frameSize（根矩阵）、targetW/H、runtimeStage 整棵树
// （逐节点内容项 + 子节点/mask/filters 指针）、以及 frameIndex 当且仅当它确实
// 被渲染器消费（与 renderDisplayObjectImpl 的 fIdx 规则一致：运行时树中仅
// !timelineBuilt 且非"带正播放头 MovieClip"的节点按根帧号取帧；AVM1 遗留分支
// 的 Sprite/Video 帧选择恒依赖根帧号）。
std::uint64_t sceneFingerprint(const Movie& movie, const std::vector<DisplayItem>& legacyItems,
                               int frameIndex, int targetW, int targetH);

}  // namespace swf
