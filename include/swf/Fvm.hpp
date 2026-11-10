// AVM2 值模型 + 栈式字节码解释器 + Flash 显示对象模型。
// 目标：执行 DoABC 中的 AS3 脚本，构建运行时显示列表，供渲染器绘制。
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "swf/Abc.hpp"

namespace swf {

struct Movie;   // 前向声明（避免与 Decoder.hpp 循环包含）
struct Filter;  // swf::Filter 定义在 Decoder.hpp；此处用 shared_ptr 持有其列表

// Flash 混合模式（PlaceObject3 blendMode 标志）。普通光栅器支持逐像素的常用模式。
enum class BlendMode {
    Normal = 0, Layer = 1, Multiply = 2, Screen = 3, Lighten = 4, Darken = 5,
    Difference = 6, Add = 7, Subtract = 8, Invert = 9, Alpha = 10, Erase = 11,
    Overlay = 12, HardLight = 13
};

namespace fvm {

// AVM2 解释器 opcode 覆盖度（编号元数据见 swf/Avm2Opcodes.hpp）。
// 供 CLI 打印实现进度、单测断言「所有标准 opcode 都有处理器」。
std::vector<std::uint8_t> implementedOpcodes();
bool isOpcodeImplemented(std::uint8_t op);

// ---------- 值 ----------
enum class VKind { Undefined, Null, Bool, Int, UInt, Number, String, Object };

struct ClassDef;
struct Object;

struct Value {
    VKind kind = VKind::Undefined;
    bool       b    = false;
    int32_t    i    = 0;
    uint32_t   u    = 0;
    double     d    = 0.0;
    std::string s;
    std::shared_ptr<Object> obj;

    static Value undefined() { return Value(); }
    static Value nullv()  { Value v; v.kind = VKind::Null; return v; }
    static Value boolean(bool x) { Value v; v.kind = VKind::Bool; v.b = x; return v; }
    static Value intg(int32_t x) { Value v; v.kind = VKind::Int; v.i = x; return v; }
    static Value uintv(uint32_t x) { Value v; v.kind = VKind::UInt; v.u = x; return v; }
    static Value num(double x) { Value v; v.kind = VKind::Number; v.d = x; return v; }
    static Value str(const std::string& x) { Value v; v.kind = VKind::String; v.s = x; return v; }
    static Value object(std::shared_ptr<Object> o) { Value v; v.kind = VKind::Object; v.obj = o; return v; }

    bool isUndefined() const { return kind == VKind::Undefined; }
    bool isNull() const { return kind == VKind::Null; }
    bool isNullOrUndef() const { return kind == VKind::Undefined || kind == VKind::Null; }
    bool isObject() const { return kind == VKind::Object; }

