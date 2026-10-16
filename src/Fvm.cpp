// AVM2 字节码解释器 + Flash 显示对象模型实现。
#include "swf/Fvm.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "swf/Decoder.hpp"   // Movie 完整定义

namespace swf {
namespace fvm {

namespace {

class Inst {
public:
    Inst(const std::vector<uint8_t>& code) : c_(code) {}
    bool atEnd() const { return p_ >= c_.size(); }
    size_t pos() const { return p_; }
    void seek(size_t p) { p_ = p; }
    uint8_t u8() { return (p_ < c_.size()) ? c_[p_++] : 0; }
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

// ---------- Value ----------
std::string Value::toString() const {
    switch (kind) {
        case VKind::String: return s;
        case VKind::Int:  return std::to_string(i);
        case VKind::UInt: return std::to_string(u);
        case VKind::Number: {
            double dv = d;
            if (dv == std::floor(dv) && std::fabs(dv) < 1e15) return std::to_string(static_cast<int64_t>(dv));
            return std::to_string(dv);
        }
        case VKind::Bool: return b ? "true" : "false";
        case VKind::Null: return "null";
        case VKind::Undefined: return "undefined";
        case VKind::Object: {
            if (!obj) return "null";
            auto p = obj->props.find("text");
            if (p != obj->props.end()) return p->second.toString();
            return "[object " + (obj->className.empty() ? std::string("Object") : obj->className) + "]";
        }
        default: return "undefined";
    }
}

// ---------- Vm ----------
Vm::Vm(const abc::AbcFile& abc, Movie& movie) : abc_(abc), movie_(movie) {}

std::shared_ptr<Object> Vm::newObject(ObjKind kind, const std::string& cls) {
    auto o = std::make_shared<Object>();
    o->kind = kind;
    o->className = cls;
    if (kind == ObjKind::DisplayObject || kind == ObjKind::DisplayObjectContainer ||
        kind == ObjKind::InteractiveObject || kind == ObjKind::Sprite || kind == ObjKind::MovieClip ||
        kind == ObjKind::Stage || kind == ObjKind::TextField || kind == ObjKind::Bitmap ||
        kind == ObjKind::Shape || kind == ObjKind::Loader) {
        auto d = std::make_shared<DisplayObject>();
        d->kind = kind;
        o->display = d;
    }
    objects_.push_back(o);
    return o;
}

std::string Vm::multinameName(uint32_t index) const {
    const abc::Multiname* m = abc_.mn(index);
    return m ? abc_.str(m->name) : std::string();
}

std::string Vm::qualifiedName(uint32_t index) const {
    const abc::Multiname* m = abc_.mn(index);
    if (!m) return "";
    std::string nm = abc_.str(m->name);
    std::string ns;
    if (m->ns < abc_.namespaces.size()) ns = abc_.str(abc_.namespaces[m->ns].name);
    return ns.empty() ? nm : (ns + "." + nm);
}

// ---- 类构建 ----
void Vm::collectTraits(const std::vector<abc::Traits>& src, std::vector<Member>& out, bool isStatic) {
    for (const auto& t : src) {
        Member m;
        m.isStatic = isStatic;
        m.name = multinameName(t.name);
        m.slotId = static_cast<int>(t.slotId);
        m.methodIndex = t.functionName;
        switch (t.kind) {
            case 0x00: m.kind = Member::Slot;  break;                 // Slot
            case 0x06: m.kind = Member::Const; break;                 // Const
            case 0x01: m.kind = Member::Method; break;                // Method
            case 0x02: m.kind = Member::Getter; break;                // Getter
            case 0x03: m.kind = Member::Setter; break;                // Setter
            case 0x04: m.kind = Member::ClassMember; break;           // Class
            case 0x05: m.kind = Member::Method; break;                // Function
            default:   m.kind = Member::Slot;  break;
        }
        out.push_back(m);
    }
}

std::shared_ptr<ClassDef> Vm::buildClass(int instanceIndex) {
    auto it = classCache_.find(instanceIndex);
    if (it != classCache_.end()) return it->second;

    auto cd = std::make_shared<ClassDef>();
    cd->instanceIndex = instanceIndex;
    classCache_[instanceIndex] = cd;   // 先放入，避免递归构造死循环

    if (instanceIndex < 0 || instanceIndex >= static_cast<int>(abc_.instances.size())) return cd;
    const auto& inst = abc_.instances[instanceIndex];
    cd->name = qualifiedName(inst.name);
    cd->superInstanceIndex = static_cast<int>(inst.superClass);

    // 父类
    if (inst.superClass > 0 && inst.superClass < abc_.multinames.size()) {
        // 通过 multiname 名找到对应 instance
        std::string supName = qualifiedName(inst.superClass);
        for (std::size_t k = 0; k < abc_.instances.size(); ++k) {
            if (qualifiedName(abc_.instances[k].name) == supName && static_cast<int>(k) != instanceIndex) {
                cd->superClass = buildClass(static_cast<int>(k));
                break;
            }
        }
    }

    collectTraits(inst.traits, cd->instanceMembers, false);
    if (instanceIndex < static_cast<int>(abc_.classes.size())) {
        collectTraits(abc_.classes[instanceIndex].traits, cd->staticMembers, true);
        cd->cinitMethod = abc_.classes[instanceIndex].cinit;
    }
    return cd;
}

// ---- 类静态初始化 ----
void Vm::ensureClassInit(std::shared_ptr<ClassDef> cd, std::shared_ptr<Object> classObj) {
    if (!cd || cd->cinitDone) return;
    cd->cinitDone = true;                       // 先标记，避免递归
    if (cd->superClass) ensureClassInit(cd->superClass, nullptr);
    if (cd->cinitMethod < abc_.methods.size()) {
        int bi = abc_.methods[cd->cinitMethod].bodyIndex;
        if (bi >= 0 && bi < static_cast<int>(abc_.methodBodies.size()))
            exec(abc_.methodBodies[bi], {}, classObj);
    }
}

// ---- 属性访问（含类成员链与 getter/setter）----
Value Vm::getProperty(std::shared_ptr<Object> obj, const std::string& name) {
    if (!obj) return Value::undefined();
    // 1) 动态属性
    auto dp = obj->props.find(name);
    if (dp != obj->props.end()) return dp->second;
    // 2) slot
    auto sp = obj->slots.find(name);
    if (sp != obj->slots.end()) return sp->second;
    // 3) 类成员链（实例成员 -> 静态成员 -> 父类）
    for (auto c = obj->cls; c; c = c->superClass) {
        for (const auto& m : c->instanceMembers) {
            if (m.name != name) continue;
            if (m.kind == Member::Getter && m.native) return m.native(obj, {});
            if (m.kind == Member::Method) {
                auto fn = newObject(ObjKind::Function, name);
                fn->props["$method"] = Value::uintv(m.methodIndex);
                fn->props["$this"] = Value::object(obj);
                return Value::object(fn);
            }
            if (m.kind == Member::Slot || m.kind == Member::Const) return m.defaultValue;
        }
        for (const auto& m : c->staticMembers) {
            if (m.name != name) continue;
            if (m.kind == Member::Getter && m.native) return m.native(obj, {});
            if (m.kind == Member::Method) {
                auto fn = newObject(ObjKind::Function, name);
                fn->props["$method"] = Value::uintv(m.methodIndex);
                fn->props["$this"] = Value::object(obj);
                return Value::object(fn);
            }
            if (m.kind == Member::Slot || m.kind == Member::Const) return m.defaultValue;
        }
    }
    // 4) 原生类（显示对象等的原生成员）
    if (obj->classDef) {
        for (const auto& m : obj->classDef->instanceMembers)
            if (m.name == name && m.native) {
                if (m.kind == Member::Getter) return m.native(obj, {});
                auto fn = newObject(ObjKind::Function, name);
                fn->props["$native"] = Value::str(name);
                fn->props["$this"] = Value::object(obj);
                return Value::object(fn);
            }
    }
    return Value::undefined();
}

void Vm::setProperty(std::shared_ptr<Object> obj, const std::string& name, const Value& v) {
    if (!obj) return;
    // setter 优先
    for (auto c = obj->cls; c; c = c->superClass) {
        for (const auto& m : c->instanceMembers)
            if (m.name == name && m.kind == Member::Setter) {
                if (m.native) { m.native(obj, {v}); return; }
                if (m.methodIndex < abc_.methods.size()) {
                    int bi = abc_.methods[m.methodIndex].bodyIndex;
                    if (bi >= 0 && bi < static_cast<int>(abc_.methodBodies.size()))
                        exec(abc_.methodBodies[bi], {v}, obj);
                    return;
                }
            }
    }
    // 显示对象内建属性同步
    if (obj->display) {
        if (name == "x") { obj->display->x = v.toNumber(); obj->props[name] = v; return; }
        if (name == "y") { obj->display->y = v.toNumber(); obj->props[name] = v; return; }
        if (name == "scaleX") { obj->display->scaleX = v.toNumber(); obj->props[name] = v; return; }
        if (name == "scaleY") { obj->display->scaleY = v.toNumber(); obj->props[name] = v; return; }
        if (name == "rotation") { obj->display->rotation = v.toNumber(); obj->props[name] = v; return; }
        if (name == "alpha") { obj->display->alpha = v.toNumber(); obj->props[name] = v; return; }
        if (name == "visible") { obj->display->visible = v.toBool(); obj->props[name] = v; return; }
        if (name == "name") { obj->display->name = v.toString(); obj->props[name] = v; return; }
        // 运行时 TextField 文本属性
        if (name == "text" || name == "htmlText") {
            obj->display->textData = v.toString();
            obj->display->hasText = true;
            obj->props[name] = v; return;
        }
        if (name == "textColor") {
            obj->display->textColor = static_cast<uint32_t>(v.toUInt());
            obj->props[name] = v; return;
        }
        if (name == "defaultTextFormat" || name == "setTextFormat") {
            // 从 TextFormat 对象读取 size / color / font
            if (v.kind == VKind::Object && v.obj) {
                auto fmt = v.obj;
                auto ps = fmt->props.find("size");
                if (ps != fmt->props.end()) obj->display->textSize = ps->second.toInt();
                auto pc = fmt->props.find("color");
                if (pc != fmt->props.end()) obj->display->textColor = static_cast<uint32_t>(pc->second.toUInt());
                auto pf = fmt->props.find("font");
                if (pf != fmt->props.end()) obj->display->textFont = pf->second.toString();
            }
            obj->props[name] = v; return;
        }
    }
    obj->props[name] = v;
}

Value Vm::callMethod(std::shared_ptr<Object> obj, const std::string& name, const std::vector<Value>& args) {
    if (!obj) return Value::undefined();
    // 0) 全局内建函数
    if (name == "trace") {
        for (size_t i = 0; i < args.size(); ++i) {
            if (i) std::printf(" ");
            std::printf("%s", args[i].toString().c_str());
        }
        std::printf("\n");
        return Value::undefined();
    }
    if (name == "parseInt" && !args.empty()) return Value::intg(static_cast<int32_t>(args[0].toNumber()));
    if (name == "parseFloat" && !args.empty()) return Value::num(args[0].toNumber());
    if (name == "isNaN" && !args.empty()) return Value::boolean(std::isnan(args[0].toNumber()));
    // 1) 原生类方法
    if (obj->classDef) {
        for (const auto& m : obj->classDef->instanceMembers)
            if (m.name == name && m.native && m.kind == Member::Method)
                return m.native(obj, args);
        for (const auto& m : obj->classDef->staticMembers)
            if (m.name == name && m.native && m.kind == Member::Method)
                return m.native(obj, args);
    }
    // 2) 类成员方法（字节码）
    for (auto c = obj->cls; c; c = c->superClass) {
        for (const auto& m : c->instanceMembers)
            if (m.name == name && m.kind == Member::Method && m.methodIndex < abc_.methods.size()) {
                int bi = abc_.methods[m.methodIndex].bodyIndex;
                if (bi >= 0 && bi < static_cast<int>(abc_.methodBodies.size()))
                    return exec(abc_.methodBodies[bi], args, obj);
            }
    }
    // 3) 动态属性中的函数
    auto dp = obj->props.find(name);
    if (dp != obj->props.end() && dp->second.kind == VKind::Object && dp->second.obj) {
        auto fn = dp->second.obj;
        auto mi = fn->props.find("$method");
        if (mi != fn->props.end()) {
            uint32_t midx = mi->second.u;
            if (midx < abc_.methods.size()) {
                int bi = abc_.methods[midx].bodyIndex;
                if (bi >= 0 && bi < static_cast<int>(abc_.methodBodies.size()))
                    return exec(abc_.methodBodies[bi], args, obj);
            }
        }
    }
    return Value::undefined();
}

// ---- 原生 AS3 / Flash API 注册 ----
void Vm::registerNativeClasses() {
    auto mkClass = [&](const char* name, ObjKind k) -> std::shared_ptr<Object> {
        auto c = newObject(ObjKind::Class, name);
        auto cd = std::make_shared<ClassDef>();
        cd->name = name;
        c->classDef = cd;
        Value v = Value::object(c);
        globals_->props[name] = v;
        std::string qn = std::string("flash.display.") + name;
        globals_->props[qn] = v;
        return c;
    };

    // ---- Object ----
    {
        auto c = mkClass("Object", ObjKind::Generic);
        Member m; m.kind = Member::Method; m.name = "toString";
        m.native = [](std::shared_ptr<Object> self, const std::vector<Value>&) -> Value {
            if (!self) return Value::str("[object Object]");
            auto t = self->props.find("text");
            if (t != self->props.end()) return Value::str(t->second.toString());
            std::string cn = self->className.empty() ? std::string("Object") : self->className;
            return Value::str("[object " + cn + "]");
        };
        c->classDef->instanceMembers.push_back(m);
        Member hv; hv.kind = Member::Method; hv.name = "hasOwnProperty";
        hv.native = [](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
            if (!self || a.empty()) return Value::boolean(false);
            return Value::boolean(self->props.count(a[0].toString()) > 0);
        };
        c->classDef->instanceMembers.push_back(hv);
    }

    // ---- Array ----
    {
        auto c = mkClass("Array", ObjKind::Array);
        Member len; len.kind = Member::Getter; len.name = "length";
        len.native = [](std::shared_ptr<Object> self, const std::vector<Value>&) -> Value {
            if (!self) return Value::intg(0);
            auto p = self->props.find("$length");
            return p != self->props.end() ? p->second : Value::intg(0);
        };
        c->classDef->instanceMembers.push_back(len);
        Member push; push.kind = Member::Method; push.name = "push";
        push.native = [](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
            if (!self) return Value::intg(0);
            int n = self->props.count("$length") ? self->props["$length"].toInt() : 0;
            for (const auto& v : a) { self->props[std::to_string(n)] = v; ++n; }
            self->props["$length"] = Value::intg(n);
            return Value::intg(n);
        };
        c->classDef->instanceMembers.push_back(push);
        Member pop; pop.kind = Member::Method; pop.name = "pop";
        pop.native = [](std::shared_ptr<Object> self, const std::vector<Value>&) -> Value {
            if (!self) return Value::undefined();
            int n = self->props.count("$length") ? self->props["$length"].toInt() : 0;
            if (n <= 0) return Value::undefined();
            auto it = self->props.find(std::to_string(n - 1));
            Value r = (it != self->props.end()) ? it->second : Value::undefined();
            self->props.erase(std::to_string(n - 1));
            self->props["$length"] = Value::intg(n - 1);
            return r;
        };
        c->classDef->instanceMembers.push_back(pop);
        Member join; join.kind = Member::Method; join.name = "join";
        join.native = [](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
            if (!self) return Value::str("");
            std::string sep = a.empty() ? "," : a[0].toString();
            int n = self->props.count("$length") ? self->props["$length"].toInt() : 0;
            std::string out;
            for (int i = 0; i < n; ++i) {
                if (i) out += sep;
                auto it = self->props.find(std::to_string(i));
                if (it != self->props.end()) out += it->second.toString();
            }
            return Value::str(out);
        };
        c->classDef->instanceMembers.push_back(join);
        Member idx; idx.kind = Member::Method; idx.name = "indexOf";
        idx.native = [](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
            if (!self || a.empty()) return Value::intg(-1);
            int n = self->props.count("$length") ? self->props["$length"].toInt() : 0;
            for (int i = 0; i < n; ++i) {
                auto it = self->props.find(std::to_string(i));
                if (it != self->props.end() && it->second.toString() == a[0].toString()) return Value::intg(i);
            }
            return Value::intg(-1);
        };
        c->classDef->instanceMembers.push_back(idx);
    }

    // ---- String ----
    {
        auto c = mkClass("String", ObjKind::String);
        Member len; len.kind = Member::Getter; len.name = "length";
        len.native = [](std::shared_ptr<Object> self, const std::vector<Value>&) -> Value {
            auto p = self->props.find("$str");
            return Value::intg(static_cast<int32_t>(p != self->props.end() ? p->second.s.size() : 0));
        };
        c->classDef->instanceMembers.push_back(len);
        Member ts; ts.kind = Member::Method; ts.name = "toString";
        ts.native = [](std::shared_ptr<Object> self, const std::vector<Value>&) -> Value {
            auto p = self->props.find("$str");
            return p != self->props.end() ? p->second : Value::str("");
        };
        c->classDef->instanceMembers.push_back(ts);
        Member sub; sub.kind = Member::Method; sub.name = "substring";
        sub.native = [](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
            auto p = self->props.find("$str");
            std::string s = (p != self->props.end()) ? p->second.s : std::string();
            int i0 = a.size() > 0 ? a[0].toInt() : 0;
            int i1 = a.size() > 1 ? a[1].toInt() : static_cast<int>(s.size());
            if (i0 < 0) i0 = 0;
            if (i1 > static_cast<int>(s.size())) i1 = static_cast<int>(s.size());
            if (i0 >= i1) return Value::str("");
            return Value::str(s.substr(i0, i1 - i0));
        };
        c->classDef->instanceMembers.push_back(sub);
        Member io; io.kind = Member::Method; io.name = "indexOf";
        io.native = [](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
            auto p = self->props.find("$str");
            std::string s = (p != self->props.end()) ? p->second.s : std::string();
            if (a.empty()) return Value::intg(-1);
            auto pos = s.find(a[0].toString());
            return Value::intg(pos == std::string::npos ? -1 : static_cast<int32_t>(pos));
        };
        c->classDef->instanceMembers.push_back(io);
        Member up; up.kind = Member::Method; up.name = "toUpperCase";
        up.native = [](std::shared_ptr<Object> self, const std::vector<Value>&) -> Value {
            auto p = self->props.find("$str");
            std::string s = (p != self->props.end()) ? p->second.s : std::string();
            for (auto& ch : s) if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 32);
            return Value::str(s);
        };
        c->classDef->instanceMembers.push_back(up);
        Member lo; lo.kind = Member::Method; lo.name = "toLowerCase";
        lo.native = [](std::shared_ptr<Object> self, const std::vector<Value>&) -> Value {
            auto p = self->props.find("$str");
            std::string s = (p != self->props.end()) ? p->second.s : std::string();
            for (auto& ch : s) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + 32);
            return Value::str(s);
        };
        c->classDef->instanceMembers.push_back(lo);
    }

