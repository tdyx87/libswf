// AVM1（ActionScript 1/2）动作字节码的反汇编与最小解释器。
// 配合时间轴播放控制（ITimeline）：gotoAndPlay / gotoAndStop / play / stop / 变量 / 算术 /
// 比较 / 分支 / 成员访问 / CallMethod 等，足以驱动绝大多数基于时间轴脚本的 Flash 内容。
#pragma once
#include <cctype>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace swf {
namespace avm1 {

struct IClip;   // 前向声明（Value 持有 IClip*）

// ---- 反汇编出的单条动作 ----
struct Action {
    std::uint8_t code = 0;
    int length = 0;                 // ActionRecord 长度（含头）
    int bytePos = 0;                // 在 Script.bytes 中的起始偏移
    int byteEnd = 0;                // 结束偏移（= 下一条动作起点）
    std::vector<std::uint8_t> pushData;  // Push（0x96）的原始 payload（运行时解析为常数池/寄存器）
    std::vector<std::uint8_t> raw; // 其它动作的原始 payload（供未知动作平衡栈用）
    std::int16_t offset = 0;       // BranchIf / Jump 偏移
    int gotoFrame = -1;            // GotoFrame（1-based）
    std::string str;               // GetURL / 标签
    std::vector<std::string> strs; // ConstantPool 字符串
    // DefineFunction / DefineFunction2
    std::string funcName;
    std::vector<std::string> funcParams;
    std::vector<int> funcParamRegs; // 参数寄存器（0=命名变量，>0=寄存器索引）
    int funcRegCount = 0;
    std::vector<std::uint8_t> funcBody;
};

struct Script {
    std::vector<Action> actions;
    std::vector<std::uint8_t> bytes;
};

// AVM1 用户函数（DefineFunction / DefineFunction2 产生）
struct Avm1Function {
    std::string name;
    std::vector<std::string> params;
    std::vector<int> paramRegs;     // 与 params 对齐；0=命名变量，>0=寄存器索引
    Script body;
    int regCount = 0;
};

// ---- 运行时值 ----
struct Value {
    enum class T { Undefined, Null, Bool, Number, String, Object, Function } type = T::Undefined;
    double num = 0;
    bool b = false;
    std::string str;
    // 对象：简单成员表（本实现仅支持 MovieClip 控制 + 通用成员）
    std::shared_ptr<std::map<std::string, Value>> obj;
    // 时间轴剪辑（_root / this）：指向播放控制器
    bool isClip = false;
    void* clip = nullptr;          // ITimeline*（非拥有）
    // 命名实例剪辑（_root.foo 得到的对象）：IClip*（非拥有）
    IClip* iclip = nullptr;
    // 用户函数（DefineFunction 产生）
    std::shared_ptr<Avm1Function> func;
    // 内建类型标记（NewObject 产生："LoadVars" / "Object" / "Array" 等）
    std::string klass;
    // 由 GetMember 产生的"方法引用"
    bool isMethod = false;
    std::string methodName;

    Value() = default;
    Value(double n) : type(T::Number), num(n) {}
    Value(bool v) : type(T::Bool), b(v) {}
    Value(const std::string& s) : type(T::String), str(s) {}
    static Value nullV() { Value v; v.type = T::Null; return v; }
    static Value undef() { return Value(); }