    double toNumber() const {
        switch (kind) {
            case VKind::Int:  return static_cast<double>(i);
            case VKind::UInt: return static_cast<double>(u);
            case VKind::Number: return d;
            case VKind::Bool: return b ? 1.0 : 0.0;
            case VKind::String: { try { return std::stod(s); } catch (...) { return 0.0; } }
            default: return 0.0;
        }
    }
    int32_t toInt() const { return static_cast<int32_t>(toNumber()); }
    uint32_t toUInt() const { return static_cast<uint32_t>(toNumber()); }
    bool toBool() const {
        switch (kind) {
            case VKind::Bool: return b;
            case VKind::Int:  return i != 0;
            case VKind::UInt: return u != 0;
            case VKind::Number: return d != 0.0 && d == d;
            case VKind::String: return !s.empty();
            case VKind::Object: return obj != nullptr;
            default: return false;
        }
    }
    std::string toString() const;
};

// 原生方法签名（须在 Value 定义之后）
using NativeFn = std::function<Value(std::shared_ptr<Object> self, const std::vector<Value>& args)>;

enum class ObjKind {
    Generic, DisplayObject, DisplayObjectContainer, InteractiveObject, Sprite, MovieClip,
    Stage, TextField, Bitmap, BitmapData, Shape, Class, Function, Array, String, Number,
    Boolean, Math, Event, EventDispatcher, Timer, Graphics, Loader, LoaderInfo, ByteArray, XML
};

// 类成员（trait 编译后的形式）
struct Member {
    enum Kind { Slot, Const, Method, Getter, Setter, ClassMember } kind = Slot;
    std::string name;
    int slotId = -1;
    uint32_t methodIndex = 0;      // Method/Getter/Setter 的 method_info 索引
    Value defaultValue;            // Slot/Const 的默认值
    NativeFn native;               // 原生实现（若非空优先调用）
    bool isStatic = false;
};

// 类定义（含实例/静态成员与父类链）
struct ClassDef {
    std::string name;
    int superInstanceIndex = -1;   // 父类在 abc.instances 的下标，-1 表示无
    std::shared_ptr<ClassDef> superClass;
    // instance_info.interfaces：本类直接实现的接口限定名。`as IStepFactory` /
    // `is IStepFactory` 必须沿类链检查接口，只比 superClass 会永远判定失败。
    std::vector<std::string> interfaceNames;
    std::vector<Member> instanceMembers;
    std::vector<Member> staticMembers;
    uint32_t cinitMethod = 0;
    bool cinitDone = false;
    int instanceIndex = -1;
    std::shared_ptr<Object> classObject;   // 运行时类对象（newclass 时设置，供惰性 cinit 使用）
    NativeFn ctor;                 // 原生类构造函数（Event/MouseEvent 等）
};

// AS3 Graphics 绘制命令：graphics.beginFill/lineStyle/moveTo/lineTo/drawRect/...
// 把每次调用按序记录到宿主 DisplayObject::graphics，渲染端回放为多边形填充/描边。
// 此前这组方法是空实现，AVM2 动态绘制内容完全画不出来（修复记录见 docs/FIXES_LOG.md）。
struct GraphicsCmd {
    enum class Op {
        BeginFill,   // 开始填充组（color=ARGB）
        EndFill,     // 提交当前填充组
        LineStyle,   // 描边样式（a=厚度像素；color=ARGB；厚度<=0/NaN 关闭描边）
        MoveTo,      // 移动当前点 (x,y)，开启新子路径
        LineTo,      // 追加线段到 (x,y)
        CurveTo,     // 二次贝塞尔：控制点 (cx,cy)，终点 (x,y)（渲染时展平）
        Rect,        // 矩形子路径 (x,y,w,h)
        RoundRect,   // 圆角矩形子路径 (x,y,w,h)，r=圆角半径
        Circle,      // 圆形子路径：圆心 (x,y)，半径 r
        Ellipse      // 椭圆子路径：外接矩形 (x,y,w,h)
    } op = Op::BeginFill;
    double x = 0, y = 0, w = 0, h = 0;
    double r = 0;                    // RoundRect 圆角 / Circle 半径
    double a = 0;                    // LineStyle 厚度（像素）
    std::uint32_t color = 0xFF000000;  // ARGB（高字节 alpha）
    double cx = 0, cy = 0;           // CurveTo 控制点
};

// 显示对象（构成运行时显示列表树）
struct DisplayObject {
    ObjKind kind = ObjKind::DisplayObject;
    std::string name;
    std::uint16_t characterId = 0;
    double x = 0, y = 0;
    double scaleX = 1, scaleY = 1;
    double rotation = 0;
    double alpha = 1;
    // PlaceObject 携带的完整 2x2 线性部分（无量纲比例，与渲染器 Lt 的
    // sx/r0/r1/sy 约定一致；平移仍由 x/y（像素）承载）。
    // hasFull 为真时渲染器直接用它（保留错切/旋转的非正交分量）；
    // AS3 改写 scaleX/scaleY/rotation 后会失效（回退到分解模型）。
    bool hasFull = false;
    double fullA = 1, fullB = 0;   // 行1: (fullA, fullB)
    double fullC = 0, fullD = 1;   // 行2: (fullC, fullD)
    // PlaceObject 携带的完整 ColorTransform（乘/加项）。alpha 属性已含 ma，
    // 渲染时 ma 不再重复应用；rgb 乘/加项作用于自身及整个子树。
    double cxMr = 1, cxMg = 1, cxMb = 1;
    double cxAr = 0, cxAg = 0, cxAb = 0;
    bool visible = true;
    bool mouseEnabled = true;      // false：自身不参与鼠标命中（可穿透到下层）
    bool mouseChildren = true;     // false：子对象不参与鼠标命中（父对象整体作为目标）
    int currentFrame = 0;
    bool playing = true;
    int depth = -1;                // 时间轴放置深度（-1 = AS3 addChild 创建）
    std::uint16_t clipDepth = 0;   // PlaceObject 的裁剪深度；>0 表示自身是遮罩层（不绘制）
    float ratio = 0.0f;            // PlaceObject 的 Morph 插值比例（0..1）
    bool timelineBuilt = false;    // children 是否由时间轴帧内容构建（渲染时不再重复绘制整段精灵）
    // 脚本/时间轴变换优先级：脚本写过几何属性后，静止的 PlaceObject 记录
    // 不再每帧覆盖它；只有时间轴变换本身发生变化时才由时间轴重新接管。
    bool scriptTransform = false;
    std::string tlStamp;           // 上一次由时间轴写入的变换指纹
    // Button 角色（DefineButton2）的交互态：0=Up 1=Over 2=Down（由鼠标命中链维护）
    int buttonState = 0;
    // AS3 运行时遮罩：`DisplayObject.mask = 另一个显示对象`。与时间轴 clipDepth 不同：
    // 遮罩对象自身不参与绘制，其渲染结果 alpha>0 的像素决定宿主对象的裁剪区域
    // （二值遮罩，Flash 语义；swf2.swf intro_17 的 `s1.mask = s2` 扫光动画即此机制）。
    std::shared_ptr<DisplayObject> mask;
    bool cacheAsBitmap = false;    // 仅属性兼容：软件光栅器无位图缓存，不影响渲染语义
    std::shared_ptr<DisplayObject> parent;
    std::vector<std::shared_ptr<DisplayObject>> children;
    // AS3 addFrameScript(frameIndex, func)：进入对应帧时执行一次
    std::map<int, Value> frameScripts;
    int lastScriptFrame = -1;
    std::weak_ptr<Object> owner;   // 对应的 AS3 对象（用于 frame script 的 this）