    // ---- Math ----
    {
        auto c = mkClass("Math", ObjKind::Math);
        auto addMath = [&](const char* n, int nargs, NativeFn fn) {
            Member m; m.kind = Member::Method; m.name = n; m.native = fn;
            c->classDef->staticMembers.push_back(m);
        };
        auto addConst = [&](const char* n, double v) {
            Member m; m.kind = Member::Const; m.name = n; m.defaultValue = Value::num(v);
            m.isStatic = true;
            c->classDef->staticMembers.push_back(m);
        };
        addMath("abs", 1, [](std::shared_ptr<Object>, const std::vector<Value>& a) {
            return Value::num(a.empty() ? 0.0 : std::fabs(a[0].toNumber())); });
        addMath("floor", 1, [](std::shared_ptr<Object>, const std::vector<Value>& a) {
            return Value::num(a.empty() ? 0.0 : std::floor(a[0].toNumber())); });
        addMath("ceil", 1, [](std::shared_ptr<Object>, const std::vector<Value>& a) {
            return Value::num(a.empty() ? 0.0 : std::ceil(a[0].toNumber())); });
        addMath("round", 1, [](std::shared_ptr<Object>, const std::vector<Value>& a) {
            return Value::num(a.empty() ? 0.0 : std::round(a[0].toNumber())); });
        addMath("sqrt", 1, [](std::shared_ptr<Object>, const std::vector<Value>& a) {
            return Value::num(a.empty() ? 0.0 : std::sqrt(a[0].toNumber())); });
        addMath("pow", 2, [](std::shared_ptr<Object>, const std::vector<Value>& a) {
            return Value::num(a.size() < 2 ? 0.0 : std::pow(a[0].toNumber(), a[1].toNumber())); });
        addMath("random", 0, [](std::shared_ptr<Object>, const std::vector<Value>&) {
            return Value::num(static_cast<double>(std::rand()) / RAND_MAX); });
        addMath("max", 2, [](std::shared_ptr<Object>, const std::vector<Value>& a) {
            return Value::num(a.size() < 2 ? 0.0 : std::fmax(a[0].toNumber(), a[1].toNumber())); });
        addMath("min", 2, [](std::shared_ptr<Object>, const std::vector<Value>& a) {
            return Value::num(a.size() < 2 ? 0.0 : std::fmin(a[0].toNumber(), a[1].toNumber())); });
        addMath("sin", 1, [](std::shared_ptr<Object>, const std::vector<Value>& a) {
            return Value::num(a.empty() ? 0.0 : std::sin(a[0].toNumber())); });
        addMath("cos", 1, [](std::shared_ptr<Object>, const std::vector<Value>& a) {
            return Value::num(a.empty() ? 0.0 : std::cos(a[0].toNumber())); });
        addConst("PI", 3.14159265358979323846);
        addConst("E", 2.7182818284590452354);
    }

