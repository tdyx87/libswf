// AVM2 值模型 + 栈式字节码解释器 + Flash 显示对象模型。
// 目标：执行 DoABC 中的 AS3 脚本，构建运行时显示列表，供渲染器绘制。
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "swf/Abc.hpp"

namespace swf {

struct Movie;   // 前向声明（避免与 Decoder.hpp 循环包含）

namespace fvm {

// ---------- 值 ----------
enum class VKind { Undefined, Null, Bool, Int, UInt, Number, String, Object };

struct Value {
    VKind kind = VKind::Undefined;
    bool       b    = false;
    int32_t    i    = 0;
    uint32_t   u    = 0;
    double     d    = 0.0;
    std::string s;
    std::shared_ptr<void> obj;   // 对象句柄（由 ObjectTable 解析）

    static Value undefined() { return Value(); }
    static Value nullv()  { Value v; v.kind = VKind::Null; return v; }
    static Value boolean(bool x) { Value v; v.kind = VKind::Bool; v.b = x; return v; }
    static Value intg(int32_t x) { Value v; v.kind = VKind::Int; v.i = x; return v; }
    static Value uintv(uint32_t x) { Value v; v.kind = VKind::UInt; v.u = x; return v; }
    static Value num(double x) { Value v; v.kind = VKind::Number; v.d = x; return v; }
    static Value str(const std::string& x) { Value v; v.kind = VKind::String; v.s = x; return v; }

    bool isUndefined() const { return kind == VKind::Undefined; }
    bool isNull() const { return kind == VKind::Null; }
    bool isNumberLike() const {
        return kind == VKind::Int || kind == VKind::UInt || kind == VKind::Number || kind == VKind::Bool;
    }
    double toNumber() const {
        switch (kind) {
            case VKind::Int:  return static_cast<double>(i);
            case VKind::UInt: return static_cast<double>(u);
            case VKind::Number: return d;
            case VKind::Bool: return b ? 1.0 : 0.0;
            case VKind::Null: return 0.0;
            case VKind::Undefined: return 0.0;
            case VKind::String: {
                try { return std::stod(s); } catch (...) { return 0.0; }
            }
            default: return 0.0;
        }
    }
    int32_t toInt() const { return static_cast<int32_t>(toNumber()); }
    bool toBool() const {
        switch (kind) {
            case VKind::Bool: return b;
            case VKind::Int:  return i != 0;
            case VKind::UInt: return u != 0;
            case VKind::Number: return d != 0.0 && d == d;
            case VKind::String: return !s.empty();
            case VKind::Null: case VKind::Undefined: return false;
            default: return kind == VKind::Object;
        }
    }
    std::string toString() const {
        switch (kind) {
            case VKind::String: return s;
            case VKind::Int:  return std::to_string(i);
            case VKind::UInt: return std::to_string(u);
            case VKind::Number: return std::to_string(d);
            case VKind::Bool: return b ? "true" : "false";
            case VKind::Null: return "null";
            case VKind::Undefined: return "undefined";
            default: return "[object]";
        }
    }
};

enum class ObjKind { Generic, DisplayObject, MovieClip, Sprite, TextField, Class, Function, Array, BitmapData };

// 显示对象（构成运行时显示列表树）
struct DisplayObject {
    ObjKind kind = ObjKind::DisplayObject;
    std::string name;
    std::uint16_t characterId = 0;   // 若为角色实例，指向 SWF 角色字典
    double x = 0, y = 0;
    double scaleX = 1, scaleY = 1;
    double alpha = 1;
    bool visible = true;
    std::shared_ptr<DisplayObject> parent;
    std::vector<std::shared_ptr<DisplayObject>> children;
};

struct Object {
    ObjKind kind = ObjKind::Generic;
    std::string className;
    std::map<std::string, Value> props;          // 字符串属性
    std::shared_ptr<DisplayObject> display;      // 若为显示对象
    // 类实例：所属类在 AbcFile 中的 instances 下标
    int instanceIndex = -1;
};

// ---------- 虚拟机 ----------
class Vm {
public:
    explicit Vm(const abc::AbcFile& abc, Movie& movie);

    // 执行一个 ABC（脚本入口）。
    void run();

    // 执行结果：顶层显示列表树
    std::vector<std::shared_ptr<DisplayObject>> stage;

private:
    // 解释一个 method_body
    Value exec(const abc::MethodBody& body, const std::vector<Value>& args,
               std::shared_ptr<Object> thisObj);

    Value execScript(const abc::MethodBody& body, std::shared_ptr<Object> thisObj);

    const abc::AbcFile& abc_;
    Movie& movie_;

    std::vector<std::shared_ptr<Object>> objects_;   // 对象表（obj 句柄索引）
    std::shared_ptr<Object> globals_;                // 全局对象（注册内建类）

    std::shared_ptr<Object> newObject(ObjKind kind, const std::string& cls);
    std::shared_ptr<Object> resolve(uint32_t multinameIndex, const std::string& ns);
    std::string multinameName(uint32_t index) const;
    std::string qualifiedName(uint32_t index) const;   // 含命名空间的完全限定名
};

// 运行影片中的全部 ABC 脚本，并把结果挂到 movie 的运行时显示列表上。
// 返回是否成功（无 ABC 或执行失败都不影响静态渲染）。
bool runMovieScripts(Movie& movie);

}  // namespace fvm
}  // namespace swf