    bool toBool() const;
    double toNumber() const;
    std::string toString() const;
};

// 时间轴播放控制接口（由 Player 实现）
struct ITimeline {
    virtual ~ITimeline() = default;
    virtual void gotoAndPlay(int frame) = 0;   // frame：1-based
    virtual void gotoAndStop(int frame) = 0;
    virtual void play() = 0;
    virtual void stop() = 0;
    virtual void nextFrame() = 0;
    virtual void prevFrame() = 0;
    virtual void stopAllSounds() = 0;
    virtual int currentFrame() const = 0;       // 1-based
    virtual int totalFrames() const = 0;
    virtual int framesLoaded() const = 0;
    // 根时间轴上的 MovieClip 扩展方法（attachMovie/duplicateMovieClip/
    // createEmptyMovieClip/startDrag/stopDrag/getBounds/setMask）。默认无实现。
    virtual Value callExt(const std::string& method, const std::vector<Value>& args) {
        (void)method; (void)args; return Value();
    }
    // ---- 命名实例查找（_root.<name> / this.<name>）----
    // 返回值为 IClip*（非拥有）；找不到返回 nullptr。
    virtual void* findChildClip(const std::string& name) { (void)name; return nullptr; }
    // 解析带点路径（如 "_root.T4.ob.box"）到显示列表中的剪辑实例；返回 IClip*（非拥有）。
    // 支持前导 "/"、"_root"/"_level0" 与 "_parent" 段。找不到返回 nullptr。
    virtual void* resolveClipPath(const std::string& path) { (void)path; return nullptr; }
    // _name / _target（根时间轴）
    virtual std::string clipName() const { return ""; }
    virtual std::string targetPath() const { return "/"; }
};

// 属性编号（与 AVM1 GetProperty/SetProperty 索引一致）
enum ClipProp {
    PROP_X = 0, PROP_Y = 1, PROP_XSCALE = 2, PROP_YSCALE = 3,
    PROP_CURRENTFRAME = 4, PROP_TOTALFRAMES = 5, PROP_ALPHA = 6,
    PROP_VISIBLE = 7, PROP_WIDTH = 8, PROP_HEIGHT = 9, PROP_ROTATION = 10,
    PROP_TARGET = 11, PROP_FRAMESLOADED = 12, PROP_NAME = 13,
    PROP_XMOUSE = 14, PROP_PARENT = 15,
};

// 命名显示实例（AVM1 可寻址的对象：主时间轴上的 PlaceObject 实例）。
// Player 为当前帧显示列表中每个带实例名的项提供一个实现（指向其 DisplayItem）。
struct IClip {
    virtual ~IClip() = default;
    // 数值属性（_x/_y/_xscale/_yscale/_alpha/_rotation/_width/_height）
    virtual bool getNumberProp(int propId, double& out) = 0;
    virtual void setNumberProp(int propId, double v) = 0;
    // 布尔属性（_visible）
    virtual bool getBoolProp(int propId, bool& out) { (void)propId; (void)out; return false; }
    virtual void setBoolProp(int propId, bool v) { (void)propId; (void)v; }
    // 字符串属性（_name/_target）
    virtual bool getStringProp(int propId, std::string& out) { (void)propId; (void)out; return false; }
    // 时间轴方法（gotoAndPlay/stop/... 作用于该实例为根的精灵时间轴）
    virtual void callTimeline(const std::string& method, const std::vector<Value>& args) {
        (void)method; (void)args;
    }
    virtual void* parentClip() { return nullptr; }     // IClip*
    // 在该剪辑自身的子显示列表中按实例名查找（_root.foo.ob 里的 "ob"）；返回 IClip*（非拥有）。
    virtual void* findChildClip(const std::string& name) { (void)name; return nullptr; }
    virtual std::string getPath() const = 0;
    // MovieClip.hitTest(target[, shapeFlag])：target 为剪辑对象或目标路径字符串。
    // 缺省实现返回 false（不支持者）。
    virtual bool hitTest(const Value& target, bool shapeFlag) {
        (void)target; (void)shapeFlag; return false;
    }
    // 同上（实例剪辑版本）。
    virtual Value callExt(const std::string& method, const std::vector<Value>& args) {
        (void)method; (void)args; return Value();
    }
    // 脚本挂载的动态成员（btn.onPress = function(){...} / 剪辑作用域变量）。
    // 共享指针：使剪辑实例跨帧重建时其属性持久（ItemClip 指向 DisplayItem::clipVars）。
    std::shared_ptr<std::map<std::string, Value>> members =
        std::make_shared<std::map<std::string, Value>>();
};

// AVM1 变量名归一化：把主时间轴写法 "/:x"、"/x"、"_root.x"、"_level0.x" 统一成 "x"。
// 变量表是扁平的 _root 作用域，脚本混用写法时必须落到同一槽。
// 注意：AVM1 的标识符**大小写不敏感**（这是 ActionScript 1/2 的既定行为，AVM2 才区分大小写），
// 因此这里统一转小写：脚本里 goalAddOn（写）与 goalAddon（读）必须命中同一变量，
// 否则读到 undefined 会让字符串拼接产生 "NaN"（swf.swf 关卡 HUD "目标钱数:$NaN" 的根因）。
inline std::string rootVarKey(const std::string& raw) {
    std::string n = raw;
    for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (n.size() > 2 && n[0] == '/' && n[1] == ':') return n.substr(2);
    if (!n.empty() && n[0] == '/') return n.substr(1);
    if (n.rfind("_root.", 0) == 0) return n.substr(6);
    if (n.rfind("_level0.", 0) == 0) return n.substr(8);
    return n;
}
// 变量表查找（先按原样，再按归一化名）。找不到返回 nullptr。
inline const Value* findVar(const std::map<std::string, Value>& g, const std::string& n) {
    auto it = g.find(n);
    if (it != g.end()) return &it->second;
    auto it2 = g.find(rootVarKey(n));
    return it2 != g.end() ? &it2->second : nullptr;
}
// 变量名是否**显式**指向根时间轴/全局（"/:x"、"/x"、"_root"、"level0"、"_level0.x"、"_global.x"）。
// 这类名字即使在剪辑内部脚本里出现，也必须落到根变量表（而不被当作剪辑实例的局部变量）。
inline bool isRootVarName(const std::string& raw) {
    if (raw.empty()) return false;
    if (raw[0] == '/') return true;
    std::string n = raw;
    for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (n == "_root" || n == "_level0" || n == "_global") return true;
    return n.rfind("_root.", 0) == 0 || n.rfind("_level0.", 0) == 0 ||
           n.rfind("_global.", 0) == 0;
}

// ---- 反汇编动作字节流（data..data+size）为 Script ----
Script disassemble(const std::uint8_t* data, std::size_t size);

// 解释器：执行一整段 Script，通过 timeline 驱动播放，globals 跨段持久。
// 支持用户函数（DefineFunction）调用：内部维护调用栈（每帧独立操作数栈/寄存器/常量池/局部作用域）。
struct Machine {
    std::map<std::string, Value> globals;
    std::map<std::string, int> frameLabels;          // 帧标签 -> 1-based 帧号
    ITimeline* timeline = nullptr;
    // 当前正在执行脚本的剪辑时间轴。主时间轴帧脚本时为空（隐含 timeline）；执行精灵内部
    // 帧脚本时指向该精灵的播放控制器，使脚本里无目标的 stop()/play()/gotoAndPlay() 作用于
    // 该精灵自身，而 _root / _level0 仍指向主时间轴。
    ITimeline* currentClip = nullptr;
    // 当前脚本上下文的"父级"剪辑对象（_parent 解析目标）。主时间轴脚本为空（_parent 无定义）；
    // 精灵/按钮脚本中指向其上级 MovieClip 实例，使 _parent 返回正确的剪辑。
    IClip* currentParent = nullptr;
    // 当前脚本的 `this` 剪辑对象（ClipEvent / 剪辑帧脚本 / 按钮处理器）。
    // 为空时 `this` 回退到 currentParent / currentClip（兼容旧行为）。
    IClip* currentThis = nullptr;
    int maxCallDepth = 256;                          // 递归深度上限，防止栈溢出
    void run(const Script& s);
    // ---- 运行时函数值调用（按钮/ClipActions 脚本里定义的函数被外部触发时用）----
    // 要求 v.type == Function 且 v.func 有效。返回函数返回值（多数为 undefined）。
    Value callFunctionValue(const Value& v, const std::vector<Value>& args);
    // ---- 鼠标/键盘事件入口（供 Player 在鼠标/键盘变化时调用）----
    // 在 globals / 命名对象上查找 handlerName（如 onPress/onRelease/onMouseMove/
    // onMouseDown/onMouseUp/onEnterFrame）并以 args 调用之。找不到则无操作。
    void callHandler(const std::string& handlerName, const std::vector<Value>& args);
};

}  // namespace avm1
}  // namespace swf