    // ---- Number ----
    {
        auto c = mkClass("Number", ObjKind::Number);
        Member tv; tv.kind = Member::Method; tv.name = "toString";
        tv.native = [](std::shared_ptr<Object> self, const std::vector<Value>&) -> Value {
            auto p = self->props.find("$num");
            return Value::str(p != self->props.end() ? p->second.toString() : "0");
        };
        c->classDef->instanceMembers.push_back(tv);
        Member tf; tf.kind = Member::Method; tf.name = "toFixed";
        tf.native = [](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
            auto p = self->props.find("$num");
            double v = (p != self->props.end()) ? p->second.toNumber() : 0.0;
            int digits = a.empty() ? 0 : a[0].toInt();
            char buf[64];
            snprintf(buf, sizeof(buf), "%.*f", digits, v);
            return Value::str(std::string(buf));
        };
        c->classDef->instanceMembers.push_back(tf);
        globals_->props["parseFloat"] = Value::undefined();
        globals_->props["isNaN"] = Value::undefined();
    }

    // ---- int / uint / Boolean ----
    for (const char* tn : {"int", "uint", "Boolean"}) {
        auto c = mkClass(tn, ObjKind::Number);
        Member tv; tv.kind = Member::Method; tv.name = "toString";
        tv.native = [](std::shared_ptr<Object> self, const std::vector<Value>&) -> Value {
            auto p = self->props.find("$num");
            return Value::str(p != self->props.end() ? p->second.toString() : "0");
        };
        c->classDef->instanceMembers.push_back(tv);
    }

    // ---- Function ----
    {
        auto c = mkClass("Function", ObjKind::Function);
        Member cl; cl.kind = Member::Method; cl.name = "call";
        cl.native = [this](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
            if (!self) return Value::undefined();
            auto mi = self->props.find("$method");
            if (mi == self->props.end()) return Value::undefined();
            uint32_t midx = mi->second.u;
            if (midx >= abc_.methods.size()) return Value::undefined();
            int bi = abc_.methods[midx].bodyIndex;
            if (bi < 0 || bi >= static_cast<int>(abc_.methodBodies.size())) return Value::undefined();
            std::vector<Value> args(a.size() > 1 ? a.size() - 1 : 0);
            for (size_t k = 1; k < a.size(); ++k) args[k - 1] = a[k];
            auto th = a.empty() ? nullptr : (a[0].kind == VKind::Object ? a[0].obj : nullptr);
            return exec(abc_.methodBodies[bi], args, th);
        };
        c->classDef->instanceMembers.push_back(cl);
        Member ap; ap.kind = Member::Method; ap.name = "apply";
        ap.native = [this](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
            if (!self) return Value::undefined();
            auto mi = self->props.find("$method");
            if (mi == self->props.end()) return Value::undefined();
            uint32_t midx = mi->second.u;
            if (midx >= abc_.methods.size()) return Value::undefined();
            int bi = abc_.methods[midx].bodyIndex;
            if (bi < 0 || bi >= static_cast<int>(abc_.methodBodies.size())) return Value::undefined();
            std::vector<Value> args;
            if (a.size() > 1 && a[1].kind == VKind::Object && a[1].obj) {
                auto arr = a[1].obj;
                int n = arr->props.count("$length") ? arr->props["$length"].toInt() : 0;
                for (int k = 0; k < n; ++k) {
                    auto it = arr->props.find(std::to_string(k));
                    if (it != arr->props.end()) args.push_back(it->second);
                }
            }
            auto th = a.empty() ? nullptr : (a[0].kind == VKind::Object ? a[0].obj : nullptr);
            return exec(abc_.methodBodies[bi], args, th);
        };
        c->classDef->instanceMembers.push_back(ap);
    }

    // ---- Date ----
    {
        auto c = mkClass("Date", ObjKind::Generic);
        Member gt; gt.kind = Member::Method; gt.name = "getTime";
        gt.native = [](std::shared_ptr<Object>, const std::vector<Value>&) -> Value { return Value::num(0); };
        c->classDef->instanceMembers.push_back(gt);
    }

    // ---- JSON ----
    {
        auto c = mkClass("JSON", ObjKind::Generic);
        Member st; st.kind = Member::Method; st.name = "stringify"; st.isStatic = true;
        st.native = [](std::shared_ptr<Object>, const std::vector<Value>& a) -> Value {
            return Value::str(a.empty() ? "" : a[0].toString());
        };
        c->classDef->staticMembers.push_back(st);
        Member ps; ps.kind = Member::Method; ps.name = "parse"; ps.isStatic = true;
        ps.native = [](std::shared_ptr<Object>, const std::vector<Value>&) -> Value { return Value::undefined(); };
        c->classDef->staticMembers.push_back(ps);
    }

    // ---- Stage ----
    {
        auto c = mkClass("Stage", ObjKind::Stage);
        Member sw; sw.kind = Member::Getter; sw.name = "stageWidth";
        sw.native = [this](std::shared_ptr<Object>, const std::vector<Value>&) -> Value {
            return Value::num((movie_.frameSize.x1 - movie_.frameSize.x0) / 20.0);
        };
        c->classDef->instanceMembers.push_back(sw);
        Member sh; sh.kind = Member::Getter; sh.name = "stageHeight";
        sh.native = [this](std::shared_ptr<Object>, const std::vector<Value>&) -> Value {
            return Value::num((movie_.frameSize.y1 - movie_.frameSize.y0) / 20.0);
        };
        c->classDef->instanceMembers.push_back(sh);
        Member fr; fr.kind = Member::Getter; fr.name = "frameRate";
        fr.native = [this](std::shared_ptr<Object>, const std::vector<Value>&) -> Value {
            return Value::num(movie_.frameRate);
        };
        c->classDef->instanceMembers.push_back(fr);
    }

    // ---- Event ----
    {
        auto c = mkClass("Event", ObjKind::Event);
        Member tp; tp.kind = Member::Getter; tp.name = "type";
        tp.native = [](std::shared_ptr<Object> self, const std::vector<Value>&) -> Value {
            auto p = self->props.find("type");
            return p != self->props.end() ? p->second : Value::str("");
        };
        c->classDef->instanceMembers.push_back(tp);
        Member en; en.kind = Member::Method; en.name = "ENTER_FRAME"; en.isStatic = true;
        en.defaultValue = Value::str("enterFrame");
        c->classDef->staticMembers.push_back(en);
    }