    // 覆盖时间轴 EditText 的初始文本（模拟加载进度等运行时数值）
    std::string textOverride;
    bool hasTextOverride = false;
    // 运行时 TextField 文本（AS3 设置 tf.text / tf.textColor / defaultTextFormat 后由渲染器栅格化）
    std::string textData;
    int   textSize = 12;            // 字号（px）
    uint32_t textColor = 0xFF000000;   // ARGB（高字节 alpha）
    std::string textFont;
    bool  hasText = false;
    BlendMode blend = BlendMode::Normal;   // 混合模式（PlaceObject3 blendMode）
    // PlaceObject3 FilterList。shared_ptr 让前向声明类型可用于不完整类型成员，
    // 且同一滤镜列表在逐帧复用显示对象时无需反复拷贝。
    std::shared_ptr<std::vector<Filter>> filters;
    // AS3 Graphics 绘制命令（sprite.graphics 的 beginFill/drawRect/... 按序记录；
    // clear() 清空）。渲染端按命令回放多边形填充与描边。
    std::shared_ptr<std::vector<GraphicsCmd>> graphics;
};

struct Object {
    ObjKind kind = ObjKind::Generic;
    std::string className;
    std::shared_ptr<ClassDef> cls;
    std::map<std::string, Value> props;      // 动态属性
    std::map<std::string, Value> slots;      // 具名 slot 存储
    std::shared_ptr<DisplayObject> display;
    int instanceIndex = -1;
    // 事件系统：事件类型 -> 监听器列表
    std::map<std::string, std::vector<Value>> listeners;
    // 闭包捕获的作用域链（newfunction 时记录）
    std::vector<std::shared_ptr<Object>> scope;
    // 类对象（ObjKind::Class）额外信息
    std::shared_ptr<ClassDef> classDef;
    std::map<std::string, Value> staticProps;
    // ByteArray 的字节存储（与 applicationDomain.domainMemory 共享同一缓冲）。
    std::shared_ptr<std::vector<std::uint8_t>> bytes;
    // Graphics 实例绑定的宿主显示对象（sprite.graphics 返回的 Graphics 对象持有），
    // 绘制命令写入宿主的 DisplayObject::graphics。
    std::weak_ptr<DisplayObject> gfxTarget;
};

// ---------- 虚拟机 ----------
class Vm {
public:
    explicit Vm(const abc::AbcFile& abc, Movie& movie);
    void run();

