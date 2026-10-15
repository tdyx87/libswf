// AVM2 字节码解释器 + Flash 显示对象模型实现。
// 策略：实现常见 opcode；未实现的 opcode 尽量安全跳过，保证不崩溃、可降级。
#include "swf/Fvm.hpp"

#include <cmath>
#include <cstring>

#include "swf/Decoder.hpp"   // Movie 完整定义（Fvm.hpp 中仅前向声明）

namespace swf {
namespace fvm {

namespace {

// ---- 指令流读取 ----
class Inst {
public:
    Inst(const std::vector<uint8_t>& code) : c_(code) {}

    bool atEnd() const { return p_ >= c_.size(); }
    size_t pos() const { return p_; }
    void seek(size_t p) { p_ = p; }

    uint8_t u8() { return (p_ < c_.size()) ? c_[p_++] : 0; }
    int8_t  s8() { return static_cast<int8_t>(u8()); }
    uint16_t u16() { uint16_t a = u8(); return static_cast<uint16_t>(a | (u8() << 8)); }
    int16_t s16() { return static_cast<int16_t>(u16()); }
    uint32_t u30() {
        uint32_t r = 0;
        for (int i = 0; i < 5; ++i) {
            uint8_t b = u8();
            if (i == 4) { r |= static_cast<uint32_t>(b & 0x0F) << 28; break; }
            r |= static_cast<uint32_t>(b & 0x7F) << (7 * i);
            if (!(b & 0x80)) break;
        }
        return r;
    }
    int32_t s24() {
        uint32_t a = u8(), b = u8(), c = u8();
        uint32_t v = a | (b << 8) | (c << 16);
        if (v & 0x800000u) v |= 0xFF000000u;
        return static_cast<int32_t>(v);
    }

private:
    const std::vector<uint8_t>& c_;
    size_t p_ = 0;
};

}  // namespace

// ---------- Vm ----------
Vm::Vm(const abc::AbcFile& abc, Movie& movie) : abc_(abc), movie_(movie) {}

std::shared_ptr<Object> Vm::newObject(ObjKind kind, const std::string& cls) {
    auto o = std::make_shared<Object>();
    o->kind = kind;
    o->className = cls;
    if (kind == ObjKind::DisplayObject || kind == ObjKind::MovieClip ||
        kind == ObjKind::Sprite || kind == ObjKind::TextField) {
        auto d = std::make_shared<DisplayObject>();
        d->kind = kind;
        o->display = d;
    }
    objects_.push_back(o);
    return o;
}

std::string Vm::multinameName(uint32_t index) const {
    const abc::Multiname* m = abc_.mn(index);
    if (!m) return "";
    return abc_.str(m->name);
}

std::string Vm::qualifiedName(uint32_t index) const {
    const abc::Multiname* m = abc_.mn(index);
    if (!m) return "";
    std::string nm = abc_.str(m->name);
    std::string ns;
    if (m->ns < abc_.namespaces.size())
        ns = abc_.str(abc_.namespaces[m->ns].name);
    if (ns.empty()) return nm;
    return ns + "." + nm;
}

Value Vm::exec(const abc::MethodBody& body, const std::vector<Value>& args,
               std::shared_ptr<Object> thisObj) {
    std::vector<Value> stack;
    stack.reserve(body.maxStack + 8);
    std::vector<Value> locals(body.localCount + 8);
    if (!locals.empty()) {
        locals[0] = Value::undefined();
        if (thisObj) { Value v; v.kind = VKind::Object; v.obj = thisObj; locals[0] = v; }
    }
    for (size_t i = 0; i < args.size() && i + 1 < locals.size(); ++i) locals[i + 1] = args[i];

    std::vector<std::shared_ptr<Object>> scope;

    Inst in(body.code);
    const size_t codeSize = body.code.size();

    auto push = [&](const Value& v) { stack.push_back(v); };
    auto pop = [&]() -> Value {
        if (stack.empty()) return Value::undefined();
        Value v = stack.back(); stack.pop_back(); return v;
    };

    size_t guard = 0;
    while (!in.atEnd()) {
        if (++guard > 200000) break;   // 指令上限，防御死循环
        size_t opPos = in.pos();
        uint8_t op = in.u8();

        switch (op) {
            // ---- 常量入栈 ----
            case 0x02: break;                                  // nop
            case 0x09: break;                                  // label
            case 0x20: push(Value::nullv()); break;            // pushnull
            case 0x21: push(Value::undefined()); break;        // pushundefined
            case 0x24: push(Value::intg(static_cast<int8_t>(in.u8()))); break;   // pushbyte
            case 0x25: push(Value::uintv(in.u30())); break;                      // pushshort
            case 0x26: push(Value::boolean(true)); break;      // pushtrue
            case 0x27: push(Value::boolean(false)); break;     // pushfalse
            case 0x28: push(Value::num(NAN)); break;           // pushnan
            case 0x2c: {                                        // pushstring
                uint32_t idx = in.u30();
                push(Value::str(abc_.str(idx)));
                break;
            }
            case 0x2d: {                                        // pushint
                uint32_t idx = in.u30();
                int32_t v = (idx < abc_.ints.size()) ? abc_.ints[idx] : 0;
                push(Value::intg(v));
                break;
            }
            case 0x2e: {                                        // pushuint
                uint32_t idx = in.u30();
                uint32_t v = (idx < abc_.uints.size()) ? abc_.uints[idx] : 0;
                push(Value::uintv(v));
                break;
            }
            case 0x2f: {                                        // pushdouble
                uint32_t idx = in.u30();
                double v = (idx < abc_.doubles.size()) ? abc_.doubles[idx] : 0.0;
                push(Value::num(v));
                break;
            }

            // ---- 栈操作 ----
            case 0x29: pop(); break;                            // pop
            case 0x2a: if (!stack.empty()) push(stack.back()); break;  // dup
            case 0x2b: {                                        // swap
                if (stack.size() >= 2) {
                    Value a = stack.back(); stack.pop_back();
                    Value b = stack.back(); stack.pop_back();
                    push(a); push(b);
                }
                break;
            }

            // ---- 局部变量 ----
            case 0x62: {                                        // getlocal
                uint32_t idx = in.u30();
                push(idx < locals.size() ? locals[idx] : Value::undefined());
                break;
            }
            case 0x63: {                                        // setlocal
                uint32_t idx = in.u30();
                Value v = pop();
                if (idx < locals.size()) locals[idx] = v;
                break;
            }
            case 0xd0: push(locals.size() > 0 ? locals[0] : Value::undefined()); break;
            case 0xd1: push(locals.size() > 1 ? locals[1] : Value::undefined()); break;
            case 0xd2: push(locals.size() > 2 ? locals[2] : Value::undefined()); break;
            case 0xd3: push(locals.size() > 3 ? locals[3] : Value::undefined()); break;
            case 0xd4: { Value v = pop(); if (locals.size() > 0) locals[0] = v; break; }
            case 0xd5: { Value v = pop(); if (locals.size() > 1) locals[1] = v; break; }
            case 0xd6: { Value v = pop(); if (locals.size() > 2) locals[2] = v; break; }
            case 0xd7: { Value v = pop(); if (locals.size() > 3) locals[3] = v; break; }

            // ---- 作用域 ----
            case 0x30: {                                        // pushscope
                Value v = pop();
                if (v.kind == VKind::Object && v.obj) {
                    auto* op = reinterpret_cast<Object*>(v.obj.get());
                    // 用共享所有权：在 objects_ 里查找同指针
                    for (auto& o : objects_) if (o.get() == op) { scope.push_back(o); break; }
                }
                break;
            }
            case 0x1d: if (!scope.empty()) scope.pop_back(); break;   // popscope
            case 0x64: break;                                   // getglobalscope (no-op)
            case 0x65: {                                        // getscopeobject
                uint32_t idx = in.u8();
                if (idx < scope.size()) {
                    Value v; v.kind = VKind::Object; v.obj = scope[idx]; push(v);
                } else push(Value::undefined());
                break;
            }

            // ---- 属性访问 ----
            case 0x66: {                                        // getproperty
                uint32_t mnIndex = in.u30();
                Value obj = pop();
                std::string name = multinameName(mnIndex);
                if (obj.kind == VKind::Object && obj.obj) {
                    auto* o = reinterpret_cast<Object*>(obj.obj.get());
                    auto it = o->props.find(name);
                    if (it != o->props.end()) { push(it->second); }
                    else push(Value::undefined());
                } else push(Value::undefined());
                break;
            }
            case 0x61: {                                        // setproperty
                uint32_t mnIndex = in.u30();
                Value val = pop();
                Value obj = pop();
                std::string name = multinameName(mnIndex);
                if (obj.kind == VKind::Object && obj.obj) {
                    auto* o = reinterpret_cast<Object*>(obj.obj.get());
                    o->props[name] = val;
                    // 同步到显示对象（x/y/alpha/visible/scale）
                    if (o->display) {
                        if (name == "x") o->display->x = val.toNumber();
                        else if (name == "y") o->display->y = val.toNumber();
                        else if (name == "alpha") o->display->alpha = val.toNumber();
                        else if (name == "visible") o->display->visible = val.toBool();
                        else if (name == "scaleX") o->display->scaleX = val.toNumber();
                        else if (name == "scaleY") o->display->scaleY = val.toNumber();
                    }
                }
                break;
            }
            case 0x68: {                                        // initproperty
                uint32_t mnIndex = in.u30();
                Value val = pop();
                Value obj = pop();
                std::string name = multinameName(mnIndex);
                if (obj.kind == VKind::Object && obj.obj) {
                    auto* o = reinterpret_cast<Object*>(obj.obj.get());
                    o->props[name] = val;
                }
                break;
            }
            case 0x6c: {                                        // getslot
                uint32_t slot = in.u30();
                Value obj = pop();
                if (obj.kind == VKind::Object && obj.obj) {
                    auto* o = reinterpret_cast<Object*>(obj.obj.get());
                    auto it = o->props.find("$" + std::to_string(slot));
                    push(it != o->props.end() ? it->second : Value::undefined());
                } else push(Value::undefined());
                break;
            }
            case 0x6d: {                                        // setslot
                uint32_t slot = in.u30();
                Value val = pop();
                Value obj = pop();
                if (obj.kind == VKind::Object && obj.obj) {
                    auto* o = reinterpret_cast<Object*>(obj.obj.get());
                    o->props["$" + std::to_string(slot)] = val;
                }
                break;
            }

            // ---- 控制流 ----
            case 0x10: {                                        // jump
                int32_t off = in.s24();
                size_t target = static_cast<size_t>(static_cast<int64_t>(in.pos()) + off);
                if (target <= codeSize) in.seek(target);
                break;
            }
            case 0x11: {                                        // iftrue
                int32_t off = in.s24();
                if (pop().toBool()) {
                    size_t t = static_cast<size_t>(static_cast<int64_t>(in.pos()) + off);
                    if (t <= codeSize) in.seek(t);
                }
                break;
            }
            case 0x12: {                                        // iffalse
                int32_t off = in.s24();
                if (!pop().toBool()) {
                    size_t t = static_cast<size_t>(static_cast<int64_t>(in.pos()) + off);
                    if (t <= codeSize) in.seek(t);
                }
                break;
            }
            case 0x47: return Value::undefined();                // returnvoid
            case 0x48: return pop();                             // returnvalue

            // ---- 算术/比较 ----
            case 0xa0: { Value b = pop(); Value a = pop();                 // add
                if (a.kind == VKind::String || b.kind == VKind::String)
                    push(Value::str(a.toString() + b.toString()));
                else push(Value::num(a.toNumber() + b.toNumber()));
                break; }
            case 0xa1: { Value b = pop(); Value a = pop(); push(Value::num(a.toNumber() - b.toNumber())); break; }
            case 0xa2: { Value b = pop(); Value a = pop(); push(Value::num(a.toNumber() * b.toNumber())); break; }
            case 0xa3: { Value b = pop(); Value a = pop();
                double d = b.toNumber(); push(Value::num(d == 0.0 ? NAN : a.toNumber() / d)); break; }
            case 0xa4: { Value b = pop(); Value a = pop();
                double d = b.toNumber(); push(Value::num(d == 0.0 ? NAN : std::fmod(a.toNumber(), d))); break; }
            case 0xa5: { Value b = pop(); Value a = pop(); push(Value::intg(a.toInt() << (b.toInt() & 31))); break; }
            case 0xa6: { Value b = pop(); Value a = pop(); push(Value::intg(a.toInt() >> (b.toInt() & 31))); break; }
            case 0xa8: { Value b = pop(); Value a = pop(); push(Value::intg(a.toInt() & b.toInt())); break; }
            case 0xa9: { Value b = pop(); Value a = pop(); push(Value::intg(a.toInt() | b.toInt())); break; }
            case 0xaa: { Value b = pop(); Value a = pop(); push(Value::intg(a.toInt() ^ b.toInt())); break; }
            case 0xab: { Value b = pop(); Value a = pop();                 // equals (loose)
                if (a.isUndefined() || b.isUndefined() || a.isNull() || b.isNull())
                    push(Value::boolean(a.isUndefined() == b.isUndefined() && a.isNull() == b.isNull()));
                else if (a.kind == VKind::String && b.kind == VKind::String)
                    push(Value::boolean(a.s == b.s));
                else push(Value::boolean(a.toNumber() == b.toNumber()));
                break; }
            case 0xac: { Value b = pop(); Value a = pop(); push(Value::boolean(a.kind == b.kind && a.toString() == b.toString())); break; }
            case 0xad: { Value b = pop(); Value a = pop(); push(Value::boolean(a.toNumber() < b.toNumber())); break; }
            case 0xae: { Value b = pop(); Value a = pop(); push(Value::boolean(a.toNumber() <= b.toNumber())); break; }
            case 0xaf: { Value b = pop(); Value a = pop(); push(Value::boolean(a.toNumber() > b.toNumber())); break; }
            case 0xb0: { Value b = pop(); Value a = pop(); push(Value::boolean(a.toNumber() >= b.toNumber())); break; }
            case 0x90: { Value a = pop(); push(Value::num(-a.toNumber())); break; }
            case 0x91: { Value a = pop(); push(Value::num(a.toNumber() + 1)); break; }
            case 0x93: { Value a = pop(); push(Value::num(a.toNumber() - 1)); break; }
            case 0x96: { Value a = pop(); push(Value::boolean(!a.toBool())); break; }
            case 0x97: { Value a = pop(); push(Value::intg(~a.toInt())); break; }
            case 0x73: { Value a = pop(); push(Value::intg(a.toInt())); break; }         // convert_i
            case 0x74: { Value a = pop(); push(Value::uintv(static_cast<uint32_t>(a.toNumber()))); break; }
            case 0x75: { Value a = pop(); push(Value::num(a.toNumber())); break; }        // convert_d
            case 0x76: { Value a = pop(); push(Value::boolean(a.toBool())); break; }      // convert_b
            case 0x70: { Value a = pop(); push(Value::str(a.toString())); break; }        // convert_s
            case 0x82: break;                                    // coerce_a (no-op)

            // ---- 对象/类 ----
            case 0x53: {                                        // newobject
                uint32_t n = in.u30();
                auto o = newObject(ObjKind::Generic, "Object");
                for (uint32_t k = 0; k < n; ++k) {
                    Value val = pop();
                    Value key = pop();
                    o->props[key.toString()] = val;
                }
                Value v; v.kind = VKind::Object; v.obj = o; push(v);
                break;
            }
            case 0x55: {                                        // newarray
                uint32_t n = in.u30();
                auto o = newObject(ObjKind::Array, "Array");
                for (uint32_t k = 0; k < n; ++k) { Value v = pop(); o->props[std::to_string(n - 1 - k)] = v; }
                Value v; v.kind = VKind::Object; v.obj = o; push(v);
                break;
            }
            case 0x40: {                                        // newfunction（创建函数对象）
                uint32_t methodIdx = in.u30();
                auto o = newObject(ObjKind::Function, "Function");
                o->props["$method"] = Value::uintv(methodIdx);
                Value v; v.kind = VKind::Object; v.obj = o; push(v);
                break;
            }
            case 0x49: {                                        // constructsuper（构造父类）
                uint32_t argc = in.u30();
                for (uint32_t k = 0; k < argc; ++k) pop();
                pop();  // this
                break;
            }
            case 0x4f: {                                        // callpropvoid（丢弃返回值的方法调用）
                uint32_t mnIndex = in.u30();
                uint32_t argc = in.u30();
                std::vector<Value> cargs(argc);
                for (uint32_t k = 0; k < argc; ++k) cargs[argc - 1 - k] = pop();
                Value objv = pop();
                std::string mname = multinameName(mnIndex);
                if (mname == "addChild" && objv.kind == VKind::Object && objv.obj && argc >= 1 &&
                    cargs[0].kind == VKind::Object && cargs[0].obj) {
                    auto* o = reinterpret_cast<Object*>(objv.obj.get());
                    auto* c = reinterpret_cast<Object*>(cargs[0].obj.get());
                    if (o->display && c->display) {
                        c->display->parent = o->display;
                        o->display->children.push_back(c->display);
                        if (!o->display->parent) {
                            bool dup = false;
                            for (auto& s : stage) if (s.get() == o->display.get()) { dup = true; break; }
                            if (!dup) stage.push_back(o->display);
                        }
                    }
                }
                break;
            }
            case 0x5d: {                                        // findproperty：查找属性所属对象
                uint32_t mnIndex = in.u30();
                std::string name = multinameName(mnIndex);
                // 先在作用域栈中找，再回退到全局对象
                Value found = Value::nullv();
                for (auto it = scope.rbegin(); it != scope.rend(); ++it) {
                    if ((*it)->props.count(name)) { Value v; v.kind = VKind::Object; v.obj = *it; found = v; break; }
                }
                if (found.isNull() && globals_) { Value v; v.kind = VKind::Object; v.obj = globals_; found = v; }
                push(found);
                break;
            }
            case 0x60: {                                        // getlex：取词法（类/变量）值
                uint32_t mnIndex = in.u30();
                std::string name = multinameName(mnIndex);
                Value found = Value::undefined();
                for (auto it = scope.rbegin(); it != scope.rend(); ++it) {
                    auto p = (*it)->props.find(name);
                    if (p != (*it)->props.end()) { found = p->second; break; }
                }
                if (found.isUndefined() && globals_) {
                    auto p = globals_->props.find(name);
                    if (p != globals_->props.end()) found = p->second;
                }
                push(found);
                break;
            }
            case 0x58: {                                        // newclass：创建类对象并注册到全局
                uint32_t clsIdx = in.u30();
                auto o = newObject(ObjKind::Class, "Class");
                o->instanceIndex = static_cast<int>(clsIdx);
                std::string cname;
                if (clsIdx < abc_.instances.size()) cname = qualifiedName(abc_.instances[clsIdx].name);
                o->className = cname;
                Value v; v.kind = VKind::Object; v.obj = o;
                if (globals_ && !cname.empty()) {
                    globals_->props[cname] = v;                       // 完全限定名
                    size_t p = cname.rfind('.');
                    if (p != std::string::npos) globals_->props[cname.substr(p + 1)] = v;  // 短名
                }
                push(v);
                break;
            }
            case 0x42: {                                        // construct：构造实例
                uint32_t argc = in.u30();
                std::vector<Value> cargs(argc);
                for (uint32_t k = 0; k < argc; ++k) cargs[argc - 1 - k] = pop();
                Value cls = pop();
                std::string cname = (cls.kind == VKind::Object && cls.obj)
                    ? reinterpret_cast<Object*>(cls.obj.get())->className : "";
                ObjKind ok = ObjKind::Generic;
                if (cname.find("MovieClip") != std::string::npos) ok = ObjKind::MovieClip;
                else if (cname.find("Sprite") != std::string::npos) ok = ObjKind::Sprite;
                else if (cname.find("TextField") != std::string::npos) ok = ObjKind::TextField;
                // 内建类：用注册时记录的 kind
                if (cls.kind == VKind::Object && cls.obj) {
                    auto* c0 = reinterpret_cast<Object*>(cls.obj.get());
                    auto nk = c0->props.find("$nativeKind");
                    if (nk != c0->props.end()) ok = static_cast<ObjKind>(nk->second.toInt());
                }
                auto inst = newObject(ok, cname);
                // 通过 SymbolClass 把 AS3 类实例关联到 SWF 角色（这样才有可绘制内容）
                {
                    auto it = movie_.symbolClasses.find(cname);
                    if (it == movie_.symbolClasses.end()) {
                        // 尝试短名（去掉包名）
                        size_t p = cname.rfind('.');
                        std::string shortName = (p == std::string::npos) ? cname : cname.substr(p + 1);
                        it = movie_.symbolClasses.find(shortName);
                    }
                    if (it != movie_.symbolClasses.end() && inst->display)
                        inst->display->characterId = it->second;
                }
                // 若类有 iinit 且有函数体，执行构造函数
                if (cls.kind == VKind::Object && cls.obj) {
                    auto* c = reinterpret_cast<Object*>(cls.obj.get());
                    if (c->instanceIndex >= 0 && c->instanceIndex < static_cast<int>(abc_.instances.size())) {
                        const auto& ii = abc_.instances[c->instanceIndex];
                        if (ii.iinit < abc_.methods.size()) {
                            int bi = abc_.methods[ii.iinit].bodyIndex;
                            if (bi >= 0 && bi < static_cast<int>(abc_.methodBodies.size())) {
                                Value tv; tv.kind = VKind::Object; tv.obj = inst;
                                exec(abc_.methodBodies[bi], cargs, inst);
                            }
                        }
                    }
                }
                Value v; v.kind = VKind::Object; v.obj = inst; push(v);
                break;
            }
            case 0x46: {                                        // callproperty
                uint32_t mnIndex = in.u30();
                uint32_t argc = in.u30();
                std::vector<Value> cargs(argc);
                for (uint32_t k = 0; k < argc; ++k) cargs[argc - 1 - k] = pop();
                Value objv = pop();
                std::string mname = multinameName(mnIndex);
                // 内建：addChild / removeChild
                if (mname == "addChild" && objv.kind == VKind::Object && objv.obj && argc >= 1) {
                    auto* o = reinterpret_cast<Object*>(objv.obj.get());
                    auto* c = reinterpret_cast<Object*>(cargs[0].obj.get());
                    if (o->display && c && c->display) {
                        c->display->parent = o->display;
                        o->display->children.push_back(c->display);
                        // 顶层容器挂到舞台（去重）
                        if (!o->display->parent) {
                            bool dup = false;
                            for (auto& s : stage) if (s.get() == o->display.get()) { dup = true; break; }
                            if (!dup) stage.push_back(o->display);
                        }
                    }
                    Value r; r.kind = VKind::Object; r.obj = cargs[0].obj; push(r);
                } else {
                    push(Value::undefined());   // 其他方法调用暂返回 undefined
                }
                break;
            }
            case 0x41: {                                        // call
                uint32_t argc = in.u30();
                for (uint32_t k = 0; k < argc; ++k) pop();
                pop();  // function
                push(Value::undefined());
                break;
            }
            case 0x4a: {                                        // constructprop
                uint32_t mnIndex = in.u30();
                uint32_t argc = in.u30();
                for (uint32_t k = 0; k < argc; ++k) pop();
                pop();
                auto o = newObject(ObjKind::Generic, multinameName(mnIndex));
                Value v; v.kind = VKind::Object; v.obj = o; push(v);
                break;
            }

            default:
                // 未实现 opcode：尽力跳过，避免崩溃（降级执行）
                break;
        }
        (void)opPos;
    }
    return Value::undefined();
}

Value Vm::execScript(const abc::MethodBody& body, std::shared_ptr<Object> thisObj) {
    return exec(body, {}, thisObj);
}

void Vm::run() {
    // 建立全局对象并注册内建显示类（AS3 代码通过 getlex/findproperty 取用）
    globals_ = newObject(ObjKind::Generic, "$global");
    auto regClass = [&](const char* name, ObjKind k) {
        auto c = newObject(ObjKind::Class, name);
        c->props["$nativeKind"] = Value::intg(static_cast<int32_t>(k));
        Value v; v.kind = VKind::Object; v.obj = c;
        globals_->props[name] = v;
        // 同时注册带 flash.display 前缀的完全限定名
        std::string qn = std::string("flash.display.") + name;
        globals_->props[qn] = v;
    };
    regClass("Sprite", ObjKind::Sprite);
    regClass("MovieClip", ObjKind::MovieClip);
    regClass("DisplayObject", ObjKind::DisplayObject);
    regClass("TextField", ObjKind::TextField);
    regClass("BitmapData", ObjKind::BitmapData);

    // 依次执行每个 script 的 init 方法（AS3 顶层代码）
    for (const auto& scr : abc_.scripts) {
        if (scr.init >= abc_.methods.size()) continue;
        int bi = abc_.methods[scr.init].bodyIndex;
        if (bi < 0 || bi >= static_cast<int>(abc_.methodBodies.size())) continue;
        execScript(abc_.methodBodies[bi], globals_);
    }

    // 实例化文档类：SymbolClass 把角色 id 映射到 AS3 类名，播放器会实例化这些类，
    // 其构造函数中通过 addChild 构建场景。这里模拟该过程。
    for (const auto& kv : movie_.symbolClasses) {
        const std::string& cname = kv.first;
        std::uint16_t cid = kv.second;
        auto it = globals_->props.find(cname);
        if (it == globals_->props.end()) {
            // 回退：用短名匹配
            size_t p = cname.rfind('.');
            if (p != std::string::npos) it = globals_->props.find(cname.substr(p + 1));
        }
        if (it == globals_->props.end()) continue;
        if (it->second.kind != VKind::Object || !it->second.obj) continue;
        auto* cls = reinterpret_cast<Object*>(it->second.obj.get());
        if (cls->instanceIndex < 0 || cls->instanceIndex >= static_cast<int>(abc_.instances.size())) continue;

        const auto& ii = abc_.instances[cls->instanceIndex];
        // 创建实例（角色 id 来自 SymbolClass，使其可绘制）
        ObjKind ok = ObjKind::MovieClip;
        auto inst = newObject(ok, cname);
        inst->display->characterId = cid;
        inst->instanceIndex = cls->instanceIndex;

        // 执行实例构造函数 iinit（其内部 addChild 会挂到 stage）
        if (ii.iinit < abc_.methods.size()) {
            int b2 = abc_.methods[ii.iinit].bodyIndex;
            if (b2 >= 0 && b2 < static_cast<int>(abc_.methodBodies.size()))
                exec(abc_.methodBodies[b2], {}, inst);
        }
        // 若没有被 addChild 到别处，则作为顶层加入舞台
        if (!inst->display->parent) {
            bool dup = false;
            for (auto& s : stage) if (s.get() == inst->display.get()) { dup = true; break; }
            if (!dup) stage.push_back(inst->display);
        }
        (void)cid;
    }
}

bool runMovieScripts(Movie& movie) {
    if (movie.abcFiles.empty()) return false;
    for (const auto& abc : movie.abcFiles) {
        Vm vm(abc, movie);
        vm.run();
        // 把 AS 构建的显示列表并入影片（追加为一帧的额外项效果在渲染时处理）
        if (!vm.stage.empty()) {
            // 保存运行时显示列表到 movie（通过最后一个字符槽位不可行，改为直接记录）
            movie.runtimeStage = vm.stage;
        }
    }
    return true;
}

}  // namespace fvm
}  // namespace swf