    // ---- 其余常用类（Shape / Bitmap / BitmapData / Loader / LoaderInfo / Sound /
    //      SoundChannel / URLLoader / XML / RegExp / ByteArray / Vector / Dictionary /
    //      SharedObject / Error / MouseEvent / KeyboardEvent ----
    for (const char* nm : {"Shape", "Bitmap", "BitmapData", "Loader", "LoaderInfo",
                           "Sound", "SoundChannel", "URLLoader", "URLRequest", "XML",
                           "XMLList", "RegExp", "ByteArray", "Vector", "Dictionary",
                           "SharedObject", "Error", "MouseEvent", "KeyboardEvent",
                           "TextFormat", "Point", "Rectangle", "Matrix", "ColorTransform",
                           "Tween", "TweenLite", "TweenMax", "NetConnection", "NetStream"}) {
        auto c = mkClass(nm, ObjKind::Generic);
        auto cd = c->classDef;
        // 通用：toString
        Member ts; ts.kind = Member::Method; ts.name = "toString";
        ts.native = [](std::shared_ptr<Object> self, const std::vector<Value>&) -> Value {
            std::string cn = self && !self->className.empty() ? self->className : std::string("Object");
            return Value::str("[object " + cn + "]");
        };
        cd->instanceMembers.push_back(ts);
        // 常用无副作用方法
        for (const char* mn : {"start", "stop", "play", "close", "load", "reset",
                               "writeByte", "writeBytes", "readByte", "readBytes",
                               "getBytesTotal", "getBytesLoaded", "clear", "flush",
                               "setProperty", "getProperty", "clone", "concat"}) {
            Member m; m.kind = Member::Method; m.name = mn;
            m.native = [](std::shared_ptr<Object>, const std::vector<Value>&) -> Value {
                return Value::undefined();
            };
            cd->instanceMembers.push_back(m);
        }
    }