    // 播放循环：推进所有 MovieClip 时间轴后派发 enterFrame 事件（动画驱动）
    void tick();

    // 把时间轴 MovieClip 停到指定内部帧（0 基）并按该帧重建放置列表。
    void setClipFrame(const std::shared_ptr<DisplayObject>& d, int frameIndex);


    // 静态渲染首帧时强制用第 0 帧建立初始显示树；-1（默认）沿用
    // "最后一个含具名实例的帧"启发式，保证播放流程能找到 main 等实例。
    void setInitialFrame(int f) { initialFrame_ = f; }

    // ---- 鼠标输入（坐标为舞台像素，左上原点，与渲染出的 PNG 一致）----
    // 移动指针：命中显示树后派发 mouseMove，并在目标变化时派发 mouseOut/mouseOver。
    void setMousePosition(double x, double y);
    void mouseDown(double x, double y);
    void mouseUp(double x, double y);
    void mouseClick(double x, double y);
    double mouseX() const { return mouseX_; }
    double mouseY() const { return mouseY_; }

    std::vector<std::shared_ptr<DisplayObject>> stage;

    // ---- 属性访问（含类成员链、getter/setter）----
    Value getProperty(std::shared_ptr<Object> obj, const std::string& name);
    Value getPropertyImpl(std::shared_ptr<Object> obj, const std::string& name);
    void  setProperty(std::shared_ptr<Object> obj, const std::string& name, const Value& v);
    Value callMethod(std::shared_ptr<Object> obj, const std::string& name, const std::vector<Value>& args);
    // 字符串原始值装箱为 String 实例（挂内建 String 类，charAt/indexOf 等方法才可解析）
    std::shared_ptr<Object> boxString(const Value& v);
    // ---- M6 command 化：opcode handler 需调用的内部构造/命名方法 ----
    std::shared_ptr<Object> newObject(ObjKind kind, const std::string& cls);
    std::string multinameName(uint32_t index) const;
    const abc::AbcFile& abcFile() const { return abc_; }
    std::shared_ptr<Object> globalsObj() const { return globals_; }
    // ---- M6 command 化：opcode handler 需递归调用/构造的方法（原为 private）----
    Value exec(const abc::MethodBody& body, const std::vector<Value>& args,
               std::shared_ptr<Object> thisObj,
               const std::vector<std::shared_ptr<Object>>* capturedScope = nullptr);
    Value invokeValue(const Value& fn, const std::vector<Value>& args,
                      std::shared_ptr<Object> thisObj = nullptr);
    std::shared_ptr<ClassDef> buildClass(int instanceIndex);
    Value constructObject(const Value& cls, const std::vector<Value>& cargs);
    std::string qualifiedName(uint32_t index) const;

    // applicationDomain.domainMemory（Alchemy / FP11+）绑定的字节缓冲。
    void setDomainMemoryData(const std::shared_ptr<std::vector<std::uint8_t>>& d) {
        domainMemoryData_ = d;
    }
    std::shared_ptr<std::vector<std::uint8_t>> domainMemoryData() const {
        return domainMemoryData_;
    }

private:
    std::shared_ptr<std::vector<std::uint8_t>> domainMemoryData_;
    // TweenLite/TweenMax 补间：按虚拟时钟在 tick() 中插值目标属性。
    struct TweenTask {
        std::shared_ptr<Object> target;
        std::map<std::string, std::pair<double, double>> fromTo;  // 属性 -> 起止数值
        double startMs = 0, durationMs = 0, delayMs = 0;
        Value onComplete;
        std::vector<Value> completeParams;
        bool hasComplete = false;
    };
    std::vector<TweenTask> tweens_;
    void advanceTweens();
    int initialFrame_ = -1;
    Value execScript(const abc::MethodBody& body, std::shared_ptr<Object> thisObj);

