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

// Flash 混合模式（PlaceObject3 blendMode 标志）。普通光栅器支持逐像素的常用模式。
enum class BlendMode {
    Normal = 0, Layer = 1, Multiply = 2, Screen = 3, Lighten = 4, Darken = 5,
    Difference = 6, Add = 7, Subtract = 8, Invert = 9, Alpha = 10, Erase = 11,
    Overlay = 12, HardLight = 13
};

namespace fvm {

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
    std::vector<Member> instanceMembers;
    std::vector<Member> staticMembers;
    uint32_t cinitMethod = 0;
    bool cinitDone = false;
    int instanceIndex = -1;
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
    bool visible = true;
    int currentFrame = 0;
    bool playing = true;
    std::shared_ptr<DisplayObject> parent;
    std::vector<std::shared_ptr<DisplayObject>> children;

    // 运行时 TextField 文本（AS3 设置 tf.text / tf.textColor / defaultTextFormat 后由渲染器栅格化）
    std::string textData;
    int   textSize = 12;            // 字号（px）
    uint32_t textColor = 0xFF000000;   // ARGB（高字节 alpha）
    std::string textFont;
    bool  hasText = false;
    BlendMode blend = BlendMode::Normal;   // 混合模式
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
    // 类对象（ObjKind::Class）额外信息
    std::shared_ptr<ClassDef> classDef;
    std::map<std::string, Value> staticProps;
};

// ---------- 虚拟机 ----------
class Vm {
public:
    explicit Vm(const abc::AbcFile& abc, Movie& movie);
    void run();

    // 播放循环：推进所有 MovieClip 时间轴后派发 enterFrame 事件（动画驱动）
    void tick();

    std::vector<std::shared_ptr<DisplayObject>> stage;

    // ---- 属性访问（含类成员链、getter/setter）----
    Value getProperty(std::shared_ptr<Object> obj, const std::string& name);
    void  setProperty(std::shared_ptr<Object> obj, const std::string& name, const Value& v);
    Value callMethod(std::shared_ptr<Object> obj, const std::string& name, const std::vector<Value>& args);

private:
    Value exec(const abc::MethodBody& body, const std::vector<Value>& args,
               std::shared_ptr<Object> thisObj);
    Value execScript(const abc::MethodBody& body, std::shared_ptr<Object> thisObj);

    // 类构建：从 abc 的 instance/class 信息建立 ClassDef 并解析 traits
    std::shared_ptr<ClassDef> buildClass(int instanceIndex);
    void collectTraits(const std::vector<abc::Traits>& src, std::vector<Member>& out, bool isStatic);
    // 确保类的静态初始化 cinit 已执行
    void ensureClassInit(std::shared_ptr<ClassDef> cd, std::shared_ptr<Object> classObj);
    // 事件派发：向注册了该类型监听器的对象回调（含事件对象参数）
    void dispatchToAll(const std::string& type);
    void advanceTimeline(const std::shared_ptr<DisplayObject>& d);

    const abc::AbcFile& abc_;
    Movie& movie_;

    std::vector<std::shared_ptr<Object>> objects_;
    std::shared_ptr<Object> globals_;
    std::map<int, std::shared_ptr<ClassDef>> classCache_;   // instanceIndex -> ClassDef

    std::shared_ptr<Object> newObject(ObjKind kind, const std::string& cls);
    std::string multinameName(uint32_t index) const;
    std::string qualifiedName(uint32_t index) const;
    void registerNativeClasses();
    void registerFlashApi();
    void instantiateDocumentClasses();
};

// 运行影片中的全部 ABC 脚本并构建运行时显示列表。
// 返回的 Vm 实例须保持存活（shared_ptr），以支持逐帧 tick() 驱动动画与事件。
std::vector<std::shared_ptr<Vm>> runMovieScripts(Movie& movie);

}  // namespace fvm
}  // namespace swf