    // ---- 全局函数（值占位，解释器按名字处理）----
    globals_->props["parseInt"] = Value::undefined();
    globals_->props["parseFloat"] = Value::undefined();
    globals_->props["isNaN"] = Value::undefined();
    globals_->props["trace"] = Value::undefined();
    globals_->props["NaN"] = Value::num(NAN);
    globals_->props["Infinity"] = Value::num(INFINITY);
    globals_->props["undefined"] = Value::undefined();
}

// ---- Flash 显示 API ----
void Vm::registerFlashApi() {
    auto mkClass = [&](const char* name, ObjKind k) -> std::shared_ptr<Object> {
        auto c = newObject(ObjKind::Class, name);
        auto cd = std::make_shared<ClassDef>();
        cd->name = name;
        c->classDef = cd;
        c->kind = ObjKind::Class;
        Value v = Value::object(c);
        globals_->props[name] = v;
        std::string qn = std::string("flash.display.") + name;
        globals_->props[qn] = v;
        std::string qe = std::string("flash.events.") + name;
        globals_->props[qe] = v;
        return c;
    };

    // ---- EventDispatcher（事件系统，游戏逻辑核心）----
    {
        auto c = mkClass("EventDispatcher", ObjKind::EventDispatcher);
        Member add; add.kind = Member::Method; add.name = "addEventListener";
        add.native = [](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
            if (self && a.size() >= 2) self->listeners[a[0].toString()].push_back(a[1]);
            return Value::undefined();
        };
        c->classDef->instanceMembers.push_back(add);
        Member rem; rem.kind = Member::Method; rem.name = "removeEventListener";
        rem.native = [](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
            if (self && !a.empty()) self->listeners.erase(a[0].toString());
            return Value::undefined();
        };
        c->classDef->instanceMembers.push_back(rem);
        Member disp; disp.kind = Member::Method; disp.name = "dispatchEvent";
        disp.native = [](std::shared_ptr<Object> self, const std::vector<Value>&) -> Value {
            return Value::boolean(self != nullptr);
        };
        c->classDef->instanceMembers.push_back(disp);
        Member has; has.kind = Member::Method; has.name = "hasEventListener";
        has.native = [](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
            if (!self || a.empty()) return Value::boolean(false);
            return Value::boolean(self->listeners.count(a[0].toString()) > 0);
        };
        c->classDef->instanceMembers.push_back(has);
    }

    // ---- DisplayObject / Sprite / MovieClip（显示列表操作）----
    auto addDisplayMembers = [this](std::shared_ptr<ClassDef> cd, ObjKind k) {
        Member addChild; addChild.kind = Member::Method; addChild.name = "addChild";
        addChild.native = [](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
            if (!self || !self->display || a.empty() || !a[0].obj || !a[0].obj->display) return a.empty() ? Value::undefined() : a[0];
            auto child = a[0].obj->display;
            child->parent = self->display;
            self->display->children.push_back(child);
            return a[0];
        };
        cd->instanceMembers.push_back(addChild);
        Member removeChild; removeChild.kind = Member::Method; removeChild.name = "removeChild";
        removeChild.native = [](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
            if (!self || !self->display || a.empty() || !a[0].obj || !a[0].obj->display) return Value::undefined();
            auto& ch = self->display->children;
            for (auto it = ch.begin(); it != ch.end(); ++it)
                if (it->get() == a[0].obj->display.get()) { ch.erase(it); break; }
            return a[0];
        };
        cd->instanceMembers.push_back(removeChild);
        Member numChildren; numChildren.kind = Member::Getter; numChildren.name = "numChildren";
        numChildren.native = [](std::shared_ptr<Object> self, const std::vector<Value>&) -> Value {
            return Value::intg(self && self->display ? static_cast<int32_t>(self->display->children.size()) : 0);
        };
        cd->instanceMembers.push_back(numChildren);
        // getChildAt(i)
        Member gca; gca.kind = Member::Method; gca.name = "getChildAt";
        gca.native = [this](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
            if (!self || !self->display || a.empty()) return Value::nullv();
            int i = a[0].toInt();
            const auto& ch = self->display->children;
            if (i < 0 || i >= static_cast<int>(ch.size())) return Value::nullv();
            auto o = newObject(ch[i]->kind == ObjKind::DisplayObject ? ObjKind::MovieClip : ch[i]->kind, "DisplayObject");
            o->display = ch[i];
            return Value::object(o);
        };
        cd->instanceMembers.push_back(gca);
        // getChildByName(n)
        Member gcn; gcn.kind = Member::Method; gcn.name = "getChildByName";
        gcn.native = [this](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
            if (!self || !self->display || a.empty()) return Value::nullv();
            std::string n = a[0].toString();
            for (auto& c : self->display->children) {
                if (c->name == n) {
                    auto o = newObject(ObjKind::MovieClip, n);
                    o->display = c;
                    return Value::object(o);
                }
            }
            return Value::nullv();
        };
        cd->instanceMembers.push_back(gcn);
        // getChildIndex(child)
        Member gci; gci.kind = Member::Method; gci.name = "getChildIndex";
        gci.native = [](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
            if (!self || !self->display || a.empty() || !a[0].obj || !a[0].obj->display) return Value::intg(-1);
            const auto& ch = self->display->children;
            for (size_t i = 0; i < ch.size(); ++i)
                if (ch[i].get() == a[0].obj->display.get()) return Value::intg(static_cast<int32_t>(i));
            return Value::intg(-1);
        };
        cd->instanceMembers.push_back(gci);
        // removeChildAt(i)
        Member rca; rca.kind = Member::Method; rca.name = "removeChildAt";
        rca.native = [](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
            if (!self || !self->display || a.empty()) return Value::undefined();
            int i = a[0].toInt();
            auto& ch = self->display->children;
            if (i >= 0 && i < static_cast<int>(ch.size())) ch.erase(ch.begin() + i);
            return Value::undefined();
        };
        cd->instanceMembers.push_back(rca);
        // contains(child)
        Member cont; cont.kind = Member::Method; cont.name = "contains";
        cont.native = [](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
            if (!self || !self->display || a.empty() || !a[0].obj || !a[0].obj->display) return Value::boolean(false);
            for (auto& c : self->display->children)
                if (c.get() == a[0].obj->display.get()) return Value::boolean(true);
            return Value::boolean(false);
        };
        cd->instanceMembers.push_back(cont);
        // parent
        Member par; par.kind = Member::Getter; par.name = "parent";
        par.native = [this](std::shared_ptr<Object> self, const std::vector<Value>&) -> Value {
            if (!self || !self->display || !self->display->parent) return Value::nullv();
            auto o = newObject(ObjKind::MovieClip, "DisplayObject");
            o->display = self->display->parent;
            return Value::object(o);
        };
        cd->instanceMembers.push_back(par);
        Member stageM; stageM.kind = Member::Getter; stageM.name = "stage";
        stageM.native = [](std::shared_ptr<Object>, const std::vector<Value>&) -> Value {
            return Value::undefined();
        };
        cd->instanceMembers.push_back(stageM);
        (void)k;
    };

    {
        auto c = mkClass("DisplayObject", ObjKind::DisplayObject);
        addDisplayMembers(c->classDef, ObjKind::DisplayObject);
    }
    {
        auto c = mkClass("Sprite", ObjKind::Sprite);
        addDisplayMembers(c->classDef, ObjKind::Sprite);
        Member g; g.kind = Member::Getter; g.name = "graphics";
        g.native = [this](std::shared_ptr<Object> self, const std::vector<Value>&) -> Value {
            auto go = newObject(ObjKind::Graphics, "Graphics");
            go->props["$owner"] = Value::object(self);
            return Value::object(go);
        };
        c->classDef->instanceMembers.push_back(g);
    }
    {
        auto c = mkClass("MovieClip", ObjKind::MovieClip);
        addDisplayMembers(c->classDef, ObjKind::MovieClip);
        Member playM; playM.kind = Member::Method; playM.name = "play";
        playM.native = [](std::shared_ptr<Object> self, const std::vector<Value>&) -> Value {
            if (self && self->display) self->display->playing = true;
            return Value::undefined();
        };
        c->classDef->instanceMembers.push_back(playM);
        Member stopM; stopM.kind = Member::Method; stopM.name = "stop";
        stopM.native = [](std::shared_ptr<Object> self, const std::vector<Value>&) -> Value {
            if (self && self->display) self->display->playing = false;
            return Value::undefined();
        };
        c->classDef->instanceMembers.push_back(stopM);
        Member gf; gf.kind = Member::Getter; gf.name = "currentFrame";
        gf.native = [](std::shared_ptr<Object> self, const std::vector<Value>&) -> Value {
            return Value::intg(self && self->display ? self->display->currentFrame : 0);
        };
        c->classDef->instanceMembers.push_back(gf);
        Member tf; tf.kind = Member::Getter; tf.name = "totalFrames";
        tf.native = [this](std::shared_ptr<Object> self, const std::vector<Value>&) -> Value {
            if (self && self->display && self->display->characterId) {
                auto it = movie_.characters.find(self->display->characterId);
                if (it != movie_.characters.end() && it->second &&
                    it->second->kind == Character::Kind::Sprite)
                    return Value::intg(static_cast<int32_t>(it->second->spriteFrames.size()));
            }
            return Value::intg(1);
        };
        c->classDef->instanceMembers.push_back(tf);
        Member gap; gap.kind = Member::Method; gap.name = "gotoAndPlay";
        gap.native = [](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
            if (self && self->display && !a.empty()) {
                self->display->currentFrame = a[0].toInt();
                self->display->playing = true;
            }
            return Value::undefined();
        };
        c->classDef->instanceMembers.push_back(gap);
        Member gas; gas.kind = Member::Method; gas.name = "gotoAndStop";
        gas.native = [](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
            if (self && self->display && !a.empty()) {
                self->display->currentFrame = a[0].toInt();
                self->display->playing = false;
            }
            return Value::undefined();
        };
        c->classDef->instanceMembers.push_back(gas);
    }
    {
        auto c = mkClass("TextField", ObjKind::TextField);
        addDisplayMembers(c->classDef, ObjKind::TextField);
        Member txt; txt.kind = Member::Getter; txt.name = "text";
        txt.native = [](std::shared_ptr<Object> self, const std::vector<Value>&) -> Value {
            if (!self) return Value::str("");
            auto p = self->props.find("text");
            return p != self->props.end() ? p->second : Value::str("");
        };
        c->classDef->instanceMembers.push_back(txt);
    }
    {
        auto c = mkClass("Timer", ObjKind::Timer);
        Member start; start.kind = Member::Method; start.name = "start";
        start.native = [](std::shared_ptr<Object>, const std::vector<Value>&) -> Value { return Value::undefined(); };
        c->classDef->instanceMembers.push_back(start);
        Member stop; stop.kind = Member::Method; stop.name = "stop";
        stop.native = [](std::shared_ptr<Object>, const std::vector<Value>&) -> Value { return Value::undefined(); };
        c->classDef->instanceMembers.push_back(stop);
    }
    {
        auto c = mkClass("Graphics", ObjKind::Graphics);
        auto noop = [](const char* n) {
            Member m; m.kind = Member::Method; m.name = n;
            m.native = [](std::shared_ptr<Object>, const std::vector<Value>&) -> Value { return Value::undefined(); };
            return m;
        };
        c->classDef->instanceMembers.push_back(noop("beginFill"));
        c->classDef->instanceMembers.push_back(noop("endFill"));
        c->classDef->instanceMembers.push_back(noop("drawRect"));
        c->classDef->instanceMembers.push_back(noop("drawCircle"));
        c->classDef->instanceMembers.push_back(noop("lineStyle"));
        c->classDef->instanceMembers.push_back(noop("moveTo"));
        c->classDef->instanceMembers.push_back(noop("lineTo"));
        c->classDef->instanceMembers.push_back(noop("clear"));
    }
}

// ---- 文档类实例化 ----
void Vm::instantiateDocumentClasses() {
    for (const auto& kv : movie_.symbolClasses) {
        const std::string& cname = kv.first;
        std::uint16_t cid = kv.second;
        // 在 globals 中查找该类（限定名或短名）
        auto it = globals_->props.find(cname);
        if (it == globals_->props.end() || it->second.kind != VKind::Object || !it->second.obj) {
            size_t p = cname.rfind('.');
            if (p == std::string::npos) continue;
            it = globals_->props.find(cname.substr(p + 1));
            if (it == globals_->props.end() || it->second.kind != VKind::Object || !it->second.obj) continue;
        }
        auto* clsObj = it->second.obj.get();
        if (clsObj->instanceIndex < 0 || clsObj->instanceIndex >= static_cast<int>(abc_.instances.size())) continue;

        int ii = clsObj->instanceIndex;
        auto cd = buildClass(ii);
        auto inst = newObject(ObjKind::MovieClip, cname);
        inst->cls = cd;
        inst->instanceIndex = ii;
        inst->display->characterId = cid;

        // 执行实例构造函数
        if (ii < static_cast<int>(abc_.instances.size())) {
            uint32_t init = abc_.instances[ii].iinit;
            if (init < abc_.methods.size()) {
                int bi = abc_.methods[init].bodyIndex;
                if (bi >= 0 && bi < static_cast<int>(abc_.methodBodies.size()))
                    exec(abc_.methodBodies[bi], {}, inst);
            }
        }
        if (!inst->display->parent) {
            bool dup = false;
            for (auto& s : stage) if (s.get() == inst->display.get()) { dup = true; break; }
            if (!dup) stage.push_back(inst->display);
        }
    }
}

void Vm::run() {
    globals_ = newObject(ObjKind::Generic, "$global");
    registerNativeClasses();
    registerFlashApi();

    // 执行各 script 的 init（AS3 顶层代码）
    for (const auto& scr : abc_.scripts) {
        if (scr.init >= abc_.methods.size()) continue;
        int bi = abc_.methods[scr.init].bodyIndex;
        if (bi < 0 || bi >= static_cast<int>(abc_.methodBodies.size())) continue;
        execScript(abc_.methodBodies[bi], globals_);
    }

    instantiateDocumentClasses();
}

// 推进一个显示对象及其子对象的时间轴（动画驱动）。
void Vm::advanceTimeline(const std::shared_ptr<DisplayObject>& d) {
    if (!d) return;
    // MovieClip 且处于播放态：按所属精灵（DefineSprite）的帧数循环推进当前帧
    if (d->kind == ObjKind::MovieClip && d->playing && d->characterId) {
        auto it = movie_.characters.find(d->characterId);
        if (it != movie_.characters.end() && it->second &&
            it->second->kind == Character::Kind::Sprite) {
            int total = static_cast<int>(it->second->spriteFrames.size());
            if (total > 0) {
                ++d->currentFrame;
                if (d->currentFrame >= total) d->currentFrame = 0;   // 循环播放
            }
        }
    }
    for (auto& c : d->children) advanceTimeline(c);
}

// 向所有注册了该类型监听器的对象派发事件（以注册者自身作为回调 this）。
void Vm::dispatchToAll(const std::string& type) {
    auto ev = std::make_shared<Object>();
    ev->kind = ObjKind::Event;
    ev->className = "Event";
    ev->props["type"] = Value::str(type);

    // 快照对象表，避免回调中新增/删除对象导致迭代器失效
    std::vector<std::shared_ptr<Object>> snap = objects_;
    for (auto& obj : snap) {
        if (!obj) continue;
        auto lit = obj->listeners.find(type);
        if (lit == obj->listeners.end()) continue;
        std::vector<Value> ls = lit->second;          // 监听器快照
        for (auto& lv : ls) {
            if (lv.kind != VKind::Object || !lv.obj) continue;
            auto mi = lv.obj->props.find("$method");
            if (mi == lv.obj->props.end()) continue;
            uint32_t midx = mi->second.u;
            if (midx >= abc_.methods.size()) continue;
            int bi = abc_.methods[midx].bodyIndex;
            if (bi < 0 || bi >= static_cast<int>(abc_.methodBodies.size())) continue;
            exec(abc_.methodBodies[bi], {Value::object(ev)}, obj);
        }
    }
}

// 播放一帧：先推进所有时间轴，再派发 Flash 标准帧事件。
void Vm::tick() {
    for (auto& d : stage) advanceTimeline(d);
    dispatchToAll("enterFrame");
    dispatchToAll("frameConstructed");
    dispatchToAll("exitFrame");
}

// ---- 解释器 ----
Value Vm::exec(const abc::MethodBody& body, const std::vector<Value>& args,
               std::shared_ptr<Object> thisObj) {
    std::vector<Value> stack;
    stack.reserve(body.maxStack + 8);
    std::vector<Value> locals(body.localCount + 8);
    if (!locals.empty()) {
        locals[0] = thisObj ? Value::object(thisObj) : Value::undefined();
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

    // 异常处理：exceptionHandlers 每项 5 个 u30 (from,to,target,excType,varName)
    auto findHandler = [&](size_t pc) -> int {
        for (size_t k = 0; k + 4 < body.exceptionHandlers.size(); k += 5) {
            if (pc >= body.exceptionHandlers[k] && pc < body.exceptionHandlers[k + 1])
                return static_cast<int>(k);
        }
        return -1;
    };

    size_t guard = 0;
    while (!in.atEnd()) {
        if (++guard > 200000) break;
        uint8_t op = in.u8();
        switch (op) {
            case 0x03: {   // throw：查找覆盖当前 pc 的 catch 并跳转
                Value ex = pop();
                int h = findHandler(in.pos());
                if (h >= 0) {
                    uint32_t target = body.exceptionHandlers[h + 2];
                    uint32_t varIdx = body.exceptionHandlers[h + 4];
                    if (varIdx < locals.size()) locals[varIdx] = ex;
                    if (target <= codeSize) in.seek(target);
                } else {
                    return Value::undefined();   // 未捕获：结束该方法
                }
                break;
            }
            case 0x1b: {   // lookupswitch：default_offset, case_count-1, case_offsets...
                int32_t defOff = in.s24();
                uint32_t caseCount = in.u30();
                std::vector<int32_t> caseOffs(caseCount + 1);
                for (uint32_t k = 0; k <= caseCount; ++k) caseOffs[k] = in.s24();
                Value idx = pop();
                int32_t n = idx.toInt();
                int32_t chosen = defOff;
                if (n >= 0 && n <= static_cast<int32_t>(caseCount)) chosen = caseOffs[n];
                size_t t = static_cast<size_t>(static_cast<int64_t>(in.pos()) + chosen);
                if (t <= codeSize) in.seek(t);
                break;
            }
            case 0x1c: {   // pushwith
                Value v = pop();
                if (v.kind == VKind::Object && v.obj) scope.push_back(v.obj);
                break;
            }
            case 0x32: {   // hasnext2：遍历属性（简化：始终返回 0 表示无更多）
                in.u30(); in.u30();
                push(Value::intg(0));
                break;
            }
            case 0x1e: { pop(); pop(); push(Value::undefined()); break; }   // nextname
            case 0x23: { pop(); pop(); push(Value::undefined()); break; }   // nextvalue
            case 0x59: {   // getdescendants
                uint32_t mn = in.u30();
                pop();
                push(Value::undefined());
                (void)mn;
                break;
            }
            case 0x57: {   // newactivation
                auto o = newObject(ObjKind::Generic, "Activation");
                push(Value::object(o));
                break;
            }
            case 0x6a: {   // deleteproperty
                uint32_t mn = in.u30();
                Value obj = pop();
                std::string name = multinameName(mn);
                if (obj.kind == VKind::Object && obj.obj) {
                    obj.obj->props.erase(name);
                    push(Value::boolean(true));
                } else push(Value::boolean(false));
                break;
            }
            case 0x04: {   // getsuper
                uint32_t mn = in.u30();
                pop();
                push(Value::undefined());
                (void)mn;
                break;
            }
            case 0x05: {   // setsuper
                uint32_t mn = in.u30();
                pop(); pop();
                (void)mn;
                break;
            }
            case 0x45: {   // callsuper
                uint32_t mn = in.u30();
                uint32_t argc = in.u30();
                for (uint32_t k = 0; k < argc; ++k) pop();
                pop();
                push(Value::undefined());
                (void)mn;
                break;
            }
            case 0x4c: {   // callproplex
                uint32_t mn = in.u30();
                uint32_t argc = in.u30();
                std::vector<Value> cargs(argc);
                for (uint32_t k = 0; k < argc; ++k) cargs[argc - 1 - k] = pop();
                Value objv = pop();
                std::string mname = multinameName(mn);
                if (objv.kind == VKind::Object && objv.obj) push(callMethod(objv.obj, mname, cargs));
                else push(Value::undefined());
                break;
            }
            case 0x5a: {   // newcatch
                uint32_t idx = in.u30();
                auto o = newObject(ObjKind::Generic, "Catch");
                o->props["$catchIdx"] = Value::uintv(idx);
                push(Value::object(o));
                break;
            }
            case 0x86: {   // astype
                in.u30();
                break;    // 保持栈顶值
            }
            case 0x87: {   // astypelate
                pop();     // 类型
                break;     // 值保留
            }
            case 0xb2: {   // istype
                in.u30();
                push(Value::boolean(false));
                break;
            }
            case 0xb3: {   // istypelate
                Value t = pop(); Value v = pop();
                push(Value::boolean(!v.isNullOrUndef()));
                (void)t;
                break;
            }
            case 0xb1: {   // instanceof
                pop(); pop();
                push(Value::boolean(false));
                break;
            }
            case 0x95: {   // typeof
                Value v = pop();
                switch (v.kind) {
                    case VKind::Undefined: push(Value::str("undefined")); break;
                    case VKind::Null:      push(Value::str("object")); break;
                    case VKind::Bool:      push(Value::str("boolean")); break;
                    case VKind::Int: case VKind::UInt: case VKind::Number: push(Value::str("number")); break;
                    case VKind::String:    push(Value::str("string")); break;
                    default:               push(Value::str("object")); break;
                }
                break;
            }
            case 0x92: {   // inclocal
                uint32_t i2 = in.u30();
                if (i2 < locals.size()) locals[i2] = Value::num(locals[i2].toNumber() + 1);
                break;
            }
            case 0x94: {   // declocal
                uint32_t i2 = in.u30();
                if (i2 < locals.size()) locals[i2] = Value::num(locals[i2].toNumber() - 1);
                break;
            }
            case 0xc0: { Value a2 = pop(); push(Value::intg(a2.toInt() + 1)); break; }   // increment_i
            case 0xc1: { Value a2 = pop(); push(Value::intg(a2.toInt() - 1)); break; }   // decrement_i
            case 0xc2: { uint32_t i2 = in.u30(); if (i2 < locals.size()) locals[i2] = Value::intg(locals[i2].toInt() + 1); break; }
            case 0xc3: { uint32_t i2 = in.u30(); if (i2 < locals.size()) locals[i2] = Value::intg(locals[i2].toInt() - 1); break; }
            case 0xc4: { Value a2 = pop(); push(Value::intg(-a2.toInt())); break; }      // negate_i
            case 0xc5: { Value b2 = pop(), a2 = pop(); push(Value::intg(a2.toInt() + b2.toInt())); break; }   // add_i
            case 0xc6: { Value b2 = pop(), a2 = pop(); push(Value::intg(a2.toInt() - b2.toInt())); break; }   // subtract_i
            case 0xc7: { Value b2 = pop(), a2 = pop(); push(Value::intg(a2.toInt() * b2.toInt())); break; }   // multiply_i
            case 0x71: case 0x72: break;    // esc_xelem / esc_xattr（保持栈顶）
            case 0x80: { in.u30(); break; }  // coerce
            case 0x85: break;                // coerce_s
            case 0x78: break;                // checkfilter
            case 0x02: case 0x09: break;                       // nop / label
            case 0x08: break;                                  // kill（局部变量作用域结束，无操作）
            case 0x20: push(Value::nullv()); break;
            case 0x21: push(Value::undefined()); break;
            case 0x24: push(Value::intg(static_cast<int8_t>(in.u8()))); break;
            case 0x25: push(Value::uintv(in.u30())); break;
            case 0x26: push(Value::boolean(true)); break;
            case 0x27: push(Value::boolean(false)); break;
            case 0x28: push(Value::num(NAN)); break;
            case 0x2c: push(Value::str(abc_.str(in.u30()))); break;
            case 0x2d: { uint32_t i2 = in.u30(); push(Value::intg(i2 < abc_.ints.size() ? abc_.ints[i2] : 0)); break; }
            case 0x2e: { uint32_t i2 = in.u30(); push(Value::uintv(i2 < abc_.uints.size() ? abc_.uints[i2] : 0)); break; }
            case 0x2f: { uint32_t i2 = in.u30(); push(Value::num(i2 < abc_.doubles.size() ? abc_.doubles[i2] : 0.0)); break; }
            case 0x29: pop(); break;
            case 0x2a: if (!stack.empty()) push(stack.back()); break;
            case 0x2b: {
                if (stack.size() >= 2) { Value a = pop(), b = pop(); push(a); push(b); }
                break;
            }
            case 0x62: { uint32_t i2 = in.u30(); push(i2 < locals.size() ? locals[i2] : Value::undefined()); break; }
            case 0x63: { uint32_t i2 = in.u30(); Value v = pop(); if (i2 < locals.size()) locals[i2] = v; break; }
            case 0xd0: push(locals.size() > 0 ? locals[0] : Value::undefined()); break;
            case 0xd1: push(locals.size() > 1 ? locals[1] : Value::undefined()); break;
            case 0xd2: push(locals.size() > 2 ? locals[2] : Value::undefined()); break;
            case 0xd3: push(locals.size() > 3 ? locals[3] : Value::undefined()); break;
            case 0xd4: { Value v = pop(); if (!locals.empty()) locals[0] = v; break; }
            case 0xd5: { Value v = pop(); if (locals.size() > 1) locals[1] = v; break; }
            case 0xd6: { Value v = pop(); if (locals.size() > 2) locals[2] = v; break; }
            case 0xd7: { Value v = pop(); if (locals.size() > 3) locals[3] = v; break; }

            case 0x30: {   // pushscope
                Value v = pop();
                if (v.kind == VKind::Object && v.obj) scope.push_back(v.obj);
                break;
            }
            case 0x1d: if (!scope.empty()) scope.pop_back(); break;
            case 0x64: { push(Value::object(globals_)); break; }   // getglobalscope
            case 0x65: { uint32_t i2 = in.u8();
                if (i2 < scope.size()) push(Value::object(scope[i2])); else push(Value::undefined());
                break; }

            case 0x66: {   // getproperty
                uint32_t mn = in.u30();
                Value obj = pop();
                std::string name = multinameName(mn);
                if (obj.kind == VKind::Object && obj.obj) push(getProperty(obj.obj, name));
                else if (obj.kind == VKind::String) {
                    // 基本类型 String 的属性（length 等）
                    auto so = newObject(ObjKind::String, "String");
                    so->props["$str"] = obj;
                    push(getProperty(so, name));
                } else push(Value::undefined());
                break;
            }
            case 0x61: {   // setproperty
                uint32_t mn = in.u30();
                Value val = pop(); Value obj = pop();
                std::string name = multinameName(mn);
                if (obj.kind == VKind::Object && obj.obj) setProperty(obj.obj, name, val);
                break;
            }
            case 0x68: {   // initproperty
                uint32_t mn = in.u30();
                Value val = pop(); Value obj = pop();
                if (obj.kind == VKind::Object && obj.obj) obj.obj->props[multinameName(mn)] = val;
                break;
            }
            case 0x6c: {   // getslot
                uint32_t slot = in.u30();
                Value obj = pop();
                if (obj.kind == VKind::Object && obj.obj) {
                    auto p = obj.obj->props.find("$" + std::to_string(slot));
                    push(p != obj.obj->props.end() ? p->second : Value::undefined());
                } else push(Value::undefined());
                break;
            }
            case 0x6d: {   // setslot
                uint32_t slot = in.u30();
                Value val = pop(); Value obj = pop();
                if (obj.kind == VKind::Object && obj.obj) obj.obj->props["$" + std::to_string(slot)] = val;
                break;
            }

            case 0x10: { int32_t o2 = in.s24(); size_t t = static_cast<size_t>(static_cast<int64_t>(in.pos()) + o2); if (t <= codeSize) in.seek(t); break; }
            case 0x11: { int32_t o2 = in.s24(); if (pop().toBool()) { size_t t = static_cast<size_t>(static_cast<int64_t>(in.pos()) + o2); if (t <= codeSize) in.seek(t); } break; }
            case 0x12: { int32_t o2 = in.s24(); if (!pop().toBool()) { size_t t = static_cast<size_t>(static_cast<int64_t>(in.pos()) + o2); if (t <= codeSize) in.seek(t); } break; }
            case 0x13: { int32_t o2 = in.s24(); Value b2 = pop(), a2 = pop();
                if (a2.toString() == b2.toString()) { size_t t = static_cast<size_t>(static_cast<int64_t>(in.pos()) + o2); if (t <= codeSize) in.seek(t); } break; }
            case 0x14: { int32_t o2 = in.s24(); Value b2 = pop(), a2 = pop();
                if (a2.toString() != b2.toString()) { size_t t = static_cast<size_t>(static_cast<int64_t>(in.pos()) + o2); if (t <= codeSize) in.seek(t); } break; }
            case 0x15: { int32_t o2 = in.s24(); Value b2 = pop(), a2 = pop();
                if (a2.toNumber() < b2.toNumber()) { size_t t = static_cast<size_t>(static_cast<int64_t>(in.pos()) + o2); if (t <= codeSize) in.seek(t); } break; }
            case 0x16: { int32_t o2 = in.s24(); Value b2 = pop(), a2 = pop();
                if (a2.toNumber() <= b2.toNumber()) { size_t t = static_cast<size_t>(static_cast<int64_t>(in.pos()) + o2); if (t <= codeSize) in.seek(t); } break; }
            case 0x17: { int32_t o2 = in.s24(); Value b2 = pop(), a2 = pop();
                if (a2.toNumber() > b2.toNumber()) { size_t t = static_cast<size_t>(static_cast<int64_t>(in.pos()) + o2); if (t <= codeSize) in.seek(t); } break; }
            case 0x18: { int32_t o2 = in.s24(); Value b2 = pop(), a2 = pop();
                if (a2.toNumber() >= b2.toNumber()) { size_t t = static_cast<size_t>(static_cast<int64_t>(in.pos()) + o2); if (t <= codeSize) in.seek(t); } break; }
            case 0x19: { int32_t o2 = in.s24(); Value b2 = pop(), a2 = pop();
                if (a2.kind == b2.kind && a2.toString() == b2.toString()) { size_t t = static_cast<size_t>(static_cast<int64_t>(in.pos()) + o2); if (t <= codeSize) in.seek(t); } break; }
            case 0x1a: { int32_t o2 = in.s24(); Value b2 = pop(), a2 = pop();
                if (!(a2.kind == b2.kind && a2.toString() == b2.toString())) { size_t t = static_cast<size_t>(static_cast<int64_t>(in.pos()) + o2); if (t <= codeSize) in.seek(t); } break; }
            case 0x47: return Value::undefined();
            case 0x48: return pop();

            case 0xa0: { Value b2 = pop(), a2 = pop();
                if (a2.kind == VKind::String || b2.kind == VKind::String) push(Value::str(a2.toString() + b2.toString()));
                else push(Value::num(a2.toNumber() + b2.toNumber())); break; }
            case 0xa1: { Value b2 = pop(), a2 = pop(); push(Value::num(a2.toNumber() - b2.toNumber())); break; }
            case 0xa2: { Value b2 = pop(), a2 = pop(); push(Value::num(a2.toNumber() * b2.toNumber())); break; }
            case 0xa3: { Value b2 = pop(), a2 = pop(); double d2 = b2.toNumber();
                push(Value::num(d2 == 0.0 ? NAN : a2.toNumber() / d2)); break; }
            case 0xa4: { Value b2 = pop(), a2 = pop(); double d2 = b2.toNumber();
                push(Value::num(d2 == 0.0 ? NAN : std::fmod(a2.toNumber(), d2))); break; }
            case 0xa5: { Value b2 = pop(), a2 = pop(); push(Value::intg(a2.toInt() << (b2.toInt() & 31))); break; }
            case 0xa6: { Value b2 = pop(), a2 = pop(); push(Value::intg(a2.toInt() >> (b2.toInt() & 31))); break; }
            case 0xa7: { Value b2 = pop(), a2 = pop(); push(Value::uintv(static_cast<uint32_t>(a2.toUInt() >> (b2.toUInt() & 31)))); break; }
            case 0xa8: { Value b2 = pop(), a2 = pop(); push(Value::intg(a2.toInt() & b2.toInt())); break; }
            case 0xa9: { Value b2 = pop(), a2 = pop(); push(Value::intg(a2.toInt() | b2.toInt())); break; }
            case 0xaa: { Value b2 = pop(), a2 = pop(); push(Value::intg(a2.toInt() ^ b2.toInt())); break; }
            case 0xab: { Value b2 = pop(), a2 = pop();
                if (a2.isNullOrUndef() || b2.isNullOrUndef())
                    push(Value::boolean(a2.isNullOrUndef() && b2.isNullOrUndef()));
                else if (a2.kind == VKind::String && b2.kind == VKind::String) push(Value::boolean(a2.s == b2.s));
                else push(Value::boolean(a2.toNumber() == b2.toNumber())); break; }
            case 0xac: { Value b2 = pop(), a2 = pop(); push(Value::boolean(a2.kind == b2.kind && a2.toString() == b2.toString())); break; }
            case 0xad: { Value b2 = pop(), a2 = pop(); push(Value::boolean(a2.toNumber() < b2.toNumber())); break; }
            case 0xae: { Value b2 = pop(), a2 = pop(); push(Value::boolean(a2.toNumber() <= b2.toNumber())); break; }
            case 0xaf: { Value b2 = pop(), a2 = pop(); push(Value::boolean(a2.toNumber() > b2.toNumber())); break; }
            case 0xb0: { Value b2 = pop(), a2 = pop(); push(Value::boolean(a2.toNumber() >= b2.toNumber())); break; }
            case 0x90: { Value a2 = pop(); push(Value::num(-a2.toNumber())); break; }
            case 0x91: { Value a2 = pop(); push(Value::num(a2.toNumber() + 1)); break; }
            case 0x93: { Value a2 = pop(); push(Value::num(a2.toNumber() - 1)); break; }
            case 0x96: { Value a2 = pop(); push(Value::boolean(!a2.toBool())); break; }
            case 0x97: { Value a2 = pop(); push(Value::intg(~a2.toInt())); break; }
            case 0x73: { Value a2 = pop(); push(Value::intg(a2.toInt())); break; }
            case 0x74: { Value a2 = pop(); push(Value::uintv(a2.toUInt())); break; }
            case 0x75: { Value a2 = pop(); push(Value::num(a2.toNumber())); break; }
            case 0x76: { Value a2 = pop(); push(Value::boolean(a2.toBool())); break; }
            case 0x70: { Value a2 = pop(); push(Value::str(a2.toString())); break; }
            case 0x82: break;   // coerce_a

            case 0x53: {   // newobject
                uint32_t n = in.u30();
                auto o = newObject(ObjKind::Generic, "Object");
                for (uint32_t k = 0; k < n; ++k) { Value v = pop(); Value key = pop(); o->props[key.toString()] = v; }
                push(Value::object(o));
                break;
            }
            case 0x55: {   // newarray
                uint32_t n = in.u30();
                auto o = newObject(ObjKind::Array, "Array");
                for (uint32_t k = 0; k < n; ++k) { Value v = pop(); o->props[std::to_string(n - 1 - k)] = v; }
                o->props["$length"] = Value::intg(static_cast<int32_t>(n));
                push(Value::object(o));
                break;
            }
            case 0x40: {   // newfunction
                uint32_t mi = in.u30();
                auto o = newObject(ObjKind::Function, "Function");
                o->props["$method"] = Value::uintv(mi);
                push(Value::object(o));
                break;
            }
            case 0x49: {   // constructsuper
                uint32_t n = in.u30();
                for (uint32_t k = 0; k < n; ++k) pop();
                pop();
                break;
            }
            case 0x58: {   // newclass：定义类并注册到全局，同时执行静态初始化 cinit
                uint32_t ci = in.u30();
                auto o = newObject(ObjKind::Class, "Class");
                o->instanceIndex = static_cast<int>(ci);
                std::string cname = (ci < abc_.instances.size()) ? qualifiedName(abc_.instances[ci].name) : std::string();
                o->className = cname;
                // 构建 ClassDef 并执行 cinit（静态字段/静态代码块）
                if (ci < abc_.instances.size()) {
                    auto cd = buildClass(static_cast<int>(ci));
                    o->classDef = cd;
                    ensureClassInit(cd, o);
                }
                Value v = Value::object(o);
                if (!cname.empty()) {
                    globals_->props[cname] = v;
                    size_t p = cname.rfind('.');
                    if (p != std::string::npos) globals_->props[cname.substr(p + 1)] = v;
                }
                push(v);
                break;
            }
            case 0x5e: break;   // finddef
            case 0x42: {   // construct
                uint32_t argc = in.u30();
                std::vector<Value> cargs(argc);
                for (uint32_t k = 0; k < argc; ++k) cargs[argc - 1 - k] = pop();
                Value cls = pop();
                ObjKind ok = ObjKind::Generic;
                std::string cname;
                auto cd = std::shared_ptr<ClassDef>();
                if (cls.kind == VKind::Object && cls.obj) {
                    auto* c0 = cls.obj.get();
                    cname = c0->className;
                    if (c0->classDef) {
                        // 原生类
                        cd = c0->classDef;
                        if (cname == "Array" || cname == "flash.utils.Array") ok = ObjKind::Array;
                        else if (cname == "String") ok = ObjKind::String;
                        else if (cname == "Sprite" || cname == "flash.display.Sprite") ok = ObjKind::Sprite;
                        else if (cname == "MovieClip" || cname == "flash.display.MovieClip") ok = ObjKind::MovieClip;
                        else if (cname == "TextField") ok = ObjKind::TextField;
                        else if (cname == "Timer") ok = ObjKind::Timer;
                    } else if (c0->instanceIndex >= 0 && c0->instanceIndex < static_cast<int>(abc_.instances.size())) {
                        cd = buildClass(c0->instanceIndex);
                        ok = ObjKind::MovieClip;
                    }
                }
                auto inst = newObject(ok, cname);
                inst->cls = cd;
                if (ok == ObjKind::Array) inst->props["$length"] = Value::intg(0);
                // SymbolClass 关联
                auto sc = movie_.symbolClasses.find(cname);
                if (sc == movie_.symbolClasses.end()) {
                    size_t p = cname.rfind('.');
                    if (p != std::string::npos) sc = movie_.symbolClasses.find(cname.substr(p + 1));
                }
                if (sc != movie_.symbolClasses.end() && inst->display) inst->display->characterId = sc->second;
                // 执行构造函数
                if (cls.kind == VKind::Object && cls.obj && cls.obj->instanceIndex >= 0 &&
                    cls.obj->instanceIndex < static_cast<int>(abc_.instances.size())) {
                    uint32_t init = abc_.instances[cls.obj->instanceIndex].iinit;
                    if (init < abc_.methods.size()) {
                        int bi = abc_.methods[init].bodyIndex;
                        if (bi >= 0 && bi < static_cast<int>(abc_.methodBodies.size()))
                            exec(abc_.methodBodies[bi], cargs, inst);
                    }
                }
                push(Value::object(inst));
                break;
            }
            case 0x46: {   // callproperty（有返回值）
                uint32_t mn = in.u30();
                uint32_t argc = in.u30();
                std::vector<Value> cargs(argc);
                for (uint32_t k = 0; k < argc; ++k) cargs[argc - 1 - k] = pop();
                Value objv = pop();
                std::string mname = multinameName(mn);
                if (objv.kind == VKind::Object && objv.obj) {
                    Value r = callMethod(objv.obj, mname, cargs);
                    // addChild 顶层挂舞台
                    if (mname == "addChild" && objv.obj->display && !objv.obj->display->parent) {
                        bool dup = false;
                        for (auto& s : stage) if (s.get() == objv.obj->display.get()) { dup = true; break; }
                        if (!dup) stage.push_back(objv.obj->display);
                    }
                    push(r);
                } else if (objv.kind == VKind::String) {
                    auto so = newObject(ObjKind::String, "String");
                    so->props["$str"] = objv;
                    push(callMethod(so, mname, cargs));
                } else push(Value::undefined());
                break;
            }
            case 0x4f: {   // callpropvoid
                uint32_t mn = in.u30();
                uint32_t argc = in.u30();
                std::vector<Value> cargs(argc);
                for (uint32_t k = 0; k < argc; ++k) cargs[argc - 1 - k] = pop();
                Value objv = pop();
                std::string mname = multinameName(mn);
                if (objv.kind == VKind::Object && objv.obj) {
                    callMethod(objv.obj, mname, cargs);
                    if (mname == "addChild" && objv.obj->display && !objv.obj->display->parent) {
                        bool dup = false;
                        for (auto& s : stage) if (s.get() == objv.obj->display.get()) { dup = true; break; }
                        if (!dup) stage.push_back(objv.obj->display);
                    }
                }
                break;
            }
            case 0x5f: {   // findpropstrict
                uint32_t mn = in.u30();
                std::string name = multinameName(mn);
                Value found = Value::nullv();
                for (auto it = scope.rbegin(); it != scope.rend(); ++it)
                    if ((*it)->props.count(name)) { found = Value::object(*it); break; }
                if (found.isNull()) found = Value::object(globals_);
                push(found);
                break;
            }
            case 0x5d: {   // findproperty
                uint32_t mn = in.u30();
                std::string name = multinameName(mn);
                Value found = Value::nullv();
                for (auto it = scope.rbegin(); it != scope.rend(); ++it)
                    if ((*it)->props.count(name)) { found = Value::object(*it); break; }
                if (found.isNull()) found = Value::object(globals_);
                push(found);
                break;
            }
            case 0x60: {   // getlex
                uint32_t mn = in.u30();
                std::string name = multinameName(mn);
                Value found = Value::undefined();
                for (auto it = scope.rbegin(); it != scope.rend(); ++it) {
                    auto p = (*it)->props.find(name);
                    if (p != (*it)->props.end()) { found = p->second; break; }
                }
                if (found.isUndefined()) {
                    auto p = globals_->props.find(name);
                    if (p != globals_->props.end()) found = p->second;
                }
                push(found);
                break;
            }
            case 0x41: {   // call
                uint32_t argc = in.u30();
                std::vector<Value> cargs(argc);
                for (uint32_t k = 0; k < argc; ++k) cargs[argc - 1 - k] = pop();
                Value fn = pop();
                Value thisv = pop();
                if (fn.kind == VKind::Object && fn.obj) {
                    auto mi = fn.obj->props.find("$method");
                    if (mi != fn.obj->props.end()) {
                        uint32_t midx = mi->second.u;
                        if (midx < abc_.methods.size()) {
                            int bi = abc_.methods[midx].bodyIndex;
                            if (bi >= 0 && bi < static_cast<int>(abc_.methodBodies.size())) {
                                auto th = thisv.kind == VKind::Object ? thisv.obj : nullptr;
                                push(exec(abc_.methodBodies[bi], cargs, th));
                                break;
                            }
                        }
                    }
                }
                push(Value::undefined());
                break;
            }
            case 0x4a: {   // constructprop
                uint32_t mn = in.u30();
                uint32_t argc = in.u30();
                for (uint32_t k = 0; k < argc; ++k) pop();
                pop();
                auto o = newObject(ObjKind::Generic, multinameName(mn));
                push(Value::object(o));
                break;
            }
            default:
                break;   // 未实现 opcode 安全跳过
        }
    }
    return Value::undefined();
}

Value Vm::execScript(const abc::MethodBody& body, std::shared_ptr<Object> thisObj) {
    return exec(body, {}, thisObj);
}

std::vector<std::shared_ptr<Vm>> runMovieScripts(Movie& movie) {
    std::vector<std::shared_ptr<Vm>> vms;
    if (movie.abcFiles.empty()) return vms;
    for (const auto& abc : movie.abcFiles) {
        auto vm = std::make_shared<Vm>(abc, movie);
        vm->run();
        if (!vm->stage.empty()) movie.runtimeStage = vm->stage;
        vms.push_back(vm);
    }
    return vms;
}

}  // namespace fvm
}  // namespace swf
