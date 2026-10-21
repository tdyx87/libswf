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
#include "swf/Avm1.hpp"
#include "swf/Fvm.hpp"

namespace swf {

struct VideoStream;   // 前向声明（Character::video 使用）

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

// 有向填充边。SWF 的每条边同时属于它两侧的填充样式：
//   fillStyle0 = 边**左侧**的填充，fillStyle1 = 边**右侧**的填充。
// 因此某个填充样式 s 的区域 = 所有"s 在其侧"的边所围成的区域。
// 按边记录方向与所在侧（sign = +1 表示该样式在边右侧，-1 表示在左侧），
// 渲染时用非零环绕（winding != 0）判定内外，可同时正确处理**并集**（部分重叠的
// 同向轮廓）与**洞**（嵌套且反向的轮廓），无需任何启发式猜测。
struct FillEdge {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int sign = 1;
};

// 解码后的形状（几何，未光栅化）
struct Shape {
    RectB bounds;
    std::vector<FillStyle> fillStyles;
    std::vector<LineStyle> lineStyles;
    // 每个填充样式一组轮廓（点序列；供变形插值 / 遮罩 / 回退使用）
    std::vector<std::vector<Contour>> fillPaths;
    // 每个填充样式的**有向边**（非零环绕填充的权威几何）
    std::vector<std::vector<FillEdge>> fillEdges;
    // 每个线样式一组轮廓（描边）
    std::vector<std::vector<Contour>> strokePaths;
    // 绘制顺序：形状记录流中每条样式路径被刷新的先后（每种样式记首次）。
    // Flash 按记录流顺序绘制——后出现的填充会盖住先出现的描边（例如
    // StateNewStyles 追加的渐变层覆盖前一批的描边）。为空时回退到"先填充后描边"。
    struct DrawOp { bool isLine; int style; };
    std::vector<DrawOp> drawOrder;
    // DefineShape4 的 UsesFillWindingRule：true 表示该形状用非零环绕规则填充
    // （仅 tag 83 提供该标志；其它版本按偶奇/嵌套规则处理）
    bool usesWinding = false;
};

// 解码后的位图（已转 RGBA）
struct Bitmap {
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgba;   // width*height*4
};

// 角色（字典项）。前向声明：DisplayItem 需持有 shared_ptr<Character>。
struct Character;
struct Bitmap;

// 位图滤镜（PlaceObject3 FilterList）：渲染器实现 DropShadow / Blur / Glow / Bevel /
// GradientGlow / GradientBevel / Convolution / ColorMatrix。
enum class FilterKind { DropShadow, Blur, Glow, Bevel, GradientGlow, GradientBevel, Convolution, ColorMatrix };
struct Filter {
    FilterKind kind = FilterKind::DropShadow;
    Color color{0, 0, 0, 255};          // 投影/辉光颜色
    Color highlight{255, 255, 255, 255}; // 斜角高光色
    float blurX = 0, blurY = 0;         // 模糊半径（twips）
    float angle = 0, distance = 0;      // 投影角度(度)/距离(twips)
    float strength = 0;                 // 强度
    bool inner = false, knockout = false, onTop = false;
    int passes = 1;                     // 模糊迭代次数
    // —— 渐变辉光 / 渐变斜角 ——
    std::vector<Color> gradientColors;             // 渐变色停
    std::vector<std::uint8_t> gradientRatios;      // 渐变位置 0..255（升序）
    std::uint8_t gradientType = 2;                // 0=Inner 1=Outer 2=Full
    // —— 卷积 ——
    std::vector<float> convMatrix;                // mx*my 卷积核
    int convW = 0, convH = 0;                     // 核尺寸
    float convDivisor = 1, convBias = 0;          // 除数 / 偏移
    Color convColor{0, 0, 0, 0};                  // 越界默认色
    bool convClamp = true, convPreserveAlpha = false;
    // —— 颜色矩阵（4x5，行主序）——
    float colorMatrix[20] = {1,0,0,0,0, 0,1,0,0,0, 0,0,1,0,0, 0,0,0,1,0};
};

// 显示列表项（PlaceObject 实例化后的一个角色）
struct DisplayItem {
    std::uint16_t depth = 0;
    std::shared_ptr<Character> character;
    // 放置时引用的角色 id。SWF 允许**前向引用**（精灵里引用的形状可能定义在其后），
    // 解析期内查不到就会留下空指针；渲染时用该 id 在角色表里补查一次。
    std::uint16_t characterId = 0;
    Matrix matrix;                  // 实例变换
    ColorTransform cxform;           // 颜色变换
    std::uint16_t clipDepth = 0;    // 0 = 非裁剪
    // 裁剪遮罩：被谁裁剪（由 clipDepth 关系决定，渲染时处理）
    int birthFrame = 0;              // 本层时间轴上被放置的帧号（精灵据此对齐内部帧）
    float ratio = 0;                // MorphShape 变形比例 0..1
    BlendMode blend = BlendMode::Normal;   // 混合模式
    bool hasMask = false;           // 本对象作为遮罩（自身不绘制，仅裁剪后续对象）
    std::vector<Filter> filters;    // PlaceObject3 滤镜列表
    // 实例名（PlaceObject2/3 Name 字段）。AVM1 脚本用 _root.<name> 寻址。
    std::string name;
    // ClipActions（PlaceObject2/3）：事件位 -> 对应脚本（load/enterFrame/mouse* 等）。
    std::map<std::uint32_t, avm1::Script> clipEvents;
    // ClipActions 的 Load/Initialize 事件只触发一次（放置时）。该标记跨帧继承。
    bool clipInited = false;
    // 剪辑作用域变量（AVM1：剪辑帧脚本/ClipEvent 里的无前缀变量即该实例的属性）。
    // 用共享指针使"同一实例在显示列表逐帧重建"时其属性得以持久（见 adoptRuntime）。
    std::shared_ptr<std::map<std::string, avm1::Value>> clipVars;
    // 按钮交互态（仅 Kind::Button 的实例）：0=Up 1=Over 2=Down 3=Hit
    int buttonState = 0;
    // ---- 运行时覆盖（AVM1 脚本 SetProperty / EditText 输入）----
    // AVM1 的 _x/_y/_xscale/_alpha 等写入落在这里；渲染时若 hasRt 则以这些值
    // 重新合成矩阵/透明度，替代静态解析出的 matrix/cxform。
    bool hasRt = false;
    double rtX = 0, rtY = 0;             // 像素（非 twips）
    double rtScaleX = 1, rtScaleY = 1;
    double rtRotation = 0;               // 度
    double rtAlpha = 1;                  // 0..1
    bool rtVisible = true;
    // Sprite 内部时间轴运行时控制（AVM1 gotoAndPlay 到命名精灵）
    int rtFrame = -1;                   // >=0 时替代 birthFrame 对齐出的内部帧
    bool rtPlaying = true;              // false = 停在 rtFrame
    // EditText 运行时文本（输入框编辑后覆盖 InitialText）
    std::string textOverride;
    bool hasTextOverride = false;
    // Sprite 内部时间轴：实例化时执行该精灵第 1 帧脚本（Flash 语义）。若脚本调用 stop()
    // （常见于被 /:busy、/:stick 等变量"开关"的效果精灵），精灵应停在首帧不外播；
    // 否则沿用按放置帧对齐的自动推进。
    bool spInit = false;                 // 是否已执行过第 1 帧脚本
    bool spStopped = false;              // 脚本把该精灵时间轴停住（不外播）
    // 精灵实例当前内部帧的**运行态**显示列表（递归树）。静态帧快照（Character::spriteFrames）
    // 只描述"最初放什么"，本字段则承载每帧重建后的实例运行态（rtFrame/按钮态/嵌套子树…），
    // 使嵌套精灵拥有独立播放头、嵌套按钮可被 keyPress 命中。
    std::vector<DisplayItem> children;
};

// AVM1 ClipEventFlags 事件位（SWF 规范）
enum Avm1ClipEvent : std::uint32_t {
    AVM1_EVENT_LOAD           = 0x00000001,
    AVM1_EVENT_ENTER_FRAME    = 0x00000002,
    AVM1_EVENT_UNLOAD         = 0x00000004,
    AVM1_EVENT_MOUSE_MOVE     = 0x00000008,
    AVM1_EVENT_MOUSE_DOWN     = 0x00000010,
    AVM1_EVENT_MOUSE_UP       = 0x00000020,
    AVM1_EVENT_KEY_DOWN       = 0x00000040,
    AVM1_EVENT_KEY_UP         = 0x00000080,
    AVM1_EVENT_DATA           = 0x00000100,
    AVM1_EVENT_INITIALIZE     = 0x00000200,
    AVM1_EVENT_RELEASE        = 0x00000400,   // 按下并释放（click）
    AVM1_EVENT_RELEASE_OUTSIDE= 0x00000800,
    AVM1_EVENT_ROLL_OUT       = 0x00001000,
    AVM1_EVENT_ROLL_OVER      = 0x00002000,
    AVM1_EVENT_DRAG_OUT       = 0x00004000,
    AVM1_EVENT_DRAG_OVER      = 0x00008000,
    // KeyPress（CLIPACTIONRECORD 带 KeyCode UI8）——CLIPEVENTFLAGS 第 17 位。
    AVM1_EVENT_KEY_PRESS      = 0x00020000,
    AVM1_EVENT_CONSTRUCT      = 0x00400000,
};

// 每帧触发的声音事件（StartSound / StartSound2 / SoundStreamBlock）
struct SoundEvent {
    std::uint16_t soundId = 0;
    bool stop = false;             // StartSound 的 Stop 标志
    bool loop = false;             // 循环播放
    int streamBlock = -1;          // >=0 时：该帧的流式声音块下标（SoundStreamBlock）
};

// 一帧：当前显示列表快照
struct Frame {
    std::vector<DisplayItem> items;   // 按 depth 升序
    std::vector<avm1::Script> doActions;  // 本帧的 DoAction（时间轴脚本）
    // 本帧触发的声音（StartSound 按遇及顺序；流式块的 streamBlock 为块下标）
    std::vector<SoundEvent> soundEvents;
};

// 按钮（DefineButton/DefineButton2）：四态显示列表 + 按钮动作脚本。
// 每个状态是一组 DisplayItem（同主时间轴）；actions 的键为 Button2 鼠标条件位序号（cond & (1<<bit)）：
//   0=IdleToOverUp(rollOver) 1=OverUpToIdle(rollOut) 2=OverUpToOverDown(press)
//   3=OverDownToOverUp(release) 4=OverDownToIdle(releaseOutside)
struct ButtonDef {
    std::vector<DisplayItem> up, over, down, hit;
    std::map<int, avm1::Script> actions;    // 鼠标条件位(0..8) -> 脚本
    // keyPress 按钮：CondFlags 的 bits 9..15 为按键码（keyCode = CondFlags >> 9），
    // 键码采用 Flash 的 keyPress 表（Left=1 Right=2 Up=14 Down=15 Space=32 ...）。
    std::map<int, avm1::Script> keyActions; // keyCode -> 脚本
    // 按钮声音（DefineButtonSound，tag 17），按规范顺序：
    // soundUp=IdleToOverUp(悬停) soundOver=OverUpToOverDown(按下)
    // soundDown=OverDownToOverUp(抬起) soundOut=OverUpToIdle(移出)
    std::uint16_t soundUp = 0, soundOver = 0, soundDown = 0, soundOut = 0;
};

// 字体（DefineFont2/3）：字形轮廓（按字形索引；索引 0 = .notdef）
struct FontGlyphs {
    // 字体族名（DefineFont2 的 Name 字段 / DefineFontName(88) 补充）。
    // 嵌入字体缺少某字形时，播放器按此名回退到系统（设备）字体渲染该字形。
    std::string name;
    bool italic = false;         // DefineFont2/3 FontFlagsItalic / DefineFontInfo Italic
    bool bold = false;           // DefineFont2/3 FontFlagsBold / DefineFontInfo Bold
    std::vector<std::shared_ptr<Shape>> shapes;
    std::vector<int> advances;   // 每字形前进宽度（twips）
    std::map<int, int> codeToGlyph;   // 字符码 -> 字形索引（运行时文本渲染用）
};

// 文本中的一个字形放置（DefineText/Text2 解码结果）
struct TextGlyph {
    std::uint16_t fontId = 0;
    std::uint16_t glyphIndex = 0;
    int advance = 0;
    int fontHeight = 0;         // 本 run 字号（twips）；0 = 未知（按 em 直接缩放）
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

// 文本字段（DefineEditText, tag 37）：时间轴上的静态/输入文本框。
struct EditText {
    RectB bounds;                       // twips
    std::string text;                   // InitialText
    std::uint16_t fontId = 0;           // 使用的字体角色 id
    std::uint16_t fontHeight = 240;     // 字号（twips）
    Color color{0, 0, 0, 255};
    bool wordWrap = false, multiline = false, password = false, readonly = false;
    bool border = false, autoSize = false;
    std::uint16_t maxLength = 0;
    // 绑定的变量名（DefineEditText 末尾的 VariableName 字符串，总是存在）。
    // AVM1 脚本 SetVariable 写入同名变量时，本字段的显示文本应随变量值更新。
    std::string variableName;
};

// 角色（字典项）
struct Character {
    enum class Kind { Shape, Bitmap, Sprite, Font, Text, Morph, EditText, Video, Button } kind = Kind::Shape;
    std::shared_ptr<Shape> shape;
    std::shared_ptr<Bitmap> bitmap;
    std::shared_ptr<FontGlyphs> font;   // Kind::Font
    std::shared_ptr<TextDef> text;      // Kind::Text
    std::shared_ptr<MorphShape> morph;  // Kind::Morph
    std::shared_ptr<EditText> editText;  // Kind::EditText
    std::shared_ptr<VideoStream> video;  // Kind::Video
    std::shared_ptr<ButtonDef> button;   // Kind::Button（四态 + 动作 + 声音）
    // Sprite（DefineSprite）内部时间轴的各帧显示列表
    std::vector<Frame> spriteFrames;
    // Sprite 内部帧标签：标签名 -> 1-based 帧号（供 gotoAndStop("label") 解析）。
    std::map<std::string, int> frameLabels;
};

// DefineSound（tag 14）解码结果
struct Sound {
    std::uint16_t id = 0;
    std::uint8_t format = 0;   // 0 未压缩, 1 ADPCM, 2 MP3, 3 未压缩(小端), 6 Nellymoser, 7 Nellymoser 8kHz
    std::uint8_t rate = 0;     // 0..3 -> 5512/11025/22050/44100 Hz
    std::uint8_t bits = 0;     // 0=8bit, 1=16bit
    std::uint8_t channels = 0;  // 0 单声道, 1 立体声
    std::uint32_t sampleCount = 0;
    std::vector<std::uint8_t> data;  // 紧跟 7 字节头之后的原始声音数据
};

// DefineVideoStream（tag 60）+ VideoFrame（tag 61）
struct VideoStream {
    std::uint16_t id = 0;
    std::uint16_t numFrames = 0;
    std::uint16_t width = 0, height = 0;
    std::uint8_t codecId = 0;   // 2=H.263, 3=ScreenVideo, 4=VP6, 5=VP6+alpha, 6=ScreenVideo2, 7=AVC
    bool smoothing = false;     // VideoFlags bit4：缩放采样时插值
    bool deblocking = false;    // VideoFlags bit5：解码后去块滤波（VP6 时代引入）
    std::map<std::uint16_t, std::vector<std::uint8_t>> frames;  // frameNum -> 压缩帧数据
    std::map<std::uint16_t, std::shared_ptr<Bitmap>> decoded;   // frameNum -> 解码后的 RGBA（ScreenVideo）
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
    // ---- AVM1（ActionScript 1/2）----
    std::map<std::uint16_t, avm1::Script> initActions;  // DoInitAction：spriteId -> 初始化脚本
    std::map<std::string, int> frameLabels;    // 帧标签 -> 1-based 帧号
    // DefineFontName(tag 88)：FontId -> 字体族名（供字体回退/诊断使用）
    std::map<std::uint16_t, std::string> fontNames;
    // ---- 音视频 ----
    std::map<std::uint16_t, Sound> sounds;          // characterId -> 声音
    std::map<std::uint16_t, std::shared_ptr<VideoStream>> videos;    // characterId -> 视频流
    // 流式声音（SoundStreamHead + 各帧 SoundStreamBlock）：用于影片背景音
    struct StreamAudio {
        bool have = false;
        std::uint8_t format = 0, rate = 0, bits = 0, channels = 0;
        std::vector<std::vector<std::uint8_t>> blocks;  // 每帧的压缩音频块（MP3 块含 4 字节 seek）
    } streamAudio;
    std::shared_ptr<Character> getCharacter(std::uint16_t id) const;
};

// 解码整个 SWF（body 已解压的字节流）。返回是否成功，错误写入 err。
bool decodeMovie(const std::uint8_t* data, std::size_t size, Movie& out, std::string& err);

}  // namespace swf
