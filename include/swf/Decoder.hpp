// 软件渲染所需的 SWF 数据模型 + 解码器。
// 目标：把 SWF 解码成"字典(角色) + 时间轴帧(显示列表)"，供光栅器逐帧绘制。
// 坐标单位：SWF 内部为 TWIPS（1/20 像素）。
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "swf/Abc.hpp"
#include "swf/Fvm.hpp"

namespace swf {

// ---- 基础类型 ----
struct Color {
    std::uint8_t r = 0, g = 0, b = 0, a = 255;
    Color() = default;
    Color(std::uint8_t r_, std::uint8_t g_, std::uint8_t b_, std::uint8_t a_ = 255)
        : r(r_), g(g_), b(b_), a(a_) {}
};

// 2x3 仿射矩阵（SWF MATRIX）：out = (sx*x + r0*y + tx, r1*x + sy*y + ty)
struct Matrix {
    float sx = 1, sy = 1;   // 缩放
    float r0 = 0, r1 = 0;   // 错切/旋转
    float tx = 0, ty = 0;   // 平移（像素）
    Matrix() = default;
    Matrix post(const Matrix& o) const;   // 组合：this * o
    void apply(float x, float y, float& ox, float& oy) const;
    bool invert(Matrix& out) const;        // 求逆（仿射）
};

struct RectB {
    std::int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

// 颜色变换（CXFORM / CXFORMWITHALPHA）
struct ColorTransform {
    bool hasMult = false, hasAdd = false;
    float mr = 1, mg = 1, mb = 1, ma = 1;     // 乘项（/256）
    float ar = 0, ag = 0, ab = 0, aa = 0;     // 加项（/256）
    Color apply(const Color& c) const;
};

// ---- 填充样式 ----
enum class FillKind { Solid, LinearGradient, RadialGradient, FocalGradient, Bitmap };

struct GradientStop {
    std::uint8_t ratio = 0;  // 0..255
    Color color;
};

struct FillStyle {
    FillKind kind = FillKind::Solid;
    Color color;                            // 纯色用
    Matrix gradientMatrix;                  // 渐变/位图用（形状局部坐标 -> 渐变/纹理空间）
    std::vector<GradientStop> gradient;
    float focalX = 0.0f, focalY = 0.0f;     // 焦点渐变焦点（相对渐变中心，归一化半径）
    std::uint16_t bitmapId = 0;             // 位图填充用
    bool tiled = true;                      // 平铺 / 裁剪
};

struct LineStyle {
    float width = 1;       // 线宽（twips）
    Color color;
};

// 轮廓：点序列（曲线已拍平为线段）。闭合多边形。
struct Contour {
    std::vector<float> xs;
    std::vector<float> ys;
};

// 解码后的形状（几何，未光栅化）
struct Shape {
    RectB bounds;
    std::vector<FillStyle> fillStyles;
    std::vector<LineStyle> lineStyles;
    // 每个填充样式一组轮廓（even-odd 填充，可含洞）
    std::vector<std::vector<Contour>> fillPaths;
    // 每个线样式一组轮廓（描边）
    std::vector<std::vector<Contour>> strokePaths;
};

// 解码后的位图（已转 RGBA）
struct Bitmap {
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgba;   // width*height*4
};

// 角色（字典项）。前向声明：DisplayItem 需持有 shared_ptr<Character>。
struct Character;
struct Bitmap;

// 显示列表项（PlaceObject 实例化后的一个角色）
struct DisplayItem {
    std::uint16_t depth = 0;
    std::shared_ptr<Character> character;
    Matrix matrix;                  // 实例变换
    ColorTransform cxform;           // 颜色变换
    std::uint16_t clipDepth = 0;    // 0 = 非裁剪
    // 裁剪遮罩：被谁裁剪（由 clipDepth 关系决定，渲染时处理）
    int birthFrame = 0;              // 本层时间轴上被放置的帧号（精灵据此对齐内部帧）
    float ratio = 0;                // MorphShape 变形比例 0..1
    BlendMode blend = BlendMode::Normal;   // 混合模式
    bool hasMask = false;           // 本对象作为遮罩（自身不绘制，仅裁剪后续对象）
};

// 一帧：当前显示列表快照
struct Frame {
    std::vector<DisplayItem> items;   // 按 depth 升序
};

// 字体（DefineFont2/3）：字形轮廓（按字形索引；索引 0 = .notdef）
struct FontGlyphs {
    std::vector<std::shared_ptr<Shape>> shapes;
    std::vector<int> advances;   // 每字形前进宽度（twips）
    std::map<int, int> codeToGlyph;   // 字符码 -> 字形索引（运行时文本渲染用）
};

// 文本中的一个字形放置（DefineText/Text2 解码结果）
struct TextGlyph {
    std::uint16_t fontId = 0;
    std::uint16_t glyphIndex = 0;
    int advance = 0;
    Color color;
    int x = 0, y = 0;           // 相对文本矩阵原点的偏移（twips）
};

struct TextDef {
    Matrix textMatrix;
    std::vector<TextGlyph> glyphs;
    int leading = 0;
};

// 变形形状（DefineMorphShape）：t=0 起始形状 与 t=1 终止形状（样式索引对齐）
struct MorphShape {
    std::shared_ptr<Shape> start;
    std::shared_ptr<Shape> end;
};

// 角色（字典项）
struct Character {
    enum class Kind { Shape, Bitmap, Sprite, Font, Text, Morph } kind = Kind::Shape;
    std::shared_ptr<Shape> shape;
    std::shared_ptr<Bitmap> bitmap;
    std::shared_ptr<FontGlyphs> font;   // Kind::Font
    std::shared_ptr<TextDef> text;      // Kind::Text
    std::shared_ptr<MorphShape> morph;  // Kind::Morph
    // Sprite（DefineSprite）内部时间轴的各帧显示列表
    std::vector<Frame> spriteFrames;
};

// 整部影片
struct Movie {
    Color backgroundColor = Color(255, 255, 255);
    RectB frameSize;                  // twips
    float frameRate = 12;
    std::uint32_t frameCount = 0;
    std::map<std::uint16_t, std::shared_ptr<Character>> characters;  // characterId -> 角色
    std::vector<Frame> frames;
    // DoABC 中的 AVM2 字节码（供 AVM2 解释器执行，构建运行时显示列表）
    std::vector<abc::AbcFile> abcFiles;
    // AVM2 执行后由 ActionScript 构建的运行时显示列表（舞台根节点集合）
    std::vector<std::shared_ptr<fvm::DisplayObject>> runtimeStage;
    // SymbolClass(tag 77)：AS3 类名 -> 角色 id（AS 实例化类时取对应角色来画）
    std::map<std::string, std::uint16_t> symbolClasses;
    std::shared_ptr<Character> getCharacter(std::uint16_t id) const;
};

// 解码整个 SWF（body 已解压的字节流）。返回是否成功，错误写入 err。
bool decodeMovie(const std::uint8_t* data, std::size_t size, Movie& out, std::string& err);

}  // namespace swf