    // 类构建：从 abc 的 instance/class 信息建立 ClassDef 并解析 traits
    void collectTraits(const std::vector<abc::Traits>& src, std::vector<Member>& out, bool isStatic);
    // 确保类的静态初始化 cinit 已执行
    void ensureClassInit(std::shared_ptr<ClassDef> cd, std::shared_ptr<Object> classObj);
    // 事件派发：向注册了该类型监听器的对象回调（含事件对象参数）
    void dispatchToAll(const std::string& type);
    void advanceTimeline(const std::shared_ptr<DisplayObject>& d);
    void runFrameScripts(const std::shared_ptr<DisplayObject>& d);
    // 按时间轴第 frameIndex 帧（0 基）重建 d 的具名子实例（Flash 的放置列表语义）。
    void applyTimelineFrame(const std::shared_ptr<DisplayObject>& d, int frameIndex);
    // 将 AS3 gotoAndStop/gotoAndPlay 的参数（1 基帧号或帧标签）解析为 0 基下标。
    int resolveFrameIndex(const std::shared_ptr<DisplayObject>& d, const Value& v) const;

    // ---- 鼠标事件 ----
    // 显示对象对应的 AS3 对象（时间轴实例用 owner；根用 Stage 缓存；其余动态包装）。
    std::shared_ptr<Object> objectForDisplay(const std::shared_ptr<DisplayObject>& d);
    // 命中测试；chain 自根到命中最深对象，未命中时 chain 仅含根（事件仍派发给舞台）。
    std::vector<std::shared_ptr<DisplayObject>> hitChain(double x, double y);
    // 以 chain 中的对象为目标派发事件（bubbles=true 时向祖先冒泡）。
    void dispatchMouseEvent(const std::string& type,
                            const std::vector<std::shared_ptr<DisplayObject>>& chain,
                            bool bubbles);

    double mouseX_ = 0, mouseY_ = 0;
    bool mouseDown_ = false;
    std::shared_ptr<DisplayObject> mouseDownTarget_;
    std::vector<std::shared_ptr<DisplayObject>> mouseOverChain_;
    // startDrag/stopDrag：被拖动对象及指针到对象原点的父级局部像素偏移
    std::shared_ptr<DisplayObject> dragTarget_;
    double dragOffsetX_ = 0, dragOffsetY_ = 0;
    void updateDragPosition();

    // ---- setTimeout/setInterval（虚拟时间，按 movie.frameRate 逐帧推进）----
    struct TimerTask {
        int id = 0;
        double dueMs = 0;
        double intervalMs = 0;   // >0 表示 setInterval
        Value fn;
        std::vector<Value> args;
    };
    double timerNowMs_ = 0;
    int nextTimerId_ = 1;
    std::vector<TimerTask> timers_;

    // ---- flash.utils.Timer：按同一虚拟时钟派发 timer / timerComplete ----
    struct TimerObjectTask {
        std::weak_ptr<Object> obj;
        double delayMs = 0;
        double dueMs = 0;
        int repeatCount = 0;   // 0 表示无限重复
        int currentCount = 0;
        bool running = false;
    };
    std::vector<TimerObjectTask> timerObjects_;

    // ---- 本地整片加载事件：loaderInfo 的 init/progress/complete 首 tick 派发 ----
    // 本地 SWF 无网络加载过程，但文档类普遍监听 loaderInfo 的进度/完成事件来驱动
    // preloader → 门户 → 游戏的流程；不派发则流程永远停在 0%。
    std::vector<std::shared_ptr<Object>> loaderInfos_;
    bool loaderEventsFired_ = false;
    void fireLoaderEvents();

    const abc::AbcFile& abc_;
    Movie& movie_;

    std::vector<std::shared_ptr<Object>> objects_;
    std::shared_ptr<Object> globals_;
    std::map<int, std::shared_ptr<ClassDef>> classCache_;   // instanceIndex -> ClassDef
    std::map<DisplayObject*, std::shared_ptr<Object>> stageCache_;   // 根显示对象 -> Stage 实例
    std::map<std::uint16_t, std::string> classByCid_;   // 角色 id -> AS3 类名（SymbolClass 反向映射）

    void registerNativeClasses();
    void registerFlashApi();
    void instantiateDocumentClasses();
};

// 运行影片中的全部 ABC 脚本并构建运行时显示列表。
// 返回的 Vm 实例须保持存活（shared_ptr），以支持逐帧 tick() 驱动动画与事件。
// initialFrame >= 0 时强制以该帧建立初始显示树（静态渲染首帧传 0）。
std::vector<std::shared_ptr<Vm>> runMovieScripts(Movie& movie, int initialFrame = -1);

}  // namespace fvm
}  // namespace swf
